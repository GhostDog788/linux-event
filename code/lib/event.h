/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only userspace API for the event.ko synchronization object.
 *
 * Include this header and link nothing: every entry point is a `static inline`
 * wrapper around open()/ioctl()/close() on the /dev/event character device that
 * event.ko exposes.
 *
 *     #include "event.h"
 *
 *     int evt = create_event();        // a fresh event object (an fd)
 *     ...
 *     wait_for_event(evt);             // subscriber: block until signaled
 *     ...
 *     signal_event(evt);               // publisher: wake all subscribers
 *     ...
 *     close_event(evt);                // destroy the event
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

#define EVENT_IOC_MAGIC 'E'
#define EVENT_IOC_WAIT _IO(EVENT_IOC_MAGIC, 1)
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)
#define EVENT_IOC_WAIT_GEN _IOWR(EVENT_IOC_MAGIC, 3, uint64_t)

/* Must match struct event_wait in ../module/event_uapi.h exactly. */
struct event_wait {
	uint64_t gen;
	int64_t timeout_ms;
	uint32_t flags;
	uint32_t reserved;
};

#define EVENT_WAIT_FL_GEN 0x1u

#define EVENT_IOC_WAIT_EX _IOWR(EVENT_IOC_MAGIC, 4, struct event_wait)

/* Timeout values and results for the *_timeout waits (see the design doc). */
#define EVT_WAIT_FOREVER (-1)
#define EVT_WAIT_ZERO 0
#define EVT_SIGNALED 0
#define EVT_TIMEOUT 1

/*
 * create_event(): allocate a new event object in the kernel.
 *
 * Returns an fd referring to the event, or -1 with errno set (the usual open()
 * failure modes; ENOENT/EACCES if event.ko is not loaded or /dev/event is not
 * accessible).
 */
static inline int create_event(void)
{
	return open(EVENT_DEVICE, O_RDWR);
}

/*
 * wait_for_event(): register the calling thread as a subscriber and block,
 * consuming no CPU, until the event is signaled.
 *
 * Returns 0 on a normal wake, or -1 with errno set (EINTR if a signal
 * interrupted the wait).
 */
static inline int wait_for_event(int evt)
{
	return ioctl(evt, EVENT_IOC_WAIT);
}

/*
 * wait_for_event_gen(): like wait_for_event(), but immune to missed signals.
 *
 * The plain wait is edge-triggered: a signal that fires while you are off
 * doing work (not yet re-registered) is lost. This variant closes that gap
 * with a generation counter: *gen carries the last signal generation this
 * caller observed -- start with 0 ("never saw a signal"). If the event has
 * been signaled since, the call returns immediately; otherwise it blocks
 * until the next signal. On success *gen is updated, ready for the next
 * call, so a wait/work/re-arm loop sees every signal exactly once (a burst
 * of signals during one stretch of work coalesces into one return -- check
 * how far *gen jumped if you care how many fired).
 *
 * Returns 0 on signal (with *gen updated), or -1 with errno set (EINTR if a
 * signal interrupted the wait).
 */
static inline int wait_for_event_gen(int evt, uint64_t *gen)
{
	return ioctl(evt, EVENT_IOC_WAIT_GEN, gen);
}

/*
 * wait_for_event_timeout(): wait_for_event() bounded in time.
 *
 * @timeout_ms: milliseconds to wait. EVT_WAIT_FOREVER (-1) never times out;
 * EVT_WAIT_ZERO (0) returns immediately (a poll).
 *
 * Returns EVT_SIGNALED when the event fired within the limit, EVT_TIMEOUT
 * when it did not, or -1 with errno set (EINTR if a signal interrupted).
 */
static inline int wait_for_event_timeout(int evt, int64_t timeout_ms)
{
	struct event_wait w = { .timeout_ms = timeout_ms };

	if (ioctl(evt, EVENT_IOC_WAIT_EX, &w) == 0)
		return EVT_SIGNALED;
	return errno == ETIMEDOUT ? EVT_TIMEOUT : -1;
}

/*
 * wait_for_event_gen_timeout(): the generation-aware wait, bounded in time.
 * Combines wait_for_event_gen() (no missed signals across a re-arm loop)
 * with the timeout semantics above. *gen is updated on EVT_SIGNALED.
 */
static inline int wait_for_event_gen_timeout(int evt, uint64_t *gen,
					     int64_t timeout_ms)
{
	struct event_wait w = { .gen = *gen,
				.timeout_ms = timeout_ms,
				.flags = EVENT_WAIT_FL_GEN };

	if (ioctl(evt, EVENT_IOC_WAIT_EX, &w) == 0) {
		*gen = w.gen;
		return EVT_SIGNALED;
	}
	return errno == ETIMEDOUT ? EVT_TIMEOUT : -1;
}

/*
 * signal_event(): wake every thread currently waiting on the event.
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
