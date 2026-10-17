"""Compare repeat phase means within each scene, including winter without object snow.

Repeats have equal weight. Reported p95 is a mean of window percentiles, not the
pooled percentile. Frame-span deltas are meaningful only when not limited by vsync.
"""
import csv
import math
from pathlib import Path
import re
import sys
from collections import defaultdict

MODES = ("off", "on", "winter", "no-tops")
PHASE = re.compile(r"^(.+)-(off|on|winter|no-tops)(?:-\d+)?$")


def mean(items, key):
    values = [float(r[key]) for r in items if r.get(key) not in (None, "", "None")]
    values = [v for v in values if math.isfinite(v)]
    return sum(values) / len(values) if values else float("nan")


def compare(rows):
    groups = defaultdict(lambda: defaultdict(list))
    for row in rows:
        phase = PHASE.match(row["phase"])
        if phase and float(row.get("frames") or 0) > 0:
            groups[phase[1]][phase[2]].append(row)
    output = []
    for scene, modes in groups.items():
        off = mean(modes.get("off", []), "span")
        for mode in MODES:
            items = modes.get(mode, [])
            if not items:
                continue
            span = mean(items, "span")
            limited = any(int(r.get("vsync_windows") or 0) > 0 for r in items + modes.get("off", []))
            cost = span - off if not limited else float("nan")
            output.append(dict(scene=scene, mode=mode, n=len(items), span=span,
                               fps=1000 / span if span > 0 else float("nan"), cost=cost,
                               p95_window_mean=mean(items, "span_p95_window_mean"),
                               clipmap=mean(items, "clipmap"), gather=mean(items, "gather"),
                               gpu_mhz=mean(items, "gpu_mhz"), mem_mhz=mean(items, "mem_mhz"),
                               util_pct=mean(items, "util_pct"), temp_c=mean(items, "temp_c"),
                               vsync=limited))
    return output


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: compare-modes.py RUN_DIR")
    with (Path(sys.argv[1]) / "phase-summary.csv").open(encoding="utf-8") as stream:
        rows = compare(list(csv.DictReader(stream)))
    print(f"{'scene':16} {'mode':8} {'n':>2} {'frame ms':>9} {'fps':>6} {'cost ms':>8} "
          f"{'p95 mean':>8} {'clipmap':>8} {'gather':>7} {'gpu MHz':>8} {'vsync':>6}")
    for row in rows:
        print(f"{row['scene']:16} {row['mode']:8} {row['n']:2} {row['span']:9.3f} {row['fps']:6.1f} "
              f"{row['cost']:+8.3f} {row['p95_window_mean']:8.2f} {row['clipmap']:8.3f} "
              f"{row['gather']:7.3f} {row['gpu_mhz']:8.0f} {str(row['vsync']):>6}")


if __name__ == "__main__":
    main()
