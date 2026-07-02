#!/usr/bin/env python3
"""check-gpu-buffer-owners.py -- GPU-BUFFER-OWNER-CI-GATE-1 static invariant gate.

Promotes the offline GpuBufferOwnerLifetimeCheck doctest (tests/unit/
test_gpu_buffer_owner.cpp) to a durable CI gate so a future code change can't
silently regress the completed GPU-buffer-owner arc. The doctest proves the
RUNTIME invariant (owners register with a non-Unset lifetime); this gate proves
the STATIC source invariant on every CI pass, no build required.

Every `RenderCore::GpuBufferOwner s_foo{ id, lifetime, name, backendHandle }`
construction site (in GameOS/gameos/*.cpp + RenderCore/*.cpp) is parsed and the
4 positional initializer args are checked:

  1. ID REGISTERED  -- the RenderResourceId arg is a real enumerator (appears in
     `enum class RenderResourceId` in RenderResourceRegistry.h) AND has a
     toString() case in RenderResourceRegistry.cpp (no orphan id).
  2. CONCRETE LIFETIME -- the RenderResourceLifetime arg is present and is a
     concrete class (Persistent / Mission / FrameLocal / External), NOT a
     defaulted/Unset one. An owner constructed with ::Unset (or a missing
     lifetime arg) FAILs -- exactly the regression this gate blocks.
  3. NAME MATCHES ID -- the debugName string literal equals toString(id), so the
     registry's human label can't drift from the logical id.

Additionally asserts the runtime doctest suite still EXISTS so the runtime arm
of the invariant stays covered:

  4. DOCTEST PRESENT -- TEST_SUITE("GpuBufferOwnerLifetimeCheck") is present in
     tests/unit/test_gpu_buffer_owner.cpp.

Pure stdlib, grep/parse only. Exit 0 = all owners clean; nonzero + message on
any violation.
"""
import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REGISTRY_H   = os.path.join(REPO_ROOT, "RenderCore", "RenderResourceRegistry.h")
REGISTRY_CPP = os.path.join(REPO_ROOT, "RenderCore", "RenderResourceRegistry.cpp")
DOCTEST      = os.path.join(REPO_ROOT, "tests", "unit", "test_gpu_buffer_owner.cpp")

# TUs that may host an owner construction site.
SCAN_DIRS = [
    os.path.join(REPO_ROOT, "GameOS", "gameos"),
    os.path.join(REPO_ROOT, "RenderCore"),
]

CONCRETE_LIFETIMES = {"FrameLocal", "Mission", "Persistent", "External"}

# GPU-BUFFER-EXCLUSION-LEDGER-1: valid TIER2 exclusion reasons.
# GPU-BUFFER-LEDGER-STRICT-1: added `legacy-mlr-gated`.
TIER2_REASONS = {"ring", "substrate-gated", "dead-path", "legacy-mlr-gated"}

# GPU-BUFFER-LEDGER-STRICT-1 strict-mode windows/evidence.
# Orphan-tag: a TIER2-EXCLUDED tag must sit within this many lines of a
# glGenBuffers OR an SSBO/UBO glBindBufferBase site.
ORPHAN_TAG_WINDOW = 3
# Ring-tag rationale: a `ring` tag must have persistent-map/fence/RING_FRAMES
# evidence within this many lines of the tag.
RING_EVIDENCE_WINDOW = 6
RING_EVIDENCE = re.compile(
    r"GLsync|glFenceSync|glClientWaitSync|glWaitSync|RING_FRAMES|"
    r"GL_MAP_PERSISTENT_BIT|glMapBufferRange|MapBufferRange")

# dead-path rule-3: a gate predicate is treated as *positively default-ON* only
# when it is one of these well-known always-on-by-default predicates. Anything
# else (unknown gate, or NO gate) is "cannot determine" -> INFO, never a fail,
# to keep the heuristic conservative (per spec). getenv("X")=="0"-to-disable
# style gates are default-ON, but we cannot statically prove the buffer is the
# only thing they guard, so they stay INFO unless whitelisted here.
DEADPATH_DEFAULT_ON_GATES = set()  # intentionally empty until a proven case exists
GATE_CALL = re.compile(
    r"\b(getenv|Is[A-Z]\w*Enabled|IsEnabled|[A-Za-z_]\w*_?[Ee]nabled)\s*\(")

