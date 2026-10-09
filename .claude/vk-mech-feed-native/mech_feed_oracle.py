#!/usr/bin/env python3
"""VK-MECH-FEED-NATIVE-1 (rung 7) oracle: mech-bookmark sweep on mc2_24 + mc2_01.

Configs:
  gl / gl2      — all VK gates unset (byte-off control, must be 0.000-class)
  vk_old        — umbrella VK config, feed-native OFF (GL-readback feed path)
  vk_native     — same + MC2_VK_MECH_FEED_NATIVE=1 (CPU-source feed path)
  vk_native_audit — native + MC2_VK_MECH_FEED_AUDIT=1 (byte-compare vs GL buffers)

Single-variable oracle: vk_old vs vk_native at identical deterministic poses must
be 0.000-class (the feed carries the SAME bytes, only the transport changed).
VK-FIRST-BOOKMARK-POSE-1 mitigation: sacrificial duplicate of bookmark #1 first.
"""
import os, sys, subprocess, time, json
from pathlib import Path

# OWNER RULE: the engine is launched through nofocus.spawn ONLY (background launch:
# non-activating, offscreen, no cursor grab). These oracles read engine PNGs.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from smoke_lib import nofocus  # noqa: E402

EXE  = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
HERE = Path(__file__).resolve().parent
SRC  = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks")

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
}
VK_NATIVE = dict(VK_BASE); VK_NATIVE["MC2_VK_MECH_FEED_NATIVE"] = "1"
VK_NATIVE_AUDIT = dict(VK_NATIVE); VK_NATIVE_AUDIT["MC2_VK_MECH_FEED_AUDIT"] = "1"

def make_sacrificial_book(mission):
    d = json.loads((SRC / f"{mission}.json").read_text(encoding="utf-8"))
    bms = d["bookmarks"]
    sac = json.loads(json.dumps(bms[0])); sac["name"] = "zzsacrificial"
    d["bookmarks"] = [sac] + bms
    out = HERE / f"{mission}_sacrificial.json"
    out.write_text(json.dumps(d), encoding="utf-8")
    return out

def run(tag, extra, mission, duration=70, trigger=140, settle=40, feed_dump=None):
    out_dir = HERE / "cap" / mission / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    book = make_sacrificial_book(mission)
    env = os.environ.copy()
    env.update({
        "MC2_SMOKE_MODE":"1","MC2_LOG":"1","MC2_SMOKE_SEED":"0xC0FFEE",
        "MC2_SMOKE_FIXED_TIMESTEP":"1","MC2_HEARTBEAT":"1",
        "MC2_DEBUG_STATE_DUMP":"1",
        "MC2_VISUAL_BOOKMARK_CAPTURE":str(book),
        "MC2_VISUAL_CAPTURE_DIR":str(out_dir),
        "MC2_VISUAL_CAPTURE_FRAME":str(trigger),
        "MC2_VISUAL_SETTLE":str(settle),
    })
    env.update(FIELD_GL_DEFAULTS)
    env.update(extra)
    if feed_dump:
        env["MC2_VK_FEED_DUMP"] = str(out_dir / feed_dump)
    args=[str(EXE),"--profile","stock","--mission",mission,"--duration",str(duration)]
    log=out_dir/f"{tag}.log"
    vkg={k:v for k,v in env.items() if k.startswith("MC2_VK") or k=="MC2_RENDER_BACKEND"}
    print(f"[mech-feed-oracle] {mission}/{tag} vkgates={vkg}", flush=True)
    with open(log,"w",encoding="utf-8",errors="replace") as lf:
        p=nofocus.spawn(args,stdout=lf,stderr=subprocess.STDOUT,cwd=str(EXE.parent),env=env,show_window=True,small_window=False)
        deadline=time.time()+duration+60
        while time.time()<deadline:
            if p.poll() is not None: break
            time.sleep(0.5)
        if p.poll() is None:
            subprocess.run(["taskkill","/PID",str(p.pid),"/T","/F"],capture_output=True, **nofocus.hidden_kwargs())
        try: p.wait(timeout=15)
        except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=10)
    pngs=sorted(out_dir.glob(f"{mission}_*.png"))
    txt = log.read_text(encoding="utf-8", errors="replace")
    for line in txt.splitlines():
        ls = line.strip()
        if ("[VK_MECH_ISLAND]" in ls) or ("vk_color_mech" in ls and "used_vulkan" in ls):
            print(f"  RECEIPT {tag}: {ls[:240]}", flush=True)
    print(f"[mech-feed-oracle] {mission}/{tag}: {len(pngs)} PNGs pid={p.pid}", flush=True)

if __name__=="__main__":
    which = sys.argv[1:]
    cfg = {"gl":{}, "gl2":{}, "vk_old":VK_BASE, "vk_native":VK_NATIVE,
           "vk_native_audit":VK_NATIVE_AUDIT}
    for spec in which:
        mission, tag = spec.split(":")
        fd = "feed.jsonl" if tag in ("vk_old","vk_native") else None
        run(tag, cfg[tag], mission, feed_dump=fd)
    print("[mech-feed-oracle] done")
