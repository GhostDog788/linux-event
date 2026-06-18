# `event`: a pollable event object

A small Linux kernel object for **blocking until an event fires**, with *zero*
polling latency and *zero* wasted CPU. It is a first-class pollable file: you
wait on it with `poll`, `epoll`, or `select`, or you `read()` it. A publisher
signals it with an ioctl, which raises a generation counter and wakes any
waiters.

## The problem

You have a condition (classically: "a global boolean became true") and you want
a thread to wait for it. Polling (check, sleep, check again) trades latency for
CPU: the shorter your sleep, the more responsive the wait but the more cycles
you burn. The event object removes the trade-off: a waiter sleeps in the kernel
and is woken the instant the event is signaled. And because it is an ordinary
waitable fd, it composes with everything the kernel already has for waiting on
many fds at once.

## The model

- An **event** is a kernel object holding a 64-bit **generation** (0 at
  creation) and a wait queue. `signal_event()` raises the generation and wakes
  everyone waiting.
- An event is **readable** while it has been signaled since you last read it.
  `read()` returns the number of signals since the last read (as a `u64`) and
  marks them consumed, which clears readiness. So a burst of signals coalesces
  into one read whose value is the burst size, and a signal that fired while you
  were away is still seen on your next read: no signal is lost.
- You wait however you like, because it is a real fd: `poll`/`epoll`/`select`
  for readiness, then `read()` to consume, or a blocking `read()` directly.
  Readiness is **level-triggered** by default (ready until you drain it) and
  works **edge-triggered** under `EPOLLET` (one report per signal).

This is exactly `eventfd`'s behavior with an ioctl signal in place of `write()`:
the readable count is `gen - seen`. An event is single-consumer per open file
description (the first reader drains it); `fork`/`dup` share one event and its
readiness, while independent listeners each create their own event.

## Layout

```
code/
├── module/          the kernel module, builds event.ko
│   ├── event.c          char-device driver exposing /dev/event
│   ├── event_uapi.h     the ioctl ABI (shared, in spirit, with the lib)
│   └── Makefile         out-of-tree kbuild
├── lib/             header-only userspace API
│   └── event.h          static-inline wrappers over /dev/event
├── example/         a runnable demo: many events, one epoll
│   ├── demo.c           watches N events with epoll, signals every other
│   └── Makefile
├── bench/           the performance yardstick for implementation iterations
│   ├── bench.c          wake-latency / throughput harness (+ eventfd, futex controls)
│   ├── compare.py       statistical A/B verdict between two bench runs
│   └── README.md        methodology + the comparison protocol
└── README.md        this file
```

## The API

You compile and load `event.ko`. In your program you `#include "event.h"` (from
`lib/`) and link nothing. The wait path uses standard syscalls on the fd; only
the publisher's signal is an ioctl.

| Function | Role | Meaning |
| --- | --- | --- |
| `int create_event(void)` | both | Allocate a new event; returns an **fd**. |
| `int create_event_flags(int flags)` | both | As above, with extra `open` flags (`O_NONBLOCK`, `O_CLOEXEC`). |
| `int signal_event(int evt)` | publisher | Raise the generation and wake every waiter. Returns 0. |
| `int event_read(int evt, uint64_t *count)` | waiter | Read the number of signals since the last read into `*count`, clearing readiness. Returns 0, or -1 with errno (`EAGAIN` if `O_NONBLOCK` and none pending). |
| `int event_wait(int evt, int timeout_ms)` | waiter | Convenience: `poll` up to `timeout_ms` (negative = forever) then drain. Returns the count (>= 1), 0 on timeout, -1 on error. |
| `int close_event(int evt)` | both | Destroy the event (drop this reference). |

Because the event is a real fd, the waiter side is just standard syscalls; the
helpers above are thin conveniences. A single-event waiter:

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

## Inside the kernel

`create_event()` is `open("/dev/event")`; each open allocates a fresh event in
`file->private_data`. The object is a wait queue plus two counters:

```c
struct event {
        wait_queue_head_t wqh;
        u64 gen;   /* total signals; under wqh.lock */
        u64 seen;  /* signals consumed via read(); under wqh.lock */
};
```

State changes and the wake happen under `wqh.lock`, the same discipline the
kernel's own `eventfd` uses.

```c
/* signal_event(): raise the generation and wake waiters. */
spin_lock_irq(&evt->wqh.lock);
evt->gen++;
wake_up_locked_poll(&evt->wqh, EPOLLIN);   /* all pollers + 1 reader */
spin_unlock_irq(&evt->wqh.lock);

/* read(): return gen - seen, mark consumed (clears readiness). */
spin_lock_irq(&evt->wqh.lock);
if (evt->gen == evt->seen) {
        if (file->f_flags & O_NONBLOCK) { unlock; return -EAGAIN; }
        wait_event_interruptible_exclusive_locked_irq(evt->wqh,
                                                      evt->gen != evt->seen);
}
cnt = evt->gen - evt->seen;
evt->seen = evt->gen;
spin_unlock_irq(&evt->wqh.lock);
/* copy_to_user(&cnt, 8) */

/* poll(): register and report readiness. */
poll_wait(file, &evt->wqh, wait);
return READ_ONCE(evt->gen) != READ_ONCE(evt->seen) ? EPOLLIN | EPOLLRDNORM : 0;
```

