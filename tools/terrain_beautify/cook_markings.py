#!/usr/bin/env python3
"""WHOLESALE-VECTORIZE-1 (course-corrected): per-TILE vector twins of the
legacy road/runway MARKING art, composed into the overlay-V2 sidecar
(`data/missions/<stem>.beauty/overlay_v2.png`).

USER RULING (locked, supersedes the first ROAD-MARKINGS-VECTOR-1 shape):
  - NO road ribbons. NO surface replacement. NO auto-invented layouts.
    The legacy tile art's FOOTPRINT and surface stay pixel-faithful.
  - Vectorize ONLY the marking pixels, per tile TYPE, traced FROM the tile
    art itself: UPSCALE the 64px tile (house ESRGAN chain:
    realesrgan-ncnn-vulkan, docs/modding-guide.md section 5) -> threshold
    the marking mask from the upscaled reference -> FIT vector primitives
    (capsules/rects; position/size/rotation SOLVED from the mask, never
    invented) -> score the rendered twin against the reference mask (IoU);
    components/tiles below threshold FALL BACK to the upscaled RASTER
    markings rather than shipping a bad vector.
  - Stamp per auto-tiler placement: the pak's per-cell overlay handle
    DECODES OFFLINE to (family, tileIndex) -- handle = NUM_TERRAIN_TYPES +
    familyBase + tileIndex, with familyBase the running sum of NumOverlays
    in mc2srcdata/textures/textures.fit OverlayType0..16 order (mirrors
    TerrainTextures::init nextAvailable allocation, terrtxm.cpp:141-357;
    MC_MAX_MIP_LEVELS=1). Verified empirically on mc2_01/03/10/17/24: every
    overlay_hi decodes into a family consistent with mission content
    (runway tiles on airfields, road chains elsewhere), zero unknowns.
    This kills the old recon assumption that the decode needed a runtime
    index. Same layout/size/orientation by construction: the 15-per-family
    tile variants ARE the orientations, so stamps need no rotation.
  - INVARIANT ("no legacy paint -> no vector paint"): tiles whose art has
    no paint (dirt roads, rough, damaged) produce EMPTY twins and are
    skipped. Cement pads/aprons/runways (terrain-type cells, mc2_01) get NO
    synthesized paint ever -- the cement classifier below is kept for
    DETECTION/REPORTING only (mc2_01 "runway found" recon requirement).
    mc2_01's real runway paint lives in its runway#### overlay tiles, which
    the stamps reproduce faithfully.
  - Bridge/gate cells (Bridge / X_PavedRoad_Bridge / BridgeDAM families)
    are excluded EXACTLY by family decode (runtime-mutable, recon landmine
    -- never statically baked). This replaces the water-adjacency
    heuristic for the stamping path.

Subcommands:
  extract  --pak <mission.pak> --out <markings.json>
      Decode per-cell overlay tile ids + cement-region report (report only).
  twins    --markings <json> --tiles-dir <64Overlays> --out-dir <dir>
           [--esrgan-exe F] [--esrgan-model N] [--no-esrgan]
      Build the per-tile vector-twin paint stamps + twins_report.json
      (per-tile IoU scores, vectorized-vs-raster component counts) + a
      triptych contact sheet (legacy 4x nearest | upscaled ref | twin).
  compose  --markings <json> --overlay <overlay_v2.png> --twins <dir>
           --out <png> [--px-per-wu N] [--max-canvas N]
      Stamp each decoded cell's twin at its exact cell footprint over the
      untouched parity raster.
  verify   --markings <json> --overlay <parity.png> --twins <dir>
           --composed <sidecar.png> [--bounds F]
      WHOLE-IMAGE acceptance gate: the composed sidecar may differ from the
      pure parity raster ONLY where marking stamps land (+ AA margin). Any
      difference outside a stamp footprint (body pixels shipped, edges
      feathered) is a FAIL (exit 1). This catches the compose-level errors the
      per-tile IoU gate is blind to.
  install  --png <file> --deploy <dir> --mission <stem>
      Same contract as overlay_extract.py's install.
  sheet    --png <file> --out <file>
      RGB + alpha contact sheet.

PIL + numpy + scipy offline; optional realesrgan-ncnn-vulkan subprocess for
the upscale step (--no-esrgan falls back to bicubic and says so in the
report). Read-only w.r.t. .pak and tile TGAs. Deterministic given the same
upscaler output (sorted traversals, no RNG).
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
)
from overlay_extract import classify_concrete  # noqa: E402

WORLD_UNITS_PER_VERTEX = 128.0

# --- overlay handle decode table ----------------------------------------------
# Source: mc2srcdata/textures/textures.fit ([Main] MaxTerrainTypes=255,
# OverlayType0..16 NumOverlays in file order) + TerrainTextures::init slot
# allocation (terrtxm.cpp:210-357: every terrain TYPE consumes exactly one
# texture slot -- initTexture or the 16x16 dummy -- THEN overlay families
# consume NumOverlays slots each, in OverlayType order, MC_MAX_MIP_LEVELS=1).
# Stored pak handle (textureData hi-16) = slot index of the tile.
NUM_TERRAIN_TYPES = 255
OVERLAY_FAMILIES = [  # (fit OverlayName, NumOverlays) in OverlayType0..16 order
    ("dirtroad", 15),
    ("pavedroad", 15),
    ("rough", 8),
    ("droad2proad", 4),
    ("droad2proadcross", 2),
    ("2lanedirt", 15),
    ("damagedroad", 15),
    ("runway", 15),
    ("x_dirt_paved", 50),
    ("x_dirt_2lanedirt", 50),
    ("x_dirt_damaged", 50),
    ("x_2lanedirt_paved", 50),
    ("x_damaged_paved", 50),
    ("x_2lanedirt_damaged", 50),
    ("bridge", 15),
    ("x_pavedroad_bridge", 50),
    ("bridgedam", 15),
]
# runtime-mutable families: NEVER statically baked (recon landmine; bridges
# swap tiles at runtime, bldng.cpp:861-876)
BRIDGE_FAMILIES = {"bridge", "x_pavedroad_bridge", "bridgedam"}


def _family_bases() -> list[tuple[str, int, int]]:
    out = []
    base = NUM_TERRAIN_TYPES
    for name, n in OVERLAY_FAMILIES:
        out.append((name, base, n))
        base += n
    return out


_FAMILY_BASES = _family_bases()


def decode_overlay_handle(hi: int) -> tuple[str, int] | None:
    """Pak textureData hi-16 -> (familyName, tileIndex), or None if the value
    is not an overlay tile slot (0/0xFFFF sentinels handled by caller)."""
    for name, base, n in _FAMILY_BASES:
        if base <= hi < base + n:
            return name, hi - base
    return None


# --- cement-region classification (DETECTION/REPORT ONLY -- no paint) ---------
# Kept from the first slice for the mc2_01 "extraction gap" requirement: the
# overlay scan cannot see CEMENT terrain-type runways/pads. Detection uses
# PCA elongation of the distance-transform core (bbox aspect fails on
# diagonal strips: mc2_01 bbox aspect 1.06 vs core PCA elongation ~3.5).
# INVARIANT: this NEVER generates paint -- cement regions with no legacy
# marking art get no vector paint, ever (user ruling).
CEMENT_MIN_COUNT = 16
CEMENT_CORE_DIST = 2
CEMENT_RUNWAY_ELONG_MIN = 2.0
RUNWAY_MIN_LONG_WU = 10 * WORLD_UNITS_PER_VERTEX


def _tile_center_world(row: float, col: float, side: int) -> tuple[float, float]:
    half_map = (side * WORLD_UNITS_PER_VERTEX) * 0.5
    x = (col + 0.5) * WORLD_UNITS_PER_VERTEX - half_map
    y = half_map - (row + 0.5) * WORLD_UNITS_PER_VERTEX
    return x, y


def _pca_strip(rows: np.ndarray, cols: np.ndarray) -> dict:
    """Second-moment (PCA) fit of a cell/pixel mask in the world-XY convention
    (x=col, y=-row so +Y is north). Angle deg CCW from +X, in (-90, 90]."""
    pts = np.stack([cols.astype(np.float64), -rows.astype(np.float64)], axis=1)
    mean = pts.mean(axis=0)
    cen = pts - mean
    cov = cen.T @ cen / max(1, len(pts))
    evals, evecs = np.linalg.eigh(cov)
    major = evecs[:, 1]
    elong = math.sqrt(max(evals[1], 1e-9) / max(evals[0], 1e-9))
    angle = math.degrees(math.atan2(major[1], major[0]))
    if angle <= -90.0:
        angle += 180.0
    elif angle > 90.0:
        angle -= 180.0
    proj_maj = cen @ major
    proj_min = cen @ evecs[:, 0]
    return {
        "angle_deg": angle, "elong": elong,
        "centroid_rc": (-mean[1], mean[0]),
        "len_units": float(proj_maj.max() - proj_maj.min() + 1.0),
        "width_units": float(proj_min.max() - proj_min.min() + 1.0),
    }


def _cement_components(concrete: np.ndarray, side: int) -> list[dict]:
    """Classify concrete TERRAIN-TYPE regions; returns report records only."""
    out: list[dict] = []
    if not concrete.any():
        return out
    lbl, ncomp = ndimage.label(concrete, structure=np.ones((3, 3)))
    for i in range(1, ncomp + 1):
        comp_mask = lbl == i
        rows, cols = np.where(comp_mask)
        if len(rows) < CEMENT_MIN_COUNT:
            continue
        dist = ndimage.distance_transform_cdt(comp_mask, metric="chessboard")
        crows, ccols = np.where(dist >= CEMENT_CORE_DIST)
        src = _pca_strip(crows, ccols) if len(crows) >= 4 else _pca_strip(rows, cols)
        cr, cc = src["centroid_rc"]
        wx, wy = _tile_center_world(cr, cc, side)
        length_wu = src["len_units"] * WORLD_UNITS_PER_VERTEX
        is_runway = (src["elong"] >= CEMENT_RUNWAY_ELONG_MIN and
                     length_wu >= RUNWAY_MIN_LONG_WU)
        out.append({
            "kind": "cement_runway" if is_runway else "cement_pad",
            "world_cx": round(wx, 1), "world_cy": round(wy, 1),
            "angle_deg": round(src["angle_deg"], 1),
            "length_wu": round(length_wu, 1),
            "width_wu": round(src["width_units"] * WORLD_UNITS_PER_VERTEX, 1),
            "count": int(len(rows)),
            "core_elong": round(src["elong"], 2),
        })
    return out


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

    n = side * side
    PCV_SIZE = 32
    OFF_TEXDATA = 16
    arr = np.frombuffer(blocks, dtype=np.uint8, count=n * PCV_SIZE).reshape(n, PCV_SIZE)
    texdata = arr[:, OFF_TEXDATA:OFF_TEXDATA + 4].copy().view(np.uint32).reshape(side, side)
    overlay_hi = (texdata >> 16) & 0xFFFF

    cells: list[dict] = []
    fam_hist: dict[str, int] = {}
    n_bridge = n_unknown = 0
    rows_idx, cols_idx = np.where((overlay_hi != 0) & (overlay_hi != 0xFFFF))
    for r, c in zip(rows_idx.tolist(), cols_idx.tolist()):
        hi = int(overlay_hi[r, c])
        dec = decode_overlay_handle(hi)
        if dec is None:
            n_unknown += 1
            continue
        family, tile = dec
        if family in BRIDGE_FAMILIES:
            n_bridge += 1  # runtime-mutable: never baked
            continue
        cells.append({"row": r, "col": c, "family": family, "tile": tile})
        fam_hist[family] = fam_hist.get(family, 0) + 1

    water_elev = read_water_elevation(pak.with_suffix(".fit"))
    layers = extract_layers(side, blocks, water_elev)
    concrete = classify_concrete(layers["terrtype"])
    cement_report = _cement_components(concrete, side)
    n_cem_runway = sum(1 for x in cement_report if x["kind"] == "cement_runway")
    n_cem_pad = len(cement_report) - n_cem_runway

    doc = {"mission": pak.stem, "side": side, "cells": cells,
           "excluded_bridge_cells": n_bridge,
           "unknown_overlay_values": n_unknown,
           "family_histogram": dict(sorted(fam_hist.items())),
           # report ONLY -- never a paint source ("no legacy paint -> no
           # vector paint" invariant)
           "cement_report": cement_report}
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(doc, indent=2, sort_keys=False))

    if n_unknown:
        print(f"[extract] WARN {n_unknown} overlay values did not decode "
              f"(table drift vs textures.fit?)", file=sys.stderr)
    print(f"[extract] {pak.stem}: side={side} decoded_cells={len(cells)} "
          f"bridge_excluded={n_bridge} unknown={n_unknown} "
          f"families={fam_hist} "
          f"cement_runway_components={n_cem_runway} cement_pad_components={n_cem_pad} "
          f"-> {out_path}")
    return 0


# --- twins: paint mask + primitive fit + IoU gate -------------------------------
TWIN_REF_SCALE = 4          # house chain is 4x (64 -> 256)
TILE_IOU_ACCEPT = 0.85      # per-tile acceptance (report gate)
COMP_IOU_VECTOR_MIN = 0.80  # per-component: below this, raster fallback
MIN_COMP_PX = 8             # despeckle: ignore paint specks below this (at 256px)

# Paint thresholds are RELATIVE to the tile's road-body statistics -- the
# legacy art is authored dark (dash lum ~121 on ~62 asphalt reads "white"
# only by contrast; measured on runway0000/0011/pavedroad0000).
#
# ROOT-CAUSE FIX (overlay_v2 "solid white runway"): a fixed `median + 26`
# delta is NOT a marking-vs-body discriminator on this art. Measured body
# luminance distributions have NO bimodal gap -- there is a smooth gradient
# from the dark asphalt core up through edge antialiasing, and `med+26` swept
# in 5-19% of the BODY on runway tiles (runway0000 pure-asphalt taxiway:
# body&lum>88 = 8%). Those body texels were then shipped as OPAQUE grey
# stamps, so the composed runway read as a solid whitish slab (dense region
# 96% opaque grey ~150) with soft feathered edges. Real markings sit a LARGE
# absolute contrast above the body median (authored as the brightest paint on
# the tile), whereas the body gradient / edge antialiasing tapers only
# modestly above it: measured runway0011/0014 threshold-bar/numeral paint
# reaches med+79..92 at p97; markingless runway0000-0010 top out at only
# med+38..50 at p97; faint pavedroad dashes at med+21..25 (pavedroad0006-14
# dashes med+39..63). WHITE_MIN_CONTRAST=55 cleanly separates the populations:
# tiles whose brightest texels are only body gradient (<55 over median) yield
# a near-empty mask that despeckle drops -> no stamp ("no legacy paint -> no
# vector paint"); only genuine bright markings survive. A single hard contrast
# floor (NOT a percentile) is used deliberately -- a percentile lands ON the
# marking luminance when markings are a non-trivial area fraction (a
# threshold-bar tile), which would exclude the very paint we want to keep.
WHITE_MIN_CONTRAST = 55     # lum >= body_median + this (marking-vs-body floor)
WHITE_CHROMA_MAX = 26       # |r-b| below this = achromatic paint
YELLOW_RB_DELTA = 18        # (r-b) > body_median_rb + delta
YELLOW_GB_MIN = 40          # (g-b) floor: true yellow paint, NOT tan dirt
                            # (dirt shoulder g-b ~32; painted ticks g-b ~90)
YELLOW_LUM_DELTA = 30       # yellow paint is also well above the body, not a
                            # faint tan tint (raised from 8: same body-gradient
                            # leak class as the white delta above)
# ROAD BODY = fully-opaque texels only. The tile alpha is the auto-tiler
# shape mask: road core ~240-255, dirt shoulder blend zone ~68-140 (measured
# pavedroad0000 alpha histogram {68,72,73,240,255}); a loose alpha gate lets
# the tan dirt fringe classify as "yellow paint".
BODY_ALPHA_MIN = 200


def _esrgan_upscale(rgba: np.ndarray, exe: Path, model: str) -> np.ndarray | None:
    """Run the house realesrgan-ncnn-vulkan chain on an RGBA tile. Returns
    upscaled RGBA array or None on failure."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "in.png"
        dst = Path(td) / "out.png"
        Image.fromarray(rgba, "RGBA").save(src)
        try:
            r = subprocess.run([str(exe), "-i", str(src), "-o", str(dst),
                                "-n", model, "-s", str(TWIN_REF_SCALE)],
                               capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as e:
            print(f"[twins] WARN esrgan failed ({e}); bicubic fallback", file=sys.stderr)
            return None
        if r.returncode != 0 or not dst.is_file():
            print(f"[twins] WARN esrgan rc={r.returncode}; bicubic fallback", file=sys.stderr)
            return None
        return np.array(Image.open(dst).convert("RGBA"), dtype=np.uint8)


def extract_paint_mask(ref: np.ndarray) -> tuple[np.ndarray, np.ndarray] | None:
    """Marking mask from an upscaled tile reference. Returns (mask, body)
    bool arrays, or None if the tile has no body/paint at all."""
    a = ref[..., 3]
    body = a > BODY_ALPHA_MIN
    if int(body.sum()) < 200:  # essentially no opaque road body
        return None
    r = ref[..., 0].astype(np.int32)
    g = ref[..., 1].astype(np.int32)
    b = ref[..., 2].astype(np.int32)
    lum = (r + g + b) // 3
    med = int(np.median(lum[body]))
    rb = r - b
    med_rb = int(np.median(rb[body]))
    # White threshold: a hard absolute contrast floor above the body median so
    # markingless asphalt (whose brightest texels are only body gradient,
    # ~med+40) contributes NOTHING, while genuine bright paint (med+55..92 on
    # real threshold bars/numerals/dashes) survives. Inclusive (>=) so a
    # marking whose luminance equals the floor is kept.
    white = (body & (lum >= med + WHITE_MIN_CONTRAST) &
             (np.abs(rb - med_rb) < WHITE_CHROMA_MAX))
    yellow = (body & (rb > med_rb + YELLOW_RB_DELTA) & ((g - b) > YELLOW_GB_MIN) &
              (lum > med + YELLOW_LUM_DELTA))
    mask = white | yellow
    if not mask.any():
        return None
    # despeckle
    lbl, ncomp = ndimage.label(mask, structure=np.ones((3, 3)))
    if ncomp:
        sizes = ndimage.sum_labels(np.ones_like(lbl), lbl, index=np.arange(1, ncomp + 1))
        drop = set((np.where(sizes < MIN_COMP_PX)[0] + 1).tolist())
        if drop:
            mask &= ~np.isin(lbl, list(drop))
    if not mask.any():
        return None
    return mask, body


def _iou(a: np.ndarray, b: np.ndarray) -> float:
    inter = float(np.logical_and(a, b).sum())
    union = float(np.logical_or(a, b).sum())
    return inter / union if union > 0 else 1.0


def _render_capsule_or_rect(shape: tuple[int, int], comp_rows: np.ndarray,
                            comp_cols: np.ndarray) -> tuple[np.ndarray, str]:
    """Fit a rotated rect AND a capsule to a paint component (position/size/
    rotation solved from the pixel mask via PCA + axis extents -- nothing
    invented) and return the better-scoring rendered mask."""
    fit = _pca_strip(comp_rows, comp_cols)
    ang = math.radians(fit["angle_deg"])
    u = np.array([math.cos(ang), math.sin(ang)])
    v = np.array([-math.sin(ang), math.cos(ang)])
    pts = np.stack([comp_cols.astype(np.float64), -comp_rows.astype(np.float64)], axis=1)
    mean = pts.mean(axis=0)
    cen = pts - mean
    pu = cen @ u
    pv = cen @ v
    # robust extents (2/98 pct): ESRGAN antialiasing grows faint tails that
    # would otherwise inflate the fitted primitive past the true paint
    lo_u, hi_u = np.percentile(pu, 2), np.percentile(pu, 98)
    lo_v, hi_v = np.percentile(pv, 2), np.percentile(pv, 98)
    half_l = (hi_u - lo_u) / 2.0 + 0.5
    half_w = (hi_v - lo_v) / 2.0 + 0.5
    mid_u = (hi_u + lo_u) / 2.0
    mid_v = (hi_v + lo_v) / 2.0
    center = mean + u * mid_u + v * mid_v  # (x, -y) frame

    comp_mask = np.zeros(shape, dtype=bool)
    comp_mask[comp_rows, comp_cols] = True

    def render(kind: str) -> np.ndarray:
        img = Image.new("L", (shape[1], shape[0]), 0)
        d = ImageDraw.Draw(img)
        cx, cy = center[0], -center[1]
        hl = half_l if kind == "rect" else max(half_l - half_w, 0.0)
        corners = []
        for su, sv in ((-1, -1), (-1, 1), (1, 1), (1, -1)):
            px = cx + u[0] * su * hl + v[0] * sv * half_w
            py = -( center[1] + u[1] * su * hl + v[1] * sv * half_w)
            corners.append((px, py))
        d.polygon(corners, fill=255)
        if kind == "capsule":
            for su in (-1, 1):
                ex = cx + u[0] * su * hl
                ey = -(center[1] + u[1] * su * hl)
                d.ellipse([ex - half_w, ey - half_w, ex + half_w, ey + half_w], fill=255)
        return np.array(img) > 127

    best_mask, best_kind, best_iou = None, "", -1.0
    for kind in ("rect", "capsule"):
        m = render(kind)
        s = _iou(m, comp_mask)
        if s > best_iou:
            best_mask, best_kind, best_iou = m, kind, s
    return best_mask, best_kind


def build_twin(ref: np.ndarray) -> tuple[np.ndarray, dict] | None:
    """Reference RGBA (upscaled tile) -> (twin RGBA paint stamp, stats).
    Vector primitives where they fit (component IoU >= COMP_IOU_VECTOR_MIN),
    crisp RASTER fallback from the reference where they don't (numerals,
    arrows, curves). Returns None when the tile has NO paint (invariant:
    no legacy paint -> no vector paint)."""
    got = extract_paint_mask(ref)
    if got is None:
        return None
    mask, body = got
    h, w = mask.shape
    twin = np.zeros((h, w, 4), dtype=np.uint8)
    lbl, ncomp = ndimage.label(mask, structure=np.ones((3, 3)))
    n_vec = n_raster = 0
    # STAMP-ONLY-MARKING GUARANTEE: a fitted rect/capsule primitive can bleed a
    # pixel or two past the true paint into the body. Clip every vector stamp
    # to a 1px dilation of the marking mask so the twin's opaque region is a
    # subset of the markings (+1px AA) and NEVER ships raw asphalt/concrete
    # body -- the exact defect that painted the runway solid grey. The raster
    # fallback already writes only `comp` (a subset of the mask), so it needs
    # no clip.
    mask_dil = ndimage.binary_dilation(mask, structure=np.ones((3, 3), dtype=bool))
    for i in range(1, ncomp + 1):
        comp = lbl == i
        crow, ccol = np.where(comp)
        color = ref[crow, ccol, :3].mean(axis=0).astype(np.uint8)
        prim, kind = _render_capsule_or_rect((h, w), crow, ccol)
        if _iou(prim, comp) >= COMP_IOU_VECTOR_MIN:
            sel = prim & mask_dil  # clip to marking mask: no body pixels ship
            twin[sel, 0] = color[0]
            twin[sel, 1] = color[1]
            twin[sel, 2] = color[2]
            twin[sel, 3] = 255
            n_vec += 1
        else:
            # raster fallback: keep the reference's own crisp pixels for this
            # component (numerals / arrows / curved paint)
            twin[comp, :3] = ref[comp, :3]
            twin[comp, 3] = 255
            n_raster += 1
    tile_iou = _iou(twin[..., 3] > 0, mask)
    return twin, {"components": ncomp, "vector": n_vec, "raster": n_raster,
                  "tile_iou": round(tile_iou, 4)}


def cmd_twins(args) -> int:
    markings_path = Path(args.markings)
    tiles_dir = Path(args.tiles_dir)
    out_dir = Path(args.out_dir)
    if not markings_path.is_file():
        print(f"[twins] ERROR markings json not found: {markings_path}", file=sys.stderr)
        return 4
    if not tiles_dir.is_dir():
        print(f"[twins] ERROR tiles dir not found: {tiles_dir}", file=sys.stderr)
        return 4
    doc = json.loads(markings_path.read_text())
    used = sorted({(c["family"], c["tile"]) for c in doc.get("cells", [])})
    if not used:
        print("[twins] no decoded cells; nothing to build")
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)

    esrgan_exe = Path(args.esrgan_exe) if args.esrgan_exe else None
    use_esrgan = (not args.no_esrgan) and esrgan_exe is not None and esrgan_exe.is_file()
    if not use_esrgan and not args.no_esrgan:
        print(f"[twins] WARN esrgan exe not found ({esrgan_exe}); bicubic fallback")

    report = {"upscaler": (f"esrgan:{args.esrgan_model}" if use_esrgan else "bicubic"),
              "ref_scale": TWIN_REF_SCALE, "tiles": {}}
    trip_rows = []
    for family, tile in used:
        name = f"{family}{tile:04d}"
        tga = tiles_dir / f"{name}.tga"
        if not tga.is_file():
            print(f"[twins] WARN tile art missing: {tga}", file=sys.stderr)
            report["tiles"][name] = {"status": "missing_art"}
            continue
        raw = np.array(Image.open(tga).convert("RGBA"), dtype=np.uint8)
        ref = None
        if use_esrgan:
            ref = _esrgan_upscale(raw, esrgan_exe, args.esrgan_model)
        if ref is None:
            big = Image.fromarray(raw, "RGBA").resize(
                (raw.shape[1] * TWIN_REF_SCALE, raw.shape[0] * TWIN_REF_SCALE), Image.BICUBIC)
            ref = np.array(big, dtype=np.uint8)
        got = build_twin(ref)
        if got is None:
            # invariant: no legacy paint -> no vector paint -> no stamp
            report["tiles"][name] = {"status": "no_paint"}
            continue
        twin, stats = got
        accepted = stats["tile_iou"] >= TILE_IOU_ACCEPT
        stats["status"] = "ok" if accepted else "below_threshold_raster_used"
        if not accepted:
            # per the ruling: below threshold, ship the upscaled RASTER
            # markings for the whole tile instead of a bad vector
            got2 = extract_paint_mask(ref)
            mask, _ = got2
            twin = np.zeros_like(twin)
            twin[mask, :3] = ref[mask, :3]
            twin[mask, 3] = 255
        Image.fromarray(twin, "RGBA").save(out_dir / f"{name}.twin.png")
        report["tiles"][name] = stats
        trip_rows.append((name, raw, ref, twin, stats))
        print(f"[twins] {name}: comps={stats['components']} vector={stats['vector']} "
              f"raster={stats['raster']} tile_iou={stats['tile_iou']} {stats['status']}")

    (out_dir / "twins_report.json").write_text(json.dumps(report, indent=2))

    # triptych sheet: legacy 4x-nearest | upscaled reference | twin paint
    if trip_rows:
        cell = 256
        pad = 16
        sheet = Image.new("RGB", (3 * cell + 4 * pad, len(trip_rows) * (cell + pad) + pad), (28, 28, 32))
        d = ImageDraw.Draw(sheet)
        for i, (name, raw, ref, twin, stats) in enumerate(trip_rows):
            y = pad + i * (cell + pad)
            legacy = Image.fromarray(raw, "RGBA").resize((cell, cell), Image.NEAREST)
            sheet.paste(Image.fromarray(np.array(legacy)[..., :3], "RGB"), (pad, y))
            sheet.paste(Image.fromarray(ref[..., :3], "RGB"), (2 * pad + cell, y))
            tw = twin.copy()
            sheet.paste(Image.fromarray(tw[..., :3], "RGB"), (3 * pad + 2 * cell, y))
            d.text((pad, y + 2), f"{name} iou={stats['tile_iou']}", fill=(255, 255, 120))
        sheet_path = out_dir / "twins_triptych.png"
        sheet.save(sheet_path)
        print(f"[twins] triptych -> {sheet_path}")
    return 0


