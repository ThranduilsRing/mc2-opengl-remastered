#!/usr/bin/env python3
"""tools/asset_cook/tests/test_g1_stage.py — G1 offline-stage geometry gate.

Stages the bigbox fixture and asserts the emitted geometry matches the runtime
importer (assimp_importer.cpp axis0 + auto-ground GROUND=2) — proving the cook's
transform replica is faithful, not the divergent workbench GlbMeshLoader.

Expected (hand-derived from a 12^3 cube centered at origin):
  axis0 keeps the symmetric cube +/-6; ground=2 shifts Y by +6 -> base at y=0.
  bounds min[-6,0,-6] max[6,12,6]; radius = center-to-corner = sqrt(108) ~ 10.3923.

  py -3 tools/asset_cook/tests/test_g1_stage.py
Exit 0 = pass.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COOK = HERE.parent / "trackg_cook.py"
FIXTURE = HERE / "fixtures" / "bigbox.glb"

EXPECTED_GEOM = {
    "convention": {"axis": 0, "vflip": True, "importer": "assimp_importer.v1"},
    "scale": 1.0,
    "bounds": {"min": [-6.0, 0.0, -6.0], "max": [6.0, 12.0, 6.0], "radius": 10.3923},
    "pivot": [0.0, 0.0, 0.0],
    "counts": {"verts": 24, "tris": 12, "submeshes": 1},
}


def approx(a, b, eps=1e-3):
    if isinstance(a, list):
        return len(a) == len(b) and all(approx(x, y, eps) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= eps
    return a == b


def main() -> int:
    errs: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        proc = subprocess.run(
            [sys.executable, str(COOK), "stage", str(FIXTURE), td,
             "--id", "bigbox", "--class", "staticprop", "--appearance", "hangar"],
            capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if proc.returncode != 0:
            print("FAIL stage exited nonzero:\n" + proc.stdout + proc.stderr)
            return 1
        staged = json.loads((Path(td) / "staged.json").read_text())
        if not (Path(td) / "bigbox.glb").exists():
            errs.append("cooked bigbox.glb not written")
        g = staged["geometry"]
        for key, exp in EXPECTED_GEOM.items():
            got = g.get(key)
            if key == "bounds":
                for bk in ("min", "max", "radius"):
                    if not approx(got.get(bk), exp[bk]):
                        errs.append(f"geometry.bounds.{bk}: got {got.get(bk)} want {exp[bk]}")
            elif key == "counts":
                if got != exp:
                    errs.append(f"geometry.counts: got {got} want {exp}")
            elif not approx(got, exp) and got != exp:
                errs.append(f"geometry.{key}: got {got} want {exp}")
        md = staged.get("materials_discovered", [])
        if len(md) != 1 or md[0].get("slot") != 0 or md[0].get("alphaClass") != 0:
            errs.append(f"materials_discovered unexpected: {md}")

    if errs:
        print(f"FAIL test_g1_stage ({len(errs)}):")
        for e in errs:
            print(f"  - {e}")
        return 1
    print("PASS test_g1_stage (bigbox stages to runtime-importer geometry: "
          "min[-6,0,-6] max[6,12,6] r~10.39, 24v/12t/1sm, convention recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
