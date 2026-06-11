# `event` — non-busy blocking on an event

A small Linux kernel object that lets one or more threads **block until an
event fires**, with *zero* polling latency and *zero* wasted CPU cycles.

## The problem

You have a condition (classically: "a global boolean became true") and you want
a thread to wait for it. Polling — check, sleep, check again — trades latency
for CPU: the shorter your sleep, the more responsive the wait but the more
cycles you burn. The `event` object removes the trade-off entirely. A waiter
sleeps in the kernel and is woken the instant the event is signaled.

## The model

- An **event** is a kernel object that holds a list of **subscribers**
  (threads currently blocked on it).
- A **subscriber** registers the calling thread and parks it
  (`TASK_INTERRUPTIBLE`), so it consumes no CPU while waiting.
- A **publisher** signals the event, which walks the subscriber list and sets
  every parked thread back to `TASK_RUNNING` — waking them all at once.

The plain wait is **edge-triggered / one-shot**: signaling wakes exactly the
threads that are *already* waiting, and a thread that registers after the
signal blocks until the next one. For wait/work/re-arm loops that must not
miss signals fired while they were busy, the **generation-aware wait**
(`wait_for_event_gen`) returns immediately when the event was signaled since
the generation the caller last saw — every signal is observed, late or not.

## Layout

```
code/
├── module/          the kernel module — builds event.ko
│   ├── event.c          char-device driver exposing /dev/event
│   ├── event_uapi.h     the ioctl ABI (shared, in spirit, with the lib)
│   └── Makefile         out-of-tree kbuild
├── lib/             header-only userspace API
│   └── event.h          static-inline wrappers over ioctl on /dev/event
├── example/         a runnable demo: many listeners, one sender
│   ├── demo.c           forks N listeners that all block on one event
│   └── Makefile
├── bench/           the performance yardstick for implementation iterations
│   ├── bench.c          wake-latency / fan-out / throughput harness (+ futex control)
│   ├── compare.py       statistical A/B verdict between two bench runs
│   └── README.md        methodology + the comparison protocol
└── README.md        this file
```

## The API

You compile and load `event.ko`. In your program you `#include "event.h"` (from
`lib/`) and link nothing — every function is a `static inline` wrapper around an
`ioctl` on the `/dev/event` device the driver exposes.

| Function | Role | Meaning |
| --- | --- | --- |
| `int create_event(void)` | publisher | Allocate a new event object; returns an **fd** to it. |
| `int wait_for_event(int evt)` | subscriber | Register the calling thread and block until the event is signaled. Returns 0 on wake. |
| `int wait_for_event_gen(int evt, uint64_t *gen)` | subscriber | Like `wait_for_event`, but returns immediately if the event was signaled after generation `*gen` — a re-arming loop misses nothing. Updates `*gen`. |
| `int wait_for_event_timeout(int evt, int64_t ms)` | subscriber | Bounded wait: `EVT_SIGNALED` on wake, `EVT_TIMEOUT` after `ms` milliseconds. `EVT_WAIT_FOREVER` / `EVT_WAIT_ZERO` for the extremes. |
| `int wait_for_event_gen_timeout(int evt, uint64_t *gen, int64_t ms)` | subscriber | Generation-aware wait with a timeout — both of the above combined. |
| `int signal_event(int evt)` | publisher | Wake every thread currently waiting. Returns the number woken. |
| `int close_event(int evt)` | publisher | Destroy the event (drop this reference). |

An event *is* an open fd, which makes sharing natural: open the event, then
`fork()`, and parent and children all reference the **same** kernel object
(the open file table is inherited across a fork). That is precisely how the
example wires up many listeners and one sender.

### Inside the kernel

`create_event()` is `open("/dev/event")`; each open allocates a fresh event in
`file->private_data`. `wait_for_event` and `signal_event` are ioctls on that fd.

