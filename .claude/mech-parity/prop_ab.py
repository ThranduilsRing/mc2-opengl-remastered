#!/usr/bin/env python3
"""BEFORE(orig spv)/AFTER(fixed spv) A/B for the prop fix on the same pose.
Swaps the deployed color_prop_tier1.vert.spv between two VK-props-only captures,
then diffs each against GL. Isolates the shader fix's effect (same pose, mechs OFF)."""
import os, sys, shutil, subprocess, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import terrain_near_oracle as T
import numpy as np

T.BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks/mc2_01_buildings.json")
T.BASE["MC2_VISUAL_BOOKMARK_CAPTURE"] = str(T.BOOK)
T.BASE["MC2_VISUAL_SETTLE"] = "80"
T.OUT = Path(r"A:/Games/mc2-vk-interop-bridge/.claude/mech-parity/prop_ab")
T.OUT.mkdir(parents=True, exist_ok=True)

DEP = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/shaders/vulkan/color_prop_tier1.vert.spv")
ORIG = Path(r"C:/Users/Joe/AppData/Local/Temp/orig_prop.vert.spv")
AFTER = Path(r"C:/Users/Joe/AppData/Local/Temp/after_prop.vert.spv")
VK_PROPS = dict(T.VK_TERR, MC2_VK_PROP_COLOR_INTEROP="1", MC2_VK_PROP_COLOR_TIER1="1", MC2_VK_PROP_MATERIAL="1")
GL = dict(T.COMMON_VK)

def cap(tag, gates): return T.run(tag, gates)[0]

def report(label, gld, vkd):
    names = sorted(set(p.name for p in gld.glob(f"{T.MISSION}_*.png")) & set(p.name for p in vkd.glob(f"{T.MISSION}_*.png")))
    print(f"\n== {label} (GL vs VK-props, whole-frame) ==")
    for n in names:
        r = T.diff(gld/n, vkd/n, thr=8, mask_obj=False)
        print(f"  {n}: cov={r['cov_pct']:6.3f}% mean_all={r['mean_all']:6.3f} max={r['max_all']:3d}")

gld = cap("ab_gl", GL)
shutil.copy(ORIG, DEP);  vkb = cap("ab_vk_before", VK_PROPS)
shutil.copy(AFTER, DEP); vka = cap("ab_vk_after",  VK_PROPS)
report("BEFORE (raw-byte seed + flat ambient)", gld, vkb)
report("AFTER  (magic-tag resolve + hemi ambient + alpha floor)", gld, vka)
