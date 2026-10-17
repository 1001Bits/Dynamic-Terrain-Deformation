"""Summarize fully contained profiler windows after each scene phase's warmup.

Means are frame weighted; span_p95_window_mean is the mean of window percentiles,
not the pooled frame percentile. Missing measurements remain empty. CPU scopes are
reported separately because gather includes object scans/impacts and shelter can
include snow-top work. This script never adds overlapping scopes into a CPU total.
"""
import argparse
import csv
import datetime as dt
import json
import math
from pathlib import Path
import re

STAMP = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d+)\]")
LABELS = {
    "span": "GPU frame span", "clipmap": "Clipmap dispatch", "landscape": "Landscape",
    "bracket": "Bracket CPU", "gather": "Gather CPU", "coverage": "Coverage CPU",
    "shelter": "Shelter CPU", "objects": "Object scan CPU", "impacts": "Impacts CPU",
    "shader_prepare": "Shader prepare CPU", "ground_decals": "Ground decals CPU",
    "snow_tops": "Snow tops CPU", "object_snow_gpu": "Snow on objects", "floor_prints_gpu": "Floor prints",
}
FIELDS = {key: re.compile(re.escape(label) + r"\s+avg\s+([\d.]+)") for key, label in LABELS.items()}
GPU_KEYS = ["gpu_mhz", "mem_mhz", "temp_c", "util_pct", "power_w"]


def clock_seconds(text):
    h, m, s = text.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def wall_seconds(text):
    # Plugin stamps are local wall time, including when records carry a UTC offset.
    stamp = dt.datetime.fromisoformat(text)
    return stamp.date().toordinal() * 86400 + clock_seconds(stamp.strftime("%H:%M:%S.%f"))


def analyze(run, warmup=20.0, gpu_csv=None):
    records = []
    for path in sorted(run.glob("*.json")):
        try:
            record = json.loads(path.read_text(encoding="utf-8-sig"))
        except (ValueError, OSError):
            continue
        if isinstance(record, dict) and all(k in record for k in ("written", "measuredUntil", "name")):
            records.append(record)

    # New records include dates. Old runs are anchored to their YYYYMMDD folder name;
    # otherwise day zero still permits one midnight rollover without fabricating dates.
    match = re.match(r"(\d{8})-", run.name)
    base_day = dt.datetime.strptime(match[1], "%Y%m%d").date().toordinal() if match else 0
    if not base_day:
        dated = [r["writtenAt"] for r in records if r.get("writtenAt")]
        if dated:
            base_day = dt.datetime.fromisoformat(min(dated)).date().toordinal()

    windows = []
    current = None
    previous_time = None
    previous_end = None
    day = base_day
    for line in (run / "NMN_DeformableTerrain.log").read_text(encoding="utf-8", errors="replace").splitlines():
        stamp = STAMP.match(line)
        if stamp:
            now = clock_seconds(stamp[1])
            if previous_time is not None and now < previous_time - 43200:
                day += 1
            previous_time = now
        if "--- Profile:" in line and stamp:
            frames = re.search(r"Profile: (\d+) frames", line)
            end = day * 86400 + now
            current = {"end": end, "start": previous_end,
                       "frames": int(frames[1]) if frames else 0, "vsync_limited": False}
            previous_end = end
            windows.append(current)
            continue
        if current is None:
            continue
        if "span is the frame INTERVAL" in line:
            current["vsync_limited"] = True
        for key, pattern in FIELDS.items():
            value = pattern.search(line)
            if value:
                current[key] = float(value[1])
                if key == "span":
                    p95 = re.search(r"p95\s+([\d.]+)", line)
                    if p95:
                        current["span_p95_window_mean"] = float(p95[1])

    telemetry = []
    if gpu_csv:
        with Path(gpu_csv).open(encoding="utf-8-sig") as stream:
            for row in csv.reader(stream):
                if len(row) < 6 or "timestamp" in row[0]:
                    continue
                try:
                    stamp = dt.datetime.strptime(row[0].strip(), "%Y/%m/%d %H:%M:%S.%f")
                    values = [float(re.sub(r"[^\d.+-]", "", v) or "nan") for v in row[1:6]]
                except ValueError:
                    continue
                telemetry.append((wall_seconds(stamp.isoformat()), values))

    phases = []
    for record in records:
        if record.get("writtenAt") and record.get("measuredUntilAt"):
            written, end = wall_seconds(record["writtenAt"]), wall_seconds(record["measuredUntilAt"])
        else:
            written = base_day * 86400 + clock_seconds(record["written"])
            end = base_day * 86400 + clock_seconds(record["measuredUntil"])
            if end < written:
                end += 86400
            # Old clock-only records may belong to the day after the log begins.
            candidates = [(written + shift * 86400, end + shift * 86400) for shift in range(day - base_day + 1)]
            matches = [(a, b) for a, b in candidates if windows and b >= windows[0]["end"] and a <= windows[-1]["end"]]
            if len(matches) > 1:
                raise ValueError(f"Ambiguous date for phase {record['name']}; dated phase records are required")
            if matches:
                written, end = matches[0]
        start = written + warmup
        chosen = [w for w in windows if w["start"] is not None and
                  start <= w["start"] < w["end"] <= end and w["frames"] > 0 and "span" in w]
        row = {"phase": record["name"], "start": record.get("writtenAt", record["written"]),
               "windows": len(chosen), "frames": sum(w["frames"] for w in chosen),
               "vsync_windows": sum(w["vsync_limited"] for w in chosen)}
        for key in [*FIELDS, "span_p95_window_mean"]:
            samples = [w for w in chosen if key in w]
            weight = sum(w["frames"] for w in samples)
            row[key] = round(sum(w[key] * w["frames"] for w in samples) / weight, 3) if weight else None
        # Telemetry covers the actual chosen windows, not the discarded warmup/tails.
        for i, key in enumerate(GPU_KEYS):
            values = [v[i] for t, v in telemetry if math.isfinite(v[i]) and
                      any(w["start"] <= t <= w["end"] for w in chosen)]
            row[key] = round(sum(values) / len(values), 1) if values else None
        phases.append((written, row))
    return [row for _, row in sorted(phases, key=lambda item: item[0])]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--warmup", type=float, default=20.0)
    parser.add_argument("--gpu")
    args = parser.parse_args()
    if args.warmup < 0 or not math.isfinite(args.warmup):
        parser.error("--warmup must be finite and nonnegative")
    phases = analyze(args.run, args.warmup, args.gpu)
    columns = ["phase", "windows", "frames", "span", "span_p95_window_mean", "clipmap", "gather",
               "shelter", "vsync_windows", *GPU_KEYS]
    print(" ".join(f"{c:>12}" for c in columns))
    for row in phases:
        print(" ".join(f"{str(row.get(c)):>12}" for c in columns))
    with (args.run / "phase-summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(phases[0]) if phases else columns)
        writer.writeheader()
        writer.writerows(phases)


if __name__ == "__main__":
    main()
