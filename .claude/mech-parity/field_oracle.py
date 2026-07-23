#!/usr/bin/env python3
"""VK-FLIP FIELD-CONFIG oracle. Runs the mc2_01 bookmark sweep under the EXACT
launcher field env (tracy-vk-compare.bat + MC2_STATICPROP_TEX_TIER=512), GL vs VK,
so the three field bugs verify under the full stack, not gate-isolated.

Usage: field_oracle.py <tag> <mission>
  captures gl + vk PNGs into .claude/mech-parity/field/<mission>_<tag>/{gl,vk}
"""
import os, sys, subprocess, time
from pathlib import Path

EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks")

# Full launcher field env (both GL and VK bake these GL defaults).
FIELD_GL_DEFAULTS = {
    "MC2_NO_LAUNCHER":"1",
    "MC2_SHADOW_CSM":"1",
    "MC2_STATIC_PROP_PBR_V1":"1",
    "MC2_STATIC_PROP_AMBIENT_V1":"1",
    "MC2_STATIC_PROP_IBL_SH":"1",
    "MC2_STATICPROP_TEX_TIER":"512",
}
# VK umbrella + the three field-fix gates.
FIELD_VK_ON = {
    "MC2_RENDER_BACKEND":"vulkan",
    "MC2_VK_SHARED_GBUFFER":"1",
    "MC2_VK_SHADOW_INTEROP":"1",
    "MC2_VK_TERRAIN_CSM":"1",
    # terrain island ownership chain (required for VK terrain to draw + shadow):
    "MC2_VK_SHARED_SCENE_DEPTH":"1",
    "MC2_VK_TERRAIN_TIER1":"1",
    "MC2_VK_TERRAIN_LIGHTING":"1",
    "MC2_VK_TERRAIN_DETAIL":"1",
    "MC2_VK_TERRAIN_DYNSHADOW":"1",
}

def run(tag, mission, extra, out_dir, duration=70, trigger=140, settle=40):
    out_dir = Path(out_dir).resolve() / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "MC2_SMOKE_MODE":"1","MC2_LOG":"1","MC2_SMOKE_SEED":"0xC0FFEE",
        "MC2_SMOKE_FIXED_TIMESTEP":"1","MC2_HEARTBEAT":"1",
        "MC2_VISUAL_BOOKMARK_CAPTURE":str(BOOK/f"{mission}.json"),
        "MC2_VISUAL_CAPTURE_DIR":str(out_dir),
        "MC2_VISUAL_CAPTURE_FRAME":str(trigger),
        "MC2_VISUAL_SETTLE":str(settle),
    })
    env.update(FIELD_GL_DEFAULTS)
    env.update(extra)
    si=None
    if os.name=="nt":
        # SW_SHOWNOACTIVATE=4: window visible (non-zero surface extent so VK does NOT
        # fail-soft to GL) but does not steal focus. SW_SHOWMINNOACTIVE(7) minimizes ->
        # zero extent -> VK backend fail-softs to GL (silently invalidates the oracle).
        si=subprocess.STARTUPINFO(); si.dwFlags|=subprocess.STARTF_USESHOWWINDOW; si.wShowWindow=4
    args=[str(EXE),"--profile","stock","--mission",mission,"--duration",str(duration)]
    log=out_dir/f"{tag}.log"
    vkg={k:v for k,v in env.items() if k.startswith("MC2_VK") or k=="MC2_RENDER_BACKEND"}
    print(f"[field-oracle] {tag} vkgates={vkg}")
    with open(log,"w",encoding="utf-8",errors="replace") as lf:
        p=subprocess.Popen(args,stdout=lf,stderr=subprocess.STDOUT,cwd=str(EXE.parent),env=env,startupinfo=si)
        deadline=time.time()+duration+50
        while time.time()<deadline:
            if p.poll() is not None: break
            time.sleep(0.5)
        try: p.wait(timeout=15)
        except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=10)
    pngs=sorted(out_dir.glob(f"{mission}_*.png"))
    print(f"[field-oracle] {tag}: {len(pngs)} bookmark PNGs pid={p.pid}")
    return out_dir

if __name__=="__main__":
    tag=sys.argv[1] if len(sys.argv)>1 else "after"
    mission=sys.argv[2] if len(sys.argv)>2 else "mc2_01"
    out=fr"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/field/{mission}_{tag}"
    run("gl", mission, {}, out)
    run("vk", mission, FIELD_VK_ON, out)
    print(f"[field-oracle] done dir={out}")
