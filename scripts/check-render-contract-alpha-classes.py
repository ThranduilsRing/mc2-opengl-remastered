#!/usr/bin/env python3
"""CONTRACT-TRUTH-2 -- reconcile the DOCUMENTED GBuffer1 alpha classes against
the constants the shaders actually EMIT.

WHY THIS EXISTS
---------------
shaders/include/render_contract.hglsl is the canonical description of the
GBuffer1.a shadow-mask class table: every other shader conforms to it, and
scripts/check-render-contract-gbuffer1.sh enforces that they route through its
helpers. It was caught being WRONG:

    :77  "Matches BOTH 0.25 (solid prop) and 0.375 (alpha-test lattice)"

0.375 is a value no shader in this repo has ever emitted. The helper twenty
lines above returns 0.1875, and the block comment above THAT argues at length
for 0.1875 and explicitly AGAINST 0.375 -- so the file contradicted its own
rationale and its own code, in the header that defines the contract. The same
stale constant had been copied into shaders/shadow_screen.frag:276.

That is a CONSTANT DUPLICATED BETWEEN A COMMENT AND CODE. The comment drifted
and nothing noticed, because nothing was comparing the two. A check that reads
only the comment proves nothing; this one derives the class table from the
EMITTED literals and tests the prose against it.

WHAT IT ENFORCES -- three censuses
----------------------------------
A. EMITTED CLASS TABLE, GL vs VK. The alpha literal each rc_gbuffer1_* helper
   returns is parsed out of the header. shaders/vulkan/color_prop_tier1.frag
   cannot #include the header (no GLSL include in the VK toolchain), so it
   INLINES the same two prop constants (5efd553e0). Those inlined values must
   equal the GL helpers' values exactly. A divergence here is the VK-GBUFFER1-
   CENSUS-1 bug returning: that shader shipped a flat 0.25 for every prop and
   misclassified every VK-drawn tree.

B. READER-BAND COHERENCE. The bounds inside rc_isStaticPropNoSelfShadow,
   rc_isAlphaTestProp and rc_pixelHandlesOwnShadow are parsed from code, and
   every emitted class is required to land where the contract says it lands
   (solid and cutout both inside the prop band; cutout inside the (0.1, 0.22]
   sub-band; solid OUTSIDE that sub-band; terrain above the own-shadow
   threshold; mech below the prop band). This is what actually makes 0.1875
   "load-bearing" testable: move an emitted class or a reader bound
   independently and the pair stops agreeing.

C. PROSE-VS-CODE NUMERAL CENSUS -- the drift catcher, and the one that would
   have caught 0.375 on the day it was written.

   Rule: any decimal literal that appears ONLY in a COMMENT, in the static-prop
   band, anywhere in the GBuffer1 contract file set, and appears in NO real code
   in that set, is a documented value nothing emits. It must be corrected, or
   registered in PROSE_ALLOWLIST below with a reason.

   The band is read from rc_isStaticPropNoSelfShadow, not hard-coded, so
   widening the reader widens the census with it. The "appears in code
   somewhere in the set" side is what keeps this honest and self-maintaining:
   0.22, 0.15, 0.35, 0.4, 0.5 are all real thresholds in these files and pass
   with no entry, while 0.375 is prose-only and fails. Measured on the tree at
   HEAD=5efd553e0 with the defect still in place, this rule flagged exactly the
   four occurrences of 0.375 and nothing else.

   NOT ENFORCED, stated so it is a known gap rather than an unknown one: a
   wrong class value written as prose OUTSIDE the file set below (a design doc,
   a release-review README, a status file) is invisible here. Frozen release
   artifacts under release_assets/ are deliberately not scanned -- editing a
   shipped review to match today's code would falsify the record.

USAGE
  py -3 scripts/check-render-contract-alpha-classes.py
  py -3 scripts/check-render-contract-alpha-classes.py --list
  py -3 scripts/check-render-contract-alpha-classes.py --selftest

EXIT CODES
  0  the prose and the emitted constants agree
  1  a documented value disagrees with what the shaders emit
  2  the check is BLIND (a scanned file is missing, or a constant/bound this
     census is built on could not be parsed). Blind is never a pass.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)

HEADER = "shaders/include/render_contract.hglsl"
VK_PROP = "shaders/vulkan/color_prop_tier1.frag"

# The GBuffer1 alpha-class contract file set: the header that DEFINES the
# classes, the reader that branches on them, the two producers that emit them,
# and the two shoreline shaders that read the same channel continuously.
CONTRACT_FILES = [
    HEADER,
    "shaders/vulkan/screen_shadow_static.frag",
    "shaders/vulkan/screen_shadow_csm.frag",
    VK_PROP,
    "shaders/vulkan/shoreline.frag",
]

# ---------------------------------------------------------------------------
# Census C exemptions. An entry is (path, value, marker, reason).
#
# `marker` must appear in the offending comment text. That is the whole point:
# an exemption is granted to a SENTENCE, not to a line number and not to a
# number. Rewrite the sentence and the entry goes STALE, the reverse ratchet
# below fails, and the next person has to re-read what they are exempting
# instead of inheriting it.
#
# If you are here because a check went red: do NOT add an entry to get green.
# The failure means the tree documents a mask-class value that no shader emits,
# and the fix is almost always the prose. An entry is correct only when the
# number is genuinely not a class value -- a rejected alternative, an unrelated
# CPU default, or intermediate arithmetic.
# ---------------------------------------------------------------------------
PROSE_ALLOWLIST = [
    (HEADER, "0.375", "VALUE CHOICE",
     "names 0.375 as the REJECTED alternative. This rationale is load-bearing "
     "-- 0.375 would have left the shoreline water band -- and it must keep "
     "saying the number it rejected."),
    (HEADER, "0.375", "Picking 0.375 would have moved",
     "same rationale, continued: the counterfactual consequence of the value "
     "that was NOT chosen."),

]

# A decimal literal. Guarded on the left so 1.0.25 / v0.4 style text and
# identifier suffixes do not produce phantom numbers.
_NUM = re.compile(r"(?<![\w.])(\d*\.\d+)")


def die_blind(msg: str) -> None:
    print(f"BLIND: {msg}", file=sys.stderr)
    print("       This census cannot answer, which is not the same as a clean "
          "tree.", file=sys.stderr)
    sys.exit(2)


def read(rel: str) -> str:
    p = os.path.join(ROOT, rel.replace("/", os.sep))
    if not os.path.isfile(p):
        die_blind(f"{rel} is absent; the alpha-class census cannot run")
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def split_comments(text: str):
    """-> (code_text, [(lineno, comment_text)]). Line-oriented `//` only.

    Block comments are not stripped. No file in the set uses them for this
    text today, and introducing one would surface as a NEW finding, which is
    the safe direction to fail.
    """
    code, comments = [], []
    for i, ln in enumerate(text.splitlines(), 1):
        if "//" in ln:
            c, m = ln.split("//", 1)
            code.append(c)
            comments.append((i, m.strip()))
        else:
            code.append(ln)
    return "\n".join(code), comments


# ---------------------------------------------------------------------------
# Census A -- the emitted class table
# ---------------------------------------------------------------------------
_HELPER = re.compile(
    r"PREC\s+vec4\s+(rc_gbuffer1_\w+)\s*\([^)]*\)\s*\{\s*return\s+vec4\s*\("
    r"([^;]*?)\)\s*;", re.S)


def emitted_classes(header_code: str) -> dict:
    """helper name -> emitted alpha literal (str), or None when continuous."""
    out = {}
    for m in _HELPER.finditer(header_code):
        name, args = m.group(1), m.group(2)
        depth, cur, parts = 0, "", []
        for ch in args:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            if ch == "," and depth == 0:
                parts.append(cur)
                cur = ""
            else:
                cur += ch
        parts.append(cur)
        last = parts[-1].strip()
        out[name] = last if re.fullmatch(r"\d*\.?\d+", last) else None
    return out


_VK_PROP_EMIT = re.compile(
    r"oGBuffer1\s*=\s*vec4\([^;]*\?\s*([\d.]+)\s*:\s*([\d.]+)\s*\)\s*;")


# ---------------------------------------------------------------------------
# Census B -- reader bounds, parsed from the readers themselves
# ---------------------------------------------------------------------------
def reader_bounds(header_code: str, fn: str):
    """-> (lo, hi) from `gbuffer1.a > lo && gbuffer1.a <= hi`, or (lo, None)."""
    m = re.search(r"bool\s+" + re.escape(fn) + r"\s*\([^)]*\)\s*\{(.*?)\}",
                  header_code, re.S)
    if not m:
        return None
    body = m.group(1)
    lo = re.search(r"\.a\s*>\s*([\d.]+)", body)
    hi = re.search(r"\.a\s*<=\s*([\d.]+)", body)
    return (float(lo.group(1)) if lo else None,
            float(hi.group(1)) if hi else None)


def run() -> int:
    fails = []
    header_code, _ = split_comments(read(HEADER))

    # ---- A ----------------------------------------------------------------
    emitted = emitted_classes(header_code)
    need = ["rc_gbuffer1_shadowHandled",
            "rc_gbuffer1_screenShadowEligible",
            "rc_gbuffer1_screenShadowEligible_noSelfShadow",
            "rc_gbuffer1_screenShadowEligible_noSelfShadow_alphaTest"]
    missing = [n for n in need if n not in emitted]
    if missing:
        die_blind(f"{HEADER} no longer defines {', '.join(missing)} in a shape "
                  f"this census can parse; the class table cannot be derived")
    solid = emitted["rc_gbuffer1_screenShadowEligible_noSelfShadow"]
    cutout = emitted["rc_gbuffer1_screenShadowEligible_noSelfShadow_alphaTest"]
    if solid is None or cutout is None:
        die_blind("a prop-class helper no longer returns a literal alpha")
    if solid == cutout:
        fails.append(f"A: the solid and alpha-test prop helpers both emit "
                     f"{solid}; the classes are no longer distinguishable, so "
                     f"rc_isAlphaTestProp can never separate them")

    vk_code, _ = split_comments(read(VK_PROP))
    vm = _VK_PROP_EMIT.search(vk_code)
    if not vm:
        die_blind(f"{VK_PROP} no longer writes attachment 1 in a shape this "
                  f"census can parse (expected the alphaTestClass ? A : B "
                  f"form); GL/VK class agreement is unverifiable")
    vk_cutout, vk_solid = vm.group(1), vm.group(2)
    if float(vk_cutout) != float(cutout) or float(vk_solid) != float(solid):
        fails.append(
            f"A: GL emits solid={solid} cutout={cutout} but {VK_PROP} emits "
            f"solid={vk_solid} cutout={vk_cutout}. The VK prop island draws "
            f"the same class; a divergence misclassifies every VK-drawn tree "
            f"(VK-GBUFFER1-CENSUS-1).")

    # ---- B ----------------------------------------------------------------
    prop = reader_bounds(header_code, "rc_isStaticPropNoSelfShadow")
    sub = reader_bounds(header_code, "rc_isAlphaTestProp")
    own = reader_bounds(header_code, "rc_pixelHandlesOwnShadow")
    if not prop or prop[0] is None or prop[1] is None:
        die_blind("cannot parse rc_isStaticPropNoSelfShadow's band")
    if not sub or sub[0] is None or sub[1] is None:
        die_blind("cannot parse rc_isAlphaTestProp's sub-band")
    if not own or own[0] is None:
        die_blind("cannot parse rc_pixelHandlesOwnShadow's threshold")
    plo, phi = prop
    slo, shi = sub
    fsolid, fcut = float(solid), float(cutout)
    terrain = emitted["rc_gbuffer1_shadowHandled"]
    mech = emitted["rc_gbuffer1_screenShadowEligible"]

    def inband(v, lo, hi):
        return lo < v <= hi

    if not inband(fsolid, plo, phi):
        fails.append(f"B: solid prop emits {solid}, outside the static-prop "
                     f"band ({plo}, {phi}] its own reader tests")
    if not inband(fcut, plo, phi):
        fails.append(f"B: alpha-test cutout emits {cutout}, outside the "
                     f"static-prop band ({plo}, {phi}] -- it would stop being "
                     f"treated as a static prop at all")
    if not inband(fcut, slo, shi):
        fails.append(f"B: alpha-test cutout emits {cutout}, outside "
                     f"rc_isAlphaTestProp's sub-band ({slo}, {shi}] -- the "
                     f"RECEIVE opt-in would stop excluding it")
    if inband(fsolid, slo, shi):
        fails.append(f"B: solid prop emits {solid}, INSIDE "
                     f"rc_isAlphaTestProp's sub-band ({slo}, {shi}] -- solid "
                     f"props would be excluded from the RECEIVE opt-in too")
    if terrain is not None and not float(terrain) > own[0]:
        fails.append(f"B: shadowHandled emits {terrain}, which does not clear "
                     f"rc_pixelHandlesOwnShadow's threshold (> {own[0]})")
    if mech is not None and float(mech) > plo:
        fails.append(f"B: screenShadowEligible (mech) emits {mech}, which is "
                     f"inside the static-prop band (> {plo}) -- mechs would "
                     f"lose their dynamic self-shadow")

    # ---- C ----------------------------------------------------------------
    code_nums, comment_rows = set(), []
    for rel in CONTRACT_FILES:
        code, comments = split_comments(read(rel))
        code_nums.update(_NUM.findall(code))
        for lineno, text in comments:
            for v in _NUM.findall(text):
                comment_rows.append((rel, lineno, v, text))

    def in_prop_band(v: str) -> bool:
        return plo < float(v) <= phi

    used = set()
    for rel, lineno, v, text in comment_rows:
        if not in_prop_band(v) or v in code_nums:
            continue
        hit = None
        for idx, (a_rel, a_val, a_mark, _why) in enumerate(PROSE_ALLOWLIST):
            if a_rel == rel and a_val == v and a_mark in text:
                hit = idx
                break
        if hit is not None:
            used.add(hit)
            continue
        fails.append(
            f"C: {rel}:{lineno} documents {v} in the static-prop band "
            f"({plo}, {phi}], but no code in the GBuffer1 contract file set "
            f"emits or tests that value. Emitted classes: solid={solid} "
            f"cutout={cutout}. Line: {text[:80]}")

    stale = [i for i in range(len(PROSE_ALLOWLIST)) if i not in used]
    for i in stale:
        rel, val, mark, _why = PROSE_ALLOWLIST[i]
        fails.append(
            f"C: STALE exemption -- PROSE_ALLOWLIST entry ({rel}, {val}, "
            f"{mark!r}) matches nothing in the tree. An exemption for prose "
            f"that no longer exists is a false record, and it silently "
            f"pre-approves that prose coming back. Delete it.")

    if fails:
        print("FAIL: documented GBuffer1 alpha classes disagree with the "
              "constants the shaders emit.\n", file=sys.stderr)
        for f in fails:
            print(f"  * {f}\n", file=sys.stderr)
        return 1

    print(f"Render contract alpha-class census PASSED "
          f"(emitted: terrain={terrain} solid={solid} cutout={cutout} "
          f"mech={mech}; prop band ({plo}, {phi}], cutout sub-band "
          f"({slo}, {shi}]; {len(CONTRACT_FILES)} files, "
          f"{len(PROSE_ALLOWLIST)} registered exemption(s)).")
    return 0


def list_table() -> int:
    header_code, _ = split_comments(read(HEADER))
    print("EMITTED GBuffer1.a classes (parsed from the helper bodies):")
    for k, v in emitted_classes(header_code).items():
        print(f"  {k:62} {v if v is not None else '(continuous)'}")
    for fn in ("rc_pixelHandlesOwnShadow", "rc_isStaticPropNoSelfShadow",
               "rc_isAlphaTestProp"):
        print(f"  reader {fn:55} {reader_bounds(header_code, fn)}")
    return 0


# ---------------------------------------------------------------------------
def selftest() -> int:
    fails = []

    def ck(name, cond, why=""):
        if not cond:
            fails.append(f"{name}: {why}")
        print(f"  {'PASS' if cond else 'FAIL'}  {name}"
              + (f"  -- {why}" if not cond else ""))

    print("check-render-contract-alpha-classes selftest")

    hdr = ("PREC vec4 rc_gbuffer1_screenShadowEligible_noSelfShadow(PREC vec3 n) {\n"
           "    return vec4(n * 0.5 + 0.5, 0.25);\n}\n"
           "PREC vec4 rc_gbuffer1_screenShadowEligible_noSelfShadow_alphaTest(PREC vec3 n) {\n"
           "    return vec4(n * 0.5 + 0.5, 0.1875);\n}\n"
           "bool rc_isStaticPropNoSelfShadow(PREC vec4 g) {\n"
           "    return g.a > 0.1 && g.a <= 0.5;\n}\n"
           "bool rc_isAlphaTestProp(PREC vec4 g) {\n"
           "    return g.a > 0.1 && g.a <= 0.22;\n}\n")
    e = emitted_classes(hdr)
    ck("emitted alpha is parsed out of the helper body, not a comment",
       e.get("rc_gbuffer1_screenShadowEligible_noSelfShadow_alphaTest")
       == "0.1875",
       "the census cannot see what the shader actually returns")
    ck("nested vec4 arithmetic does not confuse the last-argument split",
       e.get("rc_gbuffer1_screenShadowEligible_noSelfShadow") == "0.25")
    ck("reader bounds are parsed from code",
       reader_bounds(hdr, "rc_isAlphaTestProp") == (0.1, 0.22))

    # THE REAL REGRESSION, in shape: a comment naming a value nothing emits.
    code, comments = split_comments(
        "// Matches BOTH 0.25 (solid prop) and 0.375 (alpha-test lattice).\n"
        "    return vec4(n * 0.5 + 0.5, 0.1875);\n")
    cnums = set(_NUM.findall(code))
    cmt = {v for _i, t in comments for v in _NUM.findall(t)}
    ck("INJECTED prose-only class value is visible to the census",
       "0.375" in cmt and "0.375" not in cnums,
       "the 0.375 that sat in the contract header would not have been seen")
    ck("the emitted value in the same snippet is code, not comment",
       "0.1875" in cnums)
    # The false-positive direction. A census that flags every number in prose
    # is a census somebody deletes; 0.25 is quoted here AND emitted elsewhere
    # in the set, and must pass with no exemption.
    code25, com25 = split_comments(
        "// solid props are 0.25, cutouts are 0.1875\n"
        "    return vec4(n * 0.5 + 0.5, 0.25);\n"
        "    return vec4(n * 0.5 + 0.5, 0.1875);\n")
    c25 = set(_NUM.findall(code25))
    proseonly = {v for _i, t in com25 for v in _NUM.findall(t)} - c25
    ck("prose quoting a value the set DOES emit is not flagged",
       proseonly == set(),
       f"would have failed on correct prose: {sorted(proseonly)}")

    # The comment stripper is the load-bearing half of census C: if it stops
    # working, code numerals leak into the comment set and everything passes.
    c2, m2 = split_comments("float x = 0.375; // and 0.9 in prose\n")
    ck("comment stripper separates code from prose",
       "0.375" in set(_NUM.findall(c2))
       and "0.9" in {v for _i, t in m2 for v in _NUM.findall(t)}
       and "0.9" not in set(_NUM.findall(c2)),
       "a broken stripper makes every prose value look like code and the "
       "census silently passes everything")

    ck("VK inline prop pair is parsed",
       _VK_PROP_EMIT.search(
           "oGBuffer1 = vec4(normalize(v) * 0.5 + 0.5, "
           "alphaTestClass ? 0.1875 : 0.25);").groups() == ("0.1875", "0.25"))

    ck("a version-like token is not read as a decimal",
       _NUM.findall("mc2-win64-v0.4") == [],
       "identifier suffixes would manufacture phantom class values")

    print()
    if fails:
        print(f"SELFTEST FAILED ({len(fails)}):")
        for f in fails:
            print(f"  {f}")
        return 2
    print("SELFTEST PASSED")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Reconcile documented GBuffer1 alpha classes against the "
                    "constants the shaders emit.")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--list", action="store_true",
                    help="print the emitted class table and reader bounds")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.list:
        return list_table()
    return run()


if __name__ == "__main__":
    sys.exit(main())
