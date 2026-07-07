#!/usr/bin/env python3
"""CSM-BLACKOUT-RECON-1: one-shot debug-viz probe. Runs VK-only, full field config
(both DYNSHADOW+CSM armed), overview_center bookmark, sweeping
MC2_VK_TERRAIN_CSM_DEBUG=1..5 to isolate which term collapses.
"""
import os, sys, subprocess, time
from pathlib import Path

EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks")
MISSION = "mc2_01"

FIELD_GL_DEFAULTS = {
    "MC2_NO_LAUNCHER": "1",
    "MC2_SHADOW_CSM": "1",
    "MC2_STATIC_PROP_PBR_V1": "1",
    "MC2_STATIC_PROP_AMBIENT_V1": "1",
    "MC2_STATIC_PROP_IBL_SH": "1",
    "MC2_STATICPROP_TEX_TIER": "512",
}
FIELD_VK_ON = {
    "MC2_RENDER_BACKEND": "vulkan",
    "MC2_VK_SHARED_GBUFFER": "1",
    "MC2_VK_SHADOW_INTEROP": "1",
    "MC2_VK_TERRAIN_CSM": "1",
    "MC2_VK_SHARED_SCENE_DEPTH": "1",
    "MC2_VK_TERRAIN_TIER1": "1",
    "MC2_VK_TERRAIN_LIGHTING": "1",
    "MC2_VK_TERRAIN_DETAIL": "1",
    "MC2_VK_TERRAIN_DYNSHADOW": "1",
}

def run(tag, extra, out_dir, duration=70, trigger=140, settle=40):
    out_dir = Path(out_dir).resolve() / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "MC2_DEBUG_STATE_DUMP": "1",
        "MC2_DEBUG_STATE_DUMP_DIR": str(out_dir),
        "MC2_SMOKE_MODE": "1", "MC2_LOG": "1", "MC2_SMOKE_SEED": "0xC0FFEE",
        "MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_HEARTBEAT": "1",
        "MC2_VISUAL_BOOKMARK_CAPTURE": str(BOOK / f"{MISSION}.json"),
        "MC2_VISUAL_CAPTURE_DIR": str(out_dir),
        "MC2_VISUAL_CAPTURE_FRAME": str(trigger),
        "MC2_VISUAL_SETTLE": str(settle),
    })
    env.update(FIELD_GL_DEFAULTS)
    env.update(extra)
    si = None
    if os.name == "nt":
        si = subprocess.STARTUPINFO(); si.dwFlags |= subprocess.STARTF_USESHOWWINDOW; si.wShowWindow = 4
    args = [str(EXE), "--profile", "stock", "--mission", MISSION, "--duration", str(duration)]
    log = out_dir / f"{tag}.log"
    print(f"[csm-debug-probe] {tag} MC2_VK_TERRAIN_CSM_DEBUG={extra.get('MC2_VK_TERRAIN_CSM_DEBUG')}")
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.Popen(args, stdout=lf, stderr=subprocess.STDOUT, cwd=str(EXE.parent), env=env, startupinfo=si)
        pid = p.pid
        deadline = time.time() + duration + 50
        while time.time() < deadline:
            if p.poll() is not None:
                break
            time.sleep(0.5)
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
            p.wait(timeout=10)
    pngs = sorted(out_dir.glob(f"{MISSION}_*.png"))
    used = "?"
    try:
        import json
        d = json.loads((out_dir / "latest_render_state.json").read_text(encoding="utf-8"))
        used = d.get("vulkan_terrain_island", {}).get("used_vulkan", "absent")
    except Exception as e:
        used = f"dump-read-fail:{e}"
    print(f"[csm-debug-probe] {tag}: {len(pngs)} bookmark PNGs pid={pid} used_vulkan={used}")
    for p in pngs:
        print(f"  {p}")
    return out_dir

if __name__ == "__main__":
    out = r"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/csm_near_field/blackout_debug"
    for mode in ("0", "1", "2", "3", "4", "5"):
        extra = dict(FIELD_VK_ON)
        extra["MC2_VK_TERRAIN_CSM_DEBUG"] = mode
        run(f"vk_dbg{mode}", extra, out)
    print(f"[csm-debug-probe] done dir={out}")
