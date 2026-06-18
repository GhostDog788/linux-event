// SPDX-License-Identifier: MIT
/*
 * bench.c - performance harness for the event object (/dev/event).
 *
 * The point of this program is *comparability*: event.ko will go through many
 * implementation iterations, and we want a definitive answer to "is the new
 * one better, and in which regime?". So the harness
 *
 *   - measures the handful of numbers that actually distinguish event
 *     implementations (see the scenario list below),
 *   - sweeps the waiter count, because "better" usually depends on it
 *     (1 waiter exercises raw wake latency; hundreds exercise the fan-out
 *     walk and lock contention),
 *   - runs every scenario against a *futex generation-counter* baseline as
 *     well. The futex numbers are the experiment's control: the kernel's
 *     native primitive doing the same job, untouched by our module. Between
 *     two runs on the same machine they should not move; if they do, the
 *     machine was not quiet and the event-vs-event comparison is invalid,
 *   - dumps every raw sample to CSV (-c) so compare.py can attach
 *     percentiles and a significance test to the A/B verdict.
 *
 * Scenarios (-s, comma list, default all):
 *
 *   wake     One publisher signals N parked waiters, -r rounds per N.
 *            Metrics per round: wake_ns (one sample per waiter: time from
 *            just-before-signal to that waiter running again), last_wake_ns
 *            (time until the *slowest* waiter is running, fan-out
 *            completion), signal_call_ns (how long the publisher is stuck
 *            inside signal_event() itself).
 *
 *   churn    N waiters re-arm in a tight loop while the publisher signals
 *            as fast as it can for -d seconds: sustained throughput and
 *            lock contention. wakes_per_sec is counted on the waiter side
 *            (actual wait returns), since signal_all() now reports listeners
 *            notified, not parked waiters woken. Metrics per rep:
 *            wakes_per_sec, signal_calls_per_sec, empty_signal_pct (publisher
 *            found nobody parked, meaningful for the futex control; ~0 for
 *            event, whose listeners stay registered), waiter_wakes (one
 *            sample per waiter: fairness of the wake distribution).
 *
 *   loop     The realistic subscriber loop: N waiters each wait, simulate
 *            -W us of work, and re-arm; the publisher signals every -P us
 *            for -d seconds. Metrics: loop_wake_ns (per wake: time from the
 *            most recent signal to that waiter running), missed_signals (per
 *            waiter: signals that fired while it was working, coalesced into
 *            another return rather than lost = sent - caught), signals_sent.
 *
 *   signal0  signal cost when nobody is waiting (publish to no
 *            subscribers), batched. Metric: signal0_ns.
 *
 *   open     create_event()+close_event() pair cost, batched. Event impl
 *            only. Metric: open_close_ns.
 *
 * Validity: a wake round only measures wake latency if every waiter is
 * parked in the kernel before the signal (a waiter that returned instantly
 * measures nothing). The harness guarantees that twice over:
 *   1. the publisher polls /proc/self/task/<tid>/stat until every waiter
 *      thread reports state 'S' (by then it is parked in the wait path: both
 *      event.ko and futex enqueue *before* marking the task sleeping), and
 *   2. it checks signal_all()'s return == N (every listener notified / every
 *      futex waiter woken).
 * A round failing (2) is discarded and counted; a nonzero invalid_rounds in
 * the summary means the implementation under test has a registration race --
 * that is a correctness verdict, not noise.
 *
 * Build:  make            (header-only API from ../lib, plus -pthread)
 * Run:    ./bench [-i event,futex] [-s wake,churn,loop,signal0,open]
 *                 [-N 1,2,4,16,64,256] [-r rounds] [-d secs] [-R reps]
 *                 [-P period_us] [-W work_us] [-c out.csv] [-l label]
 *                 [-p cpu]
 *
 * The event scenarios need /dev/event (event.ko loaded) on this machine;
 * `-i futex` alone runs anywhere, which is handy for testing the harness.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "event.h"

#define NSEC_PER_SEC 1000000000ull

/* How long the publisher waits for stragglers before declaring the
 * implementation under test broken (lost waiter / deadlock). */
#define STUCK_TIMEOUT_NS (10 * NSEC_PER_SEC)

#define WAITER_STACK_SIZE (256 * 1024) /* keep 512 threads cheap on small VMs */

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static void relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#else
	sched_yield();
#endif
}

/* --- sample vectors + summary stats ---------------------------------------- */

struct vec {
	double *v;
	size_t n, cap;
};

