/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only userspace API for event.ko.
 *
 *     int evt = create_event();        // a signalable event (an fd)
 *     int w   = create_waiter();       // a set of events to block on (an fd)
 *     waiter_add(w, evt);
 *
 *     int ready[1];
 *     while (waiter_wait(w, ready, 1, EVT_WAIT_FOREVER) >= 0)
 *             do_work();               // ready[0..n) are the fds that fired;
 *                                      // signals during the work are caught
 *                                      // by the next wait, never lost
 *
 *     signal_event(evt);               // publisher: wake all listeners
 *
 * Events and waiters are fds: fork() shares them, close_*() destroys them. An
 * event is ready for a waiter while its generation is ahead of the one that
 * waiter last consumed; a reported event is consumed, so a wait/work/re-arm
 * loop never misses a signal and a burst coalesces into one report. The full
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
#define WAITER_DEVICE "/dev/waiter"

struct event_wait {
	uint64_t ready_fds; /* user ptr to want int slots (out) */
	uint32_t want;	    /* threshold and output capacity */
	uint32_t _pad;
	int64_t timeout_ms; /* < 0 forever, 0 poll */
};

#define EVENT_IOC_MAGIC 'E'
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)
#define EVENT_IOC_ADD _IO(EVENT_IOC_MAGIC, 3)
#define EVENT_IOC_DEL _IO(EVENT_IOC_MAGIC, 4)
#define EVENT_IOC_WAIT _IOWR(EVENT_IOC_MAGIC, 5, struct event_wait)

#define EVT_WAIT_FOREVER (-1) /* never time out */
#define EVT_WAIT_ZERO 0	      /* poll: report only what is already ready */

/* Allocate a new event; returns an fd, or -1 with errno set. */
static inline int create_event(void)
{
	return open(EVENT_DEVICE, O_RDWR);
}

/* Wake every listener and advance the generation; returns the number of
 * listeners notified, or -1 with errno set. */
static inline int signal_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SIGNAL);
}

/* Destroy the event once the last reference to it is closed. */
static inline int close_event(int evt)
{
	return close(evt);
}

/* Allocate a new waiter; returns an fd, or -1 with errno set. */
static inline int create_waiter(void)
{
	return open(WAITER_DEVICE, O_RDWR);
}

/* Start / stop listening on evt. Returns 0, or -1 with errno (EEXIST if evt is
 * already in the set, ENOENT if it is not). */
static inline int waiter_add(int w, int evt)
{
	return ioctl(w, EVENT_IOC_ADD, evt);
}

static inline int waiter_remove(int w, int evt)
{
	return ioctl(w, EVENT_IOC_DEL, evt);
}

/*
 * Block, consuming no CPU, until at least want of the waiter's events are
 * ready, or until timeout_ms expires, or (immediately) if the waiter listens
 * on fewer than want events. Fills ready_fds with the fds that fired and
 * returns how many (0..want); a short count means the timeout expired first.
 * Returns -1 with errno set on error (EINTR if interrupted by a POSIX signal).
 */
static inline int waiter_wait(int w, int *ready_fds, size_t want,
			      int64_t timeout_ms)
{
	struct event_wait req = {
		.ready_fds = (uint64_t)(uintptr_t)ready_fds,
		.want = (uint32_t)want,
		.timeout_ms = timeout_ms,
	};

	return ioctl(w, EVENT_IOC_WAIT, &req);
}

/* Destroy the waiter once the last reference to it is closed. */
static inline int close_waiter(int w)
{
	return close(w);
}

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