The implementation is **lock-free**: the event holds a single `head` pointer
to a LIFO list of subscriber nodes, and every shared access is one atomic
operation — there is no spinlock anywhere.

```c
/* wait_for_event(): register (one cmpxchg), then block until signaled. */
static int wait_for_event(struct event *evt)
{
        struct subscriber *sub = kmem_cache_alloc(subscriber_cache, GFP_KERNEL);

        sub->task  = current;             /* no refcount: RCU protects the wake */
        sub->state = EV_WAITING;

        do {                              /* Treiber push onto the list head */
                sub->next = READ_ONCE(evt->head);
        } while (cmpxchg(&evt->head, sub->next, sub) != sub->next);

        for (;;) {
                set_current_state(TASK_INTERRUPTIBLE);
                if (smp_load_acquire(&sub->state) == EV_SIGNALED)
                        break;            /* the signaler handed us the node */
                if (signal_pending(current)) {
                        if (cmpxchg(&sub->state, EV_WAITING, EV_CANCELLED)
                            == EV_WAITING)
                                return -ERESTARTSYS;  /* node abandoned in place */
                        break;            /* lost the race: we were signaled */
                }
                schedule();
        }
        __set_current_state(TASK_RUNNING);
        kmem_cache_free(subscriber_cache, sub);  /* ours on the signaled path */
        return 0;
}

/* signal_event(): claim the whole list with one xchg, wake every node. */
static int signal_event(struct event *evt)
{
        struct subscriber *sub, *next;
        int woken = 0;

        if (!READ_ONCE(evt->head))                /* common case: nobody is   */
                return 0;                         /* waiting; don't dirty the */
                                                  /* cacheline with an xchg   */
        sub = xchg(&evt->head, NULL);             /* take-all */

        rcu_read_lock();                          /* makes the wakes safe     */
        while (sub) {
                struct task_struct *task = sub->task;

                next = sub->next;                 /* read BEFORE the handoff  */
                if (xchg(&sub->state, EV_SIGNALED) == EV_WAITING) {
                        wake_up_process(task);    /* waiter frees sub         */
                        woken++;
                } else {
                        kmem_cache_free(subscriber_cache, sub); /* abandoned  */
                }
                sub = next;
        }
        rcu_read_unlock();
        return woken;
}
```

Why this is safe without a lock:

- **Take-all claiming.** `signal_event()` detaches the entire list with one
  `xchg`, after which it owns every claimed node outright — concurrent
  signalers get disjoint chains, and new waiters push onto the fresh empty
  list (the event stays edge-triggered). Take-all is also what makes the
  push-only `cmpxchg` immune to ABA.
- **Ownership handoff by state.** The single atomic that moves a node out of
  `EV_WAITING` decides who frees it: a signaler's `xchg → EV_SIGNALED` hands
  the node to the waiter; an interrupted waiter's `cmpxchg → EV_CANCELLED`
  abandons the node in place (it cannot be unlinked from the middle of the
  list without a lock) for the next signal or the final `close()` to free.
- **RCU-protected wakes, no refcounting.** A node still `EV_WAITING` at the
  signaler's `xchg` proves its waiter was inside `wait_for_event()` at that
  instant — it cannot return (let alone exit) before observing
  `EV_SIGNALED`, which only this signaler publishes. The task's
  `release_task()` therefore happens *inside* the signaler's RCU read
  section, and a `task_struct` is freed only one RCU grace period after
  `release_task()`, so `wake_up_process()` can never touch freed memory.
  (The same argument the kernel's `rcuwait` relies on; earlier iterations
  paid a `get/put_task_struct` pair per node instead, which dominated large
  fan-outs.)

