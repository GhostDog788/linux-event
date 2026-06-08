#include "chuck_device.h"

#include "chuck_message.h"

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/version.h>

struct chuck_device_state {
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *device;
	bool registered;
};

static struct chuck_device_state chuck_device;

static int chuck_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t chuck_read(struct file *file, char __user *buffer, size_t count,
			  loff_t *position)
{
	return chuck_message_read(buffer, count, position);
}

/* .llseek deliberately unset — kernel installs default_llseek for char
 * devices, which matches chuck_read's behavior (it respects *position).
 * Don't reach for no_llseek; it was removed in 6.12 (commit 868941b14441).
 */
static const struct file_operations chuck_fops = {
	.owner = THIS_MODULE,
	.open = chuck_open,
	.read = chuck_read,
};

static struct class *chuck_class_create(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	return class_create(CHUCK_CLASS_NAME);
#else
	return class_create(THIS_MODULE, CHUCK_CLASS_NAME);
#endif
}

int chuck_device_register(void)
{
	int ret;

	ret = alloc_chrdev_region(&chuck_device.devt, 0, 1, CHUCK_DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&chuck_device.cdev, &chuck_fops);
	chuck_device.cdev.owner = THIS_MODULE;

	ret = cdev_add(&chuck_device.cdev, chuck_device.devt, 1);
	if (ret)
		goto unregister_region;

	chuck_device.class = chuck_class_create();
	if (IS_ERR(chuck_device.class)) {
		ret = PTR_ERR(chuck_device.class);
		chuck_device.class = NULL;
		goto delete_cdev;
	}

	chuck_device.device = device_create(chuck_device.class, NULL,
					    chuck_device.devt, NULL,
					    CHUCK_DEVICE_NAME);
	if (IS_ERR(chuck_device.device)) {
		ret = PTR_ERR(chuck_device.device);
		chuck_device.device = NULL;
		goto destroy_class;
	}

	chuck_device.registered = true;
	pr_info("chuck_norise: registered /dev/%s major=%u minor=%u\n",
		CHUCK_DEVICE_NAME, MAJOR(chuck_device.devt),
		MINOR(chuck_device.devt));
	return 0;

destroy_class:
	class_destroy(chuck_device.class);
	chuck_device.class = NULL;
delete_cdev:
	cdev_del(&chuck_device.cdev);
unregister_region:
	unregister_chrdev_region(chuck_device.devt, 1);
	return ret;
}

void chuck_device_unregister(void)
{
	if (!chuck_device.registered)
		return;

	device_destroy(chuck_device.class, chuck_device.devt);
	class_destroy(chuck_device.class);
	cdev_del(&chuck_device.cdev);
	unregister_chrdev_region(chuck_device.devt, 1);
	chuck_device.registered = false;

	pr_info("chuck_norise: unregistered /dev/%s\n", CHUCK_DEVICE_NAME);
}
