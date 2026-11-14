#!/usr/bin/env python3
"""Boot-state transition matrix over the captured console logs.

Reuses measure_desktop_reach.py's session split and verdicts, so the states here
are exactly the ones the report table uses. Prints, for every log, the ordered
state sequence and the 上一段 -> 下一段 counts, then pools them and tests the
"a failed boot leaves the Data volume in a state that changes the next boot"
hypothesis: P(NO_DRAWABLE | previous SEED) against P(NO_DRAWABLE | previous PASS).

    python boot_state_transitions.py --log <file> [--log <file> ...]

Exit codes: 0 ran; 3 no sessions.
"""
import argparse
import collections
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_desktop_reach as m


def state(row):
    if row["verdict"] == "PASS":
        return "PASS"
    if row["verdict"] == "TRUNC":
        return "TRUNC"
    return row["subtype"].replace("STALL_", "")


def fisher_two_sided(a, b, c, d):
    """p for [[a, b], [c, d]]; a = SEED->NO_DRAWABLE."""
    def hyper(k, n, K, N):
        return math.comb(K, k) * math.comb(N - K, n - k) / math.comb(N, n)
    n1, K, N = a + b, a + c, a + b + c + d
    observed = hyper(a, n1, K, N)
    total = 0.0
    for k in range(0, min(n1, K) + 1):
        p = hyper(k, n1, K, N)
        if p <= observed + 1e-12:
            total += p
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True)
    ap.add_argument("--window", type=float, default=300.0)
    args = ap.parse_args()

    sequences = []
    for path in args.log:
        sessions, lines = m.load_sessions([path])
        if not sessions:
            print("NO_SESSION %s" % path)
            continue
        rows = m.measure(sessions, lines, args.window)
        seq = [state(r) for r in rows]
        sequences.append((os.path.basename(path), seq))
        print("== %s  %d sessions" % (os.path.basename(path), len(seq)))
        print("   " + " -> ".join(seq))
        print("   totals: %s" % dict(collections.Counter(seq)))

    pooled = collections.Counter()
    for _, seq in sequences:
        pooled.update(zip(seq, seq[1:]))

    print("\n== transitions (pooled, %d)" % sum(pooled.values()))
    states = sorted({a for a, _ in pooled} | {b for _, b in pooled})
    width = max(len(s) for s in states) + 2
    print(" " * width + "".join("%-16s" % b for b in states))
    for a in states:
        row = "".join("%-16d" % pooled.get((a, b), 0) for b in states)
        print(("%-*s" % (width, a)) + row)

    seed_total = sum(n for (a, _), n in pooled.items() if a == "SEED")
    seed_nd = pooled.get(("SEED", "NO_DRAWABLE"), 0)
    pass_total = sum(n for (a, _), n in pooled.items() if a == "PASS")
    pass_nd = pooled.get(("PASS", "NO_DRAWABLE"), 0)
    print("\nP(NO_DRAWABLE | previous SEED) = %d/%d" % (seed_nd, seed_total))
    print("P(NO_DRAWABLE | previous PASS) = %d/%d" % (pass_nd, pass_total))
    if seed_total and pass_total:
        p = fisher_two_sided(seed_nd, seed_total - seed_nd,
                             pass_nd, pass_total - pass_nd)
        print("Fisher exact two-sided p = %.3f" % p)
        print("verdict: %s" % ("no evidence of state dependence"
                               if p > 0.05 else "state dependence supported"))
    nd_total = sum(1 for _, s in sequences for x in s if x == "NO_DRAWABLE")
    n_total = sum(len(s) for _, s in sequences)
    print("NO_DRAWABLE overall: %d/%d = %.1f%%" % (nd_total, n_total, 100.0 * nd_total / n_total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
