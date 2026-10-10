#!/usr/bin/env python3
"""RENDER-VIEW-CURRENCY-1 invariant check.

The terrain dispatch MVP snapshot (gos_terrain_indirect_getDispatchMvp16) is a
CAMERA-DEPENDENT SNAPSHOT. Using it without proving its view epoch matches the
current view is the BUG1/BUG3/BUG5 root (objects projected through a stale /
mid-frame-superseded camera -> zoom wobble, rotate flicker, props vanish until
pause). To make that bug class structurally impossible:

  * raw gos_terrain_indirect_getDispatchMvp16() is PHASE-PRIVATE to terrain-coupled
    draw passes (terrain / water / shoreline / overlay / decal), which run in the
    same phase the snapshot is published, so currency is structural for them;
  * every OTHER consumer (static props, mechs, vehicles, buildings, generic object
    batchers, UI/editor draw) MUST obtain the MVP via gos_GetObjectDrawMVP()
    (gos_object_draw_mvp.h), which validates the view epoch and falls back to the
    live MVP when it does not match.

This script FAILS the build if a non-allowlisted translation unit references the
raw snapshot getter. Add a file to ALLOWLIST only if it is a genuine terrain-phase
consumer — never to silence a violation in object/mech draw.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Whole-file allow: pure terrain-coupled TUs + the sanctioned accessor/definition.
# Every raw snapshot read in these files is same-phase-as-publish (currency is
# structural) or is the definition/accessor itself. Basenames.
FULL_ALLOW = {
    "gos_terrain_indirect_pack.cpp",      # defines the snapshot
    "gos_terrain_indirect_dispatch.cpp",  # defines the getter
    "gos_terrain_indirect.h",
    "gos_object_draw_mvp.h",       # the currency-checked accessor (only sanctioned reader)
    "gos_terrain_lod_chunk_draw.cpp",   # terrain solid draw
    "gos_terrain_lod_chunk_internal.h", # declares the dispatch-MVP getter for the draw piece
    "gos_terrain_water_stream.cpp",# water fast path (pure water)
    "gos_terrain_water_fast.vert", # (shader, never matches the C symbol; harmless)
    "debug_renderer.cpp",          # diagnostics dump
}

# MIXED TUs: contain BOTH legitimate terrain-phase reads (water/overlay/decal) and
# object-phase draw. A raw snapshot read here is allowed ONLY on a line carrying the
# TERRAIN-PHASE-RAW-MVP-OK tag. Object-phase reads (e.g. object shadows) must instead
# use gos_GetObjectDrawMVP(). This closes the hole that let gos_SetupObjectShadows
# read the raw snapshot blind under a whole-file allowlist (OBJECT-SHADOW-MVP-CURRENCY-1).
MARKER_REQUIRED = {
    "gameos_graphics.cpp",
}
MARKER = "TERRAIN-PHASE-RAW-MVP-OK"

# The raw phase-private symbol. gos_GetObjectDrawMVP / getDispatchMvpViewEpoch /
# getDispatchMvpFrameIdx / getDispatchMvpFp are NOT this — only the matrix getter.
RAW = re.compile(r"\bgos_terrain_indirect_getDispatchMvp16\b")

SCAN_DIRS = ["GameOS/gameos", "mclib", "code", "RenderCore", "RenderWorld", "GameAdapters"]
EXTS = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl"}

# CORPUS FLOOR (UNWIRED-CHECK-CLOSE-1, 2026-07-30).
# This check reports a violation-free PASS by finding nothing. That verdict is
# indistinguishable from finding nothing because it LOOKED AT NOTHING: the
# `if not base.is_dir(): continue` below silently skips a scan dir that has
# been renamed, moved or is absent because the check ran from the wrong cwd.
# Rename all six dirs and this prints "OK (no raw dispatch-MVP reads...)"
# having opened zero files. The corpus was 1147 files when this floor was set;
# 200 is a deliberately loose floor that a real refactor will not trip but a
# blind run always will.
CORPUS_FLOOR = 200


def main() -> int:
    violations = []
    scanned = 0
    missing_dirs = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            missing_dirs.append(d)
            continue
        for f in base.rglob("*"):
            if f.suffix not in EXTS or f.name in FULL_ALLOW:
                continue
            scanned += 1
            marker_ok = f.name in MARKER_REQUIRED
            try:
                text = f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for i, line in enumerate(text.splitlines(), 1):
                # Strip // line-comment and single-line /* */ so prose mentioning
                # the symbol (e.g. "phase-private" NOTEs) is not a violation. Read
                # the marker from the FULL line (comments included) before stripping.
                tagged = MARKER in line
                code = re.sub(r"/\*.*?\*/", "", line)
                code = code.split("//", 1)[0]
                if RAW.search(code):
                    if marker_ok and tagged:
                        continue   # tagged terrain-phase read in a mixed TU — allowed
                    violations.append((f.relative_to(ROOT), i, line.strip()))

    if violations:
        print("RENDER-VIEW-CURRENCY-1 VIOLATION: raw dispatch-MVP snapshot read "
              "outside a terrain-phase translation unit.\n"
              "Object/mech/prop draw must use gos_GetObjectDrawMVP() "
              "(gos_object_draw_mvp.h), which validates the view epoch.\n", file=sys.stderr)
        for path, ln, src in violations:
            print(f"  {path}:{ln}: {src}", file=sys.stderr)
        print(f"\n{len(violations)} violation(s).", file=sys.stderr)
        return 1

    # Blindness floor is checked AFTER violations: a run that found real
    # violations proves it could see, and should report them as violations.
    if scanned < CORPUS_FLOOR:
        print("check-object-mvp-currency WENT BLIND: scanned only "
              f"{scanned} file(s), floor is {CORPUS_FLOOR}.", file=sys.stderr)
        if missing_dirs:
            print(f"  missing scan dirs: {', '.join(missing_dirs)}", file=sys.stderr)
        print("  A clean verdict from an empty corpus is not a pass. Check the "
              "cwd/repo root, or update SCAN_DIRS if the tree was reorganised.",
              file=sys.stderr)
        return 1

    print(f"check-object-mvp-currency: OK (no raw dispatch-MVP reads outside "
          f"terrain phase; {scanned} files scanned)")
    return 0


def selftest() -> int:
    """Prove the blindness floor fires. Not a fixture -- it re-runs the REAL
    scan against a root where the scan dirs do not exist, which is exactly the
    condition that produced a silent green before the floor existed."""
    global ROOT
    import tempfile
    ok = True

    real = main()
    if real != 0:
        print("[selftest] SKIP: live tree is not clean, cannot grade the floor",
              file=sys.stderr)
        return 0
    print("[selftest]   ok  live scan passes and reports its corpus size")

    saved = ROOT
    try:
        with tempfile.TemporaryDirectory() as td:
            ROOT = Path(td)          # no SCAN_DIRS exist under here
            blind = main()
    finally:
        ROOT = saved

    if blind == 0:
        print("[selftest] FAIL: empty corpus still returned PASS -- the floor "
              "is not wired", file=sys.stderr)
        ok = False
    else:
        print("[selftest]   ok  empty corpus FAILS (blindness floor fires)")

    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
