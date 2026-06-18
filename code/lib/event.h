/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only userspace API for event.ko (/dev/event).
 *
 * A broadcast event: one source, many listeners, each sees every signal.
 *
 *   publisher:
 *     int evt = create_event();
 *     signal_event(evt);          // raise the generation, wake all listeners
 *
 *   listener (its own loop, may also watch other pollable fds):
 *     int sub = subscribe_event(evt);   // a pollable fd of its own
 *     struct pollfd p = { .fd = sub, .events = POLLIN };
 *     poll(&p, 1, -1);            // or epoll/select; it is a real fd
 *     uint64_t n;
 *     event_read(sub, &n);        // n = signals since last read; clears ready
 *
 * The event fd is shared (fork/dup); each listener SUBSCRIBEs to get its own
 * subscription fd, which carries its own consumed generation, so every listener
 * sees every signal (true broadcast) and a burst coalesces into one read. A
 * listener waits on k-of-n events with its own epoll (see event_wait_first /
 * event_wait_any / event_wait_all, which the kernel leaves to userspace). The
 * ABI below must stay in sync with ../module/event_uapi.h.
 */
#ifndef EVENT_H
#define EVENT_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_DEVICE "/dev/event"

#define EVENT_IOC_MAGIC 'E'
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)
#define EVENT_IOC_SUBSCRIBE _IO(EVENT_IOC_MAGIC, 6)

/* Upper bound on n for event_wait_first / event_wait_any / event_wait_all.
 * They build per-call arrays on the stack, so n is bounded; for a larger set,
 * run your own epoll over the subscription fds. */
#define EVENT_WAIT_MAX 1024

/* ---- publisher / factory ---- */

/* Allocate a new broadcast event; returns an fd, or -1 with errno set. */
static inline int create_event(void)
{
	return open(EVENT_DEVICE, O_RDWR);
}

/* Raise the generation and wake every subscription; returns the number
 * notified, or -1 with errno set. */
static inline int signal_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SIGNAL);
}

/* Destroy the event once the last reference to it is closed. Live
 * subscriptions keep working (they see a hangup) until they too are closed. */
static inline int close_event(int evt)
{
	return close(evt);
}

/* ---- listener ----
 *
 * Return conventions across the consume/wait helpers (each is detailed at its
 * definition; collected here because they differ):
 *
 *   event_read       1 alive (*count set, 0 = nothing) / 0 dead / -1 errno
 *   event_wait       count (>= 1) / 0 timeout / -1 errno (ESHUTDOWN = closed)
 *   event_wait_first count written (k on success, fewer on timeout) / -1 errno
 *   event_wait_any   the ready fd (>= 0) / -1 errno (ETIMEDOUT = timeout)
 *   event_wait_all   n / fewer on timeout / -1 errno
 */

/* Create a subscription onto evt; returns a pollable fd, or -1 with errno. */
static inline int subscribe_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SUBSCRIBE);
}

/*
 * Consume signals on a subscription, never blocking (it is poll-to-wait,
 * read-to-consume): set *count to the number of signals since the last read and
 * advance the cursor. The return value tells you which of three things happened:
 *   1   alive: *count is valid (0 means nothing fired for you, > 0 is the count)
 *   0   dead:  the event was closed; no more signals will ever come (a hangup)
 *  -1   error: errno is set
 */
static inline int event_read(int sub, uint64_t *count)
{
	uint64_t c;
	ssize_t r = read(sub, &c, sizeof(c));

	if (r == (ssize_t)sizeof(c)) {
		*count = c;
		return 1; /* alive */
	}
	if (r == 0)
		return 0; /* event closed (hangup) */
	return -1;	  /* error */
}

/*
 * Convenience for one subscription: wait up to timeout_ms (negative = forever)
 * then drain. Returns the count read (>= 1), 0 on timeout, or -1 with errno on
 * error, including errno == ESHUTDOWN if the event was closed. Raw
 * poll/epoll/select on the fd remain available.
 */
