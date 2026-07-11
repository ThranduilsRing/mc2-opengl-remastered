#!/usr/bin/env python3
"""VK FIELD-ENV regression fence (VK-FIELD-ENV-COMPOSITE-ARM-1).

Launches mc2.exe with EXACTLY the product/field env -- MC2_RENDER_BACKEND=vulkan
plus the MC2_VK_SHARED_GBUFFER umbrella plus the launcher's default GL gates, and
NOTHING ELSE. No runner-only strictness/present/composite gates. This is the config
the tracy-vk-compare.bat "on" state and a real launcher-driven VK session actually
use, and the one the strict proof env historically did NOT exercise.

WHY A BACKBUFFER ORACLE (not island counters / not the scene FBO): the field bug
this fence guards (present-path gates MC2_VK_POST_COMPOSITE / MC2_VK_PRESENT_DIRECT_FBO
not arming from the umbrella) left every VK island reporting used_vulkan > 0 -- the
islands RAN and stole the GL scene draws -- but their output was never composited to
the backbuffer, so terrain went invisible under a grey wash. The engine's own
glReadPixels capture reads the SCENE FBO (see gos_visual_capture.cpp:204) which shows
the VK scene regardless of whether present composited it -> a FALSE PASS. This fence
therefore takes an OS-level screenshot of the presented window (the true backbuffer)
and asserts the scene region carries real terrain chroma. Broken grey wash ~= chroma 4;
correct textured terrain ~= chroma 43. Threshold 15 sits in the empty middle.

Also asserts the VK ownership receipts (terrain/prop/mech island used_vulkan > 0) from
the debug_state dump as a secondary signal -- but NOTE those are true in the broken
state too, so the backbuffer chroma check is the load-bearing assertion.

Exit 0 = field env renders correctly. Exit 1 = broken / regressed.

Usage:
  py -3 scripts/run_vk_field_check.py \
      --install A:/Games/mc2-opengl/mc2-win64-0.4c --mission mc2_01 --wait 32
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

FIELD_ENV = {
    "MC2_NO_LAUNCHER": "1",
    "MC2_DEBUG_STATE_DUMP": "1",
    "MC2_SHADOW_CSM": "1",
    "MC2_STATIC_PROP_PBR_V1": "1",
    "MC2_STATIC_PROP_AMBIENT_V1": "1",
    "MC2_STATIC_PROP_IBL_SH": "1",
    "MC2_RENDER_BACKEND": "vulkan",
    "MC2_VK_SHARED_GBUFFER": "1",
}

# The runner-only present/composite/strict gates the field env MUST NOT set -- the
# whole point is proving the umbrella arms the present chain by itself. If any of
# these leaks in from the outer shell we scrub it so the check stays honest.
SCRUB = (
    "MC2_VK_POST_COMPOSITE", "MC2_VK_PRESENT_DIRECT_FBO", "MC2_VK_PRESENT_TAIL_HOOK",
    "MC2_VK_SHARED_SCENE_STRICT", "MC2_VK_PRESENT_NATIVE_STRICT",
    "MC2_VK_PRESENT_NO_GL_IMPORT", "MC2_VK_PRESENT_SKIP_GL_HANDOFF",
    "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN", "MC2_VK_HUD", "MC2_VK_IMGUI",
    "MC2_VK_TERRAIN_ALL_LODS", "MC2_VK_TERRAIN_SKIRTS", "MC2_VK_WATER_TIER1",
    "MC2_VK_NO_GL_INTEROP_STRICT", "MC2_VISUAL_BOOKMARK_CAPTURE",
    "MC2_SMOKE_FIXED_TIMESTEP", "MC2_SMOKE_MODE",
)

CHROMA_MIN = 15.0   # broken grey wash ~4.2, correct terrain ~43.1


def screenshot_primary(out_png: str) -> None:
    ps = (
        "Add-Type -AssemblyName System.Windows.Forms;"
        "Add-Type -AssemblyName System.Drawing;"
        "$b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds;"
        "$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height;"
        "$g=[System.Drawing.Graphics]::FromImage($bmp);"
        "$g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size);"
        f"$bmp.Save('{out_png}',[System.Drawing.Imaging.ImageFormat]::Png);"
        "$g.Dispose();$bmp.Dispose()"
    )
    subprocess.check_call(["powershell", "-NoProfile", "-Command", ps])


def scene_chroma(png: str) -> float:
    from PIL import Image
    import numpy as np
    im = np.asarray(Image.open(png).convert("RGB")).astype(float)
    h, w, _ = im.shape
    c = im[int(h * 0.15):int(h * 0.75), int(w * 0.2):int(w * 0.8)]
    mx = c.max(2)
    mn = c.min(2)
    return float((mx - mn).mean())


def island_receipts(install: Path):
    p = install / "debug_state" / "latest_render_state.json"
    if not p.exists():
        return {}
    d = json.loads(p.read_text())
    out = {}
    for k in ("vulkan_terrain_island", "vulkan_color_prop_island", "vulkan_mech_island"):
        v = d.get(k, {})
        out[k] = v.get("used_vulkan", 0) if isinstance(v, dict) else 0
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", default="A:/Games/mc2-opengl/mc2-win64-0.4c")
    ap.add_argument("--mission", default="mc2_01")
    ap.add_argument("--wait", type=int, default=32)
    ap.add_argument("--out-png", default=None)
    args = ap.parse_args()

    install = Path(args.install)
    exe = install / "mc2.exe"
    if not exe.exists():
        print(f"[vk-field-check] FAIL: exe not found: {exe}")
        return 1
    out_png = args.out_png or str(install / "debug_state" / "vk_field_check.png")

    env = os.environ.copy()
    for k in SCRUB:
        env.pop(k, None)
    env.update(FIELD_ENV)

    print(f"[vk-field-check] launching {exe} -mission {args.mission} (field env only)")
    proc = subprocess.Popen([str(exe), "-mission", args.mission], cwd=str(install), env=env)
    ok = False
    try:
        time.sleep(args.wait)
        screenshot_primary(out_png)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    chroma = scene_chroma(out_png)
    receipts = island_receipts(install)
    islands_ok = all(v > 0 for v in receipts.values()) if receipts else False
    chroma_ok = chroma >= CHROMA_MIN

    print(f"[vk-field-check] backbuffer scene chroma={chroma:.2f} (min {CHROMA_MIN})")
    print(f"[vk-field-check] island receipts={receipts}")
    print(f"[vk-field-check] screenshot={out_png}")
    ok = chroma_ok and islands_ok
    if not chroma_ok:
        print("[vk-field-check] FAIL: scene region is a low-chroma grey wash -- the VK "
              "scene did not composite to the backbuffer (present-path gates not armed "
              "by the umbrella?).")
    if not islands_ok:
        print("[vk-field-check] FAIL: a VK scene island reported used_vulkan==0 "
              "(shared-gbuffer scene not owned by VK).")
    print(f"[vk-field-check] {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
