/* SPDX-License-Identifier: MIT */
/*
 * event_uapi.h - ioctl ABI shared between event.ko and userspace.
 * ../lib/event.h redefines these to stay standalone; keep the two in sync.
 *
 * Two objects. An event (/dev/event) is a generation counter (0 at creation)
 * plus a persistent list of listeners; EVENT_IOC_SIGNAL bumps the generation
 * and wakes every listener without removing it. A waiter (/dev/waiter) holds
 * a set of events (EVENT_IOC_ADD / EVENT_IOC_DEL) and blocks in
 * EVENT_IOC_WAIT until at least ->want of them are ready, where an event is
 * ready while its generation is ahead of the generation this waiter last
 * consumed for it. A reported event is consumed (its generation marked seen),
 * so a wait/work/re-arm loop never misses a signal and a burst coalesces.
 */
#ifndef EVENT_UAPI_H
#define EVENT_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* in: ->want ready fds requested (also the capacity of ->ready_fds);
 * out: ->ready_fds[0..ret) hold the fds that fired, ret is the count. */
struct event_wait {
	__aligned_u64 ready_fds;  /* user ptr to ->want __s32 slots (out) */
	__u32 want;		  /* threshold and output capacity */
	__u32 _pad;
	__s64 timeout_ms;	  /* < 0 = forever, 0 = poll */
};

#define EVENT_WAIT_MAX 1024	  /* upper bound on ->want */

#define EVENT_IOC_MAGIC 'E'

/* On an event fd: wake every listener and bump the generation; returns the
 * number of listeners notified. */
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

/* On a waiter fd: add / remove an event by fd (arg = the event fd). */
#define EVENT_IOC_ADD _IO(EVENT_IOC_MAGIC, 3)
#define EVENT_IOC_DEL _IO(EVENT_IOC_MAGIC, 4)

/* On a waiter fd: block per struct event_wait; returns the number of ready
 * fds written (0..want), or -1 with errno. */
#define EVENT_IOC_WAIT _IOWR(EVENT_IOC_MAGIC, 5, struct event_wait)

#endif /* EVENT_UAPI_H */
