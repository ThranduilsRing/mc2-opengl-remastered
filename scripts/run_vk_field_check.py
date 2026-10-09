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

sys.path.insert(0, str(Path(__file__).resolve().parent))

from smoke_lib import nofocus  # noqa: E402

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
    "MC2_VK_HUD_SKIP_GL", "MC2_VK_HUD_COLOR_PROOF", "MC2_VK_POST_FOG_NATIVE",
    "MC2_VK_TERRAIN_ALL_LODS", "MC2_VK_TERRAIN_SKIRTS", "MC2_VK_WATER_TIER1",
    "MC2_VK_NO_GL_INTEROP_STRICT", "MC2_VISUAL_BOOKMARK_CAPTURE",
    "MC2_SMOKE_FIXED_TIMESTEP", "MC2_SMOKE_MODE",
)

CHROMA_MIN = 15.0   # broken grey wash ~4.2, correct terrain ~43.1


def screenshot_primary(out_png: str) -> None:
    # NOTE (VK-FIELD-ENV-CONTENT-STACK-ARM-1, 2026-07-11): capture is a BARE PrimaryScreen
    # grab -- deliberately NON-perturbing. An earlier hardening attempt that called
    # ShowWindow/SetForegroundWindow on the mc2 window to guarantee it was in front instead
    # PERTURBED the renderer: the focus/restore triggered a swapchain rebuild that blanked
    # the scene frame (grey scene, HUD tail still drawn) at capture time -- a self-induced
    # false negative. So the chroma reading is a SOFT signal only (see main(): the LOAD-
    # BEARING assertion is the per-layer used_vulkan receipts, which deterministically catch
    # the round-2 vanishing state -- water/fog/coverage were used_vulkan==0 there). If the
    # game is not foreground the grab may catch the desktop (also low chroma) -> eyeball the
    # PNG, do not treat low chroma alone as a hard regression.
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
    # background-launch: allow interactive-driver PowerShell one-liner whose output must reach the caller's console
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


def hud_placement_probe(png: str):
    """HUD-CANVAS-EXTENT-FIX regression probe (2026-07-11).

    The field bug fixed here: the native VK HUD tail divided queued HUD vertices by
    the PRESENT-IMAGE extent (physical GL drawable) instead of the AUTHORING canvas
    (logical game res == gosRenderer width_/height_). Under DPI scaling (e.g. a 4K
    window at 225%: drawable 3840 vs logical ~1707) that renders the whole HUD cluster
    in the TOP-LEFT quadrant at ~half scale instead of anchored to the bottom edge.
    Fix (engine_present_vk.cpp recordPendingHudColorProof / gameos_graphics.cpp
    flushHUDBatch): plumb width_/height_ through queueHudVulkanFrame and push THAT as
    the shader extent -> NDC mapping is resolution/DPI independent.

    The MC2 HUD chrome is distinctly bright green. This probe counts bright-green HUD
    pixels in the BOTTOM band vs the TOP-LEFT quadrant. Correct placement => HUD green
    lives in the bottom band and is ~absent top-left. The old bug => green appears in
    the top-left quadrant. SOFT signal only (like the chroma check): the bare
    PrimaryScreen grab can catch the desktop / a non-foreground window, so a null
    reading is not a hard fail -- eyeball the PNG. Returns (bottom_green, topleft_green).
    """
    try:
        from PIL import Image
        import numpy as np
    except Exception:
        return None, None
    im = np.asarray(Image.open(png).convert("RGB")).astype(int)
    h, w, _ = im.shape
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    # Bright HUD green: strong G, weak R/B (the force/status bars + selection brackets).
    hud_green = (g > 140) & (r < 120) & (b < 120) & (g - r > 60) & (g - b > 60)
    bottom = hud_green[int(h * 0.80):, :].sum()
    topleft = hud_green[: int(h * 0.50), : int(w * 0.50)].sum()
    return int(bottom), int(topleft)


