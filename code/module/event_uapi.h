/* SPDX-License-Identifier: MIT */
/*
 * event_uapi.h - ioctl ABI shared between event.ko and userspace.
 * ../lib/event.h redefines these to stay standalone; keep the two in sync.
 *
 * Signals are counted in a per-event generation (0 at creation). A wait
 * returns once the event is signaled past ->gen, immediately if it
 * already has been, and writes the current generation back, so a
 * wait/work/re-arm loop never misses a signal.
 */
#ifndef EVENT_UAPI_H
#define EVENT_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct event_wait {
	__u64 gen;	  /* in: last seen generation; out: current */
	__s64 timeout_ms; /* < 0 = forever, 0 = poll; -ETIMEDOUT on expiry */
};

#define EVENT_IOC_MAGIC 'E'

#define EVENT_IOC_WAIT _IOWR(EVENT_IOC_MAGIC, 1, struct event_wait)

/* Wake every waiter and bump the generation; returns the number woken. */
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

#endif /* EVENT_UAPI_H */
