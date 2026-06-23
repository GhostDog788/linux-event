/* SPDX-License-Identifier: MIT */
/*
 * event_uapi.h - the /dev/event ioctl ABI. ../lib/event.h redefines these to
 * stay standalone; keep the two in sync (code/test/abi-check enforces it).
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
