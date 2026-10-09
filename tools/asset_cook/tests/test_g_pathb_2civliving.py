#!/usr/bin/env python3
"""tools/asset_cook/tests/test_g_pathb_2civliving.py — Track G path B transform gate.

Path B = stock .tgl -> (workbench --export-tgl-meshdump) -> meshdump.json ->
tglmeshdump_to_glb (calibrated `-x,-z,y`) -> stage.

CALIBRATED MAPPING (visually confirmed 2026-06-04, see docs/assets/trackg-r0/, archived to E:/mc2-archive-2026-10/docs/claude-evidence-2026-10-09.tar.gz):
the workbench MeshData is GL-space (-sx, sz, sy). The engine renders stock props
in (-sx, sy, sz). So the cooked glb maps MeshData -> (-mx,-mz,my) and the importer
axisMap0 yields engine (-sx, sy, sz). Net effect on extents: engine X == stock X,
but engine Y == stock-Z and engine Z == stock-Y (Stuff Y/Z preserved, NOT the
workbench's GL swap). The earlier `-x,-y,z` produced a 90deg-X rotation (Y/Z swap)
that passed a naive extents oracle but rendered wrong — so this test asserts the
SWAPPED mapping, and the screenshot is the real gate (extents alone are
mirror/rotation-blind).

  py -3 tools/asset_cook/tests/test_g_pathb_2civliving.py
Exit 0 = pass. Uses the committed meshdump fixture (no workbench rebuild needed).
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COOK = HERE.parent / "trackg_cook.py"
TOGLB = HERE.parent / "tglmeshdump_to_glb.py"
DUMP = HERE / "fixtures" / "2civliving_meshdump.json"


def run(*a):
    return subprocess.run([sys.executable, *map(str, a)], capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def ext(mn, mx):
    return [round(mx[i] - mn[i], 2) for i in range(3)]


def main() -> int:
    if not DUMP.exists():
        print(f"SKIP test_g_pathb_2civliving (fixture missing: {DUMP})")
        return 0
    dump = json.loads(DUMP.read_text())
    se = ext(dump["bmin"], dump["bmax"])           # stock MeshData extents [X, Ygl, Zgl]
    # calibrated engine extents: X kept, MeshData Y/Z swapped (Stuff Y/Z preserved)
    expected = [se[0], se[2], se[1]]

    errs = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        glb = td / "2civliving.glb"
        r = run(TOGLB, DUMP, glb)
        if r.returncode != 0:
            print("FAIL toglb:\n" + r.stdout + r.stderr); return 1
        r = run(COOK, "stage", glb, td / "out", "--id", "2civliving",
                "--class", "staticprop", "--appearance", "2civliving")
        if r.returncode != 0:
            print("FAIL stage:\n" + r.stdout + r.stderr); return 1
        b = json.loads((td / "out" / "staged.json").read_text())["geometry"]["bounds"]
        stage_ext = ext(b["min"], b["max"])

        if any(abs(expected[i] - stage_ext[i]) > 0.05 for i in range(3)):
            errs.append(f"extent mismatch: calibrated-expected {expected} vs staged {stage_ext} "
                        f"(stock MeshData {se}); transform `-x,-z,y` not applied?")
        if abs(b["min"][1]) > 0.01:
            errs.append(f"importer-Y not grounded to 0: min.y={b['min'][1]}")

    if errs:
        print("FAIL test_g_pathb_2civliving:")
        for e in errs:
            print(f"  - {e}")
        return 1
    print(f"PASS test_g_pathb_2civliving (calibrated `-x,-z,y`: engine extents {expected} "
          f"= stock {se} with Y/Z swapped to Stuff order; visually confirmed). "
          f"NB: extents are mirror/rotation-blind — screenshot is the real gate.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