# --- compose ------------------------------------------------------------------
def _load_bounds(bounds_path: Path | None, overlay_path: Path) -> tuple[float, float, float, float]:
    if bounds_path is None:
        bounds_path = overlay_path.with_suffix("").with_name(
            overlay_path.with_suffix("").name + ".bounds.txt")
    if bounds_path.is_file():
        parts = bounds_path.read_text().split()
        if len(parts) == 4:
            return tuple(float(p) for p in parts)  # type: ignore[return-value]
    raise ValueError(f"compose: no usable bounds file found (looked for {bounds_path}); "
                     f"pass --bounds explicitly or ensure it sits alongside --overlay")


def cmd_compose(args) -> int:
    markings_path = Path(args.markings)
    overlay_path = Path(args.overlay)
    twins_dir = Path(args.twins)
    if not markings_path.is_file():
        print(f"[compose] ERROR markings json not found: {markings_path}", file=sys.stderr)
        return 4
    if not overlay_path.is_file():
        print(f"[compose] ERROR overlay png not found: {overlay_path}", file=sys.stderr)
        return 4
    if not twins_dir.is_dir():
        print(f"[compose] ERROR twins dir not found: {twins_dir}", file=sys.stderr)
        return 4

    doc = json.loads(markings_path.read_text())
    side = int(doc["side"])
    cells = doc.get("cells", [])

    bounds_path = Path(args.bounds) if args.bounds else None
    try:
        top_left_x, top_left_y, size_x, size_y = _load_bounds(bounds_path, overlay_path)
    except ValueError as e:
        print(f"[compose] ERROR {e}", file=sys.stderr)
        return 4

    px_per_wu = float(args.px_per_wu)
    canvas_w = max(1, round(size_x * px_per_wu))
    canvas_h = max(1, round(size_y * px_per_wu))
    max_canvas = int(args.max_canvas)
    if canvas_w > max_canvas or canvas_h > max_canvas:
        shrink = max_canvas / max(canvas_w, canvas_h)
        px_per_wu *= shrink
        canvas_w = max(1, round(size_x * px_per_wu))
        canvas_h = max(1, round(size_y * px_per_wu))
        print(f"[compose] WARN requested px-per-wu would exceed --max-canvas={max_canvas}; "
              f"clamped to px_per_wu={px_per_wu:.4f} -> {canvas_w}x{canvas_h}")

    # the parity raster IS the base and stays visually identical (user
    # ruling: footprint/surface pixel-faithful; only paint gets crisper).
    #
    # ROOT-CAUSE FIX (overlay_v2 "scalloped semi-transparent cement transition
    # blobs"): the parity raster is a coarse per-vertex-cell binary mask
    # (side x side, e.g. 100x100 for mc2_01, hard 0/255 alpha at cement
    # boundaries). BICUBIC-upsampling that to the 8192 canvas SMOOTHED the
    # binary alpha into a soft ramp -- ~458k texels landed at partial alpha
    # (1..249) along every cement edge, which the engine composited as
    # translucent feathered halos ("fog clouds"). NEAREST keeps the cement
    # edges hard (0 partial-alpha texels); the engine's own overlay transition
    # shader is what softens cement borders, so the sidecar must NOT pre-blur
    # them. Only the marking STAMPS below are anti-aliased (LANCZOS) -- paint
    # gets crisper, bodies/edges stay pixel-faithful.
    base = Image.open(overlay_path).convert("RGBA")
    if base.size != (canvas_w, canvas_h):
        base = base.resize((canvas_w, canvas_h), Image.NEAREST)
    canvas = base.copy()

    # per-cell stamp: cell (row,col) covers [col,col+1)x[row,row+1) of the
    # side x side grid mapped over the FULL map extent (bounds == full map
    # for the parity raster; world convention row0=top=north).
    half_map = (side * WORLD_UNITS_PER_VERTEX) * 0.5
    twin_cache: dict[str, Image.Image | None] = {}
    stamped = skipped = 0
    for cell in cells:
        name = f"{cell['family']}{cell['tile']:04d}"
        if name not in twin_cache:
            p = twins_dir / f"{name}.twin.png"
            twin_cache[name] = Image.open(p).convert("RGBA") if p.is_file() else None
        twin = twin_cache[name]
        if twin is None:
            skipped += 1  # no paint in the legacy art -> nothing to stamp
            continue
        r, c = int(cell["row"]), int(cell["col"])
        wx0 = c * WORLD_UNITS_PER_VERTEX - half_map
        wy0 = half_map - r * WORLD_UNITS_PER_VERTEX  # north edge of the cell
        x0 = round((wx0 - top_left_x) / size_x * canvas_w)
        y0 = round((top_left_y - wy0) / size_y * canvas_h)
        x1 = round((wx0 + WORLD_UNITS_PER_VERTEX - top_left_x) / size_x * canvas_w)
        y1 = round((top_left_y - (wy0 - WORLD_UNITS_PER_VERTEX)) / size_y * canvas_h)
        if x1 <= x0 or y1 <= y0:
            skipped += 1
            continue
        stamp = twin.resize((x1 - x0, y1 - y0), Image.LANCZOS)
        canvas.alpha_composite(stamp, (x0, y0))
        stamped += 1

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)
    out_bounds = out_path.with_suffix("").with_name(out_path.with_suffix("").name + ".bounds.txt")
    out_bounds.write_text(f"{top_left_x:.3f} {top_left_y:.3f} {size_x:.3f} {size_y:.3f}\n")

    print(f"[compose] {markings_path} + {overlay_path} -> {out_path} "
          f"size={canvas_w}x{canvas_h} px_per_wu={px_per_wu:.4f} "
          f"cells={len(cells)} stamped={stamped} skipped_no_paint={skipped} "
          f"bounds={out_bounds}")
    return 0


