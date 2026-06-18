# `bench`: measuring the eventfd-backed event

This directory measures the event's wait/wake performance. On this branch an
event is just an `eventfd` (see `../lib/event.h`), so there is no implementation
to iterate on; the harness instead pins down the event's numbers and compares
them to a per-waiter `futex` control, the kernel-native primitive doing the same
single-consumer job. It runs anywhere, with nothing to build or load beyond the
benchmark binary.

The model is one waitable object per waiter (the object is single-consumer); the
publisher signals each. Each waiter blocks in a plain `read()` (the most direct
wake-latency measure); `poll`/`epoll` are equally valid and are exercised by the
functional tests.

## Running it

```bash
make                       # builds ./bench (needs only ../lib/event.h + -pthread)
./bench -h                 # all knobs
./bench                    # event vs futex, full sweep
./bench -c out.csv -l run1 # raw samples to out.csv, tagged run1
```

To compare two runs (for example the same binary on a quiet vs busy machine, or
before vs after a kernel change), capture two CSVs and run the verdict tool:

```bash
./bench -c a.csv -l a
./bench -c b.csv -l b
python3 compare.py a.csv b.csv
```

Rules that make the result meaningful:

1. **Same machine, back to back, otherwise idle.** Absolute numbers from
   different machines (or the same VM on different days) are not comparable.
2. **Trust the control.** The `futex` rows are kernel-native code doing the same
   job. If `compare.py` reports them shifted significantly between two runs, the
   machine conditions changed and any `event` difference is void. Re-run.
3. **A difference is real only if the test says so.** `compare.py` runs a
   Mann-Whitney U test per cell and only prints BETTER/WORSE when p < 0.01
   *and* the median moved >= 3%. Everything else is `~`; treat it as a tie.

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
| `loop` | `missed_signals` | signals that fired while a waiter was working, coalesced into another read rather than delivered one-for-one (signals-sent minus wait-returns) | waiters running behind the publisher |
| `signal0` | `signal0_ns` | signal cost with nobody reading | events that are mostly idle |
| `open` | `open_close_ns` | create + destroy cost of one object | short-lived events created per request |

The sweep (`-N`) shows where any crossover between `event` and `futex` sits.

## How the harness keeps rounds honest

A `wake` round only measures wake-up latency if every waiter is actually parked
before the signal; a waiter that returned instantly measures nothing. Each round
therefore:

1. waits for every waiter to announce readiness (userspace atomic),
2. polls `/proc/self/task/<tid>/stat` until every waiter thread is in state `S`
   (enqueued in its wait path: eventfd and futex both enqueue before marking the
   task sleeping),
3. timestamps, signals all n objects, and waits for all n to report woken under
   a stuck-timeout. A lost wakeup trips the timeout and aborts loudly rather
   than polluting the data.

Other choices worth knowing:

- One waitable object per waiter, because the object is single-consumer; the
  publisher signals each. `signal_call_ns` is the cost of signaling all n.
- Waiters are threads, not forked processes (the demo uses fork). The kernel
  wake path is identical (waking a task blocked on a wait queue); threads just
  make cross-waiter timestamps and barriers cheap and exact.
- Timestamps are `CLOCK_MONOTONIC`; `wake_ns` includes the signal's own entry
  cost; that is deliberate: it is the latency the *system* delivers.
- The first `max(3, rounds/10)` rounds per combination are warmup and discarded.
- In `churn`, throughput is counted waiter-side as the sum of actual wait
  returns over the window.
- `-p <cpu>` pins the publisher for steadier `signal_call_ns`; use it
  consistently on both sides of a comparison or not at all.
