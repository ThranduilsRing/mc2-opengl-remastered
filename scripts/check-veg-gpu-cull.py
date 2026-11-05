#!/usr/bin/env python3
"""check-veg-gpu-cull.py -- the vegetation cull stays on the GPU, and its C++/GLSL parameter block stays in lockstep.

Rows:
  1. gos_vegetation.cpp has no per-instance CPU walk (no 64 B vertex build, no instance loop, no CPU copy of the population) and requests the GPU run.
  2. mc2veg::CullParams (C++) and VegParams / VegDrawParams (GLSL) have the same member list and size.
  3. The compute stages and the vegetation vertex shader are baked (listed in the gameos shader basenames).
Exit 0 = clean.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
errors = []


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


veg = read("GameOS/gameos/gos_vegetation.cpp")
for needle in ("VfxGeometryVertex", "for (size_t index", "instances[", "std::vector<GosVegetation::Instance> instances"):
    if needle in veg:
        errors.append(f"gos_vegetation.cpp: CPU per-instance path is back ({needle!r})")
if "mc2_vk_vfx_vegetation_run" not in veg:
    errors.append("gos_vegetation.cpp: no mc2_vk_vfx_vegetation_run call")

# 2. parameter block lockstep
hdr = read("GameOS/gameos/vk_vegetation_state.h")
m = re.search(r"struct CullParams \{(.*?)\};", hdr, re.S)
cpp_members = re.findall(r"(float|uint32_t)\s+(\w+)\[(\d)\]", m.group(1)) if m else []
cpp_names = [n for _, n, _ in cpp_members]
if cpp_names != ["plane", "planeLen", "clipRow", "cam", "misc", "grid", "counts", "flags"]:
    errors.append(f"CullParams member list changed: {cpp_names}")
if "sizeof(CullParams) == 176" not in hdr:
    errors.append("CullParams size assert (176) missing")


def glsl_members(text, struct):
    mm = re.search(r"struct\s+" + struct + r"\s*\{(.*?)\}", text, re.S)
    if not mm:
        return None
    return re.findall(r"(?:u?vec4)\s+(\w+)(?:\[\d\])?\s*;", mm.group(1))


got = glsl_members(read("shaders/vulkan/include/vegetation_structs.glsl"), "VegParamsData")
if got != ["plane", "planeLen", "clipRow", "cam", "misc", "grid", "counts", "flags"]:
    errors.append(f"vegetation_structs.glsl: VegParamsData members {got} != CullParams")
for rel in ("shaders/vulkan/include/vegetation_cull_common.glsl", "shaders/vulkan/vfx_vegetation.vert"):
    if "vegetation_structs.glsl" not in read(rel):
        errors.append(f"{rel}: does not include the shared vegetation_structs.glsl")

# 3. baked
cm = read("GameOS/gameos/CMakeLists.txt")
for base in ("vegetation_region", "vegetation_cull", "vegetation_compact", "vfx_vegetation"):
    if base not in cm:
        errors.append(f"GameOS/gameos/CMakeLists.txt: shader {base} is not baked")
    if not any((ROOT / "shaders/vulkan").glob(base + ".*")):
        errors.append(f"shaders/vulkan/{base}.* missing")

if errors:
    print("check-veg-gpu-cull: FAIL")
    for e in errors:
        print("  - " + e)
    sys.exit(1)
print("check-veg-gpu-cull: OK")