# --- whole-image acceptance gate ---------------------------------------------
# The per-tile IoU gate above scores each stamp against its own tile mask, but
# it is BLIND to whole-image compose errors: a stamp that ships body pixels, or
# a base that got feathered on upsample, both pass every per-tile check while
# wrecking the composed sidecar (the "solid white runway" + "cement blob"
# regressions). This gate closes that hole: the ONLY texels allowed to differ
# between the composed sidecar and the pure parity raster are the ones the
# marking stamps cover (+ a small AA margin). Any difference OUTSIDE a stamp
# footprint means the base surface/edges were altered -> FAIL.
GATE_AA_MARGIN = 2          # px dilation of stamp footprints (LANCZOS AA fringe)
GATE_DIFF_EPS = 6           # per-channel |delta| below this = identical (noise)


def whole_image_diff_confined(
        composed: np.ndarray, parity_base: np.ndarray, stamp_footprint: np.ndarray,
        aa_margin: int = GATE_AA_MARGIN, eps: int = GATE_DIFF_EPS) -> dict:
    """Assert composed == parity_base everywhere OUTSIDE the (dilated) stamp
    footprint. `composed`/`parity_base` are HxWx4 uint8 at the SAME size;
    `stamp_footprint` is HxW bool (True where any marking stamp was composited).
    Returns a report dict with `pass` (bool) and the offending-pixel count."""
    assert composed.shape == parity_base.shape, (composed.shape, parity_base.shape)
    assert stamp_footprint.shape == composed.shape[:2], stamp_footprint.shape
    diff = np.abs(composed.astype(np.int16) - parity_base.astype(np.int16)).max(axis=2)
    changed = diff > eps
    allowed = stamp_footprint
    if aa_margin > 0:
        allowed = ndimage.binary_dilation(
            allowed, structure=np.ones((3, 3), dtype=bool), iterations=aa_margin)
    outside = changed & ~allowed
    n_outside = int(outside.sum())
    return {
        "pass": n_outside == 0,
        "changed_texels": int(changed.sum()),
        "outside_footprint_texels": n_outside,
        "footprint_texels": int(allowed.sum()),
        "aa_margin": aa_margin, "eps": eps,
    }


