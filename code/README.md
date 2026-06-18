# `event` + `waiter`: blocking on events, one or many

A small Linux kernel subsystem for **blocking until events fire**, with *zero*
polling latency and *zero* wasted CPU. Two objects:

- an **event** you can signal, and
- a **waiter** that blocks on a *set* of events until at least *k* of them have
  fired.

## The problem

You have a condition (classically: "a global boolean became true") and you want
a thread to wait for it. Polling (check, sleep, check again) trades latency for
CPU: the shorter your sleep, the more responsive the wait but the more cycles
you burn. These objects remove the trade-off: a waiter sleeps in the kernel and
is woken the instant an event it listens on is signaled.

The waiter generalizes that to many events at once. A thread can listen on a
whole set and block until at least *k* of them are ready, which a single-event
wait cannot express without polling or a thread per event.

## The model

- An **event** holds a 64-bit **generation** (0 at creation) and a *persistent*
  list of **listeners**. `signal_event()` bumps the generation and wakes every
  listener, leaving them registered. The event does not track how many
  listeners it has.
- A **waiter** holds a set of events (one **listener** per event) and a wait
  queue. `waiter_wait()` blocks until at least `want` of its events are ready,
  or a timeout expires, or it listens on fewer than `want` events (which can
  never reach the threshold, so it returns at once).
- An event is **ready** for a waiter while the event's generation is ahead of
  the generation that waiter last **consumed** for it. A wait that reports an
  event consumes it, advancing the mark to the event's current generation.

Readiness is therefore **counted, not edge-triggered**: a signal that fires
while a waiter is between waits is still seen on its next wait, and a burst of
signals during one stretch of work coalesces into a single report with the
generation jumped by the burst size. A wait/work/re-arm loop observes every
signal no matter how late it re-arms.

## Layout

```
code/
├── module/          the kernel module, builds event.ko
│   ├── event.c          char-device driver exposing /dev/event and /dev/waiter
│   ├── event_uapi.h     the ioctl ABI (shared, in spirit, with the lib)
│   └── Makefile         out-of-tree kbuild
├── lib/             header-only userspace API
│   └── event.h          static-inline wrappers over ioctl on the two devices
├── example/         a runnable demo: many listeners, one sender
│   ├── demo.c           forks N listeners that each block on the one event
│   └── Makefile
├── bench/           the performance yardstick for implementation iterations
│   ├── bench.c          wake-latency / fan-out / throughput harness (+ futex control)
│   ├── compare.py       statistical A/B verdict between two bench runs
│   └── README.md        methodology + the comparison protocol
└── README.md        this file
```

## The API

You compile and load `event.ko`. In your program you `#include "event.h"` (from
`lib/`) and link nothing: every function is a `static inline` wrapper around an
`ioctl` on `/dev/event` or `/dev/waiter`.

| Function | Object | Meaning |
| --- | --- | --- |
| `int create_event(void)` | event | Allocate a new event; returns an **fd**. |
| `int signal_event(int evt)` | event | Bump the generation and wake every listener. Returns the number of listeners notified. |
| `int close_event(int evt)` | event | Destroy the event (drop this reference). |
| `int create_waiter(void)` | waiter | Allocate a new waiter; returns an **fd**. |
| `int waiter_add(int w, int evt)` | waiter | Start listening on `evt`. Returns 0, or -1/`EEXIST` if already in the set. |
| `int waiter_remove(int w, int evt)` | waiter | Stop listening on `evt`. Returns 0, or -1/`ENOENT`. |
| `int waiter_wait(int w, int *ready_fds, size_t want, int64_t timeout_ms)` | waiter | Block until at least `want` events are ready, the timeout expires, or the waiter listens on fewer than `want` events. Fills `ready_fds` with the fds that fired and returns how many (0 to `want`); a short count means the timeout expired. Pass `EVT_WAIT_FOREVER` to never time out, `EVT_WAIT_ZERO` to poll. |
| `int close_waiter(int w)` | waiter | Destroy the waiter (drop this reference). |

Events and waiters are open fds, which makes sharing natural: open the event,
then `fork()`, and parent and children all reference the **same** kernel object
(the open file table is inherited across a fork). That is how the example wires
up many listeners and one sender. `ready_fds` reports the event fds that fired;
if you close an event while a waiter still lists it, the reported fd number is
stale (the object itself stays alive while listed, see below), so remove it
first.

The single-event wait is just the `want = 1`, one-event case:

```c
int evt = create_event();
int w   = create_waiter();
waiter_add(w, evt);

int ready[1];
while (waiter_wait(w, ready, 1, EVT_WAIT_FOREVER) == 1)
        do_work();        /* signals during the work are caught by the next
                             wait, never lost */
```

Quorum across many events is the general case:

