# Build the event benchmark. Like the demo, the userspace API is header-only,
# so all we need is the include path to ../lib -- plus -pthread and, unlike
# the demo, -O2: we are timing the harness's own code paths, so it must not
# be the bottleneck. Keep -g for perf/annotate on the target.

CC      ?= cc
LIBDIR  := ../lib
CFLAGS  ?= -g -O2 -Wall -Wextra
CFLAGS  += -I$(LIBDIR) -pthread
LDFLAGS += -pthread

TARGET  := bench

.PHONY: all clean

all: $(TARGET)

$(TARGET): bench.c $(LIBDIR)/event.h
	$(CC) $(CFLAGS) -o $@ bench.c $(LDFLAGS)

clean:
	rm -f $(TARGET)
