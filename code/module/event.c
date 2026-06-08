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
 * See ../README.md for the design and a walk-through of the wait/signal logic.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "event_uapi.h"

/*
 * One entry per blocked subscriber. The struct lives on the *waiter's* kernel
 * stack while it sleeps inside the EVENT_IOC_WAIT ioctl, so it stays valid for
 * as long as the thread is blocked -- no kmalloc/kfree, and no use-after-free.
 */
struct subscriber {
	struct list_head node;
	struct task_struct *task;
	bool signaled;
};

struct event {
	struct list_head subscribers; /* list of struct subscriber */
	spinlock_t lock;	      /* protects @subscribers and ->signaled */
};

/* create_event(): each open() hands out a brand-new event object. */
static int event_open(struct inode *inode, struct file *file)
{
	struct event *evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	INIT_LIST_HEAD(&evt->subscribers);
	spin_lock_init(&evt->lock);
	file->private_data = evt;
	return 0;
}

/* Destroy the event once the last fd referencing it is closed. */
static int event_release(struct inode *inode, struct file *file)
{
	struct event *evt = file->private_data;

	/*
	 * By the time the final close() runs, every waiter must already have
	 * left wait_for_event() (each removes itself from the list before it
	 * returns), so the list is empty here. We assert that and free.
	 */
	WARN_ON_ONCE(!list_empty(&evt->subscribers));
	kfree(evt);
	return 0;
}

/*
 * wait_for_event(): register the calling thread and block until the event is
 * signaled (or a signal interrupts the wait).
 *
 * The classic prepare-to-wait pattern avoids the lost-wakeup race: we set the
 * task state to TASK_INTERRUPTIBLE *before* re-checking the condition under the
 * lock, so a concurrent signal_event() either sees us on the list (and wakes
 * us) or we see ->signaled (and skip the sleep).
 */
static int wait_for_event(struct event *evt)
{
	struct subscriber sub = {
		.task = current,
		.signaled = false,
	};
	int ret = 0;

	spin_lock(&evt->lock);
	list_add_tail(&sub.node, &evt->subscribers);
	spin_unlock(&evt->lock);

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);

		spin_lock(&evt->lock);
		if (sub.signaled) {
			spin_unlock(&evt->lock);
			break;
		}
		spin_unlock(&evt->lock);

		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}

		schedule();
	}
	__set_current_state(TASK_RUNNING);

	/*
	 * Remove ourselves from the list. If signal_event() already unlinked us
	 * (list_del_init), our node points to itself and this is a harmless
	 * no-op. Either way the publisher never touches our stack after this.
	 */
	spin_lock(&evt->lock);
	list_del_init(&sub.node);
	spin_unlock(&evt->lock);

	return ret;
}

/*
 * signal_event(): wake every currently-registered subscriber.
 *
 * We unlink and mark each waiter under the lock, then wake_up_process() flips
 * it back to TASK_RUNNING. We never free the subscriber here -- it is owned by
 * the waiter's stack.
 */
static int signal_event(struct event *evt)
{
	struct subscriber *sub, *tmp;
	int woken = 0;

	spin_lock(&evt->lock);
	list_for_each_entry_safe(sub, tmp, &evt->subscribers, node) {
		list_del_init(&sub->node);
		sub->signaled = true;
		wake_up_process(sub->task);
		woken++;
	}
	spin_unlock(&evt->lock);

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
	int ret = misc_register(&event_misc);

	if (ret)
		pr_err("event: misc_register failed: %d\n", ret);
	else
		pr_info("event: loaded, device at /dev/event\n");
	return ret;
}

static void __exit event_exit(void)
{
	misc_deregister(&event_misc);
	pr_info("event: unloaded\n");
}

module_init(event_init);
module_exit(event_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("dor");
MODULE_DESCRIPTION("Minimal event synchronization object (/dev/event)");
MODULE_VERSION("1.0");