def _reconstruct_parity_and_footprint(
        overlay_path: Path, twins_dir: Path, doc: dict,
        top_left_x: float, top_left_y: float, size_x: float, size_y: float,
        canvas_w: int, canvas_h: int) -> tuple[np.ndarray, np.ndarray]:
    """Rebuild the pure parity base (NEAREST-upsampled, NO stamps) and the
    boolean stamp-footprint mask, using the SAME geometry as cmd_compose."""
    side = int(doc["side"])
    base = Image.open(overlay_path).convert("RGBA")
    if base.size != (canvas_w, canvas_h):
        base = base.resize((canvas_w, canvas_h), Image.NEAREST)
    parity = np.array(base, dtype=np.uint8)
    footprint = np.zeros((canvas_h, canvas_w), dtype=bool)
    half_map = (side * WORLD_UNITS_PER_VERTEX) * 0.5
    twin_cache: dict[str, Image.Image | None] = {}
    for cell in doc.get("cells", []):
        name = f"{cell['family']}{cell['tile']:04d}"
        if name not in twin_cache:
            p = twins_dir / f"{name}.twin.png"
            twin_cache[name] = Image.open(p).convert("RGBA") if p.is_file() else None
        twin = twin_cache[name]
        if twin is None:
            continue
        r, c = int(cell["row"]), int(cell["col"])
        wx0 = c * WORLD_UNITS_PER_VERTEX - half_map
        wy0 = half_map - r * WORLD_UNITS_PER_VERTEX
        x0 = round((wx0 - top_left_x) / size_x * canvas_w)
        y0 = round((top_left_y - wy0) / size_y * canvas_h)
        x1 = round((wx0 + WORLD_UNITS_PER_VERTEX - top_left_x) / size_x * canvas_w)
        y1 = round((top_left_y - (wy0 - WORLD_UNITS_PER_VERTEX)) / size_y * canvas_h)
        if x1 <= x0 or y1 <= y0:
            continue
        stamp = np.array(twin.resize((x1 - x0, y1 - y0), Image.LANCZOS), dtype=np.uint8)
        sx0, sy0 = max(0, x0), max(0, y0)
        sx1, sy1 = min(canvas_w, x1), min(canvas_h, y1)
        if sx1 <= sx0 or sy1 <= sy0:
            continue
        # a texel is "covered" wherever the resized stamp has any alpha
        salpha = stamp[sy0 - y0:sy1 - y0, sx0 - x0:sx1 - x0, 3] > 0
        footprint[sy0:sy1, sx0:sx1] |= salpha
    return parity, footprint


