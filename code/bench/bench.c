// SPDX-License-Identifier: MIT
/*
 * bench.c - performance harness for the event object (/dev/event).
 *
 * The point of this program is *comparability*: event.ko will go through many
 * implementation iterations, and we want a definitive answer to "is the new
 * one better, and in which regime?". So the harness
 *
 *   - measures the handful of numbers that distinguish implementations (see
 *     the scenario list below),
 *   - sweeps the waiter count, because "better" usually depends on it,
 *   - runs every scenario against controls: the kernel's own *eventfd* (the
 *     same shape as our object, an ioctl signal aside) and a per-waiter
 *     *futex*. The control numbers anchor what the hardware + scheduler can do
 *     and catch a noisy machine: between two runs they should not move.
 *   - dumps every raw sample to CSV (-c) so compare.py can attach percentiles
 *     and a significance test to the A/B verdict.
 *
 * The object is single-consumer (eventfd-shaped), so the model is one waitable
 * object per waiter; the publisher signals each. Scenarios (-s, default all):
 *
 *   wake     N waiters each block on their own object; the publisher signals
 *            all N, -r rounds. Metrics: wake_ns (per waiter: signal to that
 *            waiter running), last_wake_ns (until the slowest is running),
 *            signal_call_ns (publisher time to signal all N: the per-waiter
 *            fan-out cost, one broadcast for futex... no, per-waiter here too).
 *
 *   churn    N waiters re-arm in a tight loop while the publisher signals all
 *            their objects flat out for -d seconds. Metrics: wakes_per_sec
 *            (counted waiter-side), signal_calls_per_sec, waiter_wakes
 *            (fairness).
 *
 *   loop     The realistic loop: N waiters each wait, simulate -W us of work,
 *            re-arm; the publisher signals all every -P us for -d seconds.
 *            Metrics: loop_wake_ns (most-recent-signal to running),
 *            missed_signals (fired while working, coalesced = sent - caught),
 *            signals_sent.
 *
 *   signal0  signal cost with nobody reading, batched. Metric: signal0_ns.
 *
 *   open     create+destroy cost of one object, batched. Metric: open_close_ns.
 *
 * Validity: a wake round only measures wake latency if every waiter is parked
 * before the signal. The publisher polls /proc/self/task/<tid>/stat until
 * every waiter thread is in state 'S' (parked in its wait path), then signals
 * and waits for all to report woken under a stuck-timeout.
 *
 * Build:  make            (header-only API from ../lib, plus -pthread)
 * Run:    ./bench [-i event,eventfd,futex] [-s wake,churn,loop,signal0,open]
 *                 [-N 1,2,4,16,64,256] [-r rounds] [-d secs] [-R reps]
 *                 [-P period_us] [-W work_us] [-c out.csv] [-l label] [-p cpu]
 *
 * The event scenarios need /dev/event (event.ko loaded); -i eventfd,futex run
 * anywhere, handy for testing the harness.
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
#include <sys/eventfd.h>
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
 * that sleeps, means it has entered the wait path and is enqueued (event,
 * eventfd and futex all enqueue before marking the task sleeping).
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
 * One waitable object per waiter. wait() blocks until the object is signaled;
 * signal() signals one object. The object under test (event) is driven through
 * the public ../lib/event.h API; eventfd and a per-waiter futex are the
 * kernel-native controls doing the same single-consumer job.
 */

struct obj {
	int fd;			 /* event / eventfd */
	_Atomic uint32_t futex;	 /* futex word */
	uint32_t my_gen;	 /* futex: generation this waiter carries */
};

struct impl {
	const char *name;
	bool has_open; /* supports the open (create/destroy) scenario */
	void (*create)(struct obj *o);
	void (*destroy)(struct obj *o);
	void (*wait)(struct obj *o);
	void (*signal)(struct obj *o);
};

/* event: the object under test, blocked on with a plain read() (poll/epoll
 * work too; read is the most direct wake-latency measure). */

