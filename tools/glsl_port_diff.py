#!/usr/bin/env python3
"""glsl_port_diff.py — VK-OFFLINE-PROOFS-1 VERBATIM-PORT PROVER.

Extracts a named //PORT-BEGIN:<name> / //PORT-END:<name> block from a GL
shader and its VK counterpart, normalizes whitespace/comments, applies the
port's rename map, and diffs.

Exit 0 = every port is verbatim (post-rename) or is PINNED DEBT (see below).
Exit 1 = a real divergence, a stale allowlist entry, or pinned debt that changed.
Exit 2 = usage / markers-not-found / bad allowlist.

This does NOT silently "fix" shaders — a divergence is a FINDING to report,
not something this tool patches.

RENAME MAP (per port)
---------------------
The VK shaders are not copies of the GL ones at the identifier level: the VK
terrain shaders prefix helpers `mv_`, read uniforms through the std140 block
instance (`uT.<field>`, GL `u_<field>`), and the water FS reads packed `F.fN`
vec4 slots where GL has named uniforms. Those differences are by design, so each
port lists the rewrites that map the VK spelling back onto the GL one before
comparing (`rename`, applied per VK statement). Scaffolding that exists on only one
side (a VK function wrapper around a GL inline block; a local the VK caller passes
in as a parameter; GL-only compat overloads) is removed with `vk_drop` / `gl_drop`
(per statement), `gl_rename` (GL-side spelling) and `gl_sub` (regexes deleted from the
newline-joined GL statements, for multi-statement scaffolding). A rename or drop
must never hide arithmetic: if the math differs it has to show up as a divergence.

PINNED DEBT (tools/glsl_port_diff.allowlist)
--------------------------------------------
Some divergences are REAL VK gaps that have not been closed yet. They are recorded
in the allowlist with the hash of the exact post-rename diff and a one-line WHY, so
the suite stays green while the gap is owned. The allowlist cannot rot:
  * an entry whose port now matches verbatim FAILS (stale: delete the entry);
  * an entry whose diff changed FAILS (the debt moved: review, re-pin);
  * a divergence with no entry FAILS.

Markers are placed by hand at the ported blocks (see the shader files).

Usage:
    py -3 tools/glsl_port_diff.py --name calc_light \\
        --gl shaders/include/lighting.hglsl --vk shaders/vulkan/mech_tier1.vert

    py -3 tools/glsl_port_diff.py --all                  # runs the registry below
    py -3 tools/glsl_port_diff.py --all --print-hashes   # for (re-)pinning debt
"""
import argparse
import difflib
import hashlib
import os
import re
import sys

ALLOWLIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "glsl_port_diff.allowlist")

# Rename rules are (regex, replacement) pairs applied to each normalized statement.
MV_PREFIX = (r"\bmv_(\w+)", r"\1")       # terrain_tier1.frag helper prefix
UT_TO_U = (r"\buT\.(\w+)", r"u_\1")      # std140 block instance field -> GL uniform


class Port:
    def __init__(self, name, gl, vk, rename=(), vk_drop=(), gl_drop=(), gl_rename=(), gl_sub=()):
        self.name, self.gl, self.vk = name, gl, vk
        self.rename, self.vk_drop, self.gl_drop = list(rename), list(vk_drop), list(gl_drop)
        self.gl_rename, self.gl_sub = list(gl_rename), list(gl_sub)


