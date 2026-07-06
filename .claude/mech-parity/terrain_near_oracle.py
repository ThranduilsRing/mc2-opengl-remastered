#!/usr/bin/env python3
"""VK-TERRAIN-NEAR-ORACLE-1 — deterministic near-camera terrain oracle.

The bookmark sweep is sim-frozen + hard-set-pose => byte-stable across runs.
FORCE_LOD=0 (applied to BOTH GL and VK, a LOD policy not a VK gate) forces every
drawn block to lodStep==1, so the VK terrain island co-writes a LARGE pixel region
(instead of ~0 at coarse-LOD bookmark cameras). This is the same-camera oracle that
was missing: near-detail terrain features (anti-tile / POM) ARE exercised AND the
capture is deterministic.

Acceptance it measures:
  (i)  same-config self-diff = 0.000 across repeated runs, GL-config AND VK-config
  (ii) VKon-vs-VKoff shows nonzero terrain coverage (proves VK LOD0 co-write)
  (iii) pose is scripted (mc2_01_terrain_near.json) + one-command re-runnable

Usage:
  terrain_near_oracle.py selfdiff        # (i)+(ii): GLx2, VKx2, coverage
  terrain_near_oracle.py <gate=1 ...>    # baseline for a feature: gl vs vk vs vk+gate
"""
import os, sys, subprocess, time
from pathlib import Path
from PIL import Image
import numpy as np

EXE  = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks/mc2_01_terrain_near.json")
OUT  = Path(r"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/terrain/near_oracle")
MISSION = "mc2_01"

# FORCE_LOD=0 => whole frame lodStep==1 => VK co-writes. Applied to GL and VK alike.
BASE = {
    "MC2_SMOKE_MODE":"1","MC2_LOG":"1","MC2_SMOKE_SEED":"0xC0FFEE",
    "MC2_SMOKE_FIXED_TIMESTEP":"1","MC2_HEARTBEAT":"1","MC2_NO_LAUNCHER":"1",
    "MC2_VISUAL_BOOKMARK_CAPTURE":str(BOOK),
    "MC2_VISUAL_CAPTURE_FRAME":"120",   # sweep-trigger frame
    "MC2_VISUAL_SETTLE":"30",
    "MC2_TERRAIN_LOD_CHUNK_FORCE_LOD":"0",
}
COMMON_VK = {"MC2_RENDER_BACKEND":"vulkan","MC2_VK_SHARED_SCENE_DEPTH":"1"}
VK_TERR = dict(COMMON_VK, MC2_VK_TERRAIN_TIER1="1", MC2_VK_TERRAIN_LIGHTING="1",
               MC2_VK_TERRAIN_DETAIL="1")

def run(tag, gates, duration=60):
    out_dir = OUT / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(BASE)
    env["MC2_VISUAL_CAPTURE_DIR"] = str(out_dir)
    for k,v in gates.items(): env[k]=v
    si=None
    if os.name=="nt":
        si=subprocess.STARTUPINFO(); si.dwFlags|=subprocess.STARTF_USESHOWWINDOW; si.wShowWindow=7
    args=[str(EXE),"--profile","stock","--mission",MISSION,"--duration",str(duration)]
    log=out_dir/f"{tag}.log"
    vkg={k:v for k,v in gates.items() if k.startswith('MC2_VK')}
    print(f"[near-oracle] run {tag} vk_gates={vkg}")
    with open(log,"w",encoding="utf-8",errors="replace") as lf:
        p=subprocess.Popen(args,stdout=lf,stderr=subprocess.STDOUT,cwd=str(EXE.parent),env=env,startupinfo=si)
        deadline=time.time()+duration+40
        while time.time()<deadline:
            if p.poll() is not None: break
            time.sleep(0.5)
        try: p.wait(timeout=15)
        except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=10)
    pngs=sorted(out_dir.glob(f"{MISSION}_*.png"))
    print(f"[near-oracle] {tag}: {len(pngs)} PNGs -> {[q.name for q in pngs]}")
    return out_dir, pngs

def load(p): return np.asarray(Image.open(p).convert("RGB"), dtype=np.int32)