static void event_create(struct obj *o)
{
	o->fd = create_event();
	if (o->fd < 0)
		die("create_event: %s (is event.ko loaded?)", strerror(errno));
}

static void fd_destroy(struct obj *o)
{
	close(o->fd);
}

static void fd_wait_read(struct obj *o)
{
	uint64_t c;

	if (read(o->fd, &c, sizeof(c)) != (ssize_t)sizeof(c))
		die("read: %s", strerror(errno));
}

static void event_signal(struct obj *o)
{
	if (signal_event(o->fd) < 0)
		die("signal_event: %s", strerror(errno));
}

/* eventfd: the same-shape control (counter + read drains; write adds). */

static void eventfd_create(struct obj *o)
{
	o->fd = eventfd(0, 0);
	if (o->fd < 0)
		die("eventfd: %s", strerror(errno));
}

static void eventfd_signal(struct obj *o)
{
	uint64_t one = 1;

	if (write(o->fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		die("eventfd write: %s", strerror(errno));
}

/* futex: a per-waiter 32-bit generation. wait FUTEX_WAITs while the word still
 * equals the generation carried; signal bumps it and FUTEX_WAKEs the one
 * waiter. The bench never runs long enough to wrap 32 bits. */

static long sys_futex(_Atomic uint32_t *uaddr, int op, uint32_t val)
{
	return syscall(SYS_futex, uaddr, op, val, NULL, NULL, 0);
}

static void futex_create(struct obj *o)
{
	atomic_store(&o->futex, 0);
	o->my_gen = 0;
}

static void futex_noop_destroy(struct obj *o)
{
	(void)o;
}

static void futex_wait(struct obj *o)
{
	uint32_t cur;

	while ((cur = atomic_load(&o->futex)) == o->my_gen) {
		long ret = sys_futex(&o->futex, FUTEX_WAIT_PRIVATE, o->my_gen);

		if (ret < 0 && errno != EAGAIN && errno != EINTR)
			die("futex_wait: %s", strerror(errno));
	}
	o->my_gen = cur;
}

static void futex_signal(struct obj *o)
{
	atomic_fetch_add(&o->futex, 1);
	if (sys_futex(&o->futex, FUTEX_WAKE_PRIVATE, 1) < 0)
		die("futex_wake: %s", strerror(errno));
}

static const struct impl impls[] = {
	{
		.name = "event",
		.has_open = true,
		.create = event_create,
		.destroy = fd_destroy,
		.wait = fd_wait_read,
		.signal = event_signal,
	},
	{
		.name = "eventfd",
		.has_open = true,
		.create = eventfd_create,
		.destroy = fd_destroy,
		.wait = fd_wait_read,
		.signal = eventfd_signal,
	},
	{
		.name = "futex",
		.has_open = false,
		.create = futex_create,
		.destroy = futex_noop_destroy,
		.wait = futex_wait,
		.signal = futex_signal,
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

/* --- scenario: wake (per-waiter fan-out latency) ----------------------------
 *
 * Per round: every waiter blocks on its own object (publisher verifies they
 * are parked), the publisher timestamps, signals all N objects, and every
 * waiter timestamps the moment it is running again. A per-round start barrier
 * (including the publisher) resets state without races; a woke counter under a
 * stuck-timeout catches a lost wakeup.
 */

struct wake_shared {
	const struct impl *impl;
	struct obj *objs;
	pthread_barrier_t start; /* n + 1 parties */
	_Atomic int ready;
	_Atomic int woke;
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
		atomic_fetch_add(&sh->ready, 1);
		sh->impl->wait(&sh->objs[a->idx]);
		sh->wake_ts[a->idx] = now_ns();
		atomic_fetch_add(&sh->woke, 1);
	}
	return NULL;
}

static void run_wake(const struct impl *impl, int n, int rounds, int warmup)
{
	struct wake_shared sh = { .impl = impl, .n = n };
	struct wake_waiter_arg *args;
	struct vec wake = { 0 }, last = { 0 }, sig = { 0 };
	pthread_t *threads;
	int r, i;

	sh.objs = calloc(n, sizeof(*sh.objs));
	sh.tids = calloc(n, sizeof(*sh.tids));
	sh.wake_ts = calloc(n, sizeof(*sh.wake_ts));
	threads = calloc(n, sizeof(*threads));
	args = calloc(n, sizeof(*args));
	if (!sh.objs || !sh.tids || !sh.wake_ts || !threads || !args)
		die("out of memory");
	for (i = 0; i < n; i++)
		impl->create(&sh.objs[i]);
	pthread_barrier_init(&sh.start, NULL, n + 1);

	for (i = 0; i < n; i++) {
		args[i] = (struct wake_waiter_arg){ .sh = &sh, .idx = i };
		threads[i] = spawn_waiter(wake_waiter, &args[i]);
	}

	for (r = 0; r < warmup + rounds; r++) {
		uint64_t t0, t1, deadline, slowest = 0;

		atomic_store(&sh.ready, 0);
		atomic_store(&sh.woke, 0);
		pthread_barrier_wait(&sh.start);

		deadline = now_ns() + STUCK_TIMEOUT_NS;
		while (atomic_load(&sh.ready) < n) {
			if (now_ns() > deadline)
				die("wake/%s n=%d: waiters never became ready",
				    impl->name, n);
			relax();
		}
		wait_all_parked(sh.tids, n, "wake");

		t0 = now_ns();
		for (i = 0; i < n; i++)
			impl->signal(&sh.objs[i]);
		t1 = now_ns();

		while (atomic_load(&sh.woke) < n) {
			if (now_ns() > deadline)
				die("wake/%s n=%d round %d: only %d/%d woke: "
				    "lost wakeup",
				    impl->name, n, r, atomic_load(&sh.woke), n);
			relax();
		}

		if (r < warmup)
			continue;

		vec_push(&sig, (double)(t1 - t0));
		csv_emit("wake", impl->name, n, r - warmup, "signal_call_ns",
			 (double)(t1 - t0));
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

	printf("wake    %-7s n=%-4d rounds=%-4d "
	       "wake p50=%7.2fus p99=%8.2fus  last p50=%8.2fus  "
	       "signal-all p50=%7.2fus\n",
	       impl->name, n, rounds, vec_pct(&wake, 50) / 1e3,
	       vec_pct(&wake, 99) / 1e3, vec_pct(&last, 50) / 1e3,
	       vec_pct(&sig, 50) / 1e3);

	pthread_barrier_destroy(&sh.start);
	vec_reset(&wake);
	vec_reset(&last);
	vec_reset(&sig);
	for (i = 0; i < n; i++)
		impl->destroy(&sh.objs[i]);
	free(sh.objs);
	free(sh.tids);
	free(sh.wake_ts);
	free(threads);
	free(args);
}

/* --- scenario: churn (sustained throughput) --------------------------------
 *
 * No round gating: waiters re-arm as fast as they can and the publisher signals
 * every object as fast as it can. Throughput is counted waiter-side (actual
 * wait returns) over the timed window.
 */

struct churn_count {
	_Atomic uint64_t c;
	char pad[64 - sizeof(_Atomic uint64_t)]; /* avoid false sharing */
};

struct churn_shared {
	const struct impl *impl;
	struct obj *objs;
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
		sh->impl->wait(&sh->objs[a->idx]);
		atomic_fetch_add(&sh->counts[a->idx].c, 1);
	}
	atomic_fetch_add(&sh->exited, 1);
	return NULL;
}

static void run_churn(const struct impl *impl, int n, double dur_secs, int reps)
{
	int rep, i;

	for (rep = 0; rep < reps; rep++) {
		struct churn_shared sh = { .impl = impl, .n = n };
		struct churn_waiter_arg *args;
		pthread_t *threads;
		uint64_t t0, t1, t_end, drain_deadline;
		uint64_t total_wakes = 0, calls = 0;
		uint64_t min_wakes = UINT64_MAX, max_wakes = 0;
		double elapsed, wps, cps;

		sh.objs = calloc(n, sizeof(*sh.objs));
		sh.counts = calloc(n, sizeof(*sh.counts));
		threads = calloc(n, sizeof(*threads));
		args = calloc(n, sizeof(*args));
		if (!sh.objs || !sh.counts || !threads || !args)
			die("out of memory");
		for (i = 0; i < n; i++)
			impl->create(&sh.objs[i]);
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
			for (i = 0; i < n; i++)
				impl->signal(&sh.objs[i]);
			calls++;
		}

		atomic_store(&sh.stop, true);
		drain_deadline = now_ns() + STUCK_TIMEOUT_NS;
		while (atomic_load(&sh.exited) < n) {
			if (now_ns() > drain_deadline)
				die("churn/%s n=%d: waiters won't drain, "
				    "lost wakeup?",
				    impl->name, n);
			for (i = 0; i < n; i++)
				impl->signal(&sh.objs[i]);
			usleep(100);
		}
		for (i = 0; i < n; i++)
			pthread_join(threads[i], NULL);

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
		cps = (double)calls / elapsed; /* full sweeps of all N */
		csv_emit("churn", impl->name, n, rep, "wakes_per_sec", wps);
		csv_emit("churn", impl->name, n, rep, "signal_calls_per_sec",
			 cps);

		printf("churn   %-7s n=%-4d rep=%d  %10.0f wakes/s  "
		       "%9.0f sweeps/s  fairness min/max=%llu/%llu\n",
		       impl->name, n, rep, wps, cps,
		       (unsigned long long)min_wakes,
		       (unsigned long long)max_wakes);

		pthread_barrier_destroy(&sh.start);
		for (i = 0; i < n; i++)
			impl->destroy(&sh.objs[i]);
		free(sh.objs);
		free(sh.counts);
		free(threads);
		free(args);
	}
}

/* --- scenario: loop (the realistic subscriber loop) -------------------------
 *
 * N waiters: wait -> simulate work -> re-arm. The publisher signals every
 * object on a fixed period. last_signal_ts is stamped just before each sweep;
 * a woken waiter takes (now - last_signal_ts). Over the run the publisher sends
 * g signals per object; a waiter that was always parked returns g times, so
 * missed = g - caught is exactly the signals coalesced while it was working.
 */

struct loop_shared {
	const struct impl *impl;
	struct obj *objs;
	pthread_barrier_t start; /* n + 1 parties */
	_Atomic bool stop;
	_Atomic int exited;
	_Atomic uint64_t last_signal_ts;
	uint64_t work_ns;
	int n;
};

struct loop_waiter_arg {
	struct loop_shared *sh;
	int idx;
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
		sh->impl->wait(&sh->objs[a->idx]);
		if (atomic_load(&sh->stop))
			break;
		t = now_ns();
		vec_push(&a->lat,
			 (double)(t - atomic_load(&sh->last_signal_ts)));
		a->caught++;
		while (sh->work_ns && now_ns() < t + sh->work_ns)
			relax();
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
	struct vec lat = { 0 };
	uint64_t t_end, t_next, g = 0, drain_deadline;
	uint64_t caught = 0, missed;
	int i;

	sh.objs = calloc(n, sizeof(*sh.objs));
	threads = calloc(n, sizeof(*threads));
	args = calloc(n, sizeof(*args));
	if (!sh.objs || !threads || !args)
		die("out of memory");
	for (i = 0; i < n; i++)
		impl->create(&sh.objs[i]);
	pthread_barrier_init(&sh.start, NULL, n + 1);

	for (i = 0; i < n; i++) {
		args[i] = (struct loop_waiter_arg){ .sh = &sh, .idx = i };
		threads[i] = spawn_waiter(loop_waiter, &args[i]);
	}
	pthread_barrier_wait(&sh.start);

	t_next = now_ns();
	t_end = t_next + (uint64_t)(dur_secs * NSEC_PER_SEC);
	while (now_ns() < t_end) {
		struct timespec ts;

		g++;
		atomic_store(&sh.last_signal_ts, now_ns());
		for (i = 0; i < n; i++)
			impl->signal(&sh.objs[i]);

		t_next += period_us * 1000;
		ts.tv_sec = t_next / NSEC_PER_SEC;
		ts.tv_nsec = t_next % NSEC_PER_SEC;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
	}

	atomic_store(&sh.stop, true);
	drain_deadline = now_ns() + STUCK_TIMEOUT_NS;
	while (atomic_load(&sh.exited) < n) {
		if (now_ns() > drain_deadline)
			die("loop/%s n=%d: waiters won't drain", impl->name, n);
		for (i = 0; i < n; i++)
			impl->signal(&sh.objs[i]);
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

	printf("loop    %-7s n=%-4d period=%lluus work=%lluus  "
	       "signals=%-6llu  wake p50=%8.2fus p99=%8.2fus  "
	       "missed=%llu (%.2f%% of deliveries)\n",
	       impl->name, n, (unsigned long long)period_us,
	       (unsigned long long)work_us, (unsigned long long)g,
	       vec_pct(&lat, 50) / 1e3, vec_pct(&lat, 99) / 1e3,
	       (unsigned long long)missed,
	       g && n ? 100.0 * (double)missed / ((double)g * n) : 0.0);

	pthread_barrier_destroy(&sh.start);
	vec_reset(&lat);
	for (i = 0; i < n; i++)
		impl->destroy(&sh.objs[i]);
	free(sh.objs);
	free(threads);
	free(args);
}

/* --- scenario: signal0 (signal with nobody reading) ------------------------ */

#define BATCH_OPS 1000
#define BATCHES 200

static void run_signal0(const struct impl *impl)
{
	struct vec s = { 0 };
	struct obj o;
	int b, k;

	impl->create(&o);
	for (b = 0; b < BATCHES; b++) {
		uint64_t t0 = now_ns(), t1;
		double per_op;

		for (k = 0; k < BATCH_OPS; k++)
			impl->signal(&o);
		t1 = now_ns();
		per_op = (double)(t1 - t0) / BATCH_OPS;
		vec_push(&s, per_op);
		csv_emit("signal0", impl->name, 0, b, "signal0_ns", per_op);
	}
	impl->destroy(&o);

	printf("signal0 %-7s       p50=%7.1fns  p99=%7.1fns  mean=%7.1fns\n",
	       impl->name, vec_pct(&s, 50), vec_pct(&s, 99), vec_mean(&s));
	vec_reset(&s);
}

/* --- scenario: open (create/destroy cost) ---------------------------------- */

static void run_open(const struct impl *impl)
{
	struct vec s = { 0 };
	int b, k;

	for (b = 0; b < BATCHES; b++) {
		uint64_t t0 = now_ns(), t1;
		double per_op;

		for (k = 0; k < BATCH_OPS; k++) {
			struct obj o;

			impl->create(&o);
			impl->destroy(&o);
		}
		t1 = now_ns();
		per_op = (double)(t1 - t0) / BATCH_OPS;
		vec_push(&s, per_op);
		csv_emit("open", impl->name, 0, b, "open_close_ns", per_op);
	}

	printf("open    %-7s       p50=%7.1fns  p99=%7.1fns  mean=%7.1fns\n",
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
		"  -i list   implementations: event,eventfd,futex (default all)\n"
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
	const char *impl_list = "event,eventfd,futex";
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
		fprintf(csv, "label,scenario,impl,waiters,round,metric,value\n");
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
