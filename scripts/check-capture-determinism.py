#!/usr/bin/env python3
"""CAPTURE-DETERMINISM-1 -- capture-noise ratchet.

WHY THIS EXISTS
---------------
Visual A/B is the primary evidence instrument on this project: a lane changes a
gate, captures the same pose before and after, and grades the pixel delta. That
is only valid if two captures of the SAME arm are identical. Measured on
mc2_10 (2026-07-26, exe c0111762, 3 launches, byte-identical env) they were not:

    saved_204937   up to 27.3% of pixels differing (17.4% at >8/255)
    lance_close    up to 23.2%
    saved_204753    up to  9.1%
    saved_212019 / saved_212140     0.000%  (byte-identical)

An effect smaller than that is unmeasurable, and several lanes graded effects
well under it -- believing a delta that was noise. Worse, the artifacts did not
say so: the sidecars matched on frame, pose, seed and `deterministic:true`.

This check is the ratchet. It compares repeated captures of one arm and FAILS
when they differ beyond the threshold, so the instrument's own breakage is
caught instead of being silently inherited by whatever lane uses it next.

MODES
-----
default : grade the newest multi-run capture set found on disk (a dir holding
          r1/, r2/, ... produced by run_visual_capture.py --runs N). SKIP
          (exit 0) when none exists, so this is safe on a machine that has
          never captured -- same contract shape as default_build_quiet.
--run   : actually perform the captures (needs --exe). Opt-in; a contract-suite
          run must not launch the game.

THRESHOLD
---------
--max-pct (default 0.0) is the ceiling on the share of differing pixels between
any two runs of the same arm. The default is 0.0 because the capture path
CLAIMS byte-stability (run_visual_capture.py stamps `deterministic`); any
nonzero value is a documented, deliberate concession, not a default.

DETERMINISM-RATCHET-2 -- three defects found and fixed
------------------------------------------------------
This check was added to stop nondeterministic captures being trusted, and then
did not do that. GOLDEN-REBLESS-2 refused 12 regions for run-to-run noise
(spotlight_towers 20.0%, runway_edge_lights 18.1%, light_clusters_census 15.1%,
lamp_cones 14.7%, far_water_surface 13.0%, light_pools_feathered 8.7%, plus six
under 1.1%) using a MANUAL 3-run comparison. This check saw none of it. Why:

1. BLIND TO THE INSTRUMENT THAT GRADES RELEASES. Artifacts were discovered with
   `rundir.glob("*.png")` -- one flat level. That matches
   `run_visual_capture.py`, which writes `<out>/rN/<mission>_<pose>.png`. It does
   NOT match `tools/visual_verify/capture.py`, the pipeline behind
   `tests/visual/goldens/` -- that writes `<out>/<bookmark>/<mission>_<name>.png`
   and `<out>/crops/<bookmark>/<region>.png`, i.e. one and two levels deep. A
   3-run visual_verify set therefore presented ZERO comparable artifacts, and
   `find_run_sets` skipped its parent dir outright (the `any(runs[0].glob(...))`
   guard). Measured on the real set left by that lane, `scratch/gr2/{r2,r3}`:
   234 PNGs on disk, 0 seen. Artifacts are now discovered with `rglob` and keyed
   by path RELATIVE to the run dir, so both layouts grade.

2. EMPTY WAS A PASS. When a set was found but yielded no comparable pairs, the
   default path printed SKIP and returned 0. That is precisely the failure mode
   defect 1 produces: the check reports success while seeing nothing. An empty
   grade of a set that EXISTS is now a FAIL. (No multi-run set anywhere on disk
   is still a SKIP -- that is "never captured", not "captured and blind".)

3. TWO RUNS COULD BLESS AN UNSTABLE ARTIFACT. `--runs 2` was the default.
   GOLDEN-REBLESS-2 measured `mc2_10_saved_204611 / horizon_white_billboards` at
   0.0000% on r1-vs-r2 and 1.0163% on r1-vs-r3: byte-perfect on the pair a 2-run
   check compares, unstable on the pair it never looks at. `lamp_cones` is
   bidirectional the other way (14.65% r1-v-r2, 0.13% r1-v-r3). Grading now
   requires `--min-runs` (default 3) run dirs and compares ALL pairs; a set with
   fewer is a FAIL, not a silent pass.
"""
from __future__ import annotations

