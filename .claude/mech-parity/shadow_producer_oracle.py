#!/usr/bin/env python3
"""VK-FULL-OWNERSHIP rung-2 oracle: VK-PRODUCED dynamic shadow vs GL-COPIED dynamic
shadow, both consumed by VK terrain, ground-truthed against GL.

Reuses the terrain near-oracle harness (deterministic FORCE_LOD=0 pose sweep,
mc2_01_terrain_near.json, near_land_low + near_land_off).

Three configs:
  GL       = pure GL (ground truth)
  VK_COPY  = VK terrain consumes GL's shadow map via the existing copy-interop
             (MC2_VK_TERRAIN_DYNSHADOW=1, the shipped 43e6bbc2 path)
  VK_PROD  = VK terrain consumes the VK-PRODUCED shadow map
             (MC2_VK_SHADOW_INTEROP=1 layered on top -- rung 2, this task)

Reports:
  (a) GL vs VK_COPY   -- existing baseline (should match doc's ~4.7-5.0 mean_all / ~57% cov)
  (b) GL vs VK_PROD   -- the new number: does VK-producing the shadow match GL at least
                         as well as the copy path?
  (c) VK_COPY vs VK_PROD -- isolates JUST the producer-vs-copy delta (shadow-only footprint)
  (d) gate-OFF byte control: MC2_VK_SHADOW_INTEROP=0 explicit vs unset -> must be 0.000
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import terrain_near_oracle as T
import numpy as np

GL_ALL      = dict(T.COMMON_VK)
VK_COPY     = dict(T.VK_TERR, MC2_VK_TERRAIN_DYNSHADOW="1")
VK_PROD     = dict(VK_COPY,   MC2_VK_SHADOW_INTEROP="1")
VK_PROD_OFF = dict(VK_COPY,   MC2_VK_SHADOW_INTEROP="0")  # explicit-off control

def pair(label, dA, dB, thr=8):
    pa = sorted(dA.glob(f"{T.MISSION}_*.png")); pb = sorted(dB.glob(f"{T.MISSION}_*.png"))
    names = sorted(set(x.name for x in pa) & set(x.name for x in pb))
    print(f"\n== {label} ==")
    for n in names:
        a = T.load(dA/n); b = T.load(dB/n)
        h = min(a.shape[0], b.shape[0]); w = min(a.shape[1], b.shape[1])
        a, b = a[:h,:w], b[:h,:w]
        d = np.abs(a - b)
        dm = d.max(axis=2)
        cov = dm > thr
        ncov = int(cov.sum())
        mean_all = float(d.mean())
        mean_cov = float(d[cov].mean()) if ncov else 0.0
        print(f"  {n}: cov={100.0*ncov/dm.size:6.3f}% ncov={ncov:>7} "
              f"mean_all={mean_all:6.4f} mean_cov={mean_cov:6.3f} max={int(dm.max()):3d}")

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    gl      = T.run("shd_gl",       GL_ALL)[0]
    vk_copy = T.run("shd_vk_copy",  VK_COPY)[0]
    vk_prod = T.run("shd_vk_prod",  VK_PROD)[0]
    pair("(a) GL vs VK_COPY (existing baseline)", gl, vk_copy)
    pair("(b) GL vs VK_PROD (rung-2: VK renders its own shadow)", gl, vk_prod)
    pair("(c) VK_COPY vs VK_PROD (producer-vs-copy delta)", vk_copy, vk_prod)
    if mode == "all":
        vk_off = T.run("shd_vk_prod_off", VK_PROD_OFF)[0]
        pair("(d) gate-OFF control: VK_COPY vs VK_PROD(explicit-off) -- MUST be 0.000", vk_copy, vk_off)
