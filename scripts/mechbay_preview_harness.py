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

Screen names match mc2BootScreenXY in code/missionbegin.cpp: purchase|bay|loadout|launch|briefing. The encyclopedia names boot the Mechlopedia from
the main menu instead (code/mainmenu.cpp, no mission is played): encyclopedia (mechs), encyclopedia_vehicles, encyclopedia_pilots.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# MC2_SMOKE_MODE + MC2_NO_CURSOR_GRAB (set below) stop the game grabbing the pointer; nofocus.spawn refuses an exe too old to honour them.
from smoke_lib import host_lease, nofocus  # noqa: E402

DEFAULT_DEPLOY = "A:/Games/mc2-opengl/mc2-win64-v0.4"


def acquire_label(args) -> str:
    return f"mechbay_preview_harness {args.screen} capture"


def main():
    os.environ.update(MC2_HOST_GAME_SLOTS="2", MC2_HOST_CORES_PER_SLOT="2")
    ap = argparse.ArgumentParser()
    ap.add_argument("screen", choices=["mainmenu", "campaign_select", "purchase", "bay", "loadout", "launch", "briefing", "encyclopedia", "encyclopedia_vehicles", "encyclopedia_pilots"])
    ap.add_argument("--campaign", default="campaign",
                    help="stock campaign .fit basename passed to MC2_BOOT_TO_BAY (default: 'campaign')")
    ap.add_argument("--deploy", default=DEFAULT_DEPLOY)
    ap.add_argument("--preview-log", action="store_true",
                    help="set MC2_LOG_PREVIEW=1 (diagnostics go to the run log)")
    ap.add_argument("--frame", type=int, default=200,
                    help="boot frame number, or -5 for each screen's first five frames")
    ap.add_argument("--timeout", type=int, default=25,
                    help="hard kill after this many seconds regardless of screenshot state")
    ap.add_argument("--win-after-sec", type=int, default=2,
                    help="MC2_SOAK_WIN_AFTER_SEC -- auto-win the first mission this fast so the bay is reached quickly")
    ap.add_argument("--out", default="",
                    help="output TGA path (default <deploy>/harness_<screen>.tga)")
    ap.add_argument("--tracy", type=Path, help="record a 20-second Tracy trace of the front-end path")
    ap.add_argument("--walk", action="store_true", help="exercise purchase, loadout and launch through the existing soak screen walk")
    ap.add_argument("--mod-deps", default="",
                    help="mount these add-on mods over the stock campaign (MC2_MOD_DEPS), e.g. bt2018_mechs")
    ap.add_argument("--buy-and-modify", action="store_true",
                    help="buy + deploy one mech at the bay (a mounted package chassis first) and open the mech lab on it "
                         "(MC2_SOAK_AUTO_PURCHASE + MC2_BOOT_TO_MECHLAB); use with screen 'bay'")
    ap.add_argument("--chassis", default="",
                    help="with --buy-and-modify: buy the variant whose name or package chassis stem contains this text "
                         "(MECH_PREVIEW_CHASSIS), e.g. marauder")
    ap.add_argument("--size", default="",
                    help="WxH drawable size (MC2_WIDTH/MC2_HEIGHT); the nofocus launcher otherwise defaults to 854x480")
    ap.add_argument("--script", default="",
                    help="MC2_INPUT_SCRIPT file: scripted real mouse/keys (click X Y in 800x600, shot PATH.tga) for the lab")
    args = ap.parse_args()

    deploy = Path(args.deploy).resolve()
    exe = deploy / "mc2.exe"
    if not exe.is_file():
        sys.exit(f"exe not found: {exe}")

    out_tga = Path(args.out).resolve() if args.out else (deploy / f"harness_{args.screen}.tga")
    out_tga.parent.mkdir(parents=True, exist_ok=True)
    if out_tga.exists():
        out_tga.unlink()
    for previous in out_tga.parent.glob(out_tga.name + ".screen*.frame*.tga"):
        previous.unlink()
    out_stdout = deploy / f"harness_{args.screen}_stdout.txt"
    out_stderr = deploy / f"harness_{args.screen}_stderr.txt"

    # Stock: explicitly unset any mod env so this never picks up an ambient
    # MC2_ACTIVE_MOD from the caller's shell.
    env = os.environ.copy()
    # The auto-win writes after_mission_*.fit: never into the player's savegame folder (MC2_SAVE_DIR, engine redirect).
    save_dir = Path(env.get("MC2_SAVE_DIR") or (out_tga.parent / "saves")).resolve()
    if ".mechcommander2" in save_dir.as_posix().lower():
        sys.exit(f"MC2_SAVE_DIR must be a private dir, not the player's save folder: {save_dir}")
    save_dir.mkdir(parents=True, exist_ok=True)
    env["MC2_SAVE_DIR"] = str(save_dir)
    for k in ("MC2_ACTIVE_MOD", "MC2_MOD_DEPS", "MC2_SOAK_AUTOWIN", "MC2_SOAK_AUTO_PURCHASE",
              "MC2_SOAK_LANCE_RANDOM", "MC2_SOAK_CHECK_SCREENS", "MC2_SOAK_KILL_ENEMY",
              "MC2_SOAK_PILOT_PROMOTE"):
        env.pop(k, None)

    encyclopedia = args.screen.startswith("encyclopedia")
    front_end = encyclopedia or args.screen == "mainmenu"
    env.update({
        "MC2_BOOT_TO_BAY": args.campaign,
        "MC2_BOOT_TO_SCREEN": args.screen,
        "MC2_MENU_CANARY_SKIP_INTRO": "1",   # skip front-end videos (headless-safe)
        "MC2_SCREENSHOT_AT_FRAME": str(args.frame),
        "MC2_SCREENSHOT_PATH": str(out_tga),
        # The screenshot is the Vulkan present readback (menus and HUD included); a GL run writes no TGA.
        "MC2_RENDER_BACKEND": "vulkan",
        "MC2_VK_SHARED_GBUFFER": "1",
        "MC2_LOG": "1",
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
        # Engine-side cursor release is required here.
        # NOT MC2_SMOKE_MODE -- this harness deliberately drives the real
        # logistics/front-end path and smoke mode skips movies and screens.
        "MC2_NO_CURSOR_GRAB": "1",
    })
    if args.mod_deps:
        env["MC2_MOD_DEPS"] = args.mod_deps
    if args.buy_and_modify:
        env["MC2_SOAK_AUTO_PURCHASE"] = "1"
        env["MC2_BOOT_TO_MECHLAB"] = "1"
    if args.chassis:
        env["MECH_PREVIEW_CHASSIS"] = args.chassis
    if args.size:
        from smoke_lib import render_resolution
        env.update(render_resolution.size_env(*render_resolution.parse(args.size)))
    if args.script:
        env["MC2_INPUT_SCRIPT"] = str(Path(args.script).resolve())
    if args.walk:
        env.update(MC2_SOAK_AUTO_PURCHASE="1", MC2_SOAK_CHECK_SCREENS="1")
        env.pop("MC2_SOAK_STOP_AT_BAY", None)

    if args.preview_log:
        env["MC2_LOG_PREVIEW"] = "1"
    else:
        env.pop("MC2_LOG_PREVIEW", None)

    if front_end:
        # The Mechlopedia boots from the main menu: no campaign mission, no auto-win.
        for k in ("MC2_BOOT_TO_BAY", "MC2_SOAK_AUTOWIN", "MC2_SOAK_WIN_AFTER_SEC", "MC2_SOAK_STOP_AT_BAY"):
            env.pop(k, None)

    print(f"[harness] launching {exe} (screen={args.screen} campaign='{args.campaign}' frame={args.frame})")
    # Readbacks belong to this process's swapchain; no desktop capture or input.
    # Use a normal game slot. Timings under concurrent lane load are provisional.
    try:
        host_lease.acquire(acquire_label(args))
    except host_lease.HostLeaseBusy as busy:
        print(f"[harness] FAILED: {busy}")
        sys.exit(2)
    with open(out_stdout, "w") as fo, open(out_stderr, "w") as fe:
        recorder = None
        p = None
        started = time.monotonic()
        if args.tracy:
            env.setdefault("TRACY_PORT", "8087")
        try:
            p = nofocus.spawn([str(exe)], show_window=True, cwd=str(deploy), stdout=fo, stderr=fe, env=env)
            if args.tracy:
                # The campaign first loads a mission. Profile the return to
                # logistics rather than spending the trace on that mission load.
                if not front_end:
                    while p.poll() is None and time.monotonic() - started < args.timeout - 21:
                        fo.flush()
                        if "[SOAK] autowin mission=" in out_stdout.read_text(errors="replace"):
                            break
                        time.sleep(0.1)
                sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
                from tracy_capture_compare import tracy_exe
                args.tracy.parent.mkdir(parents=True, exist_ok=True)
                port = env["TRACY_PORT"]
                recorder = nofocus.spawn([str(tracy_exe("tracy-capture.exe")), "-a", "127.0.0.1", "-p", port,
                                             "-s", "20", "-f", "-o", str(args.tracy)], stdout=fo, stderr=fe,
                                            **nofocus.hidden_kwargs(creationflags=0x00004000))
            targets = ([21, 20, 22, 31] if args.walk else
                       [-2] if encyclopedia else [-1] if args.screen == "mainmenu" else
                       [{"campaign_select":1,"purchase":20,"bay":21,"loadout":22,"launch":31,"briefing":11}[args.screen]])
            while p.poll() is None and time.monotonic() - started < args.timeout:
                if recorder is not None and recorder.poll() is not None:
                    break
                if args.frame == -5 and all(Path(str(out_tga) + f".screen{s}.frame{f}.tga").is_file()
                                            for s in targets for f in range(1,6)):
                    break
                time.sleep(0.1)
            if p.poll() is None:
                p.kill()
            p.wait()
            print(f"[harness] run ended (code {p.returncode})")
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            print(f"[harness] killed after {args.timeout}s (expected — screenshot hook doesn't self-exit)")
        finally:
            if p is not None and p.poll() is None:
                p.kill()
                p.wait()
            if recorder is not None:
                try:
                    recorder.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    recorder.kill()
                    recorder.wait()

    if (out_tga.exists() or (args.tracy and args.tracy.is_file()) or
            (args.frame == -5 and all(Path(str(out_tga) + f".screen{s}.frame{f}.tga").is_file()
                                     for s in targets for f in range(1,6)))):
        print(f"[harness] capture complete: {args.tracy or out_tga}")
        sys.exit(0)
    else:
        print(f"[harness] FAILED: no screenshot at {out_tga}")
        print(f"[harness] see {out_stderr} / {out_stdout} for engine log")
        sys.exit(1)


if __name__ == "__main__":
    main()