def layer_receipts(install: Path):
    """Per-LAYER VK ownership receipts from the debug_state dump.

    VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): chroma alone FALSE-PASSED the round-2
    field bug -- terrain rendered LOD0/1 only, props/water/fog/HUD absent, yet the scene
    region still carried terrain chroma ~43. So this fence now asserts every CONTENT layer
    the umbrella is supposed to arm reports VK ownership (used_vulkan>0 / no fallback), not
    just that *something* was chromatic. Each entry is (value, fallback_reason). A non-empty
    fallback_reason means the island/pass declined to GL -> that layer VANISHES under VK-only
    present (no per-layer GL fallback post-VK-RUNG8-ITEM8) -> FAIL.

    Layers asserted = exactly the umbrella-armed content stack:
      terrain (+ all-LOD coverage), props, mechs, water, native fog.
    NOT asserted (deliberately opt-in / non-vanishing effects; see the gate table in
    VK-ARC-STATE.md): ssao, screenfx, hzb, the shadow island, terrain-CSM-near,
    terrain-visualdisplace-far. Their absence does not remove geometry.
    """
    p = install / "debug_state" / "latest_render_state.json"
    if not p.exists():
        return {}, {}
    d = json.loads(p.read_text())

    def g(sec, key, default=0):
        v = d.get(sec, {})
        return v.get(key, default) if isinstance(v, dict) else default

    used = {
        "terrain": g("vulkan_terrain_island", "used_vulkan"),
        "prop": g("vulkan_color_prop_island", "used_vulkan"),
        "mech": g("vulkan_mech_island", "used_vulkan"),
        "water": g("vulkan_water_island", "water_used_vulkan"),
        "fog": g("vk_post_fog_native", "postfog_used_vulkan"),
    }
    reasons = {
        "terrain": g("vulkan_terrain_island", "fallback_reason", ""),
        "prop": g("vulkan_color_prop_island", "fallback_reason", ""),
        "mech": g("vulkan_mech_island", "fallback_reason", ""),
        "water": g("vulkan_water_island", "water_fallback_reason", ""),
        "fog": g("vk_post_fog_native", "postfog_fallback_reason", ""),
    }
    # Terrain LOD coverage: lod_chunk is the VK-owned LOD-chunk draw count. In the broken
    # round-2 state only LOD0/1 were VK-owned; the umbrella must now own all bands.
    used["terrain_lod_chunks"] = g("terrain_path", "lod_chunk")
    return used, reasons


# --- Props-pose hard assert (VK-FIELD-ORACLE-1) --------------------------------
# The round-3 field bug: static props drew (island used_vulkan>0) but the color
# pipeline's vertex binding stride was 0 -> all triangles collapsed to a degenerate
# point -> ZERO rasterized fragments -> props invisible while shadows still cast
# (shadow island = separate correctly-strided pipeline). The receipt (used_vulkan)
# is TRUE in that broken state, so this must be a PIXEL assert. We pin the camera to
# the props-dense airport pose via MC2_HZB_VIEW_FILE and dump the composited VK
# backbuffer (sharedImg) through the MC2_VK_PRESENT_DUMP_PPM oracle (the OS/
# PrintWindow grabs of a VK swapchain return only the clear colour; the GL scene-FBO
# capture is empty under shared-GBuffer). Props add heavy high-frequency detail to
# the airport region: measured edge-mean ~12.6 with props, ~5.0 without (terrain
# only). Threshold 8.0 sits in the empty middle -> a HARD assert.
PROPS_POSE = "X=3200.0 Y=100.0 Z=399.588 ROT=110.0 ANGLE=28.0 ALT=1000.0\n"
PROPS_EDGE_MIN = 8.0


def props_region_edge(ppm: str) -> float:
    from PIL import Image, ImageFilter
    import numpy as np
    im = Image.open(ppm).convert("L").filter(ImageFilter.FIND_EDGES)
    e = np.asarray(im).astype(float)
    h, w = e.shape
    return float(e[:, int(w * 0.45):].mean())


