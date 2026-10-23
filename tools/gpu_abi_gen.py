#!/usr/bin/env python3
"""gpu_abi_gen.py -- single source of truth for C++/GLSL mirrored GPU structs.

The annotated C++ struct is the source. This tool computes the std430/std140
layout independently of the compiler and emits, byte-deterministically (LF, no
timestamps):
  * <name>.layout.gen.h   C++ static_asserts (sizeof, alignof, per-field offsetof)
  * a GLSL struct in shaders/include/ (FLAT file: the CMake DEPENDS glob is
    non-recursive)

Usage:
    py -3 tools/gpu_abi_gen.py --write   regenerate the committed files
    py -3 tools/gpu_abi_gen.py --check   regenerate in memory and diff (drift = exit 1)
Exit 2 = parse / unknown type / layout-vs-sizeof error.

Source subset: `// @gpu std430 glsl=<GlslName>` above `struct [alignas(N)] Name {`;
fields are fixed-width scalars (float, int32_t, uint32_t) or float[N]. Arrays
of float with N in {2,3,4,16} need an annotation in the trailing comment
(@vec2 / @vec3 / @vec4 / @mat4); a bare float[N] is a GLSL scalar array.

std140 UNIFORM BLOCKS: `// @gpu std140 glsl=<Block> block=<instance> set=<S> binding=<B>
[slot=<bytes>]` emits the whole `layout(...) uniform` block (not a bare struct), so
the shader and the C++ POD share one member list. Extra member annotations:
  @mat4xN   float[16*N]: GLSL mat4[N]
  @rgb      float[3]: three consecutive GLSL scalars <name>R/<name>G/<name>B (no vec3
            padding; the following member packs at +12)
  @cpp_only member exists only in C++ (slot tail padding); not in the GLSL block
`slot` is the C++ sizeof (dynamic-offset stride), which may exceed the std140 size.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (source header, generated C++ layout header, generated GLSL include)
STRUCTS = [
    ("RenderCore/gpu_abi/GpuStaticPropInstance.h",
     "RenderCore/gpu_abi/GpuStaticPropInstance.layout.gen.h",
     "shaders/include/gpu_abi_static_prop_instance.gen.hglsl"),
    ("RenderCore/gpu_abi/GpuMechInstance.h",
     "RenderCore/gpu_abi/GpuMechInstance.layout.gen.h",
     "shaders/include/gpu_abi_mech_instance.gen.hglsl"),
    ("RenderCore/gpu_abi/GpuObjectLights.h",
     "RenderCore/gpu_abi/GpuObjectLights.layout.gen.h",
     "shaders/include/gpu_abi_object_lights.gen.hglsl"),
    # VK-only per-frame uniform blocks of the Tier-1 islands (outside the GL/VK-shared
    # shaders/include that the GL shader lints scan).
    ("RenderCore/gpu_abi/GpuColorFrameUBO.h",
     "RenderCore/gpu_abi/GpuColorFrameUBO.layout.gen.h",
     "shaders/vulkan/include/color_frame_ubo_vk.glsl"),
    ("RenderCore/gpu_abi/GpuMechFrameUBO.h",
     "RenderCore/gpu_abi/GpuMechFrameUBO.layout.gen.h",
     "shaders/vulkan/include/mech_frame_ubo_vk.glsl"),
    ("RenderCore/gpu_abi/GpuLegacyFrameUBO.h",
     "RenderCore/gpu_abi/GpuLegacyFrameUBO.layout.gen.h",
     "shaders/vulkan/include/legacy_frame_ubo_vk.glsl"),
    # VK-only uniform block: lives beside the other VK includes (shaders/vulkan/include),
    # outside the GL/VK-shared shaders/include that the GL shader lints scan.
    ("RenderCore/gpu_abi/GpuTerrainFrameUBO.h",
     "RenderCore/gpu_abi/GpuTerrainFrameUBO.layout.gen.h",
     "shaders/vulkan/include/terrain_frame_ubo_vk.glsl"),
]


# Active native shaders include the generated ABI declarations directly.
# Keep the mirror validator for offline fixtures; the last raw GL consumer retired.
MIRRORS = []


class AbiError(Exception):
    pass


# ---------------------------------------------------------------------------
# Layout engine. Types: "float" | "int" | "uint" | "bool" | "vec2/3/4" |
# "ivecN"/"uvecN" | "matN" (square, column-major) | ("arr", type, n) |
# ("struct", [(name, type), ...]).
# ---------------------------------------------------------------------------
_SCALARS = {"float", "int", "uint", "bool"}


def _round_up(x, a):
    return (x + a - 1) // a * a


def _vec_dim(t):
    m = re.fullmatch(r"[iub]?vec([234])", t) if isinstance(t, str) else None
    return int(m.group(1)) if m else None


def align_size(t, rules):
    """Return (alignment, size) of type t under rules 'std140' or 'std430'."""
    if rules not in ("std140", "std430"):
        raise AbiError("unknown rules %r" % rules)
    if isinstance(t, str):
        if t in _SCALARS:
            return 4, 4
        n = _vec_dim(t)
        if n:
            return {2: (8, 8), 3: (16, 12), 4: (16, 16)}[n]
        m = re.fullmatch(r"mat([234])", t)
        if m:
            c = int(m.group(1))  # square matrix: c columns of vec<c>
            return align_size(("arr", "vec%d" % c, c), rules)
        raise AbiError("unknown type %r" % t)
    kind = t[0]
    if kind == "arr":
        _, et, n = t
        ea, es = align_size(et, rules)
        if rules == "std140":
            ea = _round_up(ea, 16)
        stride = _round_up(es, ea)
        return ea, stride * n
    if kind == "struct":
        al = 4
        off = 0
        for _, ft in t[1]:
            fa, fs = align_size(ft, rules)
            off = _round_up(off, fa) + fs
            al = max(al, fa)
        if rules == "std140":
            al = _round_up(al, 16)
        return al, _round_up(off, al)
    raise AbiError("unknown type %r" % (t,))


def struct_offsets(fields, rules):
    """fields: [(name, type)]. Returns ([(name, offset)], align, size)."""
    out = []
    off = 0
    al = 4
    for name, ft in fields:
        fa, fs = align_size(ft, rules)
        off = _round_up(off, fa)
        out.append((name, off))
        off += fs
        al = max(al, fa)
    if rules == "std140":
        al = _round_up(al, 16)
    return out, al, _round_up(off, al)


# ---------------------------------------------------------------------------
# C++ header parser (regex-lite, restricted subset, fails loudly).
# ---------------------------------------------------------------------------
_CPP_SCALAR = {"float": "float", "int32_t": "int", "uint32_t": "uint"}
# annotation -> (GLSL type, array length, required C++ scalar type)
_ANNOT_ARR = {"@mat4": ("mat4", 16, "float"), "@vec4": ("vec4", 4, "float"),
              "@vec3": ("vec3", 3, "float"), "@vec2": ("vec2", 2, "float"),
              "@ivec4": ("ivec4", 4, "int32_t")}
_ANNOT_BLOCK = ("@rgb", "@cpp_only")
# Valid in plain structs and blocks alike.
_ANNOT_ANY = ("@mat4xN", "@vec4xN", "@ivec4tail", "@fold")
_FIELD_RE = re.compile(
    r"^\s*(float|int32_t|uint32_t)\s+(\w+)(?:\[(\w+)\])?(?:\[(\w+)\])?\s*;\s*(?://\s*(.*))?$")
_CONST_RE = re.compile(r"^\s*(?:static\s+|inline\s+)*constexpr\s+\w+\s+(\w+)\s*=\s*(\d+)\s*;", re.M)


def parse_source(text, label):
    m = re.search(r"^//\s*@gpu\s+(std430|std140)\s+glsl=(\w+)((?:\s+\w+=\S+)*)\s*$", text, re.M)
    if not m:
        raise AbiError("%s: missing '// @gpu <rules> glsl=<Name>' annotation" % label)
    rules, glsl_name = m.group(1), m.group(2)
    opts = dict(kv.split("=", 1) for kv in m.group(3).split())
    block = opts.get("block")
    if block and (rules != "std140" or "set" not in opts or "binding" not in opts):
        raise AbiError("%s: block= needs std140 plus set= and binding=" % label)
    if set(opts) - {"block", "set", "binding", "slot", "cpp_align", "set_macro"}:
        raise AbiError("%s: unknown @gpu option(s) %s" % (label, sorted(opts)))
    if opts.get("cpp_align", "any") != "any":
        raise AbiError("%s: cpp_align= only accepts 'any'" % label)
    sm = re.compile(r"struct\s+(?:alignas\((\d+)\)\s+)?(\w+)\s*\{").search(text, m.end())
    if not sm:
        raise AbiError("%s: no struct after @gpu annotation" % label)
    cpp_align = int(sm.group(1)) if sm.group(1) else None
    cpp_name = sm.group(2)
    end = text.index("};", sm.end())
    body = text[sm.end():end]
    # Array dimensions may name a constexpr integer declared earlier in the header, so the
    # cap is written once and shared with C++ code that sizes loops from it.
    consts = {k: int(v) for k, v in _CONST_RE.findall(text[:m.start()])}

    def dim(tok, what):
        if tok is None:
            return None
        if tok.isdigit():
            return int(tok)
        if tok in consts:
            return consts[tok]
        raise AbiError("%s: unknown array dimension %r on %s" % (label, tok, what))

    fields, cpp_fields = [], []   # cpp_fields: (cpp name, first GLSL member | None, byte delta)
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        fm = _FIELD_RE.match(raw)
        if not fm:
            raise AbiError("%s: unsupported member line: %r" % (label, raw))
        ctype, name, comment = fm.group(1), fm.group(2), fm.group(5)
        count, count2 = dim(fm.group(3), name), dim(fm.group(4), name)
        annot, rename = None, None
        if comment:
            toks = comment.split()
            if toks and toks[0].startswith("@"):
                annot = toks[0]
                if annot not in _ANNOT_ARR and annot not in _ANNOT_ANY \
                        and not (block and annot in _ANNOT_BLOCK):
                    raise AbiError("%s: unknown annotation %s on %s" % (label, annot, name))
            for t in toks[:3]:
                rm = re.match(r"glsl=(\w+)", t)
                if rm:
                    rename = rm.group(1)
        gname = rename or name   # GLSL member name (rename= keeps shader-side snake_case)
        base = _CPP_SCALAR[ctype]
        if annot == "@cpp_only":
            cpp_fields.append((name, None, 0))
            continue
        if annot == "@rgb":
            if ctype != "float" or count != 3 or count2 is not None:
                raise AbiError("%s: @rgb needs float[3] (%s)" % (label, name))
            fields += [(name + c, "float") for c in "RGB"]
            cpp_fields.append((name, name + "R", 0))
            continue
        if annot == "@mat4xN" and count2 is not None:
            if ctype != "float" or count2 != 16:
                raise AbiError("%s: @mat4xN needs float[N][16] (%s)" % (label, name))
            fields.append((gname, ("arr", "mat4", count)))
            cpp_fields.append((name, gname, 0))
            continue
        if annot == "@vec4xN":
            if ctype != "float" or count2 != 4:
                raise AbiError("%s: @vec4xN needs float[N][4] (%s)" % (label, name))
            fields.append((gname, ("arr", "vec4", count)))
            cpp_fields.append((name, gname, 0))
            continue
        if count2 is not None:
            raise AbiError("%s: 2-D array %s needs @mat4xN or @vec4xN" % (label, name))
        if annot == "@mat4xN":
            if ctype != "float" or count is None or count % 16:
                raise AbiError("%s: @mat4xN needs float[16*N] (%s)" % (label, name))
            fields.append((gname, ("arr", "mat4", count // 16)))
            cpp_fields.append((name, gname, 0))
            continue
        if annot == "@ivec4tail":
            # A scalar that opens a GLSL ivec4; the next member (@fold) is its 3 pad lanes.
            if ctype != "int32_t" or count is not None:
                raise AbiError("%s: @ivec4tail needs a scalar int32_t (%s)" % (label, name))
            fields.append((gname, "ivec4"))
            cpp_fields.append((name, gname, 0))
            continue
        if annot == "@fold":
            if ctype != "int32_t" or count != 3 or not fields or fields[-1][1] != "ivec4":
                raise AbiError("%s: @fold needs int32_t[3] right after an @ivec4tail (%s)"
                               % (label, name))
            cpp_fields.append((name, fields[-1][0], 4))
            continue
        if count is None:
            if annot:
                raise AbiError("%s: %s on non-array %s" % (label, annot, name))
            gtype = base
        elif annot:
            gtype, want, need = _ANNOT_ARR[annot]
            if ctype != need:
                raise AbiError("%s: %s needs %s[N] (%s)" % (label, annot, need, name))
            if count != want:
                raise AbiError("%s: %s expects %s[%d], %s is [%s]"
                               % (label, annot, need, want, name, count))
        else:
            if count < 2:
                raise AbiError("%s: array %s too small" % (label, name))
            gtype = ("arr", base, count)
        fields.append((gname, gtype))
        cpp_fields.append((name, gname, 0))
    if not fields:
        raise AbiError("%s: empty struct" % label)
    return {"rules": rules, "glsl": glsl_name, "cpp": cpp_name,
            "cpp_align": cpp_align, "cpp_align_any": opts.get("cpp_align") == "any",
            "fields": fields, "cpp_fields": cpp_fields,
            "block": block, "set": opts.get("set"), "binding": opts.get("binding"),
            "set_macro": opts.get("set_macro"),
            "slot": int(opts["slot"]) if "slot" in opts else None}


def glsl_type_str(t):
    if isinstance(t, str):
        return t, ""
    return t[1], "[%d]" % t[2]


# ---------------------------------------------------------------------------
# Emitters.
# ---------------------------------------------------------------------------
def build(info, src_rel, glsl_rel):
    offs, align, size = struct_offsets(info["fields"], info["rules"])
    block = info["block"]
    if info["rules"] != "std430" and not block:
        raise AbiError("std140 structs are only generated as uniform blocks (block=)")
    if info["cpp_align"] is not None and info["cpp_align"] != align:
        raise AbiError("%s: alignas(%d) != %s alignment %d"
                       % (info["cpp"], info["cpp_align"], info["rules"], align))
    off_of = dict(offs)
    end = offs[-1][1] + align_size(info["fields"][-1][1], info["rules"])[1]
    slot = info["slot"] if info["slot"] is not None else size
    if slot < size:
        raise AbiError("%s: slot=%d smaller than %s size %d" % (info["cpp"], slot, info["rules"], size))
    if sum(1 for _, g, _d in info["cpp_fields"] if g is None) > 1:
        raise AbiError("%s: at most one @cpp_only tail member" % info["cpp"])
    name, g = info["cpp"], info["glsl"]
    src_name = os.path.basename(src_rel)
    lay = ["// GENERATED by tools/gpu_abi_gen.py from %s -- DO NOT EDIT." % src_name,
           "// Regenerate: py -3 tools/gpu_abi_gen.py --write   Verify: --check",
           "// Offsets computed from %s rules, independent of the compiler." % info["rules"],
           "#pragma once", "",
           "static_assert(std::is_trivially_copyable<%s>::value, \"%s must be trivially copyable\");" % (name, name),
           "static_assert(sizeof(%s) == %d, \"%s size must match %s GLSL struct\");" % (name, slot, name, info["rules"])]
    if (not block and not info["cpp_align_any"]) or info["cpp_align"] is not None:
        lay.append("static_assert(alignof(%s) == %d, \"%s alignment\");" % (name, align, name))
    for cname, gname, delta in info["cpp_fields"]:
        o = end if gname is None else off_of[gname] + delta
        lay.append("static_assert(offsetof(%s, %s) == %d, \"%s offset\");" % (name, cname, o, cname))
    lay.append("")
    gl = ["// GENERATED by tools/gpu_abi_gen.py from %s -- DO NOT EDIT." % src_rel,
          "// Regenerate: py -3 tools/gpu_abi_gen.py --write   Verify: --check"]
    # Array lengths as constants, so shader loops bound to an array member cannot be
    # hand-copied and drift from the length declared in the C++ struct.
    len_defs = ["#define GPU_ABI_%s_%s_LEN %d" % (g.upper(), fname.upper(), ft[2])
                for fname, ft in info["fields"] if not isinstance(ft, str)]
    if block:
        gl += ["// %s rules, %d bytes (C++ slot %d). Declares the whole uniform block." % (info["rules"], size, slot),
               "#ifndef GPU_ABI_%s_GEN_GLSL" % g.upper(),
               "#define GPU_ABI_%s_GEN_GLSL" % g.upper()]
        gl += len_defs
        descriptor_set = info["set"]
        if info.get("set_macro"):
            descriptor_set = info["set_macro"]
            if not re.fullmatch(r"[A-Z][A-Z0-9_]*", descriptor_set):
                raise AbiError("invalid descriptor set macro: " + descriptor_set)
            gl += ["#ifndef " + descriptor_set, "#define %s %s" % (descriptor_set, info["set"]), "#endif"]
        gl += ["layout(set = %s, binding = %s, std140) uniform %s {" % (descriptor_set, info["binding"], g)]
    else:
        gl += ["// %s rules, %d bytes. Include from the shader; the shader keeps its own" % (info["rules"], size),
               "// `layout(...) buffer` wrapper (GL and VK binding syntax differ).",
               "#ifndef GPU_ABI_%s_GEN_HGLSL" % g.upper(),
               "#define GPU_ABI_%s_GEN_HGLSL" % g.upper()] + len_defs + [
               "struct %s {" % g]
    for fname, ft in info["fields"]:
        ts, suffix = glsl_type_str(ft)
        gl.append("    %-5s %s%s;  // off %d" % (ts, fname, suffix, off_of[fname]))
    gl += ["} %s;" % block if block else "};", "#endif", ""]
    return {"layout": "\n".join(lay), "glsl": "\n".join(gl)}


def generate_all(root):
    results = []
    for src, lay_rel, glsl_rel in STRUCTS:
        with open(os.path.join(root, src), "r", encoding="utf-8", newline="") as f:
            text = f.read().replace("\r\n", "\n")
        info = parse_source(text, src)
        out = build(info, src, glsl_rel)
        results.append((lay_rel, out["layout"]))
        results.append((glsl_rel, out["glsl"]))
    return results


def verify_mirrors(root):
    """Problems (strings) where a raw-loaded shader's private mirror struct differs from
    the generated struct it mirrors, in member name, type or array length."""
    problems = []
    for shader, mirror, target in MIRRORS:
        want = None
        for src, _, _ in STRUCTS:
            with open(os.path.join(root, src), "r", encoding="utf-8", newline="") as f:
                info = parse_source(f.read().replace("\r\n", "\n"), src)
            if info["glsl"] == target:
                want = [(n, glsl_type_str(t)[0], glsl_type_str(t)[1]) for n, t in info["fields"]]
        if want is None:
            problems.append("%s: mirror target %s is not a generated struct" % (shader, target))
            continue
        text = re.sub(r"//[^\n]*", "", _read(os.path.join(root, shader)) or "")
        defs = {k: v for k, v in re.findall(r"^\s*#\s*define\s+(\w+)\s+(\d+)", text, re.M)}
        m = re.search(r"\bstruct\s+" + mirror + r"\s*\{([^}]*)\}", text)
        if not m:
            problems.append("%s: mirror struct %s not found" % (shader, mirror))
            continue
        got = []
        for line in m.group(1).split(";"):
            fm = re.fullmatch(r"\s*(\w+)\s+(\w+)\s*(?:\[\s*(\w+)\s*\])?\s*", line)
            if not fm:
                if line.strip():
                    problems.append("%s: cannot parse mirror member %r" % (shader, line.strip()))
                continue
            dim = fm.group(3)
            if dim is not None:
                dim = defs.get(dim, dim)
            got.append((fm.group(2), fm.group(1), "" if dim is None else "[%s]" % dim))
        if got != want:
            problems.append("%s: %s differs from generated %s (mirror %s, generated %s)"
                            % (shader, mirror, target, got, want))
    return problems


_SHADER_EXTS = (".vert", ".frag", ".comp", ".geom", ".glsl", ".hglsl")


# Deliberate prefix declarations: the overlay/decal vertex stages read only the leading
# worldToClipGL mat4 of the terrain block, whose offset 0 is pinned by the generated
# offsetof assert, so a one-member prefix block cannot stride-drift.
_REDECL_ALLOW = {("shaders/vulkan/include/terrain_overlay_vs.glsl", "TerrainFrameUBO")}


def find_redeclarations(root, results):
    """Shader files (other than the generated ones) that re-declare a generated GLSL
    struct or block. A private copy is an independent stride contract that nothing
    checks -- the class of bug behind the 48-vs-64 byte shadow_mech.vert record."""
    generated = {rel for rel, _ in results}
    pats = []
    for src, _, _ in STRUCTS:
        with open(os.path.join(root, src), "r", encoding="utf-8", newline="") as f:
            info = parse_source(f.read().replace("\r\n", "\n"), src)
        kw = r"uniform\s+" if info["block"] else r"struct\s+"
        pats.append((info["glsl"], re.compile(r"\b" + kw + info["glsl"] + r"\s*\{")))
    bad = []
    for dirpath, dirnames, files in os.walk(os.path.join(root, "shaders")):
        dirnames[:] = [d for d in dirnames if d != "spv"]
        for fn in files:
            if not fn.endswith(_SHADER_EXTS):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if rel in generated:
                continue
            text = re.sub(r"//[^\n]*", "", _read(path) or "")
            for name, pat in pats:
                if pat.search(text) and (rel, name) not in _REDECL_ALLOW:
                    bad.append((rel, name))
    return sorted(bad)


def _read(path):
    try:
        with open(path, "r", encoding="utf-8", newline="") as f:
            return f.read()
    except OSError:
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    ap.add_argument("--root", default=ROOT, help="repo root (tests use a temp copy)")
    a = ap.parse_args(argv)
    try:
        results = generate_all(a.root)
    except AbiError as e:
        print("gpu_abi_gen: ERROR: %s" % e, file=sys.stderr)
        return 2
    bad = 0
    for rel, content in results:
        path = os.path.join(a.root, rel)
        if a.write:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if _read(path) != content:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(content)
                print("wrote %s" % rel)
        else:
            if _read(path) != content:
                print("gpu_abi_gen: DRIFT: %s is stale/missing; run "
                      "py -3 tools/gpu_abi_gen.py --write" % rel, file=sys.stderr)
                bad += 1
    if a.check:
        for rel, name in find_redeclarations(a.root, results):
            print("gpu_abi_gen: REDECLARED: %s declares %s; include the generated "
                  "GLSL instead" % (rel, name), file=sys.stderr)
            bad += 1
        for msg in verify_mirrors(a.root):
            print("gpu_abi_gen: MIRROR: %s" % msg, file=sys.stderr)
            bad += 1
        if bad:
            return 1
        print("gpu_abi_gen: %d generated files match their sources" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
