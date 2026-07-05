#!/usr/bin/env python3
"""TERRAIN-OVERLAY-V2-PARITY-1: offline extraction/authoring/validation harness
for the authored cement/pad/runway overlay sidecar
(`data/missions/<stem>.beauty/overlay_v2.png` + companion
`overlay_v2.bounds.txt`).

Engine contract (mclib/terrain.cpp, gated MC2_TERRAIN_OVERLAY_V2, search
"TERRAIN_OVERLAY_V2 v1" / gos_TerrainLodChunk_UploadOverlaySidecar;
shaders/terrain_lod_chunk.frag u_overlaySidecar/u_useOverlaySidecar):
  - RGBA8 PNG, ARBITRARY WxH (NOT tied to Terrain::realVerticesMapSide --
    the whole point of the sidecar is to decouple from the 128wu cement tile
    grid / vertex grid so authored overlays can be higher resolution and
    off-grid).
  - RGB = pre-tinted cement/overlay diffuse colour, A = coverage/edge alpha
    (0 = no overlay at this texel, >=0.5 counts as a legacy-parity "hit").
  - Sampled by WORLD XY in the frag: uv.x=(worldX-topLeftX)/sizeX,
    uv.y=(topLeftY-worldY)/sizeY -- SAME convention as the colormap atlas
    uniforms (u_atlasTopLeftX/Y) and control_map_tool.py's documented
    row-order (PNG row 0 = top of image = north edge = vertex row 0, no
    vertical flip). topLeftX = MIN world X (west edge), topLeftY = MAX world
    Y (north/top edge).
  - Companion "<name>.bounds.txt": 4 whitespace-separated floats
    "topLeftX topLeftY sizeX sizeY" in world units. Absent -> engine defaults
    to the full map extent ([-halfMap,+halfMap] both axes, halfMap =
    Terrain::worldUnitsMapSide/2).
  - Loader decode: mclib/control_map_png_decode.cpp ControlMapPng_DecodeRGBA()
    (same stb_image entry point control_map_tool.py uses; arbitrary WxH is
    fine -- there is no side==side check for this sidecar, unlike control_map).

Scope v1 (TERRAIN-OVERLAY-V2-PARITY-1 ruling #4): concrete PADS/RUNWAYS ONLY.
Classification mirrors mclib/terrain.cpp's terrainTypeToMaterial lambda /
control_map_tool.py's terrain_type_to_material EXACTLY (same _CONCRETE set) --
this IS the practical proxy for isCement() available from a baked pak (the
runtime isCement/isAlpha flags live on TerrainTXM, not in PostcompVertex).
Roads/runway AUTO-TILER ART and the separate overlay-pass PBR tint are
explicitly OUT of scope for v1 (ruling #4: pads/runways first, roads follow);
this v1 extractor renders a FLAT PER-CELL FILL matching the legacy "solid
cement" branch's per-tile colour (terrain_lod_chunk.frag ~line 540:
`base = cementColor`), which is the majority case for MC2 concrete pads.
Transition/edge-tint (the 0.82/0.80/0.76 darken from terrain_overlay.frag) is
OUT of scope for v1 -- that is EDGE-BLEND-1 (soft edges / detail masks).

Subcommands:
  extract   --pak <mission.pak> --out <png> [--supersample N]
      Rasterizes concrete cells (terrainType-classified) into a world-XY RGBA
      sidecar at vertex-resolution * supersample (default 1, i.e. one texel
      per vertex-cell -- matches the legacy per-tile flat fill exactly).
      Writes <out> and a companion "<out-stem>.bounds.txt" (full map extent).
      RGB = a flat authored "cement" grey (mid-grey placeholder -- this is a
      PARITY ORACLE for the classification/coverage, not a colour-matched
      re-texture; real art authoring is a follow-on). A = 255 where concrete,
      0 elsewhere.
  validate  --png <file> [--bounds <file>]
      RGBA8 format check; bounds file parse sanity (4 floats, sizeX/sizeY>0).
      Prints PASS/FAIL, exit 0/1.
  install   --png <file> --deploy <dir> --mission <stem>
      Copies to <deploy>/data/missions/<stem>.beauty/overlay_v2.png (+ the
      companion .bounds.txt alongside it if present next to --png).
  sheet     --png <file> --out <file>
      RGB + alpha contact sheet (reuses terrain_workbench's panel convention).

PIL + numpy only, offline, read-only w.r.t. .pak files. No engine launch, no
gameos import.
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
)

WORLD_UNITS_PER_VERTEX = 128.0

# Mirrors mclib/terrain.cpp terrainTypeToMaterial lambda / control_map_tool.py
# terrain_type_to_material EXACTLY (search "MUST mirror gos_terrain_indirect.cpp
# terrainTypeToMaterialLocal" in terrain.cpp ~line 806). Concrete = material 3.
_CONCRETE = {10, 13, 14, 15, 16, 17, 18, 19, 20}

# Placeholder flat "cement" diffuse colour for the v1 parity oracle (mid-grey,
# distinct from any stock terrain colormap hue so coverage is visually obvious
# in a contact sheet). Real art-matched colour is a follow-on (EDGE-BLEND-1 /
# authoring tool), not required for the coverage/gate parity proof here.
_CEMENT_RGB = (150, 150, 150)


def classify_concrete(terrtype: np.ndarray) -> np.ndarray:
    """terrtype: uint32 array of PostcompVertex.terrainType indices, any shape.
    Returns bool mask, True where the vertex classifies as concrete."""
    mask = np.zeros(terrtype.shape, dtype=bool)
    for v in _CONCRETE:
        mask |= (terrtype == v)
    return mask


_BRIDGE_DILATE = 2  # cells of water-adjacency treated as bridge/gate-likely


def read_bridge_gate_overlay_cells(side: int, blocks: bytes, water_elev: float) -> np.ndarray:
    """Returns a bool mask of cells that are BRIDGE/GATE-LIKELY (water-adjacent),
    for the caller to intersect with its own overlay/concrete tag mask.

    Cells carrying a runtime-mutable overlay (bridge/gate) MUST be excluded
    from the static bake (recon landmine #7 / #2 -- bridges mutate overlay at
    runtime, bldng.cpp:861-876).

    ROOT-BUG FIX (found by WHOLESALE-VECTORIZE-1, previously worked around
    locally in cook_markings.py): the original body returned
    `(overlay_hi != 0) & (overlay_hi != 0xFFFF)` -- i.e. "is this cell
    overlay-tagged AT ALL" -- which is not a bridge/gate discriminator, it is
    the exact same predicate used to detect ANY overlay cell (roads, runway
    paint, bridges, gates alike). Applied as an exclusion mask in cmd_extract,
    it wrongly dropped every overlay-tagged concrete cell from the sidecar
    bake, bridge or not (measured: 449/1170 concrete cells wrongly excluded on
    mc2_24, none of them within 2 cells of water).

    The real per-family tile decode (bridge#### vs road#### vs runway####)
    needs `TerrainTextures::baseTXMIndex`, a runtime texture-manager-assigned
    index that is not content-derivable offline (recon). So this uses the
    same WATER-ADJACENCY heuristic cook_markings.py adopted: bridges
    physically cross water, so a cell within `_BRIDGE_DILATE` cells of a
    water cell is treated as bridge/gate-likely; everything else (including
    plain overlay-tagged concrete on dry land) is kept by the caller.
    Conservative (errs toward excluding, never toward baking a runtime-
    mutable cell)."""
    water_elev_arr = np.float32(water_elev)
    layers = extract_layers(side, blocks, water_elev_arr)
    water = layers["water"]
    dilated = water.copy()
    for _ in range(_BRIDGE_DILATE):
        nxt = dilated.copy()
        nxt[1:, :] |= dilated[:-1, :]
        nxt[:-1, :] |= dilated[1:, :]
        nxt[:, 1:] |= dilated[:, :-1]
        nxt[:, :-1] |= dilated[:, 1:]
        dilated = nxt
    return dilated


# --- extract ------------------------------------------------------------------
def cmd_extract(args) -> int:
    pak = Path(args.pak)
    if not pak.is_file():
        print(f"[extract] ERROR pak not found: {pak}", file=sys.stderr)
        return 4
    packets = read_packets(pak)
    md = locate_mapdata(packets)
    if md is None:
        print(f"[extract] ERROR no MapData packet matched signature in {pak}", file=sys.stderr)
        return 4
    pkt_idx, side, blocks = md

    mission = pak.stem
    fit_path = pak.with_suffix(".fit")
    water_elev = read_water_elevation(fit_path)
    layers = extract_layers(side, blocks, water_elev)
    concrete = classify_concrete(layers["terrtype"])  # (side, side) bool, row0=top=north

    bridge_gate = read_bridge_gate_overlay_cells(side, blocks, water_elev)
    excluded = int((concrete & bridge_gate).sum())
    concrete = concrete & ~bridge_gate  # landmine #7/#2: never bake bridge/gate overlay cells

    ss = max(1, int(args.supersample))
    res = side * ss
    rgba = np.zeros((res, res, 4), dtype=np.uint8)
    concrete_up = np.repeat(np.repeat(concrete, ss, axis=0), ss, axis=1)
    rgba[..., 0] = np.where(concrete_up, _CEMENT_RGB[0], 0)
    rgba[..., 1] = np.where(concrete_up, _CEMENT_RGB[1], 0)
    rgba[..., 2] = np.where(concrete_up, _CEMENT_RGB[2], 0)
    rgba[..., 3] = np.where(concrete_up, 255, 0)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(out_path)

    # Bounds: full map extent, topLeftX=-halfMap (west), topLeftY=+halfMap
    # (north) -- matches mclib/terrain.cpp's default when no bounds file is
    # present, but writing it explicitly documents the mapping and lets
    # `install` ship it alongside the PNG.
    half_map = (side * WORLD_UNITS_PER_VERTEX) * 0.5
    bounds_path = out_path.with_suffix("")
    bounds_path = bounds_path.with_name(bounds_path.name + ".bounds.txt")
    bounds_path.write_text(f"{-half_map:.3f} {half_map:.3f} {2*half_map:.3f} {2*half_map:.3f}\n")

    concrete_count = int(concrete.sum())
    print(f"[extract] {mission}: side={side} supersample={ss} -> {out_path} "
          f"concrete_cells={concrete_count} bridge_gate_excluded={excluded} "
          f"bounds={bounds_path}")
    return 0


# --- validate -------------------------------------------------------------
def cmd_validate(args) -> int:
    png_path = Path(args.png)
    ok = True

    if not png_path.is_file():
        print(f"[validate] FAIL png not found: {png_path}")
        return 1

    img = Image.open(png_path)
    if img.mode != "RGBA":
        print(f"[validate] FAIL mode={img.mode}, want RGBA (8-bit/channel)")
        ok = False
    w, h = img.size
    print(f"[validate] size {w}x{h}")

    bounds_path = Path(args.bounds) if args.bounds else png_path.with_suffix("").with_name(
        png_path.with_suffix("").name + ".bounds.txt")
    if bounds_path.is_file():
        parts = bounds_path.read_text().split()
        if len(parts) != 4:
            print(f"[validate] FAIL bounds file {bounds_path} does not have 4 floats: {parts}")
            ok = False
        else:
            try:
                tlx, tly, sx, sy = (float(p) for p in parts)
                if sx <= 0 or sy <= 0:
                    print(f"[validate] FAIL bounds sizeX/sizeY must be > 0, got sx={sx} sy={sy}")
                    ok = False
                else:
                    print(f"[validate] bounds topLeftX={tlx} topLeftY={tly} sizeX={sx} sizeY={sy}: PASS")
            except ValueError:
                print(f"[validate] FAIL bounds file {bounds_path} not parseable as floats: {parts}")
                ok = False
    else:
        print(f"[validate] WARN no bounds file at {bounds_path} "
              "(engine will fall back to full-map-extent default)")

    arr = np.array(img.convert("RGBA"), dtype=np.uint8)
    coverage = int((arr[..., 3] >= 128).sum())
    total = arr.shape[0] * arr.shape[1]
    print(f"[validate] alpha coverage: {coverage}/{total} texels ({100.0*coverage/max(total,1):.1f}%)")

    print("[validate] PASS" if ok else "[validate] FAIL")
    return 0 if ok else 1


# --- install ----------------------------------------------------------------
def cmd_install(args) -> int:
    src = Path(args.png)
    if not src.is_file():
        print(f"[install] ERROR source png not found: {src}", file=sys.stderr)
        return 4
    dest_dir = Path(args.deploy) / "data" / "missions" / f"{args.mission}.beauty"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "overlay_v2.png"
    shutil.copyfile(src, dest)
    print(f"[install] {src} -> {dest}")

    src_bounds = src.with_suffix("").with_name(src.with_suffix("").name + ".bounds.txt")
    if src_bounds.is_file():
        dest_bounds = dest_dir / "overlay_v2.bounds.txt"
        shutil.copyfile(src_bounds, dest_bounds)
        print(f"[install] {src_bounds} -> {dest_bounds}")
    else:
        print(f"[install] WARN no bounds file found at {src_bounds} "
              "-- engine will use full-map-extent default")
    return 0


# --- sheet ------------------------------------------------------------------
PANEL = 220  # matches terrain_workbench.py's panel size convention


def _resize(rgb: np.ndarray, n: int = PANEL) -> Image.Image:
    return Image.fromarray(rgb, "RGB").resize((n, n), Image.NEAREST)


def cmd_sheet(args) -> int:
    png_path = Path(args.png)
    if not png_path.is_file():
        print(f"[sheet] ERROR png not found: {png_path}", file=sys.stderr)
        return 4
    arr = np.array(Image.open(png_path).convert("RGBA"))

    panels = []
    labels = []
    rgb = arr[..., :3].copy()
    panels.append(_resize(rgb))
    labels.append("RGB")
    a = arr[..., 3]
    gray_a = np.stack([a, a, a], axis=-1)
    panels.append(_resize(gray_a))
    labels.append("alpha")

    cols = len(panels)
    sheet = Image.new("RGB", (PANEL * cols, PANEL + 24), (30, 30, 30))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
    for i, (panel, label) in enumerate(zip(panels, labels)):
        sheet.paste(panel, (i * PANEL, 24))
        draw.text((i * PANEL + 4, 4), label, fill=(255, 255, 255))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path)
    print(f"[sheet] {png_path} -> {out_path}")
    return 0


# --- CLI ----------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="overlay_extract: offline extraction/authoring/validation for the terrain overlay-v2 sidecar")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract", help="classify stock mission concrete terrainType -> RGBA overlay sidecar + bounds")
    p.add_argument("--pak", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--supersample", type=int, default=1,
                    help="texels per vertex-cell per axis (default 1 = legacy-fill resolution)")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("validate", help="format + bounds-file sanity checks")
    p.add_argument("--png", required=True)
    p.add_argument("--bounds", help="explicit bounds file path (default: <png-stem>.bounds.txt)")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("install", help="copy png(+bounds) into <deploy>/data/missions/<stem>.beauty/overlay_v2.png")
    p.add_argument("--png", required=True)
    p.add_argument("--deploy", required=True)
    p.add_argument("--mission", required=True, help="mission stem, e.g. mc2_24")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("sheet", help="RGB + alpha contact sheet")
    p.add_argument("--png", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_sheet)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
