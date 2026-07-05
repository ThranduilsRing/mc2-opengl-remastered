#!/usr/bin/env python3
"""check-sampler-bindings.py — SHADER-SAMPLER-BINDING-MANIFEST-1

CI/check-time occupancy + lockstep checker for GPU TEXTURE-UNIT (sampler)
bindings — the GL_TEXTURE_* namespace that check-binding-slots.py explicitly
leaves out. Like the buffer-slot checker, this is NOT a flat global registry:
the census proved sampler units are MULTIPLEXED per pass (unit 0 = colormap /
atlas / u_tex / sceneTex / uAtlas across ~15 programs; unit 5 = matNormalArray /
matNormal0 / mineSpriteArray). A unit number is only semantic INSIDE a program.
This script gives visibility instead of a false flat namespace, and catches the
one genuine lockstep surface samplers have.

The lockstep surface (samplers carry NO layout(binding=) in GLSL — only a
trailing comment): the C++ deferred-uniform wrapper Program::setInt("name", unit)
and GLProgram::setSamplerUnit("name", unit) name the sampler with a STRING
literal. That string is the join key to the GLSL `uniform sampler* name;` decl.
So we can check:
  - which program binds which sampler name to which unit (occupancy table)
  - does a sampler name's C++-assigned unit disagree with its own GLSL
    unit-comment? (named comment-vs-code drift)  -> FAIL
  - does one program receiver assign the same sampler name two different units?
    (intra-program contradiction)                -> FAIL
  - which samplers have NO discoverable C++ unit binder (dormant / via generic
    setSamplerUnit / dynamic helper)?            -> WARN (UNKNOWN)
  - cross-pass unit reuse + bare glUniform1i(loc, N) literals + duplicated unit
    constants                                    -> WARN (expected / hand-lockstep)

Exit code: 0 unless a FAIL is found. WARN never fails the build. Intentional
mode-alternate pairs (dynamicShadowArray / dynamicShadowMap share a unit on
mutually-exclusive CSM-vs-single-map branches) are DOCUMENTED, not failed.

Note: literal glUniform1i(loc, N) sites bind via a location-cache variable
(s_locColormap, tl.tex1, ...), so the sampler NAME is not statically recoverable
from the call alone; those are counted, not name-resolved. The setInt/
setSamplerUnit string-literal path is the name-resolvable surface.

Usage:
  py -3 scripts/check-sampler-bindings.py [--root <repo>] [--json <out.json>] [--quiet]
"""
import argparse
import json
import os
import re
import sys

CPP_DIRS = ["GameOS", "RenderCore", "RenderWorld", "GameAdapters", "mclib", "code"]
GLSL_DIR = "shaders"
EXCLUDE_SUBSTR = [
    os.path.join("tools", ""),
    os.path.join("shaders", "fixtures", ""),
    os.path.join("build64", ""),
    os.path.join("3rdparty", ""),
    os.path.join(".claude", ""),
]
CPP_EXT = (".h", ".hpp", ".cpp", ".cc", ".cxx")
GLSL_EXT = (".vert", ".frag", ".comp", ".tesc", ".tese", ".geom", ".glsl", ".hglsl")

# GLSL: uniform [layout(...)] samplerTYPE name;  (TYPE = 2D, 2DArray, Cube,
# 2DShadow, 2DArrayShadow, 3D, ...). layout(binding=) is essentially never
# present for samplers in this codebase, but tolerate it.
RE_GLSL_SAMPLER = re.compile(
    r"\buniform\s+(?:layout\s*\([^)]*\)\s*)?"
    r"(sampler[0-9A-Za-z]*)\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;"
)
# trailing-comment unit hint: "// ... unit 5", "// bound at unit 5", "= 5"
RE_COMMENT_UNIT = re.compile(r"unit\s*(\d+)", re.IGNORECASE)

# C++: [recv->|recv.]setInt("name", N)   and   [recv->]setSamplerUnit("name", N)
RE_CPP_SETINT = re.compile(
    r"(?:([A-Za-z_]\w*)\s*(?:->|\.))?\s*setInt\s*\(\s*\"([A-Za-z_]\w*)\"\s*,\s*(\d+)"
)
RE_CPP_SETSAMPLER = re.compile(
    r"(?:([A-Za-z_]\w*)\s*(?:->|\.))?\s*setSamplerUnit\s*\(\s*\"([A-Za-z_]\w*)\"\s*,\s*(\d+)"
)
# bare literal sampler binds (counted only — name not statically recoverable)
RE_CPP_UNIFORM1I = re.compile(r"\bglUniform1i\s*\(")
RE_CPP_ACTIVETEX = re.compile(r"\bglActiveTexture\s*\(\s*GL_TEXTURE(\d+)\b")

