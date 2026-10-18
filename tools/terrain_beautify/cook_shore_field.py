#!/usr/bin/env python3
"""SHORE-FIELD-1: offline cook of the smooth coastline field the VK water path
draws its shoreline AND its deep/shallow depth tint from.

WHY
---
Every mission's coast comes out of a 128 wu heightmap, and several stock maps
are terraced (mc2_10 staircase index 0.737), so any edge derived from that grid
is a 45-degree staircase with jut-outs; visual_height_4x.r32 (32 wu) is smoother
but keeps the terraces where the stock data has them. The water's deep/shallow
tint was interpolated per coarse vertex, so a single low vertex in shallow water
drew a dark diamond ("inverted pyramid"). Memory note
terrain/shoreline_staircase_root_cause_2026_10_08.md: nothing derived only from
the coarse data beats the quantization.

RECIPE (owner + outside reviewer, 2026-10-18: "upsample, then erode only at the shore")
------
  1. continuous field: the coarse pak heightfield the terrain mesh is drawn from
     (--source coarse, default; --source visual uses visual_height_4x.r32, whose
     re-authored coast drifts from the rendered land), linearly upsampled
     (as the terrain triangles interpolate it) --upsample x over the 128 wu grid (default 16 -> 8 wu cells), corner-aligned
     (side = (n-1)*f+1, texel centres on grid vertices).
  2. threshold at the mission water level -> land/water; morphological open
     (removes spikes) and close (fills notches).
  3. signed distance field, + land / - water, in world units.
  4. Gaussian passes over the SDF restricted to a feathered coastal band, so
     far inland and deep water are unaffected. The shoreline is SDF = 0.
  5. bake a PSEUDO-HEIGHT the water FS reads like a height (field = water - depth):
       depth = the real depth, Gaussian-smoothed (authored shallow shelves keep their
       tint and translucency; lone low vertices stop drawing diamonds), with its sign
       forced to that of -SDF by a gentle floor ramp (shore_slope * |SDF|), so the
       edge is exactly SDF = 0.
  6. store at --store x over visual (default 2 -> 16 wu) as `<stem>.beauty/shore_field.r16f`:
     16-byte header (SHF1, side, water elevation) + float16 of (height - water elevation).
Gameplay terrain heights are untouched; only the water edge/tint reads this.
The engine (mclib/terrain_init.cpp lodWaterShore) prefers shore_field.r16f over
visual_height_4x.r32 for the water shore texture; shore mode 3 cuts the edge
and the depth tint from it (shaders/vulkan/water_geom.frag).

USAGE
-----
  py -3 tools/terrain_beautify/cook_shore_field.py cook --pak <m>.pak --out <dir>/<m>.beauty/shore_field.r16f
  py -3 tools/terrain_beautify/cook_shore_field.py all-missions --missions-dir <install>/data/missions \\
        [--beauty-root tests/terrain/beautify] --out-root <dir>
  py -3 tools/terrain_beautify/cook_shore_field.py preview --pak <m>.pak --png out.png [--crop x0,y0,x1,y1]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
)

WORLD_UNITS_PER_VERTEX = 128.0
FIELD_NAME = "shore_field.r16f"

DEFAULT_SOURCE = "coarse"
DEFAULT_UPSAMPLE = 16          # over the coarse 128 wu grid -> 8 wu cells (4 over visual_height_4x)
DEFAULT_STORE = 2              # over visual_height_4x -> 16 wu cells stored
DEFAULT_OPEN_WU = 16.0         # morphological open radius (spikes), wu
DEFAULT_CLOSE_WU = 16.0        # morphological close radius (notches), wu
DEFAULT_SDF_SIGMA_WU = 48.0    # Gaussian per smoothing pass over the SDF, wu
DEFAULT_SDF_PASSES = 4
DEFAULT_BAND_WU = 320.0        # coastal band: SDF smoothing + the shore depth ramp live inside it
DEFAULT_DEPTH_SIGMA_WU = 96.0  # smoothing of the real depth used beyond the band
DEFAULT_SHORE_SLOPE = 0.02     # water floor ramp: minimum depth (wu) per wu inside the smoothed edge
LAND_FLOOR_SLOPE = 0.25        # land floor ramp: minimum height (wu) above water per wu inland
EDGE_BIAS_WU = 12.0            # smoothed edge moved inland: covers most of the colormap's teal staircase corners;
                               # more pushes the edge into coarse terrain above the lifted plane (its stair shows)


def load_source(pak: Path, visual_height: Path | None, source: str = "coarse") -> tuple[np.ndarray, float, float]:
    """Returns (grid, cell_wu, water_elevation). Row 0 = north (vertex row 0).
    source="coarse" (default): the pak heightfield the terrain mesh is actually drawn from, so the
    smoothed coast stays on the rendered land; "visual": visual_height_4x.r32, whose re-authored
    coast drifts from the rendered terrain (water then paints over dry land)."""
    welev = read_water_elevation(pak.with_suffix(".fit"))
    if source == "visual" and visual_height is not None and visual_height.is_file():
        data = np.fromfile(visual_height, dtype="<f4")
        v = int(round(data.size ** 0.5))
        if v * v == data.size:
            return data.reshape(v, v).astype(np.float64), WORLD_UNITS_PER_VERTEX / 4.0, welev
    md = locate_mapdata(read_packets(pak))
    if md is None:
        raise RuntimeError(f"{pak}: no MapData packet")
    _idx, side, blocks = md
    layers = extract_layers(side, blocks, welev)
    return np.asarray(layers["elev"], dtype=np.float64), WORLD_UNITS_PER_VERTEX, welev


def upsample(grid: np.ndarray, factor: int, order: int = 1) -> np.ndarray:
    """Corner-aligned upsample: (n-1)*factor+1 samples per side. LINEAR by default: it is how the
    terrain triangles interpolate the coarse heights, so the waterline stays on the rendered coast.
    A cubic spline overshoots next to every terrace step of the stock heightmaps and dips land just
    above the water level below it (mc2_10: +2.5% of the frame turned to water, eating the plateau)."""
    if factor <= 1:
        return grid.copy()
    n = grid.shape[0]
    return ndimage.zoom(grid, ((n - 1) * factor + 1) / n, order=order, mode="nearest", grid_mode=False)


def smoothstep(e0: float, e1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - e0) / max(e1 - e0, 1e-9), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def signed_distance_wu(land: np.ndarray, cell_wu: float) -> np.ndarray:
    """+ on land, - in water, crossing zero half a cell either side of the boundary."""
    if land.all() or not land.any():
        big = float(land.shape[0]) * cell_wu
        return np.full(land.shape, big if land.all() else -big)
    d_land = ndimage.distance_transform_edt(land) * cell_wu
    d_water = ndimage.distance_transform_edt(~land) * cell_wu
    return np.where(land, d_land - 0.5 * cell_wu, -(d_water - 0.5 * cell_wu))


def build_field(h: np.ndarray, welev: float, cell_wu: float, *, open_wu: float, close_wu: float,
                sdf_sigma_wu: float, sdf_passes: int, band_wu: float, depth_sigma_wu: float,
                shore_slope: float) -> dict:
    land = h >= welev
    # Morphology on an edge-padded copy: scipy treats outside the array as background, which
    # eroded land along every map border (a strip of water around maps whose land meets the edge).
    it_open = max(int(round(open_wu / cell_wu)), 1) if open_wu > 0 else 0
    it_close = max(int(round(close_wu / cell_wu)), 1) if close_wu > 0 else 0
    pad = it_open + it_close + 1
    lp = np.pad(land, pad, mode="edge")
    if it_open:
        lp = ndimage.binary_opening(lp, iterations=it_open)
    if it_close:
        lp = ndimage.binary_closing(lp, iterations=it_close)
    land = lp[pad:-pad, pad:-pad]
    sdf0 = signed_distance_wu(land, cell_wu)
    band = 1.0 - smoothstep(0.5 * band_wu, band_wu, np.abs(sdf0))
    sdf = sdf0.copy()
    sig = max(sdf_sigma_wu / cell_wu, 0.5)
    for _ in range(max(sdf_passes, 0)):
        sdf = sdf + band * (ndimage.gaussian_filter(sdf, sig, mode="nearest") - sdf)
    # Depth (the tint AND the edge alpha source) is the REAL depth, Gaussian-smoothed so a lone low
    # coarse vertex no longer draws a deep diamond. The smoothed SDF only fixes its SIGN: a gentle
    # floor ramp keeps water just inside the smoothed edge wet and land just outside dry. (A steep
    # ramp made shallow flats read as deep, opaque water and flooded low plateaus visually.)
    real_depth = welev - ndimage.gaussian_filter(h, max(depth_sigma_wu / cell_wu, 0.5), mode="nearest")
    # Landward edge bias: the stock colormap paints underwater tiles teal, so the smooth edge must
    # not retreat seaward of the coarse waterline's inner staircase corners (they showed as a row of
    # teal dots on the land). Moving the edge EDGE_BIAS_WU inland covers them; real land standing
    # above the water still wins the depth test, so this does not flood it.
    sdf = sdf - EDGE_BIAS_WU
    water = sdf < 0.0
    # Land side gets a FIRMER floor: the engine reconstructs this grid with Catmull-Rom, which
    # overshoots toward the deeper water texels next to a near-zero land texel; with the gentle
    # floor the land just inside the edge dipped below the water level in a tile-periodic row of
    # water dots (mc2_10 diagonal coast).
    depth = np.where(water, np.maximum(real_depth, -shore_slope * sdf),
                     np.minimum(real_depth, -LAND_FLOOR_SLOPE * sdf))
    return {"field": welev - depth, "sdf": sdf, "sdf0": sdf0, "band": band}


def cook_field(pak: Path, visual_height: Path | None, *, source: str = DEFAULT_SOURCE,
               upsample_f: int = DEFAULT_UPSAMPLE,
               store_f: int = DEFAULT_STORE, open_wu: float = DEFAULT_OPEN_WU, close_wu: float = DEFAULT_CLOSE_WU,
               sdf_sigma_wu: float = DEFAULT_SDF_SIGMA_WU, sdf_passes: int = DEFAULT_SDF_PASSES,
               band_wu: float = DEFAULT_BAND_WU, depth_sigma_wu: float = DEFAULT_DEPTH_SIGMA_WU,
               shore_slope: float = DEFAULT_SHORE_SLOPE) -> dict:
    src, src_cell, welev = load_source(pak, visual_height, source)
    # upsample_f is relative to the coarse grid; a visual (32 wu) source needs a quarter of it
    f = upsample_f if src_cell == WORLD_UNITS_PER_VERTEX else max(upsample_f // 4, 1)
    fine = upsample(src, f)
    fine_cell = src_cell / max(f, 1)
    res = build_field(fine, welev, fine_cell, open_wu=open_wu, close_wu=close_wu, sdf_sigma_wu=sdf_sigma_wu,
                      sdf_passes=sdf_passes, band_wu=band_wu, depth_sigma_wu=depth_sigma_wu,
                      shore_slope=shore_slope)
    # Store on a corner-aligned subsample: 32 wu / store_f cells.
    step = max(int(round((WORLD_UNITS_PER_VERTEX / 4.0 / max(store_f, 1)) / fine_cell)), 1)
    res.update({"stored": res["field"][::step, ::step], "fine_src": fine, "welev": welev,
                "fine_cell_wu": fine_cell, "store_cell_wu": fine_cell * step})
    return res


FIELD_MAGIC = b"SHF1"


def write_field(path: Path, field: np.ndarray, welev: float) -> None:
    """16-byte header (magic SHF1, uint32 side, float32 water elevation, uint32 0) then side*side
    little-endian float16 of (height - water elevation): float16 is precise near zero, where the
    coast is cut (absolute heights near 80 quantize to 1/16 wu, which scalloped the edge)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    side = int(field.shape[0])
    header = FIELD_MAGIC + np.array([side], "<u4").tobytes() + np.array([welev], "<f4").tobytes() + bytes(4)
    with open(path, "wb") as f:
        f.write(header)
        f.write(np.ascontiguousarray(field - welev, dtype="<f2").tobytes())


