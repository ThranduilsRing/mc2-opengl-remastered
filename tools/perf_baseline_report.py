#!/usr/bin/env python3
"""perf_baseline_report.py -- steady-state frame-time percentiles and the top self-time zones of Tracy traces.

Reads the .tracy traces a `tracy_capture_compare.py baseline` run wrote and prints a markdown report:
  * frame-time percentiles from the wall-clock delta between consecutive DrawScreen starts, after dropping the first
    40% of frames (mission load / settle), or --skip-frames when a camera pose pins at a known frame, and any delta over 200 ms (same rule as the harness);
  * the top N zones by SELF time per frame over that same steady-state window. Self time = a zone's duration minus
    the time of the zones nested directly inside it on the same thread, rebuilt from tracy-csvexport --unwrap
    (the aggregate CSV cannot be windowed, so its totals would include mission-load zones).
Only CPU zones exist: the Vulkan islands carry no Tracy zones, so their cost shows up inside the zone that waits on
them (the present / fence zones), not under their own names.

usage: py -3 tools/perf_baseline_report.py TRACE.tracy [TRACE.tracy ...] [--top 15]
"""
import argparse
import csv
import io
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from smoke_lib import nofocus  # noqa: E402  (OWNER RULE: no window for any child process)

FRAME_ZONE = "DrawScreen"
WARMUP_FRAC = 0.4
HITCH_MS = 200.0


def export_exe():
    return Path(os.environ.get("TRACY_TOOLS_DIR", "C:/Users/Joe/Downloads/TracyProfiler")) / "tracy-csvexport.exe"


def pct(sorted_samples, q):
    k = max(0, min(len(sorted_samples) - 1, int(round(q / 100.0 * (len(sorted_samples) - 1)))))
    return sorted_samples[k]


def run_csv(args, trace):
    # background-launch: allow Tracy csvexport console tool, windowless via hidden_kwargs
    p = subprocess.Popen([str(export_exe())] + args + [str(trace)], stdout=subprocess.PIPE, text=True,
                         encoding="utf-8", errors="replace", **nofocus.hidden_kwargs())
    return p


def frame_starts(trace):
    p = run_csv(["-f", FRAME_ZONE, "-u"], trace)
    ts = []
    for row in csv.DictReader(p.stdout):
        if row.get("name") == FRAME_ZONE and row.get("ns_since_start"):
            ts.append(int(row["ns_since_start"]))
    p.wait()
    return sorted(ts)


def analyse(trace, top, skip_frames=0):
    ts = frame_starts(trace)
    if len(ts) < 50:
        return "### %s\n\nfewer than 50 frames -- not a steady-state trace.\n" % trace.name
    cut = max(int(len(ts) * WARMUP_FRAC), skip_frames)
    t0 = ts[cut]
    steady = ts[cut:]
    deltas = sorted(d for d in ((b - a) / 1e6 for a, b in zip(steady, steady[1:])) if d < HITCH_MS)
    nframes = len(steady)
    mean = sum(deltas) / len(deltas)
    out = ["### %s" % trace.name, "",
           "%d frames in the steady window (%d dropped as warm-up); frame time ms: mean %.2f, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f "
           "(%.1f fps mean, %.1f fps at p99)." % (nframes, cut, mean, pct(deltas, 50), pct(deltas, 95), pct(deltas, 99),
                                                    deltas[-1], 1000.0 / mean, 1000.0 / pct(deltas, 99)), ""]

    # per-thread event lists inside the window
    events = defaultdict(list)       # thread -> [(start, dur, name)]
    p = run_csv(["-u"], trace)
    for row in csv.DictReader(p.stdout):
        s = row.get("ns_since_start")
        if not s or not row.get("exec_time_ns") or not row.get("name"):
            continue
        s = int(s)
        if s < t0:
            continue
        events[row["thread"]].append((s, int(float(row["exec_time_ns"])), row["name"]))
    p.wait()

    incl = defaultdict(int)
    calls = defaultdict(int)
    threads = defaultdict(set)
    for th, evs in events.items():
        evs.sort(key=lambda e: (e[0], -e[1]))
        for _start, dur, name in evs:
            calls[name] += 1
            incl[name] += dur
            threads[name].add(th)
    selft = defaultdict(int)
    for th, evs in events.items():
        stack = []                    # [end, name, direct-child ns, duration]
        for start, dur, name in evs:
            while stack and stack[-1][0] <= start:
                _end, nm, child, d = stack.pop()
                selft[nm] += d - child
            if stack:
                stack[-1][2] += dur
            stack.append([start + dur, name, 0, dur])
        while stack:
            _end, nm, child, d = stack.pop()
            selft[nm] += d - child

    ranked = sorted(selft.items(), key=lambda kv: -kv[1])[:top]
    out += ["| # | zone | self ms/frame | inclusive ms/frame | calls/frame | thread(s) |", "|---|---|---|---|---|---|"]
    for i, (name, ns) in enumerate(ranked, 1):
        out.append("| %d | `%s` | %.3f | %.3f | %.1f | %s |" % (
            i, name, ns / 1e6 / nframes, incl[name] / 1e6 / nframes, calls[name] / nframes,
            ",".join(sorted(threads[name]))))
    out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--skip-frames", type=int, default=0,
                    help="drop at least this many leading frames (a pinned camera pose settles at a known frame)")
    a = ap.parse_args()
    for t in a.traces:
        print(analyse(Path(t), a.top, a.skip_frames))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
