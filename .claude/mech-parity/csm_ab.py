#!/usr/bin/env python3
"""VK-FLIP-FIELD-REPORT-1 Issue-2 CSM subset A/B.
GL (real CSM object shadow on near terrain) vs VK-terrain CSM subset OFF vs ON.
Umbrella + CSM launcher-default (MC2_SHADOW_CSM defaults on). Measures whether the
full-map cascade subset actually darkens VK terrain under a building/mech shadow to
approach GL, and that CSM-OFF leaves VK terrain unshadowed (the gap this closes)."""
import os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import terrain_near_oracle as T
import numpy as np

T.BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks/mc2_01_buildings.json")
T.BASE["MC2_VISUAL_BOOKMARK_CAPTURE"] = str(T.BOOK)
T.BASE["MC2_VISUAL_SETTLE"] = "60"
# CSM is a launcher default; make sure it's ON for both GL and VK so GL casts CSM shadow.
T.BASE["MC2_SHADOW_CSM"] = "1"
T.OUT = Path(r"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/csm_ab")
T.OUT.mkdir(parents=True, exist_ok=True)

# VK terrain-only (props/mech OFF so we measure the terrain shadow, not props).
VK_TERR_DYN = dict(T.VK_TERR, MC2_VK_TERRAIN_DYNSHADOW="1")
GL = dict(T.COMMON_VK)

gl,_   = T.run("csm_gl",     GL)
off,_  = T.run("csm_vk_off", VK_TERR_DYN)                       # CSM subset OFF -> no dyn shadow (CSM active)
on,_   = T.run("csm_vk_on",  dict(VK_TERR_DYN, MC2_VK_TERRAIN_CSM="1"))  # CSM subset ON -> full-map shadow

def stats(a,b,label):
    names = sorted(set(p.name for p in a.glob(f"{T.MISSION}_*.png")) & set(p.name for p in b.glob(f"{T.MISSION}_*.png")))
    print(f"\n== {label} ==")
    for n in names:
        r=T.diff(a/n,b/n,thr=8,mask_obj=False)
        print(f"  {n}: mean_all={r['mean_all']:6.3f} cov={r['cov_pct']:6.2f}% max={r['max_all']:3d}")

stats(gl, off, "GL vs VK CSM-OFF (gap: VK terrain unshadowed)")
stats(gl, on,  "GL vs VK CSM-ON  (subset: full-map shadow should CLOSE the gap)")
stats(off, on, "VK OFF vs VK ON  (the shadow the subset adds -> non-zero = it works)")