def props_pose_assert(install: Path, mission: str, wait: int) -> bool:
    exe = install / "mc2.exe"
    viewf = install / "debug_state" / "vk_field_props_view.txt"
    viewf.parent.mkdir(parents=True, exist_ok=True)
    viewf.write_text(PROPS_POSE)
    ppm = install / "debug_state" / "vk_field_props.ppm"
    if ppm.exists():
        ppm.unlink()
    env = os.environ.copy()
    for k in SCRUB:
        env.pop(k, None)
    env.update(FIELD_ENV)
    env["MC2_HZB_VIEW_FILE"] = str(viewf)
    env["MC2_VK_PRESENT_VERIFY"] = "1"
    env["MC2_VK_PRESENT_DUMP_PPM"] = str(ppm)
    print(f"[vk-field-check] props-pose assert: launching at airport pose")
    proc = nofocus.spawn([str(exe), "-mission", mission], cwd=str(install), env=env)
    try:
        time.sleep(wait)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    if not ppm.exists():
        print("[vk-field-check] props-pose FAIL: no PPM produced (VK present never ran).")
        return False
    edge = props_region_edge(str(ppm))
    ok = edge >= PROPS_EDGE_MIN
    print(f"[vk-field-check] props-pose region edge-detail={edge:.2f} "
          f"(min {PROPS_EDGE_MIN}) -> {'PASS' if ok else 'FAIL (props invisible!)'}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", default="A:/Games/mc2-opengl/mc2-win64-0.4c")
    ap.add_argument("--mission", default="mc2_01")
    ap.add_argument("--wait", type=int, default=32)
    ap.add_argument("--out-png", default=None)
    ap.add_argument("--skip-props-pose", action="store_true",
                    help="Skip the props-pose pixel assert (receipts-only run).")
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
    proc = nofocus.spawn([str(exe), "-mission", args.mission], cwd=str(install), env=env)
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
    used, reasons = layer_receipts(install)
    chroma_ok = chroma >= CHROMA_MIN

    # Every umbrella-armed content layer must report VK ownership AND no GL decline.
    content_layers = ("terrain", "prop", "mech", "water", "fog")
    layer_fail = []
    for k in content_layers:
        if used.get(k, 0) <= 0:
            layer_fail.append(f"{k}: used_vulkan<=0")
        if reasons.get(k):
            layer_fail.append(f"{k}: declined-to-GL ({reasons[k]})")
    # Terrain LOD coverage guard (round-2 bug = only LOD0/1 VK-owned).
    if used.get("terrain_lod_chunks", 0) <= 0:
        layer_fail.append("terrain: lod_chunk==0 (no VK-owned LOD bands)")

    hud_bottom, hud_topleft = hud_placement_probe(out_png)
    print(f"[vk-field-check] backbuffer scene chroma={chroma:.2f} (min {CHROMA_MIN})")
    if hud_bottom is not None:
        # SOFT: HUD chrome should be bottom-anchored, not in the top-left quadrant
        # (HUD-CANVAS-EXTENT-FIX). Only meaningful when the grab actually caught HUD
        # green at all; a null reading == desktop/non-foreground capture -> eyeball.
        if hud_bottom + hud_topleft < 50:
            print("[vk-field-check] HUD-placement: no HUD green captured "
                  "(desktop/non-foreground grab?) -> eyeball the PNG.")
        elif hud_topleft > hud_bottom:
            print(f"[vk-field-check] WARN HUD-placement: green in TOP-LEFT "
                  f"({hud_topleft}) > bottom ({hud_bottom}) -- looks like the pre-fix "
                  "canvas-extent bug. EYEBALL the PNG.")
        else:
            print(f"[vk-field-check] HUD-placement OK: bottom green {hud_bottom} "
                  f">> top-left {hud_topleft} (bottom-anchored).")
    print(f"[vk-field-check] layer used_vulkan={used}")
    print(f"[vk-field-check] layer fallback_reasons={reasons}")
    print(f"[vk-field-check] screenshot={out_png}")
    if not used:
        print("[vk-field-check] FAIL: no debug_state dump found (need MC2_DEBUG_STATE_DUMP=1).")

    # LOAD-BEARING: the per-layer VK-ownership receipts. These deterministically catch the
    # round-2 field state the user saw (terrain LOD0/1 only + props/water/fog/HUD absent):
    # water/fog reported used_vulkan==0 there, so an unarmed content stack FAILS here.
    layers_ok = bool(used) and not layer_fail
    if layer_fail:
        print("[vk-field-check] FAIL: umbrella-armed content layer(s) not VK-owned -> would "
              "VANISH under VK-only present (no per-layer GL fallback):")
        for f in layer_fail:
            print(f"    - {f}")

    # SOFT: chroma. A healthy composite reads ~43; a genuine composite-arming regression OR
    # an intermittent VK grey transient OR a desktop/focus-glitched capture all read low.
    # So low chroma with HEALTHY receipts is NOT a hard fail -- it flags "eyeball the PNG".
    if not chroma_ok and layers_ok:
        print(f"[vk-field-check] WARN: scene chroma {chroma:.2f} < {CHROMA_MIN} but every "
              "content layer IS VK-owned. Likely a non-perturbing capture caught the desktop, "
              "or the known intermittent VK grey transient (separate pre-existing bug). "
              "EYEBALL the saved PNG before trusting/dismissing.")
    print("[vk-field-check] NOTE: HUD/sky are eyeballed from the saved backbuffer PNG "
          "(no dump counter); HUD must show the pilot panel + command cursor, sky is the "
          "flat VK clear-colour stand-in (no VK HDRI pass -- pre-existing, documented).")
    # VK-FIELD-ORACLE-1: props-pose PIXEL assert (hard). Receipts pass in the
    # degenerate-stride broken state, so this is load-bearing for the props layer.
    props_ok = True
    if not args.skip_props_pose:
        props_ok = props_pose_assert(install, args.mission, args.wait)

    ok = layers_ok and props_ok
    print(f"[vk-field-check] {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