REGISTRY = [
    # Real VK gap pinned in the allowlist: GL calc_light has the sunAxisMc2Fix 5th
    # parameter (MC2_SUN_AXIS_FIX); the VK re-port has none. The rename map folds VK's
    # short locals onto GL's names and removes GL-only scaffolding (`in` qualifiers, the
    # empty TERRAIN branch, the 4-arg compat overload, the light_dir_v temp VK inlines)
    # so the residual is the real gap, not noise.
    Port("calc_light", "shaders/include/lighting.hglsl", "shaders/vulkan/mech_tier1.vert",
         rename=[(r"\buLights\.light\b", "light"),
                 (r"\bidx\b", "lights_index"), (r"\bwpos\b", "vertex_world_pos"),
                 (r"\blt\b", "light_type"), (r"\blc\b", "lcolor"), (r"\bndl\b", "n_dot_l"),
                 (r"\blp\b", "light_pos"), (r"\btl\b", "to_light"),
                 (r"\bd\b", "dist"), (r"\bf\b", "falloff")],
         gl_rename=[(r"\bin (int|vec3)\b", r"\1"),
                    (r"\blight_dir_v\b", "to_light / dist")],
         gl_drop=[r"^vec3 light_dir_v = to_light / dist;$"],
         # mech.vert defines MC2_STATIC_PROP_LIGHTING, so GL's zero-light early-out for the
         # mech variant is `return base_light`, which is what VK's one-liner does.
         vk_drop=[r"^if \(ld\.numLights\.x == 0\) return base_light;$"],
         gl_sub=[r"\nif \(0 == ld\.numLights\.x\)\n#ifdef MC2_STATIC_PROP_LIGHTING\nreturn base_light;"
                 r"\n#else\nreturn vec3\(1, 1, 1\);\n#endif",
                 r"\nelse if \(light_type == TG_LIGHT_TERRAIN\) \{\n\}",
                 r"\nvec3 calc_light\(int lights_index, vec3 normal, vec3 vertex_world_pos, "
                 r"vec3 base_light\) \{\nreturn calc_light\([^\n]*\n\}$"]),
    # VK wraps GL's inline block in mv_computeLodNear(); the rename map folds the
    # wrapper, the early-out gate and the camera/world spellings back to GL's.
    Port("lodNear", "shaders/gos_terrain.frag", "shaders/vulkan/terrain_tier1.frag",
         rename=[(r"\buT\.cameraPosMC2\b", "cameraPos"),
                 (r"\bworldXY\b", "WorldPos.xy"),
                 (r"\bworldZ\b", "WorldPos.z"),
                 (r"\bLODFADE_NEAR_FADE\b", "LOD_NEAR_FADE"),
                 (r"\bLODFADE_NEAR\b", "LOD_NEAR"),
                 (r"^return (1\.0 - smoothstep)", r"float lodNear = \1")],
         vk_drop=[r"^float mv_computeLodNear\(",
                  r"^if \(uT\.lodFadeEnabled == 0\) return 1\.0;",   # gate-off early-out
                  r"^\}$"]),
    Port("skirtVertMath", "shaders/terrain_lod_chunk.vert", "shaders/vulkan/terrain_tier1_skirt.vert",
         rename=[UT_TO_U]),
    # The VK caller passes sunAltSin in (clamp(shSun.z,0,1)); GL derives it inline.
    Port("calcDynamicShadowCsm", "shaders/include/shadow.hglsl", "shaders/vulkan/include/terrain_frame_vk.glsl",
         rename=[MV_PREFIX],
         gl_drop=[r"^float sunAltSin = clamp\(lightDir\.z, 0\.0, 1\.0\);"]),
    # visualDisplaceFar is NOT registered: it has VK markers (terrain_tier1.vert,
    # terrain_tier1_skirt.vert; the VK block spans an else-branch boundary) but no GL
    # markers in shaders/terrain_lod_chunk.vert, so there is nothing to diff. Re-add
    # both rows when the GL block is marked.
    # Armed-water base-layer color math (waveLOD/fBm/
    # absorption/shore/light/crest/glint), GL MDI water FS vs the VK water FS.
    # Real VK gap pinned in the allowlist: GL's shore-meander block is absent in VK.
    Port("waterBaseColor", "shaders/gos_terrain_water_mdi.frag", "shaders/vulkan/water_geom.frag",
         rename=[(r"int\(F\.f4\.x \+ 0\.5\)", "u_shoreMode"),
                 (r"int\(F\.f4\.y \+ 0\.5\)", "u_hasShoreHeight"),
                 (r"int\(F\.f4\.z \+ 0\.5\)", "u_hasShoreMask"),
                 (r"\bF\.f4\.w\b", "u_shoreFeatherWu"),
                 (r"\bF\.f5\b", "u_shoreHeightBounds"),
                 (r"\bF\.f6\b", "u_shoreMaskBounds"),
                 (r"\bF\.f7\.x\b", "waterElevation")]),
    # Water reflection block (SH-L2 coeffs + HDRI equirect eval + SH eval
    # + wave-normal/reflect setup + Fresnel mix). Terrain-RT contact sub-block NOT
    # ported (MC2_WATER_REFLECTION_RT default-OFF -> GL no-op in the field config).
    Port("waterSkyShCoeffs", "shaders/gos_terrain_water_mdi.frag", "shaders/vulkan/water_geom.frag"),
    Port("waterEvalHdri", "shaders/gos_terrain_water_mdi.frag", "shaders/vulkan/water_geom.frag"),
    Port("waterEvalSkySh", "shaders/gos_terrain_water_mdi.frag", "shaders/vulkan/water_geom.frag"),
    Port("waterReflSetup", "shaders/gos_terrain_water_mdi.frag", "shaders/vulkan/water_geom.frag",
         rename=[(r"\bF\.f9\.w\b", "u_waterProjAspect"),
                 (r"\bF\.f9\.xyz\b", "u_waterSkyTintColor")]),
    Port("waterReflMix", "shaders/gos_terrain_water_mdi.frag", "shaders/vulkan/water_geom.frag"),
    # Cloud-shadow pass (inlined simplex fbm
    # + world-reconstruct main) and shoreline-foam pass (noise.hglsl fbm the GL
    # frag #includes + main). UBO plumbing differs by design (#define aliases);
    # the marked math blocks must stay verbatim.
    Port("cloudShadowNoise", "shaders/cloud.frag", "shaders/vulkan/cloud_shadow.frag"),
    Port("cloudShadowMain", "shaders/cloud.frag", "shaders/vulkan/cloud_shadow.frag"),
    Port("shorelineNoise", "shaders/include/noise.hglsl", "shaders/vulkan/shoreline.frag"),
    Port("shorelineMain", "shaders/shoreline.frag", "shaders/vulkan/shoreline.frag"),
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
    """Strip comments, collapse whitespace, and re-split into statements.

    Statement granularity (split after ; { }) rather than source lines, so a call
    wrapped differently on the GL and VK side is not reported as a divergence.
    Preprocessor lines stay standalone.
    """
    out, buf = [], []

    def flush():
        text = " ".join(buf).strip()
        buf.clear()
        if text:
            out.extend(s for s in re.split(r"(?<=[;{}])\s+", text) if s)

    for line in lines:
        line = re.sub(r"//.*$", "", line)
        line = re.sub(r"\s+", " ", line.strip())
        if not line:
            continue
        if line.startswith("#"):
            flush()
            out.append(line)
        else:
            buf.append(line)
    flush()
    return out


def _rename_all(stmts, rules):
    out = []
    for s in stmts:
        for pat, rep in rules:
            s = re.sub(pat, rep, s)
        out.append(s)
    return out


def apply_port_rules(port, gl_n, vk_n):
    gl_n = _rename_all([s for s in gl_n if not any(re.search(p, s) for p in port.gl_drop)],
                       port.gl_rename)
    vk_n = [s for s in vk_n if not any(re.search(p, s) for p in port.vk_drop)]
    vk_n = _rename_all(vk_n, port.rename)
    if port.gl_sub:
        text = "\n".join(gl_n)
        for pat in port.gl_sub:
            text = re.sub(pat, "", text)
        gl_n = [l for l in text.split("\n") if l]
    return gl_n, vk_n


def diff_hash(diff_lines):
    body = "\n".join(l for l in diff_lines if l[:1] in "+-" and not l.startswith(("---", "+++")))
    return hashlib.sha1(body.encode("utf-8")).hexdigest()[:12]


def diff_block(port):
    """Returns (code, msg, hash_or_None). code: 0 verbatim, 1 diverges, 2 markers missing."""
    gl_raw = extract_block(port.gl, port.name)
    vk_raw = extract_block(port.vk, port.name)
    if gl_raw is None:
        return 2, f"[{port.name}] markers not found in GL file {port.gl}", None
    if vk_raw is None:
        return 2, f"[{port.name}] markers not found in VK file {port.vk}", None
    gl_n, vk_n = apply_port_rules(port, normalize(gl_raw), normalize(vk_raw))
    if gl_n == vk_n:
        return 0, f"[{port.name}] VERBATIM ({port.gl} == {port.vk}, {len(gl_n)} normalized statements)", None
    diff = list(difflib.unified_diff(gl_n, vk_n, fromfile=f"GL:{port.name}", tofile=f"VK:{port.name}",
                                     lineterm="", n=1))
    msg = f"[{port.name}] DIVERGES ({port.gl} vs {port.vk}):\n" + "\n".join(diff)
    return 1, msg, diff_hash(diff)


def load_allowlist(path):
    """name<TAB>vk_path<TAB>diff_hash<TAB>WHY  (blank lines and # comments ignored)."""
    entries = {}
    if not os.path.exists(path):
        return entries
    with open(path, "r", encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = [p.strip() for p in line.split("\t")]
            if len(parts) != 4 or not all(parts):
                raise ValueError(f"{path}:{n}: want name<TAB>vk_path<TAB>diff_hash<TAB>WHY")
            entries[(parts[0], parts[1])] = (parts[2], parts[3], n)
    return entries


def run_registry(print_hashes):
    try:
        allow = load_allowlist(ALLOWLIST)
    except ValueError as e:
        print(f"bad allowlist: {e}", file=sys.stderr)
        return 2
    keys = {(p.name, p.vk) for p in REGISTRY}
    worst = 0
    for key, (_, _, n) in allow.items():
        if key not in keys:
            print(f"[{key[0]}] allowlist line {n} names a port not in the registry ({key[1]})")
            worst = 2
    for port in REGISTRY:
        code, msg, h = diff_block(port)
        key = (port.name, port.vk)
        if code == 1 and key in allow:
            want, why, _ = allow[key]
            if h == want and not print_hashes:
                print(f"[{port.name}] PINNED-DEBT ({port.vk}): {why}")
                continue
            print(msg)
            print(f"[{port.name}] hash={h}")
            if h != want:
                print(f"[{port.name}] DEBT-CHANGED: pinned hash {want}, now {h}. The known gap moved; "
                      f"review the diff above, then re-pin in tools/glsl_port_diff.allowlist")
                worst = max(worst, 1)
            continue
        if code == 0 and key in allow:
            print(msg)
            print(f"[{port.name}] STALE-ALLOWLIST: now verbatim; delete its line from "
                  f"tools/glsl_port_diff.allowlist")
            worst = max(worst, 1)
            continue
        print(msg)
        if code == 1:
            print(f"[{port.name}] hash={h}  (unpinned divergence: fix the port, or pin it as debt)")
        worst = max(worst, code)
    return worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name")
    ap.add_argument("--gl")
    ap.add_argument("--vk")
    ap.add_argument("--all", action="store_true",
                    help="run the built-in registry of known ports (rename maps + pinned-debt allowlist)")
    ap.add_argument("--print-hashes", action="store_true",
                    help="with --all: print every pinned divergence's diff and hash (for re-pinning)")
    args = ap.parse_args()

    if args.all:
        sys.exit(run_registry(args.print_hashes))
    if not args.name or not args.gl or not args.vk:
        print("usage: --name NAME --gl PATH --vk PATH   (or --all)", file=sys.stderr)
        sys.exit(2)
    code, msg, _ = diff_block(Port(args.name, args.gl, args.vk))
    print(msg)
    sys.exit(code)


if __name__ == "__main__":
    main()
