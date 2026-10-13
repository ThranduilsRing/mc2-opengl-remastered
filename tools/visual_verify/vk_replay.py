#!/usr/bin/env python3
"""What: the GFXReconstruct capture/replay proof for the Vulkan backend. One Vulkan mission frame is captured with the GFXReconstruct layer while the engine
captures its own PNG of the presented frame (the "live" frame), the capture file is replayed without a window, and the frame hash of the replay is compared with the
frame hash of the live frame.
Why: a Vulkan stream that replays to the same pixels is the oracle the parity work needs (best-practices review, rank 2): it proves the engine's Vulkan command
stream, not the GL fallback, produced the frame, and it gives every later Vulkan change a recorded stream to compare against. Hashing the decoded RGB pixels (not
the file bytes) makes the PNG of the engine and the PNG/BMP of the replay comparable.

Usage:
  py -3 tools/visual_verify/vk_replay.py proof   --install DIR --bookmark NAME --out DIR [--frames 440-470] [--frame 431]
  py -3 tools/visual_verify/vk_replay.py capture --install DIR --bookmark NAME --out DIR [--frames 440-470] [--frame 431]
  py -3 tools/visual_verify/vk_replay.py replay  --capture FILE.gfxr --out DIR [--frames 1-31]
  py -3 tools/visual_verify/vk_replay.py compare --live PNG --shots DIR
  py -3 tools/visual_verify/vk_replay.py env     --capture FILE.gfxr --frames 440-470          (print the layer environment)
  --dry-run on capture / replay / proof prints the commands and runs nothing.
Both launches are background-only: the engine goes through tools/visual_verify/capture.py (nofocus spawn, host lease); the replay has no window (offscreen swapchain) and
holds the same host lease. Exit 0 = a replayed frame equals the live frame, 1 = no replayed frame equals it, 2 = a tool failed."""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

LAYER_NAME = "VK_LAYER_LUNARG_gfxreconstruct"
# The umbrella that makes the engine's Vulkan present path draw the frame (the backend alone falls back to GL).
VULKAN_ENV = ("MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1")
# The capture's time budget starts before it has the host lease, so it must cover a wait behind another lane's run; the engine stops early once the pose is written.
DEFAULT_DURATION_S = 3000
DEFAULT_FRAMES = (440, 470)          # application frames to capture; the engine's own capture fires at its trigger frame (431 + settle) inside it


def sdk_bin(environ=None) -> Path:
    """The Vulkan SDK `Bin` directory: MC2_TOOL_VULKAN_SDK, then VULKAN_SDK, then the newest C:/VulkanSDK/* (the order docs/host-tools.md lists)."""
    env = os.environ if environ is None else environ
    for name in ("MC2_TOOL_VULKAN_SDK", "VULKAN_SDK"):
        if env.get(name):
            return Path(env[name]) / "Bin"
    found = sorted(glob.glob("C:/VulkanSDK/*/Bin"))
    if not found:
        raise FileNotFoundError("no Vulkan SDK found (set MC2_TOOL_VULKAN_SDK or VULKAN_SDK)")
    return Path(found[-1])


def replay_exe(environ=None) -> Path:
    env = os.environ if environ is None else environ
    if env.get("MC2_TOOL_GFXRECON_REPLAY"):
        return Path(env["MC2_TOOL_GFXRECON_REPLAY"])
    return sdk_bin(environ) / "gfxrecon-replay.exe"


def parse_frames(text: str) -> "tuple[int, int]":
    """`440-470` or `455` (one frame) as (first, last), 1-based and inclusive; a reversed or zero range is an error."""
    first, _, last = text.partition("-")
    a, b = int(first), int(last or first)
    if a < 1 or b < a:
        raise ValueError(f"bad frame range {text!r}: frames are 1-based and the range must not be reversed")
    return a, b


def capture_env(capture_file: Path, frames: "tuple[int, int]", layer_dir: Path, log_file: "Path | None" = None, environ=None) -> "dict[str, str]":
    """The variables that put the GFXReconstruct layer under the engine (the ones gfxrecon-capture-vulkan.py sets), as a dict to add to the child environment."""
    env = os.environ if environ is None else environ
    path = str(layer_dir)
    if env.get("VK_LAYER_PATH"):
        path += os.pathsep + env["VK_LAYER_PATH"]
    layers = LAYER_NAME if not env.get("VK_INSTANCE_LAYERS") else LAYER_NAME + os.pathsep + env["VK_INSTANCE_LAYERS"]
    out = {
        "VK_INSTANCE_LAYERS": layers,
        "VK_LAYER_PATH": path,
        "GFXRECON_CAPTURE_FILE": str(capture_file),
        "GFXRECON_CAPTURE_FILE_TIMESTAMP": "false",     # the file name is the one the replay stage is given
        "GFXRECON_CAPTURE_FRAMES": f"{frames[0]}-{frames[1]}",
        "GFXRECON_LOG_LEVEL": "info",
    }
    if log_file is not None:
        out["GFXRECON_LOG_FILE"] = str(log_file)
    return out


