/* SPDX-License-Identifier: MIT */
/*
 * event_uapi.h - the ioctl ABI shared between event.ko and userspace.
 *
 * The userspace library (../lib/event.h) defines the *same* constants so it
 * can stay standalone/header-only. If you change anything here, change it
 * there too -- the two must match exactly.
 *
 * The whole ABI is one wait and one signal:
 *
 *  - Every EVENT_IOC_SIGNAL bumps the event's generation counter (starting
 *    from 0 at creation) and wakes every waiter.
 *
 *  - EVENT_IOC_WAIT takes the generation the caller last observed. If the
 *    event has been signaled since, it returns immediately; otherwise it
 *    blocks until the next signal or until timeout_ms expires. The current
 *    generation is written back on success, ready for the next call -- so a
 *    wait/work/re-arm loop observes every signal, no matter how late it
 *    re-arms (a burst during one stretch of work coalesces into one return,
 *    with the generation jumped by the burst size).
 */
#ifndef EVENT_UAPI_H
#define EVENT_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct event_wait {
	__u64 gen;	  /* in: last seen generation; out: current */
	__s64 timeout_ms; /* in: < 0 = wait forever, 0 = poll */
};

#define EVENT_IOC_MAGIC 'E'

/* Block until the event is signaled past ->gen; -ETIMEDOUT on expiry. */
#define EVENT_IOC_WAIT _IOWR(EVENT_IOC_MAGIC, 1, struct event_wait)

/* Wake every thread currently waiting; returns the number woken. */
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

#endif /* EVENT_UAPI_H */
