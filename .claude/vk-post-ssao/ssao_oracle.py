#!/usr/bin/env python3
"""VK-POST-SSAO-NATIVE-1 (rung 8.2) oracle: mc2_01 AO-bookmark sweep.

Configs (all with MC2_SSAO=1 — the GL master gate; SSAO native requires it):
  gl / gl2        — byte-off control (new gate unset; GL SSAO chain, determinism)
  gl_island       — GL + region-iface VK SSAO island (the parity-proven old
                    readback path) — impl-swap attribution control, like 8.1's
                    gl_subgraph
  vk_base(2)      — VK umbrella, GL SSAO path on the shared GBuffer
  vk_ssao(2)      — vk_base + MC2_VK_POST_SSAO=1 (single variable)

AO-visible poses: near buildings/props contact areas.
VK-FIRST-BOOKMARK-POSE-1 mitigation: sacrificial duplicate of bookmark #1 first.
"""
import os, sys, subprocess, time, json
from pathlib import Path

EXE  = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
HERE = Path(__file__).resolve().parent
BOOK_SRC = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks/mc2_01.json")
BOOK = HERE / "mc2_01_ssao_sacrificial.json"

# AO-relevant subset (building/prop contact + low-angle poses).
AO_BOOKMARKS = ["runway_overview", "ridge_lowangle", "highangle_wide",
                "cliff_side_on", "overview_center"]

FIELD_GL_DEFAULTS = {
    "MC2_NO_LAUNCHER":"1",
    "MC2_SHADOW_CSM":"1",
    "MC2_STATIC_PROP_PBR_V1":"1",
    "MC2_STATIC_PROP_AMBIENT_V1":"1",
    "MC2_STATIC_PROP_IBL_SH":"1",
    "MC2_STATICPROP_TEX_TIER":"512",
    "MC2_SSAO":"1",
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
VK_SSAO = dict(VK_BASE); VK_SSAO["MC2_VK_POST_SSAO"] = "1"
GL_ISLAND = {
    "MC2_RENDER_BACKEND_REGION_IFACE":"1",
    "MC2_POSTPROCESS_BACKEND":"vk",
}

def make_sacrificial_book():
    d = json.loads(BOOK_SRC.read_text(encoding="utf-8"))
    bms = [b for b in d["bookmarks"] if b["name"] in AO_BOOKMARKS]
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
    vkg={k:v for k,v in env.items() if k.startswith("MC2_VK") or k in ("MC2_RENDER_BACKEND","MC2_SSAO","MC2_POSTPROCESS_BACKEND","MC2_RENDER_BACKEND_REGION_IFACE")}
    print(f"[ssao-oracle] {tag} gates={vkg}", flush=True)
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
    txt = log.read_text(encoding="utf-8", errors="replace")
    for line in txt.splitlines():
        if "[VK_POST_SSAO_NATIVE]" in line or "[SSAO v1]" in line:
            print(f"  RECEIPT {tag}: {line.strip()[:220]}", flush=True)
    print(f"[ssao-oracle] {tag}: {len(pngs)} PNGs pid={p.pid}", flush=True)

if __name__=="__main__":
    make_sacrificial_book()
    which = sys.argv[1:] or ["gl","gl2","vk_base","vk_ssao"]
    cfg = {"gl":{}, "gl2":{}, "gl_island":GL_ISLAND,
           "vk_base":VK_BASE, "vk_base2":VK_BASE,
           "vk_ssao":VK_SSAO, "vk_ssao2":VK_SSAO}
    for tag in which:
        run(tag, cfg[tag])
    print("[ssao-oracle] done")