def capture_command(install: Path, bookmark: str, out_dir: Path, frame: int, extra_env: "list[str] | None" = None, duration: int = DEFAULT_DURATION_S) -> "list[str]":
    """The engine capture that produces the live frame: tools/visual_verify/capture.py with the Vulkan umbrella (a backend request alone falls back to GL)."""
    cmd = [sys.executable, str(HERE / "capture.py"), "--install", str(install), "--bookmarks", bookmark, "--frame", str(frame), "--duration", str(duration), "--clean", "--out", str(out_dir)]
    for item in list(VULKAN_ENV) + list(extra_env or []):
        cmd += ["--env", item]
    return cmd


def replay_command(exe: Path, capture_file: Path, shot_dir: Path, frames: "tuple[int, int]", log_file: "Path | None" = None) -> "list[str]":
    """Windowless replay of `capture_file` (offscreen swapchain: no surface, no window; --wsi headless still tries to create the captured Win32 surface) writing PNG screenshots of replay frames `frames` (1-based, counted from the first captured frame)."""
    cmd = [str(exe), "--swapchain", "offscreen", "--screenshots", f"{frames[0]}-{frames[1]}", "--screenshot-format", "png", "--screenshot-dir", str(shot_dir),
           "--screenshot-prefix", "replay"]
    if log_file is not None:
        cmd += ["--log-file", str(log_file)]
    return cmd + [str(capture_file)]


def decode_rgb(path: Path):
    """The pixels of a PNG / BMP as an (h, w, 3) uint8 array, alpha dropped (a swapchain image and an engine PNG differ in channel layout, not in colour)."""
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.uint8)


def frame_hash(path: Path) -> str:
    """sha256 over the decoded RGB bytes plus the size: equal iff the two images show the same pixels at the same resolution."""
    pixels = decode_rgb(path)
    h = hashlib.sha256()
    h.update(f"{pixels.shape[1]}x{pixels.shape[0]}".encode("ascii"))
    h.update(pixels.tobytes())
    return h.hexdigest()


def replay_frame_number(name: str) -> "int | None":
    """The 1-based replay frame in a screenshot name (`replay_frame_23.png` -> 23), None for any other name."""
    m = re.search(r"replay_frame_(\d+)\.", name)
    return int(m.group(1)) if m else None


def compare(live_png: Path, shot_dir: Path, first_app_frame: "int | None" = None) -> dict:
    """Hash the live frame and every screenshot in `shot_dir`; `matches` lists the screenshots whose hash equals the live one, and with `first_app_frame` (the
    first frame GFXRECON_CAPTURE_FRAMES recorded) `app_frames` is the application frame each match is (replay frame k is application frame first + k - 1)."""
    live = frame_hash(live_png)
    shots = []
    for p in sorted(shot_dir.glob("*.png")) + sorted(shot_dir.glob("*.bmp")):
        shots.append({"file": p.name, "hash": frame_hash(p)})
    matches = [s["file"] for s in shots if s["hash"] == live]
    out = {"live": str(live_png), "live_hash": live, "shots": shots, "matches": matches, "equal": bool(matches)}
    if first_app_frame is not None:
        out["app_frames"] = [first_app_frame + n - 1 for n in map(replay_frame_number, matches) if n is not None]
    return out


def captured_file(out: Path) -> Path:
    """The capture file the layer wrote: with GFXRECON_CAPTURE_FRAMES it appends the frame range to the name (mc2.gfxr -> mc2_frames_440_through_470.gfxr)."""
    files = sorted(out.glob("*.gfxr"), key=lambda p: p.stat().st_size, reverse=True)
    if not files:
        raise FileNotFoundError(f"the GFXReconstruct layer wrote no .gfxr under {out} (is the layer on VK_LAYER_PATH, and did the engine run on Vulkan?)")
    return files[0]


def find_live_png(capture_dir: Path, bookmark: str) -> Path:
    pngs = sorted((capture_dir / bookmark).glob("*.png"))
    if not pngs:
        raise FileNotFoundError(f"the engine capture wrote no PNG under {capture_dir / bookmark}")
    return pngs[0]


