#!/usr/bin/env python3
"""Launch the game under RenderDoc and save a frame capture at a fixed pose.

Drives the engine-side in-process hook shipped in
``GameOS/gameos/gos_rdoc_capture.cpp`` (env ``MC2_RDC_CAPTURE_FRAME`` /
``MC2_RDC_CAPTURE_PATH`` / ``MC2_RDC_EXIT_AFTER``). The game is launched under
``renderdoccmd capture`` so ``renderdoc.dll`` is injected; the hook waits for
the frame-solid latch, counts frames, calls ``TriggerCapture()`` at the target
frame, writes the ``.rdc``, and exits.

What this adds on top of ``scripts/renderdoc_capture.py`` (which is the parity
/ convert-and-diff orchestrator):
  * **pose control** via the game's bookmark/park fixtures -- ``--bookmark``
    sets ``MC2_VISUAL_BOOKMARK_CAPTURE`` and ``--park`` sets
    ``MC2_CAMERA_PARK`` so the captured frame is a *known* pose, not whatever
    the menu-driven smoke run happened to be showing;
  * a **sequential game-run lane guard** so parallel agents don't launch the
    game on top of each other;
  * it targets an arbitrary **install dir** (``--install``), not just the
    canonical v0.4 deploy.

The output ``.rdc`` is then fed to ``attribute.py`` / ``states.py``.

Example (the acceptance run)::

    py -3 tools/renderdoc/capture.py \\
        --install A:/Games/mc2-opengl/mc2-win64-v0.4 \\
        --mission mc2_10 \\
        --bookmark tests/visual/bookmarks/mc2_10.json \\
        --frame 120 \\
        --out tests/smoke/captures/mc2_10_frame120.rdc
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _rdcommon as rc  # noqa: E402

WORKTREE = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(WORKTREE / "scripts"))

from smoke_lib import nofocus  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", required=True,
                    help="game install dir containing mc2.exe")
    ap.add_argument("--mission", default="mc2_10",
                    help="mission stem (default mc2_10)")
    ap.add_argument("--frame", type=int, default=120,
                    help="capture frame after the frame-solid latch (default 120)")
    ap.add_argument("--bookmark",
                    help="bookmark JSON for pose control "
                    "(sets MC2_VISUAL_BOOKMARK_CAPTURE)")
    ap.add_argument("--park",
                    help="camera-park fixture value (sets MC2_CAMERA_PARK)")
    ap.add_argument("--out", required=True,
                    help="output .rdc path")
    ap.add_argument("--duration", type=int, default=120,
                    help="max game run seconds (default 120)")
    ap.add_argument("--exe-name", default="mc2.exe")
    ap.add_argument("--renderdoccmd", help="explicit renderdoccmd.exe path")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the launch plan and exit without running")
    ap.add_argument("--no-lane", action="store_true",
                    help="skip the sequential game-run lane guard")
    args = ap.parse_args(argv)

    install = Path(args.install).resolve()
    exe = install / args.exe_name
    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    tpl = out.with_suffix("")   # RenderDoc appends _frameN.rdc

    rdoc = rc.find_tool("renderdoccmd", args.renderdoccmd)

    env = os.environ.copy()
    env["MC2_RDC_CAPTURE_FRAME"] = str(args.frame)
    env["MC2_RDC_CAPTURE_PATH"] = str(tpl)
    env["MC2_RDC_EXIT_AFTER"] = "1"
    env["MC2_SMOKE_MODE"] = "1"
    env.setdefault("MC2_SMOKE_SEED", "0xC0FFEE")
    if args.bookmark:
        bm = Path(args.bookmark).resolve()
        if not bm.is_file():
            raise SystemExit("bookmark not found: %s" % bm)
        env["MC2_VISUAL_BOOKMARK_CAPTURE"] = str(bm)
        # Align the visual-capture trigger frame with the RDC trigger frame so
        # the bookmark pose is applied by the time we capture.
        env.setdefault("MC2_VISUAL_CAPTURE_FRAME", str(args.frame))
    if args.park:
        env["MC2_CAMERA_PARK"] = args.park

    cmd = [str(rdoc) if rdoc else "renderdoccmd", "capture", "-w",
           str(exe), "--mission", args.mission,
           "--duration", str(args.duration), "--smoke-active"]

    plan = {
        "renderdoccmd": str(rdoc) if rdoc else "<NOT FOUND>",
        "exe": str(exe),
        "mission": args.mission,
        "frame": args.frame,
        "bookmark": args.bookmark,
        "park": args.park,
        "template": str(tpl),
        "cmd": " ".join(cmd),
        "cwd": str(install),
    }
    print("[capture] launch plan:")
    for k, v in plan.items():
        print("    %-13s %s" % (k, v))

    if args.dry_run:
        print("[capture] --dry-run: not launching.")
        return 0

    # --- real launch below: needs RenderDoc + a valid install ---------------
    if not rdoc:
        raise SystemExit(
            "renderdoccmd.exe not found. Install RenderDoc or pass "
            "--renderdoccmd. (Use --dry-run to validate the plan without it.)")
    if not exe.is_file():
        raise SystemExit("mc2.exe missing at %s; deploy first." % exe)

    release = (lambda: None) if args.no_lane else rc.acquire_game_lane(
        "capture:%s:%s" % (args.mission, args.frame))
    try:
        print("[capture] launching (own PID sequential lane held)...")
        t0 = time.time()
        # NOTE: our STARTUPINFO stops at renderdoccmd -- IT spawns mc2.exe, so
        # nCmdShow never reaches the engine window. The ENV is what covers the
        # engine: renderdoccmd passes its environment through, so nofocus's
        # MC2_NO_CURSOR_GRAB=1 suppresses SDL activation and cursor grabbing.
        proc = nofocus.run(cmd, env=env, cwd=str(install))
        dt = time.time() - t0
        print("[capture] renderdoccmd exit=%s in %.1fs"
              % (proc.returncode, dt))
    finally:
        release()

    # renderdoccmd capture returns the child's code; engine exits 0 (or 4 on
    # the smoke early-exit path) after MC2_RDC_EXIT_AFTER fires.
    produced = sorted(glob.glob(str(tpl) + "*.rdc"), key=os.path.getmtime)
    if not produced:
        raise SystemExit(
            "no .rdc produced (looked for %s*.rdc). The frame-solid latch may "
            "not have armed, or the frame count was never reached. Check the "
            "run log; try a larger --duration or verify MC2_RDC_* env." % tpl)
    newest = produced[-1]
    if Path(newest).resolve() != out and not out.exists():
        try:
            os.replace(newest, out)
            newest = str(out)
        except OSError:
            pass
    print("[capture] .rdc: %s" % newest)
    print("[capture] next: py -3 tools/renderdoc/attribute.py --rdc %s "
          "--region 0,0,W,60" % newest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