# A glGenBuffers target var name is in-scope for the owned-OR-excluded partition
# iff it looks like an SSBO or UBO (the frame-graph resource kinds we own). VBO /
# IBO / VB / IB / PBO / CmdBuf / generic `local`/`buffer`/`id` handles are out of
# scope by design (per-pass-rebind / vertex-index data, not owned resources).
SSBO_UBO_NAME = re.compile(r"[Ss][Ss][Bb][Oo]|[Uu][Bb][Oo]")

# `glGenBuffers(<n>, &<var>)` -- capture the target var name.
GEN_BUFFERS = re.compile(r"glGenBuffers\s*\(\s*[^,]+,\s*&\s*([A-Za-z_]\w*)\s*\)")

# GPU-BUFFER-GATE-TIGHTEN-1: the DEFINITIVE SSBO/UBO signal is the binding
# TARGET, not the handle's name. Any buffer bound with
#   glBindBufferBase(GL_SHADER_STORAGE_BUFFER, <slot>, <handle>)
#   glBindBufferBase(GL_UNIFORM_BUFFER,        <slot>, <handle>)
# IS an SSBO/UBO regardless of whether its name contains "ssbo"/"ubo". This
# catches *Buffer / *Buf / *Count / *Pool-named handles (water-stream
# g_recipeBuffer/g_thinBuffer, lightgrid s_cursorBuf/s_indexPool/s_sphereCount,
# gpu-cull s_bucketCountsBuf/s_actorVisBuf/..., terrain-surface s_surfaceVB/TB)
# that escape the pure-name heuristic. The set of bound handles SUPERSETS the
# name heuristic; the partition scan unions the two.
BIND_BASE = re.compile(
    r"glBindBufferBase\s*\(\s*GL_(?:SHADER_STORAGE|UNIFORM)_BUFFER\s*,"
    r"\s*[^,]+,\s*(.+?)\)\s*;")

# Peel the wrappers off a bound-handle expression to reach the backing var name:
#   static_cast<GLuint>(x) / (GLuint)x  -> x
#   owner.backendHandle                 -> (owned, resolved separately)
#   0 / 0u / nullptr                    -> unbind, not a handle
# Returns the bare identifier, or None if the expr is a literal/getter-call/field
# access that doesn't name a glGenBuffers target var directly.
CAST_PREFIX = re.compile(r"^\s*(?:static_cast\s*<[^>]+>\s*\(\s*|\(\s*GLuint\s*\)\s*)")
BARE_IDENT  = re.compile(r"^([A-Za-z_]\w*)\s*\)*\s*$")

# `// TIER2-EXCLUDED: <reason>`
TIER2_TAG = re.compile(r"//\s*TIER2-EXCLUDED:\s*([A-Za-z0-9_-]+)")

# `RenderCore::GpuBufferOwner   s_name{` (var-decl construction, not a typedef/include/comment).
OWNER_DECL = re.compile(
    r"(?:RenderCore::)?GpuBufferOwner\s+[A-Za-z_]\w*\s*\{", re.M)


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_enum_ids():
    """Names in `enum class RenderResourceId` (excluding Unknown/Count)."""
    text = read(REGISTRY_H)
    m = re.search(r"enum\s+class\s+RenderResourceId\s*:[^\{]*\{(.*?)\};", text, re.DOTALL)
    if not m:
        return None
    body = re.sub(r"//.*$", "", m.group(1), flags=re.MULTILINE)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    ids = set()
    for raw in body.split(","):
        name = raw.split("=")[0].strip()
        if re.match(r"^[A-Za-z_]\w*$", name):
            ids.add(name)
    return ids


def parse_tostring_cases():
    """{idName: literal} from toString(RenderResourceId) cases."""
    text = read(REGISTRY_CPP)
    out = {}
    for m in re.finditer(
            r"case\s+RenderResourceId::(\w+)\s*:\s*return\s+\"([^\"]*)\"", text):
        out[m.group(1)] = m.group(2)
    return out


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.DOTALL)
    s = re.sub(r"//.*$", "", s, flags=re.MULTILINE)
    return s


def owner_sites():
    """Yield (relpath, lineno, args[]) for each owner construction site.

    args = the 4 positional initializer tokens between the outermost braces:
    [id-expr, lifetime-expr, name-literal-or-expr, backendHandle-expr].
    """
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(d, fn)
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            text = read(path)
            for m in OWNER_DECL.finditer(text):
                brace_open = m.end() - 1  # index of the `{`
                lineno = text.count("\n", 0, m.start()) + 1
                # Find the matching close brace.
                depth = 0
                i = brace_open
                while i < len(text):
                    c = text[i]
                    if c == "{":
                        depth += 1
                    elif c == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                inner = strip_comments(text[brace_open + 1:i])
                # Split on top-level commas (no nesting expected in these PODs).
                args = [a.strip() for a in inner.split(",") if a.strip()]
                yield rel, lineno, args


