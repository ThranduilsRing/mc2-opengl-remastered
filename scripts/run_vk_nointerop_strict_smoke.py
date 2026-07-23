#!/usr/bin/env python3
"""Run run_smoke.py with the Vulkan no-interop strict gate stack."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from run_vk_nointerop_strict_visible import STRICT_ENV


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main() -> int:
    root = repo_root()
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe")
    ap.add_argument("--tier", default="tier1")
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--show-window", action="store_true")
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--resource-strict", action="store_true")
    ap.add_argument("--uncapped", action="store_true")
    ap.add_argument("--env", action="append", default=[])
    args = ap.parse_args()

    env = os.environ.copy()
    env.update(STRICT_ENV)
    if args.resource_strict:
        env["MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT"] = "1"
    if args.uncapped:
        env["MC2_FPS_CAP"] = "0"
        env["MC2_VSYNC"] = "0"
    for item in args.env:
        if "=" not in item:
            ap.error("--env entries must be KEY=VALUE")
        key, value = item.split("=", 1)
        if not key:
            ap.error("--env key must be non-empty")
        env[key] = value

    cmd = [
        sys.executable,
        str(root / "scripts" / "run_smoke.py"),
        "--duration",
        str(args.duration),
        "--exe",
        args.exe,
        "--no-lease",
    ]
    if args.keep_logs:
        cmd.append("--keep-logs")
    if args.show_window:
        cmd.append("--show-window")
    if args.mission:
        for mission in args.mission:
            cmd.extend(["--mission", mission])
    else:
        cmd.extend(["--tier", args.tier])

    return subprocess.call(cmd, cwd=str(root), env=env)


if __name__ == "__main__":
    raise SystemExit(main())
