# How the broadcast event works

This explains the machinery of the pollable broadcast event: the two kernel
objects and how they fit together, the `anon_inode_getfd` trick that turns a
subscription into a real pollable fd, and a worked example that waits on a
socket and an event in one `epoll`.

For the why (when you would reach for this versus a plain `eventfd`), see
[README.md](README.md). This document is about the how.

## Two objects, one job each

There are two kernel objects, and the split is deliberate.

- The **event** is the broadcast *source*. It is shared: one publisher, and the
  one piece of state every listener observes is its 64-bit **generation**
  counter. Signaling raises the generation and wakes everyone.
- A **subscription** is one listener's private *cursor* onto that source. It
  remembers the generation that listener has already consumed (`seen`), and it
  is the thing the listener actually waits on. Each listener has its own.

The reason it is two objects and not one: readiness and consume state in Linux
live per open file description, and there is no per-listener state on a single
shared fd. Broadcast needs the generation shared but each listener's "what have
I seen" kept separate, and each of those cursors must be its own fd so each
listener can poll it independently. A per-listener, separately-pollable cursor
onto a shared source is, by definition, a second object. (A plain `eventfd` is
one object only because it is single-consumer: its counter and its sole reader's
state are the same thing.)

### The event object

```c
struct event {
        spinlock_t lock;        /* protects subs + dead */
        struct list_head subs;  /* every live subscription hangs here */
        atomic64_t gen;         /* signal count; read locklessly */
        refcount_t refcount;    /* the event fd + each live subscription */
        bool dead;              /* event fd closed: no more signals coming */
};
```

`open("/dev/event")` allocates one of these and stashes it in
`file->private_data`. `gen` starts at 0.

`gen` is an `atomic64`, on purpose: a subscription reading or polling only needs
to compare the source generation against its own `seen`, and it does that with a
lockless `atomic64_read`. So hundreds of listeners can read and poll in parallel
and never touch `event->lock`. The spinlock guards only the rarely-touched
things: the subscription list and the `dead` flag.

### The subscription object

```c
struct subscription {
        struct list_head node;  /* linkage on event->subs */
        struct event *event;    /* the source it belongs to (holds a ref) */
        wait_queue_head_t wqh;  /* this listener parks here */
        u64 seen;               /* generation already consumed; under wqh.lock */
};
```

A subscription has its own wait queue (`wqh`). That is what makes it
independently waitable: a listener blocked in `read()` sleeps on its own `wqh`,
and `poll`/`epoll` register on that same `wqh`. The `seen` field is what makes
the consume independent: only this listener's own reads advance it.

## How they work together

### Subscribing

The listener calls the `SUBSCRIBE` ioctl on the event fd. The kernel allocates a
`struct subscription`, points it at the event, sets `seen` to the event's
*current* generation (so the subscription is edge-from-now: only future signals
count), links it onto `event->subs`, takes a reference on the event, and hands
back a fresh fd (the `anon_inode_getfd` trick, next section).

Sharing across processes is just fd sharing: the publisher opens the event,
`fork`s, and each child calls `SUBSCRIBE` on the inherited event fd to get its
own subscription. One source, many cursors.

### Signaling (the broadcast)

```c
atomic64_inc(&evt->gen);                       /* the signal exists first */
spin_lock(&evt->lock);
list_for_each_entry(sub, &evt->subs, node)
        wake_up_interruptible_poll(&sub->wqh, EPOLLIN);
spin_unlock(&evt->lock);
```

One ioctl, and the kernel walks the list and wakes every subscription. The
publisher never learns how many there are; adding a listener is just another
node on the list. The generation is bumped before any wake, and the wake carries
the memory barrier, so a woken listener is guaranteed to observe the new
generation.

### Reading (per-listener consume)

The subscription is poll-to-wait, read-to-consume: the waiting is done by
`poll`/`epoll` (next section), and `read()` never blocks. It is just a counter
swap:

```c
spin_lock_irq(&sub->wqh.lock);
cur = atomic64_read(&evt->gen);
if (cur == sub->seen && evt->dead) {               /* closed, nothing left */
        unlock; return 0;                          /* 0-byte read == EOF */
}
cnt = cur - sub->seen;                             /* signals since last read */
sub->seen = cur;                                   /* consume them (maybe 0) */
spin_unlock_irq(&sub->wqh.lock);
/* copy the u64 cnt out to userspace, return 8 */
```

`read()` returns the number of signals since this listener last read (possibly
0), and clears its readiness by advancing `seen` to the current generation. The
one reserved case is hangup: if the event has been closed and nothing is
pending, `read()` returns 0 bytes (EOF), which stays distinct from an 8-byte
read whose value happens to be 0 ("I checked, nothing fired"). Two consequences
fall out for free:

- **Coalescing.** Five signals during one stretch of work become one `read()`
  that returns 5, not five wakeups.
- **Never missed.** A signal that fired while the listener was away still leaves
  `gen > seen`, so the next `read()` sees it.

Because each subscription has its own `seen`, every listener gets the full
sequence. That is the broadcast: listener A draining its subscription does not
affect listener B.

`poll` is the lockless mirror of the same check:

```c
poll_wait(file, &sub->wqh, wait);                  /* park the poller on wqh */
return (atomic64_read(&evt->gen) != sub->seen ? EPOLLIN | EPOLLRDNORM : 0)
     | (evt->dead ? EPOLLHUP : 0);
```

`poll_wait` adds the caller (a `poll`/`select` call, or an `epoll` instance) to
the subscription's wait queue, and `signal_event`'s `wake_up_interruptible_poll`
kicks every registered poller, so `epoll` on a subscription works with no extra
machinery. Readiness is level by default (it stays set until `read()` drains it)
and edge under `EPOLLET` (one report per signal). All waiting happens here; the
subsequent `read()` only consumes.

### Lifetime: who frees what

The event is reference counted by its own fd plus every live subscription, so it
outlives whichever closes first.

- A listener closing its subscription runs `subscription_release`: unlink from
  `event->subs` under `event->lock`, drop the event reference (freeing the event
  if that was the last one), free the subscription.
- The publisher closing the event fd runs `event_release`: set `dead`, wake
  every subscription with `EPOLLHUP` so listeners learn the source is gone (a
  poller sees `EPOLLHUP` and the next `read()` returns 0, an EOF), and drop the
  event-fd reference. The event struct stays alive until the last subscription
  also closes.

Closing a subscription cleans up automatically because `close()` on the fd calls
our `.release`. No explicit unsubscribe call is needed, and a listener that
crashes drops its fds and is cleaned up the same way.

### Locking, briefly

Signal and event-close take `event->lock` then a `wqh.lock` (inside the wake).
Subscription `read`/`poll` take only a `wqh.lock`. Subscription release takes
only `event->lock`. Nothing takes them in the opposite nesting, so there is no
deadlock, and a subscription cannot be freed out from under a signal because
release and the wake-walk both serialize on `event->lock`.

## The `anon_inode_getfd` trick

A subscription is not a file under `/dev`. It is minted on demand by an ioctl,
the same way `epoll_create()` mints an epoll fd or `eventfd()` mints an eventfd.
The mechanism is `anon_inode_getfd`:

```c
fd = anon_inode_getfd("[event-sub]", &subscription_fops, sub,
                      O_RDONLY | O_CLOEXEC);
```

What this does, in one call:

1. **Allocates an unused fd number** in the calling process.
2. **Creates a `struct file`** backed by the kernel's single shared *anonymous*
   inode. There is no real inode, no directory entry, no filesystem node; the
   anon inode is a placeholder the kernel keeps for exactly this purpose, so a
   file object can exist without a backing file. That is why it is "anonymous."
