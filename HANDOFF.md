# Project handoff: the `event` kernel object

This document is the complete written record of everything learned while
building, benchmarking, and iterating on this project. It exists so that a
new person can pick up the work from this exact point with no missing
context. If a fact about this project matters and is not in the code, the
READMEs, or this file, it was lost; nothing was intentionally left oral.

Written at archive time, June 2026. Branch `dev` is the live line; one
experiment is parked on `exp/fast-walk` (see section 7).

---

## 1. What this is

A minimal Linux kernel synchronization object: threads block, consuming no
CPU, until a publisher signals; the signal wakes every current waiter. It is
implemented as an out-of-tree module (`event.ko`) exposing `/dev/event`,
with a header-only userspace API. It began from two paper designs (see
section 11 for their location): the right idea, a subscriber list with
park-then-wake maintained by atomic operations, but with racy pseudo-code.
The project's real product is twofold: a correct, lean, lock-free
implementation, and a benchmarking methodology that can judge any future
iteration definitively.

The intended production profile (the owner's stated use case): 100 to 3000
listeners on a single event, each in an infinite wait, work, re-arm loop,
where the metric that matters most is the time from `signal_event()` until a
listener is running again.

## 2. The final API and semantics

One wait, one signal. Signals are counted in a per-event 64-bit generation
(0 at creation). A wait carries the generation its caller last observed:
if the event has been signaled past it, the wait returns immediately;
otherwise it parks until the next signal or its timeout. The generation is
written back on success. Consequence: a wait/work/re-arm loop observes
every signal no matter how late it re-arms; a burst during one stretch of
work coalesces into one return with the generation jumped by the burst size.

```c
int evt = create_event();                      /* an fd; fork() shares it */
uint64_t gen = 0;
wait_for_event(evt, &gen, EVT_WAIT_FOREVER);   /* park until signaled    */
wait_for_event(evt, &gen, 500);                /* at most 500 ms         */
wait_for_event(evt, &gen, EVT_WAIT_ZERO);      /* poll                   */
signal_event(evt);                             /* wake all, bump gen     */
close_event(evt);
```

Returns: `EVT_SIGNALED` (0, gen updated), `EVT_TIMEOUT` (1), or -1 with
errno (EINTR on an interrupting POSIX signal). Kernel ABI: two ioctls,
`EVENT_IOC_WAIT` with `struct event_wait { u64 gen; s64 timeout_ms; }` and
`EVENT_IOC_SIGNAL` (returns the number of waiters woken; the bench depends
on that return value, keep it). The ABI was deliberately broken twice on the
way here; there are no compatibility shims.

The kernel design (lock-free LIFO with one cmpxchg registration, take-all
claiming via one xchg, three-state ownership handoff, RCU-protected wakes
with no refcounting) and all of its safety arguments are documented in
`code/README.md`, section "Inside the kernel". That section is the
authoritative explanation; this file does not duplicate it.

## 3. Version history and why each step happened

All on branch `dev`, oldest first. The srcversion column is `modinfo -F
srcversion`, useful to verify what is actually loaded on a target
(`cat /sys/module/event/srcversion`).

| Commit | What | Why |
| --- | --- | --- |
| `8a2f3aa` | initial lab scaffolding | |
| `2fe0b98` | v1: spinlock + `list_head`, subscriber on the waiter's stack | first correct implementation; fixed the paper design's three bugs (broken cmpxchg append, lost wakeups, use-after-free in signal) |
| `53d30cb` | benchmark harness, compare tool, runner script | created the methodology before iterating (section 5) |
| `54eef59` | v2: first lock-free (Treiber push, take-all xchg, 3-state handoff, heap nodes, get/put_task_struct per node) | the owner wanted the paper's lock-free intent honored: no locks, atomics only |
| `c1664ea` | v3: RCU-protected wakes replace the per-node refcount pair; empty-signal fast path (plain read); SLAB_HWCACHE_ALIGN nodes | v2 lost large fan-outs badly (signal walk 144us at n=64 vs 16us locked); the refcount pair was the dominant cost and RCU (the rcuwait argument) removes it |
| `f62e590` | generation counter (`EVENT_IOC_WAIT_GEN`) | the edge-triggered wait silently lost signals fired while a listener was busy, the same flaw that got Win32 PulseEvent deprecated; fatal for the owner's loop profile |
| `e943948` | timed waits (`EVENT_IOC_WAIT_EX`) | from the original design doc "event - kernel with timeout.md": ms timeouts, EVT_WAIT_FOREVER/ZERO, EVT_SIGNALED/EVT_TIMEOUT |
| `5cf5023` | `loop` bench scenario | neither `wake` (one-shot fan-out) nor `churn` (signal storm) modeled the real wait/work/re-arm profile; `loop` does, and counts missed signals |
| `713e055` | v4: ABI collapsed to one generation wait (breaking) | with compatibility waived, one wait struct `{gen, timeout_ms}` replaced three ioctls and all flag plumbing |
| `fbabb67` | one `wait_for_event(evt, &gen, timeout_ms)`; code leaned down | the owner wanted a single wait function and lean code: deep docs moved to `code/README.md`, comments kept only where a line cannot speak for itself |
| `99faa22` | standard punctuation throughout | style rule, see section 10 |
| `c0b4b07` (branch `exp/fast-walk`) | v5 experiment: cancellation-free fast signal walks | negative result, preserved off-dev; see section 7 |

## 4. The benchmark methodology (how to judge any future change)

Everything lives in `code/bench/` (full docs in `code/bench/README.md`).
The protocol in one breath: run `scripts/06-bench.sh server` per version,
back to back on a quiet machine, then
`python3 code/bench/compare.py baseline.csv candidate.csv`.

The parts that make a result definitive rather than anecdotal:

- **The futex control.** Every run also benchmarks a futex generation
  counter implementation of the same object, kernel-native code the module
  never touches. Between two runs its numbers must not move; if compare.py
  reports significant futex shifts, the machine changed between runs and
  the event verdicts are void. This fired many times during the project and
  was right every time.
- **Statistical verdicts.** compare.py runs a Mann-Whitney U test per cell
  and declares BETTER/WORSE only at p < 0.01 with a median shift of at
  least 3%; everything else is a tie. Churn needs at least 8 reps for the
  test to have power (the default is 10).
- **Validity gating in the wake scenario.** Rounds only measure if every
  waiter was truly parked first (checked via `/proc/<tid>/stat` state and
  by signal's woken count == N). Failures are counted in `invalid_rounds`;
  nonzero means the implementation has a registration race. Every version
  ever tested scored zero.
- **Attribution.** `scripts/06-bench.sh` labels CSVs with
  `git describe --dirty` and warns when the loaded module's srcversion
  differs from the last-built `.ko` (the classic "benchmarked a stale
  module" mistake; it happened once before the warning existed).

Scenarios and what each regime represents: `wake` (one-shot fan-out:
per-waiter latency, time-to-last-waiter, publisher's signal cost, swept
over -N), `churn` (saturated re-arm throughput), `loop` (the owner's real
profile: -P signal period, -W per-listener work, measures wake latency and
missed signals), `signal0` (signal with no waiters), `open` (create/destroy).

Known limitations of the methodology, learned the hard way:

- The target VM runs on the development host, so host activity pollutes
  benchmarks. The control catches it, but expect to re-run; chain both runs
  in a single command and stay idle.
- Cross-ABI comparisons (different bench binaries per version) invalidate
  the control, because the harness's futex code itself differs between
  bench versions. Within one ABI the control is fully meaningful. If a
  courtroom-grade locked-vs-current comparison is ever needed, teach the
  current bench the old `_IO('E',1)` wait as a compat impl.
- `churn wakes_per_sec` changed meaning at v4: it counts only parked wakes
  (the signal's return value), and gen-aware waiters can consume signals
  without parking. Cross-version churn comparisons spanning v3 to v4+ must
  use the per-waiter `waiter_wakes` (deliveries) instead.
- At large fan-out on the 2-CPU target, the publisher's `signal_call_ns` is
  dominated by scheduler dynamics and is bimodal run to run (observed 2x
  swings on identical code). Treat that cell as untrustworthy on small
  hosts.

## 5. Performance findings (the complete record)

Hardware context for everything below: the `server` target, a QEMU VM on
the dev host, kernel 6.8.0-117-generic, 1 vCPU for early runs and 2 vCPUs
from the mid-project resize onward. All absolute numbers are from that box;
treat them as relative evidence, not portable constants.

**Locked (v1) vs lock-free (v3/v4), the durable picture:**

- Small fan-out wake latency (n=1 to 2): lock-free wins consistently,
  roughly 4.7us vs 10us per-listener at n=1 on 2 CPUs (magnitudes vary with
  noise; the direction never flipped in any valid run).
- Mid fan-out (n=16): the locked version's soft win, lock-free was 20% to
  130% worse depending on the run. Cause: the lock-free walk pays per-node
  work where the locked walk is plain stores under one lock hold.
- Large fan-out (n=64 to 3000): statistical tie. Everything is scheduler
  bound; ~1.8us per listener of pure wake-throughput on 2 CPUs. At n=3000,
  median listener latency ~5.4ms, last listener ~8.8ms, for every
  implementation. Listener latency scales roughly as N divided by cores;
  adding vCPUs is worth more than any further bookkeeping work.
- Saturated churn with many waiters (n=16 to 256): the locked version's one
  real crown, 10% to 18% more park/wake cycles per second (~540K vs ~460K),
  drift-corrected and confirmed repeatedly. Cause analysis in section 7.
  Notably, locked at saturated churn also beats the kernel's own futex
  (~540K vs ~480K), which is why this regime should be considered
  fundamentally lock-friendly on this hardware.
- Churn with few waiters (n=2 to 4): lock-free wins (+9% to +22%), cheaper
  re-arm (1 cmpxchg vs 3 lock round-trips).
- Signal with no waiters and create/destroy: ties (~540ns, ~2.4us).
- v2 to v3 specifically: removing the per-node get/put_task_struct pair
  collapsed the n=64 publisher walk from 144us toward the locked version's
  range; the refcount pair was the single most expensive mistake of the
  project.
- The generation machinery (v3 to v4) measured free: signal0 unchanged
  despite the added atomic64_inc, wake latencies unchanged at the profile
  sizes, no churn change beyond the accounting semantics.
- Delivery throughput under signal storms (v4 semantics): an order of
  magnitude win for the gen design at small N (1.38M vs 130K observed
  events/sec at n=1), because waiters absorb bursts via immediate
  generation catch-ups instead of park/wake cycles. Converges to the same
  ~460K ceiling at high N.
- The `loop` scenario baseline for the owner's profile on v4.1 (2ms period,
  no work, 2 CPUs): mean signal-to-running 23 to 45us at n<=16, ~104us at
  n=64, ~396us at n=256, zero missed signals. With work 5ms against a 2ms
  period, the measured ~60% miss rate matches theory exactly (and with the
  old edge API those would have been silent losses; with gen they are
  visible coalesced deliveries).
- 1-CPU vs 2-CPU lesson: on 1 CPU a spinlock is never contended, so early
  1-CPU results flattered the locked design less than feared and the
  overall shape (small-N lockless win, high-churn locked win, big-N tie)
  survived the move to 2 CPUs. All of it should be re-measured if the
  target ever gets many cores; that is the single biggest open variable.

**Recommendation that followed from the data:** for the owner's profile
(latency-first, 100 to 3000 listeners, re-arm loop) the lock-free
generation design is equal or better everywhere it matters and is the only
design that does not lose signals; the locked design's surviving advantage
(saturated many-waiter churn throughput) is outside that profile.

## 6. How this object relates to real mechanisms

Short version of a longer analysis: the closest real twin is the Win32
Event used with `PulseEvent`, which Microsoft deprecated precisely for the
missed-wakeup hazard this project later fixed with the generation counter.
The futex is the kernel-native equivalent and serves as the bench control;
it wins nothing dramatic against this object, and this object beat it on
median listener latency at n=3000 (5.4ms vs 5.8ms). eventfd is the
fd-counter analogue (level-triggered, pollable, but awkward for wake-all).
Condition variables solve missed signals by re-checking a predicate under a
mutex (the textbook pattern). This object's genuine advantages: a 4-call
API, one syscall per operation, fd lifetime (auto-cleanup on close/exit),
fork-natural sharing, a private waiter list immune to futex hash-bucket
interference, and competitive mass fan-out. Its genuine gaps vs the real
mechanisms: no syscall-free fast path (futex/condvar check userspace state
first), not pollable (cannot sit in an epoll set), and one event per fd.

## 7. Negative results and dead ends (do not re-walk these)

This section is the most valuable Toshba; each item cost real effort.

- **v5, cancellation-free fast walks (branch `exp/fast-walk`, `c0b4b07`).**
  Hypothesis: the per-node cmpxchg in the signal walk is the high-churn
  bottleneck; skip it when no cancellation is outstanding (counter +
  in-flight-walk handshake + RCU-deferred reaping). Implementation is
  correct (survived the full selftest, a 1.4M-signal cancel storm against
  sub-millisecond timeouts, zero invalid rounds, clean dmesg) and the
  protocol is sound; keep it if ever needed. But the verdict was no
  measurable win: churn was bit-for-bit unchanged because the walk is NOT
  the churn bottleneck (the walk cost amortizes over ~50 wakes per signal;
  the cycle is bound by the waiter-side path: slab alloc, contended push,
  scheduling), and at large fan-out the walk's micro-cost drowns in
  scheduler noise (the same cell measured -70% then +133% across two valid
  pairs). Re-evaluate only on a many-core target where the walk's atomics
  cross real cache domains. On dev, v4.1 stays.
- **Stack-allocated nodes (the locked version's trick) are impossible in
  the lock-free design.** A cancelling waiter (timeout/EINTR/gen race) must
  be able to leave while its node is still linked, because a middle node
  cannot be unlinked from a singly-linked list without a lock. Every
  attempt to avoid heap nodes ran into either unbounded waiting on a future
  signal that may never come, or task-lifetime UAF on the wake. Heap nodes
  plus abandonment is the design; do not relitigate without new ideas.
- **Blind-store walks without the cancel handshake are unsound.** A
  re-check on the canceller side alone cannot close the race with an
  in-flight walk whose store lands arbitrarily late; waking an abandoned
  node's task is a use-after-free on the task struct. The walkers-counter
  handshake in v5 is the minimal sound construction found.
- **Per-CPU sublist sharding**: rejected for this target because all
  waiters timeshare one CPU (the publisher hogs the other), so push
  contention is not the limiter. The standard move on a many-core target;
  revisit there.
- **Spin-before-park**: helps latency at low load, actively harmful at
  saturated churn on 2 CPUs (burns the CPU the other waiters need).
- **Lock-free node freelists**: arbitrary-pop Treiber lists have the ABA
  problem; the prize (beating SLUB's per-CPU fastpath) is tens of
  nanoseconds. Not worth it.
- **getting churn parity with the lock at saturation may be impossible
  here**: the queued spinlock batches re-registrations into fuller wake
  sweeps and each cycle uses plain stores on stack nodes. The locked design
  even out-throughputs the futex in this one regime. Treat it as that
  design's legitimate home turf on small SMP.

## 8. Open questions and natural next steps

- **More cores.** Every conclusion above is conditioned on a 1-2 CPU VM.
  On real SMP: lock contention starts costing the locked design, per-CPU
  sharding and the v5 fast walk become plausible wins, and the fan-out
  ceiling rises. The whole comparison should be re-run; it is one
  `scripts/06-bench.sh server` per version.
- **Pollability**: integrating with epoll (a `poll` fop driven by the
  generation) would close the biggest API gap vs eventfd.
- **Wake-N / requeue semantics** (wake exactly one, or requeue to another
  event) if the object ever needs to serve mutex-like patterns.
- **Compat impls in the bench** for cross-ABI comparisons with a valid
  control (section 4).
- **The loop scenario with the owner's real numbers**: `-P` and `-W` were
  never filled with production values; the defaults (2ms period, no work)
  are a synthetic stand-in.

## 9. Operational runbook (the lab)

- Targets are defined in `lab.local.env` (gitignored; `lab.example.env` is
  the template). The working target is `server`: a QEMU VM at
  localhost:2222, user `dor`, kernel 6.8.0-117-generic, currently 2 vCPUs,
  ~2GB RAM, qemu gdbstub on 127.0.0.1:6722. `scripts/00-check-target.sh
  server` verifies everything.
- Pipeline: `00` check, `01` provision, `02` host build setup, `03` build
  module against the cached target headers (`kernel-cache/current ->
  server`), `04` deploy + gdb prep, `05` run the demo under gdbserver,
  `06` bench (build, deploy, run, fetch CSV into `results/`).
- Plain `make` at the repo root builds `event.ko` for the target into
  `build/artifacts/server/<kernel>/`. `MODULE_DIR=/some/dir make` builds an
  out-of-tree source snapshot without touching the tree (used to rebuild
  historical versions; `git show <rev>:code/module/event.c` provides the
  sources).
- The target's staging dir is `/tmp/kmod-debug-lab-dor`; it is wiped on VM
  reboot (this bit once: a reboot for the CPU resize deleted all staged
  binaries). Re-upload after reboots.
- `lab_ssh_sudo "a && b"` runs only `a` under sudo; wrap compound commands:
  `lab_ssh_sudo "sh -c 'a && b'"` (this bit once: rmmod succeeded, insmod
  ran unprivileged).
- Always verify which module is loaded before benchmarking:
  `cat /sys/module/event/srcversion` vs `modinfo -F srcversion <ko>`.
- Functional tests live in `code/test/`: `selftest.c` (park/wake, missed
  signal immediacy, burst coalescing, timeout accuracy, poll,
  signal-beats-deadline, EINTR, abandoned-node reaping) and
  `cancel-storm.c` (timeouts racing a signal storm; exercises every
  cancellation path). Build with `make -C code/test`, copy to the target,
  run, then check `dmesg` for WARNs. They were the project's regression
  gate after every kernel change.
- `results/` is gitignored. Inventory at archive time: `full-locked/-v3/
  -v41.csv` (full sweeps, cross-ABI so control-weak), `proto-v41/-v5.csv`
  and `wake-v41/-v5.csv` (the v5 protocol runs), `b2b-*.csv` (the locked vs
  v2 vs v3 back-to-back session), `bigfan-*.csv` (100/1024/3000 listeners),
  `churn-*.csv`. CSVs label their rows; `# kernel ...` header lines record
  the CPU count.

## 10. Conventions for future work

- Punctuation: no double-dash or em-dash asides anywhere (code comments,
  docs, commit messages, output strings); commas, colons, semicolons.
- Code stays lean: comments only where a line cannot speak for itself;
  design essays and safety arguments belong in `code/README.md`.
- Every kernel change: build, run `code/test/` on the target, check dmesg,
  then the bench protocol before claiming a performance result.
- Performance claims require the protocol's verdict (control clean,
  p < 0.01, >= 3% median shift); everything else is called a tie.
- Iterations that lose on the bench are preserved on `exp/*` branches with
  the negative result in the commit message, not merged and not deleted.

## 11. External artifacts (outside this repo)

- The original paper designs live in the owner's `~/Documents/`:
  `event - kernel.md` (the base design whose three bugs v1 fixed),
  `event - kernel with timeout.md` (the timeout API contract implemented in
  `e943948`), `event - usemode.md` (never used in this project).
- The Claude Code session memory for this repo stores the two style rules
  (punctuation, lean docs); they are duplicated in section 10 so this file
  stands alone.
