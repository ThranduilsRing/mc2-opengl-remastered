#!/usr/bin/env python3
"""sim_determinism - is the simulation the same run after run, and the same across a refactor?

WHAT: reads the `[SIMID v1] simframe=N t=T n=C graded=.. visual=.. strict=..` line the engine prints for every sim frame
(MC2_SIM_IDENTITY_TRACE) and compares two runs of one mission frame by frame over the frames both reached. It names the first divergent
sim frame and prints both lines. `run` launches the mission twice through scripts/run_smoke.py under MC2_SMOKE_FIXED_TIMESTEP=1,
MC2_DETERMINISTIC_RNG=1 and MC2_SIM_IDENTITY_TRACE=1 (the three gates that remove wall-clock and RNG-seed variation). `--baseline`
additionally compares the chain digest of the first N frames with a committed one, which is the "SIMID unchanged" proof for a refactor
(ownership, enum, math steps) across commits.
WHY: a step whose proof is "same behaviour" needs a behaviour oracle that is itself stable. Two runs of the same exe must agree before a
difference between two exes means anything. The runs end at different frames (the smoke duration is wall clock), so only the common
prefix is compared.

  py -3 tools/sim_determinism.py run mc2_03 --duration 20 [--baseline tests/smoke/sim_determinism_baseline.json]
  py -3 tools/sim_determinism.py compare run_a/mc2_03.log run_b/mc2_03.log
  py -3 tools/sim_determinism.py write-baseline run_a/mc2_03.log --frames 1500 --out tests/smoke/sim_determinism_baseline.json
  py -3 tools/sim_determinism.py selftest          # offline; the `sim_determinism` board row
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

PREFIX = "[SIMID v1] simframe="
GATES = {"MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_DETERMINISTIC_RNG": "1", "MC2_SIM_IDENTITY_TRACE": "1"}
#: A comparison over fewer frames proves nothing (the mission may not have left the loading screen).
MIN_FRAMES = 300
DEFAULT_BASELINE = os.path.join(ROOT, "tests", "smoke", "sim_determinism_baseline.json")


def load_simid(path: str) -> list:
    """The SIMID lines of a log, newline stripped, in order."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        return [ln.rstrip("\r\n") for ln in fh if ln.startswith(PREFIX)]


def chain(lines, frames: int | None = None) -> str:
    """Short digest of the first `frames` lines; the same function as scripts/smoke_lib/logparse.py (simid_chain)."""
    h = hashlib.sha256()
    for ln in (lines if frames is None else lines[:frames]):
        h.update(ln.rstrip().encode("utf-8", "replace"))
    return h.hexdigest()[:16]


def compare(a, b, min_frames: int = MIN_FRAMES):
    """-> (ok, message). Only the frames both runs reached are compared."""
    common = min(len(a), len(b))
    if common < min_frames:
        return False, f"only {common} common sim frame(s) (need {min_frames}): the mission did not run long enough to prove anything"
    for i in range(common):
        if a[i] != b[i]:
            prev = a[i - 1] if i else "(first frame)"
            return False, (f"DIVERGED at sim frame index {i}\n  run a: {a[i]}\n  run b: {b[i]}\n  last agreed: {prev}\n"
                           "  rerun with MC2_SIM_IDENTITY_TRACE_DETAIL=1 for the per-mover lines of that frame")
    return True, f"identical over {common} sim frames (chain {chain(a, common)}; runs had {len(a)} and {len(b)})"


def check_baseline(lines, baseline_path: str):
    with open(baseline_path, encoding="utf-8") as fh:
        base = json.load(fh)
    n = int(base["frames"])
    if len(lines) < n:
        return False, f"the run has {len(lines)} sim frames, the baseline needs {n}"
    got = chain(lines, n)
    if got != base["chain"]:
        return False, f"chain over the first {n} frames is {got}, baseline {base['chain']} ({base.get('mission')} at {base.get('sha')})"
    return True, f"first {n} frames match the baseline chain {got}"


def _build_sha(log: str) -> str:
    """The sha of the exe that wrote the log (its [INSTR v1] enabled: line carries build=<sha>)."""
    with open(log, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if ln.startswith("[INSTR v1] enabled:") and " build=" in ln:
                return ln.split(" build=", 1)[1].split()[0]
    return ""


def _git_sha() -> str:
    r = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"], stdout=subprocess.PIPE, text=True,
                       **console_child.child_kwargs(redirected=True))
    return r.stdout.strip()


