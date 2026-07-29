#!/usr/bin/env python3
"""menu_oracle.py — visual menu-visibility oracle for mc2.exe (Windows only).

Launches the exe, waits for the main menu, grabs the CLIENT RECT of the game
window, and prints a JSON verdict on whether anything visible is being drawn:

    py -3 tools/menu_oracle.py --exe A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe \
        [--workdir <dir>] [--wait 18] [--env K=V ...] [--threshold 10] \
        [--esc-taps 0] [--shot out.png]

Output (stdout, last line): {"mean": .., "nonblack_frac": .., "verdict": ..}
Exit codes: 0 = VISIBLE, 1 = BLACK, 2 = INCONCLUSIVE (no window / foreground
verify failed / process died before grab).

Hard-won correctness details (do NOT remove):
  * SetProcessDpiAwareness(2) BEFORE any window queries — without it, coords
    are DPI-virtualized and ImageGrab captures the WRONG screen region.
  * Window restore = ShowWindow(SW_RESTORE) + SetForegroundWindow, preceded by
    an ALT key tap (keybd_event 0x12) — Windows refuses SetForegroundWindow
    from a background process unless an input event unlocks it.
    BACKGROUND-SMOKE-1: this is a foreground STEAL, so it now runs only when
    MC2_HARNESS_ALLOW_FOREGROUND=1. By default the exe is launched via
    smoke_lib.nofocus with SW_SHOWNOACTIVATE — visible, never activated — and
    ImageGrab reads its on-screen pixels without it ever taking focus. ESC taps
    are likewise skipped by default (they would land in the user's own window).
  * Foreground is VERIFIED via WindowFromPoint at the client-rect center; the
    grab is retried (window nudged TOPMOST to 60,60) until the point resolves
    to our hwnd. A grab without this check can silently capture an occluding
    window. That verification is only possible when foreground is allowed; in
    background mode the result carries "foreground_verified": false and an
    occluding window could contaminate the grab.

KNOWN LIMIT: a sparse text-on-black menu can legitimately read mean < 1 while
perfectly visible. The default threshold (mean > 10 AND nonblack_frac > 0.05)
is tuned for the stock splash/menu; interpreting near-threshold values — and
lowering --threshold for sparse menus — is the CALLER's job. Treat BLACK as
"probably black, eyeball the --shot png before acting on it".

Kills only the process it launched. Requires: pillow, numpy (pip).
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "scripts"))
# BACKGROUND-SMOKE-1: single chokepoint for non-activating launch + guarded focus.
from smoke_lib import nofocus  # noqa: E402


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True, help="path to mc2.exe (or any GUI exe)")
    ap.add_argument("--workdir", default=None, help="cwd for the game (default: exe dir)")
    ap.add_argument("--wait", type=int, default=18, help="seconds to wait for menu (default 18)")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="extra env var for the child (repeatable)")
    ap.add_argument("--threshold", type=float, default=10.0,
                    help="mean-luma threshold for VISIBLE (default 10; see sparse-menu caveat)")
    ap.add_argument("--frac-threshold", type=float, default=0.05,
                    help="min fraction of pixels with luma>30 (default 0.05)")
    ap.add_argument("--esc-taps", type=int, default=0,
                    help="number of ESC taps after restore (skip splash screens)")
    ap.add_argument("--shot", default=None, help="save the grabbed client-rect PNG here")
    return ap.parse_args()


def main():
    args = parse_args()
    try:
        from PIL import ImageGrab  # noqa: F401
        import numpy  # noqa: F401
    except ImportError as e:
        print(json.dumps({"verdict": "INCONCLUSIVE", "error": f"missing dep: {e}"}))
        return 2

    exe = os.path.abspath(args.exe)
    workdir = args.workdir or os.path.dirname(exe)
    env = dict(os.environ)
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    u32 = ctypes.windll.user32
    # DPI awareness FIRST — else client-rect coords are virtualized on scaled
    # displays and we grab the wrong region.
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        u32.SetProcessDPIAware()

    # BACKGROUND-SMOKE-1: launch non-activating (and with MC2_NO_CURSOR_GRAB=1).
    # show_window=True keeps the window VISIBLE (SW_SHOWNOACTIVATE) rather than
    # minimized, because this tool grabs the client rect -- a minimized window
    # has no on-screen pixels to grab. It still never takes focus.
    p = nofocus.spawn([exe], cwd=workdir, env=env, show_window=True,
                      stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    print(f"[menu_oracle] launched pid={p.pid}, waiting {args.wait}s", flush=True)
    result = {"mean": -1.0, "nonblack_frac": -1.0, "verdict": "INCONCLUSIVE"}
    try:
        time.sleep(args.wait)

        # Find visible top-level windows belonging to our pid.
        hwnds = []

        @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
        def cb(hwnd, lp):
            pid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == p.pid and u32.IsWindowVisible(hwnd):
                hwnds.append(hwnd)
            return True

        u32.EnumWindows(cb, 0)
        print(f"[menu_oracle] hwnds={hwnds} alive={p.poll() is None}", flush=True)
        if not hwnds or p.poll() is not None:
            result["error"] = "no window or process exited"
            print(json.dumps(result), flush=True)
            return 2
        h = hwnds[0]

        # BACKGROUND-SMOKE-1: restore+foreground is a deliberate foreground
        # steal (with an ALT-tap below to defeat the foreground lock). It is
        # user-present-only now; by default the window is already visible but
        # unactivated, and ImageGrab reads its on-screen pixels regardless.
        if nofocus.allow_foreground():
            u32.ShowWindow(h, 9)          # SW_RESTORE
            u32.SetForegroundWindow(h)
        time.sleep(2)
        # Synthetic keystrokes land in whatever window has focus -- forbidden
        # unless the session is user-present (BACKGROUND-SMOKE-1).
        if args.esc_taps and not nofocus.allow_foreground():
            print("[menu_oracle] --esc-taps ignored: needs "
                  "MC2_HARNESS_ALLOW_FOREGROUND=1", flush=True)
        elif nofocus.allow_foreground():
            for _ in range(args.esc_taps):
                u32.keybd_event(0x1B, 0, 0, 0)
                u32.keybd_event(0x1B, 0, 2, 0)
                time.sleep(2)

        # Bring to front and VERIFY with WindowFromPoint before grabbing.
        # BACKGROUND-SMOKE-1: the whole raise-and-verify dance (ALT tap to
        # defeat the foreground lock, TOPMOST reposition, SetForegroundWindow)
        # is a foreground steal, so it runs only when explicitly allowed. In
        # background mode we grab the unactivated window's rect directly and
        # accept that an occluding window could contaminate the grab -- an
        # INCONCLUSIVE-vs-wrong-pixels tradeoff the caller is warned about.
        verified = False
        if not nofocus.allow_foreground():
            result["foreground_verified"] = False
            verified = True
        for attempt in range(0 if verified else 6):
            u32.keybd_event(0x12, 0, 0, 0)  # ALT tap unlocks SetForegroundWindow
            u32.keybd_event(0x12, 0, 2, 0)
            u32.SetWindowPos(h, -1, 60, 60, 0, 0, 0x0001)  # HWND_TOPMOST, NOSIZE
            u32.SetForegroundWindow(h)
            time.sleep(0.8)
            c = wintypes.RECT()
            u32.GetClientRect(h, ctypes.byref(c))
            pt = wintypes.POINT(c.right // 2, c.bottom // 2)
            u32.ClientToScreen(h, ctypes.byref(pt))
            root = u32.GetAncestor(u32.WindowFromPoint(pt), 2)  # GA_ROOT
            if root == h:
                verified = True
                break
            print(f"[menu_oracle] fg-verify attempt={attempt} got={root} want={h}", flush=True)
        if not verified:
            result["error"] = "foreground verify failed (window occluded?)"
            print(json.dumps(result), flush=True)
            return 2

        from PIL import ImageGrab
        import numpy as np
        c = wintypes.RECT()
        u32.GetClientRect(h, ctypes.byref(c))
        pt = wintypes.POINT(0, 0)
        u32.ClientToScreen(h, ctypes.byref(pt))
        img = ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + c.right, pt.y + c.bottom))
        if args.shot:
            img.save(args.shot)
        a = np.asarray(img.convert("L"), dtype=np.float32)
        result["mean"] = float(a.mean())
        result["nonblack_frac"] = float((a > 30).mean())
        visible = (result["mean"] > args.threshold
                   and result["nonblack_frac"] > args.frac_threshold)
        result["verdict"] = "VISIBLE" if visible else "BLACK"
    finally:
        try:
            p.kill()
            p.wait(10)
        except Exception:
            pass

    print(json.dumps(result), flush=True)
    return 0 if result["verdict"] == "VISIBLE" else 1


if __name__ == "__main__":
    sys.exit(main())
