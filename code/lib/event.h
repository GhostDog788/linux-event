/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only event API, backed entirely by eventfd + epoll.
 *
 * There is no kernel module here: an "event" is just an eventfd, and you wait
 * on it with the kernel's own poll/epoll/select. This branch shows that the
 * pollable event needs no custom object at all; the API below is identical to
 * the module-backed version, so the same demo, tests, and bench run unchanged.
 *
 *     int evt = create_event();
 *     struct pollfd p = { .fd = evt, .events = POLLIN };
 *     poll(&p, 1, -1);            // or epoll/select; it is a real fd
 *     uint64_t n;
 *     event_read(evt, &n);        // n = signals since last read; clears ready
 *
 *     signal_event(evt);          // publisher: add one, wake all
 *
 * An event is an eventfd: a counter you wait on as a file. signal_event() adds
 * one (a write of 1) and read() returns the accumulated count and zeroes it, so
 * a burst coalesces into one read and no signal is lost. fork()/dup share it
 * (and its counter); a shared fd is single consumer (the first reader drains
 * it), so independent listeners each create their own event.
 */
#ifndef EVENT_H
#define EVENT_H

#include <errno.h>
#include <fcntl.h> /* O_NONBLOCK / O_CLOEXEC, == EFD_NONBLOCK / EFD_CLOEXEC */
#include <poll.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate a new event; returns an fd, or -1 with errno set. */
static inline int create_event(void)
{
	return eventfd(0, 0);
}

/*
 * As create_event(), with extra eventfd flags. EFD_NONBLOCK and EFD_CLOEXEC
 * are numerically equal to O_NONBLOCK and O_CLOEXEC, so either spelling works.
 */
static inline int create_event_flags(int flags)
{
	return eventfd(0, flags);
}

/* Raise the count by one and wake every waiter; returns 0, or -1 with errno. */
static inline int signal_event(int evt)
{
	uint64_t one = 1;

	return write(evt, &one, sizeof(one)) == (ssize_t)sizeof(one) ? 0 : -1;
}

/*
 * Read the number of signals since the last read into *count (clearing
 * readiness). Returns 0 on success, or -1 with errno set: EAGAIN if the fd is
 * non-blocking and nothing is pending, EINTR if a blocking read was interrupted.
 */
static inline int event_read(int evt, uint64_t *count)
{
	uint64_t c;
	ssize_t r = read(evt, &c, sizeof(c));

	if (r != (ssize_t)sizeof(c))
		return -1;
	*count = c;
	return 0;
}

/*
 * Convenience for the single-event case: wait up to timeout_ms (negative =
 * forever) then drain. Returns the signal count read (>= 1), 0 on timeout, or
 * -1 with errno. Raw poll/epoll/select on the fd remain available.
 */
static inline int event_wait(int evt, int timeout_ms)
{
	struct pollfd p = { .fd = evt, .events = POLLIN };
	uint64_t count;
	int r = poll(&p, 1, timeout_ms);

	if (r < 0)
		return -1;
	if (r == 0)
		return 0; /* timed out */
	if (event_read(evt, &count) < 0)
		return -1;
	return (int)count;
}

/* Destroy the event once the last reference to it is closed. */
static inline int close_event(int evt)
{
	return close(evt);
}

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
