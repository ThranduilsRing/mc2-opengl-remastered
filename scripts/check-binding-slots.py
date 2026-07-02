#!/usr/bin/env python3
"""check-binding-slots.py — GPU-BINDING-SLOTS-LOCKSTEP-1

CI/check-time lockstep + occupancy checker for GPU buffer BASE BINDING slots
(SSBO / UBO binding-base points). NOT a flat global registry: the census proved
slot numbers are multiplexed per-pass (slot 0 is bound by 7 different buffers in
different passes), so a number is only semantic INSIDE a pass/pipeline. This
script gives visibility instead of a false flat namespace:

  - which file binds which role to which slot (occupancy table)
  - do C++ and GLSL agree where a binding is SHARED (a #define present on both
    sides, or a known C++const<->GLSL-literal pair)?
  - does any single GLSL stage file bind two different blocks to one slot?
  - which bindings are bare GLSL literals not tied to a named constant?

Exit code: 0 unless a FAIL is found (named C++/GLSL pair mismatch, or a same-file
slot collision). WARN never fails the build. Intentional cross-pass reuse is
DOCUMENTED, not failed.

Texture-unit bindings (layout(binding=N) uniform sampler*) are OUT of scope —
that is the GL_TEXTURE_* namespace, not the buffer-binding-base namespace.

Usage:
  py -3 scripts/check-binding-slots.py [--root <repo>] [--json <out.json>] [--quiet]
"""
import argparse
import json
import os
import re
import sys

# ---- scope -----------------------------------------------------------------
# Engine C++ that defines binding constants. We scan all .h/.hpp/.cpp under
# these dirs; tool-only binders (tools/asset_viewer) and GLSL test fixtures are
# excluded because they intentionally diverge (recon: view_uniforms_contract.frag
# declares slot 3 as an SSBO while production declares it a UBO).
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

# Cross-file same-PIPELINE collisions that were RESOLVED by an explicit slice
# (this same-file checker cannot see cross-file/cross-stage collisions — the
# per-pipeline classifier scripts/check-vulkan-bindings.py owns that. This list
# is a documentation cross-reference + regression breadcrumb, printed for
# visibility. Format: (namespace, slot) -> note).
RESOLVED_COLLISIONS = {
    ("SSBO", 2): (
        "VK-H2-BINDING-SPLIT-1: former terrain-mask-solid/water same-pipeline "
        "collision (vert LightingBuf@2 vs frag ThinRecordBufFrag@2) RESOLVED — "
        "LightingBuf moved to SSBO slot 4. Slot 2 now carries only "
        "ThinRecordBufFrag in the mask programs. Do NOT reintroduce a second "
        "buffer at SSBO:2 in the terrain mask VS (check-vulkan-bindings.py "
        "enforces the ERROR)."
    ),
}

# A name is "binding-ish" if it plausibly names a buffer binding-base slot.
BINDINGISH = re.compile(r"(BINDING|_SLOT|SsboBinding|Binding)\b", re.IGNORECASE)

# C++ define / const forms ----------------------------------------------------
RE_CPP_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+(\d+)\b")
RE_CPP_CONST = re.compile(
    r"\b(?:static\s+)?(?:constexpr|const)\s+(?:\w+\s+)*?([A-Za-z_]\w*)\s*=\s*(\d+)\s*u?\s*;"
)
# literal glBindBufferBase(GL_SHADER_STORAGE_BUFFER|GL_UNIFORM_BUFFER, N, ...)
RE_CPP_BINDBASE = re.compile(
    r"glBindBufferBase\s*\(\s*(GL_SHADER_STORAGE_BUFFER|GL_UNIFORM_BUFFER)\s*,\s*(\d+)\s*u?\s*,"
)

# GLSL forms ------------------------------------------------------------------
RE_GLSL_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+(\d+)\b")
# layout(... binding = N ...) [readonly|writeonly|coherent|restrict|volatile]* (buffer|uniform) Block {
# value may be a number OR a macro token.
RE_GLSL_BLOCK = re.compile(
    r"layout\s*\(([^)]*)\)\s*"
    r"(?:(?:readonly|writeonly|coherent|restrict|volatile|std140|std430)\s+)*"
    r"(buffer|uniform)\s+([A-Za-z_]\w*)\s*\{"
)
RE_BINDING_IN_LAYOUT = re.compile(r"\bbinding\s*=\s*([A-Za-z_]\w*|\d+)")
RE_STD = re.compile(r"\bstd(140|430)\b")


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