static void vec_push(struct vec *s, double x)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 1024;
		s->v = realloc(s->v, s->cap * sizeof(*s->v));
		if (!s->v)
			die("out of memory");
	}
	s->v[s->n++] = x;
}

static void vec_reset(struct vec *s)
{
	free(s->v);
	memset(s, 0, sizeof(*s));
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

/* Nearest-rank percentile; sorts the vector in place. */
static double vec_pct(struct vec *s, double p)
{
	size_t idx;

	if (!s->n)
		return 0.0;
	qsort(s->v, s->n, sizeof(*s->v), cmp_double);
	idx = (size_t)(p / 100.0 * (double)s->n);
	if (idx >= s->n)
		idx = s->n - 1;
	return s->v[idx];
}

static double vec_mean(const struct vec *s)
{
	double sum = 0.0;
	size_t i;

	for (i = 0; i < s->n; i++)
		sum += s->v[i];
	return s->n ? sum / (double)s->n : 0.0;
}

/* --- CSV output ------------------------------------------------------------ */

static FILE *csv;
static const char *label = "";

static void csv_emit(const char *scenario, const char *impl, int waiters,
		     int round, const char *metric, double value)
{
	if (csv)
		fprintf(csv, "%s,%s,%s,%d,%d,%s,%.3f\n", label, scenario, impl,
			waiters, round, metric, value);
}

/* --- /proc task-state gating ------------------------------------------------
 *
 * State is the field after the last ')' of the comm in
 * /proc/self/task/<tid>/stat. 'S' (interruptible sleep) for one of our waiter
 * threads, which between announcing readiness and blocking executes nothing
 * that sleeps, means it has entered the implementation's wait path and is
 * registered (both event.ko and futex enqueue before marking the task
 * sleeping).
 */
static char task_state(pid_t tid)
{
	char path[64], buf[256];
	ssize_t n;
	char *p;
	int fd;

	snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)tid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return '?';
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return '?';
	buf[n] = '\0';
	p = strrchr(buf, ')');
	if (!p || p[1] != ' ')
		return '?';
	return p[2];
}

static void wait_all_parked(const pid_t *tids, int n, const char *who)
{
	uint64_t deadline = now_ns() + STUCK_TIMEOUT_NS;
	int i;

	for (i = 0; i < n; i++) {
		while (task_state(tids[i]) != 'S') {
			if (now_ns() > deadline)
				die("%s: waiter %d (tid %d) never parked, "
				    "implementation under test looks stuck",
				    who, i, (int)tids[i]);
			sched_yield();
		}
	}
}

/* --- implementations under test --------------------------------------------
 *
 * Both share one shape: wait() blocks until the object is signaled (parking if
 * it has not been since the last call), and signal_all() wakes everyone.
 * signal_all() returns a count: for futex the number of waiters actually woken,
 * for event the number of listeners notified. Neither impl needs the caller to
 * track a generation: event tracks the consumed generation in the kernel, and
 * futex keeps its per-thread generation thread-locally below. A waiter never
 * loses a signal across calls, which is also what keeps the harness race-free.
 */

struct ctx {
	int fd;			    /* event */
	_Atomic uint32_t futex_gen; /* futex */
};

struct impl {
	const char *name;
	bool has_open; /* supports the open (create/destroy) scenario */
	void (*setup)(struct ctx *c);
	void (*teardown)(struct ctx *c);
	void (*wait)(struct ctx *c);
	int (*signal_all)(struct ctx *c);
};

/* event: the object under test, driven through the public ../lib/event.h API
 * exactly the way an application would use it. Each waiter thread owns its own
 * waiter object (one thread blocks in one waiter), created lazily and closed by
 * a TLS destructor when the thread exits, so a sweep does not leak fds. */

static pthread_key_t event_waiter_key;
static pthread_once_t event_key_once = PTHREAD_ONCE_INIT;

static void event_waiter_dtor(void *p)
{
	int w = (int)(intptr_t)p;

	if (w > 0)
		close_waiter(w);
}

static void event_key_init(void)
{
	if (pthread_key_create(&event_waiter_key, event_waiter_dtor))
		die("pthread_key_create");
}

/* This thread's waiter, registered on the shared event; created on first use. */
static int event_thread_waiter(struct ctx *c)
{
	intptr_t w;

	pthread_once(&event_key_once, event_key_init);
	w = (intptr_t)pthread_getspecific(event_waiter_key);
	if (!w) {
		int fd = create_waiter();

		if (fd < 0)
			die("create_waiter: %s", strerror(errno));
		if (waiter_add(fd, c->fd) < 0)
			die("waiter_add: %s", strerror(errno));
		w = fd;
		pthread_setspecific(event_waiter_key, (void *)w);
	}
	return (int)w;
}

