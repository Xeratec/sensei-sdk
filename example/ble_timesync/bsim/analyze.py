#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Node-to-node time error of the PPS outputs of a BabbleSim run.

Input: the files written by the GPIO model (option -gpio_out_file) of the master and of the followers. Format:
``time(microsecond),port,pin,level``. The time is the time of the device, not the time of the radio simulator (the
HW model logs nsi_hws_get_time(), and the XO model of -xo_drift makes it run at (1 + drift) times the simulator time:
dev_time = (1 + drift) * phy_time). The files are converted to the time of the simulator (true time) with the drift
that was set for each device:  true = dev / (1 + drift).

Every edge of a follower is paired with the master edge of the same level that is nearest in true time (the edges are
500 ms apart, so this is unambiguous). Printed per follower, in ns, over the edges after --skip seconds: count, mean,
std, min, max of (follower - master). The log has 1 us resolution, so each difference has a quantization noise of up
to +-1 us (std about 0.4 us) that is part of the numbers.

Exit code 1 if a follower produced no edges, if the largest |difference| exceeds --threshold-us (default 5), if a follower
edge after --skip has no master edge within --pair-window-ms (a stray edge), or if the follower covers less than
--min-coverage (default 0.95) of the master edges after --skip.
"""

import argparse
import bisect
import statistics
import sys


def read_edges(path, port, pin):
    """Return [(dev_time_us, level)] of the pin."""
    edges = []
    with open(path) as f:
        next(f, None)  # header
        for line in f:
            t, p, n, lvl = line.strip().split(",")
            if int(p) == port and int(n) == pin:
                edges.append((int(t), int(lvl)))
    return edges


def to_true_ns(edges, drift):
    return [(t / (1.0 + drift) * 1000.0, lvl) for t, lvl in edges]


def main():
    ap = argparse.ArgumentParser(description = __doc__, formatter_class = argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--master", required = True, help = "GPIO log of the master")
    ap.add_argument("--master-drift-ppm", type = float, default = 0.0)
    ap.add_argument("--follower",
                    action = "append",
                    required = True,
                    metavar = "NAME:FILE:DRIFT_PPM",
                    help = "GPIO log and XO drift (ppm) of a follower; repeat")
    ap.add_argument("--port", type = int, default = 3, help = "GPIO model port of P1 of the application core (P0: 2)")
    ap.add_argument("--pin", type = int, default = 6)
    ap.add_argument("--skip", type = float, default = 20.0, help = "seconds of true time before the statistics start")
    ap.add_argument("--threshold-us", type = float, default = 5.0)
    ap.add_argument("--pair-window-ms", type = float, default = 5.0)
    ap.add_argument("--min-coverage", type = float, default = 0.95)
    ap.add_argument("--rising-only",
                    action = "store_true",
                    help = "pulse mode: compare only the rising edges (and report the pulse width of the master)")
    ap.add_argument("--csv", help = "write all pairs (follower, true master time s, difference ns) here")
    a = ap.parse_args()

    master = to_true_ns(read_edges(a.master, a.port, a.pin), a.master_drift_ppm * 1e-6)
    if a.rising_only:
        widths = [
            master[i + 1][0] - master[i][0]
            for i in range(len(master) - 1)
            if master[i][1] == 1 and master[i + 1][1] == 0
        ]
        if widths:
            print(f"master pulse width ns: count {len(widths)}  min {min(widths):.0f}  max {max(widths):.0f}")
        master = [e for e in master if e[1] == 1]
    skip_ns = a.skip * 1e9
    m_after = [e for e in master if e[0] >= skip_ns]
    m_last = master[-1][0] if master else 0.0
    m_times = {0: [t for t, lvl in master if lvl == 0], 1: [t for t, lvl in master if lvl == 1]}
    print(f"master: {len(master)} edges, {len(m_after)} after {a.skip:g} s")
    if not m_after:
        print("FAIL: the master produced no edges after the skip time")
        return 1

    fail = False
    rows = []
    for spec in a.follower:
        name, path, ppm = spec.split(":")
        fol = to_true_ns(read_edges(path, a.port, a.pin), float(ppm) * 1e-6)
        if a.rising_only:
            fol = [e for e in fol if e[1] == 1]
        diffs, stray, matched_master = [], 0, set()
        for t, lvl in fol:
            if t < skip_ns or t > m_last + a.pair_window_ms * 1e6:
                continue  # before convergence, or after the last master edge (the simulation ended)
            ts = m_times[lvl]
            i = bisect.bisect_left(ts, t)
            cand = [ts[j] for j in (i - 1, i) if 0 <= j < len(ts)]
            best = min(cand, key = lambda x: abs(x - t)) if cand else None
            if best is None or abs(best - t) > a.pair_window_ms * 1e6:
                stray += 1
                continue
            diffs.append(t - best)
            matched_master.add((best, lvl))
            rows.append((name, best / 1e9, t - best))
        cov = len(matched_master) / len(m_after)
        print(f"{name} (drift {ppm} ppm): {len(fol)} edges, {len(diffs)} paired after skip, "
              f"stray {stray}, coverage {cov:.3f}")
        if not diffs:
            print(f"  FAIL: {name} produced no paired edges")
            fail = True
            continue
        mean = statistics.fmean(diffs)
        std = statistics.pstdev(diffs)
        worst = max(abs(d) for d in diffs)
        print(
            f"  error ns: count {len(diffs)}  mean {mean:+.0f}  std {std:.0f}  min {min(diffs):+.0f}  max {max(diffs):+.0f}"
        )
        if worst > a.threshold_us * 1e3:
            print(f"  FAIL: |max| {worst:.0f} ns exceeds {a.threshold_us:g} us")
            fail = True
        if stray:
            print(f"  FAIL: {stray} edges without a master edge within {a.pair_window_ms:g} ms")
            fail = True
        if cov < a.min_coverage:
            print(f"  FAIL: coverage {cov:.3f} below {a.min_coverage}")
            fail = True
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("follower,master_true_time_s,error_ns\n")
            for r in rows:
                f.write(f"{r[0]},{r[1]:.6f},{r[2]:.0f}\n")
    print("RESULT:", "FAIL" if fail else "PASS")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
