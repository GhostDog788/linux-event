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
};

struct event {
	struct subscriber *head; /* lock-free LIFO: cmpxchg push, xchg take-all */
	atomic64_t gen;		 /* signal count */
};

static struct kmem_cache *subscriber_cache;

/* True: the node is now abandoned (EV_CANCELLED) for a later signal or
 * release to free; the caller must not touch it again. False: a signaler
 * got there first -- we are signaled and the node is ours. */
static bool subscriber_cancel(struct subscriber *sub)
{
	return cmpxchg(&sub->state, EV_WAITING, EV_CANCELLED) == EV_WAITING;
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
		 * parked or we see its update -- no lost wakeup. */
		set_current_state(TASK_INTERRUPTIBLE);

		if (smp_load_acquire(&sub->state) == EV_SIGNALED)
			break;

		/* A signal claimed the list before our push: it will never
		 * wake us, but the caller must see it. */
		if ((u64)atomic64_read(&evt->gen) != w->gen) {
			if (subscriber_cancel(sub))
				sub = NULL;
			break;
		}

		if (signal_pending(current)) {
			if (subscriber_cancel(sub)) {
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
			if (subscriber_cancel(sub)) {
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

	/* Counted before anyone can observe the wake (the ordered RMW
	 * traffic below propagates it), so late waiters always catch up. */
	atomic64_inc(&evt->gen);

	/* Nobody waiting: a plain read spares the cacheline an xchg. */
	if (!READ_ONCE(evt->head))
		return 0;

	/* Take-all: every claimed node now belongs to this call alone. */
	sub = xchg(&evt->head, NULL);

	rcu_read_lock(); /* pins each still-parked waiter's task (README) */
	while (sub) {
		struct task_struct *task = sub->task;

		/* The waiter may free sub the instant it sees EV_SIGNALED;
		 * read everything first. */
		next = sub->next;

		if (xchg(&sub->state, EV_SIGNALED) == EV_WAITING) {
			wake_up_process(task);
			woken++;
		} else {
			kmem_cache_free(subscriber_cache, sub); /* abandoned */
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
	kmem_cache_destroy(subscriber_cache);
	pr_info("event: unloaded\n");
}

module_init(event_init);
module_exit(event_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("dor");
MODULE_DESCRIPTION("Minimal event synchronization object (/dev/event)");
MODULE_VERSION("4.1");