def owner_aliased_names():
    """SSBO/UBO-named vars that ARE owned even though glGenBuffers writes them.

    Two owner patterns in the tree write the raw handle without a temp var:
      (a) `#define <var> (static_cast<GLuint>(<owner>.backendHandle))` -- <var>
          is a READ-ONLY macro alias for the owner's 64-bit backendHandle field
          (e.g. s_lightDataSsbo, s_dynamicPropShadowSsbo). The bare
          `(<owner>.backendHandle)` form (pre-L6.1-widen) is also accepted.
      (b) `<owner>.backendHandle = <var>` near a glGenBuffers(&<var>) -- the freshly
          generated name is stored straight into an owner (e.g. view_uniforms
          `ubo`). Detected per-site in the partition scan, not here.
    Returns the set of macro-aliased names (pattern a).
    """
    aliased = set()
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not (fn.endswith(".cpp") or fn.endswith(".h")):
                continue
            text = read(os.path.join(d, fn))
            for m in re.finditer(
                    r"#define\s+([A-Za-z_]\w*)\s*\(\s*(?:static_cast\s*<\s*GLuint\s*>\s*\(\s*)?"
                    r"[A-Za-z_]\w*\.backendHandle\s*\)?\s*\)",
                    text):
                aliased.add(m.group(1))
    return aliased


def bound_ssbo_ubo_vars():
    """glGenBuffers target vars bound as SSBO/UBO via their binding TARGET.

    Scans SCAN_DIRS for glBindBufferBase(GL_SHADER_STORAGE_BUFFER/GL_UNIFORM_
    BUFFER, slot, <handle>), peels casts/parens, and keeps the bare identifier.
    `owner.backendHandle` field-access and literal `0`/`0u`/`nullptr` unbinds and
    getter calls are dropped -- their backing buffer is named + classified at
    its own glGenBuffers site (or is an owner). The returned set is intersected
    with actual glGenBuffers target vars by the caller, so getter-return locals
    that never appear at a glGenBuffers(&x) site simply don't widen anything.

    Keyed PER FILE (relpath -> set of names): a handle counts as SSBO/UBO only
    where it is bound as one in its OWN TU. This avoids cross-file name-collision
    false positives -- e.g. gameos_graphics's draw-indirect `s_indirectCmdBuf`
    must NOT inherit SSBO-scope from gpu_cull_compute's unrelated same-named
    SSBO global.
    """
    by_file = {}
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(d, fn)
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            text = strip_comments(read(path))
            names = set()
            for m in BIND_BASE.finditer(text):
                expr = m.group(1).strip()
                expr = CAST_PREFIX.sub("", expr)  # drop one cast/paren wrapper
                bm = BARE_IDENT.match(expr)
                if bm:
                    names.add(bm.group(1))
            by_file[rel] = names
    return by_file


