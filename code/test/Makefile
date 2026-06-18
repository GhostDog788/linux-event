# Functional tests for event.ko. Build here, copy to the target (where
# /dev/event lives), run, then check dmesg for WARNs. See ../../HANDOFF.md.

CC      ?= cc
LIBDIR  := ../lib
CFLAGS  ?= -g -O2 -Wall -Wextra
CFLAGS  += -I$(LIBDIR)

TARGETS := selftest poll-storm

.PHONY: all clean

all: $(TARGETS)

selftest: selftest.c $(LIBDIR)/event.h
	$(CC) $(CFLAGS) -o $@ selftest.c

poll-storm: poll-storm.c $(LIBDIR)/event.h
	$(CC) $(CFLAGS) -pthread -o $@ poll-storm.c

clean:
	rm -f $(TARGETS)
