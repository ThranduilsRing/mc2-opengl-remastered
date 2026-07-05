#!/usr/bin/env python3
"""TERRAIN-SHORELINE-V3 (was MASK-1): offline cook for the land-side wet/foam
shoreline sidecar (`data/missions/<stem>.beauty/shoreline_mask.png` + companion
`shoreline_mask.bounds.txt`).

DEMOTED ROLE (V3): this mask is now an OPTIONAL MODULATOR, not the band's
placement source. v1/v2 placed the band by projecting this mask's world-XY
signed-distance field onto the terrain -- root cause of the "zigzag AND floats
up-slope" defect: the EDT here is cooked from a height SOURCE (coarse pak grid
or a 16x-smoothed bake) that never exactly equals the RENDERED waterline (the
coarse mesh's bilinear-interpolated surface), so a horizontal-distance mask
either staircases (faithful source) or floats above the true shoreline
(smoothed source). V3 moved PLACEMENT into the frag shader itself: wetness =
smoothstep over (v_worldPos.z - u_waterElevation), which hugs the drawn
waterline by construction (v_worldPos.z IS the interpolated height the
rasterizer produced) at any LOD or slope, no cook required. Gate
MC2_TERRAIN_SHORELINE alone now produces full elevation bands with NO sidecar.
This cook's output, when present, is still consumed -- its G/B channels
MULTIPLICATIVELY MODULATE the elevation bands (e.g. wide-beach falloff on
flat shores, or narrowing/excluding a below-water basin) -- but it is no
longer required, and its own coarse-grid stair-steps can no longer create a
misplaced band on their own (only shape an already-correctly-placed one).
No cook-side changes were made this slice; the math below (EDT/bands) is
unchanged and still valid as a modulator source.

Engine contract (mclib/terrain.cpp, gated MC2_TERRAIN_SHORELINE; loader
gos_TerrainLodChunk_UploadShorelineMask; shaders/terrain_lod_chunk.frag
u_shorelineMask/u_useShorelineMask/u_hasShorelineMask/u_shorelineBounds/
u_waterElevation):
  - RGBA8 PNG, ARBITRARY WxH -- world-XY sampled (overlay-V2 sidecar pattern),
    NOT tied to the vertex grid, so mask resolution is decoupled from the
    coarse pak grid and can carry sub-tile shoreline curvature.
  - Channels: R = normalized signed distance to the water edge (0.5 = exact
    waterline; <0.5 = water side, >0.5 = land side; scaled by BAND_NORM_WU so
    the 0..1 range covers +/-BAND_NORM_WU world units around the edge).
    G = wet weight (0..1, land-side damp/wet ramp, 0 at >=wet_w from edge).
    B = foam weight (0..1, edge lobe, 0 outside +/-foam_w of the edge).
    A = valid/coverage (255 inside the cooked mission bounds, 0 outside --
    always 255 for a full-map cook; kept for future partial-region cooks).
  - Sampled by WORLD XY in the frag: uv.x=(worldX-topLeftX)/sizeX,
    uv.y=(topLeftY-worldY)/sizeY -- SAME convention as u_overlayBounds /
    u_atlasTopLeftX/Y (PNG row 0 = top of image = north edge = vertex row 0,
    no vertical flip).
  - Companion "<name>.bounds.txt": 4 whitespace-separated floats
    "topLeftX topLeftY sizeX sizeY" in world units (overlay-V2 sidecar
    convention). Always written (full map extent for v1).

Signal source (USER AMENDMENT, critical): the EDT MUST source from the
HI-RES heightfield when available -- `<stem>.beauty/visual_height_4x.r32`
(V=(side-1)*4+1, mclib/terrain.cpp ~line 1057) or an original-import .r32 --
NOT the coarse pak grid, so the mask carries the TRUE waterline curve at
sub-tile precision. Falls back to the coarse pak height (mission_terrain_
analyzer's `elev`) only when no hi-res source exists. Water level is ALWAYS
read from [Water].Elevation in the mission .fit via
mission_terrain_analyzer.read_water_elevation -- never the PostcompVertex
.water byte (that field is a packed alpha, not a bool; see analyzer:178-186).

Mask math: signed Euclidean distance transform (scipy.ndimage.
distance_transform_edt) of the water/land boolean split, signed water-
negative/land-positive, in WORLD UNITS (grid spacing = world units per mask
cell, NOT per vertex -- the hi-res grid is finer than 128wu/vertex).
  dry            : d > damp_w                      -> G=0, B=0
  damp/wet band   : 0 < d < damp_w  (land side)      -> G ramps 1->0
  foam edge       : |d| < foam_w                     -> B ramps 1->0 at |d|=foam_w
  shallow/deep    : d < 0 (water side)                -> G=0 (terrain-side mask
                     does not paint the water surface; water FS owns that -- v1
                     is terrain-side-only per recon ruling).

Band widths have a FLOOR (recon landmine #4, mirrors shoreBlend=max(alphaDepth,
1.0)): damp_w/wet_w/foam_w are configurable but always >= a minimum so the
mask never degenerates to a zero-width band.

Subcommands:
  cook      --pak <mission.pak> [--fit <mission.fit>] [--visual-height <r32>]
            --out <png> [--damp-width WU] [--foam-width WU] [--supersample N]
      Cooks the RGBA shoreline mask + companion bounds.txt. --visual-height
      overrides hi-res source auto-detection (default: look for
      "<pak-stem>.beauty/visual_height_4x.r32" next to the pak, else fall
      back to the pak's own coarse heightfield).
  validate  --png <file> [--bounds <file>]
      RGBA8 format check; bounds file parse sanity (4 floats, sizeX/sizeY>0).
      Prints PASS/FAIL, exit 0/1.
  install   --png <file> --deploy <dir> --mission <stem>
      Copies to <deploy>/data/missions/<stem>.beauty/shoreline_mask.png (+ the
      companion .bounds.txt).
  sheet     --png <file> --out <file> [--height <png-or-r32>]
      R/G/B/A channel contact sheet (reuses terrain_workbench's panel
      convention), optionally composited over a height hillshade panel.

PIL + numpy + scipy only, offline, read-only w.r.t. .pak/.fit files. No engine
launch, no gameos import.
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

try:
    from scipy.ndimage import distance_transform_edt
except ImportError:  # pragma: no cover - scipy is an existing beautify dep
    distance_transform_edt = None

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
)

WORLD_UNITS_PER_VERTEX = 128.0

# Band-width floors (recon landmine #4): a band derived from a runtime/authored
# parameter must never collapse to zero width.
MIN_BAND_WU = 1.0
# VISUAL-QUALITY FIX (post-ship, user report: bands read as huge bright "paint
# stripes" on mc2_17): the original 64wu damp / 24wu foam solid bands were far
# too wide/bright at the coarse-grid resolution these cooks actually shipped
# at. Narrowed to the target range from the fix brief -- damp 24-32wu subtle
# darkening, foam 4-8wu narrow noise-broken rim -- with headroom kept in the
# CLI flags for per-mission tuning.
DEFAULT_DAMP_WIDTH_WU = 28.0    # land-side wet/damp darken ramp extent
DEFAULT_FOAM_WIDTH_WU = 6.0     # foam lobe half-width straddling the waterline
# R-channel normalization: distance range mapped into [0,1] (0.5 = waterline).
# Wide enough to cover the default damp band with headroom before clamping.
BAND_NORM_WU = 128.0


def _load_visual_height(path: Path) -> tuple[np.ndarray, int] | None:
    """Load a visual_height_4x.r32-style raw float grid. Returns (grid, V) or
    None if the file doesn't exist / isn't a valid square grid."""
    if not path.is_file():
        return None
    data = np.fromfile(path, dtype="<f4")
    v = int(round(data.size ** 0.5))
    if v * v != data.size:
        return None
    return data.reshape(v, v).astype(np.float64), v


def _default_visual_height_path(pak: Path) -> Path:
    return pak.parent / f"{pak.stem}.beauty" / "visual_height_4x.r32"


def compute_signed_distance_wu(water: np.ndarray, cell_wu: float) -> np.ndarray:
    """water: bool grid (True = water cell). Returns signed EDT in WORLD UNITS,
    negative on the water side, positive on the land side, ~0 at the boundary.
    Uses scipy's distance_transform_edt (already a beautify dependency, see
    recon 2 recommendation) on both water and land masks and combines."""
    if distance_transform_edt is None:
        raise RuntimeError("scipy is required for cook_shoreline.py (distance_transform_edt)")
    land = ~water
    # distance_transform_edt(mask) = distance from each False cell to nearest True
    # cell; sampling in cell units, scaled to world units via `sampling`.
    dist_to_water = distance_transform_edt(~water, sampling=cell_wu)  # 0 inside water
    dist_to_land = distance_transform_edt(~land, sampling=cell_wu)    # 0 inside land
    signed = np.where(water, -dist_to_land, dist_to_water)
    return signed


def build_mask_rgba(signed_d_wu: np.ndarray, damp_w: float, foam_w: float) -> np.ndarray:
    """signed_d_wu: water-negative/land-positive signed distance in WU.
    Returns HxWx4 uint8 RGBA (R=norm dist, G=wet weight, B=foam weight, A=255)."""
    damp_w = max(damp_w, MIN_BAND_WU)
    foam_w = max(foam_w, MIN_BAND_WU)

    # R: normalized signed distance, 0.5 = waterline, clamped to [0,1].
    r_norm = np.clip(signed_d_wu / BAND_NORM_WU * 0.5 + 0.5, 0.0, 1.0)

    # G: wet/damp weight on the LAND side only (d in [0, damp_w] -> 1..0 ramp).
    # Terrain-side-only v1 (recon Sec.3): water-side (d<0) gets G=0, the water
    # FS already owns the water-surface colour ramp. NOTE: clip(d,0,None) alone
    # would map d<0 (water) to land_d=0 -> wet=1, painting the whole water body
    # "wet" -- the explicit `np.where(signed_d_wu > 0, ..., 0.0)` guard below is
    # required so water-side pixels are hard-zeroed regardless of magnitude.
    land_d = np.clip(signed_d_wu, 0.0, None)
    wet = np.where(signed_d_wu > 0.0, np.clip(1.0 - land_d / damp_w, 0.0, 1.0), 0.0)

    # B: foam lobe straddling the waterline (|d| < foam_w -> 1..0 ramp), both
    # sides so the foam rim reads on the water edge itself.
    foam = np.clip(1.0 - np.abs(signed_d_wu) / foam_w, 0.0, 1.0)

    h, w = signed_d_wu.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[..., 0] = (r_norm * 255.0 + 0.5).astype(np.uint8)
    rgba[..., 1] = (wet * 255.0 + 0.5).astype(np.uint8)
    rgba[..., 2] = (foam * 255.0 + 0.5).astype(np.uint8)
    rgba[..., 3] = 255
    return rgba


# --- cook ---------------------------------------------------------------------
def cook_one(pak: Path, out_path: Path, *, fit: Path | None = None,
             visual_height: Path | None = None, damp_width: float = DEFAULT_DAMP_WIDTH_WU,
             foam_width: float = DEFAULT_FOAM_WIDTH_WU, supersample: int = 1,
             quiet: bool = False) -> dict:
    """Cook one shoreline mask + bounds. Returns a census dict
    (mission/side/water_elev/water_cells/wet_cells/foam_cells/water_pct/out/
    bounds/source), or {"error": ...} on failure. Shared by cmd_cook and
    cmd_all_missions so the single-mission and batch paths cook identically."""
    if not pak.is_file():
        return {"mission": pak.stem, "error": f"pak not found: {pak}"}
    packets = read_packets(pak)
    md = locate_mapdata(packets)
    if md is None:
        return {"mission": pak.stem, "error": f"no MapData packet in {pak}"}
    _pkt_idx, side, blocks = md

    fit_path = fit if fit else pak.with_suffix(".fit")
    water_elev = read_water_elevation(fit_path)
    layers = extract_layers(side, blocks, water_elev)
    coarse_elev = layers["elev"]  # (side, side), row0=top=north

    # --- hi-res source selection (USER AMENDMENT) ---
    vh_path = visual_height if visual_height else _default_visual_height_path(pak)
    vh = _load_visual_height(vh_path)
    if vh is not None:
        elev, grid_side = vh
        factor = (grid_side - 1) / (side - 1) if side > 1 else 1.0
        cell_wu = WORLD_UNITS_PER_VERTEX / factor
        source = f"hi-res visual_height ({vh_path.name}, V={grid_side}, factor={factor:.2f})"
    else:
        elev, grid_side = coarse_elev, side
        cell_wu = WORLD_UNITS_PER_VERTEX
        source = "coarse pak heightfield (no hi-res bake found — fallback)"

    water = elev <= water_elev
    ss = max(1, int(supersample))
    if ss > 1:
        # nearest-neighbour upsample of the boolean water field before EDT so
        # the distance transform itself runs at the requested output resolution
        # (keeps the sub-tile curve rather than upsampling a blocky distance
        # field after the fact).
        water = np.repeat(np.repeat(water, ss, axis=0), ss, axis=1)
        cell_wu = cell_wu / ss
        grid_side = grid_side * ss

    signed_d = compute_signed_distance_wu(water, cell_wu)
    damp_w = max(float(damp_width), MIN_BAND_WU)
    foam_w = max(float(foam_width), MIN_BAND_WU)
    rgba = build_mask_rgba(signed_d, damp_w, foam_w)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(out_path)

    # Bounds: full map extent (overlay-V2 convention: topLeftX=-halfMap west,
    # topLeftY=+halfMap north).
    half_map = (side * WORLD_UNITS_PER_VERTEX) * 0.5
    bounds_path = out_path.with_suffix("")
    bounds_path = bounds_path.with_name(bounds_path.name + ".bounds.txt")
    bounds_path.write_text(f"{-half_map:.3f} {half_map:.3f} {2*half_map:.3f} {2*half_map:.3f}\n")

    n = int(coarse_elev.size)
    water_cells = int((coarse_elev <= water_elev).sum())  # coarse-grid water census
    foam_cells = int((rgba[..., 2] > 0).sum())
    wet_cells = int((rgba[..., 1] > 0).sum())
    census = {
        "mission": pak.stem, "side": int(side), "grid_side": int(grid_side),
        "source": source, "water_elev": float(water_elev),
        "damp_w": float(damp_w), "foam_w": float(foam_w), "supersample": int(ss),
        "water_cells": water_cells, "wet_cells": wet_cells, "foam_cells": foam_cells,
        "water_pct": round(100.0 * water_cells / max(n, 1), 3),
        "out": str(out_path), "bounds": str(bounds_path),
    }
    if not quiet:
        print(f"[cook] {pak.stem}: side={side} mask={grid_side}x{grid_side} "
              f"source={source} water_elev={water_elev:.2f}wu "
              f"damp_w={damp_w:.1f}wu foam_w={foam_w:.1f}wu "
              f"water_cells={water_cells} wet_cells={wet_cells} foam_cells={foam_cells} "
              f"-> {out_path} bounds={bounds_path}")
    return census


def cmd_cook(args) -> int:
    r = cook_one(
        Path(args.pak), Path(args.out),
        fit=Path(args.fit) if args.fit else None,
        visual_height=Path(args.visual_height) if args.visual_height else None,
        damp_width=args.damp_width, foam_width=args.foam_width,
        supersample=args.supersample)
    if r.get("error"):
        print(f"[cook] ERROR {r['error']}", file=sys.stderr)
        return 4
    return 0


# --- all-missions (SHORELINE-BATCH-COOK-1) -----------------------------------
def cmd_all_missions(args) -> int:
    """Batch-cook shoreline masks for every water-bearing mission in a dir.

    Water detection: [Water].Elevation from the mission .fit vs the coarse pak
    heightfield (analyzer.read_water_elevation + elev<=elev) -- NEVER the
    PostcompVertex .water byte (packed alpha, not a bool). Missions with zero
    water cells are skipped and reported in the census.

    Sources the hi-res bake from <missions-dir-or-beauty-root>/<stem>.beauty/
    visual_height_4x.r32 when present (falls back to the coarse pak grid, which
    at 128wu cells yields empty narrow bands -- run the bake first, or pass
    --supersample). Writes <out>/<stem>.beauty/shoreline_mask.png (+ bounds) and
    an all_missions_census.json."""
    import glob as _glob
    import json as _json

    mdir = Path(args.missions_dir)
    beauty_root = Path(args.beauty_root) if args.beauty_root else mdir
    out_root = Path(args.out_root)
    paks = sorted(mdir.glob("*.pak"))
    if not paks:
        print(f"[all-missions] ERROR no .pak files in {mdir}", file=sys.stderr)
        return 4

    census = {"missions_dir": str(mdir), "beauty_root": str(beauty_root),
              "out_root": str(out_root), "damp_w": args.damp_width,
              "foam_w": args.foam_width, "supersample": args.supersample,
              "cooked": [], "skipped": []}
    rc = 0
    for pak in paks:
        stem = pak.stem
        md = locate_mapdata(read_packets(pak))
        if md is None:
            census["skipped"].append({"mission": stem, "reason": "no MapData packet"})
            continue
        _idx, side, blocks = md
        welev = read_water_elevation(pak.with_suffix(".fit"))
        layers = extract_layers(side, blocks, welev)
        wcells = int(layers["water"].sum())
        if wcells == 0:
            census["skipped"].append({"mission": stem, "reason": "no water cells",
                                      "water_elev": float(welev)})
            print(f"[all-missions] skip {stem}: no water (elev={welev:.2f}wu)")
            continue
        vh = beauty_root / f"{stem}.beauty" / "visual_height_4x.r32"
        out = out_root / f"{stem}.beauty" / "shoreline_mask.png"
        r = cook_one(pak, out, visual_height=vh if vh.is_file() else None,
                     damp_width=args.damp_width, foam_width=args.foam_width,
                     supersample=args.supersample)
        if r.get("error"):
            census["skipped"].append({"mission": stem, "reason": r["error"]})
            print(f"[all-missions] FAIL {stem}: {r['error']}", file=sys.stderr)
            rc = 1
            continue
        census["cooked"].append(r)

    if args.census:
        Path(args.census).parent.mkdir(parents=True, exist_ok=True)
        Path(args.census).write_text(_json.dumps(census, indent=2), encoding="utf-8")
        print(f"[all-missions] census -> {args.census}")
    print(f"[all-missions] cooked={len(census['cooked'])} skipped={len(census['skipped'])}")
    return rc


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
    waterline_texels = int(((arr[..., 0].astype(np.int32) - 128) ** 2 < 4).sum())
    total = arr.shape[0] * arr.shape[1]
    print(f"[validate] near-waterline (R~=128) texels: {waterline_texels}/{total} "
          f"({100.0*waterline_texels/max(total,1):.2f}%)")

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
    dest = dest_dir / "shoreline_mask.png"
    shutil.copyfile(src, dest)
    print(f"[install] {src} -> {dest}")

    src_bounds = src.with_suffix("").with_name(src.with_suffix("").name + ".bounds.txt")
    if src_bounds.is_file():
        dest_bounds = dest_dir / "shoreline_mask.bounds.txt"
        shutil.copyfile(src_bounds, dest_bounds)
        print(f"[install] {src_bounds} -> {dest_bounds}")
    else:
        print(f"[install] WARN no bounds file found at {src_bounds} "
              "-- engine will use full-map-extent default")
    return 0


# --- sheet ------------------------------------------------------------------
PANEL = 220  # matches terrain_workbench.py's panel size convention


def _resize_gray(chan: np.ndarray, n: int = PANEL) -> Image.Image:
    rgb = np.stack([chan, chan, chan], axis=-1)
    return Image.fromarray(rgb, "RGB").resize((n, n), Image.NEAREST)


def cmd_sheet(args) -> int:
    png_path = Path(args.png)
    if not png_path.is_file():
        print(f"[sheet] ERROR png not found: {png_path}", file=sys.stderr)
        return 4
    arr = np.array(Image.open(png_path).convert("RGBA"))

    panels = [
        _resize_gray(arr[..., 0]),
        _resize_gray(arr[..., 1]),
        _resize_gray(arr[..., 2]),
        _resize_gray(arr[..., 3]),
    ]
    labels = ["R dist (waterline=128)", "G wet", "B foam", "A valid"]

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
    ap = argparse.ArgumentParser(description="cook_shoreline: offline EDT-based shoreline wet/foam mask cook")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("cook", help="cook a signed-EDT wet/foam RGBA mask + bounds from a stock mission")
    p.add_argument("--pak", required=True)
    p.add_argument("--fit", help="override .fit path (default: <pak-stem>.fit)")
    p.add_argument("--visual-height", help="override hi-res .r32 path "
                   "(default: <pak-stem>.beauty/visual_height_4x.r32 next to the pak, "
                   "else falls back to the coarse pak heightfield)")
    p.add_argument("--out", required=True)
    p.add_argument("--damp-width", type=float, default=DEFAULT_DAMP_WIDTH_WU,
                   help=f"land-side wet/damp band width in world units (default {DEFAULT_DAMP_WIDTH_WU})")
    p.add_argument("--foam-width", type=float, default=DEFAULT_FOAM_WIDTH_WU,
                   help=f"foam lobe half-width in world units (default {DEFAULT_FOAM_WIDTH_WU})")
    p.add_argument("--supersample", type=int, default=1,
                   help="extra upsample factor applied to the water mask before the EDT (default 1)")
    p.set_defaults(func=cmd_cook)

    p = sub.add_parser("all-missions", help="SHORELINE-BATCH-COOK-1: cook masks for "
                       "every water-bearing mission in a dir (skips dry maps, emits census)")
    p.add_argument("--missions-dir", required=True,
                   help="dir of stock <stem>.pak/.fit missions")
    p.add_argument("--beauty-root",
                   help="dir holding <stem>.beauty/visual_height_4x.r32 hi-res bakes "
                        "(default: --missions-dir); falls back to coarse pak grid if absent")
    p.add_argument("--out-root", required=True,
                   help="writes <out-root>/<stem>.beauty/shoreline_mask.png (+bounds)")
    p.add_argument("--damp-width", type=float, default=DEFAULT_DAMP_WIDTH_WU)
    p.add_argument("--foam-width", type=float, default=DEFAULT_FOAM_WIDTH_WU)
    p.add_argument("--supersample", type=int, default=8,
                   help="EDT upsample so the default 28/6wu bands are representable "
                        "against a 4x hi-res bake (32wu cells at ss=1 => empty bands); "
                        "default 8 => 4wu cells")
    p.add_argument("--census", help="write a JSON census (mission -> water/band stats)")
    p.set_defaults(func=cmd_all_missions)

    p = sub.add_parser("validate", help="format + bounds-file sanity checks")
    p.add_argument("--png", required=True)
    p.add_argument("--bounds", help="explicit bounds file path (default: <png-stem>.bounds.txt)")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("install", help="copy png(+bounds) into <deploy>/data/missions/<stem>.beauty/shoreline_mask.png")
    p.add_argument("--png", required=True)
    p.add_argument("--deploy", required=True)
    p.add_argument("--mission", required=True, help="mission stem, e.g. mc2_17")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("sheet", help="R/G/B/A channel contact sheet")
    p.add_argument("--png", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_sheet)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