def check_partition(aliased, bound):
    """Every SSBO/UBO glGenBuffers target is owned OR // TIER2-EXCLUDED.

    In-scope = name-heuristic (SSBO_UBO_NAME) UNION binding-target (var appears
    as a glBindBufferBase(GL_SHADER_STORAGE/UNIFORM_BUFFER,...) handle). The
    union closes the *Buffer/*Buf/*Count/*Pool name-escape hole.

    Returns (failures, n_owned, n_excluded, n_bind_caught).
    n_bind_caught = in-scope-ONLY-because-of-binding-target (not name-matched).
    """
    failures = []
    n_owned = 0
    n_excluded = 0
    n_bind_caught = 0
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(d, fn)
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            bound_here = bound.get(rel, set())
            lines = read(path).splitlines()
            for i, line in enumerate(lines):
                gm = GEN_BUFFERS.search(line)
                if not gm:
                    continue
                var = gm.group(1)
                name_hit = bool(SSBO_UBO_NAME.search(var))
                bind_hit = var in bound_here
                if not name_hit and not bind_hit:
                    continue  # VBO/IBO/generic, never SSBO/UBO-bound -- oo scope.
                if bind_hit and not name_hit:
                    n_bind_caught += 1  # closed the name-escape hole.
                where = "%s:%d %s" % (rel, i + 1, var)

                # Tag on this line or the 2 above it (used by ownership AND
                # exclusion; a tag on an owned site is a strict CONTRADICTION).
                tag = None
                for j in range(max(0, i - 2), i + 1):
                    tm = TIER2_TAG.search(lines[j])
                    if tm:
                        tag = tm.group(1)
                        break

                # OWNED via macro alias (#define var (cast)(owner.backendHandle)) OR
                # `<owner>.backendHandle = <var>` within a few lines (temp-assigned).
                lookahead = "\n".join(lines[i:i + 4])
                owned = var in aliased or bool(re.search(
                    r"\.backendHandle\s*=\s*(?:static_cast<[^>]+>\s*\(\s*)?"
                    + re.escape(var) + r"\b", lookahead))
                if owned:
                    # STRICT rule 1: owned + excluded is a contradiction.
                    if tag is not None:
                        failures.append(
                            "%s -- OWNED (GpuBufferOwner) AND carries a "
                            "// TIER2-EXCLUDED:%s tag (contradiction: a buffer "
                            "is owned XOR excluded, never both)" % (where, tag))
                    else:
                        n_owned += 1
                    continue

                # EXCLUDED via a // TIER2-EXCLUDED tag.
                if tag is None:
                    failures.append(
                        "%s -- un-owned SSBO/UBO with NO // TIER2-EXCLUDED tag "
                        "(must be a GpuBufferOwner OR tagged ring/"
                        "substrate-gated/dead-path/legacy-mlr-gated)" % where)
                    continue
                if tag not in TIER2_REASONS:
                    failures.append(
                        "%s -- // TIER2-EXCLUDED reason %r not in %s"
                        % (where, tag, sorted(TIER2_REASONS)))
                    continue

                # STRICT rule 4: `ring` tag needs fence/persistent-map/
                # RING_FRAMES evidence near the site.
                if tag == "ring":
                    lo = max(0, i - RING_EVIDENCE_WINDOW)
                    hi = min(len(lines), i + RING_EVIDENCE_WINDOW + 1)
                    if not any(RING_EVIDENCE.search(lines[k])
                               for k in range(lo, hi)):
                        failures.append(
                            "%s -- // TIER2-EXCLUDED:ring but NO ring rationale "
                            "(GLsync/fence/RING_FRAMES/persistent-map) within "
                            "%d lines" % (where, RING_EVIDENCE_WINDOW))
                        continue

                # STRICT rule 3 (conservative): `dead-path` tag whose nearest
                # enclosing gate is *provably* default-ON. We only fire on a
                # positively-known default-ON predicate; unknown / no gate is
                # "cannot determine" -> INFO (never fail), to avoid false
                # positives (spec).
                if tag == "dead-path":
                    lo = max(0, i - 12)
                    gate_on = False
                    for k in range(i, lo - 1, -1):
                        gm2 = GATE_CALL.search(lines[k])
                        if not gm2:
                            continue
                        callee = re.search(r"([A-Za-z_]\w*)\s*\(",
                                           lines[k][gm2.start():])
                        if callee and callee.group(1) in DEADPATH_DEFAULT_ON_GATES:
                            gate_on = True
                        break  # nearest gate only
                    if gate_on:
                        failures.append(
                            "%s -- // TIER2-EXCLUDED:dead-path but nearest gate "
                            "is default-ON (path is reachable; a real dead-path "
                            "must be provably gated-OFF or retired)" % where)
                        continue

                n_excluded += 1
    return failures, n_owned, n_excluded, n_bind_caught


