/* Functional selftest for the v4 event ABI (one gen+timeout wait):
 *   1. park + wake, gen advances
 *   2. missed signal -> immediate return; burst coalesces into one gen jump
 *   3. timeouts: bounded, poll, signal-beats-deadline, gen untouched on expiry
 *   4. EINTR on interrupting signal; abandoned nodes never corrupt anything
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
	uint64_t gen = 0, t0, dt;
	int evt, status, i;
	pid_t pid;

	sigaction(SIGALRM, &sa, NULL);

	evt = create_event();
	if (evt < 0)
		return fail("create_event");

	/* 1: child parks, parent signals, child sees gen 1 */
	pid = fork();
	if (pid == 0) {
		uint64_t g = 0;
		_exit(wait_for_event(evt, &g, EVT_WAIT_FOREVER) == 0 && g == 1 ? 0 : 1);
	}
	sleep(1);
	if (signal_event(evt) != 1)
		return fail("signal should wake the 1 parked waiter");
	if (waitpid(pid, &status, 0) < 0 || WEXITSTATUS(status) != 0)
		return fail("waiter should wake with gen=1");
	printf("ok: park + wake, gen=1\n");
	gen = 1;

	/* 2: missed signal returns immediately; burst coalesces */
	signal_event(evt); /* gen -> 2, nobody waiting */
	t0 = now_ms();
	if (wait_for_event(evt, &gen, EVT_WAIT_FOREVER) != 0 || gen != 2 || now_ms() - t0 > 100)
		return fail("missed signal should return immediately with gen=2");
	for (i = 0; i < 5; i++)
		signal_event(evt); /* gen -> 7 */
	if (wait_for_event(evt, &gen, EVT_WAIT_FOREVER) != 0 || gen != 7)
		return fail("burst of 5 should coalesce to gen=7");
	printf("ok: missed signal immediate, burst coalesced (gen=7)\n");

	/* 3: timeouts */
	t0 = now_ms();
	if (wait_for_event(evt, &gen, 200) != EVT_TIMEOUT)
		return fail("200ms wait should time out");
	dt = now_ms() - t0;
	if (dt < 180 || dt > 1000 || gen != 7)
		return fail("timeout should take ~200ms and leave gen alone");
	if (wait_for_event(evt, &gen, EVT_WAIT_ZERO) != EVT_TIMEOUT)
		return fail("zero-timeout poll should report EVT_TIMEOUT");
	pid = fork();
	if (pid == 0) {
		uint64_t g = 7;
		_exit(wait_for_event(evt, &g, 5000) == EVT_SIGNALED ?
			      0 : 1);
	}
	sleep(1);
	if (signal_event(evt) != 1)
		return fail("signal should wake the timed waiter");
	if (waitpid(pid, &status, 0) < 0 || WEXITSTATUS(status) != 0)
		return fail("timed waiter should report EVT_SIGNALED");
	printf("ok: timeout honored (%llums), poll works, signal beats deadline\n",
	       (unsigned long long)dt);
	gen = 8;

	/* 4: EINTR, then abandoned nodes are reaped invisibly */
	for (i = 0; i < 3; i++) {
		alarm(1);
		if (wait_for_event(evt, &gen, EVT_WAIT_FOREVER) == 0 || errno != EINTR)
			return fail("interrupted wait should fail with EINTR");
		alarm(0);
	}
	if (signal_event(evt) != 0)
		return fail("abandoned nodes must not count as woken");
	gen = 9;
	pid = fork();
	if (pid == 0) {
		uint64_t g = 9;
		_exit(wait_for_event(evt, &g, EVT_WAIT_FOREVER) == 0 ? 0 : 1);
	}
	sleep(1);
	if (signal_event(evt) != 1)
		return fail("signal after cancels should wake exactly 1");
	if (waitpid(pid, &status, 0) < 0 || WEXITSTATUS(status) != 0)
		return fail("waiter after cancels");
	printf("ok: EINTR x3, abandoned nodes reaped, later waiters unaffected\n");

	close_event(evt);
	printf("all selftests passed\n");
	return 0;
}
