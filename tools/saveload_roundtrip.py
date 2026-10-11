#!/usr/bin/env python3
"""saveload_roundtrip - does a quicksave followed by a quickload put the simulation back where it was?

WHAT: runs one mission with units moving (mc2_16 under MC2_FIXTURE_ORDER_LIVENESS) under the determinism gates, quicksaves at render frame
SAVE_AT and quickloads at LOAD_AT (the MC2_BOOT_QUICKSAVE_AT / MC2_BOOT_QUICKLOAD_AT fixtures set the same flags as the pause menu buttons),
and compares the `[SIMID v1]` digests the simulation prints AFTER the load with the ones it printed after the save, frame by frame. The load puts
the scenario clock back, so the first post-load frame is matched to the pre-load frame with the same time. A save/load that restores everything
the simulation reads gives identical digests for as long as the run lasts (the mission itself is run-to-run deterministic, tools/sim_determinism.py).
WHY: nothing else exercises Mission::save/load; the enum and ownership steps touch the serialized state. Owner ruling: within a release line a save
must load, and the round trip is the proof.
With MC2_SIM_IDENTITY_TRACE_DETAIL=1 the first divergent frame is explained mover by mover (gesture, path step, position, target).

  py -3 tools/saveload_roundtrip.py run [--mission mc2_16] [--frames 300] [--deploy]
  py -3 tools/saveload_roundtrip.py analyze path/to/mission.log [--frames 300]
  py -3 tools/saveload_roundtrip.py selftest
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402
import sim_determinism as sd  # noqa: E402

SAVE_AT, LOAD_AT = "1000", "1250"
ENV = dict(sd.GATES, MC2_SMOKE_FREEZE_ANIM="1", MC2_FIXTURE_ORDER_LIVENESS="1", MC2_SIM_IDENTITY_TRACE_DETAIL="1",
           MC2_BOOT_QUICKSAVE_AT=SAVE_AT, MC2_BOOT_QUICKLOAD_AT=LOAD_AT)
_LINE = re.compile(r"^\[SIMID v1\] simframe=(\d+) t=([\d.]+) (.*)$")
_MOVER = re.compile(r"^\[SIMID v1 mover\] simframe=(\d+) id=(\d+) (.*)$")


def parse(text_lines):
    """-> (frames [(simframe, t, digest text)], movers {simframe: {id: record}})"""
    frames, movers = [], {}
    for ln in text_lines:
        m = _LINE.match(ln)
        if m:
            frames.append((int(m.group(1)), float(m.group(2)), m.group(3).strip()))
            continue
        m = _MOVER.match(ln)
        if m:
            movers.setdefault(int(m.group(1)), {})[int(m.group(2))] = m.group(3).strip()
    return frames, movers


def analyze(text_lines, frames_needed: int = 300):
    """-> (ok, [report lines])"""
    frames, movers = parse(text_lines)
    jumps = [i for i in range(1, len(frames)) if frames[i][1] < frames[i - 1][1] - 0.01]
    if not jumps:
        return False, ["no load found: the scenario clock never went back (did the quickload fixture fire? SAVE_AT/LOAD_AT are render frames)"]
    j = jumps[0]
    t_load = frames[j][1]
    pre = [i for i in range(j) if abs(frames[i][1] - t_load) < 0.02]
    if not pre:
        return False, [f"no pre-load frame at t={t_load:.3f} to match the first post-load frame"]
    i0 = pre[0]
    after = len(frames) - j
    report = [f"load at sim frame {frames[j][0]} (t={t_load:.3f}) matched to pre-load frame {frames[i0][0]}; {after} frames after the load"]
    first = None
    for k in range(min(after, j - i0)):
        if frames[i0 + k][2] != frames[j + k][2]:
            first = k
            break
    horizon = min(after, j - i0)
    if first is None:
        ok = horizon >= frames_needed
        report.append(f"digests identical over {horizon} frames" + ("" if ok else f" (need {frames_needed})"))
        return ok, report
    report.append(f"DIGESTS DIVERGE {first} frame(s) after the load (identical before that)")
    a, b = movers.get(frames[i0 + first][0], {}), movers.get(frames[j + first][0], {})
    diff = [i for i in a if a[i] != b.get(i)]
    if diff:
        report.append(f"{len(diff)} of {len(a)} movers differ at that frame; first three:")
        for i in diff[:3]:
            report.append(f"  id {i}\n    saved : {a[i]}\n    loaded: {b.get(i)}")
    else:
        report.append("(no per-mover detail in the log: rerun with MC2_SIM_IDENTITY_TRACE_DETAIL=1)")
    # How far the state stays close: the share of post-load frames whose digest still equals the saved run's.
    same = sum(1 for k in range(min(after, j - i0)) if frames[i0 + k][2] == frames[j + k][2])
    report.append(f"{same} of {horizon} compared frames have an identical digest")
    return first >= frames_needed, report


def selftest() -> int:
    def mk(n, t0=0.0, tag="a"):
        return [f"[SIMID v1] simframe={i} t={t0 + i / 30:.3f} n=2 graded={tag}{i} visual=v strict=s" for i in range(n)]

    fails = []
    pre = mk(100)                           # frames 0..99, t up to 3.3
    good = pre + mk(60, t0=60 / 30.0)       # load back to frame 60's time; same digests names -> they differ by index, so build exact copy
    good = pre + [f"[SIMID v1] simframe={100 + i} t={(60 + i) / 30:.3f} n=2 graded=a{60 + i} visual=v strict=s" for i in range(40)]
    ok, rep = analyze(good, frames_needed=30)
    if not ok:
        fails.append("an exact restore must pass: " + "; ".join(rep))
    bad = pre + [f"[SIMID v1] simframe={100 + i} t={(60 + i) / 30:.3f} n=2 graded=a{60 + i if i < 10 else 'X'} visual=v strict=s" for i in range(40)]
    ok, rep = analyze(bad, frames_needed=30)
    if ok or "10 frame(s)" not in " ".join(rep):
        fails.append("a restore that drifts after 10 frames must fail and say so: " + "; ".join(rep))
    ok, rep = analyze(pre, frames_needed=30)
    if ok or "no load found" not in rep[0]:
        fails.append("a log without a load must fail")
    ok, rep = analyze(good[:110], frames_needed=30)
    if ok:
        fails.append("too few post-load frames must fail")
    for f in fails:
        print("saveload_roundtrip selftest FAIL: " + f)
    print("saveload_roundtrip selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def run(mission: str, frames: int, deploy: bool, out: str | None):
    if deploy:
        sd.build_and_deploy()
    env = dict(os.environ, **ENV)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py"), "--mission", mission, "--duration", "28", "--deploy", "0.4",
                        "--keep-logs"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       **console_child.child_kwargs(redirected=True))
    arts = sorted(glob.glob(os.path.join(ROOT, "tests", "smoke", "artifacts", "20*")), key=os.path.getmtime)
    if not arts or not os.path.exists(os.path.join(arts[-1], mission + ".log")):
        sys.exit("saveload_roundtrip: the smoke run produced no log:\n" + "\n".join(r.stdout.splitlines()[-8:]))
    dest = out or tempfile.mkdtemp(prefix="saveload_")
    shutil.copytree(arts[-1], os.path.join(dest, "run"), dirs_exist_ok=True)
    log = os.path.join(dest, "run", mission + ".log")
    with open(log, encoding="utf-8", errors="replace") as fh:
        return analyze(fh.read().splitlines(), frames)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--mission", default="mc2_16")
    r.add_argument("--frames", type=int, default=300, help="post-load frames that must match")
    r.add_argument("--deploy", action="store_true")
    r.add_argument("--out")
    a = sub.add_parser("analyze")
    a.add_argument("log")
    a.add_argument("--frames", type=int, default=300)
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "analyze":
        with open(args.log, encoding="utf-8", errors="replace") as fh:
            ok, rep = analyze(fh.read().splitlines(), args.frames)
    else:
        ok, rep = run(args.mission, args.frames, args.deploy, args.out)
    print(("saveload_roundtrip: PASS" if ok else "saveload_roundtrip: FAIL") + "\n" + "\n".join(rep))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