def cmd_verify(args) -> int:
    """Whole-image acceptance: composed sidecar vs pure parity raster diff must
    be confined to marking-stamp pixels (+ AA margin)."""
    markings_path = Path(args.markings)
    overlay_path = Path(args.overlay)
    twins_dir = Path(args.twins)
    composed_path = Path(args.composed)
    for label, p in (("markings", markings_path), ("overlay", overlay_path),
                     ("composed", composed_path)):
        if not p.is_file():
            print(f"[verify] ERROR {label} not found: {p}", file=sys.stderr)
            return 4
    if not twins_dir.is_dir():
        print(f"[verify] ERROR twins dir not found: {twins_dir}", file=sys.stderr)
        return 4

    doc = json.loads(markings_path.read_text())
    composed = np.array(Image.open(composed_path).convert("RGBA"), dtype=np.uint8)
    canvas_h, canvas_w = composed.shape[:2]

    bounds_path = Path(args.bounds) if args.bounds else None
    try:
        tlx, tly, sx, sy = _load_bounds(bounds_path, overlay_path)
    except ValueError as e:
        print(f"[verify] ERROR {e}", file=sys.stderr)
        return 4

    parity, footprint = _reconstruct_parity_and_footprint(
        overlay_path, twins_dir, doc, tlx, tly, sx, sy, canvas_w, canvas_h)
    rep = whole_image_diff_confined(composed, parity, footprint)
    print(f"[verify] {composed_path.name}: changed={rep['changed_texels']} "
          f"footprint={rep['footprint_texels']} "
          f"outside_footprint={rep['outside_footprint_texels']} "
          f"-> {'PASS' if rep['pass'] else 'FAIL'}")
    if not rep["pass"]:
        print(f"[verify] FAIL: {rep['outside_footprint_texels']} texels differ from the "
              f"parity raster OUTSIDE any marking stamp -- the base surface/edges "
              f"were altered (body pixels shipped, or edges feathered).",
              file=sys.stderr)
    return 0 if rep["pass"] else 1


