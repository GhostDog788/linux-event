#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""compare.py - decide which of two bench runs is better, and where.

    python3 compare.py baseline.csv candidate.csv

Both files are raw-sample CSVs produced by `bench -c` (one row per sample:
label,scenario,impl,waiters,round,metric,value). For every
(scenario, impl, waiters, metric) present in both runs the script prints
medians, p99s, the relative delta, and a verdict.

The verdict is statistical, not eyeballed: a two-sided Mann-Whitney U test
(normal approximation with tie correction) on the raw samples. A cell is
called BETTER/WORSE only when p < ALPHA *and* the median moved by more than
MIN_EFFECT, otherwise it is '~' (no meaningful difference). This is what
makes the comparison definitive: run-to-run scheduler noise fails the test,
real regressions do not.

The futex rows are the control. The same kernel primitive ran in both
sessions, so its numbers must NOT move: any significant futex delta means the
machine conditions differed between the runs (load, frequency scaling, ...)
and the event-vs-event verdicts on this pair of files should not be trusted.
The script checks this automatically and warns.
"""

import csv
import math
import sys
from collections import defaultdict

ALPHA = 0.01        # significance level for the Mann-Whitney test
MIN_EFFECT = 0.03   # ignore median shifts under 3% even if "significant"

# Direction of "better" per metric. Metrics absent here (e.g. the churn
# diagnostics) are informational: printed, never given a verdict.
LOWER_IS_BETTER = {"wake_ns", "last_wake_ns", "signal_call_ns",
                   "signal0_ns", "open_close_ns", "loop_wake_ns",
                   "missed_signals"}
HIGHER_IS_BETTER = {"wakes_per_sec"}


def load(path):
    """-> {(scenario, impl, waiters, metric): [values]}"""
    out = defaultdict(list)
    with open(path) as f:
        for row in csv.reader(line for line in f if not line.startswith("#")):
            if not row or row[0] == "label":
                continue
            _, scenario, impl, waiters, _, metric, value = row
            out[(scenario, impl, int(waiters), metric)].append(float(value))
    if not out:
        sys.exit(f"{path}: no samples found (was it written by bench -c?)")
    return out


def percentile(sorted_vals, p):
    idx = min(int(p / 100.0 * len(sorted_vals)), len(sorted_vals) - 1)
    return sorted_vals[idx]


def mann_whitney_p(a, b):
    """Two-sided p-value, normal approximation with tie correction.

    Good for the sample sizes bench produces (>= ~20 per cell); for tiny
    cells the approximation is rough, so we simply refuse (return None).
    """
    n1, n2 = len(a), len(b)
    if n1 < 8 or n2 < 8:
        return None
    pooled = sorted([(v, 0) for v in a] + [(v, 1) for v in b])
    n = n1 + n2

    # Average ranks over ties, and collect tie-group sizes for the
    # variance correction.
    ranks = [0.0] * n
    tie_term = 0.0
    i = 0
    while i < n:
        j = i
        while j + 1 < n and pooled[j + 1][0] == pooled[i][0]:
            j += 1
        rank = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[k] = rank
        t = j - i + 1
        tie_term += t ** 3 - t
        i = j + 1

    r1 = sum(r for r, (_, grp) in zip(ranks, pooled) if grp == 0)
    u1 = r1 - n1 * (n1 + 1) / 2.0
    mu = n1 * n2 / 2.0
    var = (n1 * n2 / 12.0) * ((n + 1) - tie_term / (n * (n - 1)))
    if var <= 0:           # all values identical
        return 1.0
    z = (u1 - mu) / math.sqrt(var)
    return math.erfc(abs(z) / math.sqrt(2.0))


def fmt(metric, v):
    if metric.endswith("_ns"):
        return f"{v / 1e3:10.2f}us" if v >= 10_000 else f"{v:10.1f}ns"
    if metric.endswith("per_sec"):
        return f"{v:11.0f}/s"
    return f"{v:12.1f}"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base_path, cand_path = sys.argv[1], sys.argv[2]
    base, cand = load(base_path), load(cand_path)

    keys = sorted(set(base) & set(cand))
    if not keys:
        sys.exit("the two files share no (scenario, impl, waiters, metric) "
                 "cells, were they produced with the same bench options?")
    only = sorted(set(base) ^ set(cand))
    if only:
        print(f"note: {len(only)} cell(s) present in only one file are "
              f"skipped (different -s/-N/-i options?)\n")

    header = (f"{'scenario':8} {'impl':6} {'n':>4} {'metric':>20} | "
              f"{'base p50':>12} {'cand p50':>12} {'delta':>8} | "
              f"{'base p99':>12} {'cand p99':>12} | verdict")
    print(f"baseline:  {base_path}\ncandidate: {cand_path}\n")
    print(header)
    print("-" * len(header))

    control_shifted = []
    for key in keys:
        scenario, impl, waiters, metric = key
        if metric == "invalid_rounds":  # reported in the warning block below
            continue
        a, b = sorted(base[key]), sorted(cand[key])
        a50, b50 = percentile(a, 50), percentile(b, 50)
        a99, b99 = percentile(a, 99), percentile(b, 99)
        delta = (b50 - a50) / a50 if a50 else 0.0
        p = mann_whitney_p(a, b)

        if metric in LOWER_IS_BETTER:
            improved = delta < 0
        elif metric in HIGHER_IS_BETTER:
            improved = delta > 0
        else:
            improved = None

        if improved is None:
            verdict = "(info)"
        elif p is None:
            verdict = "too few samples"
        elif p >= ALPHA or abs(delta) < MIN_EFFECT:
            verdict = "~ no significant change"
        else:
            verdict = (f"{'BETTER' if improved else 'WORSE'} "
                       f"(p={p:.1e})")
            if impl == "futex":
                control_shifted.append(key)

        print(f"{scenario:8} {impl:6} {waiters:>4} {metric:>20} | "
              f"{fmt(metric, a50)} {fmt(metric, b50)} {delta:+7.1%} | "
              f"{fmt(metric, a99)} {fmt(metric, b99)} | {verdict}")

    # Correctness backstop: any invalid wake rounds in either run.
    for name, data in (("baseline", base), ("candidate", cand)):
        bad = {k: v for k, v in data.items()
               if k[3] == "invalid_rounds" and sum(v) > 0}
        for (scenario, impl, waiters, _), v in sorted(bad.items()):
            print(f"\nWARNING: {name} had {int(sum(v))} invalid {scenario} "
                  f"round(s) for {impl} n={waiters}, the implementation "
                  f"woke fewer waiters than were parked (registration race).")

    if control_shifted:
        print("\n" + "!" * 72)
        print("WARNING: the futex CONTROL moved significantly in "
              f"{len(control_shifted)} cell(s):")
        for scenario, impl, waiters, metric in control_shifted:
            print(f"  {scenario} n={waiters} {metric}")
        print("The same kernel code ran in both sessions, so this means the\n"
              "machine conditions differed (load, governor, VM neighbors).\n"
              "Do NOT trust the event verdicts above; re-run both sessions\n"
              "back-to-back on a quiet machine.")
        print("!" * 72)


if __name__ == "__main__":
    main()