def read_field(path: Path) -> tuple[np.ndarray, float]:
    raw = Path(path).read_bytes()
    assert raw[:4] == FIELD_MAGIC, path
    side = int(np.frombuffer(raw[4:8], "<u4")[0])
    welev = float(np.frombuffer(raw[8:12], "<f4")[0])
    rel = np.frombuffer(raw[16:], "<f2").astype(np.float64).reshape(side, side)
    return rel + welev, welev


def coast_preview(res: dict, crop: tuple[float, float, float, float] | None, scale: int) -> Image.Image:
    """Land beige / water blue (darker = deeper, i.e. the tint source); red = waterline of
    the unsmoothed upsampled heightfield, yellow = the cooked edge (SDF = 0)."""
    src, out, we = res["fine_src"], res["field"], res["welev"]
    n = out.shape[0]
    if crop:
        x0, y0, x1, y1 = (int(c * n) for c in crop)
        src, out = src[y0:y1, x0:x1], out[y0:y1, x0:x1]
    depth = np.clip((we - out) / 60.0, 0.0, 1.0)
    rgb = np.zeros(out.shape + (3,), np.float64)
    land = out >= we
    rgb[land] = [150, 130, 90]
    wcol = np.stack([40 + 0 * depth, 170 - 120 * depth, 160 - 60 * depth], -1)
    rgb[~land] = wcol[~land]

    def edge(a: np.ndarray) -> np.ndarray:
        w = a < we
        return w ^ ndimage.binary_erosion(w)
    rgb[edge(src)] = [230, 40, 40]
    rgb[edge(out)] = [255, 230, 0]
    img = Image.fromarray(rgb.clip(0, 255).astype(np.uint8))
    return img.resize((img.width * scale, img.height * scale), Image.NEAREST) if scale > 1 else img


