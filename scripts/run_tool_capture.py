#!/usr/bin/env python3
"""run_tool_capture - launch a viewport tool in the background, let it write one swapchain screenshot, and grade the PNG.

WHAT: the smallest launch-and-screenshot runner for the Vulkan viewport tools (tools/common/tool_vk_shell.*):
  asset_viewer (mc2_asset_viewer.exe), mc2fx_preview, mc2weapon_gui.
  It takes the host-wide game lease, starts the tool through nofocus.spawn (hidden window, no focus, no cursor), sets
  MC2_TOOL_CAPTURE=<png> (the tool writes the swapchain image after frame MC2_TOOL_CAPTURE_FRAME and exits), waits, then checks the
  PNG exists, is not one flat colour, and (with --validation) that the tool printed `[TOOLVK] validation errors=0`.
WHY: the owner rule is that nothing launches in the foreground; tools had no harness at all (they were run by hand).
USAGE:
  py -3 scripts/run_tool_capture.py mc2weapon_gui --out out.png
  py -3 scripts/run_tool_capture.py mc2fx_preview --out out.png --validation -- A:/path/effects.fx
  py -3 scripts/run_tool_capture.py asset_viewer --out out.png --cwd A:/Games/mc2-opengl/mc2-win64-lane2 -- --view props data/tgl/2civliving.tgl
Exit 0 = PASS, 1 = FAIL (printed reason), 2 = the tool could not be found/launched.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from smoke_lib import host_lease, nofocus  # noqa: E402

TOOLS = {
    "asset_viewer": "mc2_asset_viewer.exe",
    "mc2fx_preview": "mc2fx_preview.exe",
    "mc2weapon_gui": "mc2weapon_gui.exe",
}


def find_exe(tool: str, tree: str) -> Path | None:
    name = TOOLS[tool]
    base = ROOT / tree
    hits = sorted(base.rglob(name), key=lambda p: p.stat().st_mtime, reverse=True)
    return hits[0] if hits else None


def png_stats(path: Path) -> tuple[int, int, int]:
    """(width, height, distinct colours in a 4096-pixel sample) of an 8-bit RGB PNG with stored or deflated IDAT."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    colours = set()
    for y in range(0, h, max(1, h // 64)):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(0, w, max(1, w // 64)):
            colours.add(row[x * 3:x * 3 + 3])
    return w, h, len(colours)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("tool", choices=sorted(TOOLS))
    ap.add_argument("--out", required=True, help="PNG to write")
    ap.add_argument("--tree", default="build-alltargets", help="build tree under the repo root holding the tool exe")
    ap.add_argument("--exe", help="explicit tool exe (overrides --tree)")
    ap.add_argument("--cwd", help="working directory for the tool (the install for asset_viewer)")
    ap.add_argument("--frame", type=int, default=12, help="swapchain frame to capture")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--validation", action="store_true", help="MC2_VULKAN_VALIDATION=1 and require 0 validation errors")
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for the tool")
    ap.add_argument("args", nargs="*", help="arguments for the tool (after --)")
    a = ap.parse_args()

    exe = Path(a.exe) if a.exe else find_exe(a.tool, a.tree)
    if not exe or not exe.is_file():
        print(f"run_tool_capture: {a.tool} not built under {a.tree}", file=sys.stderr)
        return 2
    out = Path(a.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    log = out.with_suffix(".log")

    env = {"MC2_TOOL_CAPTURE": str(out), "MC2_TOOL_CAPTURE_FRAME": str(a.frame)}
    if a.validation:
        env["MC2_VULKAN_VALIDATION"] = "1"
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v

    # The tool exes sit in the build tree's out/ folder; SDL2.dll sits at the tree root.
    env["PATH"] = os.pathsep.join([str(ROOT / a.tree), os.environ.get("PATH", "")])

    host_lease.ensure_held("run_tool_capture " + a.tool)
    argv = [str(exe)] + list(a.args)
    with open(log, "wb") as lf:
        proc = nofocus.spawn(argv, env=env, stdout=lf, stderr=lf, cwd=a.cwd or str(exe.parent))
        t0 = time.monotonic()
        while proc.poll() is None and time.monotonic() - t0 < a.timeout:
            time.sleep(0.25)
        if proc.poll() is None:
            proc.kill()   # the pid this runner spawned, never an image-wide kill
            print(f"FAIL: {a.tool} did not exit within {a.timeout:.0f} s (killed pid {proc.pid})")
            return 1
    rc = proc.returncode
    text = log.read_text(errors="replace")
    verdicts = []
    if rc != 0:
        verdicts.append(f"exit code {rc}")
    if not out.is_file():
        verdicts.append("no PNG written")
    else:
        w, h, colours = png_stats(out)
        print(f"capture {w}x{h}, {colours} distinct sampled colours -> {out}")
        if colours < 4:
            verdicts.append("PNG is flat (fewer than 4 colours)")
    if a.validation:
        if "[TOOLVK] validation errors=0" not in text:
            verdicts.append("validation errors not 0 (see log)")
    for line in text.splitlines():
        if line.startswith("[TOOLVK]"):
            print(line)
    if verdicts:
        print("FAIL: " + "; ".join(verdicts) + f"  (log {log})")
        return 1
    print(f"PASS: {a.tool} (log {log})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
