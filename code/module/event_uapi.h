/* SPDX-License-Identifier: MIT */
/*
 * event_uapi.h - ioctl ABI shared between event.ko and userspace.
 * ../lib/event.h redefines these to stay standalone; keep the two in sync.
 *
 * An event (/dev/event) is a broadcast source. SUBSCRIBE returns a pollable
 * subscription fd; SIGNAL raises the generation and wakes every subscription.
 * A subscription is read/poll only: read() returns the number of signals since
 * the last read (a u64) and clears readiness.
 */
#ifndef EVENT_UAPI_H
#define EVENT_UAPI_H

#include <linux/ioctl.h>

#define EVENT_IOC_MAGIC 'E'

/* On an event fd: raise the generation and wake every subscription; returns
 * the number notified. */
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

/* On an event fd: create a new subscription; returns its fd. */
#define EVENT_IOC_SUBSCRIBE _IO(EVENT_IOC_MAGIC, 6)

#endif /* EVENT_UAPI_H */