def scan_cpp(root):
    """Return: defines{name:[(val,rel,line)]}, bindbases[(target,val,rel,line)]"""
    defines = {}
    bindbases = []
    for p, rel in walk(root, CPP_DIRS, CPP_EXT):
        for i, line in enumerate(read(p).splitlines(), 1):
            m = RE_CPP_DEFINE.match(line)
            if m and BINDINGISH.search(m.group(1)):
                defines.setdefault(m.group(1), []).append((int(m.group(2)), rel, i))
                continue
            m = RE_CPP_CONST.search(line)
            if m and BINDINGISH.search(m.group(1)):
                defines.setdefault(m.group(1), []).append((int(m.group(2)), rel, i))
            for m in RE_CPP_BINDBASE.finditer(line):
                bindbases.append((m.group(1), int(m.group(2)), rel, i))
    return defines, bindbases


RE_PP_OPEN = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b")
RE_PP_MID = re.compile(r"^\s*#\s*(elif|else)\b")
RE_PP_CLOSE = re.compile(r"^\s*#\s*endif\b")


def compute_branch_paths(text):
    """Map each 1-based line to its preprocessor branch path:
    {group_id: branch_index}. Two blocks are mutually exclusive (cannot both be
    compiled into one program permutation) iff they share a group_id with a
    different branch_index — i.e. one is in the #if side and the other in the
    #else/#elif side of the same conditional. Used so intentional mode-alternate
    bindings (slot 9 = VisibleIds in C1b vs DebugOut in C1a) are NOT flagged as
    same-pass collisions."""
    paths = {}
    stack = []  # [group_id, branch_idx]
    gid = 0
    for i, line in enumerate(text.splitlines(), 1):
        if RE_PP_OPEN.match(line):
            gid += 1
            stack.append([gid, 0])
        elif RE_PP_MID.match(line):
            if stack:
                stack[-1][1] += 1
        elif RE_PP_CLOSE.match(line):
            if stack:
                stack.pop()
        paths[i] = {g: b for g, b in stack}
    return paths


def mutually_exclusive(p1, p2):
    for g, b in p1.items():
        if g in p2 and p2[g] != b:
            return True
    return False


def scan_glsl(root):
    """Return: defines{name:[(val,rel,line)]}, blocks[ {..} ]"""
    defines = {}
    blocks = []
    for p, rel in walk(root, [GLSL_DIR], GLSL_EXT):
        text = read(p)
        for i, line in enumerate(text.splitlines(), 1):
            m = RE_GLSL_DEFINE.match(line)
            if m and BINDINGISH.search(m.group(1)):
                defines.setdefault(m.group(1), []).append((int(m.group(2)), rel, i))
        branch_paths = compute_branch_paths(text)
        # block decls may span lines; scan the whole text with line attribution
        for m in RE_GLSL_BLOCK.finditer(text):
            layout, kind, block = m.group(1), m.group(2), m.group(3)
            bm = RE_BINDING_IN_LAYOUT.search(layout)
            if not bm:
                continue  # block without explicit binding — not our concern
            line = text.count("\n", 0, m.start()) + 1
            std = RE_STD.search(layout)
            blocks.append(
                {
                    "file": rel,
                    "line": line,
                    "kind": kind,  # buffer (SSBO) | uniform (UBO)
                    "block": block,
                    "binding_token": bm.group(1),
                    "std": ("std" + std.group(1)) if std else None,
                    "branch": branch_paths.get(line, {}),
                }
            )
    return defines, blocks


