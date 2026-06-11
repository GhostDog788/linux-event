// SPDX-License-Identifier: MIT
/*
 * event.ko - a minimal "event" synchronization object for Linux.
 *
 * An event lets one or more subscriber threads block (consuming zero CPU,
 * with zero polling latency) until a publisher thread signals the event.
 *
 * Userspace contract (see ../lib/event.h):
 *
 *     int fd = open("/dev/event", O_RDWR);   // create_event(): a fresh event
 *     ioctl(fd, EVENT_IOC_WAIT);             // wait_for_event(): block here
 *     ioctl(fd, EVENT_IOC_SIGNAL);           // signal_event(): wake all waiters
 *     close(fd);                             // destroy the event
 *
 * Each open() of /dev/event allocates an independent event object, stored in
 * file->private_data. Because fork() shares the parent's open file table,
 * children inherit the *same* event object across a fork -- which is exactly
 * how the example wires up many listeners and one sender.
 *
 * LOCK-FREE DESIGN (iteration 3) -- no lock anywhere; every shared access is
 * a single atomic operation:
 *
 *  - Registration is a Treiber push: one cmpxchg() swings the list head to
 *    the new node.
 *
 *  - signal_event() claims the ENTIRE list with one xchg(head, NULL) and
 *    then owns every claimed node outright -- no other signaler, and no new
 *    waiter, can reach them. Take-all claiming is also what makes the
 *    push-only cmpxchg ABA-safe: a recycled node address can only reappear
 *    at the head by being legitimately pushed again. An empty event is
 *    detected with a plain read first, so the (common) signal-with-no-
 *    subscribers case never dirties the shared cacheline.
 *
 *  - Lifetime is a three-state handoff instead of a lock. Each node holds
 *    an atomic state, and the single xchg()/cmpxchg() that moves it out of
 *    EV_WAITING decides who frees the node:
 *
 *        EV_WAITING --xchg by signaler--> EV_SIGNALED   waiter frees it
 *        EV_WAITING --cmpxchg by waiter-> EV_CANCELLED  waiter abandons it;
 *                                                       the next signal (or
 *                                                       release) frees it
 *
 *    A cancelled node cannot be unlinked from the middle of a singly-linked
 *    list without a lock, so it is left in place ("deferred reclamation"):
 *    at most one allocation per interrupted wait lingers until the next
 *    signal_event() or the event's destruction. This is also why nodes are
 *    heap-allocated (a cache-aligned slab) rather than living on the
 *    waiter's stack: an interrupted waiter must be able to leave while its
 *    node is still reachable.
 *
 *  - The wake itself is made safe by RCU, not refcounting (this iteration's
 *    main change -- the previous one paid a get/put_task_struct pair per
 *    node, which dominated large fan-outs). A node still EV_WAITING at the
 *    xchg() means its waiter was inside wait_for_event() at that instant:
 *    it cannot return (and so cannot exit) before observing EV_SIGNALED,
 *    which only we publish. Its release_task() -- after which the
 *    task_struct is freed one RCU grace period later -- therefore happens
 *    after the xchg(), i.e. inside our read-side section, so the grace
 *    period cannot complete until we leave it: wake_up_process() never
 *    touches freed memory. (This is the same argument the kernel's rcuwait
 *    relies on.)
 *
 * See ../README.md for the full walk-through of the wait/signal logic.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include "event_uapi.h"

/* Who owns (and frees) a subscriber node -- see the state diagram above. */
enum subscriber_state {
	EV_WAITING,   /* on the list, waiter parked; owner: pending */
	EV_SIGNALED,  /* signaler woke (or will wake) the waiter; waiter frees */
	EV_CANCELLED, /* waiter left early (signal); claimer/release frees */
};

/*
 * One entry per subscriber, allocated per wait from a cache-aligned slab
 * (one allocation is noise next to the context switch a wait implies, and
 * the alignment keeps two waiters' nodes from false-sharing a cacheline).
 * ->task is NOT reference-counted: it is only ever dereferenced under RCU
 * by a signaler that proved the waiter still parked -- see above.
 */
struct subscriber {
	struct subscriber *next;
	struct task_struct *task;
	unsigned int state;
};

struct event {
	struct subscriber *head; /* lock-free LIFO; cmpxchg/xchg only */
};

static struct kmem_cache *subscriber_cache;

/* create_event(): each open() hands out a brand-new event object. */
static int event_open(struct inode *inode, struct file *file)
{
	struct event *evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	file->private_data = evt;
	return 0;
}

/*
 * Destroy the event once the last fd referencing it is closed.
 *
 * No waiter can still be inside wait_for_event() here: every waiter (and
 * every signaler) holds a reference on the file across its ioctl, so release
 * only runs after they have all returned. Whatever is left on the list is
 * therefore an abandoned EV_CANCELLED node from an interrupted wait -- free
 * them. Seeing EV_WAITING here would mean a waiter left without resolving
 * its node; that is a bug worth screaming about.
 */
static int event_release(struct inode *inode, struct file *file)
{
	struct event *evt = file->private_data;
	struct subscriber *sub, *next;

	sub = xchg(&evt->head, NULL);
	while (sub) {
		next = sub->next;
		WARN_ON_ONCE(READ_ONCE(sub->state) == EV_WAITING);
		kmem_cache_free(subscriber_cache, sub);
		sub = next;
	}
	kfree(evt);
	return 0;
}

