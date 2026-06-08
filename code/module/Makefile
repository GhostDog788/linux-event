# Out-of-tree Makefile for event.ko.
#
# Standalone build (against the running kernel's headers):
#     make
#     sudo insmod event.ko      # creates /dev/event (mode 0666)
#     sudo rmmod event
#
# Or build against a specific kernel tree:
#     make KDIR=/path/to/linux-headers-X
#
# The `ifneq ($(KERNELRELEASE),)` wrapper lets the same file work both as a
# top-level `make` here and when Kbuild recurses back in with KERNELRELEASE set.

ifneq ($(KERNELRELEASE),)

obj-m := event.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif
