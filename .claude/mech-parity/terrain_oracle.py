#!/usr/bin/env python3
"""Terrain sub-feature oracle: GL-full vs VK-terrain on the bookmark sweep
(byte-stable, fixed-step). Compares per-bookmark PNGs. Used oracle-before-feature
for the VK terrain dynamic-shadow / anti-tile / POM / markings ports."""
import os, sys, subprocess, time, shutil
from pathlib import Path

EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks")

def run(tag, mission, gates, out_dir, duration=60, trigger=120, settle=30):
    out_dir = Path(out_dir).resolve() / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "MC2_SMOKE_MODE":"1","MC2_LOG":"1","MC2_SMOKE_SEED":"0xC0FFEE",
        "MC2_SMOKE_FIXED_TIMESTEP":"1","MC2_HEARTBEAT":"1","MC2_NO_LAUNCHER":"1",
        "MC2_VISUAL_BOOKMARK_CAPTURE":str(BOOK/f"{mission}.json"),
        "MC2_VISUAL_CAPTURE_DIR":str(out_dir),
        "MC2_VISUAL_CAPTURE_FRAME":str(trigger),
        "MC2_VISUAL_SETTLE":str(settle),
    })
    for k,v in gates.items(): env[k]=v
    si=None
    if os.name=="nt":
        si=subprocess.STARTUPINFO(); si.dwFlags|=subprocess.STARTF_USESHOWWINDOW; si.wShowWindow=7
    args=[str(EXE),"--profile","stock","--mission",mission,"--duration",str(duration)]
    log=out_dir/f"{tag}.log"
    print(f"[terrain-oracle] {tag} gates={ {k:v for k,v in gates.items() if k.startswith('MC2_VK')} }")
    with open(log,"w",encoding="utf-8",errors="replace") as lf:
        p=subprocess.Popen(args,stdout=lf,stderr=subprocess.STDOUT,cwd=str(EXE.parent),env=env,startupinfo=si)
        deadline=time.time()+duration+40
        while time.time()<deadline:
            if p.poll() is not None: break
            time.sleep(0.5)
        try: p.wait(timeout=15)
        except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=10)
    pngs=sorted(out_dir.glob(f"{mission}_*.png"))
    print(f"[terrain-oracle] {tag}: {len(pngs)} bookmark PNGs")
    return out_dir

if __name__=="__main__":
    mission=sys.argv[1] if len(sys.argv)>1 else "mc2_01"
    which=sys.argv[2] if len(sys.argv)>2 else "baseline"  # baseline | dynshadow
    out=fr"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/terrain/{mission}_{which}"
    common={"MC2_RENDER_BACKEND":"vulkan"}
    vk_terr=dict(common, MC2_VK_SHARED_SCENE_DEPTH="1", MC2_VK_TERRAIN_TIER1="1",
                 MC2_VK_TERRAIN_LIGHTING="1", MC2_VK_TERRAIN_DETAIL="1")
    run("gl", mission, dict(common, MC2_VK_SHARED_SCENE_DEPTH="1"), out)
    if which=="dynshadow":
        vk_terr["MC2_VK_TERRAIN_DYNSHADOW"]="1"
    run("vk", mission, vk_terr, out)
    print(f"[terrain-oracle] done dir={out}")
