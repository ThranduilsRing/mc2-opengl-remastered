#!/usr/bin/env python3
"""check_ubo_layout.py — VK-OFFLINE-PROOFS-1 UBO LAYOUT LOCK.

Cross-checks the C++ island UBO mirror structs (the registry rows below) against the
GLSL std140 uniform block each shader declares, by:
  1. Computing std140 offsets from the GLSL block field list (independent
     recomputation, not trusting the shader's own `// off N` comments).
  2. Parsing the `// off N` comments the C++ struct ALREADY carries at every
     field (LOCKSTEP convention already used throughout this codebase).
  3. Diffing computed-GLSL-offset vs stated-C++-offset per matching field name.

This is intentionally "parse-lite": it does not build a full GLSL AST. It
understands the small set of std140-relevant types used in these shaders
(float, int, uint, vec2/3/4, mat4, and arrays of those). Anything else is
reported as UNKNOWN-TYPE (treated as a failure) rather than silently skipped.

Usage:
    py -3 tools/check_ubo_layout.py
    py -3 tools/check_ubo_layout.py --pair terrain

Exit 0 = all pairs' offsets match. Exit 1 = at least one mismatch (printed).
Exit 2 = parse/registry error.
"""
import argparse
import re
import sys

# (label, cpp_file, cpp_struct_name, glsl_file, glsl_block_name)
REGISTRY = [
    # The terrain, color-prop, mech and legacy-shape frame blocks are not listed:
    # RenderCore/gpu_abi/Gpu*FrameUBO.h generates both the C++ offset asserts and the GLSL
    # block (tools/gpu_abi_gen.py --check).
    # Water island FS params (grew for the reflection port).
    ("waterfs", "GameOS/gameos/vulkan_water_island_internal.h", "WaterFsParams",
     "shaders/vulkan/water_geom.frag", "WaterFsParams"),
    # The native fog pair reuses the subgraph's
    # shaders verbatim; its UBO PODs must stay byte-identical to those blocks.
    ("postfog_edge", "GameOS/gameos/gos_postprocess.h", "EdgeFogParams",
     "shaders/include/fog_policy.hglsl", "EdgeFogParams"),
    ("postfog_oob", "GameOS/gameos/vulkan_post_fog_native.cpp", "FogOobParams",
     "shaders/vulkan/fog_oob.frag", "FogOobParams"),
    # Native cloud-shadow + shoreline UBOs.
    ("screenfx_cloud", "GameOS/gameos/vulkan_post_screenfx_native.cpp", "ScreenFxCloudParams",
     "shaders/vulkan/cloud_shadow.frag", "CloudFxParams"),
    ("screenfx_shore", "GameOS/gameos/vulkan_post_screenfx_native.cpp", "ScreenFxShoreParams",
     "shaders/vulkan/shoreline.frag", "ShorelineFxParams"),
    # UBO mirrors that previously had neither static_asserts
    # nor a registry row.
    ("watervs", "GameOS/gameos/vulkan_water_island_internal.h", "WaterVsParams",
     "shaders/vulkan/water_geom.vert", "WaterVsParams"),
    ("vfxvs", "GameOS/gameos/vulkan_vfx_island.cpp", "VfxVsParams",
     "shaders/vulkan/vfx_particle.vert", "VfxVsParams"),
    ("shadow_live", "GameOS/gameos/vulkan_shadow_island_live.cpp", "ShadowFrameUBO",
     "shaders/vulkan/shadow_prop.vert", "ShadowFrameUBO"),
]

STD140_BASE_ALIGN = {
    "float": 4, "int": 4, "int32_t": 4, "uint": 4, "uint32_t": 4, "bool": 4,
    "vec2": 8,
    "vec3": 16, "vec4": 16,
    "mat4": 16,
}
STD140_BASE_SIZE = {
    "float": 4, "int": 4, "int32_t": 4, "uint": 4, "uint32_t": 4, "bool": 4,
    "vec2": 8,
    "vec3": 12, "vec4": 16,
    "mat4": 64,  # 4 columns * vec4(16B each), column-aligned to 16
}
# std140 array/struct rule: base alignment rounds up to 16B multiple. Arrays of
# scalars/vec2/vec3 also get element stride rounded to 16.
GLSL_DECL_RE = re.compile(r"^(mat4|vec4|vec3|vec2|float|int|uint|bool)\s+(.+)$")
GLSL_DECLARATOR_RE = re.compile(r"^(\w+)(\[(\d+)\])?$")
CPP_FIELD_RE = re.compile(
    r"^\s*(float|int32_t|uint32_t|uint64_t)\s+(\w+)(\[(\d+)\])?(\[(\d+)\])?\s*;\s*(//.*)?$")
