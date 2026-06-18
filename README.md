# event: a pollable broadcast event for Linux

A small out-of-tree Linux kernel module (`event.ko`) that provides a **broadcast
event**: one publisher signals with a single call, and many independent
listeners each see every signal, waiting with the kernel's own
`poll`/`epoll`/`select`.

It fills a gap between two standard primitives: `eventfd` is pollable but
single-consumer (the first reader drains it), and `futex` can wake many waiters
but is not pollable (so it cannot share a wait with sockets and timers). `event`
is both: a broadcast source whose listeners are ordinary pollable fds.

```
publisher:   signal_event(evt);                 // one call wakes every listener
listener:    int sub = subscribe_event(evt);    // a pollable fd of your own
             /* add sub to your epoll alongside sockets, timerfd, ... */
             /* on wake: event_read(sub, &count)  -> signals since last read */
```

A listener can wait on many events at once and wake when the first `k` of `n`
are ready, in `O(ready)`, because the `k`-of-`n` logic is just `epoll` in
userspace; the kernel only broadcasts.

## Quick start

```bash
# build and load the module (needs linux-headers-$(uname -r))
cd code/module && make && sudo insmod event.ko    # creates /dev/event, mode 0666

# run the demo: one signal wakes many listeners, then a first-k-of-n wait
cd ../example && make && ./demo 5

# functional tests and the ABI drift guard
cd ../test && make && ./selftest && ./broadcast-storm

# unload when done
sudo rmmod event
```

## Documentation

- [`code/README.md`](code/README.md): full API, semantics, the kernel design and
  its safety argument, requirements, build/run.
- [`code/MECHANISM.md`](code/MECHANISM.md): how it works end to end (the two
  objects, the `anon_inode` subscription trick, a worked epoll example mixing a
  socket and an event).
- [`code/bench/README.md`](code/bench/README.md): the benchmark and the A/B
  comparison methodology (`event` vs a futex broadcast control).

## Repository layout

```
code/        the project
  module/      event.c, the kernel module -> event.ko (/dev/event)
  lib/         event.h, the header-only userspace API
  example/     demo.c, a runnable broadcast + wait-first demo
  test/        selftest, broadcast-storm, abi-check
  bench/       bench.c + compare.py, the performance yardstick
scripts/, Makefile, .vscode/, lab.example.env
             optional remote-VM dev lab (see code/README.md); not needed to
             build, run, or test the project
```

## License

MIT. See [LICENSE](LICENSE).
