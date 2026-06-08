# Build the event demo. The userspace API is header-only, so the only thing we
# need is an include path pointing at ../lib.

CC      ?= cc
LIBDIR  := ../lib
# -g and -O0 so the demo is debuggable (the "User: demo (remote)" F5 wants
# source-level breakpoints and readable locals); a demo needs no optimization.
CFLAGS  ?= -g -O0 -Wall -Wextra
CFLAGS  += -I$(LIBDIR)

TARGET  := demo

.PHONY: all clean run

all: $(TARGET)

$(TARGET): demo.c $(LIBDIR)/event.h
	$(CC) $(CFLAGS) -o $@ demo.c

# Convenience: build then run with the default number of listeners.
run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
