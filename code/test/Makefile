# Functional tests and an ABI drift guard for event.ko. Build here, copy the
# functional tests to the target (where /dev/event lives), run, then check
# dmesg for WARNs. abi-check builds locally and needs no target.

CC      ?= cc
LIBDIR  := ../lib
UAPIDIR := ../module
CFLAGS  ?= -g -O2 -Wall -Wextra
CFLAGS  += -I$(LIBDIR)

TARGETS := selftest broadcast-storm abi-check

.PHONY: all clean

all: $(TARGETS)

selftest: selftest.c $(LIBDIR)/event.h
	$(CC) $(CFLAGS) -o $@ selftest.c

broadcast-storm: broadcast-storm.c $(LIBDIR)/event.h
	$(CC) $(CFLAGS) -pthread -o $@ broadcast-storm.c

# Fails to compile (macro redefinition under -Werror) if the lib and the kernel
# uapi disagree on the ioctl numbers. Pure compile check, produces no useful
# binary beyond proving the two headers match.
abi-check: abi-check.c $(LIBDIR)/event.h $(UAPIDIR)/event_uapi.h
	$(CC) $(CFLAGS) -I$(UAPIDIR) -Werror -o $@ abi-check.c

clean:
	rm -f $(TARGETS)