def run_capture(args) -> int:
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    frames = parse_frames(args.frames)
    gfxr = out / "mc2.gfxr"                                     # the name asked for; the layer adds the frame range, see captured_file()
    cmd = capture_command(Path(args.install), args.bookmark, out / "live", args.frame, duration=args.duration)
    env = dict(os.environ)
    env.update(capture_env(gfxr, frames, sdk_bin(), out / "gfxrecon_capture.log"))
    if args.dry_run:
        print(json.dumps({"command": cmd, "environment": {k: v for k, v in env.items() if k.startswith(("GFXRECON_", "VK_"))}}, indent=1))
        return 0
    # capture.py spawns the engine through nofocus (non-activating window, host lease): this process must not hold the lease itself.
    # background-launch: allow capture.py is the harness; the engine is launched there through nofocus.spawn
    r = subprocess.run(cmd, env=env, cwd=str(ROOT), **console_child.child_kwargs())
    try:
        recorded = captured_file(out)
    except FileNotFoundError as e:
        print(f"[vk_replay] capture failed (exit {r.returncode}): {e}", file=sys.stderr)
        return 2
    if r.returncode != 0:
        print(f"[vk_replay] the engine capture exited {r.returncode}", file=sys.stderr)
        return 2
    print(f"[vk_replay] capture file {recorded} ({recorded.stat().st_size / 1e6:.1f} MB), live frame {find_live_png(out / 'live', args.bookmark)}")
    return 0


def run_replay(args) -> int:
    out = Path(args.out).resolve()
    shots = out / "replay"
    shots.mkdir(parents=True, exist_ok=True)
    gfxr = Path(args.capture) if args.capture else (out / "mc2.gfxr" if args.dry_run else captured_file(out))
    first, last = parse_frames(args.frames) if args.frames else (1, 1 + (DEFAULT_FRAMES[1] - DEFAULT_FRAMES[0]))
    cmd = replay_command(replay_exe(), gfxr, shots, (first, last), out / "gfxrecon_replay.log")
    if args.dry_run:
        print(json.dumps({"command": cmd}, indent=1))
        return 0
    import smoke_lib.host_lease as host_lease
    host_lease.ensure_held("vk_replay " + gfxr.name)             # the replay uses the GPU: one GPU job per host
    r = subprocess.run(cmd, cwd=str(out), **console_child.child_kwargs())
    if r.returncode != 0:
        print(f"[vk_replay] gfxrecon-replay failed (exit {r.returncode})", file=sys.stderr)
        return 2
    return 0


def run_compare(args) -> int:
    result = compare(Path(args.live), Path(args.shots), getattr(args, "first_frame", None))
    print(json.dumps(result, indent=1))
    distinct = len({s["hash"] for s in result["shots"]})
    where = f" = application frame(s) {result['app_frames']}" if result.get("app_frames") else ""
    print(f"[vk_replay] {'EQUAL' if result['equal'] else 'DIFFERENT'}: live {result['live_hash'][:16]} vs {len(result['shots'])} replayed frame(s) "
          f"({distinct} distinct); matches {result['matches'] or 'none'}{where}")
    return 0 if result["equal"] else 1


def run_proof(args) -> int:
    rc = run_capture(args)
    if args.dry_run:
        return rc or run_replay(argparse.Namespace(out=args.out, capture=None, frames=None, dry_run=True))
    if rc:
        return rc
    rc = run_replay(argparse.Namespace(out=args.out, capture=None, frames=None, dry_run=False))
    if rc:
        return rc
    return run_compare(argparse.Namespace(live=str(find_live_png(Path(args.out) / "live", args.bookmark)), shots=str(Path(args.out) / "replay"),
                                          first_frame=parse_frames(args.frames)[0]))


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("capture", "proof"):
        p = sub.add_parser(name)
        p.add_argument("--install", required=True)
        p.add_argument("--bookmark", required=True, help="a bookmark stem of <install>/saved_bookmarks, e.g. mc2_01_saved_205644")
        p.add_argument("--out", required=True)
        p.add_argument("--frames", default=f"{DEFAULT_FRAMES[0]}-{DEFAULT_FRAMES[1]}", help="application frames to record (GFXRECON_CAPTURE_FRAMES)")
        p.add_argument("--frame", type=int, default=431, help="the engine's deterministic trigger frame")
        p.add_argument("--duration", type=int, default=DEFAULT_DURATION_S, help="time budget in seconds, host-lease wait included")
        p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("replay")
    p.add_argument("--capture", default=None)
    p.add_argument("--out", required=True)
    p.add_argument("--frames", default=None, help="replay frames to screenshot, 1-based from the first recorded frame")
    p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("compare")
    p.add_argument("--live", required=True)
    p.add_argument("--shots", required=True)
    p.add_argument("--first-frame", type=int, default=None, help="the first application frame the capture recorded; names the application frame of a match")
    p = sub.add_parser("env")
    p.add_argument("--capture", required=True)
    p.add_argument("--frames", default=f"{DEFAULT_FRAMES[0]}-{DEFAULT_FRAMES[1]}")
    args = ap.parse_args(argv)
    if args.cmd == "env":
        for k, v in sorted(capture_env(Path(args.capture), parse_frames(args.frames), sdk_bin()).items()):
            print(f"{k}={v}")
        return 0
    return {"capture": run_capture, "replay": run_replay, "compare": run_compare, "proof": run_proof}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
