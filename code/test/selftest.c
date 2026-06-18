/* Functional selftest for the v5 event/waiter ABI:
 *   1. park + wake; the ready set names the fd that fired
 *   2. missed signal -> immediate return; a burst coalesces into one report;
 *      a consumed event is not reported again until it fires anew
 *   3. quorum: want-of-N waits for enough; fewer events than want returns now
 *   4. timeouts: bounded (returns the short count), poll, signal beats deadline
 *   5. EINTR on an interrupting signal
 *   6. remove: a removed event no longer wakes the waiter
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "event.h"

static int fail(const char *what) { printf("FAIL: %s (%s)\n", what, strerror(errno)); return 1; }

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_alarm(int sig) { (void)sig; }

int main(void)
{
	struct sigaction sa = { .sa_handler = on_alarm }; /* no SA_RESTART */
	int evt, e0, e1, e2, w, status, i, ready[5];
	uint64_t t0, dt;
	pid_t pid;

	sigaction(SIGALRM, &sa, NULL);

	evt = create_event();
	if (evt < 0)
		return fail("create_event");

	/* 1: child registers and parks, parent signals, child names the fd */
	pid = fork();
	if (pid == 0) {
		int r, rd[1], cw = create_waiter();
		if (cw < 0 || waiter_add(cw, evt) < 0)
			_exit(2);
		r = waiter_wait(cw, rd, 1, EVT_WAIT_FOREVER);
		_exit(r == 1 && rd[0] == evt ? 0 : 1);
	}
	sleep(1);
	if (signal_event(evt) != 1)
		return fail("signal should notify the 1 listener");
	if (waitpid(pid, &status, 0) < 0 || WEXITSTATUS(status) != 0)
		return fail("waiter should wake naming evt");
	printf("ok: park + wake, ready set names the fd\n");

	/* 2: missed signal is immediate; burst coalesces; consumed stays quiet */
	w = create_waiter();
	if (w < 0 || waiter_add(w, evt) < 0)
		return fail("create_waiter + add");
	signal_event(evt); /* not waiting yet */
	t0 = now_ms();
	if (waiter_wait(w, ready, 1, EVT_WAIT_FOREVER) != 1 || ready[0] != evt ||
	    now_ms() - t0 > 100)
		return fail("missed signal should return immediately");
	for (i = 0; i < 5; i++)
		signal_event(evt);
	if (waiter_wait(w, ready, 1, EVT_WAIT_FOREVER) != 1 || ready[0] != evt)
		return fail("burst should coalesce into one report");
	if (waiter_wait(w, ready, 1, EVT_WAIT_ZERO) != 0)
		return fail("consumed event must not be reported again");
	printf("ok: missed signal immediate, burst coalesced, consume sticks\n");

	/* 3: quorum across three events, and fewer-than-want returns now */
	e0 = create_event();
	e1 = create_event();
	e2 = create_event();
	if (e0 < 0 || e1 < 0 || e2 < 0)
		return fail("create_event x3");
	close_waiter(w);
	w = create_waiter();
	if (w < 0 || waiter_add(w, e0) || waiter_add(w, e1) || waiter_add(w, e2))
		return fail("add e0,e1,e2");
	signal_event(e0);
	signal_event(e1);
	if (waiter_wait(w, ready, 3, 200) != 2) /* only 2 of 3 ready */
		return fail("want=3 with 2 ready should time out at 2");
	signal_event(e0); /* re-arm all three */
	signal_event(e1);
	signal_event(e2);
	if (waiter_wait(w, ready, 3, EVT_WAIT_FOREVER) != 3)
		return fail("want=3 with 3 ready should return 3");
	/* listens on 3 < want 5 -> immediate, reporting whatever is ready */
	signal_event(e2);
	if (waiter_wait(w, ready, 5, EVT_WAIT_FOREVER) != 1 || ready[0] != e2)
		return fail("fewer events than want should return immediately");
	printf("ok: quorum (2/3 times out, 3/3 returns), fewer-than-want now\n");

	/* 4: timeouts and signal-beats-deadline */
	close_waiter(w);
	w = create_waiter();
	if (w < 0 || waiter_add(w, evt) < 0)
		return fail("re-add evt");
	(void)waiter_wait(w, ready, 1, EVT_WAIT_ZERO); /* drain if ready */
	t0 = now_ms();
	if (waiter_wait(w, ready, 1, 200) != 0)
		return fail("200ms wait with nothing ready should return 0");
	dt = now_ms() - t0;
	if (dt < 180 || dt > 1000)
		return fail("timeout should take ~200ms");
	if (waiter_wait(w, ready, 1, EVT_WAIT_ZERO) != 0)
		return fail("poll with nothing ready should return 0");
	pid = fork();
	if (pid == 0) {
		int rd[1], cw = create_waiter();
		if (cw < 0 || waiter_add(cw, evt) < 0)
			_exit(2);
		_exit(waiter_wait(cw, rd, 1, 5000) == 1 ? 0 : 1);
	}
	sleep(1);
	if (signal_event(evt) < 1)
		return fail("signal should wake the timed waiter");
	if (waitpid(pid, &status, 0) < 0 || WEXITSTATUS(status) != 0)
		return fail("timed waiter should report 1 ready");
	printf("ok: timeout honored (%llums), poll works, signal beats deadline\n",
	       (unsigned long long)dt);

	/* 5: EINTR on an interrupting POSIX signal */
	close_waiter(w);
	w = create_waiter();
	if (w < 0 || waiter_add(w, e0) < 0)
		return fail("add for EINTR test");
	(void)waiter_wait(w, ready, 1, EVT_WAIT_ZERO);
	for (i = 0; i < 3; i++) {
		alarm(1);
		if (waiter_wait(w, ready, 1, EVT_WAIT_FOREVER) != -1 ||
		    errno != EINTR)
			return fail("interrupted wait should fail with EINTR");
		alarm(0);
	}
	printf("ok: EINTR x3\n");

	/* 6: a removed event no longer wakes the waiter */
	if (waiter_remove(w, e0) < 0)
		return fail("waiter_remove");
	if (signal_event(e0) != 0)
		return fail("removed listener must not be notified");
	if (waiter_wait(w, ready, 1, 100) != 0)
		return fail("empty waiter should return 0 immediately");
	printf("ok: remove unlinks the listener\n");

	close_waiter(w);
	close_event(evt);
	close_event(e0);
	close_event(e1);
	close_event(e2);
	printf("all selftests passed\n");
	return 0;
}