/*
 * wait_for_event(): register the calling thread and block until the event is
 * signaled (or a signal interrupts the wait).
 *
 * The lost-wakeup race is closed the classic way: the task state is set to
 * TASK_INTERRUPTIBLE *before* re-checking ->state, so a signaler either sees
 * us parked (and wakes us) or we see EV_SIGNALED (and skip the sleep).
 * set_current_state() is a full barrier; the acquire load below pairs with
 * the signaler's fully-ordered xchg() of ->state.
 */
static int wait_for_event(struct event *evt)
{
	struct subscriber *sub;
	int ret = 0;

	sub = kmem_cache_alloc(subscriber_cache, GFP_KERNEL);
	if (!sub)
		return -ENOMEM;
	sub->task = current;
	sub->state = EV_WAITING;

	/*
	 * Treiber push. cmpxchg() is fully ordered, so a signaler that claims
	 * the list and finds this node is guaranteed to see the fields set
	 * above. On failure (another waiter pushed first) just re-read and
	 * retry; there is nothing to clean up.
	 */
	do {
		sub->next = READ_ONCE(evt->head);
	} while (cmpxchg(&evt->head, sub->next, sub) != sub->next);

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);

		if (smp_load_acquire(&sub->state) == EV_SIGNALED)
			break;

		if (signal_pending(current)) {
			/*
			 * Try to take the node back from the signalers. If we
			 * win, the node stays on the list as EV_CANCELLED for
			 * a later signal/release to free -- we must not free
			 * it ourselves, it is still linked. If we lose, a
			 * signaler is already waking us: report success.
			 */
			if (cmpxchg(&sub->state, EV_WAITING, EV_CANCELLED) ==
			    EV_WAITING) {
				ret = -ERESTARTSYS;
				sub = NULL; /* abandoned */
			}
			break;
		}

		schedule();
	}
	__set_current_state(TASK_RUNNING);

	/* EV_SIGNALED: the signaler is done with the node; we own it. */
	if (sub)
		kmem_cache_free(subscriber_cache, sub);

	return ret;
}

/*
 * signal_event(): wake every currently-registered subscriber.
 *
 * One xchg() detaches the whole subscriber list; from that point every node
 * on it belongs to this call alone (concurrent signalers get disjoint
 * chains, new waiters push onto the fresh empty list and wait for the next
 * signal -- the event stays edge-triggered).
 *
 * Per node, ->next and ->task are read *before* publishing EV_SIGNALED,
 * because the instant the waiter can observe that state it may free the
 * node. One RCU read-side section spanning the walk is what makes the wakes
 * themselves safe without any refcounting -- see the lifetime notes at the
 * top of the file. ->task is only dereferenced when the xchg() proved the
 * waiter was still parked; a cancelled node's stale pointer is never used.
 */
static int signal_event(struct event *evt)
{
	struct subscriber *sub, *next;
	int woken = 0;

	/*
	 * Common fast path: nobody is waiting. A plain read keeps the
	 * cacheline shared between concurrent signalers (an xchg would
	 * dirty it even when there is nothing to take). Returning 0 against
	 * a concurrently-racing registration is linearizable: that waiter
	 * simply registered "after" this signal.
	 */
	if (!READ_ONCE(evt->head))
		return 0;

	sub = xchg(&evt->head, NULL);

	rcu_read_lock();
	while (sub) {
		struct task_struct *task = sub->task;

		next = sub->next;

		if (xchg(&sub->state, EV_SIGNALED) == EV_WAITING) {
			wake_up_process(task);
			woken++;
			/* the waiter frees sub; it is dead to us */
		} else {
			/* EV_CANCELLED: abandoned by an interrupted waiter */
			kmem_cache_free(subscriber_cache, sub);
		}

		sub = next;
	}
	rcu_read_unlock();

	return woken;
}

static long event_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct event *evt = file->private_data;

	switch (cmd) {
	case EVENT_IOC_WAIT:
		return wait_for_event(evt);
	case EVENT_IOC_SIGNAL:
		return signal_event(evt);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations event_fops = {
	.owner = THIS_MODULE,
	.open = event_open,
	.release = event_release,
	.unlocked_ioctl = event_ioctl,
	.compat_ioctl = event_ioctl,
};

static struct miscdevice event_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "event",
	.fops = &event_fops,
	.mode = 0666, /* world rw so the example can run without root */
};

static int __init event_init(void)
{
	int ret;

	subscriber_cache = kmem_cache_create("event_subscriber",
					     sizeof(struct subscriber), 0,
					     SLAB_HWCACHE_ALIGN, NULL);
	if (!subscriber_cache)
		return -ENOMEM;

	ret = misc_register(&event_misc);
	if (ret) {
		pr_err("event: misc_register failed: %d\n", ret);
		kmem_cache_destroy(subscriber_cache);
		return ret;
	}

	pr_info("event: loaded (lock-free v3), device at /dev/event\n");
	return 0;
}

static void __exit event_exit(void)
{
	misc_deregister(&event_misc);
	kmem_cache_destroy(subscriber_cache);
	pr_info("event: unloaded\n");
}

module_init(event_init);
module_exit(event_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("dor");
MODULE_DESCRIPTION("Minimal event synchronization object (/dev/event), lock-free");
MODULE_VERSION("3.0");
