#!/usr/bin/env python3
"""mechbay_preview_harness.py — MECH-PREVIEW-FBO-VALIDATE-1

Narrow, offline (no manual clicking) validation harness for the offscreen-FBO
mech-preview-panel fix (PREVIEW-FBO-FIXED-800x600-1). Boots STOCK (no
MC2_ACTIVE_MOD/MC2_MOD_DEPS) straight to a single logistics screen via
MC2_BOOT_TO_BAY + MC2_BOOT_TO_SCREEN, takes ONE screenshot at a fixed frame,
then kills the process. Deliberately does NOT set MC2_SOAK_AUTOWIN — nothing
auto-advances past the requested screen; this is a copy of
scripts/sweep_campaign.py's env-var plumbing, narrowed to a single static
screen capture instead of a full campaign soak.

Usage:
  py -3 scripts/mechbay_preview_harness.py bay
  py -3 scripts/mechbay_preview_harness.py purchase
  py -3 scripts/mechbay_preview_harness.py loadout
  py -3 scripts/mechbay_preview_harness.py bay --campaign "campaign" --frame 200 --deploy <dir>

Screen names match mc2BootScreenXY in code/missionbegin.cpp: purchase|bay|loadout|launch.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_DEPLOY = "A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("screen", choices=["purchase", "bay", "loadout", "launch"])
    ap.add_argument("--campaign", default="campaign",
                    help="stock campaign .fit basename passed to MC2_BOOT_TO_BAY (default: 'campaign')")
    ap.add_argument("--deploy", default=DEFAULT_DEPLOY)
    ap.add_argument("--frame", type=int, default=200,
                    help="capture frame number (frames since boot settle)")
    ap.add_argument("--timeout", type=int, default=25,
                    help="hard kill after this many seconds regardless of screenshot state")
    ap.add_argument("--win-after-sec", type=int, default=2,
                    help="MC2_SOAK_WIN_AFTER_SEC -- auto-win the first mission this fast so the bay is reached quickly")
    ap.add_argument("--out", default="",
                    help="output TGA path (default <deploy>/harness_<screen>.tga)")
    args = ap.parse_args()

    deploy = Path(args.deploy).resolve()
    exe = deploy / "mc2.exe"
    if not exe.is_file():
        sys.exit(f"exe not found: {exe}")

    out_tga = Path(args.out).resolve() if args.out else (deploy / f"harness_{args.screen}.tga")
    if out_tga.exists():
        out_tga.unlink()
    out_stdout = deploy / f"harness_{args.screen}_stdout.txt"
    out_stderr = deploy / f"harness_{args.screen}_stderr.txt"

    # Stock: explicitly unset any mod env so this never picks up an ambient
    # MC2_ACTIVE_MOD from the caller's shell.
    env = os.environ.copy()
    for k in ("MC2_ACTIVE_MOD", "MC2_MOD_DEPS", "MC2_SOAK_AUTOWIN", "MC2_SOAK_AUTO_PURCHASE",
              "MC2_SOAK_LANCE_RANDOM", "MC2_SOAK_CHECK_SCREENS", "MC2_SOAK_KILL_ENEMY",
              "MC2_SOAK_PILOT_PROMOTE"):
        env.pop(k, None)

    env.update({
        "MC2_BOOT_TO_BAY": args.campaign,
        "MC2_BOOT_TO_SCREEN": args.screen,
        "MC2_MENU_CANARY_SKIP_INTRO": "1",   # skip front-end videos (headless-safe)
        "MC2_SCREENSHOT_AT_FRAME": str(args.frame),
        "MC2_SCREENSHOT_PATH": str(out_tga),
        "MC2_SCREENSHOT_SOURCE": "backbuffer",   # logistics/menu UI never touches sceneFBO_
        "MC2_LOG": "1",
        "MC2_LOG_PREVIEW": "1",
        # Stock MC2_BOOT_TO_BAY does NOT skip straight to logistics — it launches
        # the campaign's first mission for real. The mech bay is only reached
        # after that mission plays out through the after-action screens. So we
        # DO need auto-win here (just the one mission, not a full campaign
        # soak) to get there at all; MC2_SOAK_WIN_AFTER_SEC keeps that mission
        # short. No MC2_SOAK_AUTO_PURCHASE / MC2_SOAK_LANCE_RANDOM / etc -- once
        # back at logistics this harness does NOT keep clicking further
        # (--frame/--timeout just need to land after the bay settles but
        # before any subsequent auto-advance carries it into mission 2).
        "MC2_SOAK_AUTOWIN": "1",
        "MC2_SOAK_WIN_AFTER_SEC": str(args.win_after_sec),
        "MC2_SOAK_STOP_AT_BAY": "1",   # park at the bay, never NEXT past it
    })

    print(f"[harness] launching {exe} (screen={args.screen} campaign='{args.campaign}' frame={args.frame})")
    with open(out_stdout, "w") as fo, open(out_stderr, "w") as fe:
        p = subprocess.Popen([str(exe)], cwd=str(deploy), stdout=fo, stderr=fe, env=env)
        try:
            p.wait(timeout=args.timeout)
            print(f"[harness] process exited on its own (code {p.returncode})")
        except subprocess.TimeoutExpired:
            p.kill()
            print(f"[harness] killed after {args.timeout}s (expected — screenshot hook doesn't self-exit)")

    if out_tga.exists():
        print(f"[harness] screenshot captured: {out_tga} ({out_tga.stat().st_size} bytes)")
        sys.exit(0)
    else:
        print(f"[harness] FAILED: no screenshot at {out_tga}")
        print(f"[harness] see {out_stderr} / {out_stdout} for engine log")
        sys.exit(1)


if __name__ == "__main__":
    main()
