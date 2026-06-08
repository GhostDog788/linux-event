#include "chuck_device.h"
#include "chuck_message.h"

#include <linux/delay.h>
#include <linux/module.h>

static unsigned int debug_delay_ms = 5000;
module_param(debug_delay_ms, uint, 0644);
MODULE_PARM_DESC(debug_delay_ms,
		 "Milliseconds to sleep at the start of init so the host can attach GDB and set breakpoints");

static int hello_init(void)
{
	int ret;

	if (debug_delay_ms)
		msleep(debug_delay_ms);

	ret = chuck_device_register();
	if (ret) {
		pr_err("hello: failed to register /dev/%s: %d\n",
		       CHUCK_DEVICE_NAME, ret);
		return ret;
	}

	pr_info("hello: loaded; reading /dev/%s repeats \"%s\"\n",
		CHUCK_DEVICE_NAME, chuck_message_text());
	return 0;
}

static void hello_exit(void)
{
	chuck_device_unregister();
	pr_info("hello: unloaded\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Kernel Module Debug Lab");
MODULE_DESCRIPTION("Multi-file char device example for kernel source debugging");