Why this is correct:

- **No lost wakeup.** The generation is raised and the wake issued under one
  `wqh.lock`; a blocking reader checks `gen != seen` under the same lock before
  it sleeps, so a signal cannot slip between the check and the sleep. `poll`
  reads the counters locklessly as a hint and `read` re-checks under the lock
  before consuming, so a stale poll only costs a re-poll, never a missed event
  (the standard `eventfd` pattern).
- **Single consumer, no double-count.** `seen` advances to `gen` exactly once
  per read under the lock, so concurrent readers of one fd split the signals
  rather than each seeing them; nothing is counted twice.
- **Exclusive blocking readers.** A blocking `read` waits exclusively, so a
  signal wakes one reader, not a thundering herd, while `poll`/`epoll` waiters
  (non-exclusive) are all woken.

See `module/event.c` for the full, commented source.

## Relationship to eventfd

This object is, deliberately, `eventfd` reimplemented: a counter you wait on as
a file, where `read()` drains it. The only behavioral difference is the signal
path, a named `EVENT_IOC_SIGNAL` ioctl (`signal_event()`) instead of `write()`,
and that the counter is expressed as a monotonic generation internally
(`count == gen - seen`). That near-equivalence is the point: it makes the
kernel's own `eventfd` the perfect benchmark control (same semantics, untouched
by our module), and it keeps the object a small, readable lab piece for studying
the wait/wake path. If you want this in production, you almost certainly want
`eventfd` itself; this exists to be understood and measured.

## Build & run

You need the kernel headers for your running kernel
(`sudo apt install linux-headers-$(uname -r)`).

```bash
# 1. build and load the module
cd module
make
sudo insmod event.ko          # creates /dev/event (mode 0666, no root to use)

# 2. build and run the demo (5 events by default; pass a count to change)
cd ../example
make
./demo 5

# 3. when you're done
sudo rmmod event
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
> `epoll_wait` first. Signals are counted in the generation, so an event
> signaled before anyone waits is still reported on the next wait, and event 0
> (signaled twice) coalesces into one ready report of count 2.

### Debugging the demo in VS Code

The lab can build, upload, and source-debug the demo on the target (where
`/dev/event` lives):

- **Build:** run the **"User: Build demo"** task (it just `make`s `code/example`).
- **Debug (F5):** pick **"User: demo (remote)"**. Its pre-launch task
  (`scripts/05-debug-user.sh <target>`) builds the demo, uploads it, loads
  `event.ko` if `/dev/event` is missing, and starts `gdbserver` on the target;
  the launch then attaches host-side gdb and breakpoints in `demo.c` bind.

The gdbserver port is forwarded over the SSH connection, so it works through a
NAT'd VM with no extra port-forwarding. Knobs (env vars read by
`scripts/05-debug-user.sh`): `USERDEBUG_PORT` (port, default 2345), `DEMO_ARGS`
(event count, default 3).

The debugger follows the parent by default; that is the path that runs the
`epoll` loop. The signaler is a forked child; to break inside it, run
`set follow-fork-mode child` in the Debug Console before continuing.

## Benchmarking an implementation iteration

The implementation in `module/event.c` is meant to be iterated on. Whether an
iteration is actually *better*, and in which regime (single waiter, large
fan-out, high churn), is decided by the benchmark in [`bench/`](bench/), not by
eyeballing:

```bash
scripts/06-bench.sh server          # run on the target, CSV lands in results/
# ...change event.c, rebuild (scripts/03) and reload (scripts/04), rerun...
python3 code/bench/compare.py results/<baseline>.csv results/<candidate>.csv
```

`compare.py` prints per-metric medians/p99s with a significance test. Each run
also measures two controls: the kernel's own `eventfd` (the same shape as our
object, so `event` should track it closely) and a per-waiter `futex`. If a
control shifts between runs the machine was not quiet and the verdict is void.
The full protocol lives in [`bench/README.md`](bench/README.md).

## History

This object began as a custom in-kernel wait mechanism: first a lock-free
single-event park/wake (`git log` on the `dev` line), then a lock-based
`event` + `waiter` quorum object that could block on a set of events
(`feature/epoll-like`). Both built their own way to put a thread to sleep and
wake it. This version drops the bespoke mechanism entirely: the event is just a
pollable file, and the kernel's existing `poll`/`epoll`/`select` do the waiting.
What remains is the smallest useful primitive, an `eventfd` signaled by ioctl.

## License

MIT; see [../LICENSE](../LICENSE).
