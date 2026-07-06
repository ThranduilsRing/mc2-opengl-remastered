#!/usr/bin/env python3
"""VK-FLIP-FIELD-REPORT-1 prop green-glow / imported-washout oracle.

Reuses terrain_near_oracle primitives but points the bookmark sweep at
mc2_01_buildings.json (low poses framing base buildings close-up). Two modes:

  footprint  GL vs VK-all restricted to the STATIC-PROP footprint
             (VK_all vs VK_all-with-prop-islands-OFF) -> the prop parity number,
             isolated from terrain residual. This is the number of record.
  whole      GL vs VK-all whole-frame (context).

Run BEFORE and AFTER the fix; compare footprint mean/ch + max.
"""
import os, sys, subprocess, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import terrain_near_oracle as T
import numpy as np

# point the sweep at the buildings bookmark (close-up base structures)
T.BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks/mc2_01_buildings.json")
T.BASE["MC2_VISUAL_BOOKMARK_CAPTURE"] = str(T.BOOK)
T.BASE["MC2_VISUAL_SETTLE"] = "80"

PROP_GATES = ["MC2_VK_PROP_COLOR_INTEROP", "MC2_VK_PROP_COLOR_TIER1", "MC2_VK_PROP_MATERIAL"]
VK_ALL = dict(T.VK_TERR,
              MC2_VK_PROP_COLOR_INTEROP="1", MC2_VK_PROP_COLOR_TIER1="1",
              MC2_VK_PROP_MATERIAL="1",
              MC2_VK_MECH_TIER1="1", MC2_VK_MECH_MATERIAL="1", MC2_VK_MECH_LIGHTING="1")
GL_ALL = dict(T.COMMON_VK)

def footprint():
    gl,_  = T.run("bldg_gl",  GL_ALL)
    vka,_ = T.run("bldg_vka", VK_ALL)
    vkt_gates = {k:v for k,v in VK_ALL.items() if k not in PROP_GATES}
    vkt,_ = T.run("bldg_vkt", vkt_gates)   # props OFF
    names = sorted(set(p.name for p in gl.glob(f"{T.MISSION}_*.png"))
                   & set(p.name for p in vka.glob(f"{T.MISSION}_*.png"))
                   & set(p.name for p in vkt.glob(f"{T.MISSION}_*.png")))
    print("\n== PROP footprint (|VK_all - VK_propsOFF|>8 ; report |GL - VK_all| there) ==")
    for n in names:
        g  = T.load(gl/n); va = T.load(vka/n); vt = T.load(vkt/n)
        fp = (np.abs(va-vt).max(axis=2) > 8)
        nfp = int(fp.sum())
        if nfp == 0:
            print(f"  {n}: footprint EMPTY"); continue
        gd = np.abs(g-va)[fp]
        print(f"  {n}: footprint_px={nfp:>6} mean/ch={gd.mean():6.3f} "
              f"max={int(gd.max()):3d} p99/ch={np.percentile(gd,99):5.1f}")

def whole():
    gl,_  = T.run("bldg_gl",  GL_ALL)
    vka,_ = T.run("bldg_vka", VK_ALL)
    names = sorted(set(p.name for p in gl.glob(f"{T.MISSION}_*.png"))
                   & set(p.name for p in vka.glob(f"{T.MISSION}_*.png")))
    print("\n== whole-frame GL vs VK-all ==")
    for n in names:
        r = T.diff(gl/n, vka/n, thr=8, mask_obj=False)
        print(f"  {n}: cov={r['cov_pct']:6.3f}% mean_all={r['mean_all']:6.4f} "
              f"max={r['max_all']:3d} mean_cov={r['mean_cov']:6.3f}")

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "footprint"
    T.OUT = Path(r"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/prop_buildings")
    T.OUT.mkdir(parents=True, exist_ok=True)
    (footprint if mode == "footprint" else whole)()
