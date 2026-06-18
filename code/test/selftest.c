/* Functional selftest for the pollable event:
 *   1. read returns the signal count; a burst coalesces into one read
 *   2. poll() readiness is level: ready after a signal, stays ready until read
 *      drains it, clears after
 *   3. O_NONBLOCK read returns EAGAIN when empty
 *   4. a blocking read wakes on a signal from another process
 *   5. epoll level-triggered re-reports until drained; EPOLLET fires once per
 *      signal
 *   6. select() sees readiness
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include "event.h"

static int fail(const char *what) { printf("FAIL: %s (%s)\n", what, strerror(errno)); return 1; }

/* poll the fd with a zero timeout: is it readable right now? */
static int readable_now(int evt)
{
	struct pollfd p = { .fd = evt, .events = POLLIN };

	return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

int main(void)
{
	int evt, nb, ep, status, i;
	uint64_t count;
	pid_t pid;

	evt = create_event();
	if (evt < 0)
		return fail("create_event");

	/* 1: read returns the count; a burst coalesces */
	if (readable_now(evt))
		return fail("fresh event must not be readable");
	if (signal_event(evt) != 0)
		return fail("signal_event");
	if (!readable_now(evt))
		return fail("event must be readable after a signal");
	if (event_read(evt, &count) != 0 || count != 1)
		return fail("read should report 1 signal");
	for (i = 0; i < 5; i++)
		signal_event(evt);
	if (event_read(evt, &count) != 0 || count != 5)
		return fail("a burst of 5 should coalesce into one read of 5");
	printf("ok: read reports the count, burst coalesces\n");

	/* 2: readiness is level and clears on read */
	signal_event(evt);
	if (!readable_now(evt) || !readable_now(evt))
		return fail("readiness must persist until drained (level)");
	if (event_read(evt, &count) != 0 || count != 1)
		return fail("drain read");
	if (readable_now(evt))
		return fail("must not be readable after draining");
	printf("ok: level-triggered readiness, clears on read\n");

	/* 3: O_NONBLOCK read returns EAGAIN when empty */
	nb = create_event_flags(O_NONBLOCK);
	if (nb < 0)
		return fail("create_event_flags(O_NONBLOCK)");
	if (event_read(nb, &count) != -1 || errno != EAGAIN)
		return fail("nonblocking read of an empty event should EAGAIN");
	signal_event(nb);
	if (event_read(nb, &count) != 0 || count != 1)
		return fail("nonblocking read after a signal should return 1");
	close_event(nb);
	printf("ok: O_NONBLOCK read EAGAIN then 1\n");

	/* 4: a blocking read wakes on a signal from another process */
	pid = fork();
	if (pid == 0) {
		uint64_t c = 0;
		_exit(event_read(evt, &c) == 0 && c >= 1 ? 0 : 1);
	}
	sleep(1);
	if (signal_event(evt) != 0)
		return fail("signal to the blocked reader");
	if (waitpid(pid, &status, 0) < 0 || WEXITSTATUS(status) != 0)
		return fail("blocked reader should wake and read");
	printf("ok: blocking read wakes on a cross-process signal\n");

	/* 5: epoll level vs edge */
	ep = epoll_create1(0);
	if (ep < 0)
		return fail("epoll_create1");
	{
		struct epoll_event ev = { .events = EPOLLIN }, out[1];

		ev.data.fd = evt;
		if (epoll_ctl(ep, EPOLL_CTL_ADD, evt, &ev) < 0)
			return fail("epoll_ctl ADD (level)");
		signal_event(evt);
		if (epoll_wait(ep, out, 1, 100) != 1)
			return fail("level epoll should report the signal");
		if (epoll_wait(ep, out, 1, 100) != 1)
			return fail("level epoll should re-report until drained");
		if (event_read(evt, &count) != 0)
			return fail("drain after level epoll");
		if (epoll_wait(ep, out, 1, 100) != 0)
			return fail("level epoll should be quiet once drained");

		ev.events = EPOLLIN | EPOLLET;
		if (epoll_ctl(ep, EPOLL_CTL_MOD, evt, &ev) < 0)
			return fail("epoll_ctl MOD (edge)");
		signal_event(evt);
		if (epoll_wait(ep, out, 1, 100) != 1)
			return fail("edge epoll should report a signal");
		if (epoll_wait(ep, out, 1, 100) != 0)
			return fail("edge epoll should not re-report without a new signal");
		signal_event(evt);
		if (epoll_wait(ep, out, 1, 100) != 1)
			return fail("edge epoll should report the next signal");
		event_read(evt, &count);
	}
	close(ep);
	printf("ok: epoll level re-reports, edge fires once per signal\n");

	/* 6: select() sees readiness */
	{
		fd_set r;
		struct timeval tv = { 0, 0 };

		FD_ZERO(&r);
		FD_SET(evt, &r);
		if (select(evt + 1, &r, NULL, NULL, &tv) != 0)
			return fail("select should see an empty event as not ready");
		signal_event(evt);
		FD_ZERO(&r);
		FD_SET(evt, &r);
		if (select(evt + 1, &r, NULL, NULL, &tv) != 1 ||
		    !FD_ISSET(evt, &r))
			return fail("select should see a signaled event as ready");
		event_read(evt, &count);
	}
	printf("ok: select() sees readiness\n");

	close_event(evt);
	printf("all selftests passed\n");
	return 0;
}