static inline int event_wait(int sub, int timeout_ms)
{
	struct pollfd p = { .fd = sub, .events = POLLIN };
	uint64_t count;
	int r = poll(&p, 1, timeout_ms);

	if (r < 0)
		return -1;
	if (r == 0)
		return 0; /* timed out */
	switch (event_read(sub, &count)) {
	case 1:
		return (int)count;
	case 0:
		errno = ESHUTDOWN; /* event closed */
		return -1;
	default:
		return -1;
	}
}

/*
 * Wait for the first k of the n subscription fds to become ready
 * (1 <= k <= n <= EVENT_WAIT_MAX). Drains and writes the fds of those k to
 * ready[] (capacity >= k) in the order observed, and returns how many it wrote:
 * k on success, fewer on timeout, or -1 with errno (EINVAL for a bad n or k).
 * It stops at k, so any extra subscriptions ready in the same poll pass stay
 * readable and surface on the next call (no signal is lost). It reports *which*
 * fds fired, not how many times each did: the per-event counts are consumed and
 * dropped, so call event_read yourself if you need them.
 *
 * If a signal interrupts the underlying poll, the count collected so far is
 * returned (already-drained signals are never thrown away); only when nothing
 * has been collected yet does it return -1 with errno == EINTR. Built on
 * poll(); for large n or to mix with non-event fds, run your own epoll with the
 * same accumulate-until-k loop.
 */
static inline int event_wait_first(const int *subs, int n, int k,
				   int timeout_ms, int *ready)
{
	int nready = 0, i;

	if (k < 1 || k > n || n > EVENT_WAIT_MAX) {
		errno = EINVAL;
		return -1;
	}

	/* n is now validated (1 <= k <= n <= EVENT_WAIT_MAX): safe to size the
	 * per-call arrays by it. */
	struct pollfd p[n];
	char seen[n];

	for (i = 0; i < n; i++) {
		p[i].fd = subs[i];
		p[i].events = POLLIN;
		seen[i] = 0;
	}

	while (nready < k) {
		int r = poll(p, n, timeout_ms);

		if (r < 0)
			return errno == EINTR && nready ? nready : -1;
		if (r == 0)
			return nready; /* timed out: however many we have */
		for (i = 0; i < n && nready < k; i++) {
			uint64_t count;

			/* POLLHUP (a closed event) counts as ready too, so a
			 * dead subscription does not stall the wait. */
			if (seen[i] || !(p[i].revents & (POLLIN | POLLHUP)))
				continue;
			if (event_read(subs[i], &count) < 0) /* -1 only on error */
				return -1;
			seen[i] = 1;
			ready[nready++] = subs[i];
		}
	}
	return nready;
}

/*
 * Wait for any one of the n subscription fds to become ready; consume it and
 * return its fd (>= 0). On timeout returns -1 with errno == ETIMEDOUT; on other
 * errors returns -1 with errno set. The k == 1 case of event_wait_first.
 */
static inline int event_wait_any(const int *subs, int n, int timeout_ms)
{
	int fd, r = event_wait_first(subs, n, 1, timeout_ms, &fd);

	if (r == 1)
		return fd;
	if (r == 0)
		errno = ETIMEDOUT;
	return -1;
}

/*
 * Wait for all n subscription fds to become ready (the k == n case). Returns n
 * when all are ready, fewer on timeout, or -1 with errno. No ready[] is needed:
 * on success every one of subs fired.
 */
static inline int event_wait_all(const int *subs, int n, int timeout_ms)
{
	if (n < 1 || n > EVENT_WAIT_MAX) {
		errno = EINVAL;
		return -1;
	}
	int ready[n];

	return event_wait_first(subs, n, n, timeout_ms, ready);
}

/* Destroy a subscription (it auto-detaches from its event). */
static inline int close_subscription(int sub)
{
	return close(sub);
}

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
