/* SPDX-License-Identifier: MIT */
/*
 * abi-check.c - compile-time guard that the userspace library (../lib/event.h)
 * and the kernel uapi (../module/event_uapi.h) agree on the ioctl ABI.
 *
 * Both headers define EVENT_IOC_MAGIC / EVENT_IOC_SIGNAL / EVENT_IOC_SUBSCRIBE.
 * Including both and building with -Werror turns any drift into a macro
 * redefinition error; identical definitions compile silently. This is the
 * single source of truth check the two hand-synced copies otherwise lack.
 */
#include "event_uapi.h"
#include "event.h"

int main(void)
{
	return 0;
}