import argparse
import itertools
import json
import subprocess
import sys
from pathlib import Path
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)

ROOT = Path(__file__).resolve().parents[1]
# Scanned by the DEFAULT (contract-suite) mode. Deliberately NOT all of
# scratch/: a lane's ad-hoc capture experiment must not fail the global suite,
# and scratch dirs routinely hold sets a lane already knows are unstable. These
# two are the designated ratchet locations -- `--run` writes to the first, and
# run_visual_capture.py's default --out-dir is the second. Grade anything else
# explicitly with --set-dir.
DEFAULT_SEARCH = [
    ROOT / "scratch" / "capdet_ratchet",
    ROOT / "tests" / "visual" / "captures",
]


def _load(p):
    from PIL import Image
    import numpy as np
    return np.asarray(Image.open(p).convert("RGB")).astype(np.int16)


def _pct_diff(a, b):
    import numpy as np
    if a.shape != b.shape:
        return None, None
    m = np.abs(a - b).max(axis=2)
    n = m.size
    return (100.0 * float((m > 0).sum()) / n,
            100.0 * float((m > 8).sum()) / n)


def run_dirs(d):
    """The rN/ subdirs of `d`, ordered by N."""
    return sorted([c for c in d.iterdir()
                   if c.is_dir() and c.name.startswith("r")
                   and c.name[1:].isdigit()],
                  key=lambda p: int(p.name[1:]))


def artifacts(rundir):
    """{relative posix path -> Path} for every PNG under a run dir.

    RECURSIVE and keyed by RELATIVE path (DETERMINISM-RATCHET-2 defect 1). A
    flat `glob("*.png")` matched run_visual_capture.py's layout only and was
    blind to tools/visual_verify/capture.py -- the pipeline that produces the
    golden crops which actually grade releases. Keying by relative path (not
    basename) also keeps `crops/poseA/lamp_cones.png` distinct from
    `crops/poseB/lamp_cones.png`, which a basename key would have collided.
    """
    return {p.relative_to(rundir).as_posix(): p for p in rundir.rglob("*.png")}


def find_run_sets(roots):
    """Yield dirs that directly contain >=2 subdirs named r1, r2, ..."""
    out = []
    for root in roots:
        if not root.exists():
            continue
        for d in root.rglob("*"):
            if not d.is_dir():
                continue
            runs = run_dirs(d)
            if len(runs) >= 2 and artifacts(runs[0]):
                out.append((d, runs))
    out.sort(key=lambda t: max(r.stat().st_mtime for r in t[1]), reverse=True)
    return out


def grade(setdir, runs, max_pct, quiet=False):
    """Return (verdict, rows, worst). verdict in PASS/FAIL/EMPTY."""
    names = {}
    per_run = {r: artifacts(r) for r in runs}
    for r in runs:
        for rel in per_run[r]:
            names.setdefault(rel, []).append(r)
    rows = []
    worst = 0.0
    for nm in sorted(names):
        present = [r for r in runs if nm in per_run[r]]
        if len(present) < 2:
            continue
        imgs = {r.name: _load(per_run[r][nm]) for r in present}
        for x, y in itertools.combinations(sorted(imgs), 2):
            pct, pct8 = _pct_diff(imgs[x], imgs[y])
            if pct is None:
                rows.append((nm, f"{x}v{y}", None, None, "SHAPE-MISMATCH"))
                worst = max(worst, 100.0)
                continue
            worst = max(worst, pct)
            rows.append((nm, f"{x}v{y}", pct, pct8,
                         "ok" if pct <= max_pct else "OVER"))
    verdict = "PASS" if worst <= max_pct and rows else ("FAIL" if rows else "EMPTY")
    if not quiet or verdict != "PASS":
        print(f"[capdet] set: {setdir}")
        print(f"[capdet] runs: {len(runs)} ({', '.join(r.name for r in runs)})  "
              f"artifacts: {len(names)}  pairs graded: {len(rows)}")
        print(f"[capdet] {'artifact':58} {'pair':9} {'pct_any':>8} {'pct_gt8':>8}  flag")
        for nm, pair, pct, pct8, flag in rows:
            if flag == "ok" and quiet:
                continue
            ps = "  n/a  " if pct is None else f"{pct:8.3f}"
            p8 = "  n/a  " if pct8 is None else f"{pct8:8.3f}"
            print(f"[capdet] {nm:58} {pair:9} {ps} {p8}  {flag}")
        print(f"[capdet] worst pct_any={worst:.3f} threshold={max_pct:.3f} -> {verdict}")
    if verdict == "EMPTY":
        # DETERMINISM-RATCHET-2 defect 2: a set that exists but yields nothing
        # comparable is the exact shape of this check's own past failure. Say so.
        print(f"[capdet] FAIL: set {setdir} exists with "
              f"{len(runs)} run dir(s) but produced NO comparable artifact "
              f"pairs. A ratchet that sees nothing must not report success.")
    return verdict, rows, worst