static void event_setup(struct ctx *c)
{
	c->fd = create_event();
	if (c->fd < 0)
		die("create_event: %s (is event.ko loaded?)", strerror(errno));
}

static void event_teardown(struct ctx *c)
{
	close_event(c->fd);
}

static void event_wait_op(struct ctx *c)
{
	int w = event_thread_waiter(c);
	int ready[1];

	if (waiter_wait(w, ready, 1, EVT_WAIT_FOREVER) != 1)
		die("waiter_wait: %s", strerror(errno));
}

static int event_signal_all(struct ctx *c)
{
	int woken = signal_event(c->fd);

	if (woken < 0)
		die("signal_event: %s", strerror(errno));
	return woken;
}

/* futex: the control. A 32-bit generation counter; waiters FUTEX_WAIT while
 * the word still equals the generation they carry (EAGAIN = it moved before
 * they parked = a caught signal), the publisher bumps it and FUTEX_WAKEs
 * everyone. This is the kernel-native way to build the same object, so it
 * anchors what the hardware + scheduler can do. The bench never runs long
 * enough to wrap 32 bits. */

static long sys_futex(_Atomic uint32_t *uaddr, int op, uint32_t val)
{
	return syscall(SYS_futex, uaddr, op, val, NULL, NULL, 0);
}

static void futex_setup(struct ctx *c)
{
	atomic_store(&c->futex_gen, 0);
}

static void futex_teardown(struct ctx *c)
{
	(void)c;
}

static void futex_wait_op(struct ctx *c)
{
	static __thread uint32_t my_gen; /* per-thread generation carried */
	uint32_t cur;

	while ((cur = atomic_load(&c->futex_gen)) == my_gen) {
		long ret = sys_futex(&c->futex_gen, FUTEX_WAIT_PRIVATE, my_gen);

		if (ret < 0 && errno != EAGAIN && errno != EINTR)
			die("futex_wait: %s", strerror(errno));
	}
	my_gen = cur;
}

static int futex_signal_all(struct ctx *c)
{
	long woken;

	atomic_fetch_add(&c->futex_gen, 1);
	woken = sys_futex(&c->futex_gen, FUTEX_WAKE_PRIVATE, INT_MAX);
	if (woken < 0)
		die("futex_wake: %s", strerror(errno));
	return (int)woken;
}

static const struct impl impls[] = {
	{
		.name = "event",
		.has_open = true,
		.setup = event_setup,
		.teardown = event_teardown,
		.wait = event_wait_op,
		.signal_all = event_signal_all,
	},
	{
		.name = "futex",
		.has_open = false,
		.setup = futex_setup,
		.teardown = futex_teardown,
		.wait = futex_wait_op,
		.signal_all = futex_signal_all,
	},
};

/* --- waiter thread plumbing ------------------------------------------------ */

static pthread_t spawn_waiter(void *(*fn)(void *), void *arg)
{
	pthread_attr_t attr;
	pthread_t t;
	int err;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, WAITER_STACK_SIZE);
	err = pthread_create(&t, &attr, fn, arg);
	pthread_attr_destroy(&attr);
	if (err)
		die("pthread_create: %s", strerror(err));
	return t;
}

/* --- scenario: wake (fan-out latency) --------------------------------------
 *
 * Per round: all waiters park (publisher verifies, see header comment), the
 * publisher timestamps, signals once, and every waiter timestamps the moment
 * it is running again. Round boundaries are pthread barriers that include
 * the publisher, so per-round state can be reset without races.
 */

struct wake_shared {
	const struct impl *impl;
	struct ctx *ctx;
	pthread_barrier_t start, done; /* both have n + 1 parties */
	_Atomic int ready;
	_Atomic bool quit;
	pid_t *tids;
	uint64_t *wake_ts;
	int n;
};

struct wake_waiter_arg {
	struct wake_shared *sh;
	int idx;
};