# Intentional same-unit pairs on mutually-exclusive runtime branches (census).
MODE_ALTERNATE_PAIRS = [frozenset({"dynamicShadowArray", "dynamicShadowMap"})]

# ---------------------------------------------------------------------------
# Hard anchors (SHADER-SAMPLER-HARD-ANCHORS-1)
# ---------------------------------------------------------------------------
# A SMALL allowlist of provably-stable, NON-multiplexed (name->unit) bindings
# promoted from WARN to FAIL: the build will not silently let these drift. Each
# entry must meet all four criteria (one GLSL decl file, one C++ receiver, one
# unit, comment agrees) — verified at recon and re-checked here every run. The
# anchor is on the (name -> unit) PAIR, never on the bare unit (unit 0 is
# legitimately multiplexed across ~15 programs). The check is SELF-DEMOTING: if
# a future change makes an anchored name multiplexed (a 2nd decl file or 2nd
# receiver appears), the equality guards below FAIL loudly, forcing the author
# to remove it from this table rather than silently passing a now-unsafe anchor.
HARD_ANCHORS = {
    # name: (expected_unit, glsl_file_substr, cpp_receiver)
    "uSrc":     (0, "shaders/hzb_reduce.frag",  "hzbReduceProg_"),
    "sceneTex": (0, "shaders/postprocess.frag", "compositeProg_"),
    "ssaoTex":  (0, "shaders/ssao_apply.frag",  "ssaoApplyProg_"),
}

# ---------------------------------------------------------------------------
# Vulkan descriptor-contract cross-reference (SAMPLER-UNIT-MANIFEST-1)
# ---------------------------------------------------------------------------
# RenderCore/vulkan_contract.h enumerates exactly 4 CombinedImageSampler rows —
# FBO-backed images the enum-registry OWNS and that are sampled downstream. Each
# GLSL sampler NAME that samples one of those owned resources is tagged with its
# RenderResourceId here, so the sampler occupancy manifest links to the Vulkan
# descriptor contract (a combined image-sampler's future (set,binding) descriptor
# grounds against a KNOWN resource, not just a bare unit literal).
#
# Every sampler here has VkDescriptorClass::CombinedImageSampler by construction
# (that is the only image-descriptor class the contract enumerates today; the
# other ~40 samplers bind textures the registry does NOT yet enum-own — asset
# atlases, splat-material arrays, PBR maps — so they are left resourceId=null,
# descriptorClass="CombinedImageSampler?" i.e. "will be a CIS post-port but the
# resource is not yet enum-backed").
#
# Mapping derived from the vulkan_contract.h row DESCRIPTIONS (verified below at
# runtime against the header text so this table cannot silently drift):
#   MainColor        "scene HDR color (sceneFBO_ attach0); sampled by PostProcess"
#   MainDepth        "scene depth; sampled by VFX/Water/Veg/PostProcess"
#   MainNormal       "GBuffer1 normal (attach1); sampled by SSAO/Shoreline/ScreenShadow/BoxDecals"
#   ShadowDynamicMap "dynamic shadow depth; sampled by StaticProp/Terrain/Mech/PostProcess"
VULKAN_SAMPLER_RESOURCE = {
    # sampler name -> RenderResourceId
    "sceneTex":            "MainColor",
    "u_sceneColor":        "MainColor",
    "sceneDepthTex":       "MainDepth",
    "depthTex":            "MainDepth",
    "u_sceneDepth":        "MainDepth",
    "u_sceneDepthTex":     "MainDepth",
    "sceneNormalTex":      "MainNormal",
    "u_sceneNormalTex":    "MainNormal",
    "dynamicShadowArray":  "ShadowDynamicMap",
    "dynamicShadowMap":    "ShadowDynamicMap",
    "dynamicFullMapShadow":"ShadowDynamicMap",
}
# The set of RenderResourceIds this table claims are CombinedImageSampler; the
# runtime check verifies these are exactly the CIS rows present in the header.
VULKAN_CIS_EXPECTED = {"MainColor", "MainDepth", "MainNormal", "ShadowDynamicMap"}
VULKAN_CONTRACT_H = os.path.join("RenderCore", "vulkan_contract.h")


