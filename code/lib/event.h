/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only userspace API for event.ko (/dev/event).
 *
 *     int evt = create_event();
 *     ... in a waiter: ...
 *     struct pollfd p = { .fd = evt, .events = POLLIN };
 *     poll(&p, 1, -1);            // or epoll/select; it is a real fd
 *     uint64_t n;
 *     event_read(evt, &n);        // n = signals since last read; clears ready
 *
 *     signal_event(evt);          // publisher: raise the generation, wake all
 *
 * An event is a pollable fd, behaving like eventfd with an ioctl signal in
 * place of write(): the readable count is the number of signals since the last
 * read. fork()/dup share it (and its readiness); a shared fd is single
 * consumer (the first reader drains it), so independent listeners each create
 * their own event. The full design lives in ../README.md. The ABI below must
 * stay in sync with ../module/event_uapi.h.
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

/* Allocate a new event; returns an fd, or -1 with errno set. */
static inline int create_event(void)
{
	return open(EVENT_DEVICE, O_RDWR);
}

/* As create_event(), with extra open flags (e.g. O_NONBLOCK, O_CLOEXEC). */
static inline int create_event_flags(int flags)
{
	return open(EVENT_DEVICE, O_RDWR | flags);
}

/* Raise the generation and wake every waiter; returns 0, or -1 with errno. */
static inline int signal_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SIGNAL);
}

/*
 * Read the number of signals since the last read into *count (clearing
 * readiness). Returns 0 on success, or -1 with errno set: EAGAIN if the fd is
 * O_NONBLOCK and nothing is pending, EINTR if a blocking read was interrupted.
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