static void *wake_waiter(void *p)
{
	struct wake_waiter_arg *a = p;
	struct wake_shared *sh = a->sh;

	sh->tids[a->idx] = (pid_t)syscall(SYS_gettid);

	for (;;) {
		pthread_barrier_wait(&sh->start);
		if (atomic_load(&sh->quit))
			break;
		/* Each waiter carries its own state (consumed event /
		 * thread-local futex gen), so a fresh round has nothing
		 * pending and this wait really parks. */
		atomic_fetch_add(&sh->ready, 1);
		sh->impl->wait(sh->ctx);
		sh->wake_ts[a->idx] = now_ns();
		pthread_barrier_wait(&sh->done);
	}
	return NULL;
}

static void run_wake(const struct impl *impl, int n, int rounds, int warmup)
{
	struct wake_shared sh = { .impl = impl, .n = n };
	struct wake_waiter_arg *args;
	struct vec wake = { 0 }, last = { 0 }, sig = { 0 };
	pthread_t *threads;
	struct ctx ctx;
	int r, i, invalid = 0;

	impl->setup(&ctx);
	sh.ctx = &ctx;
	sh.tids = calloc(n, sizeof(*sh.tids));
	sh.wake_ts = calloc(n, sizeof(*sh.wake_ts));
	threads = calloc(n, sizeof(*threads));
	args = calloc(n, sizeof(*args));
	if (!sh.tids || !sh.wake_ts || !threads || !args)
		die("out of memory");
	pthread_barrier_init(&sh.start, NULL, n + 1);
	pthread_barrier_init(&sh.done, NULL, n + 1);

	for (i = 0; i < n; i++) {
		args[i] = (struct wake_waiter_arg){ .sh = &sh, .idx = i };
		threads[i] = spawn_waiter(wake_waiter, &args[i]);
	}

	for (r = 0; r < warmup + rounds; r++) {
		uint64_t t0, t1, spin_deadline;
		bool valid;
		int woken;

		pthread_barrier_wait(&sh.start);

		spin_deadline = now_ns() + STUCK_TIMEOUT_NS;
		while (atomic_load(&sh.ready) < n) {
			if (now_ns() > spin_deadline)
				die("wake/%s n=%d: waiters never became ready",
				    impl->name, n);
			relax();
		}
		wait_all_parked(sh.tids, n, "wake");

		t0 = now_ns();
		woken = impl->signal_all(sh.ctx);
		t1 = now_ns();

		/* The parked check above should make woken == n always; if
		 * not, the impl has a registration race. Release the
		 * stragglers so the barrier passes, then drop the round. */
		valid = (woken == n);
		while (woken < n) {
			if (now_ns() > spin_deadline)
				die("wake/%s n=%d round %d: only %d/%d woken "
				    "and stragglers won't wake: lost waiter",
				    impl->name, n, r, woken, n);
			woken += impl->signal_all(sh.ctx);
			sched_yield();
		}

		pthread_barrier_wait(&sh.done);
		atomic_store(&sh.ready, 0);

		if (r < warmup)
			continue;
		if (!valid) {
			invalid++;
			continue;
		}

		vec_push(&sig, (double)(t1 - t0));
		csv_emit("wake", impl->name, n, r - warmup, "signal_call_ns",
			 (double)(t1 - t0));
		uint64_t slowest = 0;
		for (i = 0; i < n; i++) {
			uint64_t lat = sh.wake_ts[i] - t0;

			if (lat > slowest)
				slowest = lat;
			vec_push(&wake, (double)lat);
			csv_emit("wake", impl->name, n, r - warmup, "wake_ns",
				 (double)lat);
		}
		vec_push(&last, (double)slowest);
		csv_emit("wake", impl->name, n, r - warmup, "last_wake_ns",
			 (double)slowest);
	}

	atomic_store(&sh.quit, true);
	pthread_barrier_wait(&sh.start);
	for (i = 0; i < n; i++)
		pthread_join(threads[i], NULL);

	printf("wake    %-6s n=%-4d rounds=%-4d invalid=%-3d "
	       "wake p50=%7.2fus p99=%8.2fus  last p50=%8.2fus  "
	       "signal p50=%7.2fus\n",
	       impl->name, n, rounds, invalid, vec_pct(&wake, 50) / 1e3,
	       vec_pct(&wake, 99) / 1e3, vec_pct(&last, 50) / 1e3,
	       vec_pct(&sig, 50) / 1e3);
	if (invalid)
		printf("        ^^^ %d invalid round%s: signal woke fewer "
		       "waiters than were parked, registration race?\n",
		       invalid, invalid == 1 ? "" : "s");
	csv_emit("wake", impl->name, n, -1, "invalid_rounds", invalid);

	pthread_barrier_destroy(&sh.start);
	pthread_barrier_destroy(&sh.done);
	vec_reset(&wake);
	vec_reset(&last);
	vec_reset(&sig);
	free(sh.tids);
	free(sh.wake_ts);
	free(threads);
	free(args);
	impl->teardown(&ctx);
}

