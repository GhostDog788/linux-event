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
 * listener waits on k-of-n events with its own epoll (see event_wait_quorum,
 * which the kernel leaves to userspace). The ABI below must stay in sync with
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

/* ---- listener ---- */

/* Create a subscription onto evt; returns a pollable fd, or -1 with errno. */
static inline int subscribe_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SUBSCRIBE);
}

/*
 * Read the number of signals since the last read into *count (clearing
 * readiness). Returns 0 on success, or -1 with errno set: EAGAIN if the
 * subscription is O_NONBLOCK and nothing is pending, EINTR if interrupted. A
 * read of 0 (count set to 0) means the event was closed (hangup).
 */
static inline int event_read(int sub, uint64_t *count)
{
	uint64_t c;
	ssize_t r = read(sub, &c, sizeof(c));

	if (r == 0) {
		*count = 0;
		return 0; /* event closed */
	}
	if (r != (ssize_t)sizeof(c))
		return -1;
	*count = c;
	return 0;
}

/* Make a subscription non-blocking (so event_read returns EAGAIN when empty). */
static inline int subscription_set_nonblock(int sub)
{
	int fl = fcntl(sub, F_GETFL);

	return fl < 0 ? -1 : fcntl(sub, F_SETFL, fl | O_NONBLOCK);
}

/*
 * Convenience for one subscription: wait up to timeout_ms (negative = forever)
 * then drain. Returns the count read (>= 1), 0 on timeout, -1 with errno. Raw
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
	if (event_read(sub, &count) < 0)
		return -1;
	return (int)count;
}

/*
 * Wait until at least k of the n subscription fds are ready (k <= n; k == n
 * waits for all). Drains the ready ones, writes their fds to ready[] (capacity
 * >= n) in the order observed, and returns how many are ready (>= k), 0 on
 * timeout, or -1 with errno. Built on poll(); for large n or to mix with
 * non-event fds, run your own epoll with the same accumulate-until-k loop.
 */
static inline int event_wait_quorum(const int *subs, int n, int k,
				    int timeout_ms, int *ready)
{
	struct pollfd p[n];
	char seen[n];
	int have = 0, nready = 0, i;

	if (k < 1 || k > n)
		return -1;
	for (i = 0; i < n; i++) {
		p[i].fd = subs[i];
		p[i].events = POLLIN;
		seen[i] = 0;
	}

	while (have < k) {
		int r = poll(p, n, timeout_ms);

		if (r < 0)
			return errno == EINTR ? (have ? nready : -1) : -1;
		if (r == 0)
			return nready; /* timed out: however many we have */
		for (i = 0; i < n; i++) {
			uint64_t count;

			if (seen[i] || !(p[i].revents & POLLIN))
				continue;
			if (event_read(subs[i], &count) < 0)
				return -1;
			seen[i] = 1;
			ready[nready++] = subs[i];
			have++;
		}
	}
	return nready;
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