def resolve_token(token, glsl_defs, cpp_defs):
    """A GLSL binding token may be a literal or a macro. Resolve to int if known."""
    if token.isdigit():
        return int(token), "literal"
    for store, side in ((glsl_defs, "glsl-define"), (cpp_defs, "cpp-define")):
        if token in store:
            return store[token][0][0], side
    return None, "unresolved"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    cpp_defs, bindbases = scan_cpp(root)
    glsl_defs, blocks = scan_glsl(root)

    fails, warns = [], []

    # --- FAIL 1: shared C++<->GLSL named define mismatch --------------------
    shared = set(cpp_defs) & set(glsl_defs)
    for name in sorted(shared):
        cv = cpp_defs[name][0][0]
        gv = glsl_defs[name][0][0]
        if cv != gv:
            fails.append(
                f"named-pair mismatch: {name} = {cv} (C++ {cpp_defs[name][0][1]}:"
                f"{cpp_defs[name][0][2]}) vs {gv} (GLSL {glsl_defs[name][0][1]}:"
                f"{glsl_defs[name][0][2]})"
            )

    # Known C++-const <-> GLSL-literal pairs that are NOT shared #defines but
    # must stay in lockstep by convention. Value comes from C++; GLSL uses a bare
    # literal. We assert the literal is present at the expected value in the named
    # shader; absence/mismatch is a FAIL.
    KNOWN_LITERAL_PAIRS = [
        # (cpp_const_name, glsl_file_substr, role)
        ("kViewUniformsBinding", "view_uniforms.hglsl", "ViewUniforms UBO"),
    ]
    for cname, gfile, role in KNOWN_LITERAL_PAIRS:
        if cname not in cpp_defs:
            warns.append(f"known-pair: C++ const {cname} not found (skipped {role})")
            continue
        cval = cpp_defs[cname][0][0]
        hit = [b for b in blocks if gfile in b["file"]]
        resolved = []
        for b in hit:
            v, _ = resolve_token(b["binding_token"], glsl_defs, cpp_defs)
            if v is not None:
                resolved.append(v)
        if hit and cval not in resolved:
            fails.append(
                f"known-pair mismatch: {cname}={cval} but {gfile} binds "
                f"{sorted(set(resolved))} (role {role})"
            )

    # --- FAIL 2: same-file slot collision (two distinct blocks, one binding) -
    by_file = {}
    for b in blocks:
        by_file.setdefault(b["file"], []).append(b)
    for f, bs in sorted(by_file.items()):
        seen = {}  # (namespace, slot) -> list of block dicts
        for b in bs:
            v, _ = resolve_token(b["binding_token"], glsl_defs, cpp_defs)
            if v is None:
                continue
            ns = "UBO" if b["kind"] == "uniform" else "SSBO"
            key = (ns, v)
            for prev in seen.get(key, []):
                if prev["block"] == b["block"]:
                    continue  # same block re-declared (e.g. include) — fine
                if mutually_exclusive(prev["branch"], b["branch"]):
                    warns.append(
                        f"mode-alternate (intentional): {f} {ns} slot {v} = "
                        f"'{prev['block']}' / '{b['block']}' on exclusive "
                        f"#if branches"
                    )
                else:
                    fails.append(
                        f"same-pass collision in {f}: {ns} slot {v} bound by "
                        f"'{prev['block']}' and '{b['block']}' (not on exclusive "
                        f"branches)"
                    )
            seen.setdefault(key, []).append(b)

    # --- WARN: bare literals + unresolved tokens ----------------------------
    bare = 0
    for b in blocks:
        v, how = resolve_token(b["binding_token"], glsl_defs, cpp_defs)
        if how == "literal":
            bare += 1
        elif how == "unresolved":
            warns.append(
                f"unresolved binding token '{b['binding_token']}' in "
                f"{b['file']}:{b['line']} ({b['block']})"
            )
    if bare:
        warns.append(
            f"{bare} GLSL block bindings are bare numeric literals not tied to a "
            f"named constant (lockstep-by-hand surface)"
        )

    # --- occupancy: slot -> usages (cross-pass reuse is informational) ------
    occ = {}
    for b in blocks:
        v, _ = resolve_token(b["binding_token"], glsl_defs, cpp_defs)
        if v is None:
            continue
        ns = "UBO" if b["kind"] == "uniform" else "SSBO"
        occ.setdefault(f"{ns}:{v}", []).append(
            {"side": "glsl", "file": b["file"], "line": b["line"],
             "role": b["block"], "std": b["std"]}
        )
    for tgt, v, rel, ln in bindbases:
        ns = "UBO" if tgt == "GL_UNIFORM_BUFFER" else "SSBO"
        occ.setdefault(f"{ns}:{v}", []).append(
            {"side": "cpp", "file": rel, "line": ln, "role": "glBindBufferBase(literal)"}
        )
    for slot, uses in occ.items():
        files = {u["file"] for u in uses if u["side"] == "glsl"}
        if len(files) > 1:
            warns.append(f"cross-pass reuse: {slot} bound in {len(files)} shader files (expected — multiplexed)")

    report = {
        "summary": {
            "cpp_binding_constants": len(cpp_defs),
            "glsl_blocks_with_binding": len(blocks),
            "shared_named_pairs": sorted(shared),
            "fails": len(fails),
            "warns": len(warns),
        },
        "fails": fails,
        "warns": warns,
        "occupancy": {k: occ[k] for k in sorted(occ, key=lambda s: (s.split(":")[0], int(s.split(":")[1])))},
    }

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-binding-slots] GPU-BINDING-SLOTS-LOCKSTEP-1")
        print(f"  C++ binding constants : {len(cpp_defs)}")
        print(f"  GLSL bound blocks     : {len(blocks)}")
        print(f"  shared named pairs    : {', '.join(sorted(shared)) or '(none)'}")
        for (ns, slot), note in sorted(RESOLVED_COLLISIONS.items()):
            print(f"  RESOLVED: {ns}:{slot} — {note}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
