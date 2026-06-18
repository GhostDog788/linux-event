# `event`: a pollable broadcast event

A small Linux kernel object for **broadcasting an event to many listeners**,
each of which waits with the kernel's own `poll`/`epoll`/`select`. One publisher
signals with a single call and never has to know how many are listening; every
listener independently sees every signal; and a listener can wait on many events
at once (plus any other pollable fd), waking when *k of n* are ready and finding
them in O(ready).

## The problem

You want to notify many waiters that something happened: a task finished, a
state changed. The waiters should sleep with zero CPU until it does, the
publisher should stay trivial (one call, no list of subscribers in its code),
and each waiter should be able to fold this into its existing event loop
alongside sockets, timers, and everything else it already waits on.

No standard primitive does all of that. `eventfd` is single-consumer (the first
reader drains it, so it cannot fan out to many). `futex` can wake many but is
not pollable, so it cannot share a wait with other fds. This object fills the
gap: a **broadcast source** whose listeners are **pollable fds**.

## The model

- An **event** (one `open("/dev/event")`) is a broadcast source: a generation
  counter and a list of subscriptions. `signal_event()` raises the generation
  once and wakes every subscription. One syscall, no enumeration by the caller.
- A **subscription** is a pollable fd a listener gets from the `SUBSCRIBE`
  ioctl. It carries its *own* consumed generation, so each listener sees every
  signal independently (true broadcast). It is readable while the event has been
  signaled since that listener last read it; `read()` returns the number of
  signals since then (a `u64`) and clears readiness. A burst coalesces into one
  read; a signal that fired while a listener was away is still seen next read.
- **k-of-n is the listener's epoll**, not the kernel's: a listener adds its
  subscription fds (and any other fds) to its own `epoll`, and waits until at
  least k are ready, finding them in O(ready). The kernel only broadcasts; the
  k-of-n wait lives in userspace where it composes with everything else.

## Layout

```
code/
├── module/          the kernel module, builds event.ko
│   ├── event.c          /dev/event (broadcast source) + subscription anon fds
│   ├── event_uapi.h     the ioctl ABI (shared, in spirit, with the lib)
│   └── Makefile         out-of-tree kbuild
├── lib/             header-only userspace API
│   └── event.h          wrappers + the epoll-based k-of-n helper
├── example/         a runnable demo
│   ├── demo.c           broadcast one signal to N listeners, then a k-of-n wait
│   └── Makefile
├── bench/           the performance yardstick (event vs a futex broadcast control)
│   ├── bench.c          wake-latency / fan-out / throughput harness
│   ├── compare.py       statistical A/B verdict between two bench runs
│   └── README.md        methodology + the comparison protocol
├── test/            functional tests (selftest, broadcast-storm)
└── README.md        this file
```

## The API

You load `event.ko` and `#include "event.h"`. The publisher uses an ioctl to
signal; the listener uses standard syscalls on its subscription fd.

| Function | Role | Meaning |
| --- | --- | --- |
| `int create_event(void)` | publisher | Allocate a broadcast event; returns an **fd**. |
| `int signal_event(int evt)` | publisher | Raise the generation and wake every subscription. Returns the number notified. |
| `int close_event(int evt)` | publisher | Drop the event; live subscriptions get a hangup and finish on their own. |
| `int subscribe_event(int evt)` | listener | Create a subscription onto `evt`; returns a pollable **fd**. |
| `int event_read(int sub, uint64_t *count)` | listener | Consume (never blocks): set `*count` to signals since last read and advance the cursor. Returns `1` alive (`*count` valid, 0 means nothing fired), `0` if the event is dead (closed), `-1` on error. |
| `int event_wait(int sub, int timeout_ms)` | listener | Convenience: `poll` one subscription then drain. Count (>= 1), 0 on timeout, -1 on error. |
| `int event_wait_first(const int *subs, int n, int k, int timeout_ms, int *ready)` | listener | Wait for the first k of n subscriptions to be ready; drains those k, fills `ready[]` (capacity >= k), returns how many it wrote (k on success, fewer on timeout). |
| `int event_wait_any(const int *subs, int n, int timeout_ms)` | listener | Wait for any one to be ready; returns its fd (>= 0), or -1 with errno (`ETIMEDOUT` on timeout). The k == 1 case. |
| `int event_wait_all(const int *subs, int n, int timeout_ms)` | listener | Wait for all n to be ready (the k == n case); returns n on success, fewer on timeout. No `ready[]`: on success every one of `subs` fired. |
| `int close_subscription(int sub)` | listener | Drop a subscription (auto-detaches from its event). |