def grade_or_fail(setdir, runs, args):
    """Enforce the min-runs rule, then grade. Returns a process exit code."""
    if len(runs) < args.min_runs:
        # DETERMINISM-RATCHET-2 defect 3.
        print(f"[capdet] FAIL: set {setdir} has {len(runs)} run dir(s), "
              f"need >= {args.min_runs}. Two runs can agree byte-perfectly on a "
              f"region that is unstable against a third "
              f"(measured: horizon_white_billboards 0.0000% r1-v-r2, "
              f"1.0163% r1-v-r3).")
        return 1
    verdict, _, _ = grade(setdir, runs, args.max_pct, args.quiet)
    return 0 if verdict == "PASS" else 1


def do_run(exe, mission, bookmarks, outdir, runs, duration, settle):
    cmd = [sys.executable, str(ROOT / "scripts" / "run_visual_capture.py"),
           "--mission", mission, "--exe", str(exe),
           "--out-dir", str(outdir), "--runs", str(runs),
           "--duration", str(duration), "--settle", str(settle), "--no-kill"]
    if bookmarks:
        cmd += ["--bookmarks", str(bookmarks)]
    print(f"[capdet] capturing: {' '.join(cmd)}")
    subprocess.run(cmd, check=False, **console_child.child_kwargs())


