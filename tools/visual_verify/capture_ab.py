#!/usr/bin/env python3
"""What: before/after capture sets (GL twice + VK) for a code change that should not alter the picture, and the compact diff lines between two sets.
Why: judging a refactor by eye needs the run-to-run noise floor next to the before/after numbers; this prints both as one-line verdicts per bookmark,
so only a result clearly above the noise floor needs a full montage.

Usage:
  py -3 tools/visual_verify/capture_ab.py capture --install DIR --tag TAG [--root DIR] [--bookmarks a,b]
      writes <root>/<TAG>1-gl, <root>/<TAG>2-gl and <root>/<TAG>-vk (capture.py --clean; VK via MC2_RENDER_BACKEND=vulkan).
      Take the BEFORE set on the old install first, deploy the change, then take the AFTER set.
  py -3 tools/visual_verify/capture_ab.py compare --before TAG --after TAG [--root DIR] [--bookmarks a,b]
      prints, per bookmark: NOISE (before GL1 vs before GL2), GL (before GL1 vs after GL1 and GL2), VK (before vs after), each as montage_compact's DIFF line.
Launches go through capture.py only (lease, background-only, no window)."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "scripts"))

import console_child  # noqa: E402
import montage_compact as mc  # noqa: E402

DEFAULT_BOOKMARKS = "mc2_01_saved_205644,mc2_10_saved_204523"
DEFAULT_ROOT = os.path.join(REPO, ".claude", "l2cap")


def diff_line(before_png: str, after_png: str) -> str:
    """The montage_compact DIFF summary of two frames, or why they cannot be compared."""
    try:
        before, after = mc.load_rgb(before_png), mc.load_rgb(after_png)
    except OSError as e:
        return f"MISSING {e}"
    if before.shape != after.shape:
        return f"SIZE-MISMATCH {before.shape[1]}x{before.shape[0]} vs {after.shape[1]}x{after.shape[0]}"
    clusters = mc.clusters_of(mc.diff_mask(before, after))
    outside = sum(c["px"] for c in clusters)
    big = max(clusters, key=lambda c: c["w"] * c["h"], default=None)
    biggest = f"{big['w']}x{big['h']}@{big['x']},{big['y']}" if big else "0x0@0,0"
    return f"DIFF outside_box={outside} clusters={len(clusters)} max_cluster={biggest} inside_box=0"


def frame(root: str, set_name: str, bookmark: str) -> str:
    return os.path.join(root, set_name, bookmark, bookmark + ".png")


def compare(root: str, before: str, after: str, bookmarks: list[str]) -> list[str]:
    """One line per comparison per bookmark: the noise floor first, then the before/after pairs."""
    out = []
    for bm in bookmarks:
        pairs = [
            ("NOISE GL before1->before2", f"{before}1-gl", f"{before}2-gl"),
            ("GL before1->after1", f"{before}1-gl", f"{after}1-gl"),
            ("GL before1->after2", f"{before}1-gl", f"{after}2-gl"),
            ("VK before->after", f"{before}-vk", f"{after}-vk"),
        ]
        for label, a, b in pairs:
            out.append(f"{bm} {label}: {diff_line(frame(root, a, bm), frame(root, b, bm))}")
    return out


def run_capture(install: str, out_dir: str, bookmarks: str, vulkan: bool, freeze_anim: bool = True) -> int:
    cmd = [sys.executable, os.path.join(HERE, "capture.py"), "--install", install, "--bookmarks", bookmarks,
           "--out", out_dir, "--clean"]
    if freeze_anim:
        cmd += ["--env", "MC2_SMOKE_FREEZE_ANIM=1"]      # no animation-phase noise in the A/B (needs an exe built with the gate, on BOTH sets)
    if vulkan:
        cmd += ["--env", "MC2_RENDER_BACKEND=vulkan"]
    env = dict(os.environ, MC2_CURSOR_NO_PARK="1")
    print("[capture_ab] " + " ".join(cmd[1:]), flush=True)
    return subprocess.run(cmd, cwd=REPO, env=env, **console_child.child_kwargs()).returncode


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("--install", required=True)
    c.add_argument("--tag", required=True)
    c.add_argument("--root", default=DEFAULT_ROOT)
    c.add_argument("--bookmarks", default=DEFAULT_BOOKMARKS)
    c.add_argument("--keep-anim", action="store_true", help="do NOT set MC2_SMOKE_FREEZE_ANIM=1 (animations then carry phase noise between sets)")
    d = sub.add_parser("compare")
    d.add_argument("--before", required=True)
    d.add_argument("--after", required=True)
    d.add_argument("--root", default=DEFAULT_ROOT)
    d.add_argument("--bookmarks", default=DEFAULT_BOOKMARKS)
    args = ap.parse_args(argv)
    if args.cmd == "compare":
        print("\n".join(compare(args.root, args.before, args.after, args.bookmarks.split(","))))
        return 0
    os.makedirs(args.root, exist_ok=True)
    rc = 0
    for name, vk in ((f"{args.tag}1-gl", False), (f"{args.tag}2-gl", False), (f"{args.tag}-vk", True)):
        rc = run_capture(args.install, os.path.join(args.root, name), args.bookmarks, vk, not args.keep_anim) or rc
    return rc


if __name__ == "__main__":
    sys.exit(main())