def check_orphan_tags():
    """STRICT rule 2: every // TIER2-EXCLUDED tag must sit within
    ORPHAN_TAG_WINDOW lines of a glGenBuffers OR an SSBO/UBO glBindBufferBase
    site. A tag matching nothing is an orphan (stale/misplaced) -> fail.
    """
    failures = []
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(d, fn)
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            lines = read(path).splitlines()
            for i, line in enumerate(lines):
                if not TIER2_TAG.search(line):
                    continue
                lo = max(0, i - ORPHAN_TAG_WINDOW)
                hi = min(len(lines), i + ORPHAN_TAG_WINDOW + 1)
                near = any(
                    GEN_BUFFERS.search(lines[k]) or BIND_BASE.search(lines[k])
                    for k in range(lo, hi))
                if not near:
                    failures.append(
                        "%s:%d -- // TIER2-EXCLUDED tag with NO glGenBuffers or "
                        "SSBO/UBO glBindBufferBase site within %d lines "
                        "(orphan tag)" % (rel, i + 1, ORPHAN_TAG_WINDOW))
    return failures


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress OK chatter; offenders still printed")
    args = ap.parse_args()

    for p in (REGISTRY_H, REGISTRY_CPP, DOCTEST):
        if not os.path.exists(p):
            print("[check-gpu-buffer-owners] ERROR: missing %s"
                  % os.path.relpath(p, REPO_ROOT), file=sys.stderr)
            return 2

    enum_ids = parse_enum_ids()
    if not enum_ids:
        print("[check-gpu-buffer-owners] ERROR: could not parse "
              "enum class RenderResourceId", file=sys.stderr)
        return 2
    tostring = parse_tostring_cases()

    failures = []
    sites = list(owner_sites())
    if not sites:
        print("[check-gpu-buffer-owners] ERROR: parsed 0 GpuBufferOwner "
              "construction sites (parser broke?)", file=sys.stderr)
        return 2

    ok = []
    for rel, lineno, av in sites:
        where = "%s:%d" % (rel, lineno)
        if len(av) < 3:
            failures.append("%s -- only %d initializer args (need id, "
                            "lifetime, name, ...)" % (where, len(av)))
            continue

        id_arg, life_arg, name_arg = av[0], av[1], av[2]

        # 1. id registered (enum + toString case).
        idm = re.search(r"RenderResourceId::(\w+)", id_arg)
        if not idm:
            failures.append("%s -- arg0 is not a RenderResourceId:: enumerator: %r"
                            % (where, id_arg))
            continue
        idname = idm.group(1)
        if idname not in enum_ids:
            failures.append("%s -- RenderResourceId::%s not in enum (orphan id)"
                            % (where, idname))
        if idname not in tostring:
            failures.append("%s -- RenderResourceId::%s has no toString() case "
                            "(orphan id)" % (where, idname))

        # 2. concrete, non-Unset lifetime.
        lifem = re.search(r"RenderResourceLifetime::(\w+)", life_arg)
        if not lifem:
            failures.append("%s -- arg1 is not a RenderResourceLifetime:: "
                            "value: %r" % (where, life_arg))
        else:
            life = lifem.group(1)
            if life == "Unset":
                failures.append("%s -- owner %s constructed with Unset lifetime "
                                "(defaulted/regression)" % (where, idname))
            elif life not in CONCRETE_LIFETIMES:
                failures.append("%s -- unknown lifetime %s" % (where, life))

        # 3. debugName literal matches toString(id).
        nm = re.match(r'^\s*"([^"]*)"\s*$', name_arg)
        if not nm:
            failures.append("%s -- arg2 debugName is not a string literal: %r"
                            % (where, name_arg))
        elif idname in tostring and nm.group(1) != tostring[idname]:
            failures.append("%s -- debugName %r != toString(%s)=%r"
                            % (where, nm.group(1), idname, tostring[idname]))

        if not any(f.startswith(where) for f in failures):
            ok.append((where, idname,
                       lifem.group(1) if lifem else "?"))

    # 4. runtime doctest suite still present.
    if 'TEST_SUITE("GpuBufferOwnerLifetimeCheck")' not in read(DOCTEST):
        failures.append("tests/unit/test_gpu_buffer_owner.cpp -- runtime suite "
                        'TEST_SUITE("GpuBufferOwnerLifetimeCheck") missing '
                        "(runtime invariant arm gone)")

    # GPU-BUFFER-EXCLUSION-LEDGER-1: owned-OR-excluded partition over every
    # SSBO/UBO-named glGenBuffers target.
    part_failures, n_owned_gen, n_excluded, n_bind_caught = check_partition(
        owner_aliased_names(), bound_ssbo_ubo_vars())
    failures.extend(part_failures)

    # GPU-BUFFER-LEDGER-STRICT-1 rule 2: orphan tags.
    failures.extend(check_orphan_tags())

    if failures:
        print("[check-gpu-buffer-owners] FAIL:", file=sys.stderr)
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        return 1

    if not args.quiet:
        print("[check-gpu-buffer-owners] %d GpuBufferOwner construction site(s):"
              % len(sites))
        for where, idname, life in ok:
            print("  OK  %-30s %-12s %s" % (idname, life, where))
        print("[check-gpu-buffer-owners] SSBO/UBO partition: %d owned "
              "(GpuBufferOwner), %d excluded-by-tag, %d caught-by-binding-target "
              "(not name-matched), 0 unclassified."
              % (n_owned_gen, n_excluded, n_bind_caught))
        print("[check-gpu-buffer-owners] PASS -- all owners use a registered id "
              "+ concrete non-Unset lifetime + matching name; every SSBO/UBO "
              "(by name OR glBindBufferBase target) glGenBuffers is owned OR "
              "// TIER2-EXCLUDED; runtime doctest present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
