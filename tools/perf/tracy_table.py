#!/usr/bin/env python3
"""Per-capture Tracy CPU attribution: tracy_table.py ROOT OUTPUT.json.

Mission load begins at Mission::init. Its first subsequent mission-interface
update is a conservative upper bound on mission_ready (the mission is active).
No wall-clock/log-clock alignment is assumed. Missing anchors or identity
columns are errors, never a whole-capture gameplay fallback. First playable
means the first 60 complete Frame scopes after this readiness upper bound;
steady play begins after those frames. Times include waits, not CPU execution.
Self time excludes direct children on the same thread; worker time overlaps
main time and must not be added to it as critical-path latency.
"""
import argparse
from bisect import bisect_right
from collections import defaultdict
import csv
import io
import json
import math
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke_lib import nofocus
from tracy_capture_compare import tracy_exe


def stats(values):
    values = sorted(values)
    return {"mean_ms": sum(values) / len(values) / 1e6,
            "p99_ms": values[math.ceil(.99 * len(values)) - 1] / 1e6,
            "worst_ms": values[-1] / 1e6}


def summarize(reader, mission_load_ns=None, mission_ready_ns=None, first_playable_frames=60):
    """Keep (thread, name, source file, source line) identity in every phase."""
    events = []
    for row in reader:
        duration_key = next((k for k in ("ns", "duration_ns", "exec_time_ns", "duration") if k in row), None)
        if not duration_key or any(not row.get(k) for k in
                                  ("name", "thread", "src_file", "src_line", "ns_since_start")):
            raise ValueError("Tracy unwrap CSV requires timestamps, thread and source identity")
        start, duration = int(row["ns_since_start"]), int(row[duration_key])
        if start < 0 or duration < 0:
            raise ValueError("negative Tracy timestamp or duration")
        events.append({"identity": (row["thread"], row["name"], row["src_file"], int(row["src_line"])),
                       "start": start, "end": start + duration, "self": [(start, start + duration)]})
    if mission_load_ns is None or mission_ready_ns is None:
        loads = [e for e in events if e["identity"][1] == "Mission::init"]
        if len(loads) != 1:
            raise ValueError("capture needs exactly one Mission::init phase anchor")
        mission_load_ns = loads[0]["start"]
        updates = [e["start"] for e in events if e["identity"][1] == "GameLogic.Mission.Interface"
                   and e["start"] >= loads[0]["end"]]
        if not updates:
            raise ValueError("capture lacks an active mission readiness anchor")
        mission_ready_ns = min(updates)
    if not 0 <= mission_load_ns < mission_ready_ns or first_playable_frames < 1:
        raise ValueError("invalid mission phase boundaries")
    all_frames = sorted((e["start"], e["end"]) for e in events if e["identity"][1] == "Frame")
    frames = [(s, t) for s, t in all_frames if s >= mission_ready_ns]
    if len(frames) <= first_playable_frames:
        raise ValueError("capture lacks steady Frame scopes after mission_ready")
    if any(a[1] > b[0] for a, b in zip(frames, frames[1:])):
        raise ValueError("overlapping Frame scopes: ambiguous frame timeline")
    # Retain exclusive intervals to charge a crossing zone to each actual frame.
    stacks = defaultdict(list)
    for event in sorted(events, key=lambda e: (e["start"], -e["end"])):
        stack = stacks[event["identity"][0]]
        while stack and event["start"] >= stack[-1]["end"]:
            stack.pop()
        if stack:
            parent = stack[-1]
            if event["end"] > parent["end"]:
                raise ValueError("crossing zones on one thread")
            lo, hi = parent["self"].pop()
            parent["self"].extend([(lo, event["start"]), (event["end"], hi)])
        stack.append(event)
    steady = frames[first_playable_frames][0]
    end = frames[-1][1]  # trailing exporter/shutdown work is not steady gameplay
    windows = [("startup", 0, mission_load_ns),
               ("mission_load", mission_load_ns, mission_ready_ns),
               ("first_playable", mission_ready_ns, steady), ("steady_play", steady, end)]
    result = {}
    for phase, lo, hi in windows:
        phase_frames = [(s, t) for s, t in all_frames if lo <= s and t <= hi]
        starts = [s for s, _ in phase_frames]
        groups = {}
        for event in events:
            if event["start"] >= hi or event["end"] <= lo:
                continue
            if phase in ("first_playable", "steady_play") and event["start"] < mission_ready_ns:
                continue
            identity = event["identity"]
            group = groups.setdefault(identity, {"count": 0, "incl_ns": 0, "self_ns": 0,
                                                 "incl_frames": [0] * len(starts), "self_frames": [0] * len(starts)})
            group["count"] += 1
            for kind, intervals in (("incl", [(event["start"], event["end"])]), ("self", event["self"])):
                for a, b in intervals:
                    a, b = max(a, lo), min(b, hi)
                    if b <= a:
                        continue
                    group[kind + "_ns"] += b - a
                    index = max(0, bisect_right(starts, a) - 1)
                    while index < len(starts) and starts[index] < b:
                        s, t = phase_frames[index]
                        group[kind + "_frames"][index] += max(0, min(b, t) - max(a, s))
                        index += 1
        zones = []
        for (thread, name, source, line), group in groups.items():
            zones.append({"thread": thread, "name": name, "src_file": source, "src_line": line,
                          "count": group["count"], "self_total_ms": group["self_ns"] / 1e6,
                          "incl_total_ms": group["incl_ns"] / 1e6,
                          "self_per_frame": stats(group["self_frames"]) if starts else None,
                          "incl_per_frame": stats(group["incl_frames"]) if starts else None})
        result[phase] = {"start_ns": lo, "end_ns": hi, "frames": len(starts),
                         "frame_latency": stats([t - s for s, t in phase_frames]) if starts else None,
                         "zones": sorted(zones, key=lambda z: -z["self_total_ms"])}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    traces = sorted(args.root.rglob("*.tracy"))
    if not traces:
        parser.error("no captures found")
    output = {}
    for trace in traces:
        flags = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if sys.platform == "win32" else {}
        # background-launch: allow the windowless Tracy CSV console exporter.
        exported = subprocess.run([str(tracy_exe("tracy-csvexport.exe")), "-u", str(trace)],
                                  capture_output=True, text=True, check=True, timeout=180,
                                  **nofocus.hidden_kwargs(**flags))
        output[str(trace.relative_to(args.root))] = summarize(csv.DictReader(io.StringIO(exported.stdout)))
    args.out.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    for trace, phases in output.items():
        for phase, data in phases.items():
            print(f"{trace} {phase}: frames={data['frames']} latency={data['frame_latency']}")
            print("| zone | thread | source | self total ms | self/frame mean ms | p99 ms | worst ms |")
            print("|---|---|---|---:|---:|---:|---:|")
            for zone in data['zones'][:18]:
                per_frame = zone['self_per_frame']
                tail = (f"{per_frame['mean_ms']:.3f} | {per_frame['p99_ms']:.3f} | {per_frame['worst_ms']:.3f}"
                        if per_frame else "n/a | n/a | n/a")
                label = zone['name'].replace('|', '\\|').replace('\n', ' ')
                print(f"| {label} | {zone['thread']} | {zone['src_file']}:{zone['src_line']} | "
                      f"{zone['self_total_ms']:.3f} | {tail} |")


if __name__ == "__main__":
    main()
