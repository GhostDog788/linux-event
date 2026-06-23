// SPDX-License-Identifier: MIT
/*
 * event.ko - the /dev/event pollable broadcast event.
 *
 * open("/dev/event") is a broadcast source: a generation counter plus a list
 * of subscriptions. The SUBSCRIBE ioctl returns a pollable anon_inode fd with
 * its own consumed generation; SIGNAL raises the generation and wakes every
 * subscription, so one call reaches all listeners without the publisher
 * counting them. A subscription is poll-to-wait, read-to-consume. Design and
 * safety argument: ../README.md.
 */

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "event_uapi.h"

struct event {
	spinlock_t lock;	/* protects subs + dead */
	struct list_head subs;	/* struct subscription.node */
	atomic64_t gen;		/* signal count; read locklessly */
	refcount_t refcount;	/* the event fd + each live subscription */
	bool dead;		/* event fd closed: no more signals coming */
};

struct subscription {
	struct list_head node;	  /* on event->subs (under event->lock) */
	struct event *event;	  /* holds a refcount reference */
	wait_queue_head_t wqh;
	u64 seen;		  /* consumed generation; under wqh.lock */
};

static void event_put(struct event *evt)
{
	if (refcount_dec_and_test(&evt->refcount))
		kfree(evt);
}

/* ---- the broadcast source: /dev/event ---- */

static int event_open(struct inode *inode, struct file *file)
{
	struct event *evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	spin_lock_init(&evt->lock);
	INIT_LIST_HEAD(&evt->subs);
	atomic64_set(&evt->gen, 0);
	refcount_set(&evt->refcount, 1);

	file->private_data = evt;
	return 0;
}

/* The publisher is gone: wake every subscription with a hangup so listeners
 * can stop, and drop the event-fd reference (subscriptions keep it alive). */
static int event_release(struct inode *inode, struct file *file)
{
	struct event *evt = file->private_data;
	struct subscription *sub;

	spin_lock(&evt->lock);
	evt->dead = true;
	list_for_each_entry(sub, &evt->subs, node)
		wake_up_interruptible_poll(&sub->wqh, EPOLLHUP);
	spin_unlock(&evt->lock);

	event_put(evt);
	return 0;
}

static int signal_event(struct event *evt)
{
	struct subscription *sub;
	int woken = 0;

	atomic64_inc(&evt->gen); /* before any wake, which carries the barrier */

	spin_lock(&evt->lock);
	list_for_each_entry(sub, &evt->subs, node) {
		wake_up_interruptible_poll(&sub->wqh, EPOLLIN);
		woken++;
	}
	spin_unlock(&evt->lock);

	return woken;
}

/* ---- a subscription: a pollable anon_inode fd ---- */

static const struct file_operations subscription_fops;

/*
 * Consume, never block: return the signals since the last read as a u64
 * (possibly 0), advancing the cursor. A 0-byte return is hangup (event closed,
 * nothing pending), distinct from an 8-byte read of value 0 ("nothing fired").
 */
static ssize_t subscription_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct subscription *sub = file->private_data;
	struct event *evt = sub->event;
	u64 cur, cnt;

	if (count < sizeof(cnt))
		return -EINVAL;

	spin_lock_irq(&sub->wqh.lock);
	cur = (u64)atomic64_read(&evt->gen);
	if (cur == sub->seen && READ_ONCE(evt->dead)) {
		spin_unlock_irq(&sub->wqh.lock);
		return 0; /* source gone, nothing pending: EOF */
	}
	cnt = cur - sub->seen;
	sub->seen = cur;
	spin_unlock_irq(&sub->wqh.lock);

	if (copy_to_user(buf, &cnt, sizeof(cnt)))
		return -EFAULT;
	return sizeof(cnt);
}

static __poll_t subscription_poll(struct file *file, poll_table *wait)
{
	struct subscription *sub = file->private_data;
	struct event *evt = sub->event;
	__poll_t events = 0;

	poll_wait(file, &sub->wqh, wait);

	if ((u64)atomic64_read(&evt->gen) != READ_ONCE(sub->seen))
		events |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(evt->dead))
		events |= EPOLLHUP;
	return events;
}

static int subscription_release(struct inode *inode, struct file *file)
{
	struct subscription *sub = file->private_data;
	struct event *evt = sub->event;

	spin_lock(&evt->lock);
	list_del(&sub->node);
	spin_unlock(&evt->lock);

	event_put(evt);
	kfree(sub);
	return 0;
}

static const struct file_operations subscription_fops = {
	.owner = THIS_MODULE,
	.read = subscription_read,
	.poll = subscription_poll,
	.release = subscription_release,
	.llseek = noop_llseek,
};

/* Hand out a fresh subscription fd onto evt. The ioctl holds the event fd open,
 * so the event cannot be released under us. */
static int subscribe_event(struct event *evt)
{
	struct subscription *sub;
	int fd;

	sub = kzalloc(sizeof(*sub), GFP_KERNEL);
	if (!sub)
		return -ENOMEM;
	init_waitqueue_head(&sub->wqh);
	sub->event = evt;

	spin_lock(&evt->lock);
	if (evt->dead) {
		spin_unlock(&evt->lock);
		kfree(sub);
		return -ESHUTDOWN;
	}
	sub->seen = (u64)atomic64_read(&evt->gen); /* edge from now */
	refcount_inc(&evt->refcount);
	list_add(&sub->node, &evt->subs);
	spin_unlock(&evt->lock);

	fd = anon_inode_getfd("[event-sub]", &subscription_fops, sub,
			      O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		spin_lock(&evt->lock);
		list_del(&sub->node);
		spin_unlock(&evt->lock);
		refcount_dec(&evt->refcount); /* event fd still holds a ref */
		kfree(sub);
	}
	return fd;
}

static long event_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct event *evt = file->private_data;

	switch (cmd) {
	case EVENT_IOC_SIGNAL:
		return signal_event(evt);
	case EVENT_IOC_SUBSCRIBE:
		return subscribe_event(evt);
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
MODULE_DESCRIPTION("Pollable broadcast event (/dev/event)");
MODULE_VERSION("1.0");