3. **Wires our behavior in.** It sets `file->f_op = &subscription_fops` and
   `file->private_data = sub`. From now on every syscall on this fd dispatches
   to our handlers: `read()` calls `subscription_read`, `poll`/`epoll` calls
   `subscription_poll`, `close()` calls `subscription_release`. Each reaches its
   `struct subscription` through `file->private_data`.
4. **Installs the fd** (`fd_install`), making it visible to userspace.

```c
static const struct file_operations subscription_fops = {
        .owner   = THIS_MODULE,
        .read    = subscription_read,
        .poll    = subscription_poll,
        .release = subscription_release,
        .llseek  = noop_llseek,
};
```

The result is an ordinary fd. Userspace `read()`s it, puts it in `epoll`,
`select`s on it, passes it over a Unix socket, and `close()`s it, with no idea it
came from an ioctl rather than `open()`. In `/proc/<pid>/fd` it shows up as
`anon_inode:[event-sub]`. This is the standard kernel pattern for "an fd that is
a pure in-kernel object," used by `eventfd`, `signalfd`, `timerfd`, `epoll`, and
`io_uring`, and it is what lets the broadcast source hand each listener a
first-class pollable cursor without a device node per subscription.

The `.owner = THIS_MODULE` line matters for a module: while any subscription fd
is open the module's reference count is held, so `rmmod` is refused until every
subscription (and event) is closed.

## Example: one epoll over a socket and an event

A listener almost never waits on just the event; it folds the event into the
loop it already runs over its sockets, timers, and so on. Because a subscription
is a normal fd, that is simply adding it to the same `epoll`.

This example uses raw syscalls (no `event.h` helpers): it opens the event,
subscribes, also makes a socket to watch, and waits on both at once.

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* The ABI, copied from module/event_uapi.h (no library used). */
#define EVENT_IOC_MAGIC 'E'
#define EVENT_IOC_SIGNAL    _IO(EVENT_IOC_MAGIC, 2)
#define EVENT_IOC_SUBSCRIBE _IO(EVENT_IOC_MAGIC, 6)

int main(void)
{
        /* The broadcast source. */
        int evt = open("/dev/event", O_RDWR);

        /* Any other pollable fd; a socketpair stands in for a real socket. */
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

        /* This listener's own pollable view of the event. */
        int sub = ioctl(evt, EVENT_IOC_SUBSCRIBE);   /* returns an fd */

        /* One epoll over both the event and the socket. */
        int ep = epoll_create1(0);
        struct epoll_event ev;

        ev.events = EPOLLIN;
        ev.data.fd = sub;
        epoll_ctl(ep, EPOLL_CTL_ADD, sub, &ev);

        ev.events = EPOLLIN;
        ev.data.fd = sv[0];
        epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev);

        /* Stand in for a publisher firing the event and a peer writing data. */
        ioctl(evt, EVENT_IOC_SIGNAL);
        write(sv[1], "hello", 5);

        /* The listener loop: block until either side is ready, in O(ready). */
        for (;;) {
                struct epoll_event out[8];
                int n = epoll_wait(ep, out, 8, -1);

                for (int i = 0; i < n; i++) {
                        if (out[i].data.fd == sub) {
                                uint64_t count;

                                /* 8-byte read: signals since last read. */
                                if (read(sub, &count, sizeof(count)) == 8)
                                        printf("event fired %llu time(s)\n",
                                               (unsigned long long)count);
                        } else if (out[i].data.fd == sv[0]) {
                                char buf[64];
                                ssize_t r = read(sv[0], buf, sizeof(buf));

                                printf("socket: %.*s\n", (int)r, buf);
                        }
                }
        }
}
```

Run it with `event.ko` loaded; it prints `event fired 1 time(s)` and
`socket: hello`, then blocks waiting for the next of either.

The shape generalizes. To wait until **k of n** events are ready, add all n
subscription fds (plus any other fds) to the epoll and accumulate ready ones
across `epoll_wait` calls until you have k, reading each as it arrives. The
kernel only broadcasts; the k-of-n wait and the mixing are plain epoll, which is
the whole point of leaving them in userspace.
