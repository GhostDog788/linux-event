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
 * Each listener SUBSCRIBEs for its own subscription fd and consumed generation,
 * so all of them see every signal and bursts coalesce. The wait helpers below
 * do k-of-n in userspace over the subscription fds. Keep the ABI in sync with
 * ../module/event_uapi.h.
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
 * Consume, never blocks: set *count to the signals since the last read (0 if
 * none) and advance the cursor. Returns 1 alive / 0 dead / -1 error (table above).
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
 * Convenience for one subscription: poll up to timeout_ms (negative = forever)
 * then drain. Returns count / 0 timeout / -1 errno (ESHUTDOWN = closed); raw
 * poll/epoll/select on the fd also work.
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
 * Wait for the first k of n subscription fds to become ready
 * (1 <= k <= n <= EVENT_WAIT_MAX, else EINVAL). Drains those k and writes their
 * fds to ready[] (capacity >= k); returns the count written (k, or fewer on
 * timeout). It stops at k, so extras ready in the same pass surface on the next
 * call (nothing lost), and it reports which fds fired, not the per-fd counts
 * (those are consumed; use event_read if you need them). A signal-interrupted
 * poll returns what it has, or -1/EINTR if nothing yet. For large n or to mix
 * with non-event fds, run your own epoll with this accumulate-until-k loop.
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
