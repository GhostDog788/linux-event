# Functional tests for the eventfd-backed event API (../lib/event.h).
# No module to load; just build and run.

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
