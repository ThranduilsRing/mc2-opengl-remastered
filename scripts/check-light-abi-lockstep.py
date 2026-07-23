#!/usr/bin/env python3
"""check-light-abi-lockstep.py — LIGHT-ABI-WIDEN-STAGE0-1 tripwire.

The per-object GPU light record (ObjectLights / TG_HWLightsData) has fixed
inner arrays whose size IS the per-object ABI cap N. Widening N changes the
SSBO record STRIDE. Five sites must agree byte-for-byte or every record after
the first is read at the wrong offset -> silent per-fragment light corruption
(documented mc2_24 regression 2026-05-02). One of them (kLightRecordStride in
gameos_graphics.cpp) is a HAND-COPIED literal in a TU that cannot include
tgl.h — the sneakiest drift site. This checker is the tripwire for exactly
that "edit one site, forget the others" failure mode.

TRAP (do NOT conflate):
  MAX_LIGHTS_IN_WORLD in mclib/tgl.h  = the GLOBAL light pool (1024).
  MAX_LIGHTS_IN_WORLD in lighting.hglsl = the per-object GPU ABI cap (this N).
  The C++ ABI cap is a THIRD macro: MAX_HW_LIGHTS_IN_WORLD in tgl.h.

The five lockstep sites:
  1. shaders/include/lighting.hglsl   MAX_LIGHTS_IN_WORLD      (cap N)
  2. mclib/tgl.h                       MAX_HW_LIGHTS_IN_WORLD   (cap N)
  3. mclib/tgl.h                       static_assert sizeof(TG_HWLightsData) (stride)
  4. GameOS/gameos/gameos_graphics_light_ssbo.cpp kLightRecordStride (stride; TU split by GAMEOS-GRAPHICS-SPLIT-1)
  5. shaders/lightgrid_build.comp      LIGHTGRID_MAX_LIGHTS     (cap N)

Stride is derived from the cap: stride = N*112 + 16
  (mat4=64 + dir 16 + color 16 + falloff 16 per light, + ivec4 tail 16).

Exit 0 if all five agree (and strides match the derived value); nonzero + a
diagnostic table otherwise.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def derive_stride(n):
    return n * 112 + 16


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def grab(rel, pattern, label):
    """Return (int_value, None) on success or (None, error_str)."""
    try:
        text = read(rel)
    except OSError as e:
        return None, "%s: cannot read %s (%s)" % (label, rel, e)
    m = re.search(pattern, text)
    if not m:
        return None, "%s: pattern not found in %s" % (label, rel)
    return int(m.group(1)), None


def main():
    quiet = "--quiet" in sys.argv or "-q" in sys.argv

    findings = []  # (label, kind, value, err)

    # Site 1: GLSL per-object ABI cap.
    v, e = grab("shaders/include/lighting.hglsl",
                r"#define\s+MAX_LIGHTS_IN_WORLD\s+(\d+)", "glsl_cap")
    findings.append(("lighting.hglsl MAX_LIGHTS_IN_WORLD", "cap", v, e))

    # Site 2: C++ ABI cap.
    v, e = grab("mclib/tgl.h",
                r"#define\s+MAX_HW_LIGHTS_IN_WORLD\s+(\d+)", "cpp_cap")
    findings.append(("tgl.h MAX_HW_LIGHTS_IN_WORLD", "cap", v, e))

    # Site 5: lightgrid mirror cap.
    v, e = grab("shaders/lightgrid_build.comp",
                r"#define\s+LIGHTGRID_MAX_LIGHTS\s+(\d+)", "lg_cap")
    findings.append(("lightgrid_build.comp LIGHTGRID_MAX_LIGHTS", "cap", v, e))

    # Site 3: C++ static_assert stride.
    v, e = grab("mclib/tgl.h",
                r"static_assert\(\s*sizeof\(TG_HWLightsData\)\s*==\s*(\d+)", "cpp_stride")
    findings.append(("tgl.h static_assert sizeof(TG_HWLightsData)", "stride", v, e))

    # Site 4: hand-copied upload stride.
    v, e = grab("GameOS/gameos/gameos_graphics_light_ssbo.cpp",
                r"kLightRecordStride\s*=\s*(\d+)", "stride_literal")
    findings.append(("gameos_graphics_light_ssbo.cpp kLightRecordStride", "stride", v, e))

    errors = [f for f in findings if f[3] is not None]
    caps = [f[2] for f in findings if f[1] == "cap" and f[2] is not None]
    strides = [f for f in findings if f[1] == "stride" and f[2] is not None]

    problems = []
    for label, kind, value, err in errors:
        problems.append("MISSING: " + err)

    if caps and len(set(caps)) != 1:
        problems.append("CAP MISMATCH: the three cap sites disagree: %s" % caps)

    expected_stride = derive_stride(caps[0]) if len(set(caps)) == 1 and caps else None
    for label, kind, value, err in strides:
        if expected_stride is not None and value != expected_stride:
            problems.append(
                "STRIDE MISMATCH: %s = %d, but cap N=%d implies stride N*112+16 = %d"
                % (label, value, caps[0], expected_stride))
    if len(set(f[2] for f in strides)) > 1:
        problems.append("STRIDE DISAGREEMENT: stride sites differ: %s"
                        % [(f[0], f[2]) for f in strides])

    if not quiet:
        print("LIGHT-ABI-WIDEN lockstep check:")
        for label, kind, value, err in findings:
            shown = err if err else "%s = %s" % (kind, value)
            print("  %-48s %s" % (label, shown))
        if expected_stride is not None:
            print("  derived stride (N*112+16) for N=%d: %d" % (caps[0], expected_stride))

    if problems:
        print("LIGHT-ABI-WIDEN lockstep: FAIL", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1

    if not quiet:
        print("LIGHT-ABI-WIDEN lockstep: PASS (cap N=%d, stride=%d, 5 sites agree)"
              % (caps[0], expected_stride))
    return 0


if __name__ == "__main__":
    sys.exit(main())
