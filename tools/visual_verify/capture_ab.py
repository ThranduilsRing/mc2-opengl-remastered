#!/usr/bin/env python3
"""What: before/after capture sets (Vulkan twice) for a code change that should not alter the picture, and the compact diff lines between two sets.
Why: judging a refactor by eye needs the run-to-run noise floor next to the before/after numbers; this prints both as one-line verdicts per bookmark,
so only a result clearly above the noise floor needs a full montage.

Usage:
  py -3 tools/visual_verify/capture_ab.py capture --install DIR --tag TAG [--root DIR] [--bookmarks a,b]
      writes <root>/<TAG>1-vk and <root>/<TAG>2-vk (capture.py --clean; VK via the umbrella MC2_RENDER_BACKEND=vulkan
      AND MC2_VK_SHARED_GBUFFER=1: the selector alone makes the engine force GL and the "VK" set would be a GL capture). Take the BEFORE set on the old install first, deploy the change, then take the AFTER set.
  py -3 tools/visual_verify/capture_ab.py compare --before TAG --after TAG [--root DIR] [--bookmarks a,b]
      prints, per bookmark: NOISE (before VK1 vs before VK2), VK (before VK1 vs after VK1 and VK2), each as a montage_compact DIFF line.
      Refuses (exit 1) sets whose render options differ (--vary names a deliberate difference) and sets that were not drawn by the backend their name
      says: a "-vk" set whose sidecars say GL is not Vulkan evidence.
Launches go through capture.py only (lease, background-only, no window)."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "scripts"))

import console_child  # noqa: E402
import montage_compact as mc  # noqa: E402
from smoke_lib import capture_backend, render_options_pin  # noqa: E402

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


def arms(tag: str) -> "list[tuple[str, str]]":
    """The (set name, backend) pairs of one tag: two Vulkan sets, the second one is the noise floor."""
    return [(f"{tag}1-vk", capture_backend.VULKAN), (f"{tag}2-vk", capture_backend.VULKAN)]


def check_options(root: str, before: str, after: str, vary: "list[str]") -> None:
    """Refuse to compare sets that ran with different render options (options.cfg is an input of the picture); raises OptionsMismatch."""
    def sweep_options(set_name: str):
        try:
            with open(os.path.join(root, set_name, "capture_run.json"), encoding="utf-8") as f:
                return json.load(f).get("render_options")
        except (OSError, ValueError):
            return None
    sets = [(name, f"{label} {name}") for label, tag in (("BEFORE", before), ("AFTER", after)) for name, _ in arms(tag)]
    ref_name, ref_label = sets[0]
    reference = {"values": sweep_options(ref_name)}
    for name, label in sets[1:]:
        render_options_pin.require_same(reference, {"values": sweep_options(name)}, vary, ref_label, label)


def check_backends(root: str, before: str, after: str, bookmarks: "list[str]") -> "list[str]":
    """One message per "-vk" set that was not drawn by Vulkan. The pose sidecars decide (the engine writes `backend`, an older one only
    `readback_source`); a set with no sidecar at all is also a problem. A set that drew GL says so in the message."""
    problems = []
    for tag in (before, after):
        for name, want in arms(tag):
            sidecars = []
            for bm in bookmarks:
                try:
                    with open(os.path.join(root, name, bm, bm + ".json"), encoding="utf-8") as f:
                        sidecars.append((f"{name}/{bm}", json.load(f)))
                except (OSError, ValueError):
                    sidecars.append((f"{name}/{bm}", None))
            found = capture_backend.check(want, sidecars)
            problems += ["VK arm UNPROVEN (GL pixels or no proof): " + m for m in found]
    return problems


def compare(root: str, before: str, after: str, bookmarks: list[str]) -> list[str]:
    """One line per comparison per bookmark: the noise floor first, then the before/after pairs."""
    out = []
    for bm in bookmarks:
        pairs = [
            ("NOISE VK before1->before2", f"{before}1-vk", f"{before}2-vk"),
            ("VK before1->after1", f"{before}1-vk", f"{after}1-vk"),
            ("VK before1->after2", f"{before}1-vk", f"{after}2-vk"),
        ]
        for label, a, b in pairs:
            out.append(f"{bm} {label}: {diff_line(frame(root, a, bm), frame(root, b, bm))}")
    return out


def capture_cmd(install: str, out_dir: str, bookmarks: str, freeze_anim: bool = True) -> list[str]:
    cmd = [sys.executable, os.path.join(HERE, "capture.py"), "--install", install, "--bookmarks", bookmarks,
           "--out", out_dir, "--clean"]
    if freeze_anim:
        cmd += ["--env", "MC2_SMOKE_FREEZE_ANIM=1"]      # no animation-phase noise in the A/B (needs an exe built with the gate, on BOTH sets)
    cmd += ["--env", "MC2_RENDER_BACKEND=vulkan", "--env", "MC2_VK_SHARED_GBUFFER=1"]   # golden-compatible declared env (the umbrella var is inert)
    return cmd


def run_capture(install: str, out_dir: str, bookmarks: str, freeze_anim: bool = True) -> int:
    cmd = capture_cmd(install, out_dir, bookmarks, freeze_anim)
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
    d.add_argument("--vary", default="", help="comma-separated options.cfg keys that differ between the sets on purpose (any other difference is refused)")
    args = ap.parse_args(argv)
    if args.cmd == "compare":
        try:
            check_options(args.root, args.before, args.after, [k for k in args.vary.split(",") if k])
        except render_options_pin.OptionsMismatch as e:
            print(f"REFUSED: {e}", file=sys.stderr)
            return 1
        bookmarks = args.bookmarks.split(",")
        bad = check_backends(args.root, args.before, args.after, bookmarks)
        if bad:
            print("REFUSED: a set was not drawn by the backend its name says:\n  " + "\n  ".join(bad), file=sys.stderr)
            return 1
        print("\n".join(compare(args.root, args.before, args.after, bookmarks)))
        return 0
    os.makedirs(args.root, exist_ok=True)
    rc = 0
    for name, _ in arms(args.tag):
        rc = run_capture(args.install, os.path.join(args.root, name), args.bookmarks, not args.keep_anim) or rc
    return rc


if __name__ == "__main__":
    sys.exit(main())
