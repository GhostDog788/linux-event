/* SPDX-License-Identifier: MIT */
/*
 * event_uapi.h - ioctl ABI shared between event.ko and userspace.
 * ../lib/event.h redefines these to stay standalone; keep the two in sync.
 *
 * An event is a pollable file. Wait on it with poll/epoll/select or read():
 * read() returns the number of signals since the last read (as a u64) and
 * clears readiness. The only ioctl is the publisher's signal.
 */
#ifndef EVENT_UAPI_H
#define EVENT_UAPI_H

#include <linux/ioctl.h>

#define EVENT_IOC_MAGIC 'E'

/* Raise the generation and wake every waiter. Returns 0. */
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

#endif /* EVENT_UAPI_H */
