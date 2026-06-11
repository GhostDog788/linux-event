// SPDX-License-Identifier: MIT
/*
 * event.ko - the /dev/event synchronization object.
 *
 * Each open() creates an independent event. Waiters park (zero CPU) until
 * the event is signaled past the generation they carry; signal_event()
 * bumps the generation and wakes every parked waiter. Lock-free: every
 * shared access is one atomic operation. The design and its safety
 * arguments are documented in ../README.md.
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

/* The atomic that moves ->state out of EV_WAITING decides who frees the
 * node. */
enum subscriber_state {
	EV_WAITING,
	EV_SIGNALED,  /* signaler wakes the waiter; the waiter frees */
	EV_CANCELLED, /* waiter left early; the next signal/release frees */
	EV_REAPED,    /* abandoned node claimed by a walk; freed via RCU */
};

/*
 * Slab-allocated per wait: an early-exiting waiter abandons its node in
 * place (a middle node cannot be unlinked lock-free), so it cannot live on
 * the waiter's stack. ->task is unreferenced; signalers may only
 * dereference it under RCU after proving the waiter still parked.
 */
struct subscriber {
	struct subscriber *next;
	struct task_struct *task;
	unsigned int state;
	struct rcu_head rcu;
};

struct event {
	struct subscriber *head; /* lock-free LIFO: cmpxchg push, xchg take-all */
	atomic64_t gen;		 /* signal count */
	atomic_t cancels;	 /* cancellations outstanding; 0 enables the
				  * fast signal walk (see README) */
	atomic_t fast_walks;	 /* fast walks in flight */
};

static struct kmem_cache *subscriber_cache;

static void subscriber_free_rcu(struct rcu_head *rcu)
{
	kmem_cache_free(subscriber_cache,
			container_of(rcu, struct subscriber, rcu));
}

/*
 * True: the cancel stands and the node is no longer ours (abandoned in
 * place, or already reaped by a walk). False: a signal won the race; the
 * caller is signaled and the node, marked EV_SIGNALED, is ours to free.
 */
static bool subscriber_cancel(struct event *evt, struct subscriber *sub)
{
	unsigned int s;

	/* Announce before marking: a signal walk either sees the count and
	 * takes the careful path, or is already in flight and waited out
	 * below. */
	atomic_inc(&evt->cancels);
	smp_mb__after_atomic();

	if (cmpxchg(&sub->state, EV_WAITING, EV_CANCELLED) != EV_WAITING) {
		atomic_dec(&evt->cancels);
		return false;
	}

	/* Fast walks blindly signal every node, so wait out any that began
	 * before our announcement was visible (bounded: no new one can
	 * start), then look at what happened. RCU keeps the node readable
	 * even if a careful walk reaps it meanwhile. */
	rcu_read_lock();
	while (atomic_read_acquire(&evt->fast_walks))
		cpu_relax();
	s = smp_load_acquire(&sub->state);
	rcu_read_unlock();

	if (s == EV_SIGNALED) {
		atomic_dec(&evt->cancels);
		return false;
	}
	/* EV_CANCELLED: abandoned, counted until a walk reaps it.
	 * EV_REAPED: a walk already freed it and dropped the count. */
	return true;
}

static int event_open(struct inode *inode, struct file *file)
{
	struct event *evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	file->private_data = evt;
	return 0;
}