The cost of going lock-free: nodes are slab-allocated per wait rather than
living on the waiter's stack (an interrupted waiter must be able to leave
while its node is still linked), and a cancelled wait leaves one node behind
until the next signal. Whether the trade wins is measured, not argued —
see [Benchmarking](#benchmarking-an-implementation-iteration) below.

See `module/event.c` for the full, commented source.

## Build & run

You need the kernel headers for your running kernel
(`sudo apt install linux-headers-$(uname -r)`).

```bash
# 1. build and load the module
cd module
make
sudo insmod event.ko          # creates /dev/event (mode 0666, so no root needed to use it)

# 2. build and run the demo (5 listeners by default; pass a count to change)
cd ../example
make
./demo 5

# 3. when you're done
sudo rmmod event
```

Expected output (order of the wake-ups varies — they all unblock together):

```
[sender | pid 1234] created event (fd 3), spawning 5 listeners
  [listener 0 | pid 1235] waiting for the event...
  [listener 1 | pid 1236] waiting for the event...
  ...
[sender | pid 1234] signaling the event -- waking all listeners
  [listener 0 | pid 1235] >>> woke up, event received!
  [listener 2 | pid 1237] >>> woke up, event received!
  ...
[sender | pid 1234] done (0 failures)
```

> The demo `sleep(1)`s before signaling so every forked listener has time to
> register first. Because the event is one-shot, a listener that hasn't reached
> `wait_for_event()` by the time the sender signals would miss it.

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
(listener count, default 3).

The debugger follows the **sender** (the parent) by default — that's the path
that calls `signal_event()`. The listeners are forked children; to break inside
one, run `set follow-fork-mode child` in the Debug Console before continuing.

## Benchmarking an implementation iteration

The implementation in `module/event.c` is meant to be iterated on. Whether an
iteration is actually *better* — and in which regime (single waiter, large
fan-out, high churn) — is decided by the benchmark in [`bench/`](bench/), not
by eyeballing:

```bash
scripts/06-bench.sh server          # run on the target, CSV lands in results/
# ...change event.c, rebuild (scripts/03) and reload (scripts/04), rerun...
python3 code/bench/compare.py results/<baseline>.csv results/<candidate>.csv
```

`compare.py` prints per-metric medians/p99s with a significance test, and a
built-in futex baseline acts as a control that catches noisy runs. The full
protocol and what each metric means live in [`bench/README.md`](bench/README.md).

## What changed from the original design

This object began as a paper design (`event - kernel.md`): the right idea — a
subscriber list with park-then-wake, maintained with **atomic operations
instead of a lock** — but with racy kernel pseudo-code. This implementation
keeps the lock-free intent and fixes three real bugs from the original
sketch:

1. **The registration loop never linked the node.** The original walked the
   list with `end = atomic_cmpxchg(end, NULL, sub)` and `end = end.next`,
   which neither appends `sub` nor terminates correctly. Appending at the
   *tail* is the hard way; pushing at the *head* makes registration a single
   correct `cmpxchg` (a Treiber push), and a wake-all event doesn't care
   about list order anyway.

2. **No wakeup-condition loop → lost wakeups and spurious returns.** The
   original did `set_current_state(...); schedule();` once, with nothing to
   re-check. A signal landing between "add to list" and `schedule()` would be
   lost, and any spurious wake would return as if signaled. The fix is the
   standard pattern: set the task state *before* re-checking the node's
   state, loop on `schedule()`, and honor `signal_pending()`.

3. **Use-after-free in `signal_event`.** The original `kfree(sub)`'d each
   subscriber while the waiter still referenced it (and while it could still
   be mid-wake). Without a lock, "don't free while someone looks" becomes an
   ownership problem; here a per-node atomic state transition decides exactly
   who frees each node, and task references make the wake itself safe — see
   *Inside the kernel* above.

(An earlier iteration of this module fixed the same three bugs with a
spinlocked `list_head` and stack-resident nodes — `git log module/event.c`
has it. The benchmark in `bench/` is how the two are judged against each
other.)

The net effect matches the design's intent — block with no polling, wake all
subscribers on signal, no lock anywhere — without the races.

## License

MIT — see [../LICENSE](../LICENSE).
