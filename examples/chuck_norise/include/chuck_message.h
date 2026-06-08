#ifndef CHUCK_MESSAGE_H
#define CHUCK_MESSAGE_H

#include <linux/compiler_types.h>
#include <linux/types.h>

const char *chuck_message_text(void);
size_t chuck_message_len(void);
ssize_t chuck_message_read(char __user *buffer, size_t count, loff_t *position);

#endif