/* --- scenario: churn (sustained throughput under contention) ---------------
 *
 * No round gating here, waiters re-arm as fast as they can and the
 * publisher signals as fast as it can, which is exactly the registration /
 * signal lock contention we want to stress. Throughput is counted on the
 * publisher side from signal_all()'s return value, over the timed window
 * only. Per-waiter counts include at most one extra drain wake each.
 */

struct churn_count {
	_Atomic uint64_t c;
	char pad[64 - sizeof(_Atomic uint64_t)]; /* avoid false sharing */
};

struct churn_shared {
	const struct impl *impl;
	struct ctx *ctx;
	pthread_barrier_t start; /* n + 1 parties */
	_Atomic bool stop;
	_Atomic int exited;
	struct churn_count *counts;
	int n;
};

struct churn_waiter_arg {
	struct churn_shared *sh;
	int idx;
};

static void *churn_waiter(void *p)
{
	struct churn_waiter_arg *a = p;
	struct churn_shared *sh = a->sh;

	pthread_barrier_wait(&sh->start);
	for (;;) {
		if (atomic_load(&sh->stop))
			break;
		sh->impl->wait(sh->ctx);
		atomic_fetch_add(&sh->counts[a->idx].c, 1);
	}
	atomic_fetch_add(&sh->exited, 1);
	return NULL;
}

static void run_churn(const struct impl *impl, int n, double dur_secs,
		      int reps)
{
	int rep, i;

	for (rep = 0; rep < reps; rep++) {
		struct churn_shared sh = { .impl = impl, .n = n };
		struct churn_waiter_arg *args;
		pthread_t *threads;
		struct ctx ctx;
		uint64_t t0, t1, t_end, drain_deadline;
		uint64_t total_wakes = 0, calls = 0, empty = 0;
		uint64_t min_wakes = UINT64_MAX, max_wakes = 0;
		double elapsed, wps, cps, empty_pct;

		impl->setup(&ctx);
		sh.ctx = &ctx;
		sh.counts = calloc(n, sizeof(*sh.counts));
		threads = calloc(n, sizeof(*threads));
		args = calloc(n, sizeof(*args));
		if (!sh.counts || !threads || !args)
			die("out of memory");
		pthread_barrier_init(&sh.start, NULL, n + 1);

		for (i = 0; i < n; i++) {
			args[i] = (struct churn_waiter_arg){ .sh = &sh,
							     .idx = i };
			threads[i] = spawn_waiter(churn_waiter, &args[i]);
		}
		pthread_barrier_wait(&sh.start);

		t0 = now_ns();
		t_end = t0 + (uint64_t)(dur_secs * NSEC_PER_SEC);
		while ((t1 = now_ns()) < t_end) {
			int w = impl->signal_all(sh.ctx);

			calls++;
			if (!w)
				empty++;
		}

		atomic_store(&sh.stop, true);
		drain_deadline = now_ns() + STUCK_TIMEOUT_NS;
		while (atomic_load(&sh.exited) < n) {
			if (now_ns() > drain_deadline)
				die("churn/%s n=%d: waiters won't drain, "
				    "lost wakeup in the implementation?",
				    impl->name, n);
			impl->signal_all(sh.ctx);
			usleep(100);
		}
		for (i = 0; i < n; i++)
			pthread_join(threads[i], NULL);

		/* Throughput is counted on the waiter side (actual wait
		 * returns); signal_all()'s return means "listeners notified"
		 * for event, so it cannot stand in for wakes. empty_pct stays
		 * a publisher-side metric, meaningful for the futex control
		 * (no one parked); for event a signal always has its listeners
		 * registered, so it reads ~0. */
		for (i = 0; i < n; i++) {
			uint64_t c = atomic_load(&sh.counts[i].c);

			total_wakes += c;
			if (c < min_wakes)
				min_wakes = c;
			if (c > max_wakes)
				max_wakes = c;
			csv_emit("churn", impl->name, n, rep, "waiter_wakes",
				 (double)c);
		}

		elapsed = (double)(t1 - t0) / (double)NSEC_PER_SEC;
		wps = (double)total_wakes / elapsed;
		cps = (double)calls / elapsed;
		empty_pct = calls ? 100.0 * (double)empty / (double)calls :
				    0.0;
		csv_emit("churn", impl->name, n, rep, "wakes_per_sec", wps);
		csv_emit("churn", impl->name, n, rep, "signal_calls_per_sec",
			 cps);
		csv_emit("churn", impl->name, n, rep, "empty_signal_pct",
			 empty_pct);

		printf("churn   %-6s n=%-4d rep=%d  %10.0f wakes/s  "
		       "%10.0f signals/s  empty=%5.1f%%  "
		       "fairness min/max=%llu/%llu\n",
		       impl->name, n, rep, wps, cps, empty_pct,
		       (unsigned long long)min_wakes,
		       (unsigned long long)max_wakes);

		pthread_barrier_destroy(&sh.start);
		free(sh.counts);
		free(threads);
		free(args);
		impl->teardown(&ctx);
	}
}