```c
int ready[N];
int n = waiter_wait(w, ready, K, 500);   /* >= K ready, or 500 ms */
for (int i = 0; i < n; i++)
        handle(ready[i]);                /* ready[i] is the fd that fired */
```

## Inside the kernel

`create_event()` and `create_waiter()` are `open()` on the two devices; each
open allocates a fresh object in `file->private_data`. Everything else is an
ioctl. A **listener** is one `(waiter, event)` registration, linked on both
objects' lists at once:

```c
struct event {
        spinlock_t lock;            /* protects listeners */
        struct list_head listeners; /* struct listener.ev_node */
        atomic64_t gen;             /* signal count; read without the lock */
};

struct waiter {
        spinlock_t lock;            /* protects listeners */
        struct list_head listeners; /* struct listener.w_node */
        wait_queue_head_t wq;       /* signalers wake this */
};

struct listener {
        struct list_head ev_node;   /* on event->listeners  */
        struct list_head w_node;    /* on waiter->listeners */
        struct event *event;
        struct waiter *waiter;
        struct file *evt_file;      /* ref held; keeps the event alive */
        int evt_fd;                 /* identity reported in the ready set */
        u64 seen_gen;               /* generation this listener last consumed */
};
```

The generation is atomic, so readiness is a lock-free read:
`atomic64_read(&l->event->gen) != l->seen_gen`.

**Signaling** bumps the generation, then walks the event's list and wakes each
listener's waiter:

```c
static int signal_event(struct event *evt)
{
        struct listener *l;
        int woken = 0;

        atomic64_inc(&evt->gen);            /* the signal exists before */
                                            /* any woken waiter observes it */
        spin_lock(&evt->lock);
        list_for_each_entry(l, &evt->listeners, ev_node) {
                wake_up_interruptible(&l->waiter->wq);
                woken++;
        }
        spin_unlock(&evt->lock);
        return woken;
}
```

**Waiting** blocks on the waiter's queue until enough events are ready, then
gathers and consumes the ready ones:

```c
/* wake condition, re-evaluated on every wake */
static bool waiter_satisfied(struct waiter *w, u32 want)
{
        struct listener *l;
        u32 ready = 0, total = 0;

        spin_lock(&w->lock);
        list_for_each_entry(l, &w->listeners, w_node) {
                total++;
                if ((u64)atomic64_read(&l->event->gen) != l->seen_gen)
                        ready++;
        }
        spin_unlock(&w->lock);
        return ready >= want || total < want;    /* fewer than want => now */
}

/* in waiter_wait(): */
ret = wait_event_interruptible_hrtimeout(w->wq,
                                         waiter_satisfied(w, want),
                                         ms_to_ktime(timeout_ms));
/* then, under w->lock, copy up to want ready fds into a buffer and set each
 * one's seen_gen to the event's current generation (consume), and
 * copy_to_user() the buffer after dropping the lock. */
```

### Why this is correct

- **Generation ordering.** The counter is bumped before any waiter can be
  woken (the wake carries the barrier), so a waiter that wakes always observes
  the new generation, and a signal that lands while a waiter is away is
  reflected by `gen != seen_gen` on its next wait. No signal is lost; bursts
  coalesce.

- **Lock order, no ABBA.** Two locks exist, plus each waiter's wait-queue lock.
  - `waiter_add` and `waiter_remove` take `waiter->lock` then `event->lock`,
    always in that order.
  - `signal_event` takes `event->lock` then a wait-queue lock (inside
    `wake_up_interruptible`); it never takes a `waiter->lock`. It reaches
    waiters only through their wait queues.
  - the wait path takes `waiter->lock` (inside the condition) and, separately,
    a wait-queue lock (inside the wait macro). The condition reads each event's
    generation atomically, taking no `event->lock`.

  The only place two object locks nest is add/remove, in one fixed order, so
  there is no cycle.

- **Listener lifetime.** A listener is unlinked from an event's list only under
  that `event->lock`, and `signal_event` walks the list under the same lock, so
  it never dereferences a listener that is being freed, and `l->waiter` is
  valid for the duration of the walk (a waiter unlinks all of its listeners
  before it is freed).

