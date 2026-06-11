/* SPDX-License-Identifier: MIT */
/*
 * event_uapi.h - the ioctl ABI shared between event.ko and userspace.
 *
 * The userspace library (../lib/event.h) defines the *same* constants so it can
 * stay standalone/header-only. If you change the magic or command numbers here,
 * change them there too -- the two must match exactly.
 */
#ifndef EVENT_UAPI_H
#define EVENT_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define EVENT_IOC_MAGIC 'E'

/* Block the calling thread until the event is signaled. Edge-triggered: a
 * signal fired before this call is not seen (see EVENT_IOC_WAIT_GEN). */
#define EVENT_IOC_WAIT _IO(EVENT_IOC_MAGIC, 1)

/* Wake every thread currently waiting on the event. */
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

/*
 * Generation-aware wait: arg is a __u64 * holding the last signal generation
 * this caller observed (0 = never; the counter starts at 0 and each
 * signal_event() increments it). If the event has been signaled since, the
 * call returns 0 immediately; otherwise it blocks until the next signal.
 * Either way the current generation is written back through arg, ready for
 * the next call -- so a caller that loops on this can never miss a signal,
 * no matter how late it re-arms.
 */
#define EVENT_IOC_WAIT_GEN _IOWR(EVENT_IOC_MAGIC, 3, __u64)

#endif /* EVENT_UAPI_H */
