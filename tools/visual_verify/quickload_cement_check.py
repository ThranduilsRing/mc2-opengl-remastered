#!/usr/bin/env python3
"""quickload_cement_check - does the airport cement pad / runway overlay survive a quickload?

WHAT: runs mc2_01 twice through scripts/run_visual_capture.py (same exe, same bookmarks): a control with no load, and a run that quicksaves at
frame 1000 and quickloads at frame 1100 (MC2_BOOT_QUICKSAVE_AT / MC2_BOOT_QUICKLOAD_AT), both captured at trigger frame 1500. Then counts the
pixels that differ per bookmark. FAIL when runway_overview differs by more than --max-px (default 2000; the fixed build measures under 100,
the bug measured about 315000: the pad interior and the runway art vanish and only the pad outline stays).
WHY: Mission::load is a parallel init path to Mission::init; the per-frame tex_resolve memo made the cement atlas build see stale texture handles
when the load ran mid-frame (BUGS-FOUND-FIXED row 56). Nothing else compares a loaded mission with a fresh one.
The run replaces the owner's quicksave, so it goes through savegame_backup.guarded().

  py -3 tools/visual_verify/quickload_cement_check.py [--exe PATH] [--out-dir DIR] [--max-px N]
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402  (windowless console children; scripts/console_child.py)
import savegame_backup  # noqa: E402

BOOKMARK = "mc2_01_runway_overview.png"


def capture(exe: str, out: str, load: bool) -> int:
    args = [sys.executable, os.path.join(ROOT, "scripts", "run_visual_capture.py"), "--mission", "mc2_01", "--out-dir", out,
            "--trigger-frame", "1500", "--duration", "150", "--no-kill"]
    if exe:
        args += ["--exe", exe]
    if load:
        args += ["--env-declare", "MC2_BOOT_QUICKSAVE_AT=1000", "--env-declare", "MC2_BOOT_QUICKLOAD_AT=1100"]
    return subprocess.run(args, cwd=ROOT, **console_child.child_kwargs()).returncode


def diff_px(a: str, b: str) -> int:
    from PIL import Image, ImageChops
    d = ImageChops.difference(Image.open(a).convert("RGB"), Image.open(b).convert("RGB")).convert("L")
    return sum(1 for p in d.getdata() if p > 8)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, ".scratch", "quickload_cement"))
    ap.add_argument("--max-px", type=int, default=2000)
    a = ap.parse_args()
    ctl, ql = os.path.join(a.out_dir, "ctl"), os.path.join(a.out_dir, "ql")
    with savegame_backup.guarded():
        if capture(a.exe, ctl, False) or capture(a.exe, ql, True):
            print("FAIL: a capture run failed")
            return 2
    worst = 0
    for f in sorted(glob.glob(os.path.join(ql, "*.png"))):
        g = os.path.join(ctl, os.path.basename(f))
        if os.path.exists(g):
            n = diff_px(f, g)
            print(f"{os.path.basename(f)} {n}")
            if os.path.basename(f) == BOOKMARK:
                worst = n
    ok = worst <= a.max_px
    print(("PASS" if ok else "FAIL") + f": {BOOKMARK} differs by {worst} px (max {a.max_px})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
