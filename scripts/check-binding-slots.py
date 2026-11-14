#!/usr/bin/env python3
"""Vulkan GLSL descriptor census and explicit native C++/GLSL lockstep.

Expand local includes per stage; compare four live mech buffer layout records with
shader set/binding/type. Reject same-stage (set, binding) collisions across ALL
descriptor types. Cross-stage checks belong to check-vulkan-bindings.py.
Slang layouts are covered by separate contracts. See binding-slot-occupancy.md.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

from importlib import import_module
strip_comments = import_module("check-lighting-divergence").strip_comments

GLSL_DIR = "shaders/vulkan"
GLSL_EXT = (".vert", ".frag", ".comp", ".tesc", ".tese", ".geom")

# GLSL forms ------------------------------------------------------------------
# layout(... binding = N ...) [readonly|writeonly|coherent|restrict|volatile]* (buffer|uniform) Block {
# value may be a number OR a macro token.
RE_GLSL_BLOCK = re.compile(
    r"layout\s*\(([^)]*)\)\s*"
    r"(?:(?:readonly|writeonly|coherent|restrict|volatile|highp|mediump|lowp)\s+)*"
    r"(buffer|uniform)\s+"
    r"(?:(?:readonly|writeonly|coherent|restrict|volatile|highp|mediump|lowp)\s+)*"
    r"([A-Za-z_]\w*)\s*(?:\{|([A-Za-z_]\w*)\s*(?:\[[^]]*\])?\s*;)"
)
RE_BINDING_IN_LAYOUT = re.compile(r"\bbinding\s*=\s*([A-Za-z_]\w*|\d+)")
RE_SET = re.compile(r"\bset\s*=\s*([A-Za-z_]\w*|\d+)")
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
                yield p, rel


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


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


def shader_lines(path, seen=None):
    """Expand includes with original file/line provenance."""
    seen = set() if seen is None else seen
    path = Path(path).resolve()
    if path in seen:
        return []
    seen.add(path)
    out = []
    for number, line in enumerate(strip_comments(read(path)).splitlines(), 1):
        inc = re.match(r'^\s*#\s*include\s+"([^"\n]+)"', line)
        if inc:
            out.extend(shader_lines(path.parent / inc[1], seen))
        else:
            out.append((line, path, number))
    seen.remove(path)
    return out


def macro_states(lines):
    """Resolve macros in order; fork unknown conditionals and merge only agreed values."""
    defs, stack, states, active = {}, [], {}, True
    for i, (line, _, _) in enumerate(lines, 1):
        pp = re.match(r'^\s*#\s*(\w+)\s*(.*)', line)
        if pp:
            op, arg = pp.groups()
            arg = strip_comments(arg).strip()
            if op in ('if', 'ifdef', 'ifndef'):
                # Unknown feature conditions retain both branches for the census.
                known = (arg not in defs if op == 'ifndef' else True if op == 'ifdef' and arg in defs
                         else bool(int(arg)) if op == 'if' and arg in ('0', '1') else None)
                stack.append([dict(defs), [], active, known, False])
                active = active and known is not False
            elif op in ('else', 'elif') and stack:
                base, ends, parent, known, _ = stack[-1]
                if active:
                    ends.append(dict(defs))
                defs = dict(base)
                condition = (True if op == 'else' else
                             bool(int(arg)) if arg in ('0', '1') else None)
                stack[-1][4] = op == 'else'
                active = parent and known is not True and condition is not False
                stack[-1][3] = (True if known is True or condition is True else
                                False if known is False and condition is False else None)
            elif op == 'endif' and stack:
                base, ends, parent, known, has_else = stack.pop()
                if active:
                    ends.append(dict(defs))
                if not has_else and known is not True:
                    ends.append(base)
                defs = {k: v for k, v in ends[0].items() if all(e.get(k) == v for e in ends)} if ends else base
                active = parent
            elif op == 'define' and active:
                parts = arg.split(None, 1)
                name, value = parts[0], parts[1].strip() if len(parts) > 1 else ''
                defs[name] = value
            elif op == 'undef' and active:
                defs.pop(arg.strip(), None)
        states[i] = dict(defs) if active else None
    return states


def macro_value(token, defs):
    seen = set()
    while token in defs and token not in seen:
        seen.add(token)
        token = defs[token]
    return int(token) if token.isdigit() else None


def scan_glsl(root):
    """Census live Vulkan stages, including their buffer, sampler and image declarations."""
    defines, blocks = {}, []
    for p, rel in walk(root, [GLSL_DIR], GLSL_EXT):
        if not rel.endswith((".vert", ".frag", ".comp", ".tesc", ".tese", ".geom")):
            continue
        lines = shader_lines(p)
        text = '\n'.join(line for line, _, _ in lines)
        states = macro_states(lines)
        branch_paths = compute_branch_paths(text)
        for m in RE_GLSL_BLOCK.finditer(text):
            layout, kind, block, resource = m.groups()
            bm = RE_BINDING_IN_LAYOUT.search(layout)
            if not bm:
                continue  # push constants, not descriptors
            sm = RE_SET.search(layout)
            line = text.count("\n", 0, m.start()) + 1
            local_defs = states[line]
            if local_defs is None:
                continue
            slot = macro_value(bm.group(1), local_defs)
            descriptor_set = macro_value(sm.group(1) if sm else "0", local_defs)
            if slot is None or descriptor_set is None:
                raise ValueError(f"unresolved Vulkan set/binding in {rel}: {layout}")
            _, source, source_line = lines[line - 1]
            std = RE_STD.search(layout)
            blocks.append({"file": rel, "line": source_line,
                           "source": os.path.relpath(source, root).replace('\\', '/'), "kind": kind,
                           "block": resource or block, "type": block if resource else kind,
                           "binding_token": str(slot), "set": descriptor_set,
                           "std": ("std" + std.group(1)) if std else None,
                           "branch": branch_paths.get(line, {})})
    return defines, blocks


def native_lockstep(root, blocks):
    """Compare four live mech buffers to the actual C++ layout assignments.

    Array indices identify C++ layout records, NOT binding numbers. The set is
    read from the pipeline's pSetLayouts array. Missing source/pairs fail closed.
    Other native layouts are outside this explicit lockstep surface.
    """
    source = strip_comments(read(os.path.join(root, "GameOS/gameos/vulkan_mech_island.cpp")))
    layout = re.search(r'const VkDescriptorSetLayout layouts\[\]\s*\{([^}]+)\}', source)
    sets = [item.strip() for item in layout.group(1).split(",")] if layout else []
    descriptor_set = sets.index("s->dsl.get()") if "s->dsl.get()" in sets else None
    pairs = {0: "MechFrameUBO", 1: "InstanceBuffer", 2: "BoneBuffer", 3: "LightsData"}
    fails = []
    for index, role in pairs.items():
        match = re.search(rf'\bb\[{index}\]\.binding\s*=\s*(\d+)\s*;\s*'
                          rf'b\[{index}\]\.descriptorType\s*=\s*VK_DESCRIPTOR_TYPE_(\w+)', source)
        sites = [b for b in blocks if b["file"] in
                 ("shaders/vulkan/mech_tier1.vert", "shaders/vulkan/mech_tier1.frag")
                 and b["block"] == role]
        expected_type = {"UNIFORM_BUFFER_DYNAMIC": "uniform",
                         "STORAGE_BUFFER_DYNAMIC": "buffer",
                         "COMBINED_IMAGE_SAMPLER": "sampler2D"}.get(match[2]) if match else None
        if (not match or descriptor_set is None or not sites or
                any((b["set"], int(b["binding_token"]), b["type"]) !=
                    (descriptor_set, int(match[1]), expected_type) for b in sites)):
            fails.append(f"native-pair mismatch: mech {role} vs C++ b[{index}] layout set/binding/type")
    return fails, len(pairs)


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

    glsl_defs, blocks = scan_glsl(root)

    fails, pair_count = native_lockstep(root, blocks)
    warns = []
    if not blocks:
        fails.append("Vulkan descriptor census is empty")

    # --- Same-stage check: same-file slot collision (two distinct blocks, one binding) -
    by_file = {}
    for b in blocks:
        by_file.setdefault(b["file"], []).append(b)
    for f, bs in sorted(by_file.items()):
        seen = {}  # (set, binding) -> list of descriptor declarations
        for b in bs:
            v, _ = resolve_token(b["binding_token"], glsl_defs, {})
            key = (b["set"], v)
            for prev in seen.get(key, []):
                if (prev["block"], prev["type"]) == (b["block"], b["type"]):
                    continue  # same block re-declared (e.g. include) — fine
                if mutually_exclusive(prev["branch"], b["branch"]):
                    warns.append(
                        f"mode-alternate (intentional): {f} set {b['set']} binding {v} = "
                        f"'{prev['block']}' / '{b['block']}' on exclusive "
                        f"#if branches"
                    )
                else:
                    fails.append(
                        f"same-pass collision in {f}: set {b['set']} binding {v} bound by "
                        f"'{prev['block']}' and '{b['block']}' (not on exclusive "
                        f"branches)"
                    )
            seen.setdefault(key, []).append(b)

    # Descriptor occupancy, keyed by Vulkan set and binding.
    occ = {}
    for b in blocks:
        v, _ = resolve_token(b["binding_token"], glsl_defs, {})
        occ.setdefault(f"set{b['set']}:binding{v}", []).append(
            {"side": "glsl", "file": b["file"], "line": b["line"],
             "role": b["block"], "std": b["std"], "type": b["type"],
             "source": b["source"], "branch": b["branch"]}
        )
    report = {
        "summary": {
            "glsl_blocks_with_binding": len(blocks),
            "native_pairs": pair_count,
            "fails": len(fails),
            "warns": len(warns),
        },
        "fails": fails,
        "warns": warns,
        "occupancy": {k: occ[k] for k in sorted(occ)},
        "stages": sorted(rel for _, rel in walk(root, [GLSL_DIR], GLSL_EXT)),
    }

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-binding-slots] GPU-BINDING-SLOTS-LOCKSTEP-1")
        print(f"  native layout pairs    : {pair_count}")
        print(f"  GLSL bound blocks     : {len(blocks)}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
