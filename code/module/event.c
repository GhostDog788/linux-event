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
 * LOCK-FREE DESIGN -- this iteration takes no lock anywhere; every shared
 * access is a single atomic operation. This is the original paper design's
 * intent (an atomic_cmpxchg'd subscriber list) done correctly:
 *
 *  - Registration is a Treiber push: one cmpxchg() swings the list head to
 *    the new node. Pushing at the head (not appending at the tail, as the
 *    paper sketched) is what makes a single cmpxchg sufficient.
 *
 *  - signal_event() claims the ENTIRE list with one xchg(head, NULL) and
 *    then owns every claimed node outright -- no other signaler, and no new
 *    waiter, can reach them. Take-all claiming is also what makes the
 *    push-only cmpxchg ABA-safe: a recycled node address can only reappear
 *    at the head by being legitimately pushed again.
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
 *    heap-allocated (kmem_cache) rather than living on the waiter's stack
 *    as the locked iteration had: an interrupted waiter must be able to
 *    leave while its node is still reachable.
 *
 *  - Each node pins its task with get_task_struct(), and the signaler takes
 *    a temporary reference of its own before publishing EV_SIGNALED, so
 *    wake_up_process() can never touch a task that already exited -- even
 *    if the woken waiter returns, closes the fd, and dies in the window
 *    between the state change and the wake.
 *
 * See ../README.md for the full walk-through of the wait/signal logic.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>

#include "event_uapi.h"

/* Who owns (and frees) a subscriber node -- see the state diagram above. */
enum subscriber_state {
	EV_WAITING,   /* on the list, waiter parked; owner: pending */
	EV_SIGNALED,  /* signaler woke (or will wake) the waiter; waiter frees */
	EV_CANCELLED, /* waiter left early (signal); claimer/release frees */
};

/*
 * One entry per subscriber. Allocated from a slab cache per wait (a wait is
 * a sleep, so one allocation is noise next to the context switch), because
 * an interrupted waiter abandons its node in place -- a stack node could not
 * outlive its owner. ->task holds a reference, dropped by whoever frees the
 * node, so ->task can be dereferenced for as long as the node exists.
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

static void subscriber_free(struct subscriber *sub)
{
	put_task_struct(sub->task);
	kmem_cache_free(subscriber_cache, sub);
}

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
		subscriber_free(sub);
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
	sub->task = get_task_struct(current);
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
		subscriber_free(sub);

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
 * node. The temporary task reference covers the wake itself: the node's own
 * reference dies with the node, which a woken waiter may free before
 * wake_up_process() has run. Taking it before the xchg is safe -- in either
 * reachable state (EV_WAITING, or EV_CANCELLED whose freer is this very
 * walk) the node, and thus its task reference, is still alive.
 */
static int signal_event(struct event *evt)
{
	struct subscriber *sub, *next;
	int woken = 0;

	sub = xchg(&evt->head, NULL);
	while (sub) {
		struct task_struct *task = sub->task;

		next = sub->next;
		get_task_struct(task);

		if (xchg(&sub->state, EV_SIGNALED) == EV_WAITING) {
			wake_up_process(task);
			woken++;
			/* the waiter frees sub; it is dead to us */
		} else {
			/* EV_CANCELLED: abandoned by an interrupted waiter */
			subscriber_free(sub);
		}

		put_task_struct(task);
		sub = next;
	}

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
					     sizeof(struct subscriber), 0, 0,
					     NULL);
	if (!subscriber_cache)
		return -ENOMEM;

	ret = misc_register(&event_misc);
	if (ret) {
		pr_err("event: misc_register failed: %d\n", ret);
		kmem_cache_destroy(subscriber_cache);
		return ret;
	}

	pr_info("event: loaded (lock-free), device at /dev/event\n");
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
MODULE_VERSION("2.0");
