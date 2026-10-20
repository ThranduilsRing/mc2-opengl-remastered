#!/usr/bin/env python3
"""DRAWBUFFER-OWNERSHIP-1 — draw-buffer set ownership consistency checker.

The draw-buffer set each pass renders into is now an explicit PipelineDesc field
(`drawBuffers`, RenderCore/PipelineDesc.h) instead of an implicit inheritance from
whatever the previous pass left bound. This checker keeps that field honest:

  A. SingleColor0 SAFETY — a row whose drawBuffers==SingleColor0 must NOT declare
     COLOR1/COLOR2 writes (colorAttachments.color1/color2). Only COLOR0 is in the
     draw set; a declared color1/2 write would be silently dropped.
  B. MainSceneMRT SANITY — a MainSceneMRT row must declare color0=true (it renders
     scene color into the MRT).
  C. CONTRACT-TABLE LOCKSTEP — for every registry row that maps to a pass in
     render_pass_attachment.h kContracts[], the declared draw set must match.

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "RenderCore" / "PipelineRegistry.cpp"
CONTRACTS = ROOT / "GameOS" / "gameos" / "render_pass_attachment.h"

ROW_HEADER = re.compile(r"//\s*\[(\d+)\]\s*([A-Za-z]\w+)\b")
COLOR_ATT = re.compile(
    r"/\*\s*colorAttachments\s*\*/\s*\{\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}")
DRAWBUF = re.compile(r"/\*\s*drawBuffers\s*\*/\s*DrawBufferSet::(\w+)")
CONTRACT_ENTRY = re.compile(
    r'\{\s*"(\w+)"\s*,\s*TargetFbo::\w+\s*,\s*DrawBufferSetId::(\w+)', re.DOTALL)

# registry row name -> contract-table pass key
NAME_MAP = {
    "TerrainSolid": "TerrainSolid", "MechOpaque": "MechOpaque",
    "StaticPropOpaque": "StaticPropOpaque", "WaterArmed": "WaterFastPath",
    "VfxBillboardAlpha": "VfxBillboard", "VfxBillboardAdditive": "VfxBillboard",
    "VfxTubeAlpha": "VfxTube", "VfxTubeAdditive": "VfxTube",
    "VfxMeshAlpha": "VfxMesh", "VfxMeshAdditive": "VfxMesh",
    "PostProcessCloudShadow": "CloudShadow", "PostProcessShoreline": "Shoreline",
    "PostProcessEdgeFog": "EdgeFog", "PostProcessFogOob": "FogOob",
}


def parse_registry(text: str):
    rows = {}
    name = None; catt = None; dbuf = None
    for ln in text.splitlines():
        h = ROW_HEADER.search(ln)
        if h:
            name = h.group(2); catt = None; dbuf = None
            continue
        if name:
            c = COLOR_ATT.search(ln)
            if c:
                catt = (c.group(1), c.group(2), c.group(3))
            d = DRAWBUF.search(ln)
            if d:
                dbuf = d.group(1)
                rows[name] = {"catt": catt, "dbuf": dbuf}
                name = None
    return rows


def main() -> int:
    quiet = "--quiet" in sys.argv
    for f in (REGISTRY, CONTRACTS):
        if not f.exists():
            print(f"[drawbuf] ERROR missing: {f}", file=sys.stderr)
            return 2

    rows = parse_registry(REGISTRY.read_text(encoding="utf-8", errors="replace"))
    if not rows:
        print("[drawbuf] ERROR no rows parsed", file=sys.stderr)
        return 2

    ctext = CONTRACTS.read_text(encoding="utf-8", errors="replace")
    start = ctext.find("kContracts[] = {")
    contract_db = {m.group(1): m.group(2)
                   for m in CONTRACT_ENTRY.finditer(ctext[start:] if start >= 0 else ctext)}

    fails = []
    for name, r in rows.items():
        db = r["dbuf"]; catt = r["catt"]
        if catt is None:
            fails.append(f"{name}: drawBuffers set but no colorAttachments parsed")
            continue
        c0, c1, c2 = (catt[0] == "true", catt[1] == "true", catt[2] == "true")
        # A
        if db == "SingleColor0" and (c1 or c2):
            fails.append(f"{name}: drawBuffers=SingleColor0 but colorAttachments "
                         f"declares COLOR1/COLOR2 write — would be dropped")
        # B
        if db == "MainSceneMRT" and not c0:
            fails.append(f"{name}: drawBuffers=MainSceneMRT but color0=false")
        # C
        key = NAME_MAP.get(name)
        if key and key in contract_db and contract_db[key] != db:
            fails.append(f"{name}: drawBuffers={db} but render_pass_attachment.h "
                         f"contract '{key}'={contract_db[key]} — out of lockstep")

    if fails:
        print("[drawbuf] FAIL — draw-buffer ownership inconsistencies:", file=sys.stderr)
        for x in fails:
            print(f"  - {x}", file=sys.stderr)
        return 1
    if not quiet:
        print(f"[drawbuf] PASS — {len(rows)} rows: SingleColor0 safe, MainSceneMRT "
              f"sane, contract-table lockstep.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