CPP_OFFSET_IN_COMMENT_RE = re.compile(r"off\s*(\d+)")


def round_up(x, align):
    return ((x + align - 1) // align) * align


def parse_glsl_block(path, block_name):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"uniform\s+" + re.escape(block_name) + r"\s*\{", text)
    if not m:
        return None
    start = m.end()
    depth = 1
    i = start
    while depth > 0 and i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    body = text[start:i - 1]

    # Statement-based, not line-based: a line may declare several members
    # (`float tintRockR; float tintRockG;`) or a comma list (`float a, b, c;`), and
    # std140 offsets advance for every one of them.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//.*", "", body)
    body = "\n".join(l for l in body.splitlines() if not l.lstrip().startswith("#"))
    fields = []
    offset = 0
    for stmt in body.split(";"):
        stmt = " ".join(stmt.split())
        if not stmt:
            continue
        mm = GLSL_DECL_RE.match(stmt)
        if not mm:
            raise ValueError(f"UNKNOWN-TYPE/unparsed std140 member {stmt!r} in {block_name} ({path})")
        typ, declarators = mm.groups()
        for decl in declarators.split(","):
            dm = GLSL_DECLARATOR_RE.match(decl.strip())
            if not dm:
                raise ValueError(f"unparsed declarator {decl.strip()!r} in {block_name} ({path})")
            name, _, arrcount = dm.groups()
            align = STD140_BASE_ALIGN[typ]
            size = STD140_BASE_SIZE[typ]
            if arrcount:
                # array element stride rounds up to 16B in std140
                align = 16
                size = round_up(size, 16) * int(arrcount)
            offset = round_up(offset, align)
            fields.append((name, offset))
            offset += size
    return fields


def parse_cpp_struct(path, struct_name):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"struct\s+(?:alignas\([^)]*\)\s+)?" + re.escape(struct_name) + r"\s*\{", text)
    if not m:
        return None
    start = m.end()
    depth = 1
    i = start
    while depth > 0 and i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    body = text[start:i - 1]

    fields = []
    for line in body.splitlines():
        line = line.strip()
        mm = CPP_FIELD_RE.match(line)
        if not mm:
            continue
        _typ, name = mm.group(1), mm.group(2)
        comment = mm.group(7) or ""
        offm = CPP_OFFSET_IN_COMMENT_RE.search(comment)
        if not offm:
            continue  # field has no "off N" in its trailing comment -- can't cross-check
        fields.append((name, int(offm.group(1))))
    return fields


def check_pair(label, cpp_file, cpp_struct, glsl_file, glsl_block):
    try:
        glsl_fields = parse_glsl_block(glsl_file, glsl_block)
    except ValueError as e:
        return 2, [f"[{label}] {e}"]
    cpp_fields = parse_cpp_struct(cpp_file, cpp_struct)
    if glsl_fields is None:
        return 2, [f"[{label}] GLSL block '{glsl_block}' not found in {glsl_file}"]
    if cpp_fields is None:
        return 2, [f"[{label}] C++ struct '{cpp_struct}' not found in {cpp_file}"]

    glsl_map = dict(glsl_fields)
    cpp_map = dict(cpp_fields)
    mismatches = []
    for name, cpp_off in cpp_map.items():
        if name not in glsl_map:
            continue  # C++-only padding field with no GLSL counterpart; not an error
        glsl_off = glsl_map[name]
        if glsl_off != cpp_off:
            mismatches.append(f"  {name}: GLSL(computed)={glsl_off} C++(stated)={cpp_off}")
    if mismatches:
        return 1, [f"[{label}] OFFSET MISMATCH:"] + mismatches
    return 0, [f"[{label}] OK ({len(cpp_map)} cross-checked fields, "
               f"{cpp_file}::{cpp_struct} vs {glsl_file}::{glsl_block})"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pair", help="restrict to one registry label (e.g. terrain)")
    args = ap.parse_args()

    entries = [e for e in REGISTRY if not args.pair or e[0] == args.pair]
    if not entries:
        print(f"no registry entry named {args.pair!r}", file=sys.stderr)
        sys.exit(2)

    worst = 0
    for label, cpp_file, cpp_struct, glsl_file, glsl_block in entries:
        code, lines = check_pair(label, cpp_file, cpp_struct, glsl_file, glsl_block)
        for l in lines:
            print(l)
        worst = max(worst, code)
    sys.exit(worst)


if __name__ == "__main__":
    main()
