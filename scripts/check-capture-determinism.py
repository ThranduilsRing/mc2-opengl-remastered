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
"""
from __future__ import annotations

import argparse
import itertools
import json
import subprocess
import sys
from pathlib import Path

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


def find_run_sets(roots):
    """Yield dirs that directly contain >=2 subdirs named r1, r2, ..."""
    out = []
    for root in roots:
        if not root.exists():
            continue
        for d in root.rglob("*"):
            if not d.is_dir():
                continue
            runs = sorted([c for c in d.iterdir()
                           if c.is_dir() and c.name.startswith("r")
                           and c.name[1:].isdigit()],
                          key=lambda p: int(p.name[1:]))
            if len(runs) >= 2 and any(runs[0].glob("*.png")):
                out.append((d, runs))
    out.sort(key=lambda t: max(r.stat().st_mtime for r in t[1]), reverse=True)
    return out


def grade(setdir, runs, max_pct, quiet=False):
    """Return (verdict, rows). verdict in PASS/FAIL."""
    names = {}
    for r in runs:
        for png in r.glob("*.png"):
            names.setdefault(png.name, []).append(r)
    rows = []
    worst = 0.0
    for nm in sorted(names):
        present = [r for r in runs if (r / nm).exists()]
        if len(present) < 2:
            continue
        imgs = {r.name: _load(r / nm) for r in present}
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
        print(f"[capdet] {'artifact':40} {'pair':9} {'pct_any':>8} {'pct_gt8':>8}  flag")
        for nm, pair, pct, pct8, flag in rows:
            if flag == "ok" and quiet:
                continue
            ps = "  n/a  " if pct is None else f"{pct:8.3f}"
            p8 = "  n/a  " if pct8 is None else f"{pct8:8.3f}"
            print(f"[capdet] {nm:40} {pair:9} {ps} {p8}  {flag}")
        print(f"[capdet] worst pct_any={worst:.3f} threshold={max_pct:.3f} -> {verdict}")
    return verdict, rows, worst


def do_run(exe, mission, bookmarks, outdir, runs, duration, settle):
    cmd = [sys.executable, str(ROOT / "scripts" / "run_visual_capture.py"),
           "--mission", mission, "--exe", str(exe),
           "--out-dir", str(outdir), "--runs", str(runs),
           "--duration", str(duration), "--settle", str(settle), "--no-kill"]
    if bookmarks:
        cmd += ["--bookmarks", str(bookmarks)]
    print(f"[capdet] capturing: {' '.join(cmd)}")
    subprocess.run(cmd, check=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-pct", type=float, default=0.0,
                    help="ceiling on %% of differing pixels between two runs "
                         "of the SAME arm (default 0.0 = byte-stable, which is "
                         "what the capture path claims)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--set-dir", default=None,
                    help="grade this specific dir (must contain r1/, r2/, ...)")
    ap.add_argument("--run", action="store_true",
                    help="perform the captures instead of grading existing ones")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--mission", default="mc2_10")
    ap.add_argument("--bookmarks", default=None)
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--duration", type=int, default=60)
    ap.add_argument("--settle", type=int, default=30)
    args = ap.parse_args()

    try:
        import PIL, numpy  # noqa: F401
    except ImportError:
        print("[capdet] SKIP: Pillow/numpy unavailable (cannot diff PNGs)")
        return 0

    if args.run:
        if not args.exe:
            print("[capdet] ERROR --run requires --exe", file=sys.stderr)
            return 2
        out = Path(args.out_dir) if args.out_dir else (
            ROOT / "scratch" / "capdet_ratchet" / args.mission)
        do_run(args.exe, args.mission, args.bookmarks, out, args.runs,
               args.duration, args.settle)
        runs = sorted([c for c in out.iterdir()
                       if c.is_dir() and c.name.startswith("r")],
                      key=lambda p: int(p.name[1:]))
        if len(runs) < 2:
            print("[capdet] FAIL: capture did not produce >=2 run dirs")
            return 1
        verdict, _, _ = grade(out, runs, args.max_pct, args.quiet)
        return 0 if verdict == "PASS" else 1

    if args.set_dir:
        d = Path(args.set_dir)
        if not d.is_dir():
            print(f"[capdet] SKIP: {d} does not exist")
            return 0
        runs = sorted([c for c in d.iterdir()
                       if c.is_dir() and c.name.startswith("r")
                       and c.name[1:].isdigit()],
                      key=lambda p: int(p.name[1:]))
        if len(runs) < 2:
            print(f"[capdet] SKIP: {d} has <2 run dirs")
            return 0
        verdict, _, _ = grade(d, runs, args.max_pct, args.quiet)
        return 0 if verdict == "PASS" else 1

    sets = find_run_sets(DEFAULT_SEARCH)
    if not sets:
        print("[capdet] SKIP: no multi-run capture set on disk "
              "(run_visual_capture.py --runs N produces one). "
              "This check grades the newest set; it never launches the game.")
        return 0
    setdir, runs = sets[0]
    verdict, _, _ = grade(setdir, runs, args.max_pct, args.quiet)
    if verdict == "EMPTY":
        print("[capdet] SKIP: newest set had no comparable artifact pairs")
        return 0
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
