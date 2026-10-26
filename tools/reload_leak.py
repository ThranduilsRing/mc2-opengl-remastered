#!/usr/bin/env python3
"""reload_leak.py -- battery item 10-Reload Leak Loop: do quicksave reloads give memory back?

WHAT: builds a disposable MC2_ALLOC_STATS=ON exe in a throwaway build tree, stages a
throwaway copy of an install with that exe, runs one mission under the engine's built-in
reload fixture, and parses its [ALLOC_RELOAD] phase=pre/phase=post pairs. The fixture
quicksaves once (cycle 0) and reloads that save on the following cycles; each reload is
a whole Mission::load, so a leak shows up as live/bytes climbing from one cycle to the
next instead of returning to the pre-reload value.

  py -3 tools/reload_leak.py --install A:/Games/mc2-opengl/mc2-win64-v0.4
  py -3 tools/reload_leak.py --install A:/Games/mc2-opengl/mc2-win64-v0.4 --cycles 10 --mission mc2_01
  py -3 tools/reload_leak.py analyze tests/smoke/artifacts/<stamp>/mc2_01.log   # parse a kept log only

The fixture knobs (code/mechcmd2_frame.cpp, MC2_ALLOC_STATS only): S10_RELOADS=cycles
(engine default 8), S10_STRIKE=1 calls an artillery strike in before each reload (the
pooled rounds are the interesting allocation), S10_RELOAD=1 actually reloads (0 is the
sample-only control), and MC2_BOOT_QUICKSAVE_AT=300 places the first quicksave. So N
cycles give one quicksave and N-1 reload pairs.

WHERE IT WRITES: the proof build goes to build64-battery/ (a gitignored sibling of
build64/; --build-tree overrides). The staged install is a fresh copy under
E:/mc2-installs/reload_leak-stage/<stamp>/ -- data is copied, never hardlinked, because G: is
a different volume and a staged copy must not share writable state with an install.
The install named by --install is only read; this tool never deploys into it.

VERDICT: PASS when every reload's post live count and byte total are back at that
reload's pre value (within --tolerance) and every requested cycle ran. A run that
captured no pair (exe without MC2_ALLOC_STATS, or the fixture never fired) is exit 2.

Exit codes: 0 PASS, 1 leak (post != pre) or a failed smoke run, 2 config/staging/
insufficient evidence, 3 proof build or deploy error.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import datetime as dt
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "scripts"))
import console_child  # noqa: E402  (windowless console children that still print)
import stage_root as stages  # noqa: E402  (default staging roots on E:, free-space preflight)
import savegame_backup  # noqa: E402  (back the owner's quicksave up around the fixture)
from configure_ninja import NINJA, find_cmake, under_vcvars  # noqa: E402
from bootstrap_worktree_build import cache_args  # noqa: E402

PREFIX = "[reload_leak]"
#: Disposable staging root; created on first run.
STAGE_ROOT = stages.default_root("reload_leak")
DEFAULT_INSTALL = "A:/Games/mc2-opengl/mc2-win64-v0.4"
DEFAULT_BUILD_TREE = ROOT / "build64-battery"
EXE_NAME = "mc2.exe"
DEFAULT_MISSION = "mc2_01"
DEFAULT_DURATION = 30
DEFAULT_CYCLES = 10

#: The battery's fixture environment (S10_* are plain names, not gate-registry rows).
FIXTURE_ENV = {
    "S10_STRIKE": "1",
    "S10_RELOAD": "1",
    "MC2_BOOT_QUICKSAVE_AT": "300",
}

#: [ALLOC_RELOAD] n=<cycle> phase=pre|post live=<n> bytes=<n> t=<scenario ms>
RELOAD_RE = re.compile(r"\[ALLOC_RELOAD\]\s+n=(\d+)\s+phase=(pre|post)\s+live=(-?\d+)\s+bytes=(-?\d+)")
TICK_RE = re.compile(r"\[ALLOC_TICK\]\s+n=(\d+)\s+frame=(\d+)\s+live=(-?\d+)\s+bytes=(-?\d+)")
#: analyze() status -> process exit code (0 pass, 1 leak, 2 no evidence / inconclusive).
EXIT_BY_STATUS = {"pass": 0, "leak": 1, "noevidence": 2, "inconclusive": 2}

#: Install subtrees a smoke run never needs; skipping them keeps the copy at ~7 GB.
STAGE_SKIP_DIRS = {"_rdoc", "_pngcap", "debug_state", "crashes", "saved_bookmarks",
                   "terrain_gen_out", "dev_shell_out"}


def log(msg):
    print(f"{PREFIX} {msg}", flush=True)


def fail(msg, code=2):
    print(f"{PREFIX} FAIL: {msg}", file=sys.stderr, flush=True)
    return code


def _tail(path, n=15):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").splitlines()[-n:]
    except OSError:
        return []


def _run(cmd, log_path, env=None):
    """Run a child to completion with stdout+stderr in log_path. -> exit code."""
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log(f"run: {subprocess.list2cmdline([str(c) for c in cmd])}")
    with open(log_path, "w", encoding="utf-8", errors="replace") as fh:
        rc = subprocess.call([str(c) for c in cmd], env=env, stdout=fh,
                             stderr=subprocess.STDOUT, **console_child.child_kwargs(redirected=True))
    log(f"exit={rc} log={log_path}")
    return rc


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def parse_pairs(lines):
    """-> (pairs {n: {"pre": (live, bytes), "post": (live, bytes)}}, ticks [(n, frame, live, bytes)])."""
    pairs, ticks = {}, []
    for line in lines:
        if "[ALLOC_RELOAD]" in line:
            for m in RELOAD_RE.finditer(line):
                pairs.setdefault(int(m.group(1)), {})[m.group(2)] = (int(m.group(3)), int(m.group(4)))
        elif "[ALLOC_TICK]" in line:
            for m in TICK_RE.finditer(line):
                ticks.append(tuple(int(m.group(i)) for i in (1, 2, 3, 4)))
    return pairs, ticks


def analyze(lines, cycles=DEFAULT_CYCLES, tolerance=0):
    """Judge pre/post pairs. -> (status, report lines); status is pass|leak|noevidence|inconclusive."""
    pairs, ticks = parse_pairs(lines)
    complete = sorted(n for n, p in pairs.items() if "pre" in p and "post" in p)
    report = []
    if not complete:
        return "noevidence", ["no [ALLOC_RELOAD] pre/post pair in the log: the fixture never fired "
                              "(or this exe was not built with MC2_ALLOC_STATS=ON)"]
    report.append(f"expected={cycles - 1} reloads (S10_RELOADS={cycles}, cycle 0 quicksaves) "
                  f"captured={len(complete)} cycles_seen={max(n for n in pairs) + 1} "
                  f"alloc_ticks={len(ticks)} tolerance={tolerance}")
    report.append(f"{'n':>3} {'pre_live':>10} {'post_live':>10} {'d_live':>8} "
                  f"{'pre_bytes':>12} {'post_bytes':>12} {'d_bytes':>10}")
    bad = []
    for n in complete:
        pre, post = pairs[n]["pre"], pairs[n]["post"]
        d_live, d_bytes = post[0] - pre[0], post[1] - pre[1]
        report.append(f"{n:>3} {pre[0]:>10} {post[0]:>10} {d_live:>+8} "
                      f"{pre[1]:>12} {post[1]:>12} {d_bytes:>+10}")
        if abs(d_live) > tolerance or abs(d_bytes) > tolerance:
            bad.append(n)
    first, last = pairs[complete[0]]["post"], pairs[complete[-1]]["post"]
    growth_live = last[0] - first[0]
    growth_bytes = last[1] - first[1]
    report.append(f"trend post live {first[0]} -> {last[0]} ({growth_live:+d}), "
                  f"bytes {first[1]} -> {last[1]} ({growth_bytes:+d}) over {len(complete)} reloads")
    steps = [pairs[b]["post"][0] - pairs[a]["post"][0] for a, b in zip(complete, complete[1:])]
    if steps:
        report.append("per-reload post live step: " + " ".join(f"{s:+d}" for s in steps))
    if len(complete) < cycles - 1:
        report.append(f"INCONCLUSIVE: {len(complete)} of {cycles - 1} reload pairs captured; "
                      "raise --duration (the fixture spends 90 render frames per cycle)")
        return "inconclusive", report
    if bad:
        report.append(f"LEAK: post-reload count/bytes not back at pre for cycle(s) " + ", ".join(map(str, bad)))
        return "leak", report
    report.append("PASS: every reload returned to its pre-reload live count and byte total")
    return "pass", report


# ---------------------------------------------------------------------------
# Build / stage / run
# ---------------------------------------------------------------------------

def build_proof(tree, log_dir, jobs):
    """Configure + build an MC2_ALLOC_STATS=ON mc2 in `tree`. -> exit code."""
    cmake = find_cmake()
    args = [cmake, "-S", ROOT.as_posix(), "-B", Path(tree).as_posix(), "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DMC2_ALLOC_STATS=ON"] + cache_args(ROOT.as_posix())
    Path(log_dir).mkdir(parents=True, exist_ok=True)
    cfg_log, build_log = Path(log_dir) / "configure.log", Path(log_dir) / "build.log"
    for path, cmd in ((cfg_log, args), (build_log, [NINJA, "-C", Path(tree).as_posix(), f"-j{jobs}",
                                                    "mc2"])):
        log(f"run: {subprocess.list2cmdline([str(c) for c in cmd])} > {path}")
        rc = under_vcvars(subprocess.list2cmdline([str(c) for c in cmd]) + f' > "{path}" 2>&1')
        log(f"exit={rc} log={path}")
        if rc:
            for line in _tail(path):
                print(line)
            return fail(f"{'configure' if path == cfg_log else 'build'} failed (rc={rc}); see {path}", 3)
    return 0


def stage_install(install, dest, proof_exe):
    """Copy the read-only runtime of `install` to `dest` and put the proof exe in it."""
    install, dest = Path(install), Path(dest)

    def ignore(dirpath, names):
        skip = {n for n in names if n in STAGE_SKIP_DIRS or n.lower().endswith(".pdb")}
        if Path(dirpath) == install:
            skip |= {n for n in names if n.lower().startswith("preview_debug.log")}
        return skip

    log(f"staging {install} -> {dest} (one copy; ~7 GB)")
    shutil.copytree(install, dest, ignore=ignore)
    shutil.copy2(proof_exe, dest / EXE_NAME)


def stage_if_needed(args):
    """-> (stage dir, proof exe) or (None, exit code)."""
    tree = Path(args.build_tree)
    proof_exe = tree / EXE_NAME
    install = Path(args.install)
    if not (install / EXE_NAME).is_file():
        return None, fail(f"--install has no {EXE_NAME}: {install}")
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    stage = Path(args.stage) if args.stage else STAGE_ROOT / stamp
    stages.preflight(stage)
    if not args.stage:
        stages.prune_old(STAGE_ROOT)
    logs = stage.parent / (stage.name + "_logs")
    if args.no_build:
        if not proof_exe.is_file():
            return None, fail(f"proof exe not found: {proof_exe} (run once without --no-build)")
    else:
        rc = build_proof(tree, logs, args.jobs)
        if rc:
            return None, rc
    if (stage / EXE_NAME).is_file() and not args.restage:
        # Reuse only a copy whose exe IS this proof exe (a reused --stage must not run a stale one).
        same = (stage / EXE_NAME).read_bytes() == proof_exe.read_bytes()
        if same:
            log(f"reusing staged copy {stage} (pass --restage to rebuild it)")
            return stage, 0
        log(f"staged exe differs from the proof exe; restaging {stage}")
    if stage.exists():
        shutil.rmtree(stage)
    stage_install(install, stage, proof_exe)
    return stage, 0


def run_fixture(stage, args, logs):
    """Run run_smoke.py against the staged exe. -> (exit code, artifact log path or None)."""
    exe = Path(stage) / EXE_NAME
    env = dict(os.environ, **FIXTURE_ENV)
    env["S10_RELOADS"] = str(args.cycles)
    env.pop("MC2_BOOT_QUICKLOAD_AT", None)  # the fixture reloads; a second boot load would double-count
    cmd = [sys.executable, ROOT / "scripts" / "run_smoke.py", "--mission", args.mission,
           "--duration", str(args.duration), "--exe", exe, "--keep-logs"]
    out = Path(logs) / "run_smoke.log"
    with savegame_backup.guarded(args.save_dir):
        rc = _run(cmd, out, env=env)
    artifacts = None
    if out.is_file():
        for line in out.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("[runner] artifacts: "):
                artifacts = Path(line.split(":", 1)[1].strip())
    mission_log = (artifacts / f"{args.mission}.log") if artifacts else None
    if mission_log is None or not mission_log.is_file():
        return rc, None
    return rc, mission_log


def cmd_run(args):
    stage, rc = stage_if_needed(args)
    if stage is None:
        return rc
    logs = Path(args.log_dir) if args.log_dir else stage.parent / (stage.name + "_logs")
    logs.mkdir(parents=True, exist_ok=True)
    smoke_rc, mission_log = run_fixture(stage, args, logs)
    if mission_log is None:
        for line in _tail(logs / "run_smoke.log"):
            print(line)
        return fail("the smoke run produced no mission log (artifact dir missing)", 2)
    lines = mission_log.read_text(encoding="utf-8", errors="replace").splitlines()
    status, report = analyze(lines, args.cycles, args.tolerance)
    print("\n".join(report))
    log(f"evidence: {mission_log}")
    if smoke_rc:
        print(f"{PREFIX} the smoke run itself failed (rc={smoke_rc}); tail:", file=sys.stderr)
        for line in _tail(logs / "run_smoke.log"):
            print(line, file=sys.stderr)
        return 1
    return EXIT_BY_STATUS[status]


def cmd_analyze(args):
    path = Path(args.log)
    if not path.is_file():
        return fail(f"no such log: {path}")
    status, report = analyze(path.read_text(encoding="utf-8", errors="replace").splitlines(),
                             args.cycles, args.tolerance)
    print("\n".join(report))
    return EXIT_BY_STATUS[status]


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Battery 10-Reload Leak Loop: build an MC2_ALLOC_STATS proof exe, run the "
                    "quicksave-reload fixture 10 times, and check live allocations return to baseline.")
    sub = ap.add_subparsers(dest="cmd")
    run = sub.add_parser("run", help="build, stage, run the fixture, and judge (default)")
    ana = sub.add_parser("analyze", help="judge an existing mission log (no build, no launch)")
    for p in (run, ana):
        p.add_argument("--cycles", type=int, default=DEFAULT_CYCLES,
                       help=f"S10_RELOADS cycles; cycle 0 quicksaves, so N cycles give N-1 reload pairs "
                            f"(default {DEFAULT_CYCLES})")
        p.add_argument("--tolerance", type=int, default=0, metavar="N",
                       help="allowed |post-pre| for live count and bytes, per reload (default 0)")
    run.add_argument("--install", default=DEFAULT_INSTALL,
                     help=f"read-only install to stage from (default {DEFAULT_INSTALL})")
    run.add_argument("--mission", default=DEFAULT_MISSION, help=f"mission stem (default {DEFAULT_MISSION})")
    run.add_argument("--duration", type=int, default=DEFAULT_DURATION,
                     help=f"smoke run seconds; must cover 420 + 90*(cycles-1) render frames "
                          f"(default {DEFAULT_DURATION})")
    run.add_argument("--build-tree", default=str(DEFAULT_BUILD_TREE),
                     help=f"proof build tree (default {DEFAULT_BUILD_TREE.as_posix()})")
    run.add_argument("--stage", default=None, help=f"staged install dir (default {STAGE_ROOT}/<stamp>)")
    run.add_argument("--log-dir", default=None, help="where build/run logs go (default <stage>_logs)")
    run.add_argument("--save-dir", default=None, help="savegame dir for the fixture (default: engine's)")
    run.add_argument("--jobs", type=int, default=int(os.environ.get("MC2_BUILD_JOBS", "2")),
                     help=f"ninja -j for the proof build (default {os.environ.get('MC2_BUILD_JOBS', '2')})")
    run.add_argument("--no-build", action="store_true", help="reuse the proof exe in --build-tree")
    run.add_argument("--restage", action="store_true", help="rebuild the staged copy even if it exists")
    ana.add_argument("log", help="a kept mission log, e.g. tests/smoke/artifacts/<stamp>/mc2_01.log")
    if argv is None:
        argv = sys.argv[1:]
    if not any(a in ("run", "analyze") for a in argv):
        argv = ["run"] + list(argv)  # bare flags are a run; `--help` then shows run's options
    args = ap.parse_args(argv)
    return cmd_analyze(args) if args.cmd == "analyze" else cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())