def scan_vulkan_cis(root):
    """Parse RenderCore/vulkan_contract.h for the RenderResourceIds tagged
    VkDescriptorClass::CombinedImageSampler. Returns (set_of_ids, note). Used to
    self-check VULKAN_SAMPLER_RESOURCE against the header so the map cannot drift
    silently when the contract adds/removes a CIS resource."""
    p = os.path.join(root, VULKAN_CONTRACT_H)
    if not os.path.isfile(p):
        return set(), f"{VULKAN_CONTRACT_H} not found"
    text = read(p)
    # match:  { RenderResourceId::NAME, RenderResourceKind::...,\n  VkDescriptorClass::CombinedImageSampler,
    rx = re.compile(
        r"RenderResourceId::([A-Za-z_]\w*)\s*,\s*RenderResourceKind::[A-Za-z0-9_]+\s*,"
        r"\s*VkDescriptorClass::CombinedImageSampler\b")
    ids = set(rx.findall(text))
    return ids, (f"parsed {len(ids)} CombinedImageSampler rows from "
                 f"{VULKAN_CONTRACT_H.replace(os.sep, '/')}")


def walk(root, dirs, exts):
    for d in dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dp, _, fns in os.walk(base):
            for fn in fns:
                if not fn.endswith(exts):
                    continue
                p = os.path.join(dp, fn)
                rel = os.path.relpath(p, root).replace("\\", "/")
                if any(s.replace("\\", "/") in rel + "/" for s in EXCLUDE_SUBSTR):
                    continue
                yield p, rel


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def scan_glsl(root):
    """name -> list of {file,line,target,comment_unit}."""
    samplers = {}
    for p, rel in walk(root, [GLSL_DIR], GLSL_EXT):
        for i, line in enumerate(read(p).splitlines(), 1):
            for m in RE_GLSL_SAMPLER.finditer(line):
                target, name = m.group(1), m.group(2)
                cu = RE_COMMENT_UNIT.search(line[m.end():])
                samplers.setdefault(name, []).append({
                    "file": rel, "line": i, "target": target,
                    "comment_unit": int(cu.group(1)) if cu else None,
                })
    return samplers


def scan_cpp(root, sampler_names):
    """Returns (assignments, counts). assignments = list of
    {name,unit,receiver,file,line,via}. Only names that are GLSL samplers count
    (setInt is also used for non-sampler ints)."""
    assigns = []
    uniform1i = 0
    activetex = 0
    for p, rel in walk(root, CPP_DIRS, CPP_EXT):
        for i, line in enumerate(read(p).splitlines(), 1):
            for rx, via in ((RE_CPP_SETINT, "setInt"),
                            (RE_CPP_SETSAMPLER, "setSamplerUnit")):
                for m in rx.finditer(line):
                    recv, name, unit = m.group(1), m.group(2), int(m.group(3))
                    if name not in sampler_names:
                        continue
                    assigns.append({"name": name, "unit": unit,
                                    "receiver": recv or "(this)",
                                    "file": rel, "line": i, "via": via})
            uniform1i += len(RE_CPP_UNIFORM1I.findall(line))
            activetex += len(RE_CPP_ACTIVETEX.findall(line))
    return assigns, uniform1i, activetex


