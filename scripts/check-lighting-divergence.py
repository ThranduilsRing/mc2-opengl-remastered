#!/usr/bin/env python3
"""check-lighting-divergence.py -- LIGHTING-DIVERGENCE-PIN-1.

WHAT THIS PINS, AND WHY IT EXISTS
---------------------------------
MC2 has no single shading model. Each renderable class reached its lighting by
a different route and at a different time, so the engine currently carries
FIVE named scalar ambient floors, THREE hemisphere sky/ground fill pairs, FIVE
distinct unnamed inline floors, TWO definitions of the "shared" calc_light
kernel, and TWO answers to "what does calc_light return when the actor has no
lights".

Almost every one of those is a DELIBERATE, documented decision today (see
docs/lighting-divergence-map.md, which records which are and which are not).
None of them is discoverable from any single file. That is the drift hazard
this check closes: the next well-meaning edit that adds a sixth ambient floor,
teaches another shader to hand-roll its own light loop, or changes 0.35 to 0.30
in one place out of three, currently lands silently and is only found by eye,
months later, as "why is the truck darker than the mech".

This check does NOT argue that the divergence should be removed. It pins it.
A pinned divergence is a decision; an unpinned one is drift.

HOW IT AVOIDS GOING BLIND
-------------------------
A source-scanning check that quietly stops matching is worse than no check:
it reports green forever. Three defences, all of which fail RED:

  1. COMMENTS ARE STRIPPED before scanning. Half this codebase's lighting
     constants appear in prose comments quoting other files -- e.g.
     shaders/gos_tex_vertex_lighted.vert quotes mech.vert's
     `const float kAmbientFloor = 0.35;` inside a comment block at :72 while
     declaring its own at :169. A naive grep sees two floors in that one file.

  2. A LIVE IN-TREE DECOY WITNESS. That same file is asserted to yield
     EXACTLY ONE ambient-floor site. If the comment stripper is ever removed
     or broken, that assertion fails immediately. It is not a fixture: it is
     the shipping shader, so it cannot rot into agreement with a broken
     scanner.

  3. EMPTY-CATEGORY AND MISSING-FILE GUARDS. If any scanned category comes
     back empty, or any file named in the baseline has vanished, that is
     reported as a failure rather than as "no differences found".

USAGE
  py -3 scripts/check-lighting-divergence.py            # PASS/FAIL table
  py -3 scripts/check-lighting-divergence.py --verbose  # list every site
  py -3 scripts/check-lighting-divergence.py --print-live   # emit live scan
                                                            # (to hand-review
                                                            #  before pinning)

EXIT  0 = live tree matches the pinned baseline
      1 = divergence added, removed, or changed value  (ACTION REQUIRED)
      2 = the check itself could not run (missing tree / blind scanner)

WHEN A FAILURE IS LEGITIMATE
  Update BASELINE below IN THE SAME COMMIT as the shader/engine change, and
  add the new row to docs/lighting-divergence-map.md with its INTENTIONAL /
  ACCIDENTAL call. The baseline lives in this file on purpose: silently adding
  a lighting floor then has to show up as a diff to the guard that forbids it.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Comment stripping (C / C++ / GLSL). Preserves line count so citations stay
# accurate, and does not treat "//" inside a string literal as a comment.
# ---------------------------------------------------------------------------
_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"' r"|'(?:\\.|[^'\\])*'" r"|/\*.*?\*/" r"|//[^\n]*", re.S)


def strip_comments(text: str) -> str:
    def repl(m: re.Match) -> str:
        tok = m.group(0)
        if tok.startswith("/*"):
            return "\n" * tok.count("\n")
        if tok.startswith("//"):
            return ""
        return tok  # string literal: keep

    return _TOKEN.sub(repl, text)


def read_stripped(rel: str):
    p = ROOT / rel
    if not p.is_file():
        return None
    return strip_comments(p.read_text(encoding="utf-8", errors="replace"))


def numbered(stripped: str):
    return list(enumerate(stripped.splitlines(), start=1))


# ---------------------------------------------------------------------------
# Scan surfaces. Directory sweeps, NOT fixed file lists -- a brand new shader
# that introduces a brand new ambient floor must be caught, and it would not
# be if the scanner only ever looked where floors already exist.
# ---------------------------------------------------------------------------
SHADER_GLOBS = ("shaders/**/*.vert", "shaders/**/*.frag", "shaders/**/*.hglsl",
                "shaders/**/*.comp", "shaders/**/*.glsl", "shaders/**/*.tese",
                "shaders/**/*.tesc", "shaders/**/*.geom")

# Directories excluded from the shader sweep: contract FIXTURES exist to be
# compiled, not to ship pixels.
SHADER_EXCLUDE = ("shaders/fixtures/",)


def shader_files():
    seen = {}
    for g in SHADER_GLOBS:
        for p in ROOT.glob(g):
            rel = p.relative_to(ROOT).as_posix()
            if any(rel.startswith(x) for x in SHADER_EXCLUDE):
                continue
            seen[rel] = p
    return dict(sorted(seen.items()))


# --- Category A: scalar ambient floors declared in shader code --------------
RE_SCALAR_FLOOR = re.compile(
    r"\bconst\s+float\s+(\w*(?:[Aa]mbient|AMBIENT)\w*)\s*=\s*([0-9]*\.?[0-9]+)\s*;")
# The same floor sourced from the shared environment UBO (set once by the CPU producer).
RE_SCALAR_FLOOR_UBO = re.compile(
    r"#define\s+(\w*(?:[Aa]mbient|AMBIENT)\w*)\s+(uEnv\.\w+\.\w)\b")

# --- Category B: hemisphere / directional ambient FILL colours --------------
RE_VEC3_FILL = re.compile(
    r"\bvec3\s+(\w*(?:[Aa]mb|AMB|hemi|Hemi)\w*)\s*=\s*vec3\(\s*([^)]*?)\s*\)\s*;")

# --- Category B2: UNNAMED inline light floors -------------------------------
# The named-constant patterns above miss the other half of this codebase's
# floors, which are written inline with no identifier at all:
#     (0.60 + 0.38 * NdotL)     gos_vegetation_card   -- diffuse floor 0.60
#     mix(0.4, 1.0, shadow)     shadow.hglsl          -- shadow darkness floor
#     mix(0.55, 1.0, shadow)    terrain_lod_chunk     -- ambient occlusion floor
# These ARE ambient floors -- they set how dark a fully-unlit or fully-shadowed
# fragment of that class is allowed to get -- and today they carry three
# different values across five classes. Deliberately narrow: anchored on the
# NdotL / shadow term so it cannot drag in unrelated mix() calls.
RE_INLINE_MIX = re.compile(
    r"mix\(\s*([0-9]*\.?[0-9]+)\s*,\s*([0-9]*\.?[0-9]+)\s*,\s*(?:clamp\()?\s*"
    r"(NdotL|ndl|ndotl|nDotL|NdL|shadow|moverShadow)\b")
RE_INLINE_ADD = re.compile(
    r"\(\s*([0-9]*\.?[0-9]+)\s*\+\s*([0-9]*\.?[0-9]+)\s*\*\s*"
    r"(NdotL|ndl|ndotl|nDotL|NdL|ao|AO)\b")

# --- Category C: which TG_LIGHT_* types each shader actually dispatches -----
#
# NAMED CONSTANTS ARE NOT ENOUGH, and assuming they were produced a WRONG map.
# shaders/vulkan/color_prop_tier1.vert:284 dispatches INFINITE and
# INFINITEWITHFALLOFF as bare integers -- `lt == 1 || lt == 2` -- while naming
# only TG_LIGHT_AMBIENT. A named-constants-only scan reports that shader as
# "AMBIENT only", which is false and would have been pinned as false. Fold the
# integer literals in, using the numbering declared at lighting.hglsl:17-22.
RE_LIGHT_TYPE = re.compile(r"\bTG_LIGHT_([A-Z]+)\b")
RE_LIGHT_TYPE_INT = re.compile(r"\b(?:lt|light_type|lightType)\s*==\s*([0-9]+)\b")
LIGHT_TYPE_BY_NUM = {           # lighting.hglsl:17-22
    "0": "AMBIENT", "1": "INFINITE", "2": "INFINITEWITHFALLOFF",
    "3": "POINT", "4": "SPOT", "5": "TERRAIN",
}
# Files that read the light-type field at all. A shader that reads it but names
# no type is either a binning stage (lightgrid_build.comp bins POINT/SPOT
# together as spheres) or a scan that has gone stale -- either way, visible.
RE_LIGHT_TYPE_FIELD = re.compile(r"light_dir\s*\[[^\]]*\]\s*\.w")

# --- Category D: who calls the SHARED light kernel -------------------------
RE_CALC_LIGHT_CALL = re.compile(r"(?<![\w:])(?:mv_)?calc_light\s*\(")
RE_CALC_LIGHT_DEF = re.compile(r"\bvec3\s+(?:mv_)?calc_light\s*\(")

# --- Category E: who forks calc_light's no-lights semantics -----------------
RE_SPL_DEFINE = re.compile(r"if\s*\(\s*ld\.numLights\.x\s*==\s*0\s*\)\s*return\s+base_light\s*;")

# --- Category F: C++ side ambient-floor gate defaults ----------------------
# (file, env var) -> the default literal returned when the var is unset. Read
# FROM THE READER, never from the registry or the docs.
CPP_FLOOR_GATES = (
    ("mclib/camera.cpp", "MC2_NIGHT_AMBIENT_FLOOR"),
)
# A getenv("MC2_X") read or a typed gate::get<"MC2_X">() read: either names the gate the file still reads.
RE_GETENV = re.compile(r'(?:getenv\(\s*|\b(?:get|state|isSet|isNonEmpty|str|is)\s*<\s*)"([A-Z0-9_]+)"')


def scan(verbose_sink=None):
    """Return the live inventory as plain sorted tuples."""
    floors, fills, types, callers, defs, spl = [], [], {}, [], [], []
    inline = []

    for rel, path in shader_files().items():
        s = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for ln, line in numbered(s):
            for m in RE_SCALAR_FLOOR.finditer(line):
                floors.append((rel, m.group(1), m.group(2)))
            for m in RE_SCALAR_FLOOR_UBO.finditer(line):
                floors.append((rel, m.group(1), m.group(2)))
            for m in RE_VEC3_FILL.finditer(line):
                comps = re.sub(r"\s+", "", m.group(2))
                # `vec3 ambientTerm = vec3(0.0);` is an ACCUMULATOR being zeroed,
                # not an authored ambient colour. Pinning those would bury the
                # four real colour pairs under a dozen inert rows -- and a check
                # nobody can read is a check nobody acts on.
                if all(float(c or 0) == 0.0
                       for c in comps.split(",")
                       if re.fullmatch(r"-?[0-9]*\.?[0-9]+", c or "")) and \
                   all(re.fullmatch(r"-?[0-9]*\.?[0-9]+", c) for c in comps.split(",")):
                    continue
                fills.append((rel, m.group(1), comps))
            for m in RE_INLINE_MIX.finditer(line):
                inline.append((rel, f"mix:{m.group(3)}", f"{m.group(1)}->{m.group(2)}"))
            for m in RE_INLINE_ADD.finditer(line):
                inline.append((rel, f"add:{m.group(3)}", f"{m.group(1)}+{m.group(2)}"))
        named = set(RE_LIGHT_TYPE.findall(s))
        for num in RE_LIGHT_TYPE_INT.findall(s):
            named.add(LIGHT_TYPE_BY_NUM.get(num, f"UNKNOWN_TYPE_{num}"))
        if named or RE_LIGHT_TYPE_FIELD.search(s):
            types[rel] = tuple(sorted(named)) or ("<reads type, dispatches none>",)
        if RE_CALC_LIGHT_DEF.search(s):
            defs.append(rel)
        if len(RE_CALC_LIGHT_CALL.findall(s)) > len(RE_CALC_LIGHT_DEF.findall(s)):
            callers.append(rel)
        if RE_SPL_DEFINE.search(s):
            spl.append(rel)

    cpp = []
    for rel, var in CPP_FLOOR_GATES:
        s = read_stripped(rel)
        if s is None:
            cpp.append((rel, var, "<FILE MISSING>"))
            continue
        found = var in RE_GETENV.findall(s)
        cpp.append((rel, var, "present" if found else "<READER GONE>"))

    return {
        "ambient_floor_scalar": sorted(floors),
        "ambient_fill_vec3": sorted(fills),
        "inline_light_floor": sorted(inline),
        "light_type_dispatch": tuple(sorted(types.items())),
        "calc_light_definition": sorted(defs),
        "calc_light_callers": sorted(callers),
        "native_zero_light_returns_base": sorted(spl),
        "cpp_floor_gate_readers": sorted(cpp),
    }


# ---------------------------------------------------------------------------
# PINNED BASELINE -- the deliberate current state.
# Each entry was read from the shader / C++ reader itself, not from docs.
# ---------------------------------------------------------------------------
BASELINE = {'ambient_floor_scalar': [('shaders/vulkan/water_geom.frag', 'SKY_AMBIENT', 'uEnv.cementSky.w')],
 'ambient_fill_vec3': [],
 'inline_light_floor': [('0.45+0.55', 'add:ndl', 'shaders/vulkan/valley_fog.frag'),   # INTENTIONAL: docs/lighting-divergence-map.md
                        ('0.55->1.0', 'mix:shadow', 'shaders/vulkan/fog_oob.frag')],   # INTENTIONAL: docs/lighting-divergence-map.md
 'light_type_dispatch': (('shaders/vulkan/mech_tier1.vert',
                          ('AMBIENT',
                           'INFINITE',
                           'INFINITEWITHFALLOFF',
                           'POINT',
                           'SPOT',
                           'TERRAIN')),),
 'calc_light_definition': ['shaders/vulkan/mech_tier1.vert'],
 'calc_light_callers': ['shaders/vulkan/mech_tier1.vert'],
 'native_zero_light_returns_base': ['shaders/vulkan/mech_tier1.vert'],
 'cpp_floor_gate_readers': [('mclib/camera.cpp', 'MC2_NIGHT_AMBIENT_FLOOR', 'present')]}

# ---------------------------------------------------------------------------
# ANTI-BLINDNESS WITNESSES. Each is a claim about the LIVE tree that is only
# true if the scanner is actually working. They fail exit 2 (broken check),
# distinct from exit 1 (real divergence), so the two are never confused.
# ---------------------------------------------------------------------------
WITNESS_DECOY_FILE = "shaders/vulkan/water_geom.frag"
WITNESS_DECOY_EXPECT = 1   # One actual native ambient-floor declaration.
MIN_NONEMPTY = ("ambient_floor_scalar",
                "calc_light_definition", "calc_light_callers",
                "native_zero_light_returns_base", "cpp_floor_gate_readers")


def self_check(live):
    problems = []

    if not shader_files():
        problems.append("shader sweep matched ZERO files -- wrong working directory?")

    for k in MIN_NONEMPTY:
        if not live.get(k):
            problems.append(f"category '{k}' scanned EMPTY -- scanner is blind, "
                            f"not the tree clean")

    n = sum(1 for f, _, _ in live["ambient_floor_scalar"] if f == WITNESS_DECOY_FILE)
    if n != WITNESS_DECOY_EXPECT:
        problems.append(
            f"comment-stripper witness FAILED: {WITNESS_DECOY_FILE} yielded {n} "
            f"ambient-floor site(s), expected {WITNESS_DECOY_EXPECT}. That file "
            f"must declare exactly one floor in native code; "
            f"{'>1 means comments are being scanned' if n > WITNESS_DECOY_EXPECT else 'fewer means the pattern no longer matches live code'}.")

    for rel, var, state in live["cpp_floor_gate_readers"]:
        if state != "present":
            problems.append(f"C++ floor gate {var} in {rel}: {state}")

    return problems


def fmt(v):
    if isinstance(v, (list, tuple)) and v and isinstance(v[0], (list, tuple)):
        return "\n".join("      " + " | ".join(str(x) for x in row) for row in v)
    if isinstance(v, (list, tuple)):
        return "\n".join("      " + str(x) for x in v)
    return "      " + str(v)


def norm(v):
    """Comparable form: tuples all the way down, order-insensitive.

    Rows mix str and tuple members (light_type_dispatch is
    (file, (TYPE, ...))), so sort by repr rather than by value -- ordering only
    has to be STABLE, it does not have to be meaningful.
    """
    if isinstance(v, (list, tuple)):
        return tuple(sorted((norm(x) for x in v), key=repr))
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--print-live", action="store_true",
                    help="print the live inventory (for hand-review before pinning)")
    args = ap.parse_args()

    live = scan()

    if args.print_live:
        for k in sorted(live):
            print(f"{k}:")
            print(fmt(live[k]) or "      (empty)")
        return 0

    problems = self_check(live)
    if problems:
        print("check-lighting-divergence: CHECK IS BROKEN (not a tree failure)",
              file=sys.stderr)
        for p in problems:
            print(f"  * {p}", file=sys.stderr)
        return 2

    failures = []
    for key in sorted(BASELINE):
        want, got = norm(BASELINE[key]), norm(live.get(key, ()))
        if want != got:
            added = [x for x in got if x not in want]
            removed = [x for x in want if x not in got]
            failures.append((key, added, removed))

    if not failures:
        n = sum(len(v) for v in live.values() if isinstance(v, (list, tuple)))
        print(f"check-lighting-divergence: PASS -- {n} pinned lighting-decision "
              f"sites match the baseline across {len(shader_files())} shaders.")
        if args.verbose:
            for k in sorted(live):
                print(f"{k}:")
                print(fmt(live[k]) or "      (empty)")
        return 0

    print("check-lighting-divergence: FAIL -- the lighting divergence map moved.",
          file=sys.stderr)
    for key, added, removed in failures:
        print(f"\n  [{key}]", file=sys.stderr)
        for a in added:
            print(f"    + NEW  {a}", file=sys.stderr)
        for r in removed:
            print(f"    - GONE {r}", file=sys.stderr)
    print("\n  A new or changed row here means a lighting decision was made in one\n"
          "  class without being made anywhere else. If that is deliberate, update\n"
          "  BASELINE in scripts/check-lighting-divergence.py AND add the row to\n"
          "  docs/lighting-divergence-map.md with an INTENTIONAL/ACCIDENTAL call,\n"
          "  in the same commit.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
