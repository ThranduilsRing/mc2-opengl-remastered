#!/usr/bin/env python3
"""GPU pass table for the box: the `gpu-passes` row of tools/box_run.py.

    box_gpu_passes.py run --install DIR --out DIR [--name gpu_passes]   # fixed poses, off-screen, own fresh log dir
    box_gpu_passes.py compare BASE.json TIP.json                         # per-pass ratios; under-sampled passes are NOT MEASURED

Source is the engine's `[VK_GPU_PASSES v1]` line (one window mean per 120 frames). A sample is one window mean after the
first --skip-windows warm-up windows. Ratios only: the box is not a ms source (lane rules).
Only this run's own logs count: the run wipes its dir first and a log older than the run start is ignored.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.append(str(ROOT / "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)

POSES = (("mc2_01", "day"), ("mc2_10", "night"), ("mc2_24", "battle"))
DURATION = 20
SKIP_WINDOWS = 2     # warm-up windows (240 frames) dropped
FLOOR = 6            # fewer samples than this: NOT MEASURED, no number
TAG = "[VK_GPU_PASSES v1] frame="


def parse_log(text, skip_windows=SKIP_WINDOWS):
    """{scope: [window mean ms]}; header span/sum become Frame.span/Frame.sum. Windows with dropped/overflow timestamps are skipped."""
    scopes, windows = {}, 0
    for line in text.splitlines():
        if TAG not in line:
            continue
        head, sep, tail = line.partition("|")
        f = dict(re.findall(r"(\w+)=([\d.]+)", head))
        if not sep or "n" not in f or "span_ms" not in f:
            continue
        windows += 1
        if windows <= skip_windows or int(f.get("dropped", 0)) or int(f.get("overflow", 0)):
            continue
        scopes.setdefault("Frame.span", []).append(float(f["span_ms"]))
        scopes.setdefault("Frame.sum", []).append(float(f["sum_ms"]))
        for name, val in re.findall(r"(\S+?)=([\d.]+)\(x[\d.]+\)", tail):
            if name.startswith("VKIsl."):
                scopes.setdefault(name, []).append(float(val))
    return scopes


def collect_logs(root, since):
    """Text of every *.log under root written at or after `since` (epoch s); older files are stale and ignored."""
    out = []
    for p in sorted(Path(root).rglob("*.log")):
        if p.stat().st_mtime >= since:
            out.append(p.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(out)


def pct(sorted_vals, q):
    return sorted_vals[max(0, math.ceil(q / 100 * len(sorted_vals)) - 1)]


def stats(values, floor=FLOOR):
    n = len(values)
    if n < floor:
        return {"n": n, "measured": False}
    s = sorted(values)
    return {"n": n, "measured": True, "mean": sum(s) / n, "p50": pct(s, 50), "p95": pct(s, 95)}


def table(result):
    lines = []
    for pose in result["poses"]:
        lines.append(f"[gpu-passes] pose={pose['mission']} ({pose['label']}) rc={pose['rc']} floor={result['floor']} (ms, window means; box is not a ms source)")
        if not pose["scopes"]:
            lines.append("  NOT MEASURED: no [VK_GPU_PASSES v1] windows in this run's own log")
            continue
        lines.append(f"  {'pass':<28}{'mean':>9}{'p50':>9}{'p95':>9}{'n':>5}")
        for name, st in sorted(pose["scopes"].items()):
            if st["measured"]:
                lines.append(f"  {name:<28}{st['mean']:>9.3f}{st['p50']:>9.3f}{st['p95']:>9.3f}{st['n']:>5}")
            else:
                lines.append(f"  {name:<28}{'NOT MEASURED':>27}{st['n']:>5}")
    return "\n".join(lines)


def run_pose(install, out, mission, duration, skip=SKIP_WINDOWS, ref_commit=None):
    """One game run through run_smoke into a fresh dir under out; returns (rc, scopes values)."""
    d = Path(out) / mission
    shutil.rmtree(d, ignore_errors=True)   # the run's own log dir: nothing from an earlier run may be counted
    d.mkdir(parents=True)
    t0 = time.time() - 1
    env = dict(os.environ, MC2_SMOKE_ARTIFACT_ROOT=str(d))
    cmd = [sys.executable, str(ROOT / "scripts" / "run_smoke.py"), "--mission", mission, "--duration", str(duration),
           "--keep-logs", "--exe", str(Path(install) / ("mc2.exe" if os.name == "nt" else "mc2"))]
    if ref_commit:   # the exe under test is not worktree HEAD (the before arm of an A/B)
        cmd += ["--ref-commit", ref_commit]
    p = subprocess.run(cmd, env=env, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       **console_child.child_kwargs(redirected=True))
    (d / "run_smoke.out").write_text(p.stdout, encoding="utf-8")
    return p.returncode, parse_log(collect_logs(d, t0), skip)


def do_run(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    result = {"version": 1, "floor": a.floor, "skip_windows": a.skip_windows, "duration_s": a.duration, "poses": []}
    for mission, label in POSES:
        rc, scopes = run_pose(a.install, out / a.name, mission, a.duration, a.skip_windows, a.ref_commit)
        result["poses"].append({"mission": mission, "label": label, "rc": rc,
                                "scopes": {k: stats(v, a.floor) for k, v in scopes.items()},
                                "samples": scopes})
    (out / f"{a.name}.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
    text = table(result)
    (out / f"{a.name}.txt").write_text(text + "\n", encoding="utf-8")
    print(text)
    measured = any(s["measured"] for p in result["poses"] for s in p["scopes"].values())
    return 0 if measured else 1


def compare(base, tip):
    """Lines of per-pass tip/base ratios; a pass with too few samples on either side prints NOT MEASURED."""
    lines = []
    tips = {p["mission"]: p for p in tip["poses"]}
    for bp in base["poses"]:
        tp = tips.get(bp["mission"])
        lines.append(f"[gpu-passes] A/B pose={bp['mission']} ({bp['label']}): tip/base ratio, <1 = tip faster")
        if not tp:
            lines.append("  NOT MEASURED: pose missing in tip")
            continue
        lines.append(f"  {'pass':<28}{'mean x':>9}{'p50 x':>9}{'n base/tip':>12}")
        for name in sorted(set(bp["scopes"]) | set(tp["scopes"])):
            b, t = bp["scopes"].get(name), tp["scopes"].get(name)
            ns = f"{b['n'] if b else 0}/{t['n'] if t else 0}"
            if not (b and t and b["measured"] and t["measured"]):
                lines.append(f"  {name:<28}{'NOT MEASURED':>18}{ns:>12}")
                continue
            note = ""
            if bp["samples"].get(name) == tp["samples"].get(name):
                note = "  IDENTICAL SAMPLE LISTS (same exe or stale log?)"
            lines.append(f"  {name:<28}{t['mean'] / b['mean']:>9.3f}{t['p50'] / b['p50']:>9.3f}{ns:>12}{note}" if b["mean"] > 0 and b["p50"] > 0
                         else f"  {name:<28}{'base 0':>18}{ns:>12}")
    return lines


def do_compare(a):
    base = json.loads(Path(a.base).read_text(encoding="utf-8"))
    tip = json.loads(Path(a.tip).read_text(encoding="utf-8"))
    text = "\n".join(compare(base, tip))
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n", encoding="utf-8")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--install", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--name", default="gpu_passes")
    r.add_argument("--duration", type=int, default=DURATION)
    r.add_argument("--floor", type=int, default=FLOOR)
    r.add_argument("--ref-commit", help="commit the exe was built from when it is not worktree HEAD")
    r.add_argument("--skip-windows", type=int, default=SKIP_WINDOWS)
    r.set_defaults(fn=do_run)
    c = sub.add_parser("compare")
    c.add_argument("base")
    c.add_argument("tip")
    c.add_argument("--out")
    c.set_defaults(fn=do_compare)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