# --- install (delegates to overlay_extract.py's contract) --------------------
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


# --- sheet (contact sheet preview) --------------------------------------------
PANEL = 220


def cmd_sheet(args) -> int:
    png_path = Path(args.png)
    if not png_path.is_file():
        print(f"[sheet] ERROR png not found: {png_path}", file=sys.stderr)
        return 4
    img = Image.open(png_path).convert("RGBA")
    arr = np.array(img)

    def _resize(im: Image.Image) -> Image.Image:
        return im.resize((PANEL, PANEL), Image.NEAREST)

    panels = [_resize(Image.fromarray(arr[..., :3], "RGB"))]
    labels = ["composed RGB"]
    a = arr[..., 3]
    panels.append(_resize(Image.fromarray(np.stack([a, a, a], axis=-1), "RGB")))
    labels.append("alpha")

    cols = len(panels)
    sheet = Image.new("RGB", (PANEL * cols, PANEL + 24), (30, 30, 30))
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
_DEFAULT_ESRGAN = "A:/Games/mc2-opengl-src/realesrgan-ncnn-vulkan/realesrgan-ncnn-vulkan.exe"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="cook_markings v3: per-tile vector twins of the legacy marking art, stamped into the overlay-V2 sidecar")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract", help="decode per-cell overlay tile ids + cement report -> markings.json")
    p.add_argument("--pak", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("twins", help="build per-tile vector-twin paint stamps (upscale -> mask -> fit -> IoU gate)")
    p.add_argument("--markings", required=True)
    p.add_argument("--tiles-dir", required=True, help="64Overlays tile art dir (canonical: mc2srcdata/textures/64Overlays)")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--esrgan-exe", default=_DEFAULT_ESRGAN)
    p.add_argument("--esrgan-model", default="realesrgan-x4plus")
    p.add_argument("--no-esrgan", action="store_true", help="bicubic upscale instead of the ESRGAN chain")
    p.set_defaults(func=cmd_twins)

    p = sub.add_parser("compose", help="stamp per-cell twins over the parity raster")
    p.add_argument("--markings", required=True)
    p.add_argument("--overlay", required=True)
    p.add_argument("--twins", required=True)
    p.add_argument("--bounds", help="explicit bounds file (default: <overlay-stem>.bounds.txt)")
    p.add_argument("--out", required=True)
    p.add_argument("--px-per-wu", type=float, default=8.0)
    p.add_argument("--max-canvas", type=int, default=8192,
                   help="safety cap on canvas width/height in pixels (default 8192; "
                        "at 100-cell maps this gives ~82px/cell, beating the legacy 64px tile density)")
    p.set_defaults(func=cmd_compose)

    p = sub.add_parser("verify", help="whole-image acceptance: composed vs pure parity raster diff must be confined to marking stamps")
    p.add_argument("--markings", required=True)
    p.add_argument("--overlay", required=True, help="the pure parity raster (overlay base, pre-stamp)")
    p.add_argument("--twins", required=True)
    p.add_argument("--composed", required=True, help="the composed sidecar to check")
    p.add_argument("--bounds", help="explicit bounds file (default: <overlay-stem>.bounds.txt)")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("install", help="copy composed png(+bounds) into <deploy>/data/missions/<stem>.beauty/overlay_v2.png")
    p.add_argument("--png", required=True)
    p.add_argument("--deploy", required=True)
    p.add_argument("--mission", required=True)
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("sheet", help="RGB + alpha contact sheet")
    p.add_argument("--png", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_sheet)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