/* --- scenario: loop (the realistic subscriber loop) -------------------------
 *
 * N waiters: wait -> simulate work -> re-arm, forever. One publisher signals
 * on a fixed period. This is the production shape for "many listeners on one
 * event": a listener still working when a signal fires sees it on the next
 * wait instead of sleeping into the void, and a burst that fired during the
 * work coalesces into a single return.
 *
 * Latency: the publisher stamps last_signal_ts just before each signal; a
 * woken waiter takes (now - last_signal_ts). If it had coalesced a burst,
 * that is the time since the most recent signal of the burst (a small
 * over-count bounded by the period, noted in the metric).
 *
 * Coalescing: over the run the publisher sends g signals; a waiter that was
 * always parked would return g times, one per signal. Each return consumes at
 * least one signal, so caught <= g, and missed = g - caught is exactly the
 * signals that coalesced into another return while the waiter was working.
 * With an edge-triggered design those would be silently lost events; here they
 * are accounted for. This is computed identically for both impls.
 */

struct loop_shared {
	const struct impl *impl;
	struct ctx *ctx;
	pthread_barrier_t start; /* n + 1 parties */
	_Atomic bool stop;
	_Atomic int exited;
	_Atomic uint64_t last_signal_ts; /* stamped just before each signal */
	uint64_t work_ns;
	int n;
};

struct loop_waiter_arg {
	struct loop_shared *sh;
	struct vec lat;
	uint64_t caught;
};

static void *loop_waiter(void *p)
{
	struct loop_waiter_arg *a = p;
	struct loop_shared *sh = a->sh;
	uint64_t t;

	pthread_barrier_wait(&sh->start);
	for (;;) {
		sh->impl->wait(sh->ctx);
		if (atomic_load(&sh->stop))
			break;
		t = now_ns();
		vec_push(&a->lat,
			 (double)(t - atomic_load(&sh->last_signal_ts)));
		a->caught++;
		while (sh->work_ns && now_ns() < t + sh->work_ns)
			relax(); /* simulate the listener's work */
	}
	atomic_fetch_add(&sh->exited, 1);
	return NULL;
}