def _tuning(args) -> dict:
    return dict(source=args.source, upsample_f=args.upsample, store_f=args.store, open_wu=args.open, close_wu=args.close,
                sdf_sigma_wu=args.sigma, sdf_passes=args.passes, band_wu=args.band,
                depth_sigma_wu=args.depth_sigma, shore_slope=args.shore_slope)


def _vh_for(pak: Path, beauty_root: Path | None) -> Path:
    return (beauty_root if beauty_root else pak.parent) / f"{pak.stem}.beauty" / "visual_height_4x.r32"


def cmd_cook(args) -> int:
    pak = Path(args.pak)
    vh = Path(args.visual_height) if args.visual_height else _vh_for(pak, None)
    res = cook_field(pak, vh, **_tuning(args))
    write_field(Path(args.out), res["stored"], res["welev"])
    print(f"[shore-field] {pak.stem}: {res['stored'].shape[0]}^2 @ {res['store_cell_wu']:.1f} wu -> {args.out}")
    return 0


def cmd_all(args) -> int:
    mdir = Path(args.missions_dir)
    beauty_root = Path(args.beauty_root) if args.beauty_root else None
    out_root = Path(args.out_root)
    census = {"missions_dir": str(mdir), "tuning": _tuning(args), "cooked": [], "skipped": []}
    for pak in sorted(mdir.glob("*.pak")):
        stem = pak.stem
        try:
            vh = _vh_for(pak, beauty_root)
            if not vh.is_file():
                vh = _vh_for(pak, None)
            res = cook_field(pak, vh if vh.is_file() else None, **_tuning(args))
        except Exception as e:  # noqa: BLE001 -- one bad pak must not stop the batch
            census["skipped"].append({"mission": stem, "reason": str(e)})
            print(f"[shore-field] skip {stem}: {e}")
            continue
        if not (res["sdf"] < 0.0).any():
            census["skipped"].append({"mission": stem, "reason": "no water"})
            continue
        out = out_root / f"{stem}.beauty" / FIELD_NAME
        write_field(out, res["stored"], res["welev"])
        census["cooked"].append({"mission": stem, "side": int(res["stored"].shape[0]),
                                 "cell_wu": res["store_cell_wu"], "water_elev": res["welev"]})
        print(f"[shore-field] {stem}: {res['stored'].shape[0]}^2 -> {out}")
    if args.census:
        Path(args.census).write_text(json.dumps(census, indent=2), encoding="utf-8", newline="\n")
    print(f"[shore-field] cooked={len(census['cooked'])} skipped={len(census['skipped'])}")
    return 0


