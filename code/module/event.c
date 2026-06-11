// SPDX-License-Identifier: MIT
/*
 * event.ko - a minimal "event" synchronization object for Linux.
 *
 * An event lets any number of subscriber threads block (consuming zero CPU,
 * with zero polling latency) until a publisher thread signals the event.
 * Each open() of /dev/event creates an independent event; fork() shares it
 * the way it shares any fd. The ABI is one wait and one signal -- see
 * event_uapi.h for the userspace contract and ../README.md for the design
 * walk-through.
 *
 * Signals are counted in a per-event generation. A waiter passes the
 * generation it last saw: if the event has moved past it, the wait returns
 * immediately, so a wait/work/re-arm loop can never lose a signal that
 * fired while it was busy. Up-to-date waiters park until the next signal
 * (or their timeout).
 *
 * The implementation is lock-free: every shared access is a single atomic
 * operation.
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
 *  - Node lifetime is a three-state handoff. The single atomic that moves a
 *    node out of EV_WAITING decides who frees it:
 *
 *        EV_WAITING --xchg by signaler--> EV_SIGNALED   waiter frees it
 *        EV_WAITING --cmpxchg by waiter-> EV_CANCELLED  waiter abandons it;
 *                                                       the next signal (or
 *                                                       release) frees it
 *
 *    Every early exit from a wait -- interrupting signal, timeout, or a
 *    generation bump that raced past the registration -- resolves through
 *    the same EV_CANCELLED handoff. A cancelled node cannot be unlinked
 *    from the middle of a singly-linked list without a lock, so it stays in
 *    place until the next signal walk or the event's destruction frees it;
 *    that deferred reclamation is also why nodes live on a cache-aligned
 *    slab rather than on the waiter's stack.
 *
 *  - The wake itself is made safe by RCU, not refcounting. A node still
 *    EV_WAITING at the signaler's xchg() means its waiter was inside the
 *    wait at that instant: it cannot return (and so cannot exit) before
 *    observing EV_SIGNALED, which only we publish. Its release_task() --
 *    after which the task_struct is freed one RCU grace period later --
 *    therefore happens inside our read-side section, so wake_up_process()
 *    never touches freed memory. (The same argument the kernel's rcuwait
 *    relies on.)
 */

#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "event_uapi.h"

/* Who owns (and frees) a subscriber node -- see the state diagram above. */
enum subscriber_state {
	EV_WAITING,   /* on the list, waiter parked; owner: pending */
	EV_SIGNALED,  /* signaler woke (or will wake) the waiter; waiter frees */
	EV_CANCELLED, /* waiter left early; the claiming signal/release frees */
};

/*
 * One entry per parked waiter, allocated per wait from a cache-aligned slab
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
	atomic64_t gen;		 /* signal count; never decreases */
};

static struct kmem_cache *subscriber_cache;

/*
 * Take the node back from the signalers. True: we won, the node is now
 * abandoned in place (EV_CANCELLED) for a later signal/release to free --
 * the caller must NOT free or touch it again. False: a signaler got there
 * first; we have been signaled and the node is ours to free.
 */