static void run_loop(const struct impl *impl, int n, double dur_secs,
		     uint64_t period_us, uint64_t work_us)
{
	struct loop_shared sh = { .impl = impl,
				  .n = n,
				  .work_ns = work_us * 1000 };
	struct loop_waiter_arg *args;
	pthread_t *threads;
	struct ctx ctx;
	struct vec lat = { 0 };
	uint64_t t_end, t_next, g = 0, drain_deadline;
	uint64_t caught = 0, missed;
	int i;

	impl->setup(&ctx);
	sh.ctx = &ctx;
	threads = calloc(n, sizeof(*threads));
	args = calloc(n, sizeof(*args));
	if (!threads || !args)
		die("out of memory");
	pthread_barrier_init(&sh.start, NULL, n + 1);

	for (i = 0; i < n; i++) {
		args[i] = (struct loop_waiter_arg){ .sh = &sh };
		threads[i] = spawn_waiter(loop_waiter, &args[i]);
	}
	pthread_barrier_wait(&sh.start);

	t_next = now_ns();
	t_end = t_next + (uint64_t)(dur_secs * NSEC_PER_SEC);
	while (now_ns() < t_end) {
		struct timespec ts;

		g++;
		atomic_store(&sh.last_signal_ts, now_ns());
		impl->signal_all(sh.ctx);

		t_next += period_us * 1000;
		ts.tv_sec = t_next / NSEC_PER_SEC;
		ts.tv_nsec = t_next % NSEC_PER_SEC;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
	}

	atomic_store(&sh.stop, true);
	drain_deadline = now_ns() + STUCK_TIMEOUT_NS;
	while (atomic_load(&sh.exited) < n) {
		if (now_ns() > drain_deadline)
			die("loop/%s n=%d: waiters won't drain", impl->name,
			    n);
		impl->signal_all(sh.ctx);
		usleep(1000);
	}
	for (i = 0; i < n; i++)
		pthread_join(threads[i], NULL);

	for (i = 0; i < n; i++) {
		size_t s;
		uint64_t miss_i = g > args[i].caught ? g - args[i].caught : 0;

		caught += args[i].caught;
		for (s = 0; s < args[i].lat.n; s++) {
			vec_push(&lat, args[i].lat.v[s]);
			csv_emit("loop", impl->name, n, i, "loop_wake_ns",
				 args[i].lat.v[s]);
		}
		csv_emit("loop", impl->name, n, i, "missed_signals",
			 (double)miss_i);
		vec_reset(&args[i].lat);
	}
	missed = (uint64_t)g * n > caught ? (uint64_t)g * n - caught : 0;
	csv_emit("loop", impl->name, n, -1, "signals_sent", (double)g);

	printf("loop    %-6s n=%-4d period=%lluus work=%lluus  "
	       "signals=%-6llu  wake p50=%8.2fus p99=%8.2fus  "
	       "missed=%llu (%.2f%% of deliveries)\n",
	       impl->name, n, (unsigned long long)period_us,
	       (unsigned long long)work_us, (unsigned long long)g,
	       vec_pct(&lat, 50) / 1e3, vec_pct(&lat, 99) / 1e3,
	       (unsigned long long)missed,
	       g && n ? 100.0 * (double)missed / ((double)g * n) : 0.0);

	pthread_barrier_destroy(&sh.start);
	vec_reset(&lat);
	free(threads);
	free(args);
	impl->teardown(&ctx);
}

/* --- scenario: signal0 (publish with no subscribers) ------------------------ */

#define BATCH_OPS 1000
#define BATCHES 200

static void run_signal0(const struct impl *impl)
{
	struct vec s = { 0 };
	struct ctx ctx;
	int b, k;

	impl->setup(&ctx);
	for (b = 0; b < BATCHES; b++) {
		uint64_t t0 = now_ns(), t1;
		double per_op;

		for (k = 0; k < BATCH_OPS; k++)
			impl->signal_all(&ctx);
		t1 = now_ns();
		per_op = (double)(t1 - t0) / BATCH_OPS;
		vec_push(&s, per_op);
		csv_emit("signal0", impl->name, 0, b, "signal0_ns", per_op);
	}
	impl->teardown(&ctx);

	printf("signal0 %-6s        p50=%7.1fns  p99=%7.1fns  mean=%7.1fns\n",
	       impl->name, vec_pct(&s, 50), vec_pct(&s, 99), vec_mean(&s));
	vec_reset(&s);
}

/* --- scenario: open (create/destroy cost) ----------------------------------- */

static void run_open(const struct impl *impl)
{
	struct vec s = { 0 };
	int b, k;

	for (b = 0; b < BATCHES; b++) {
		uint64_t t0 = now_ns(), t1;
		double per_op;

		for (k = 0; k < BATCH_OPS; k++) {
			int fd = create_event();

			if (fd < 0)
				die("create_event: %s", strerror(errno));
			close_event(fd);
		}
		t1 = now_ns();
		per_op = (double)(t1 - t0) / BATCH_OPS;
		vec_push(&s, per_op);
		csv_emit("open", impl->name, 0, b, "open_close_ns", per_op);
	}

	printf("open    %-6s        p50=%7.1fns  p99=%7.1fns  mean=%7.1fns\n",
	       impl->name, vec_pct(&s, 50), vec_pct(&s, 99), vec_mean(&s));
	vec_reset(&s);
}

/* --- option parsing + main --------------------------------------------------- */

static bool list_has(const char *list, const char *item)
{
	size_t len = strlen(item);
	const char *p = list;

	while ((p = strstr(p, item)) != NULL) {
		bool at_start = (p == list || p[-1] == ',');
		bool at_end = (p[len] == '\0' || p[len] == ',');

		if (at_start && at_end)
			return true;
		p += len;
	}
	return false;
}

