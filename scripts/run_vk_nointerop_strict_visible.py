#!/usr/bin/env python3
"""Run the visible Vulkan no-GL-interop strict canary with the current gate stack."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


STRICT_ENV = {
    "MC2_RENDER_BACKEND": "vulkan",
    "MC2_DEBUG_STATE_DUMP": "1",
    "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE",
    "MC2_VK_SHARED_GBUFFER": "1",
    "MC2_VK_SHARED_SCENE_STRICT": "1",
    "MC2_VK_POST_COMPOSITE": "1",
    "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN": "1",
    "MC2_VK_PRESENT_DIRECT_FBO": "1",
    "MC2_VK_PRESENT_TAIL_HOOK": "1",
    "MC2_VK_PRESENT_SKIP_GL_HANDOFF": "1",
    "MC2_VK_PRESENT_NATIVE_STRICT": "1",
    "MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR": "1",
    "MC2_VK_PROP_DIRECT_CHAIN": "1",
    "MC2_VK_MECH_DIRECT_CHAIN": "1",
    "MC2_VK_WATER_TIER1": "1",
    "MC2_VK_WATER_DIRECT_CHAIN": "1",
    "MC2_VK_NO_GL_INTEROP_STRICT": "1",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main() -> int:
    root = repo_root()
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe")
    ap.add_argument("--mission", default="mc2_01")
    ap.add_argument("--out-dir")
    ap.add_argument("--duration", type=int, default=210)
    ap.add_argument("--settle", type=int, default=80)
    ap.add_argument("--trigger", type=int, default=120)
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--allow-partial", action="store_true")
    args = ap.parse_args()

    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%SZ")
    out_dir = Path(args.out_dir) if args.out_dir else root / ".claude" / f"vk-nointerop-strict-visible-{stamp}"

    env = os.environ.copy()
    env.update(STRICT_ENV)

    cmd = [
        sys.executable,
        str(root / "scripts" / "run_visual_capture.py"),
        "--exe",
        args.exe,
        "--mission",
        args.mission,
        "--out-dir",
        str(out_dir),
        "--duration",
        str(args.duration),
        "--settle",
        str(args.settle),
        "--trigger",
        str(args.trigger),
    ]
    if not args.headless:
        cmd.append("--show-window")
    if args.allow_partial:
        cmd.append("--allow-partial")

    print("[vk-strict-visible] out_dir=%s" % out_dir)
    print("[vk-strict-visible] gates=%s" % ",".join(sorted(STRICT_ENV)))
    return subprocess.call(cmd, cwd=str(root), env=env)


if __name__ == "__main__":
    raise SystemExit(main())
