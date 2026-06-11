/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only userspace API for event.ko (/dev/event).
 *
 *     int evt = create_event();
 *     uint64_t gen = 0;
 *     while (wait_for_event(evt, &gen, EVT_WAIT_FOREVER) == EVT_SIGNALED)
 *             do_work();          // signals fired during the work are
 *                                 // caught by the next wait, never lost
 *
 *     signal_event(evt);          // publisher: wake all subscribers
 *
 * An event is an fd: fork() shares it, close_event() destroys it. The full
 * design lives in ../README.md. The ABI below must stay in sync with
 * ../module/event_uapi.h.
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

struct event_wait {
	uint64_t gen;
	int64_t timeout_ms;
};

#define EVENT_IOC_MAGIC 'E'
#define EVENT_IOC_WAIT _IOWR(EVENT_IOC_MAGIC, 1, struct event_wait)
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

#define EVT_WAIT_FOREVER (-1) /* never time out */
#define EVT_WAIT_ZERO 0	      /* poll: EVT_TIMEOUT unless already signaled */
#define EVT_SIGNALED 0
#define EVT_TIMEOUT 1

/* Allocate a new event object; returns an fd, or -1 with errno set. */
static inline int create_event(void)
{
	return open(EVENT_DEVICE, O_RDWR);
}

/*
 * Block, consuming no CPU, until the event is signaled past *gen --
 * immediately if it already has been, or until timeout_ms expires.
 * Start with *gen = 0; it is updated on EVT_SIGNALED, so a wait/work/re-arm
 * loop observes every signal no matter how late it re-arms.
 *
 * Returns EVT_SIGNALED, EVT_TIMEOUT, or -1 with errno set (e.g. EINTR).
 */
static inline int wait_for_event(int evt, uint64_t *gen, int64_t timeout_ms)
{
	struct event_wait w = { .gen = *gen, .timeout_ms = timeout_ms };

	if (ioctl(evt, EVENT_IOC_WAIT, &w) == 0) {
		*gen = w.gen;
		return EVT_SIGNALED;
	}
	return errno == ETIMEDOUT ? EVT_TIMEOUT : -1;
}

/* Wake every waiter and advance the generation; returns the number woken,
 * or -1 with errno set. */
static inline int signal_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SIGNAL);
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
