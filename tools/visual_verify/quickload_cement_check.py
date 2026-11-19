#!/usr/bin/env python3
"""quickload_cement_check - does the airport cement pad / runway overlay survive a quickload?

WHAT: runs mc2_01 twice through scripts/run_visual_capture.py (same exe, same bookmarks): a control with no load, and a run that quicksaves at
frame 1000 and quickloads at frame 1100 (MC2_BOOT_QUICKSAVE_AT / MC2_BOOT_QUICKLOAD_AT), both captured at trigger frame 1500. Then, for the
runway_overview bookmark only:
  1. NEGATIVE CONTROLS (offline, on the control PNG, first): a copy with the pad interior painted over with terrain colour must FAIL the cement
     test, and a vertically flipped copy (a different view) must FAIL the pose test; the clean copy must pass both. A comparator that passes
     either control is itself broken and the run stops.
  2. POSE: both PNGs must show the intended pose. The sidecar pose must equal the bookmark's (tests/visual/bookmarks/mc2_01.json) AND the PNG
     must look like the recorded runway thumbnail (tools/visual_verify/quickload_cement_baseline.png, mean abs difference <= --pose-tol of 255
     on a 96x54 thumbnail). The sidecar alone is not proof: a delayed present readback once delivered another pose's pixels under a correct
     sidecar, and the s55 "PASS" compared two horizon/sky views.
  3. CEMENT: the pixels that differ between the control and the quickloaded run, AFTER a 5x5 erosion, must number at most --max-px (default 2000).
     The erosion drops the one-to-three pixel outlines of dithered alpha-tested edges (building rims, tree canopies) and keeps an area loss; the
     bug measured about 315000 (the pad interior and the runway art vanish and only the pad outline stays). A moving object (a turret that
     differs after a load) is a few hundred pixels.
WHY: Mission::load is a parallel init path to Mission::init; the per-frame tex_resolve memo made the cement atlas build see stale texture handles
when the load ran mid-frame (BUGS-FOUND-FIXED row 56). Nothing else compares a loaded mission with a fresh one.
Each capture saves into a private MC2_SAVE_DIR under its out dir, never the owner's folder.

  py -3 tools/visual_verify/quickload_cement_check.py [--exe PATH] [--out-dir DIR] [--max-px N] [--pose-tol T]
  py -3 tools/visual_verify/quickload_cement_check.py --selftest CONTROL.png          # negative controls only
  py -3 tools/visual_verify/quickload_cement_check.py --record-baseline CONTROL.png   # re-record the thumbnail from a VIEWED correct capture
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402  (windowless console children; scripts/console_child.py)

BOOKMARK = "mc2_01_runway_overview.png"
BASELINE = os.path.join(HERE, "quickload_cement_baseline.png")
BOOKMARKS = os.path.join(ROOT, "tests", "visual", "bookmarks", "mc2_01.json")
THUMB = (96, 54)
PAD_BOX = (500, 120, 880, 420)       # the apron interior left of the hangar at 1920x1080
TERRAIN_SAMPLE = (200, 300)          # open desert terrain at the same view


def capture(exe: str, out: str, load: bool) -> int:
    args = [sys.executable, os.path.join(ROOT, "scripts", "run_visual_capture.py"), "--mission", "mc2_01", "--out-dir", out,
            "--trigger-frame", "1500", "--duration", "150", "--no-kill"]
    if exe:
        args += ["--exe", exe]
    if load:
        args += ["--env-declare", "MC2_BOOT_QUICKSAVE_AT=1000", "--env-declare", "MC2_BOOT_QUICKLOAD_AT=1100"]
    env = dict(os.environ, MC2_SAVE_DIR=os.path.join(out, "savegame"))
    return subprocess.run(args, cwd=ROOT, env=env, **console_child.child_kwargs()).returncode


def _open(p):
    from PIL import Image
    return Image.open(p).convert("RGB")


def raw_diff_px(a: str, b: str) -> int:
    from PIL import ImageChops
    d = ImageChops.difference(_open(a), _open(b)).convert("L")
    return sum(1 for p in d.getdata() if p > 8)


def diff_px(a: str, b: str) -> int:
    """Pixels of the difference that survive a 5x5 erosion: an area loss counts, outline speckle does not."""
    from PIL import ImageChops, ImageFilter
    d = ImageChops.difference(_open(a), _open(b)).convert("L").point(lambda v: 255 if v > 8 else 0)
    d = d.filter(ImageFilter.MinFilter(5))
    return sum(1 for p in d.getdata() if p)


def thumb_mad(a: str, b: str) -> float:
    from PIL import Image, ImageChops
    ta = _open(a).resize(THUMB, Image.BILINEAR)
    tb = _open(b).resize(THUMB, Image.BILINEAR)
    d = list(ImageChops.difference(ta, tb).convert("L").getdata())
    return sum(d) / len(d)


def sidecar_pose_ok(json_path: str, bookmark: str = "runway_overview") -> tuple[bool, str]:
    """The sidecar's rendered pose equals the bookmark's (position xy, rotation, projection, altitude)."""
    with open(BOOKMARKS) as f:
        bm = next(m for m in json.load(f)["bookmarks"] if m["name"] == bookmark)
    with open(json_path) as f:
        sc = json.load(f)
    got = (sc["cap_pos"][0], sc["cap_pos"][1], sc["cap_rot"], sc["cap_proj"], sc["cap_alt"])
    want = (bm["pos"][0], bm["pos"][1], bm["rot"], bm["proj"], bm["alt"])
    bad = [(g, w) for g, w in zip(got, want) if abs(g - w) > 0.05]
    return (not bad), f"sidecar pose {got} vs bookmark {want}"


def paint_pad_missing(src: str, dst: str) -> None:
    """The deliberately missing cement pad: the apron interior painted with the terrain colour."""
    im = _open(src)
    im.paste(im.getpixel(TERRAIN_SAMPLE), PAD_BOX)
    im.save(dst)


def flip_view(src: str, dst: str) -> None:
    from PIL import ImageOps
    ImageOps.flip(_open(src)).save(dst)


def selftest(control_png: str, max_px: int, pose_tol: float, workdir: str) -> bool:
    """Each control must FAIL the comparator it targets; the clean copy must pass both."""
    os.makedirs(workdir, exist_ok=True)
    missing = os.path.join(workdir, "neg_missing_pad.png")
    flipped = os.path.join(workdir, "neg_other_view.png")
    paint_pad_missing(control_png, missing)
    flip_view(control_png, flipped)
    clean_px, miss_px = diff_px(control_png, control_png), diff_px(control_png, missing)
    clean_mad, flip_mad = thumb_mad(control_png, BASELINE), thumb_mad(flipped, BASELINE)
    ok = True
    for name, good in (("clean copy passes the cement test", clean_px <= max_px), ("missing pad FAILS the cement test", miss_px > max_px),
                       ("clean control matches the runway thumbnail", clean_mad <= pose_tol), ("other view FAILS the pose test", flip_mad > pose_tol)):
        print(f"  selftest {'ok ' if good else 'BAD'} {name}")
        ok = ok and good
    print(f"  selftest numbers: clean_px={clean_px} missing_pad_px={miss_px} (max {max_px}); thumb_mad clean={clean_mad:.2f} flipped={flip_mad:.2f} (tol {pose_tol})")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, ".scratch", "quickload_cement"))
    ap.add_argument("--max-px", type=int, default=2000)
    ap.add_argument("--pose-tol", type=float, default=16.0)
    ap.add_argument("--selftest", default="", metavar="CONTROL.png")
    ap.add_argument("--record-baseline", default="", metavar="CONTROL.png")
    a = ap.parse_args()
    if a.record_baseline:
        from PIL import Image
        _open(a.record_baseline).resize(THUMB, Image.BILINEAR).save(BASELINE)
        print(f"recorded {BASELINE}")
        return 0
    if a.selftest:
        return 0 if selftest(a.selftest, a.max_px, a.pose_tol, os.path.join(a.out_dir, "selftest")) else 3
    ctl, ql = os.path.join(a.out_dir, "ctl"), os.path.join(a.out_dir, "ql")
    if capture(a.exe, ctl, False) or capture(a.exe, ql, True):
        print("FAIL: a capture run failed")
        return 2
    ctl_png, ql_png = os.path.join(ctl, BOOKMARK), os.path.join(ql, BOOKMARK)
    if not (os.path.exists(ctl_png) and os.path.exists(ql_png)):
        print(f"FAIL: {BOOKMARK} was not captured")
        return 2
    if not selftest(ctl_png, a.max_px, a.pose_tol, os.path.join(a.out_dir, "selftest")):
        print("FAIL: a negative control did not behave; the comparators cannot be trusted")
        return 3
    for f in sorted(glob.glob(os.path.join(ql, "*.png"))):
        g = os.path.join(ctl, os.path.basename(f))
        if os.path.exists(g) and os.path.basename(f) != BOOKMARK:
            print(f"{os.path.basename(f)} raw={raw_diff_px(f, g)} eroded={diff_px(f, g)} (informational)")
    ok = True
    for arm, d in (("ctl", ctl), ("ql", ql)):
        good, why = sidecar_pose_ok(os.path.join(d, BOOKMARK[:-4] + ".json"))
        mad = thumb_mad(os.path.join(d, BOOKMARK), BASELINE)
        print(f"POSE {arm}: {'ok' if good else 'BAD'} ({why}); thumbnail MAD vs baseline {mad:.2f} (tol {a.pose_tol})")
        ok = ok and good and mad <= a.pose_tol
    if not ok:
        print("FAIL: a capture does not show the intended runway_overview pose; the comparison would be meaningless")
        return 4
    n = diff_px(ql_png, ctl_png)
    print(f"{BOOKMARK} raw={raw_diff_px(ql_png, ctl_png)} eroded={n}")
    ok = n <= a.max_px
    print(("PASS" if ok else "FAIL") + f": {BOOKMARK} cement/runway area differs by {n} px after erosion (max {a.max_px})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
