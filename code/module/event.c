// SPDX-License-Identifier: MIT
/*
 * event.ko - the /dev/event and /dev/waiter synchronization objects.
 *
 * An event (/dev/event) is a generation counter plus a persistent list of
 * listeners. signal_event() bumps the generation and wakes every listener
 * without removing it. A waiter (/dev/waiter) holds a set of events; its wait
 * blocks until at least ->want of them are ready, where an event is ready
 * while its generation is ahead of the one this waiter last consumed for it.
 *
 * A listener is one (waiter, event) registration, linked on both objects'
 * lists. Each object has a spinlock over its list; the generation is atomic so
 * readiness is read without the event lock. The design and its locking /
 * lifetime arguments are documented in ../README.md.
 */

#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "event_uapi.h"

struct event {
	spinlock_t lock;	    /* protects listeners */
	struct list_head listeners; /* struct listener.ev_node */
	atomic64_t gen;		    /* signal count; read without the lock */
};

struct waiter {
	spinlock_t lock;	    /* protects listeners */
	struct list_head listeners; /* struct listener.w_node */
	wait_queue_head_t wq;	    /* signalers wake this */
};

/* One (waiter, event) registration; lives on both lists at once. */
struct listener {
	struct list_head ev_node; /* on event->listeners  */
	struct list_head w_node;  /* on waiter->listeners */
	struct event *event;
	struct waiter *waiter;
	struct file *evt_file;	  /* ref held; keeps the event alive while linked */
	int evt_fd;		  /* identity reported in the ready set */
	u64 seen_gen;		  /* generation this listener last consumed */
};

static struct kmem_cache *listener_cache;
static const struct file_operations event_fops;

/* True once the event's generation has moved past what this listener saw. */
static bool listener_ready(struct listener *l)
{
	return (u64)atomic64_read(&l->event->gen) != l->seen_gen;
}

/* ---- event object ---- */

static int event_open(struct inode *inode, struct file *file)
{
	struct event *evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	spin_lock_init(&evt->lock);
	INIT_LIST_HEAD(&evt->listeners);
	atomic64_set(&evt->gen, 0);

	file->private_data = evt;
	return 0;
}

/* Every listener holds an evt_file reference, so the last close() cannot run
 * while any listener is still linked: the list is necessarily empty here. */
static int event_release(struct inode *inode, struct file *file)
{
	struct event *evt = file->private_data;

	WARN_ON_ONCE(!list_empty(&evt->listeners));
	kfree(evt);
	return 0;
}

/* Bump the generation, then wake every listener (leaving it registered);
 * returns how many were notified. The generation is bumped before the wake,
 * which carries the barrier, so a woken waiter observes it. */
static int signal_event(struct event *evt)
{
	struct listener *l;
	int woken = 0;

	atomic64_inc(&evt->gen);

	spin_lock(&evt->lock);
	list_for_each_entry(l, &evt->listeners, ev_node) {
		wake_up_interruptible(&l->waiter->wq);
		woken++;
	}
	spin_unlock(&evt->lock);

	return woken;
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
	.unlocked_ioctl = event_ioctl,
	.compat_ioctl = event_ioctl,
};

/* ---- waiter object ---- */

static int waiter_open(struct inode *inode, struct file *file)
{
	struct waiter *w;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	spin_lock_init(&w->lock);
	INIT_LIST_HEAD(&w->listeners);
	init_waitqueue_head(&w->wq);

	file->private_data = w;
	return 0;
}

/* Last close(): drain every registration, unlinking each from its event
 * (event lock) before dropping the held event reference. */
static int waiter_release(struct inode *inode, struct file *file)
{
	struct waiter *w = file->private_data;
	struct listener *l, *tmp;

	spin_lock(&w->lock);
	list_for_each_entry_safe(l, tmp, &w->listeners, w_node) {
		spin_lock(&l->event->lock);
		list_del(&l->ev_node);
		spin_unlock(&l->event->lock);
		list_del(&l->w_node);
		fput(l->evt_file);
		kmem_cache_free(listener_cache, l);
	}
	spin_unlock(&w->lock);

	kfree(w);
	return 0;
}

/* Resolve an event fd to its object, holding the file reference on success. */
static struct event *event_from_fd(int fd, struct file **filep)
{
	struct file *f = fget(fd);

	if (!f)
		return ERR_PTR(-EBADF);
	if (f->f_op != &event_fops) {
		fput(f);
		return ERR_PTR(-EINVAL);
	}
	*filep = f;
	return f->private_data;
}

/* Register evt with the waiter. Lock order is waiter then event (see README).
 * seen_gen is sampled under the event lock at link time, so the listener is
 * edge-from-now: only signals after this point make it ready. */
static int waiter_add(struct waiter *w, int evt_fd)
{
	struct file *f;
	struct event *evt;
	struct listener *l, *new;
	int ret = 0;

	evt = event_from_fd(evt_fd, &f);
	if (IS_ERR(evt))
		return PTR_ERR(evt);

	new = kmem_cache_alloc(listener_cache, GFP_KERNEL);
	if (!new) {
		fput(f);
		return -ENOMEM;
	}
	new->event = evt;
	new->waiter = w;
	new->evt_file = f;
	new->evt_fd = evt_fd;

	spin_lock(&w->lock);
	list_for_each_entry(l, &w->listeners, w_node) {
		if (l->event == evt) {
			ret = -EEXIST;
			goto out;
		}
	}

	spin_lock(&evt->lock);
	new->seen_gen = (u64)atomic64_read(&evt->gen);
	list_add(&new->ev_node, &evt->listeners);
	spin_unlock(&evt->lock);
	list_add(&new->w_node, &w->listeners);

out:
	spin_unlock(&w->lock);
	if (ret) {
		kmem_cache_free(listener_cache, new);
		fput(f);
	}
	return ret;
}

