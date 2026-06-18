# `event`: a pollable event with no custom object, just eventfd

A small event API for **blocking until an event fires**, with *zero* polling
latency and *zero* wasted CPU. This branch carries no kernel module at all: an
event is an `eventfd`, and you wait on it with the kernel's own `poll`,
`epoll`, or `select`. The header-only `lib/event.h` is the whole thing.

It exists to make a point. Earlier branches built custom in-kernel objects to
get a signalable, pollable event (a lock-free park/wake, then a lock-based
quorum object, then an eventfd-shaped char device). None of that is necessary:
the kernel already ships exactly this primitive. Here the same API, demo, tests,
and benchmark from those branches run unchanged, backed only by `eventfd`.

## The problem

You have a condition (classically: "a global boolean became true") and you want
a thread to wait for it. Polling (check, sleep, check again) trades latency for
CPU: the shorter your sleep, the more responsive the wait but the more cycles
you burn. An event removes the trade-off: a waiter sleeps in the kernel and is
woken the instant the event is signaled. And because it is an ordinary waitable
fd, it composes with everything the kernel already has for waiting on many fds.

## The model

- An **event** is an `eventfd`: a 64-bit counter you wait on as a file.
- `signal_event()` adds one to the counter (a `write` of 1) and wakes waiters.
- An event is **readable** while the counter is non-zero. `read()` returns the
  accumulated count and zeroes it, so a burst of signals coalesces into one read
  whose value is the burst size, and a signal that fired while you were away is
  still seen on your next read: no signal is lost.
- You wait however you like, because it is a real fd: `poll`/`epoll`/`select`
  for readiness then `read()` to consume, or a blocking `read()` directly.
  Readiness is **level-triggered** by default (ready until you drain it) and
  works **edge-triggered** under `EPOLLET` (one report per signal).

An event is single-consumer per open file description (the first reader drains
it); `fork`/`dup` share one event and its counter, while independent listeners
each create their own event.

## Layout

```
code/
├── lib/             the entire implementation
│   └── event.h          static-inline wrappers over eventfd + poll
├── example/         a runnable demo: many events, one epoll
│   ├── demo.c           watches N events with epoll, signals every other
│   └── Makefile
├── bench/           the performance yardstick
│   ├── bench.c          wake-latency / throughput harness (event vs futex)
│   ├── compare.py       statistical A/B verdict between two bench runs
│   └── README.md        methodology + the comparison protocol
├── test/            functional tests
│   ├── selftest.c       read/coalesce, poll level vs edge, select, blocking
│   ├── poll-storm.c     signal conservation under contention
│   └── Makefile
└── README.md        this file
```

## The API

You `#include "event.h"` (from `lib/`) and link nothing. The wait path is plain
syscalls on the fd; the helpers below are thin conveniences.

| Function | Role | Meaning |
| --- | --- | --- |
| `int create_event(void)` | both | Allocate a new event (`eventfd`); returns an **fd**. |
| `int create_event_flags(int flags)` | both | As above, with eventfd flags (`O_NONBLOCK`/`EFD_NONBLOCK`, `O_CLOEXEC`/`EFD_CLOEXEC`). |
| `int signal_event(int evt)` | publisher | Add one to the counter and wake every waiter. Returns 0. |
| `int event_read(int evt, uint64_t *count)` | waiter | Read the number of signals since the last read into `*count`, clearing readiness. Returns 0, or -1 with errno (`EAGAIN` if non-blocking and none pending). |
| `int event_wait(int evt, int timeout_ms)` | waiter | Convenience: `poll` up to `timeout_ms` (negative = forever) then drain. Returns the count (>= 1), 0 on timeout, -1 on error. |
| `int close_event(int evt)` | both | Destroy the event (drop this reference). |

A single-event waiter:

```c
int evt = create_event();
struct pollfd p = { .fd = evt, .events = POLLIN };
while (poll(&p, 1, -1) == 1) {
        uint64_t n;
        event_read(evt, &n);   /* n signals fired; clears readiness  */
        do_work();             /* signals during the work are caught */
}                              /* by the next read, never lost       */
```

Waiting on many events at once is just `epoll` (what the demo does):

```c
int ep = epoll_create1(0);
for (i = 0; i < N; i++) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = evt[i] };
        epoll_ctl(ep, EPOLL_CTL_ADD, evt[i], &ev);
}
int k = epoll_wait(ep, out, N, -1);
for (i = 0; i < k; i++) {
        uint64_t n;
        event_read(out[i].data.fd, &n);   /* out[i].data.fd fired */
}
```

## How it works

There is nothing to it: the API is a few `static inline` wrappers.

```c
static inline int create_event(void) { return eventfd(0, 0); }

static inline int signal_event(int evt)
{
        uint64_t one = 1;
        return write(evt, &one, sizeof(one)) == (ssize_t)sizeof(one) ? 0 : -1;
}

static inline int event_read(int evt, uint64_t *count)
{
        uint64_t c;
        if (read(evt, &c, sizeof(c)) != (ssize_t)sizeof(c))
                return -1;
        *count = c;
        return 0;
}
```

`eventfd` provides the counter, the wait queue, the `poll`/`epoll`/`select`
readiness, the level and edge semantics, the blocking and non-blocking reads,
and the cross-`fork` sharing. The library only spells the publisher's "add one"
as `signal_event()` and the consumer's "drain" as `event_read()`.

## Build & run

No module, no headers, no root.

```bash
cd example
make
./demo 5
```

Expected output (event ordering may vary):

```
[main | pid 1234] created 5 events, watching them with epoll
[signaler | pid 1235] signaling every other event
  [main] event 0 ready (2 signals)
  [main] event 2 ready (1 signal)
  [main] event 4 ready (1 signal)
[main | pid 1234] done (0 failures)
```

> The signaler `sleep(1)`s before signaling only so the parent reaches
> `epoll_wait` first. The counter accumulates, so an event signaled before
> anyone waits is still reported on the next wait, and event 0 (signaled twice)
> coalesces into one ready report of count 2.

Functional tests:

```bash
cd test
make
./selftest        # read/coalesce, poll level vs edge, select, blocking read
./poll-storm      # signal conservation under contention
```

The demo and tests are ordinary userspace programs; debug them with `gdb`
directly, no remote target or module load needed.

## Benchmarking

The harness in [`bench/`](bench/) measures wake latency and throughput of the
event (eventfd) against a per-waiter `futex` control:

```bash
cd bench
make
./bench                                   # event vs futex, full sweep
./bench -c out.csv -l run1                # raw samples for compare.py
python3 compare.py run1.csv run2.csv      # statistical A/B verdict
```

The `futex` control anchors what the hardware and scheduler can do and catches
a noisy machine: if it shifts between two runs, the comparison is void. The full
protocol lives in [`bench/README.md`](bench/README.md).

## History

This object was built three ways before arriving here, each a custom in-kernel
mechanism: a lock-free single-event park/wake (the `dev` line), a lock-based
`event` + `waiter` quorum object (`feature/epoll-like`), and an eventfd-shaped
pollable char device (`feature/pollable-event`). Each new branch simplified the
last. This one removes the kernel object entirely: the kernel's `eventfd` plus
`epoll` already are the pollable event, so the whole module is gone and the API
is a thin header over them.

## License

MIT; see [../LICENSE](../LICENSE).