def selftest() -> int:
    """Prove the ratchet fires. DETERMINISM-RATCHET-2.

    Every defect this lane fixed had the same signature: the check returned 0
    while the thing it exists to catch was sitting on disk. A ratchet nobody
    ever watched fail is indistinguishable from one that cannot fail, so this
    builds synthetic bad input and asserts a nonzero exit.

    Self-contained (writes only to a temp dir) and offline -- no game launch.
    """
    import tempfile
    from PIL import Image

    def png(path, colour):
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.new("RGB", (64, 64), colour).save(path)

    ok = True
    with tempfile.TemporaryDirectory(prefix="capdet_selftest_") as td:
        tmp = Path(td)
        base = ["--max-pct", "0.0", "--quiet"]

        def run(setdir, extra=()):
            argv = [sys.executable, str(Path(__file__).resolve()),
                    "--set-dir", str(setdir), *base, *extra]
            p = subprocess.run(argv, capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            return p.returncode, (p.stdout + p.stderr)

        def expect(label, got, want):
            nonlocal ok
            good = got == want
            ok &= good
            print(f"[capdet:selftest] {'PASS' if good else 'FAIL'}  {label} "
                  f"(exit {got}, expected {want})")

        # (a) NESTED layout, noisy. This is the real-world case that was
        #     invisible: tools/visual_verify writes crops/<pose>/<region>.png,
        #     two levels deep. A flat glob saw nothing and reported success.
        noisy = tmp / "noisy"
        for r, shade in (("r1", (10, 10, 10)), ("r2", (200, 10, 10)),
                         ("r3", (10, 10, 10))):
            png(noisy / r / "crops" / "pose_a" / "lamp_cones.png", shade)
        expect("nested noisy set must FAIL", run(noisy)[0], 1)

        # (b) Same set, but only r1 and r2 -- the pair that AGREES. A 2-run
        #     check blesses this; the 3-run rule refuses to grade it at all.
        two = tmp / "tworun"
        for r, shade in (("r1", (10, 10, 10)), ("r2", (10, 10, 10))):
            png(two / r / "crops" / "pose_a" / "lamp_cones.png", shade)
        expect("2-run set must FAIL the min-runs rule", run(two)[0], 1)

        # (c) Set exists, 3 runs, but no artifact name is shared -> nothing
        #     comparable. Reporting empty IS the defect; it must be loud.
        empty = tmp / "empty"
        for i, r in enumerate(("r1", "r2", "r3")):
            png(empty / r / f"dir{i}" / f"only_in_{r}.png", (0, 0, 0))
        code, out = run(empty)
        expect("EMPTY set must FAIL", code, 1)
        if "NO comparable artifact pairs" not in out:
            ok = False
            print("[capdet:selftest] FAIL  EMPTY must say so explicitly")

        # (d) Control: a clean nested 3-run set must still PASS, otherwise the
        #     ratchet is merely always-red and equally uninformative.
        clean = tmp / "clean"
        for r in ("r1", "r2", "r3"):
            png(clean / r / "crops" / "pose_a" / "lamp_cones.png", (10, 10, 10))
        expect("clean nested set must PASS", run(clean)[0], 0)

        # (e) An explicit --set-dir that does not exist is a caller error.
        expect("missing --set-dir must FAIL", run(tmp / "nope")[0], 1)

    print(f"[capdet:selftest] {'ALL PASS' if ok else 'SELFTEST FAILED'}")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-pct", type=float, default=0.0,
                    help="ceiling on %% of differing pixels between two runs "
                         "of the SAME arm (default 0.0 = byte-stable, which is "
                         "what the capture path claims)")
    ap.add_argument("--min-runs", type=int, default=3,
                    help="minimum run dirs required to grade a set (default 3). "
                         "Two is not enough: a region can be byte-identical on "
                         "r1-v-r2 and unstable on r1-v-r3.")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--set-dir", default=None,
                    help="grade this specific dir (must contain r1/, r2/, ...)")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the ratchet fires on synthetic bad input "
                         "(offline, temp dir only, no game launch)")
    ap.add_argument("--run", action="store_true",
                    help="perform the captures instead of grading existing ones")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--mission", default="mc2_10")
    ap.add_argument("--bookmarks", default=None)
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--runs", type=int, default=3,
                    help="--run mode: how many captures to perform (default 3, "
                         "matching --min-runs)")
    ap.add_argument("--duration", type=int, default=60)
    ap.add_argument("--settle", type=int, default=30)
    args = ap.parse_args()

    try:
        import PIL, numpy  # noqa: F401
    except ImportError:
        print("[capdet] SKIP: Pillow/numpy unavailable (cannot diff PNGs)")
        return 0

    if args.selftest:
        return selftest()

    if args.run:
        if not args.exe:
            print("[capdet] ERROR --run requires --exe", file=sys.stderr)
            return 2
        out = Path(args.out_dir) if args.out_dir else (
            ROOT / "scratch" / "capdet_ratchet" / args.mission)
        do_run(args.exe, args.mission, args.bookmarks, out, args.runs,
               args.duration, args.settle)
        return grade_or_fail(out, run_dirs(out), args)

    if args.set_dir:
        d = Path(args.set_dir)
        if not d.is_dir():
            # An EXPLICIT --set-dir that does not exist is a caller error, not a
            # clean machine. Do not launder it into a pass.
            print(f"[capdet] FAIL: --set-dir {d} does not exist")
            return 1
        return grade_or_fail(d, run_dirs(d), args)

    sets = find_run_sets(DEFAULT_SEARCH)
    if not sets:
        # The ONLY remaining skip: nothing was ever captured on this machine.
        print("[capdet] SKIP: no multi-run capture set on disk "
              "(run_visual_capture.py --runs N produces one). "
              "This check grades the newest set; it never launches the game.")
        return 0
    setdir, runs = sets[0]
    return grade_or_fail(setdir, runs, args)


if __name__ == "__main__":
    sys.exit(main())