- **Event lifetime.** Each listener holds a reference to the event's `struct
  file` (`evt_file`), so an event's `release` cannot run while any waiter still
  lists it: by the time the last `close()` runs, the listener list is empty.
  Closing an event fd while a waiter still lists it leaves the object alive (the
  held reference) but the reported fd number stale; remove it first.

The cost of persistent membership is a lock per object instead of the previous
lock-free single event: a listener can be removed from the middle of a list
concurrently with a signal walking it, which the take-all trick of the earlier
design (claim the whole list in one `xchg`, never remove a single node) cannot
serve. Whether the trade wins, and in which regime, is measured, not argued;
see [Benchmarking](#benchmarking-an-implementation-iteration).

See `module/event.c` for the full, commented source.

## Relationship to epoll

The shape is deliberately epoll-like: two fd-backed objects, an
`epoll_ctl`-style add/remove, and a wait that returns the fds that are ready.
The semantics differ:

- **What "ready" means.** epoll watches I/O readiness of pollable fds; an event
  here is a pure application notification (`signal_event()`), with no event mask
  and no underlying file state.
- **Quorum wait.** `epoll_wait` returns as soon as one fd is ready;
  `waiter_wait` blocks until at least `want` are ready, a k-of-n barrier epoll
  has no equivalent for.
- **Counted, coalescing readiness.** A signal fired while a waiter is away is
  still seen next wait, and a burst coalesces into one report. epoll is edge or
  level on current I/O state.
- **Broadcast fan-out.** The target shape is many waiters, each its own thread,
  all listening on one shared event, all woken by one signal, closer to a
  condition-variable broadcast than epoll's one-fd-many-sources gather.
- **Surface.** No rbtree, ready-list, edge/level/oneshot flags, nested epoll, or
  user-data cookie (fd identity instead): a small, purpose-built primitive.

## Build & run

You need the kernel headers for your running kernel
(`sudo apt install linux-headers-$(uname -r)`).

```bash
# 1. build and load the module
cd module
make
sudo insmod event.ko          # creates /dev/event and /dev/waiter (mode 0666)

# 2. build and run the demo (5 listeners by default; pass a count to change)
cd ../example
make
./demo 5

# 3. when you're done
sudo rmmod event
```

Expected output (order of the wake-ups varies; they all unblock together):

```
[sender | pid 1234] created event (fd 3), spawning 5 listeners
  [listener 0 | pid 1235] waiting for the event...
  [listener 1 | pid 1236] waiting for the event...
  ...
[sender | pid 1234] signaling the event, waking all listeners
  [listener 0 | pid 1235] >>> woke up, event received!
  [listener 2 | pid 1237] >>> woke up, event received!
  ...
[sender | pid 1234] done (0 failures)
```

> The demo `sleep(1)`s before signaling only so the "waiting..." lines print
> before the wake-ups. Signals are counted in the event's generation, so a
> listener that registers and waits late returns immediately instead of missing
> the signal.

### Debugging the demo in VS Code

The lab can build, upload, and source-debug the demo on the target (where
`/dev/event` and `/dev/waiter` live):

- **Build:** run the **"User: Build demo"** task (it just `make`s `code/example`).
- **Debug (F5):** pick **"User: demo (remote)"**. Its pre-launch task
  (`scripts/05-debug-user.sh <target>`) builds the demo, uploads it, loads
  `event.ko` if the devices are missing, and starts `gdbserver` on the target;
  the launch then attaches host-side gdb and breakpoints in `demo.c` bind.

The gdbserver port is forwarded over the SSH connection, so it works through a
NAT'd VM with no extra port-forwarding. Knobs (env vars read by
`scripts/05-debug-user.sh`): `USERDEBUG_PORT` (port, default 2345), `DEMO_ARGS`
(listener count, default 3).

The debugger follows the **sender** (the parent) by default; that is the path
that calls `signal_event()`. The listeners are forked children; to break inside
one, run `set follow-fork-mode child` in the Debug Console before continuing.

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

`compare.py` prints per-metric medians/p99s with a significance test, and a
built-in futex baseline acts as a control that catches noisy runs. Two notes on
metrics under this design: `signal_event()` returns the number of listeners
notified (not parked waiters woken), so the churn scenario counts throughput on
the waiter side; and `empty_signal_pct` is a control-side metric, meaningful for
futex, near zero for the event whose listeners stay registered. The full
protocol and what each metric means live in [`bench/README.md`](bench/README.md).

## History

This subsystem began as a single lock-free `event`: one object, a `wait` that
parked the caller on that event's LIFO subscriber list, and a `signal` that
claimed the whole list in one `xchg` and woke everyone. That design (and the
three races fixed on the way to it, a missing list link, a lost-wakeup window,
and a use-after-free in the wake) lives in `git log module/event.c`.

The current version splits the object in two so a thread can block on a *set* of
events with a quorum, the shape a single-event wait cannot express without
polling or a thread per event. Persistent membership (a listener stays
registered across signals and can be removed concurrently with a signal walking
the list) is what take-all claiming cannot serve, so the implementation moved to
a lock per object plus a wait queue per waiter, the same shape the kernel's own
epoll uses. Lock-free was always a measured choice here, not a goal in itself;
the benchmark in `bench/` is how the two are judged.

## License

MIT; see [../LICENSE](../LICENSE).