The event fd is shared (`fork`/`dup`), so a publisher can hand it to listeners;
each listener `subscribe_event()`s to get its own pollable fd. Publisher:

```c
int evt = create_event();
/* ...whenever a task finishes... */
signal_event(evt);            /* one call wakes every listener */
```

A listener loop, k-of-n over its own subscriptions, mixing in any other fd via
its epoll:

```c
int sub = subscribe_event(evt);
struct epoll_event ev = { .events = EPOLLIN, .data.fd = sub };
epoll_ctl(ep, EPOLL_CTL_ADD, sub, &ev);   /* alongside sockets, timerfd, ... */
/* accumulate until k of the watched events are ready, then read each */
```

`event_wait_first` (with `event_wait_any` for k == 1 and `event_wait_all` for
k == n) packages the common pure-event case; for mixing with non-event fds, run
the same accumulate-until-k loop on your own epoll.

## Inside the kernel

`create_event()` is `open("/dev/event")`; `SUBSCRIBE` returns an `anon_inode`
fd. The two objects are reference-counted together:

```c
struct event {
        spinlock_t lock;        /* protects subs + dead */
        struct list_head subs;  /* struct subscription.node */
        atomic64_t gen;         /* signal count; read locklessly */
        refcount_t refcount;    /* the event fd + each live subscription */
        bool dead;              /* event fd closed: no more signals */
};

struct subscription {
        struct list_head node;  /* on event->subs (under event->lock) */
        struct event *event;    /* holds a refcount reference */
        wait_queue_head_t wqh;
        u64 seen;               /* consumed generation; under wqh.lock */
};
```

`gen` is atomic, so a subscription's read and poll never touch `event->lock`;
hundreds of listeners read in parallel without contending. `event->lock` covers
only the subs list and `dead`, taken by SUBSCRIBE, subscription release,
SIGNAL's wake-walk, and event close, all rare next to reads.

```c
/* signal_event(): raise the generation, wake every subscription. */
atomic64_inc(&evt->gen);
spin_lock(&evt->lock);
list_for_each_entry(sub, &evt->subs, node)
        wake_up_interruptible_poll(&sub->wqh, EPOLLIN);
spin_unlock(&evt->lock);

/* subscription read(): consume, never block. return gen - seen (maybe 0). */
spin_lock_irq(&sub->wqh.lock);
if (atomic64_read(&evt->gen) == sub->seen && evt->dead) {
        unlock; return 0;                                 /* hangup: EOF */
}
cnt = atomic64_read(&evt->gen) - sub->seen; sub->seen += cnt;
spin_unlock_irq(&sub->wqh.lock);                          /* copy_to_user(cnt) */
```

The subscription is **poll-to-wait, read-to-consume**: you wait on it with
`poll`/`epoll`, and `read` never blocks, it just reports the count since last
read (possibly 0) and advances the cursor. A 0-byte read is reserved for the
hangup case (event closed, nothing pending), so it stays distinct from an
8-byte read of value 0 ("nothing fired"). Why this is correct:

- **No lost wakeup.** The generation is raised before the wake (which carries
  the barrier). All the waiting is `poll`/`epoll` on `wqh`, which the kernel's
  poll machinery handles; `poll` reads `gen`/`seen` locklessly and the
  subsequent `read` re-checks under `wqh.lock`. Keeping `read` non-blocking
  leaves the only wakeup reasoning in the poll path.
- **Broadcast with independent consume.** Each subscription has its own `seen`,
  advanced only by its own reads, so every listener observes every signal and a
  burst coalesces, with no consumer racing another to drain a shared counter.