def run_mission(mission: str, duration: str, out: str) -> list:
    """Launch the mission twice under the determinism gates. -> [log path a, log path b]. The smoke lease is host wide and waited for."""
    env = dict(os.environ, **GATES)
    logs = []
    for tag in ("a", "b"):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py"), "--mission", mission, "--duration", duration,
                            "--deploy", "0.4", "--keep-logs"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           errors="replace", **console_child.child_kwargs(redirected=True))
        arts = sorted(glob.glob(os.path.join(ROOT, "tests", "smoke", "artifacts", "20*")), key=os.path.getmtime)
        if r.returncode or not arts:
            sys.exit(f"sim_determinism: smoke run {tag} failed (rc={r.returncode}):\n" + "\n".join(r.stdout.splitlines()[-8:]))
        dest = os.path.join(out, tag)
        shutil.rmtree(dest, ignore_errors=True)
        shutil.copytree(arts[-1], dest)
        logs.append(os.path.join(dest, mission + ".log"))
    return logs


def selftest() -> int:
    def mk(n, bad=None):
        out = []
        for i in range(n):
            g = "%016x" % (i * 7919 + (1 if i == bad else 0))
            out.append(f"{PREFIX}{i} t={i / 30:.3f} n=38 graded={g} visual={g} strict={g}")
        return out

    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    ok, _ = compare(mk(400), mk(400))
    expect(ok, "identical logs must pass")
    ok, msg = compare(mk(400), mk(400, bad=123))
    expect(not ok and "index 123" in msg and "last agreed" in msg, "a divergence must name its first frame")
    ok, _ = compare(mk(400), mk(350))
    expect(ok, "runs of different length compare over the common prefix")
    ok, _ = compare(mk(200), mk(200))
    expect(not ok, "fewer than MIN_FRAMES common frames must fail, not pass")
    ok, _ = compare([], [])
    expect(not ok, "no SIMID lines at all must fail (the trace gate was not applied)")
    expect(chain(mk(10)) == chain(mk(12), 10), "the chain over N frames ignores later frames")
    expect(chain(mk(10)) != chain(mk(10, bad=3)), "the chain sees a changed digest")
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "x.log")
        with open(p, "w") as fh:
            fh.write("noise\n" + "\n".join(mk(5)) + "\n[HEARTBEAT] frames=1\n")
        expect(len(load_simid(p)) == 5, "load_simid keeps only the SIMID lines")
        b = os.path.join(td, "b.json")
        with open(b, "w") as fh:
            json.dump({"frames": 5, "chain": chain(mk(5)), "mission": "m", "sha": "s"}, fh)
        expect(check_baseline(mk(9), b)[0], "a run that starts with the baseline frames matches it")
        expect(not check_baseline(mk(9, bad=2), b)[0], "a run that differs inside the baseline frames does not")
        expect(not check_baseline(mk(4), b)[0], "a run shorter than the baseline does not")
    for f in fails:
        print("sim_determinism selftest FAIL: " + f)
    print("sim_determinism selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("log_a")
    c.add_argument("log_b")
    c.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    r = sub.add_parser("run")
    r.add_argument("mission")
    r.add_argument("--duration", default="20")
    r.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    r.add_argument("--baseline", nargs="?", const=DEFAULT_BASELINE, help="also compare with a committed chain (default file: %s)" % os.path.relpath(DEFAULT_BASELINE, ROOT))
    r.add_argument("--out", help="keep both runs' artifacts here (default: a temporary directory)")
    w = sub.add_parser("write-baseline")
    w.add_argument("log")
    w.add_argument("--frames", type=int, required=True)
    w.add_argument("--mission", required=True)
    w.add_argument("--out", default=DEFAULT_BASELINE)
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "write-baseline":
        lines = load_simid(args.log)
        if len(lines) < args.frames:
            sys.exit(f"the log has {len(lines)} sim frames, fewer than --frames {args.frames}")
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"mission": args.mission, "frames": args.frames, "chain": chain(lines, args.frames), "sha": _build_sha(args.log) or _git_sha(),
                       "gates": GATES, "about": "chain digest of the first N [SIMID v1] lines; tools/sim_determinism.py run --baseline"}, fh, indent=1)
            fh.write("\n")
        print(f"sim_determinism: wrote {args.out}")
        return 0
    if args.cmd == "compare":
        ok, msg = compare(load_simid(args.log_a), load_simid(args.log_b), args.min_frames)
        print(("sim_determinism: PASS " if ok else "sim_determinism: FAIL ") + msg)
        return 0 if ok else 1
    out = args.out or tempfile.mkdtemp(prefix="simdet_")
    la, lb = run_mission(args.mission, args.duration, out)
    a, b = load_simid(la), load_simid(lb)
    ok, msg = compare(a, b, args.min_frames)
    print(("sim_determinism: PASS " if ok else "sim_determinism: FAIL ") + msg)
    if ok and args.baseline:
        ok2, msg2 = check_baseline(a, args.baseline)
        print(("sim_determinism: baseline PASS " if ok2 else "sim_determinism: baseline FAIL ") + msg2)
        ok = ok and ok2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
