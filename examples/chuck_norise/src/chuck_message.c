#include "chuck_message.h"

#include <linux/errno.h>
#include <linux/uaccess.h>

static const char chuck_text[] = "chuck norise!";

const char *chuck_message_text(void)
{
	return chuck_text;
}

size_t chuck_message_len(void)
{
	return sizeof(chuck_text) - 1;
}

ssize_t chuck_message_read(char __user *buffer, size_t count, loff_t *position)
{
	size_t copied = 0;
	size_t message_len = chuck_message_len();
	u64 offset;

	if (count == 0)
		return 0;

	if (*position < 0)
		return -EINVAL;

	offset = (u64)*position;

	while (copied < count) {
		char next = chuck_text[(offset + copied) % message_len];

		if (put_user(next, buffer + copied))
			return copied ? (ssize_t)copied : -EFAULT;

		copied++;
	}

	*position += copied;
	return copied;
}
