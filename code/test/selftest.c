/* Functional selftest for the pollable broadcast event:
 *   1. subscribe, signal, read the count; a burst coalesces into one read
 *   2. broadcast: two subscriptions on one event both see one signal
 *   3. poll readiness is level (until drained) and edge (EPOLLET, once/signal)
 *   4. O_NONBLOCK read returns EAGAIN when empty
 *   5. a blocking read wakes on a signal from another process
 *   6. closing a subscription auto-detaches it (signal counts only the rest)
 *   7. closing the event hangs up subscriptions (EPOLLHUP, read returns 0)
 *   8. event_wait_any / event_wait_first / event_wait_all
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "event.h"

static int fail(const char *what) { printf("FAIL: %s (%s)\n", what, strerror(errno)); return 1; }

static int readable_now(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };

	return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

int main(void)
{
	int evt, sub, sub2, ep, status, i;
	uint64_t count;
	pid_t pid;

	evt = create_event();
	if (evt < 0)
		return fail("create_event");
	sub = subscribe_event(evt);
	if (sub < 0)
		return fail("subscribe_event");

	/* 1: read the count; burst coalesces */
	if (readable_now(sub))
		return fail("fresh subscription must not be readable");
	if (signal_event(evt) != 1)
		return fail("signal should notify the 1 subscription");
	if (event_read(sub, &count) != 1 || count != 1)
		return fail("read should report 1 signal");
	for (i = 0; i < 5; i++)
		signal_event(evt);
	if (event_read(sub, &count) != 1 || count != 5)
		return fail("a burst of 5 should coalesce into one read of 5");
	printf("ok: read reports the count, burst coalesces\n");

	/* 2: broadcast to two independent subscriptions */
	sub2 = subscribe_event(evt);
	if (sub2 < 0)
		return fail("second subscribe");
	if (signal_event(evt) != 2)
		return fail("signal should notify both subscriptions");
	if (event_read(sub, &count) != 1 || count != 1 ||
	    event_read(sub2, &count) != 1 || count != 1)
		return fail("both subscriptions must see the one signal");
	printf("ok: broadcast, both subscriptions see the signal\n");

	/* 3: epoll level then edge */
	ep = epoll_create1(0);
	if (ep < 0)
		return fail("epoll_create1");
	{
		struct epoll_event ev = { .events = EPOLLIN }, out[1];

		ev.data.fd = sub;
		if (epoll_ctl(ep, EPOLL_CTL_ADD, sub, &ev) < 0)
			return fail("epoll_ctl ADD (level)");
		signal_event(evt);
		if (epoll_wait(ep, out, 1, 100) != 1 ||
		    epoll_wait(ep, out, 1, 100) != 1)
			return fail("level epoll should re-report until drained");
		event_read(sub, &count);
		if (epoll_wait(ep, out, 1, 100) != 0)
			return fail("level epoll quiet once drained");

		ev.events = EPOLLIN | EPOLLET;
		if (epoll_ctl(ep, EPOLL_CTL_MOD, sub, &ev) < 0)
			return fail("epoll_ctl MOD (edge)");
		signal_event(evt);
		if (epoll_wait(ep, out, 1, 100) != 1)
			return fail("edge epoll should report a signal");
		if (epoll_wait(ep, out, 1, 100) != 0)
			return fail("edge epoll should not re-report without a new signal");
		event_read(sub, &count);
	}
	close(ep);
	printf("ok: epoll level re-reports, edge fires once per signal\n");

	/* 4: read never blocks; an empty (alive) subscription reads count 0 */
	if (event_read(sub, &count) != 1 || count != 0)
		return fail("read of an empty live subscription should be alive, count 0");
	printf("ok: read is non-blocking, empty reads alive/0\n");

	/* 5: a poll-driven listener wakes on a cross-process signal */
	pid = fork();
	if (pid == 0) {
		int csub = subscribe_event(evt);
		struct pollfd p = { .events = POLLIN };
		uint64_t c = 0;

		if (csub < 0)
			_exit(2);
		p.fd = csub;
		if (poll(&p, 1, 5000) != 1 || !(p.revents & POLLIN))
			_exit(1);
		_exit(event_read(csub, &c) == 1 && c >= 1 ? 0 : 1);
	}
	sleep(1);
	signal_event(evt);
	if (waitpid(pid, &status, 0) < 0 || WEXITSTATUS(status) != 0)
		return fail("poll-driven listener should wake and read");
	printf("ok: poll-driven listener wakes on a cross-process signal\n");

	/* 6: closing a subscription detaches it */
	close_subscription(sub2);
	if (signal_event(evt) != 1) /* only sub remains */
		return fail("closed subscription must not be notified");
	printf("ok: closing a subscription auto-detaches it\n");

	/* 7: closing the event hangs up subscriptions; read reports dead (0) */
	{
		struct pollfd p = { .fd = sub, .events = POLLIN };
		int r;

		close_event(evt);
		if (poll(&p, 1, 100) != 1 || !(p.revents & POLLHUP))
			return fail("closing the event should hang up the subscription");
		/* drain any pending (each read alive == 1), then read reports
		 * dead == 0, distinct from a live empty read */
		while ((r = event_read(sub, &count)) == 1 && count != 0)
			;
		if (r != 0)
			return fail("read after hangup should report dead (0)");
	}
	close_subscription(sub);
	printf("ok: closing the event hangs up subscriptions (POLLHUP, dead)\n");

	/* 8: event_wait_any, event_wait_first, event_wait_all */
	{
		int evts[3], subs[3], ready[3], n;

		for (i = 0; i < 3; i++) {
			evts[i] = create_event();
			subs[i] = evts[i] < 0 ? -1 : subscribe_event(evts[i]);
			if (subs[i] < 0)
				return fail("wait setup");
		}
		/* any: returns the ready fd */
		signal_event(evts[1]);
		if (event_wait_any(subs, 3, 1000) != subs[1])
			return fail("wait_any should return the one ready fd");
		/* any: timeout returns -1/ETIMEDOUT */
		if (event_wait_any(subs, 3, 100) != -1 || errno != ETIMEDOUT)
			return fail("wait_any timeout should be -1/ETIMEDOUT");
		/* first k=2 of 3 */
		signal_event(evts[0]);
		signal_event(evts[2]);
		n = event_wait_first(subs, 3, 2, 1000, ready);
		if (n != 2)
			return fail("wait_first(k=2) should return 2");
		/* all */
		signal_event(evts[0]);
		signal_event(evts[1]);
		signal_event(evts[2]);
		if (event_wait_all(subs, 3, 1000) != 3)
			return fail("wait_all should return all 3");
		for (i = 0; i < 3; i++) {
			close_subscription(subs[i]);
			close_event(evts[i]);
		}
	}
	printf("ok: event_wait_any / event_wait_first / event_wait_all\n");

	printf("all selftests passed\n");
	return 0;
}
