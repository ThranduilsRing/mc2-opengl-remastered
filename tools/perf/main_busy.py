#!/usr/bin/env python3
"""B2 exclusive-cost table from a bench frames.jsonl (docs/plans/2026-11-02-perf-design.md section B2).

Columns add to frame_ms on main: main_exec + wait_sealed + wait_sim + wait_gpu_main = frame_ms.
MAIN_BUSY = frame_ms - wait_gpu_main (waits released by the GPU/WSI on the main thread; CPU-worker waits stay in).
wait_sealed is reported whole: its GPU/CPU intersection split needs worker interval timestamps that the sink does not record yet.
Tick frames (ticks > 0) are summarised separately so 30 Hz stalls cannot hide in the median.

usage: py -3 tools/perf/main_busy.py <frames.jsonl | run dir> [--json]
"""
import json
import sys
from pathlib import Path

COLUMNS = ("frame_ms", "main_exec", "wait_sealed", "wait_sim", "wait_gpu_main", "MAIN_BUSY")


def pct(values, q):
    if not values:
        return 0.0
    ordered = sorted(values)
    pos = (len(ordered) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (pos - lo)


def frame_columns(row):
    sealed = row.get("wait_sealed_ms", 0.0)
    sim = row.get("wait_sim_ms", 0.0)
    gpu = row.get("wait_gpu_main_ms", 0.0)
    ms = row["ms"]
    return {"frame_ms": ms, "main_exec": ms - sealed - sim - gpu, "wait_sealed": sealed, "wait_sim": sim,
            "wait_gpu_main": gpu, "MAIN_BUSY": ms - gpu}


def load_cpu_rows(path):
    path = Path(path)
    if path.is_dir():
        path = path / "frames.jsonl"
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    return [r for r in rows if r.get("type") == "cpu" and "wait_sealed_ms" in r]


def table(rows):
    out = {}
    for label, subset in (("all", rows), ("tick_frames", [r for r in rows if r.get("ticks", 0) > 0])):
        cols = [frame_columns(r) for r in subset]
        out[label] = {"frames": len(cols),
                      **{c: {"median": pct([x[c] for x in cols], 0.5), "p95": pct([x[c] for x in cols], 0.95)} for c in COLUMNS}}
    return out


def render(result):
    lines = []
    for label in ("all", "tick_frames"):
        t = result[label]
        lines.append(f"[{label}] frames={t['frames']}")
        lines.append(f"{'column':<15}{'median_ms':>12}{'p95_ms':>12}")
        for c in COLUMNS:
            lines.append(f"{c:<15}{t[c]['median']:>12.3f}{t[c]['p95']:>12.3f}")
        lines.append("")
    return "\n".join(lines)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    rows = load_cpu_rows(args[0])
    if not rows:
        print("no cpu rows with wait_* fields (run with bench_output armed on a PERF_COUNTERS build)", file=sys.stderr)
        return 1
    result = table(rows)
    print(json.dumps(result, indent=1) if "--json" in argv else render(result))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
