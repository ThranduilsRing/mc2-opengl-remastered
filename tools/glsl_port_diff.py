#!/usr/bin/env python3
"""glsl_port_diff.py — VK-OFFLINE-PROOFS-1 VERBATIM-PORT PROVER.

Extracts a named //PORT-BEGIN:<name> / //PORT-END:<name> block from a GL
shader and its VK counterpart, normalizes whitespace/comments, and diffs.
Exit 0 = verbatim (post-normalization). Exit 1 = real divergence (printed).
Exit 2 = usage/markers-not-found error.

This does NOT silently "fix" shaders — a divergence is a FINDING to report,
not something this tool patches.

Markers are placed by hand at the ported blocks (see the shader files):
  calc_light     : shaders/include/lighting.hglsl  vs shaders/vulkan/mech_tier1.vert
  lodNear        : shaders/gos_terrain.frag        vs shaders/vulkan/terrain_tier1.frag
  skirtVertMath  : shaders/terrain_lod_chunk.vert  vs shaders/vulkan/terrain_tier1_skirt.vert

Usage:
    py -3 tools/glsl_port_diff.py --name calc_light \
        --gl shaders/include/lighting.hglsl --vk shaders/vulkan/mech_tier1.vert

    py -3 tools/glsl_port_diff.py --all   # runs the registry below
"""
import argparse
import re
import sys

REGISTRY = [
    ("calc_light", "shaders/include/lighting.hglsl", "shaders/vulkan/mech_tier1.vert"),
    ("lodNear", "shaders/gos_terrain.frag", "shaders/vulkan/terrain_tier1.frag"),
    ("skirtVertMath", "shaders/terrain_lod_chunk.vert", "shaders/vulkan/terrain_tier1_skirt.vert"),
]


def extract_block(path, name):
    begin_re = re.compile(r"//\s*PORT-BEGIN:" + re.escape(name) + r"\b")
    end_re = re.compile(r"//\s*PORT-END:" + re.escape(name) + r"\b")
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    start = end = None
    for i, line in enumerate(lines):
        if start is None and begin_re.search(line):
            start = i + 1
        elif start is not None and end_re.search(line):
            end = i
            break
    if start is None or end is None:
        return None
    return lines[start:end]


def normalize(lines):
    out = []
    for line in lines:
        # strip line comments (// ...), block comments handled line-by-line (good
        # enough for these single-line-comment-heavy shader files)
        line = re.sub(r"//.*$", "", line)
        line = line.strip()
        if not line:
            continue
        # collapse internal whitespace so formatting-only diffs don't fire
        line = re.sub(r"\s+", " ", line)
        out.append(line)
    return out


def diff_block(name, gl_path, vk_path):
    gl_raw = extract_block(gl_path, name)
    vk_raw = extract_block(vk_path, name)
    if gl_raw is None:
        return 2, f"[{name}] markers not found in GL file {gl_path}"
    if vk_raw is None:
        return 2, f"[{name}] markers not found in VK file {vk_path}"
    gl_n = normalize(gl_raw)
    vk_n = normalize(vk_raw)
    if gl_n == vk_n:
        return 0, f"[{name}] VERBATIM ({gl_path} == {vk_path}, {len(gl_n)} normalized lines)"
    # Report a line-by-line diff for the divergence.
    import difflib
    diff = list(difflib.unified_diff(gl_n, vk_n, fromfile=f"GL:{name}", tofile=f"VK:{name}", lineterm=""))
    msg = f"[{name}] DIVERGES ({gl_path} vs {vk_path}):\n" + "\n".join(diff)
    return 1, msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name")
    ap.add_argument("--gl")
    ap.add_argument("--vk")
    ap.add_argument("--all", action="store_true", help="run the built-in registry of known ports")
    args = ap.parse_args()

    entries = REGISTRY if args.all else [(args.name, args.gl, args.vk)]
    if not args.all and (not args.name or not args.gl or not args.vk):
        print("usage: --name NAME --gl PATH --vk PATH   (or --all)", file=sys.stderr)
        sys.exit(2)

    worst = 0
    for name, gl_path, vk_path in entries:
        code, msg = diff_block(name, gl_path, vk_path)
        print(msg)
        worst = max(worst, code)
    sys.exit(worst)


if __name__ == "__main__":
    main()