def cmd_preview(args) -> int:
    pak = Path(args.pak)
    if args.visual_height:
        vh = Path(args.visual_height)
    else:
        vh = _vh_for(pak, Path(args.beauty_root) if args.beauty_root else None)
    res = cook_field(pak, vh if vh.is_file() else None, **_tuning(args))
    crop = tuple(float(c) for c in args.crop.split(",")) if args.crop else None
    coast_preview(res, crop, args.scale).save(args.png)
    print(f"[shore-field] preview -> {args.png}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="cook_shore_field: smooth coastline field (upsample -> SDF -> coastal smoothing)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def tuning(p):
        p.add_argument("--source", choices=("coarse", "visual"), default=DEFAULT_SOURCE)
        p.add_argument("--upsample", type=int, default=DEFAULT_UPSAMPLE)
        p.add_argument("--store", type=int, default=DEFAULT_STORE)
        p.add_argument("--open", type=float, default=DEFAULT_OPEN_WU)
        p.add_argument("--close", type=float, default=DEFAULT_CLOSE_WU)
        p.add_argument("--sigma", type=float, default=DEFAULT_SDF_SIGMA_WU)
        p.add_argument("--passes", type=int, default=DEFAULT_SDF_PASSES)
        p.add_argument("--band", type=float, default=DEFAULT_BAND_WU)
        p.add_argument("--depth-sigma", type=float, default=DEFAULT_DEPTH_SIGMA_WU)
        p.add_argument("--shore-slope", type=float, default=DEFAULT_SHORE_SLOPE)

    p = sub.add_parser("cook"); tuning(p)
    p.add_argument("--pak", required=True); p.add_argument("--visual-height"); p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_cook)
    p = sub.add_parser("all-missions"); tuning(p)
    p.add_argument("--missions-dir", required=True); p.add_argument("--beauty-root")
    p.add_argument("--out-root", required=True); p.add_argument("--census")
    p.set_defaults(fn=cmd_all)
    p = sub.add_parser("preview"); tuning(p)
    p.add_argument("--pak", required=True); p.add_argument("--visual-height"); p.add_argument("--beauty-root")
    p.add_argument("--png", required=True); p.add_argument("--crop"); p.add_argument("--scale", type=int, default=1)
    p.set_defaults(fn=cmd_preview)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
