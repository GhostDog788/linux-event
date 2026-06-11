/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only userspace API for the event.ko synchronization object.
 *
 * Include this header and link nothing: every entry point is a `static inline`
 * wrapper around open()/ioctl()/close() on the /dev/event character device
 * that event.ko exposes.
 *
 *     #include "event.h"
 *
 *     int evt = create_event();              // a fresh event object (an fd)
 *     uint64_t gen = 0;                      // "I have seen no signals yet"
 *
 *     // subscriber loop: never misses a signal, no matter how long the
 *     // work takes -- signals fired meanwhile return immediately.
 *     for (;;) {
 *             wait_for_event(evt, &gen);     // block until signaled past gen
 *             do_work();
 *     }
 *
 *     signal_event(evt);                     // publisher: wake all subscribers
 *     close_event(evt);                      // destroy the event
 *
 * Every signal bumps the event's generation counter (from 0 at creation);
 * a wait returns as soon as the event has been signaled past the caller's
 * generation and writes the current one back. A burst of signals during one
 * stretch of work coalesces into one return -- check how far *gen jumped if
 * you care how many fired.
 *
 * Because an event *is* an open fd, fork() shares it: open the event in the
 * parent, fork your listeners, and parent + children all reference the same
 * kernel object. That is exactly what ../example/ demonstrates.
 *
 * The ioctl ABI below must stay in sync with ../module/event_uapi.h.
 */
#ifndef EVENT_H
#define EVENT_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_DEVICE "/dev/event"

/* Must match ../module/event_uapi.h exactly. */
struct event_wait {
	uint64_t gen;
	int64_t timeout_ms;
};

#define EVENT_IOC_MAGIC 'E'
#define EVENT_IOC_WAIT _IOWR(EVENT_IOC_MAGIC, 1, struct event_wait)
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

/* Timeout values and results for wait_for_event_timeout(). */
#define EVT_WAIT_FOREVER (-1)
#define EVT_WAIT_ZERO 0
#define EVT_SIGNALED 0
#define EVT_TIMEOUT 1

/*
 * create_event(): allocate a new event object in the kernel.
 *
 * Returns an fd referring to the event, or -1 with errno set (the usual
 * open() failure modes; ENOENT/EACCES if event.ko is not loaded or
 * /dev/event is not accessible).
 */
static inline int create_event(void)
{
	return open(EVENT_DEVICE, O_RDWR);
}

/*
 * wait_for_event(): block, consuming no CPU, until the event is signaled
 * past *gen -- returning immediately if it already has been. Start with
 * *gen = 0; on success *gen holds the current generation, ready for the
 * next call.
 *
 * Returns 0 on signal (with *gen updated), or -1 with errno set (EINTR if
 * a POSIX signal interrupted the wait).
 */
static inline int wait_for_event(int evt, uint64_t *gen)
{
	struct event_wait w = { .gen = *gen, .timeout_ms = EVT_WAIT_FOREVER };

	if (ioctl(evt, EVENT_IOC_WAIT, &w) != 0)
		return -1;
	*gen = w.gen;
	return 0;
}

/*
 * wait_for_event_timeout(): wait_for_event() bounded in time.
 *
 * @timeout_ms: milliseconds to wait. EVT_WAIT_FOREVER (-1) never times out;
 * EVT_WAIT_ZERO (0) returns immediately (a poll).
 *
 * Returns EVT_SIGNALED when the event fired within the limit (with *gen
 * updated), EVT_TIMEOUT when it did not (*gen untouched), or -1 with errno
 * set (EINTR if a POSIX signal interrupted the wait).
 */
static inline int wait_for_event_timeout(int evt, uint64_t *gen,
					 int64_t timeout_ms)
{
	struct event_wait w = { .gen = *gen, .timeout_ms = timeout_ms };

	if (ioctl(evt, EVENT_IOC_WAIT, &w) == 0) {
		*gen = w.gen;
		return EVT_SIGNALED;
	}
	return errno == ETIMEDOUT ? EVT_TIMEOUT : -1;
}

/*
 * signal_event(): wake every thread currently waiting on the event and
 * advance its generation, so late waiters catch up on their next wait.
 *
 * Returns the number of threads woken (>= 0), or -1 with errno set.
 */
static inline int signal_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SIGNAL);
}

/*
 * close_event(): destroy the event once the last reference is closed.
 *
 * Returns 0 on success, or -1 with errno set.
 */
static inline int close_event(int evt)
{
	return close(evt);
}

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