static int waiter_remove(struct waiter *w, int evt_fd)
{
	struct file *f;
	struct event *evt;
	struct listener *l, *found = NULL;

	evt = event_from_fd(evt_fd, &f);
	if (IS_ERR(evt))
		return PTR_ERR(evt);

	spin_lock(&w->lock);
	list_for_each_entry(l, &w->listeners, w_node) {
		if (l->event == evt) {
			found = l;
			break;
		}
	}
	if (found) {
		spin_lock(&evt->lock);
		list_del(&found->ev_node);
		spin_unlock(&evt->lock);
		list_del(&found->w_node);
	}
	spin_unlock(&w->lock);

	fput(f); /* the lookup reference */
	if (!found)
		return -ENOENT;

	fput(found->evt_file);
	kmem_cache_free(listener_cache, found);
	return 0;
}

/* The wake condition: at least want ready, or fewer events listened on than
 * requested (which can never reach want, so return at once). */
static bool waiter_satisfied(struct waiter *w, u32 want)
{
	struct listener *l;
	u32 ready = 0, total = 0;

	spin_lock(&w->lock);
	list_for_each_entry(l, &w->listeners, w_node) {
		total++;
		if (listener_ready(l))
			ready++;
	}
	spin_unlock(&w->lock);

	return ready >= want || total < want;
}

/* Gather up to want ready fds into buf, consuming each (mark its generation
 * seen) so it is not reported again until the event fires anew. */
static int waiter_collect(struct waiter *w, int *buf, u32 want)
{
	struct listener *l;
	int n = 0;

	spin_lock(&w->lock);
	list_for_each_entry(l, &w->listeners, w_node) {
		u64 cur;

		if (n == (int)want)
			break;
		cur = (u64)atomic64_read(&l->event->gen);
		if (cur != l->seen_gen) {
			l->seen_gen = cur;
			buf[n++] = l->evt_fd;
		}
	}
	spin_unlock(&w->lock);

	return n;
}

static int waiter_wait(struct waiter *w, struct event_wait *uw)
{
	int __user *ready_fds = (int __user *)(uintptr_t)uw->ready_fds;
	u32 want = uw->want;
	int *buf;
	int n, ret = 0;

	if (want == 0 || want > EVENT_WAIT_MAX)
		return -EINVAL;

	buf = kmalloc_array(want, sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (uw->timeout_ms < 0)
		ret = wait_event_interruptible(w->wq,
					       waiter_satisfied(w, want));
	else if (uw->timeout_ms > 0)
		ret = wait_event_interruptible_hrtimeout(
			w->wq, waiter_satisfied(w, want),
			ms_to_ktime(uw->timeout_ms));
	/* timeout_ms == 0 polls: skip the wait, collect whatever is ready. */

	if (ret == -ERESTARTSYS) /* interrupted; -ETIME just means collect now */
		goto out;

	n = waiter_collect(w, buf, want);
	if (copy_to_user(ready_fds, buf, n * sizeof(*buf))) {
		ret = -EFAULT;
		goto out;
	}
	ret = n;

out:
	kfree(buf);
	return ret;
}

static long waiter_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct waiter *w = file->private_data;

	switch (cmd) {
	case EVENT_IOC_ADD:
		return waiter_add(w, (int)arg);
	case EVENT_IOC_DEL:
		return waiter_remove(w, (int)arg);
	case EVENT_IOC_WAIT: {
		struct event_wait __user *uw = (struct event_wait __user *)arg;
		struct event_wait wreq;

		if (copy_from_user(&wreq, uw, sizeof(wreq)))
			return -EFAULT;
		return waiter_wait(w, &wreq);
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations waiter_fops = {
	.owner = THIS_MODULE,
	.open = waiter_open,
	.release = waiter_release,
	.unlocked_ioctl = waiter_ioctl,
	.compat_ioctl = waiter_ioctl,
};

/* ---- module ---- */

static struct miscdevice event_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "event",
	.fops = &event_fops,
	.mode = 0666,
};

static struct miscdevice waiter_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "waiter",
	.fops = &waiter_fops,
	.mode = 0666,
};

static int __init event_init(void)
{
	int ret;

	listener_cache = kmem_cache_create("event_listener",
					   sizeof(struct listener), 0,
					   SLAB_HWCACHE_ALIGN, NULL);
	if (!listener_cache)
		return -ENOMEM;

	ret = misc_register(&event_misc);
	if (ret) {
		pr_err("event: misc_register(event) failed: %d\n", ret);
		goto err_cache;
	}

	ret = misc_register(&waiter_misc);
	if (ret) {
		pr_err("event: misc_register(waiter) failed: %d\n", ret);
		goto err_event;
	}

	pr_info("event: loaded, devices at /dev/event and /dev/waiter\n");
	return 0;

err_event:
	misc_deregister(&event_misc);
err_cache:
	kmem_cache_destroy(listener_cache);
	return ret;
}

static void __exit event_exit(void)
{
	misc_deregister(&waiter_misc);
	misc_deregister(&event_misc);
	kmem_cache_destroy(listener_cache);
	pr_info("event: unloaded\n");
}

module_init(event_init);
module_exit(event_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("dor");
MODULE_DESCRIPTION("Event + waiter synchronization objects (/dev/event, /dev/waiter)");
MODULE_VERSION("5.0");
