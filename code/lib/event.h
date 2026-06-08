/* SPDX-License-Identifier: MIT */
/*
 * event.h - header-only userspace API for the event.ko synchronization object.
 *
 * Include this header and link nothing: every entry point is a `static inline`
 * wrapper around open()/ioctl()/close() on the /dev/event character device that
 * event.ko exposes.
 *
 *     #include "event.h"
 *
 *     int evt = create_event();        // a fresh event object (an fd)
 *     ...
 *     wait_for_event(evt);             // subscriber: block until signaled
 *     ...
 *     signal_event(evt);               // publisher: wake all subscribers
 *     ...
 *     close_event(evt);                // destroy the event
 *
 * Because an event *is* an open fd, fork() shares it: open the event in the
 * parent, fork your listeners, and parent + children all reference the same
 * kernel object. That is exactly what ../example/ demonstrates.
 *
 * The ioctl ABI below must stay in sync with ../module/event_uapi.h.
 */
#ifndef EVENT_H
#define EVENT_H

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_DEVICE "/dev/event"

#define EVENT_IOC_MAGIC 'E'
#define EVENT_IOC_WAIT _IO(EVENT_IOC_MAGIC, 1)
#define EVENT_IOC_SIGNAL _IO(EVENT_IOC_MAGIC, 2)

/*
 * create_event(): allocate a new event object in the kernel.
 *
 * Returns an fd referring to the event, or -1 with errno set (the usual open()
 * failure modes; ENOENT/EACCES if event.ko is not loaded or /dev/event is not
 * accessible).
 */
static inline int create_event(void)
{
	return open(EVENT_DEVICE, O_RDWR);
}

/*
 * wait_for_event(): register the calling thread as a subscriber and block,
 * consuming no CPU, until the event is signaled.
 *
 * Returns 0 on a normal wake, or -1 with errno set (EINTR if a signal
 * interrupted the wait).
 */
static inline int wait_for_event(int evt)
{
	return ioctl(evt, EVENT_IOC_WAIT);
}

/*
 * signal_event(): wake every thread currently waiting on the event.
 *
 * Returns the number of threads woken (>= 0), or -1 with errno set.
 */
static inline int signal_event(int evt)
{
	return ioctl(evt, EVENT_IOC_SIGNAL);
}

/*
 * close_event(): destroy the event once the last reference is closed.
 *
 * Returns 0 on success, or -1 with errno set.
 */
static inline int close_event(int evt)
{
	return close(evt);
}

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