static bool subscriber_cancel(struct subscriber *sub)
{
	return cmpxchg(&sub->state, EV_WAITING, EV_CANCELLED) == EV_WAITING;
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
 * No waiter can still be inside do_wait() here: every waiter (and every
 * signaler) holds a reference on the file across its ioctl, so release only
 * runs after they have all returned. Whatever is left on the list is
 * therefore an abandoned EV_CANCELLED node -- free them. Seeing EV_WAITING
 * would mean a waiter left without resolving its node; scream.
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
 * do_wait(): return once the event has been signaled past w->gen -- either
 * immediately (it already has) or by registering and parking until a signal
 * or the timeout (w->timeout_ms: < 0 forever, 0 poll, > 0 bound) arrives.
 * On success w->gen is updated to the current generation.
 *
 * The lost-wakeup race is closed the classic way: the task state is set to
 * TASK_INTERRUPTIBLE *before* re-checking ->state, so a signaler either
 * sees us parked (and wakes us) or we see EV_SIGNALED (and skip the sleep).
 * set_current_state() is a full barrier; the acquire load below pairs with
 * the signaler's fully-ordered xchg() of ->state.
 *
 * A signal can also slip in between the entry generation check and our list
 * push (it claims the list without our node, so it will never mark us
 * EV_SIGNALED). The generation re-check inside the loop catches exactly
 * that window: our push and set_current_state() are full barriers, so after
 * them we cannot read a generation older than one bumped before the claim.
 */
static int do_wait(struct event *evt, struct event_wait *w)
{
	struct subscriber *sub;
	ktime_t deadline;
	u64 cur;
	int ret = 0;

	cur = (u64)atomic64_read(&evt->gen);
	if (cur != w->gen) {
		w->gen = cur;
		return 0; /* signaled while the caller was away */
	}

	if (w->timeout_ms >= 0)
		deadline = ktime_add_ms(ktime_get(), w->timeout_ms);

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

		/* A signal claimed the list before our push: it will never
		 * wake us, but the caller must see it. Resolve the node and
		 * report it. (Losing the cancel means an even newer signal
		 * did claim us: same outcome.) */
		if ((u64)atomic64_read(&evt->gen) != w->gen) {
			if (subscriber_cancel(sub))
				sub = NULL; /* abandoned */
			break;
		}

		if (signal_pending(current)) {
			if (subscriber_cancel(sub)) {
				ret = -ERESTARTSYS;
				sub = NULL; /* abandoned */
			}
			break;
		}

		if (w->timeout_ms < 0) {
			schedule();
		} else if (schedule_hrtimeout(&deadline, HRTIMER_MODE_ABS) ==
			   0) {
			/* Deadline passed; a signal racing the expiry still
			 * beats it (the cancel loses to it). */
			if (smp_load_acquire(&sub->state) == EV_SIGNALED)
				break;
			if (subscriber_cancel(sub)) {
				ret = -ETIMEDOUT;
				sub = NULL; /* abandoned */
			}
			break;
		}
	}
	__set_current_state(TASK_RUNNING);

	/* EV_SIGNALED: the signaler is done with the node; we own it. */
	if (sub)
		kmem_cache_free(subscriber_cache, sub);

	if (ret == 0)
		w->gen = (u64)atomic64_read(&evt->gen);

	return ret;
}

/*
 * signal_event(): bump the generation and wake every registered subscriber.
 *
 * One xchg() detaches the whole subscriber list; from that point every node
 * on it belongs to this call alone (concurrent signalers get disjoint
 * chains, new waiters push onto the fresh empty list and catch up through
 * the generation).
 *
 * Per node, ->next and ->task are read *before* publishing EV_SIGNALED,
 * because the instant the waiter can observe that state it may free the
 * node. One RCU read-side section spanning the walk makes the wakes safe
 * without refcounting -- see the lifetime notes at the top of the file.
 * ->task is only dereferenced when the xchg() proved the waiter was still
 * parked; a cancelled node's stale pointer is never used.
 */
static int signal_event(struct event *evt)
{
	struct subscriber *sub, *next;
	int woken = 0;

	/*
	 * Bump the generation FIRST, so that by the time any waiter is woken
	 * (or any late waiter checks), the signal is already visible in the
	 * counter. The fully-ordered xchg()/cmpxchg() traffic on ->head and
	 * ->state orders this increment for everyone who needs it: a waiter
	 * whose push lands after our claim below performs a full barrier
	 * (its cmpxchg) and then cannot read a pre-increment generation.
	 */
	atomic64_inc(&evt->gen);

	/*
	 * Common fast path: nobody is waiting. A plain read keeps the
	 * cacheline shared between concurrent signalers (an xchg would
	 * dirty it even when there is nothing to take). Returning 0 against
	 * a concurrently-racing registration is linearizable: that waiter
	 * simply registered "after" this signal -- and its generation check
	 * delivers the signal anyway.
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
			/* EV_CANCELLED: abandoned by an early-exiting waiter */
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
	case EVENT_IOC_WAIT: {
		struct event_wait __user *uw = (struct event_wait __user *)arg;
		struct event_wait w;
		int ret;

		if (copy_from_user(&w, uw, sizeof(w)))
			return -EFAULT;
		ret = do_wait(evt, &w);
		if (!ret && copy_to_user(uw, &w, sizeof(w)))
			return -EFAULT;
		return ret;
	}
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

	pr_info("event: loaded, device at /dev/event\n");
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
MODULE_DESCRIPTION("Minimal event synchronization object (/dev/event)");
MODULE_VERSION("4.0");
