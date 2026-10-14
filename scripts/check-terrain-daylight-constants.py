#!/usr/bin/env python3
"""TERRAIN-AMBIENT-FLOOR-2 — pin terrain_daylight.h's night constants to camera.cpp's.

GameOS/gameos/utils/terrain_daylight.h duplicates camera.cpp's NIGHT_LIGHT_PITCH
and NIGHT_START_PITCH as constexpr floats. The duplication is deliberate (the
header must stay engine-free for tests/unit/test_terrain_daylight_harness.cpp), but a silent duplicate is a drift bug waiting to happen: if
someone retunes the engine's night band, the terrain ambient floor's daylight
gate keeps using the old one and a night mission quietly starts taking a
brightening term.

This check exists because the header's comment CLAIMED the two were pinned
before anything actually pinned them. A comment is not a check.

  --self-test  proves the check FAILS on an injected drift (both directions),
               so a green run means something.

Exit 0 = in sync.  Exit 1 = drifted / unparseable.
"""

import os
import re
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAMERA = os.path.join(REPO, "mclib", "camera_internal.h")   # the NIGHT_* defines moved there with the camera split
HEADER = os.path.join(REPO, "GameOS", "gameos", "utils", "terrain_daylight.h")

# camera.cpp: #define NIGHT_START_PITCH		(25.0f)
CAM_RE = {
    "NIGHT_START_PITCH": re.compile(r"^\s*#define\s+NIGHT_START_PITCH\s*\(\s*([0-9.]+)f?\s*\)", re.M),
    "NIGHT_LIGHT_PITCH": re.compile(r"^\s*#define\s+NIGHT_LIGHT_PITCH\s*\(\s*([0-9.]+)f?\s*\)", re.M),
}
# header: constexpr float kNightStartDeg = 25.0f;
HDR_RE = {
    "NIGHT_START_PITCH": re.compile(r"constexpr\s+float\s+kNightStartDeg\s*=\s*([0-9.]+)f?\s*;"),
    "NIGHT_LIGHT_PITCH": re.compile(r"constexpr\s+float\s+kNightFullDeg\s*=\s*([0-9.]+)f?\s*;"),
}


def extract(path, patterns, label):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        print(f"FAIL: cannot read {label} ({path}): {e}")
        return None
    out = {}
    for name, rx in patterns.items():
        m = rx.search(text)
        if not m:
            print(f"FAIL: {label} ({os.path.relpath(path, REPO)}): "
                  f"could not find {name} — the checker's regex is stale, "
                  f"which is itself a drift.")
            return None
        out[name] = float(m.group(1))
    return out


def compare(cam, hdr, quiet=False):
    bad = []
    for name in sorted(cam):
        if cam[name] != hdr[name]:
            bad.append(f"  {name}: camera.cpp={cam[name]}  terrain_daylight.h={hdr[name]}")
    if bad:
        print("FAIL: terrain_daylight.h night constants have DRIFTED from camera.cpp:")
        print("\n".join(bad))
        print("  Effect: the terrain ambient floor's daylight gate no longer matches")
        print("  the engine's own day/night classification, so a night mission can")
        print("  take a brightening term. Fix the header, or fix this check.")
        return 1
    if not quiet:
        print(f"PASS: check-terrain-daylight-constants.py -- "
              f"NIGHT_LIGHT_PITCH={cam['NIGHT_LIGHT_PITCH']} "
              f"NIGHT_START_PITCH={cam['NIGHT_START_PITCH']} in sync")
    return 0


def self_test():
    """Inject the real regression into a COPY of the header and require a FAIL."""
    cam = extract(CAMERA, CAM_RE, "camera.cpp")
    if cam is None:
        return 1
    src = open(HEADER, encoding="utf-8").read()
    ok = True

    for label, old, new in [
        ("kNightFullDeg drift",  "kNightFullDeg  = 10.0f",  "kNightFullDeg  = 12.0f"),
        ("kNightStartDeg drift", "kNightStartDeg = 25.0f",  "kNightStartDeg = 30.0f"),
    ]:
        if old not in src:
            print(f"  self-test {label:<24} SKIP (literal '{old}' not present — "
                  f"header reformatted; tighten this self-test)")
            ok = False
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False,
                                         encoding="utf-8") as f:
            f.write(src.replace(old, new))
            tmp = f.name
        try:
            hdr = extract(tmp, HDR_RE, "injected header")
            rc = 1 if hdr is None else compare(cam, hdr, quiet=True)
            status = "detected (rc=1)" if rc == 1 else "MISSED (rc=0)  <-- CHECK IS WORTHLESS"
            print(f"  self-test {label:<24} {status}")
            if rc != 1:
                ok = False
        finally:
            os.unlink(tmp)

    # And the untouched tree must be GREEN, else the check is a permanent red.
    hdr = extract(HEADER, HDR_RE, "terrain_daylight.h")
    rc = 1 if hdr is None else compare(cam, hdr, quiet=True)
    print(f"  self-test {'clean tree is green':<24} "
          f"{'yes (rc=0)' if rc == 0 else 'NO (rc=1)  <-- CHECK IS A FALSE ALARM'}")
    if rc != 0:
        ok = False

    print("check-terrain-daylight-constants --self-test: "
          + ("PASS (green on tree, red on both injected drifts)" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    quiet = "--quiet" in sys.argv
    cam = extract(CAMERA, CAM_RE, "camera.cpp")
    hdr = extract(HEADER, HDR_RE, "terrain_daylight.h")
    if cam is None or hdr is None:
        return 1
    return compare(cam, hdr, quiet=quiet)


if __name__ == "__main__":
    sys.exit(main())
