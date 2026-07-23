#!/usr/bin/env python3
"""Terrain sub-feature free-capture oracle (natural close game camera, f200).
Runs GL-full vs VK-terrain at the natural camera where VK LOD0 fills the screen,
so near-camera terrain detail (anti-tile / POM / markings) IS exercised (bookmark
free-cams frame coarse-LOD terrain -> feature inert). Fixed-step+seed for stability.

Usage: terrain_freecap.py <tag> <gateflag=1 ...>   (extra VK gates layered on VK run)
Emits <out>/gl.png, <out>/vk.png under .claude/mech-parity/terrain/<tag>/.
"""
import os, sys, subprocess, time, shutil
from pathlib import Path

EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")

def run(name, mission, frame, gates, out_dir, duration=40):
    out_dir = Path(out_dir).resolve(); out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "MC2_SMOKE_MODE":"1","MC2_LOG":"1","MC2_SMOKE_SEED":"0xC0FFEE",
        "MC2_SMOKE_FIXED_TIMESTEP":"1","MC2_HEARTBEAT":"1","MC2_NO_LAUNCHER":"1",
        "MC2_VISUAL_CAPTURE_DIR":str(out_dir),
        "MC2_VISUAL_CAPTURE_FRAME":str(frame),
    })
    for k,v in gates.items(): env[k]=v
    si=None
    if os.name=="nt":
        si=subprocess.STARTUPINFO(); si.dwFlags|=subprocess.STARTF_USESHOWWINDOW; si.wShowWindow=7
    args=[str(EXE),"--profile","stock","--mission",mission,"--duration",str(duration)]
    log=out_dir/f"{name}.log"
    print(f"[terr-freecap] {name} vk_gates={ {k:v for k,v in gates.items() if 'VK' in k} }")
    with open(log,"w",encoding="utf-8",errors="replace") as lf:
        p=subprocess.Popen(args,stdout=lf,stderr=subprocess.STDOUT,cwd=str(EXE.parent),env=env,startupinfo=si)
        expected=out_dir/f"{mission}_frame{frame}.png"
        deadline=time.time()+duration+30
        while time.time()<deadline:
            if expected.exists(): time.sleep(1.0); break
            if p.poll() is not None: break
            time.sleep(0.5)
        try: p.wait(timeout=15)
        except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=10)
    dst=out_dir/f"{name}.png"
    if expected.exists(): shutil.copy(expected,dst); print(f"[terr-freecap] {name} -> {dst}"); return dst
    print(f"[terr-freecap] {name}: NO CAPTURE ({log})"); return None

if __name__=="__main__":
    tag=sys.argv[1] if len(sys.argv)>1 else "antitile"
    extra={}
    for a in sys.argv[2:]:
        k,_,v=a.partition("="); extra[k]=v or "1"
    mission="mc2_01"; frame=200
    out=fr"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/terrain/{tag}_f{frame}"
    common={"MC2_RENDER_BACKEND":"vulkan"}
    vk=dict(common, MC2_VK_SHARED_SCENE_DEPTH="1", MC2_VK_TERRAIN_TIER1="1",
            MC2_VK_TERRAIN_LIGHTING="1", MC2_VK_TERRAIN_DETAIL="1", **extra)
    run("gl", mission, frame, dict(common, MC2_VK_SHARED_SCENE_DEPTH="1"), out)
    run("vk", mission, frame, vk, out)
    print(f"[terr-freecap] done dir={out}")
