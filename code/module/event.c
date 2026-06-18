// SPDX-License-Identifier: MIT
/*
 * event.ko - the /dev/event waitable object.
 *
 * Each open() creates an independent event: a generation counter plus a wait
 * queue. signal_event() (an ioctl) raises the generation and wakes anyone
 * waiting. The event is a first-class pollable file, so you wait on it with
 * poll/epoll/select or read() it; read() returns the number of signals since
 * the last read and clears readiness. This is eventfd's behavior with an
 * ioctl signal in place of write(): the readable count is gen - seen. The
 * design lives in ../README.md.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "event_uapi.h"

struct event {
	wait_queue_head_t wqh;
	u64 gen;  /* total signals; under wqh.lock */
	u64 seen; /* signals consumed via read(); under wqh.lock */
};

static int event_open(struct inode *inode, struct file *file)
{
	struct event *evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	init_waitqueue_head(&evt->wqh);
	file->private_data = evt;
	return 0;
}

static int event_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

/* Raise the generation and wake waiters (all pollers, one blocking reader). */
static int signal_event(struct event *evt)
{
	spin_lock_irq(&evt->wqh.lock);
	evt->gen++;
	wake_up_locked_poll(&evt->wqh, EPOLLIN);
	spin_unlock_irq(&evt->wqh.lock);
	return 0;
}

/*
 * Return the number of signals since the last read as a u64 and mark them
 * consumed (which clears readiness). Blocks until a signal arrives unless the
 * fd is O_NONBLOCK, in which case it returns -EAGAIN when nothing is pending.
 */
static ssize_t event_read(struct file *file, char __user *buf, size_t count,
			  loff_t *ppos)
{
	struct event *evt = file->private_data;
	u64 cnt;

	if (count < sizeof(cnt))
		return -EINVAL;

	spin_lock_irq(&evt->wqh.lock);
	if (evt->gen == evt->seen) {
		if (file->f_flags & O_NONBLOCK) {
			spin_unlock_irq(&evt->wqh.lock);
			return -EAGAIN;
		}
		/* Exclusive: many blocking readers of one fd don't all wake. */
		if (wait_event_interruptible_exclusive_locked_irq(
			    evt->wqh, evt->gen != evt->seen)) {
			spin_unlock_irq(&evt->wqh.lock);
			return -ERESTARTSYS;
		}
	}
	cnt = evt->gen - evt->seen;
	evt->seen = evt->gen;
	spin_unlock_irq(&evt->wqh.lock);

	if (copy_to_user(buf, &cnt, sizeof(cnt)))
		return -EFAULT;
	return sizeof(cnt);
}

static __poll_t event_poll(struct file *file, poll_table *wait)
{
	struct event *evt = file->private_data;

	poll_wait(file, &evt->wqh, wait);

	/* Lockless hint; read() re-checks under the lock before consuming. */
	if (READ_ONCE(evt->gen) != READ_ONCE(evt->seen))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static long event_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct event *evt = file->private_data;

	switch (cmd) {
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
	.read = event_read,
	.poll = event_poll,
	.unlocked_ioctl = event_ioctl,
	.compat_ioctl = event_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice event_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "event",
	.fops = &event_fops,
	.mode = 0666,
};

static int __init event_init(void)
{
	int ret = misc_register(&event_misc);

	if (ret) {
		pr_err("event: misc_register failed: %d\n", ret);
		return ret;
	}
	pr_info("event: loaded, device at /dev/event\n");
	return 0;
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
MODULE_DESCRIPTION("Pollable event object (/dev/event)");
MODULE_VERSION("6.0");