# ANIMATED-OBJECT EXCLUSION: at the near land pose the werewolf mech/units sit
# center-frame. The sim-pause freezes scenarioTime but a unit's animation pose is
# NOT fully frozen at the pinned frame -> a small center island (~80x110 px) jitters
# run-to-run. That is a MECH artifact (mechs have their own parity oracle), NOT
# terrain. This box masks it so the terrain self-diff / co-write numbers are clean.
# Empirically: OUTSIDE this box the terrain is byte-identical across repeated runs.
# Measured union of run-to-run jitter across all 6 self-diff pairs at near_land_low
# (the werewolf mech + its shadow). Padded slightly. Terrain outside = byte-identical.
OBJ_BOX = (400, 100, 516, 200)   # x0,y0,x1,y1  (~116x100 px of 480000)

def diff(a_png, b_png, thr=8, mask_obj=True):
    a,b=load(a_png),load(b_png)
    h=min(a.shape[0],b.shape[0]); w=min(a.shape[1],b.shape[1])
    a,b=a[:h,:w],b[:h,:w]
    d=np.abs(a-b)
    if mask_obj:
        x0,y0,x1,y1=OBJ_BOX
        d[y0:y1, x0:x1, :] = 0   # exclude the animated-object island (mech, not terrain)
    dmax=d.max(axis=2); cov=dmax>thr
    ncov=int(cov.sum()); tot=d.shape[0]*d.shape[1]
    return dict(cov_pct=100.0*ncov/tot, ncov=ncov, tot=tot,
                mean_all=float(d.mean()), max_all=int(d.max()),
                mean_cov=float(d[cov].mean()) if ncov else 0.0)

def pair_report(label, dirA, dirB):
    pa=sorted(dirA.glob(f"{MISSION}_*.png")); pb=sorted(dirB.glob(f"{MISSION}_*.png"))
    print(f"\n== {label} ==")
    names=sorted(set(x.name for x in pa) & set(x.name for x in pb))
    agg=[]
    for n in names:
        r=diff(dirA/n, dirB/n)
        agg.append(r)
        print(f"  {n}: cov={r['cov_pct']:6.3f}% ncov={r['ncov']:>7} "
              f"mean_all={r['mean_all']:6.3f} max={r['max_all']:3d} mean_cov={r['mean_cov']:6.3f}")
    if agg:
        print(f"  AGGREGATE: mean cov={sum(a['cov_pct'] for a in agg)/len(agg):.3f}% "
              f"mean_all={sum(a['mean_all'] for a in agg)/len(agg):.4f}")
    return agg

if __name__=="__main__":
    mode = sys.argv[1] if len(sys.argv)>1 else "selfdiff"
    if mode=="selfdiff":
        gl=[run(f"gl_{i}", COMMON_VK)[0] for i in range(3)]
        vk=[run(f"vk_{i}", VK_TERR)[0] for i in range(3)]
        # 3-run self-diff triplet: compare run0 vs run1, run0 vs run2.
        pair_report("(i) GL self-diff r0-r1 -- MUST be 0.000", gl[0], gl[1])
        pair_report("(i) GL self-diff r0-r2 -- MUST be 0.000", gl[0], gl[2])
        pair_report("(i) VK self-diff r0-r1 -- MUST be 0.000", vk[0], vk[1])
        pair_report("(i) VK self-diff r0-r2 -- MUST be 0.000", vk[0], vk[2])
        pair_report("(ii) VKoff-vs-VKon coverage (gl0 vs vk0) -- MUST be nonzero", gl[0], vk[0])
    else:
        # feature-baseline mode: gate flags on argv
        extra={}
        for a in sys.argv[1:]:
            k,_,v=a.partition("="); extra[k]=v or "1"
        gl,_  = run("gl",  COMMON_VK)
        vk,_  = run("vk",  VK_TERR)
        vkf,_ = run("vk_feat", dict(VK_TERR, **extra))
        pair_report("GL vs VK (baseline, feature OFF)", gl, vk)
        pair_report("GL vs VK+feature", gl, vkf)
        pair_report("VK vs VK+feature (feature delta)", vk, vkf)