/* Every wait holds a file reference across its ioctl, so by the time the
 * last close() runs, anything left is an abandoned EV_CANCELLED node. */
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
 * Return once the event is signaled past w->gen: immediately if it already
 * has been, else register and park until a signal, an interrupting POSIX
 * signal (-ERESTARTSYS), or the timeout (-ETIMEDOUT). Writes the current
 * generation back on success.
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
		return 0;
	}

	if (w->timeout_ms >= 0)
		deadline = ktime_add_ms(ktime_get(), w->timeout_ms);

	sub = kmem_cache_alloc(subscriber_cache, GFP_KERNEL);
	if (!sub)
		return -ENOMEM;
	sub->task = current;
	sub->state = EV_WAITING;

	/* Treiber push; the fully-ordered cmpxchg publishes the fields. */
	do {
		sub->next = READ_ONCE(evt->head);
	} while (cmpxchg(&evt->head, sub->next, sub) != sub->next);

	for (;;) {
		/* State first, checks second: a signaler either sees us
		 * parked or we see its update; no lost wakeup. */
		set_current_state(TASK_INTERRUPTIBLE);

		if (smp_load_acquire(&sub->state) == EV_SIGNALED)
			break;

		/* A signal claimed the list before our push: it will never
		 * wake us, but the caller must see it. */
		if ((u64)atomic64_read(&evt->gen) != w->gen) {
			if (subscriber_cancel(evt, sub))
				sub = NULL;
			break;
		}

		if (signal_pending(current)) {
			if (subscriber_cancel(evt, sub)) {
				ret = -ERESTARTSYS;
				sub = NULL;
			}
			break;
		}

		if (w->timeout_ms < 0) {
			schedule();
		} else if (schedule_hrtimeout(&deadline, HRTIMER_MODE_ABS) ==
			   0) {
			/* Deadline passed; a signal racing it wins. */
			if (smp_load_acquire(&sub->state) == EV_SIGNALED)
				break;
			if (subscriber_cancel(evt, sub)) {
				ret = -ETIMEDOUT;
				sub = NULL;
			}
			break;
		}
	}
	__set_current_state(TASK_RUNNING);

	if (sub) /* EV_SIGNALED: the signaler is done with it; ours to free */
		kmem_cache_free(subscriber_cache, sub);

	if (ret == 0)
		w->gen = (u64)atomic64_read(&evt->gen);

	return ret;
}

/* Bump the generation and wake every parked waiter; returns how many. */
static int signal_event(struct event *evt)
{
	struct subscriber *sub, *next;
	int woken = 0;
	bool fast;

	/* Counted before anyone can observe the wake (the ordered RMW
	 * traffic below propagates it), so late waiters always catch up. */
	atomic64_inc(&evt->gen);

	/* Nobody waiting: a plain read spares the cacheline an xchg. */
	if (!READ_ONCE(evt->head))
		return 0;

	/* With no cancellation outstanding, every claimed node is a live
	 * parked waiter and the walk needs no per-node atomics. Announce
	 * the fast walk before checking, so a cancel this check misses is
	 * guaranteed to see it and wait for us (see subscriber_cancel). */
	atomic_inc(&evt->fast_walks);
	smp_mb__after_atomic();
	fast = atomic_read(&evt->cancels) == 0;
	if (!fast)
		atomic_dec(&evt->fast_walks);

	/* Take-all: every claimed node now belongs to this call alone. */
	sub = xchg(&evt->head, NULL);

	rcu_read_lock(); /* pins each still-parked waiter's task (README) */
	while (sub) {
		struct task_struct *task = sub->task;

		/* The waiter may free sub the instant it sees EV_SIGNALED;
		 * read everything first. */
		next = sub->next;

		if (fast) {
			smp_store_release(&sub->state, EV_SIGNALED);
			wake_up_process(task);
			woken++;
		} else if (cmpxchg(&sub->state, EV_WAITING, EV_SIGNALED) ==
			   EV_WAITING) {
			wake_up_process(task);
			woken++;
		} else {
			/* EV_CANCELLED: reap the abandoned node. RCU defers
			 * the free past any canceller still looking at it. */
			smp_store_release(&sub->state, EV_REAPED);
			call_rcu(&sub->rcu, subscriber_free_rcu);
			atomic_dec(&evt->cancels);
		}

		sub = next;
	}
	rcu_read_unlock();

	if (fast) {
		/* Order the stores above before releasing any canceller
		 * spinning on the in-flight count. */
		smp_mb__before_atomic();
		atomic_dec(&evt->fast_walks);
	}

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
	.mode = 0666,
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
	rcu_barrier(); /* flush pending subscriber_free_rcu callbacks */
	kmem_cache_destroy(subscriber_cache);
	pr_info("event: unloaded\n");
}

module_init(event_init);
module_exit(event_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("dor");
MODULE_DESCRIPTION("Minimal event synchronization object (/dev/event)");
MODULE_VERSION("5.0");
