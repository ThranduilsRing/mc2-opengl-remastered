#!/usr/bin/env python3
"""VK-WATER-TIER1 (rung 6) oracle: mc2_01 water-bookmark sweep, GL control x2
(byte-off), VK umbrella baseline (no water gate), VK + MC2_VK_WATER_TIER1=1.

VK-FIRST-BOOKMARK-POSE-1 mitigation: the sweep runs a SACRIFICIAL duplicate of
bookmark #1 first; its capture is ignored in the analysis.
"""
import os, sys, subprocess, time, json
from pathlib import Path

EXE  = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
HERE = Path(__file__).resolve().parent
BOOK_SRC = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks/mc2_01_water.json")
BOOK = HERE / "mc2_01_water_sacrificial.json"

FIELD_GL_DEFAULTS = {
    "MC2_NO_LAUNCHER":"1",
    "MC2_SHADOW_CSM":"1",
    "MC2_STATIC_PROP_PBR_V1":"1",
    "MC2_STATIC_PROP_AMBIENT_V1":"1",
    "MC2_STATIC_PROP_IBL_SH":"1",
    "MC2_STATICPROP_TEX_TIER":"512",
}
VK_BASE = {
    "MC2_RENDER_BACKEND":"vulkan",
    "MC2_VK_SHARED_GBUFFER":"1",
    "MC2_VK_SHADOW_INTEROP":"1",
    "MC2_VK_TERRAIN_CSM":"1",
    "MC2_VK_SHARED_SCENE_DEPTH":"1",
    "MC2_VK_TERRAIN_TIER1":"1",
    "MC2_VK_TERRAIN_LIGHTING":"1",
    "MC2_VK_TERRAIN_DETAIL":"1",
    "MC2_VK_TERRAIN_DYNSHADOW":"1",
}
VK_WATER = dict(VK_BASE); VK_WATER["MC2_VK_WATER_TIER1"] = "1"

def make_sacrificial_book():
    d = json.loads(BOOK_SRC.read_text(encoding="utf-8"))
    bms = d["bookmarks"]
    sac = json.loads(json.dumps(bms[0])); sac["name"] = "zzsacrificial"
    d["bookmarks"] = [sac] + bms
    BOOK.write_text(json.dumps(d), encoding="utf-8")

def run(tag, extra, duration=70, trigger=140, settle=40):
    out_dir = HERE / "cap" / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "MC2_SMOKE_MODE":"1","MC2_LOG":"1","MC2_SMOKE_SEED":"0xC0FFEE",
        "MC2_SMOKE_FIXED_TIMESTEP":"1","MC2_HEARTBEAT":"1",
        "MC2_DEBUG_STATE_DUMP":"1",
        "MC2_VISUAL_BOOKMARK_CAPTURE":str(BOOK),
        "MC2_VISUAL_CAPTURE_DIR":str(out_dir),
        "MC2_VISUAL_CAPTURE_FRAME":str(trigger),
        "MC2_VISUAL_SETTLE":str(settle),
    })
    env.update(FIELD_GL_DEFAULTS)
    env.update(extra)
    si=None
    if os.name=="nt":
        si=subprocess.STARTUPINFO(); si.dwFlags|=subprocess.STARTF_USESHOWWINDOW; si.wShowWindow=4
    args=[str(EXE),"--profile","stock","--mission","mc2_01","--duration",str(duration)]
    log=out_dir/f"{tag}.log"
    vkg={k:v for k,v in env.items() if k.startswith("MC2_VK") or k=="MC2_RENDER_BACKEND"}
    print(f"[water-oracle] {tag} vkgates={vkg}", flush=True)
    with open(log,"w",encoding="utf-8",errors="replace") as lf:
        p=subprocess.Popen(args,stdout=lf,stderr=subprocess.STDOUT,cwd=str(EXE.parent),env=env,startupinfo=si)
        deadline=time.time()+duration+60
        while time.time()<deadline:
            if p.poll() is not None: break
            time.sleep(0.5)
        if p.poll() is None:
            subprocess.run(["taskkill","/PID",str(p.pid),"/T","/F"],capture_output=True)
        try: p.wait(timeout=15)
        except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=10)
    pngs=sorted(out_dir.glob("mc2_01_*.png"))
    # receipts from log
    txt = log.read_text(encoding="utf-8", errors="replace")
    for line in txt.splitlines():
        if "[VK_WATER" in line or "used_vulkan" in line.lower():
            if "used_vulkan" in line:
                print(f"  RECEIPT {tag}: {line.strip()[:220]}", flush=True)
    print(f"[water-oracle] {tag}: {len(pngs)} PNGs pid={p.pid}", flush=True)

if __name__=="__main__":
    make_sacrificial_book()
    which = sys.argv[1:] or ["gl","gl2","vk_base","vk_water"]
    cfg = {"gl":{}, "gl2":{}, "vk_base":VK_BASE, "vk_water":VK_WATER}
    for tag in which:
        run(tag, cfg[tag])
    print("[water-oracle] done")
