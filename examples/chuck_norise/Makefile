# Standard out-of-tree Linux kernel module Makefile.
#
# Read in two modes:
#  1. As a Kbuild fragment when the kernel build system pulls it in via
#     `make -C $KDIR M=$THIS_DIR modules` (KERNELRELEASE is set; Kbuild
#     assignments run, wrapper recipe is skipped). This is the mode the
#     lab's outer build uses.
#  2. As a standalone Makefile when you run `make` in this dir directly
#     (KERNELRELEASE is unset; the wrapper recipe re-invokes mode 1
#     against the host kernel).

obj-m += chuck_norise.o
chuck_norise-y := src/hello.o src/chuck_device.o src/chuck_message.o

ccflags-y := -I$(src)/include -g -DDEBUG

ifndef KERNELRELEASE
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(CURDIR)

.PHONY: default clean

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