- **Lifetime.** The event is refcounted by its fd and every live subscription,
  so it outlives whichever closes first. Closing the event fd marks it `dead`
  and wakes every subscription with `EPOLLHUP` (a clean shutdown signal, like a
  pipe write-end closing); a subscription's `read` then returns 0. A
  subscription `close` unlinks under `event->lock` and drops its ref.
- **Lock order, no ABBA.** SIGNAL and event close take `event->lock` then a
  `wqh.lock` (inside the wake); subscription read/poll take only `wqh.lock`;
  subscription release takes only `event->lock`. Nothing nests them the other
  way, and a subscription cannot be freed mid-SIGNAL because release and the
  wake-walk serialize on `event->lock`.

See `module/event.c` for the full, commented source.

## Relationship to eventfd and epoll

A subscription behaves like an `eventfd`: a counter you wait on as a file and
`read` to drain. The event adds the one thing `eventfd` lacks, **broadcast**:
one `signal_event()` fans out to every subscription inside the kernel, so the
publisher stays O(1) no matter how many listen (writing N eventfds yourself is
O(N) syscalls and forces the publisher to track them). Everything else, the
poll/epoll wait and the k-of-n, is the kernel's existing machinery.

## Requirements

- A Linux kernel with headers installed for the running kernel
  (`sudo apt install linux-headers-$(uname -r)` on Debian/Ubuntu). Developed and
  tested on **6.17**; it uses only long-stable APIs (`anon_inode_getfd`,
  `wait_queue_head_t`, `noop_llseek`), so recent kernels in that range should
  work, but 6.17 is the version actually exercised here.
- A C toolchain (`gcc`/`clang`, `make`) for the module and the userspace
  programs. The userspace side is plain POSIX plus `epoll`, so it builds with no
  extra libraries.

## Build & run

```bash
cd module
make
sudo insmod event.ko          # creates /dev/event (mode 0666, no root to use)

cd ../example
make
./demo 5                      # 5 listeners, one signal wakes all; then wait-first

sudo rmmod event
```

Expected output (wake order varies; all unblock together):

```
[publisher | pid 1234] created event, forking 5 listeners
  [listener 0 | pid 1235] subscribed, waiting...
  ...
[publisher | pid 1234] one signal, waking all listeners
  [listener 3 | pid 1238] >>> woke up, event received!
  ...
[first] subscribing to 3 events, waiting for the first 2
[first] 2 ready: event0 event2
```

## Benchmarking an implementation iteration

`module/event.c` is meant to be iterated on; whether an iteration is better, and
in which regime, is decided by [`bench/`](bench/), not by eyeballing:

```bash
cd bench
make
./bench                                   # event vs a futex broadcast control
./bench -c run.csv -l rev                 # raw samples for compare.py
python3 compare.py baseline.csv run.csv   # statistical A/B verdict
```

`compare.py` prints per-metric medians/p99s with a significance test. Each run
also measures a **futex broadcast control** (one shared counter, `FUTEX_WAKE`
wakes all), the kernel-native way to broadcast; if it shifts between runs the
machine was not quiet and the verdict is void. See
[`bench/README.md`](bench/README.md).

## Optional: the remote-VM lab

The repo also ships a personal lab harness for developing the module against a
separate VM target: `scripts/` (provision, build, deploy, kgdb/qemu debug,
bench), the top-level `Makefile`, and the VS Code tasks/launch configs. It is
optional and not needed to build, run, or test anything above; copy
`lab.example.env` to `lab.local.env` and edit it for your machines if you want
it. With it set up, `scripts/06-bench.sh <target>` runs the bench on the target
and fetches the CSV, and **F5** in VS Code source-debugs the demo remotely.

## History

This object was built three other ways first, each a custom in-kernel mechanism
(`git log` and the sibling branches): a lock-free single-event park/wake, a
lock-based `event` + `waiter` quorum object, and an eventfd-shaped single
pollable device. A fourth branch showed plain `eventfd` + `epoll` covers the
single-consumer cases with no module at all. This version is for the case those
cannot serve: broadcast to many independent listeners. It keeps the kernel piece
to the irreducible minimum (pollable fan-out) and leaves k-of-n to epoll.

## License

MIT; see [../LICENSE](../LICENSE).
