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

#define EVENT_IOC_MAGIC 'E'

/* Block the calling thread until the event is signaled. */
#define EVENT_IOC_WAIT _IO(EVENT_IOC_MAGIC, 1)

/* Wake every thread currently waiting on the event. */
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

#endif /* EVENT_UAPI_H */
