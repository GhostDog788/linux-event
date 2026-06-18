# `bench`: measuring event implementations against each other

`event.ko` will go through many implementation iterations. This directory is
the fixed yardstick they are all measured with: a benchmark binary (`bench.c`)
that produces raw samples, and a verdict tool (`compare.py`) that says, with
statistical backing, whether iteration B beats iteration A, **and in which
regime**.

The object is single-consumer (eventfd-shaped), so the harness models one
waitable object per waiter; the publisher signals each. Each waiter blocks in a
plain `read()` (the most direct wake-latency measure); `poll`/`epoll` are
equally valid ways to wait and are exercised by the functional tests.

## The protocol

One iteration comparison = two bench runs + one compare:

```bash
# 1. baseline: build + load the current event.ko, run the bench
scripts/03-build-module.sh server
scripts/04-deploy-and-prep-gdb.sh server <method>     # (re)loads event.ko
scripts/06-bench.sh server                            # -> results/bench-server-<revA>-<ts>.csv

# 2. candidate: edit event.c, commit (or at least let it show as -dirty),
#    rebuild + RELOAD the module, run the bench again
scripts/03-build-module.sh server
scripts/04-deploy-and-prep-gdb.sh server <method>
scripts/06-bench.sh server                            # -> results/bench-server-<revB>-<ts>.csv

# 3. the verdict
python3 code/bench/compare.py results/bench-server-<revA>-*.csv \
                              results/bench-server-<revB>-*.csv
```

Rules that make the result definitive rather than anecdotal:

1. **Same machine, back to back, otherwise idle.** Absolute numbers from
   different machines (or the same VM on different days) are not comparable.
2. **Trust the controls.** Every run also measures the kernel's own `eventfd`
   (the same shape as our object, an ioctl signal aside) and a per-waiter
   `futex`, kernel-native code our module never touches. `event` should track
   `eventfd` closely; if `compare.py` reports a control row shifted
   significantly, the machine conditions changed between runs and the event
   verdicts are void. Re-run both sides.
3. **A difference is real only if the test says so.** `compare.py` runs a
   Mann-Whitney U test per cell and only prints BETTER/WORSE when p < 0.01
   *and* the median moved >= 3%. Everything else is `~`; treat it as a tie.
4. **`scripts/06-bench.sh` labels every CSV with `git describe --dirty`** and
   warns if the module loaded on the target is not the one last built, so a
   result file is always attributable to one implementation.

## What is measured, and which regime each number represents

| Scenario | Metric | What it tells you | Matters when… |
| --- | --- | --- | --- |
| `wake` | `wake_ns` p50/p99 | latency from "publisher signals this object" to "this waiter is running" | latency-sensitive waiters; **n=1 is the purest single-wake number** |
| `wake` | `last_wake_ns` | time until the *slowest* of n waiters is running | the publisher fans out to many waiters; exposes scheduler pile-up |
| `wake` | `signal_call_ns` | publisher time to signal all n objects (the per-waiter fan-out cost) | the publisher has many waiters to notify; exposes per-signal overhead × n |
| `churn` | `wakes_per_sec` | sustained throughput, counted waiter-side (actual wait returns), waiters re-arming flat out | high event rates; exposes signal/wait overhead and contention |
| `churn` | `signal_calls_per_sec` | full sweeps of all n objects per second on the publisher | publisher-side cost of fanning out |
| `churn` | `waiter_wakes` | per-waiter wake counts | fairness: one starved waiter shows up as a low outlier |
| `loop` | `loop_wake_ns` p50/p99 | most-recent-signal→waiter-running latency in the realistic wait→work→re-arm loop (publisher signals every `-P` µs, each waiter simulates `-W` µs of work) | **the production shape**; set `-P`/`-W` to your real workload's numbers |
| `loop` | `missed_signals` | signals that fired while a waiter was working, coalesced into another read rather than delivered one-for-one (counted as signals-sent minus wait-returns) | waiters running behind the publisher; an edge-triggered design without `read` draining would lose these |
| `signal0` | `signal0_ns` | signal cost with nobody reading | events that are mostly idle |
| `open` | `open_close_ns` | create + destroy cost of one object | short-lived events created per request |

So "which implementation is better **and when**" reads directly off the compare
table; the sweep (`-N`) shows where any crossover sits.

## How the harness keeps rounds honest

A `wake` round only measures wake-up latency if every waiter is actually parked
before the signal; a waiter that returned instantly measures nothing. Each round
therefore:

1. waits for every waiter to announce readiness (userspace atomic),
2. polls `/proc/self/task/<tid>/stat` until every waiter thread is in state `S`;
   by that point it is enqueued in its wait path (event, eventfd and futex all
   enqueue before marking the task sleeping),
3. timestamps, signals all n objects, and waits for all n to report woken under
   a stuck-timeout. A lost wakeup trips the timeout and aborts loudly rather
   than polluting the data.

Other choices worth knowing:

- One waitable object per waiter, because the object is single-consumer; the
  publisher signals each. `signal_call_ns` is therefore the cost of signaling
  all n, not one broadcast.
- Waiters are threads, not forked processes (the demo uses fork). The kernel
  wake path is identical (waking a task blocked on a wait queue); threads just
  make cross-waiter timestamps and barriers cheap and exact.
- Timestamps are `CLOCK_MONOTONIC`; `wake_ns` includes the signal's own entry
  cost; that is deliberate: it is the latency the *system* delivers from the
  publisher's decision to the waiter running.
- The first `max(3, rounds/10)` rounds per combination are warmup and discarded.
- In `churn`, throughput is counted waiter-side as the sum of actual wait
  returns over the window. Compare `wakes_per_sec` across implementations; treat
  `waiter_wakes` as a fairness signal within one implementation.
- `-p <cpu>` pins the publisher for steadier `signal_call_ns` numbers; use it
  consistently on both sides of a comparison or not at all.

## Running it by hand

```bash
make                       # builds ./bench (needs only ../lib/event.h + -pthread)
./bench -h                 # all knobs
./bench -i eventfd,futex   # harness self-test: runs anywhere, no module needed
./bench -c out.csv -l v6   # full sweep, raw samples to out.csv
```

A full default sweep (6 waiter counts × {wake, churn, loop} × 3 implementations
+ the batched micro-benchmarks) takes a few minutes, dominated by churn/loop
(`-d` seconds × `-R` reps per cell). For a quick smoke signal while hacking:
`./bench -s wake -N 1,64 -r 50`.