def is_mode_alternate(a, b):
    pair = frozenset({a, b})
    return pair in MODE_ALTERNATE_PAIRS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    glsl = scan_glsl(root)
    assigns, n_uniform1i, n_activetex = scan_cpp(root, set(glsl))

    fails, warns = [], []

    # Vulkan contract self-check: the CIS rows in vulkan_contract.h must match
    # what VULKAN_SAMPLER_RESOURCE claims to map to. Drift here means the
    # contract changed and this cross-reference table is stale -> FAIL loudly.
    vk_cis_ids, vk_note = scan_vulkan_cis(root)
    if vk_cis_ids:
        claimed = set(VULKAN_SAMPLER_RESOURCE.values())
        if not claimed <= vk_cis_ids:
            fails.append(f"vulkan-contract drift: sampler->resourceId map references "
                         f"{sorted(claimed - vk_cis_ids)} which are NOT CombinedImageSampler "
                         f"rows in {VULKAN_CONTRACT_H} (rows present: {sorted(vk_cis_ids)})")
        if vk_cis_ids != VULKAN_CIS_EXPECTED:
            warns.append(f"vulkan-contract CIS set changed: header has {sorted(vk_cis_ids)}, "
                         f"table expected {sorted(VULKAN_CIS_EXPECTED)} — review "
                         f"VULKAN_SAMPLER_RESOURCE for new/removed sampled resources")
    else:
        warns.append(f"vulkan-contract cross-ref skipped: {vk_note}")

    # index assignments by name and by receiver
    by_name = {}
    by_recv = {}
    for a in assigns:
        by_name.setdefault(a["name"], []).append(a)
        by_recv.setdefault(a["receiver"], []).append(a)

    # FAIL 1: a NON-multiplexed sampler name (one GLSL file, one C++ receiver,
    # one unit) whose own GLSL unit-comment disagrees with its C++ unit. A
    # multiplexed name (same name used by >1 program at different units, e.g.
    # u_hdri = 0 in skybox vs 3 in water) cannot be checked globally because we
    # do not statically link a GLSL file to its C++ program — those are WARN.
    for name, decls in sorted(glsl.items()):
        binds = by_name.get(name, [])
        cpp_units = {a["unit"] for a in binds}
        glsl_files = {d["file"] for d in decls}
        receivers = {a["receiver"] for a in binds}
        commented = [d for d in decls if d["comment_unit"] is not None]
        if not commented or len(cpp_units) != 1:
            continue  # no comment to check, or no/ambiguous binder
        cu = next(iter(cpp_units))
        multiplexed = len(glsl_files) > 1 or len(receivers) > 1
        for d in commented:
            if d["comment_unit"] == cu:
                continue
            msg = (f"sampler '{name}' bound to unit {cu} in C++ but GLSL "
                   f"{d['file']}:{d['line']} comments unit {d['comment_unit']}")
            if multiplexed:
                warns.append(f"multiplexed comment/code mismatch (not auto-FAIL "
                             f"— verify per-program): {msg}")
            else:
                fails.append(f"comment-vs-code drift: {msg}")

    # FAIL 2: one program receiver assigns the same sampler name two units.
    for recv, items in sorted(by_recv.items()):
        per = {}
        for a in items:
            per.setdefault(a["name"], {}).setdefault(a["unit"], []).append(a)
        for name, units in per.items():
            if len(units) > 1:
                where = "; ".join(
                    f"unit {u} @ {v[0]['file']}:{v[0]['line']}"
                    for u, v in sorted(units.items()))
                fails.append(
                    f"intra-program contradiction: receiver '{recv}' binds "
                    f"sampler '{name}' to multiple units ({where})")

    # FAIL 3 (hard anchors): each anchored (name -> unit) pair must still hold,
    # and must still be non-multiplexed. Any drift OR loss of the four criteria
    # is a FAIL (self-demoting: a new decl/receiver trips the equality guards).
    anchors = []
    for name, (exp_unit, exp_glsl, exp_recv) in sorted(HARD_ANCHORS.items()):
        decls = glsl.get(name, [])
        binds = by_name.get(name, [])
        problems = []
        if not decls:
            problems.append("GLSL declaration vanished/renamed")
        if not binds:
            problems.append("C++ setter vanished/renamed")
        units = {a["unit"] for a in binds}
        recvs = {a["receiver"] for a in binds}
        files = {d["file"] for d in decls}
        if units and units != {exp_unit}:
            problems.append(f"C++ unit set {sorted(units)} != expected {{{exp_unit}}}")
        if len(files) > 1 or (files and not any(exp_glsl in f for f in files)):
            problems.append(f"GLSL decl files {sorted(files)} != expected ~'{exp_glsl}' "
                            f"(anchored name became multiplexed or moved)")
        if len(recvs) > 1 or (recvs and exp_recv not in recvs):
            problems.append(f"C++ receivers {sorted(recvs)} != expected '{exp_recv}' "
                            f"(anchored name became multiplexed or moved)")
        for d in decls:
            if d["comment_unit"] is not None and d["comment_unit"] != exp_unit:
                problems.append(f"GLSL comment unit {d['comment_unit']} != {exp_unit} "
                                f"@ {d['file']}:{d['line']}")
        status = "clean" if not problems else "drift"
        anchors.append({"name": name, "expected_unit": exp_unit,
                        "status": status, "problems": problems})
        for p in problems:
            fails.append(f"hard-anchor '{name}' (expect unit {exp_unit}): {p}")

    # WARN: same receiver, one unit, two distinct sampler names (collision or
    # intentional mode-alternate).
    for recv, items in sorted(by_recv.items()):
        per_unit = {}
        for a in items:
            per_unit.setdefault(a["unit"], set()).add(a["name"])
        for unit, names in sorted(per_unit.items()):
            if len(names) > 1:
                nl = sorted(names)
                alt = any(is_mode_alternate(x, y)
                          for i, x in enumerate(nl) for y in nl[i + 1:])
                tag = "mode-alternate (intentional)" if alt else "shared-unit"
                warns.append(
                    f"{tag}: receiver '{recv}' unit {unit} = {nl}")

    # WARN: GLSL sampler with no discoverable name-resolvable C++ binder.
    for name in sorted(glsl):
        if name not in by_name:
            files = sorted({d["file"] for d in glsl[name]})
            warns.append(
                f"UNKNOWN binder: sampler '{name}' declared in {files} has no "
                f"setInt/setSamplerUnit assignment (dynamic helper, dormant "
                f"path, or literal glUniform1i loc-cache)")

    # Vulkan descriptor-class / resourceId tag helper. Every GLSL sampler is a
    # combined image-sampler post-port; only the ones sampling an enum-OWNED
    # resource get a resourceId (grounded in vulkan_contract.h). The rest are
    # "CombinedImageSampler?" (will be a CIS, resource not yet enum-backed).
    def vk_tag(name):
        rid = VULKAN_SAMPLER_RESOURCE.get(name)
        return {
            "descriptorClass": "CombinedImageSampler" if rid else "CombinedImageSampler?",
            "resourceId": rid,
        }

    # occupancy: unit -> name/source rows (cross-pass reuse is informational)
    occ = {}
    for a in assigns:
        row = {
            "name": a["name"], "side": "cpp", "via": a["via"],
            "receiver": a["receiver"], "file": a["file"], "line": a["line"]}
        row.update(vk_tag(a["name"]))
        occ.setdefault(a["unit"], []).append(row)
    for name, decls in glsl.items():
        for d in decls:
            u = d["comment_unit"]
            if u is None:
                continue
            row = {
                "name": name, "side": "glsl", "target": d["target"],
                "file": d["file"], "line": d["line"]}
            row.update(vk_tag(name))
            occ.setdefault(u, []).append(row)
    for unit, rows in occ.items():
        names = {r["name"] for r in rows}
        if len(names) > 1:
            warns.append(
                f"cross-pass reuse: unit {unit} carries {sorted(names)} "
                f"(expected — multiplexed)")

    # Vulkan-linked samplers (those mapping to an enum-owned CombinedImageSampler)
    vk_linked = sorted(
        {n for n in glsl if n in VULKAN_SAMPLER_RESOURCE})
    vk_by_resource = {}
    for n in vk_linked:
        vk_by_resource.setdefault(VULKAN_SAMPLER_RESOURCE[n], []).append(n)

    samplers_out = {}
    for n in sorted(glsl):
        rid = VULKAN_SAMPLER_RESOURCE.get(n)
        samplers_out[n] = {
            "decls": glsl[n],
            "descriptorClass": "CombinedImageSampler" if rid else "CombinedImageSampler?",
            "resourceId": rid,
        }

    report = {
        "summary": {
            "glsl_samplers": len(glsl),
            "cpp_name_resolvable_binds": len(assigns),
            "glUniform1i_sites": n_uniform1i,
            "glActiveTexture_sites": n_activetex,
            "hard_anchors": len(anchors),
            "anchors_drift": sum(1 for a in anchors if a["status"] == "drift"),
            "vulkan_cis_rows": len(vk_cis_ids),
            "vulkan_linked_samplers": len(vk_linked),
            "fails": len(fails),
            "warns": len(warns),
        },
        "vulkan": {
            "contract_header": VULKAN_CONTRACT_H.replace("\\", "/"),
            "note": vk_note,
            "cis_rows_in_header": sorted(vk_cis_ids),
            "sampler_by_resource": {k: sorted(v) for k, v in sorted(vk_by_resource.items())},
        },
        "fails": fails,
        "warns": warns,
        "anchors": anchors,
        "samplers": samplers_out,
        "assignments": sorted(assigns, key=lambda a: (a["unit"], a["name"], a["file"])),
        "occupancy": {str(u): occ[u] for u in sorted(occ)},
    }

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-sampler-bindings] SHADER-SAMPLER-BINDING-MANIFEST-1")
        print(f"  GLSL samplers              : {len(glsl)}")
        print(f"  C++ name-resolvable binds  : {len(assigns)}")
        print(f"  glUniform1i sites          : {n_uniform1i}")
        print(f"  glActiveTexture sites      : {n_activetex}")
        print(f"  hard anchors               : {len(anchors)} "
              f"({sum(1 for a in anchors if a['status']=='drift')} drift)")
        print(f"  vulkan CIS rows (contract) : {len(vk_cis_ids)}")
        print(f"  vulkan-linked samplers     : {len(vk_linked)} "
              f"-> {', '.join(f'{k}:{len(v)}' for k, v in sorted(vk_by_resource.items()))}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
