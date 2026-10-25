#!/usr/bin/env python3
"""check-light-loop-numlights-bound.py — LIGHT-CLAMP-RAISE-STAGE1-1 invariant.

Protects the single load-bearing invariant that makes raising the runtime
per-object light clamp (16 -> 32, kRuntimeLightClamp in mclib/txmmgr.cpp) SAFE:

    Every per-light loop in the lighting CONSUMER shaders must be bounded by the
    POPULATED count `min(<ld>.numLights.x, MAX_LIGHTS_IN_WORLD)`, NEVER by the
    declared ABI cap MAX_LIGHTS_IN_WORLD alone.

If a consumer loop iterates the fixed cap (0..MAX_LIGHTS_IN_WORLD) regardless of
numLights.x, then raising the clamp would let it (a) read uninitialised slots
16..31 and (b) inflate per-fragment cost to a flat 32 lights everywhere. Both
are corruption / perf regressions. The clamp raise is only safe because these
loops follow numLights.x. This checker is the tripwire for "someone rewrites a
loop to use the bare cap".

SCOPE (native per-object lighting consumers):
  shaders/vulkan/mech_tier1.vert        -- calc_light
  shaders/vulkan/color_prop_tier1.vert  -- prop ambient/sun scan

Both consume ObjectLights records and must respect the populated count.

Logic per file:
  - Find every `for (...)` loop.
  - Determine its bound token (the comparison RHS in the loop condition).
  - A loop is COMPLIANT if its bound is a local variable that is assigned from
    `min(... .numLights.x ..., MAX_LIGHTS_IN_WORLD)` earlier in the file.
  - A loop is a VIOLATION if its condition compares directly against
    `MAX_LIGHTS_IN_WORLD` (the bare cap) as the iteration bound.
  - Loops whose bound is unrelated to lights (e.g. shadow cascades) are ignored:
    we only flag loops that index a light array (light_dir/light_color/
    light_falloff/light_to_world) or whose bound is MAX_LIGHTS_IN_WORLD.

Exit 0 if all light loops are numLights-bound; nonzero + diagnostics otherwise.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CONSUMERS = [
    # Same ObjectLights record, same slot range, same failure
    # mode. Correctly bounded on entry; in scope so a rewrite cannot go quiet.
    "shaders/vulkan/mech_tier1.vert",
    "shaders/vulkan/color_prop_tier1.vert",
]

LIGHT_ARRAY_RE = re.compile(r"light_(dir|color|falloff|to_world)\s*\[")
# `min( <something>.numLights.x , MAX_LIGHTS_IN_WORLD )`
NUMLIGHTS_MIN_RE = re.compile(
    r"min\s*\([^)]*\.numLights\.x[^)]*\bMAX_LIGHTS_IN_WORLD\b[^)]*\)")
FOR_RE = re.compile(r"\bfor\s*\(([^;]*);([^;]*);([^)]*)\)")


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def compliant_bound_vars(text):
    """Local vars assigned from a numLights-bound min(...): name -> True."""
    good = {}
    # e.g.  int n = min(ld.numLights.x, MAX_LIGHTS_IN_WORLD);
    for m in re.finditer(r"\b(?:int|uint)\s+(\w+)\s*=\s*([^;]+);", text):
        name, rhs = m.group(1), m.group(2)
        if NUMLIGHTS_MIN_RE.search(rhs):
            good[name] = True
    return good


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def check_file(rel):
    problems = []
    try:
        text = read(rel)
    except OSError as e:
        return ["%s: cannot read (%s)" % (rel, e)], 0

    good_vars = compliant_bound_vars(text)
    loops_checked = 0

    for m in FOR_RE.finditer(text):
        cond = m.group(2)
        ln = line_of(text, m.start())

        # Bare cap used directly as the loop bound -> VIOLATION.
        if "MAX_LIGHTS_IN_WORLD" in cond:
            loops_checked += 1
            problems.append(
                "%s:%d VIOLATION: for-loop bound uses bare MAX_LIGHTS_IN_WORLD "
                "(cap) instead of min(numLights.x, MAX_LIGHTS_IN_WORLD): for(%s;%s;%s)"
                % (rel, ln, m.group(1).strip(), cond.strip(), m.group(3).strip()))
            continue

        # Comparison RHS bound variable.
        cmp = re.search(r"[<!]=?\s*([A-Za-z_]\w*)", cond)
        if not cmp:
            continue
        bound = cmp.group(1)

        # Only scrutinise loops that look light-related: either the bound is a
        # known numLights-min var, or the loop body indexes a light array.
        body_start = m.end()
        body = text[body_start:body_start + 600]
        indexes_lights = bool(LIGHT_ARRAY_RE.search(body))

        if bound in good_vars:
            loops_checked += 1  # compliant
            continue

        if indexes_lights:
            loops_checked += 1
            problems.append(
                "%s:%d VIOLATION: for-loop indexes a light array but bound '%s' is "
                "not derived from min(numLights.x, MAX_LIGHTS_IN_WORLD): for(%s;%s;%s)"
                % (rel, ln, bound, m.group(1).strip(), cond.strip(),
                   m.group(3).strip()))

    return problems, loops_checked


def main():
    quiet = "--quiet" in sys.argv or "-q" in sys.argv
    all_problems = []
    total_loops = 0
    for rel in CONSUMERS:
        probs, n = check_file(rel)
        all_problems.extend(probs)
        total_loops += n
        if not quiet:
            status = "FAIL" if probs else "ok"
            print("  %-40s light-loops=%d  %s" % (rel, n, status))

    if not quiet:
        print("light-loop numLights-bound check "
              "(froxel lightgrid_build.comp excluded by design):")

    if all_problems:
        print("light-loop numLights-bound: FAIL", file=sys.stderr)
        for p in all_problems:
            print("  " + p, file=sys.stderr)
        return 1

    if not quiet:
        print("light-loop numLights-bound: PASS (%d light loops, all numLights-bound)"
              % total_loops)
    return 0


if __name__ == "__main__":
    sys.exit(main())
