# `bench`: measuring event implementations against each other

`event.ko` will go through many implementation iterations. This directory is
the fixed yardstick they are all measured with: a benchmark binary (`bench.c`)
that produces raw samples, and a verdict tool (`compare.py`) that says, with
statistical backing, whether iteration B beats iteration A, **and in which
regime**.

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
2. **Trust the control.** Every run also measures a *futex generation-counter*
   implementation of the same object, kernel-native code our module never
   touches. If `compare.py` reports the futex rows shifted significantly, the
   machine conditions changed between runs and the event verdicts are void.
   Re-run both sides. (This is automatic; the script prints a loud warning.)
3. **A difference is real only if the test says so.** `compare.py` runs a
   Mann-Whitney U test per cell and only prints BETTER/WORSE when p < 0.01
   *and* the median moved ≥ 3%. Everything else is `~`; treat it as a tie.
4. **Watch `invalid_rounds`.** The harness independently verifies that every
   signal woke exactly the number of waiters that were parked. A nonzero
   count means the candidate has a registration race, a correctness bug.
   No performance number excuses that.
5. **`scripts/06-bench.sh` labels every CSV with `git describe --dirty`** and
   warns if the module loaded on the target is not the one last built, so a
   result file is always attributable to one implementation.

## What is measured, and which regime each number represents

| Scenario | Metric | What it tells you | Matters when… |
| --- | --- | --- | --- |
| `wake` | `wake_ns` p50/p99 | latency from "publisher calls signal" to "this waiter is running" | latency-sensitive waiters; **n=1 is the purest single-wake number** |
| `wake` | `last_wake_ns` | time until the *slowest* of n waiters is running (fan-out completion) | broadcast to many subscribers; exposes the cost of the wake-walk and scheduler pile-up at large n |
| `wake` | `signal_call_ns` | how long the publisher itself is stuck in `signal_event()` | the publisher has other work to do; exposes O(n) walking under the lock |
| `churn` | `wakes_per_sec` | sustained throughput, counted subscriber-side (actual wait returns), with subscribers re-arming flat out | high event rates; exposes signal/wait overhead and contention |
| `churn` | `signal_calls_per_sec` | broadcast signals per second on the publisher | publisher-side cost of one fan-out call |
| `churn` | `waiter_wakes` | per-subscriber wake counts | fairness: one starved subscriber shows up as a low outlier |
| `loop` | `loop_wake_ns` p50/p99 | most-recent-signal→subscriber-running latency in the realistic wait→work→re-arm loop (publisher signals every `-P` µs, each subscriber simulates `-W` µs of work) | **the production shape for "many listeners on one event"**; set `-P`/`-W` to your real workload's numbers |
| `loop` | `missed_signals` | signals that fired while a subscriber was working, coalesced into another read rather than delivered one-for-one (signals-sent minus wait-returns) | subscribers running behind the publisher; an edge-triggered design without `read` draining would lose these |
| `signal0` | `signal0_ns` | signal cost with zero subscribers | events that are mostly idle ("publish and nobody listens") |
| `open` | `open_close_ns` | create + destroy cost | short-lived events created per request |

So "which implementation is better **and when**" reads directly off the
compare table: an iteration might win `wake n=1` (cheaper single wake) but
lose `signal_call_ns n=256` (worse fan-out) and `churn wakes_per_sec`
(more lock contention); pick by the regime your workload lives in, and the
sweep (`-N`) shows where the crossover sits.

## How the harness keeps rounds honest

A `wake` round only measures wake-up latency if every waiter is actually
parked in the kernel before the signal; a waiter that returned instantly
off the generation counter measures nothing, and a naive bench would race
and silently mix the two. Each round therefore:

1. waits for every waiter to announce readiness (userspace atomic),
2. polls `/proc/self/task/<tid>/stat` until every waiter thread is in state
   `S`; by that point it is registered, because both event.ko and futex
   enqueue *before* marking the task sleeping,
3. timestamps, signals once, and **checks the signal's return value
   (subscriptions notified) equals n**. A short round is released, discarded,
   and counted in `invalid_rounds`.

Step 3 relies on the uapi contract that `EVENT_IOC_SIGNAL` returns the number
of subscriptions notified (all of which are parked once the round has gated on
step 2); keep that contract or teach the harness otherwise.

Other choices worth knowing:

- Each subscriber thread owns one subscription to the single shared event and
  blocks in `read()`; one `signal_event()` wakes them all. Subscribers are
  threads, not forked processes (the demo uses fork). The kernel wake path is
  identical (waking tasks on a wait queue); threads just make cross-subscriber
  timestamps and round barriers cheap and exact.
- Timestamps are `CLOCK_MONOTONIC`; `wake_ns` includes the signal ioctl's own
  entry cost; that is deliberate: it is the latency the *system* delivers
  from the publisher's decision to the waiter running.
- The first `max(3, rounds/10)` rounds per combination are warmup and
  discarded.
- In `churn`, throughput is counted subscriber-side as the sum of actual wait
  returns over the window. Compare `wakes_per_sec` across implementations;
  treat `waiter_wakes` as a fairness signal within one implementation.
- `-p <cpu>` pins the publisher for steadier `signal_call_ns` numbers; use it
  consistently on both sides of a comparison or not at all.

## Running it by hand

```bash
make                       # builds ./bench (needs only ../lib/event.h + -pthread)
./bench -h                 # all knobs
./bench -i futex           # harness self-test: runs anywhere, no module needed
./bench -c out.csv -l v2   # full sweep, raw samples to out.csv
```

A full default sweep (6 waiter counts × {wake, churn} × 2 implementations +
the batched micro-benchmarks) takes roughly 3–4 minutes, dominated by churn
(`-d` seconds × `-R` reps per cell). For a quick smoke signal while hacking:
`./bench -s wake -N 1,64 -r 50`.
