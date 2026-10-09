#!/usr/bin/env python3
"""gpu_abi_gen.py -- single source of truth for C++/GLSL mirrored GPU structs.

The annotated C++ struct is the source. This tool computes the std430/std140
layout independently of the compiler and emits, byte-deterministically (LF, no
timestamps):
  * <name>.layout.gen.h   C++ static_asserts (sizeof, alignof, per-field offsetof)
  * a GLSL struct in shaders/include/ (FLAT file: the shader_deploy hook and the
    CMake DEPENDS glob are non-recursive)

Usage:
    py -3 tools/gpu_abi_gen.py --write   regenerate the committed files
    py -3 tools/gpu_abi_gen.py --check   regenerate in memory and diff (drift = exit 1)
Exit 2 = parse / unknown type / layout-vs-sizeof error.

Source subset: `// @gpu std430 glsl=<GlslName>` above `struct [alignas(N)] Name {`;
fields are fixed-width scalars (float, int32_t, uint32_t) or float[N]. Arrays
of float with N in {2,3,4,16} need an annotation in the trailing comment
(@vec2 / @vec3 / @vec4 / @mat4); a bare float[N] is a GLSL scalar array.
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
]


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
_ANNOT_ARR = {"@mat4": ("mat4", 16), "@vec4": ("vec4", 4),
              "@vec3": ("vec3", 3), "@vec2": ("vec2", 2)}
_FIELD_RE = re.compile(
    r"^\s*(float|int32_t|uint32_t)\s+(\w+)(?:\[(\d+)\])?\s*;\s*(?://\s*(.*))?$")


def parse_source(text, label):
    m = re.search(r"^//\s*@gpu\s+(std430|std140)\s+glsl=(\w+)\s*$", text, re.M)
    if not m:
        raise AbiError("%s: missing '// @gpu <rules> glsl=<Name>' annotation" % label)
    rules, glsl_name = m.group(1), m.group(2)
    sm = re.compile(r"struct\s+(?:alignas\((\d+)\)\s+)?(\w+)\s*\{").search(text, m.end())
    if not sm:
        raise AbiError("%s: no struct after @gpu annotation" % label)
    cpp_align = int(sm.group(1)) if sm.group(1) else None
    cpp_name = sm.group(2)
    end = text.index("};", sm.end())
    body = text[sm.end():end]
    fields, cpp_fields = [], []
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        fm = _FIELD_RE.match(raw)
        if not fm:
            raise AbiError("%s: unsupported member line: %r" % (label, raw))
        ctype, name, count, comment = fm.group(1), fm.group(2), fm.group(3), fm.group(4)
        annot = None
        if comment:
            first = comment.split()[0] if comment.split() else ""
            if first.startswith("@"):
                annot = first
                if annot not in _ANNOT_ARR:
                    raise AbiError("%s: unknown annotation %s on %s" % (label, annot, name))
        base = _CPP_SCALAR[ctype]
        if count is None:
            if annot:
                raise AbiError("%s: %s on non-array %s" % (label, annot, name))
            gtype = base
        elif annot:
            if ctype != "float":
                raise AbiError("%s: %s needs float[N] (%s)" % (label, annot, name))
            gtype, want = _ANNOT_ARR[annot]
            if int(count) != want:
                raise AbiError("%s: %s expects float[%d], %s is [%s]"
                               % (label, annot, want, name, count))
        else:
            if int(count) < 2:
                raise AbiError("%s: array %s too small" % (label, name))
            gtype = ("arr", base, int(count))
        fields.append((name, gtype))
        cpp_fields.append(name)
    if not fields:
        raise AbiError("%s: empty struct" % label)
    return {"rules": rules, "glsl": glsl_name, "cpp": cpp_name,
            "cpp_align": cpp_align, "fields": fields}


def glsl_type_str(t):
    if isinstance(t, str):
        return t, ""
    return t[1], "[%d]" % t[2]


# ---------------------------------------------------------------------------
# Emitters.
# ---------------------------------------------------------------------------
def build(info, src_rel, glsl_rel):
    offs, align, size = struct_offsets(info["fields"], info["rules"])
    if info["rules"] != "std430":
        raise AbiError("only std430 SSBO structs are generated (got %s)" % info["rules"])
    if info["cpp_align"] is not None and info["cpp_align"] != align:
        raise AbiError("%s: alignas(%d) != %s alignment %d"
                       % (info["cpp"], info["cpp_align"], info["rules"], align))
    name, g = info["cpp"], info["glsl"]
    src_name = os.path.basename(src_rel)
    lay = ["// GENERATED by tools/gpu_abi_gen.py from %s -- DO NOT EDIT." % src_name,
           "// Regenerate: py -3 tools/gpu_abi_gen.py --write   Verify: --check",
           "// Offsets computed from %s rules, independent of the compiler." % info["rules"],
           "#pragma once", "",
           "static_assert(std::is_trivially_copyable<%s>::value, \"%s must be trivially copyable\");" % (name, name),
           "static_assert(sizeof(%s) == %d, \"%s size must match %s GLSL struct\");" % (name, size, name, info["rules"]),
           "static_assert(alignof(%s) == %d, \"%s alignment\");" % (name, align, name)]
    for fname, o in offs:
        lay.append("static_assert(offsetof(%s, %s) == %d, \"%s offset\");" % (name, fname, o, fname))
    lay.append("")
    gl = ["// GENERATED by tools/gpu_abi_gen.py from %s -- DO NOT EDIT." % src_rel,
          "// Regenerate: py -3 tools/gpu_abi_gen.py --write   Verify: --check",
          "// %s rules, %d bytes. Include from the shader; the shader keeps its own" % (info["rules"], size),
          "// `layout(...) buffer` wrapper (GL and VK binding syntax differ).",
          "#ifndef GPU_ABI_%s_GEN_HGLSL" % g.upper(),
          "#define GPU_ABI_%s_GEN_HGLSL" % g.upper(),
          "struct %s {" % g]
    off_of = dict(offs)
    for fname, ft in info["fields"]:
        ts, suffix = glsl_type_str(ft)
        gl.append("    %-5s %s%s;  // off %d" % (ts, fname, suffix, off_of[fname]))
    gl += ["};", "#endif", ""]
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
        if bad:
            return 1
        print("gpu_abi_gen: %d generated files match their sources" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