static int parse_int_list(const char *arg, int *out, int max)
{
	char *copy = strdup(arg), *tok, *save = NULL;
	int n = 0;

	if (!copy)
		die("out of memory");
	for (tok = strtok_r(copy, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		int v = atoi(tok);

		if (v < 1)
			die("bad count '%s' in list '%s'", tok, arg);
		if (n == max)
			die("too many entries in list '%s'", arg);
		out[n++] = v;
	}
	free(copy);
	if (!n)
		die("empty list '%s'", arg);
	return n;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -i list   implementations: event,futex (default both)\n"
		"  -s list   scenarios: wake,churn,loop,signal0,open (default all)\n"
		"  -N list   waiter counts to sweep (default 1,2,4,16,64,256)\n"
		"  -r n      wake rounds per waiter count (default 100)\n"
		"  -d secs   churn/loop duration per rep (default 1.0)\n"
		"  -R n      churn reps per waiter count (default 10; fewer\n"
		"            than 8 is too thin for compare.py's verdict)\n"
		"  -P us     loop: publisher signal period (default 2000)\n"
		"  -W us     loop: per-waiter simulated work (default 0)\n"
		"  -c file   write raw samples as CSV (for compare.py)\n"
		"  -l label  tag CSV rows (e.g. git revision)\n"
		"  -p cpu    pin the publisher thread to this CPU\n",
		argv0);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *impl_list = "event,futex";
	const char *scen_list = "wake,churn,loop,signal0,open";
	const char *csv_path = NULL;
	int counts[64] = { 1, 2, 4, 16, 64, 256 };
	int ncounts = 6, rounds = 100, churn_reps = 10, pin_cpu = -1;
	uint64_t loop_period_us = 2000, loop_work_us = 0;
	double churn_secs = 1.0;
	struct utsname uts;
	size_t ii;
	int opt, ci;

	while ((opt = getopt(argc, argv, "i:s:N:r:d:R:P:W:c:l:p:h")) != -1) {
		switch (opt) {
		case 'i':
			impl_list = optarg;
			break;
		case 's':
			scen_list = optarg;
			break;
		case 'N':
			ncounts = parse_int_list(optarg, counts, 64);
			break;
		case 'r':
			rounds = atoi(optarg);
			break;
		case 'd':
			churn_secs = atof(optarg);
			break;
		case 'R':
			churn_reps = atoi(optarg);
			break;
		case 'P':
			loop_period_us = strtoull(optarg, NULL, 10);
			break;
		case 'W':
			loop_work_us = strtoull(optarg, NULL, 10);
			break;
		case 'c':
			csv_path = optarg;
			break;
		case 'l':
			label = optarg;
			break;
		case 'p':
			pin_cpu = atoi(optarg);
			break;
		default:
			usage(argv[0]);
		}
	}
	if (rounds < 1 || churn_reps < 1 || churn_secs <= 0 ||
	    loop_period_us < 1)
		usage(argv[0]);

	if (pin_cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(pin_cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) != 0)
			die("pinning publisher to cpu %d: %s", pin_cpu,
			    strerror(errno));
	}

	uname(&uts);
	printf("# event bench: kernel %s, %ld cpus online, label '%s'\n",
	       uts.release, sysconf(_SC_NPROCESSORS_ONLN), label);

	if (csv_path) {
		csv = fopen(csv_path, "w");
		if (!csv)
			die("open %s: %s", csv_path, strerror(errno));
		fprintf(csv, "# kernel %s, %ld cpus online\n", uts.release,
			sysconf(_SC_NPROCESSORS_ONLN));
		fprintf(csv,
			"label,scenario,impl,waiters,round,metric,value\n");
	}

	for (ii = 0; ii < sizeof(impls) / sizeof(impls[0]); ii++) {
		const struct impl *impl = &impls[ii];

		if (!list_has(impl_list, impl->name))
			continue;

		if (list_has(scen_list, "wake"))
			for (ci = 0; ci < ncounts; ci++)
				run_wake(impl, counts[ci], rounds,
					 rounds / 10 > 3 ? rounds / 10 : 3);
		if (list_has(scen_list, "churn"))
			for (ci = 0; ci < ncounts; ci++)
				run_churn(impl, counts[ci], churn_secs,
					  churn_reps);
		if (list_has(scen_list, "loop"))
			for (ci = 0; ci < ncounts; ci++)
				run_loop(impl, counts[ci], churn_secs,
					 loop_period_us, loop_work_us);
		if (list_has(scen_list, "signal0"))
			run_signal0(impl);
		if (list_has(scen_list, "open") && impl->has_open)
			run_open(impl);
	}

	if (csv) {
		fclose(csv);
		printf("# raw samples written to %s\n", csv_path);
	}
	return 0;
}
