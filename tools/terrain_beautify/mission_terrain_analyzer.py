#!/usr/bin/env python3
"""MISSION-TERRAIN-ANALYZER-1 (Arc B / B1): read-only stock-mission terrain recon.

Reads a stock MC2 mission .pak (+ optional .fit), reconstructs the heightfield and
per-vertex layers, derives feature + protection masks, and writes DEBUG MASKS ONLY.

NON-NEGOTIABLE SCOPE:
  - Read-only. No .pak / .fit mutation. No engine binary. No gameplay/render touch.
  - Output is debug PNG masks + a JSON/Markdown report under the chosen out dir.
  - Deterministic: no RNG, no timestamps inside mask data (only in report header).

This proves the tool UNDERSTANDS authored terrain before any binary write is risked
(B2/B3). The mask language here is shared with Arc A (new-map generation).

Format references (verified against source):
  - PacketFile: mclib/packet.h  (magic 0xFEEDFACE, entry = type<<29 | offset)
  - PostcompVertex: mclib/vertex.h  (32 B: normal[12], elevation@12, textureData@16,
        localRGBLight@20, terrainType@24, selected@28/water@29/shadow@30/highlighted@31)
  - MapData packet 0: realVerticesMapSide^2 * 32, side in {60,80,100,120}
        (mclib/terrain.cpp:322); worldUnitsPerVertex = 128 (terrain.cpp:101).
  - Terrain objects packet (next RAW packet, "1 int count + N*40B records"):
        objTypeNum, x,y,z, rotation, damage, teamId, parentId, 2 pad ints
        (code/objmgr.cpp:1090 countTerrainObjects).
  - World->grid: mapTopLeft3d = (-side*128/2, +side*128/2); tileC=(x-tlx)/128,
        tileR=(tly-y)/128  (mclib/terrain.h:446 worldToTile).
  - Overlay enum: mclib/mapdata.h (DIRT_ROAD..DAMAGED_BRIDGE); overlay handle is the
        hi-16 of textureData (present when nonzero / not 0xFFFF).
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

# `--missions-dir` has no default, and any supplied directory must match the
# shipped mission data byte-for-byte. A Carver5 tree differs from mission.fst
# on ALL 24 campaign maps. The resolver below finds stock data and PROVES it
# byte-matches what the engine loads, or refuses to run.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "mc2data"))
from stock_missions import (  # noqa: E402
    StockDataError, add_missions_dir_arg, resolve_from_args,
)

# --- PacketFile constants (mclib/packet.h) -----------------------------------
PACKET_MAGIC = 0xFEEDFACE
TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1
ST_RAW, ST_FWF, ST_LZD, ST_HF, ST_ZLIB, ST_NUL = 0x0, 0x1, 0x2, 0x3, 0x4, 0x7

# --- PostcompVertex layout (mclib/vertex.h) ----------------------------------
PCV_SIZE = 32
OFF_ELEV = 12
OFF_TEXDATA = 16
OFF_TERRTYPE = 24
OFF_WATER = 29
WORLD_UNITS_PER_VERTEX = 128.0
VALID_GRID_SIDES = (60, 80, 100, 120, 140, 160, 180)

# --- Terrain object record (code/objmgr.cpp countTerrainObjects) -------------
OBJREC_SIZE = 40  # 10 * 4B: objTypeNum,x,y,z,rot,dmg,team,parent,pad,pad

# Thresholds (degrees) — tunable; documented in report.
CLIFF_SLOPE_DEG = 35.0
FLAT_SLOPE_DEG = 10.0
BUILDING_FOOTPRINT_RADIUS = 1  # grid cells around each object centre
PYRAMID_MAX_AREA = 600          # small island cell-count ceiling for pyramid test


# --- LZW decoder (port of mclib/lzdecomp.cpp) --------------------------------
def lzw_decompress(src: bytes) -> bytes:
    HASH_EOF, HASH_CLEAR, HASH_FREE = 257, 256, 258
    BASE_BITS, MAX_DICT = 9, 4096
    hash_chain = [0] * MAX_DICT
    hash_suffix = [0] * MAX_DICT
    dest = bytearray()
    src_len = len(src)
    bit_pos = 0

    def get_code(width: int) -> int:
        nonlocal bit_pos
        byte_idx = bit_pos >> 3
        val = 0
        for i in range(4):
            if byte_idx + i < src_len:
                val |= src[byte_idx + i] << (8 * i)
        code = (val >> (bit_pos & 7)) & ((1 << width) - 1)
        bit_pos += width
        return code

    code_width = BASE_BITS
    max_index = 1 << BASE_BITS
    free_index = HASH_FREE
    old_chain = old_suffix = 0
    while True:
        code = get_code(code_width)
        if code == HASH_EOF:
            break
        if code == HASH_CLEAR:
            code_width = BASE_BITS
            max_index = 1 << BASE_BITS
            free_index = HASH_FREE
            lit = get_code(code_width) & 0xFF
            dest.append(lit)
            old_chain = old_suffix = lit
            continue
        chain = code
        if chain >= free_index:
            hash_chain[chain] = old_chain
            hash_suffix[chain] = old_suffix & 0xFF
        stack = []
        cur = chain
        while cur >= 256:
            stack.append(hash_suffix[cur])
            cur = hash_chain[cur]
        stack.append(cur & 0xFF)
        old_suffix = cur & 0xFF
        for c in reversed(stack):
            dest.append(c)
        if free_index < MAX_DICT:
            hash_chain[free_index] = old_chain
            hash_suffix[free_index] = old_suffix
        free_index += 1
        old_chain = chain
        if free_index >= max_index and code_width < 12:
            code_width += 1
            max_index <<= 1
    return bytes(dest)


# --- PacketFile reader -------------------------------------------------------
def read_packets(path: Path):
    data = path.read_bytes()
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic != PACKET_MAGIC:
        raise ValueError(f"{path.name}: expected 0xFEEDFACE, got {magic:#x} "
                         "(checksum-armed PacketFile not supported)")
    first_off = struct.unpack_from('<I', data, 4)[0]
    num = (first_off // 4) - 2
    ent = [struct.unpack_from('<I', data, 8 + 4 * i)[0] for i in range(num)]
    out = []
    for i in range(num):
        t = (ent[i] >> TYPE_SHIFT) & 0x7
        off = ent[i] & OFFSET_MASK
        end = (ent[i + 1] & OFFSET_MASK) if i + 1 < num else len(data)
        out.append((i, t, data[off:end]))
    return out


def decode_packet(t: int, payload: bytes):
    if t == ST_RAW:
        return payload
    if t == ST_LZD:
        return lzw_decompress(payload[4:]) if len(payload) >= 4 else None
    if t == ST_ZLIB:
        try:
            return zlib.decompress(payload[4:]) if len(payload) >= 4 else None
        except zlib.error:
            return None
    if t == ST_NUL:
        return b""
    return None  # FWF/HF unhandled


def locate_mapdata(packets):
    best = None
    for idx, t, payload in packets:
        dec = decode_packet(t, payload)
        if dec is None or len(dec) <= 0 or len(dec) % PCV_SIZE != 0:
            continue
        cells = len(dec) // PCV_SIZE
        side = math.isqrt(cells)
        if side * side == cells and side in VALID_GRID_SIDES:
            if best is None or len(dec) > len(best[2]):
                best = (idx, side, dec)
    return best


# --- Per-vertex layer extraction --------------------------------------------
def read_water_elevation(fit_path: Path) -> float:
    """Authoritative water level = [Water] Elevation in the mission .fit
    (mclib/terrain.cpp:3740 waterElevation = readIdFloat("Elevation")). The
    per-vertex PostcompVertex.water byte is a packed alpha, NOT a clean bool —
    do not use it. Returns 0.0 if absent (engine default)."""
    if not fit_path.is_file():
        return 0.0
    in_block = False
    for line in fit_path.read_text(errors="ignore").splitlines():
        s = line.strip()
        if s.startswith("["):
            in_block = (s.lower() == "[water]")
            continue
        if in_block and s.lower().startswith("f elevation"):
            try:
                return float(s.split("=", 1)[1])
            except (IndexError, ValueError):
                return 0.0
    return 0.0


def extract_layers(side: int, blocks: bytes, water_elev: float) -> dict:
    n = side * side
    arr = np.frombuffer(blocks, dtype=np.uint8, count=n * PCV_SIZE).reshape(n, PCV_SIZE)
    elev = arr[:, OFF_ELEV:OFF_ELEV + 4].copy().view(np.float32).reshape(side, side)
    texdata = arr[:, OFF_TEXDATA:OFF_TEXDATA + 4].copy().view(np.uint32).reshape(side, side)
    terrtype = arr[:, OFF_TERRTYPE:OFF_TERRTYPE + 4].copy().view(np.uint32).reshape(side, side)
    elev = elev.astype(np.float64)
    water = elev <= water_elev
    overlay_hi = (texdata >> 16) & 0xFFFF
    overlay = (overlay_hi != 0) & (overlay_hi != 0xFFFF)
    return {"elev": elev, "overlay": overlay, "terrtype": terrtype,
            "water": water, "water_elev": water_elev}


# --- Derived feature maps ----------------------------------------------------
def derive_masks(elev: np.ndarray, water: np.ndarray, overlay: np.ndarray) -> dict:
    gy, gx = np.gradient(elev)            # per-cell rise over 1 step
    grad = np.sqrt((gx / WORLD_UNITS_PER_VERTEX) ** 2 +
                   (gy / WORLD_UNITS_PER_VERTEX) ** 2)
    slope_deg = np.degrees(np.arctan(grad))
    lap = np.gradient(gx, axis=1) + np.gradient(gy, axis=0)
    curvature = np.abs(lap)
    # blockiness: local variance of 2nd-difference (stair-step quantization)
    sd_r = np.zeros_like(elev); sd_c = np.zeros_like(elev)
    sd_r[1:-1, :] = elev[2:, :] - 2 * elev[1:-1, :] + elev[:-2, :]
    sd_c[:, 1:-1] = elev[:, 2:] - 2 * elev[:, 1:-1] + elev[:, :-2]
    blockiness = sd_r ** 2 + sd_c ** 2

    land = ~water
    shoreline = land & _dilate(water) & ~water
    cliff = (slope_deg > CLIFF_SLOPE_DEG) & land
    flat_playable = (slope_deg < FLAT_SLOPE_DEG) & land & ~overlay
    return {"slope_deg": slope_deg, "curvature": curvature, "blockiness": blockiness,
            "land": land, "shoreline": shoreline, "cliff": cliff,
            "flat_playable": flat_playable}


def _dilate(mask: np.ndarray) -> np.ndarray:
    out = mask.copy()
    out[1:, :] |= mask[:-1, :]; out[:-1, :] |= mask[1:, :]
    out[:, 1:] |= mask[:, :-1]; out[:, :-1] |= mask[:, 1:]
    return out


def label_components(mask: np.ndarray):
    """4-connected component labeling (BFS, no scipy)."""
    side = mask.shape[0]
    labels = np.zeros_like(mask, dtype=np.int32)
    cur = 0
    for r in range(side):
        for c in range(side):
            if mask[r, c] and labels[r, c] == 0:
                cur += 1
                stack = [(r, c)]
                labels[r, c] = cur
                while stack:
                    y, x = stack.pop()
                    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        ny, nx = y + dy, x + dx
                        if 0 <= ny < side and 0 <= nx < side and mask[ny, nx] and labels[ny, nx] == 0:
                            labels[ny, nx] = cur
                            stack.append((ny, nx))
    return labels, cur


def detect_pyramid_islands(elev, land, water):
    """Small land components, surrounded by water, with a single central peak and
    near-monotonic radial falloff = authored 'pyramid island' artefact."""
    labels, count = label_components(land)
    side = elev.shape[0]
    results = []
    pyramid_mask = np.zeros_like(land, dtype=bool)
    for lid in range(1, count + 1):
        ys, xs = np.where(labels == lid)
        area = len(ys)
        if area == 0 or area > PYRAMID_MAX_AREA:
            continue
        # must be water-bounded (skip components touching map edge — open land)
        if ys.min() == 0 or xs.min() == 0 or ys.max() == side - 1 or xs.max() == side - 1:
            continue
        comp_e = elev[ys, xs]
        peak_i = int(np.argmax(comp_e))
        py, px = ys[peak_i], xs[peak_i]
        cy, cx = ys.mean(), xs.mean()
        peak_off = math.hypot(py - cy, px - cx)
        radius = math.sqrt(area / math.pi) + 1e-6
        # radial monotonicity: corr between -distance-to-peak and elevation
        dist = np.hypot(ys - py, xs - px)
        if comp_e.std() < 1e-6:
            continue
        corr = np.corrcoef(dist, comp_e)[0, 1]  # want strongly negative
        symmetry = 1.0 - min(1.0, peak_off / radius)
        relief = float(comp_e.max() - comp_e.min())
        # score: monotonic falloff + centered peak + meaningful relief
        score = (max(0.0, -corr)) * 0.6 + symmetry * 0.4
        if score > 0.45 and relief > 1.0:
            results.append({"id": int(lid), "area": int(area),
                            "peak_rc": [int(py), int(px)],
                            "centroid_rc": [round(cy, 1), round(cx, 1)],
                            "relief_wu": round(relief, 2),
                            "radial_corr": round(float(corr), 3),
                            "symmetry": round(symmetry, 3),
                            "score": round(score, 3)})
            pyramid_mask[ys, xs] = True
    results.sort(key=lambda d: d["score"], reverse=True)
    return results, pyramid_mask


# --- Terrain object (building/prop) footprints -------------------------------
def read_object_footprints(packets, side: int):
    """Find the terrain-objects packet and rasterize footprints to the grid."""
    wu_map_side = side * WORLD_UNITS_PER_VERTEX
    tlx = -wu_map_side / 2.0
    tly = wu_map_side / 2.0
    foot = np.zeros((side, side), dtype=bool)
    objects = []
    for idx, t, payload in packets:
        dec = decode_packet(t, payload)
        if dec is None or len(dec) < 4:
            continue
        count = struct.unpack_from('<i', dec, 0)[0]
        if count <= 0 or 4 + count * OBJREC_SIZE != len(dec):
            continue  # not the object packet
        for i in range(count):
            base = 4 + i * OBJREC_SIZE
            objType, x, y, z, rot = struct.unpack_from('<i4f', dec, base)
            col = int((x - tlx) / WORLD_UNITS_PER_VERTEX)
            row = int((tly - y) / WORLD_UNITS_PER_VERTEX)
            objects.append({"type": objType, "row": row, "col": col})
            for dr in range(-BUILDING_FOOTPRINT_RADIUS, BUILDING_FOOTPRINT_RADIUS + 1):
                for dc in range(-BUILDING_FOOTPRINT_RADIUS, BUILDING_FOOTPRINT_RADIUS + 1):
                    rr, cc = row + dr, col + dc
                    if 0 <= rr < side and 0 <= cc < side:
                        foot[rr, cc] = True
        return foot, objects, idx
    return foot, objects, None


# --- Mask PNG writers --------------------------------------------------------
def _norm_u8(a: np.ndarray) -> np.ndarray:
    a = a.astype(np.float64)
    lo, hi = float(a.min()), float(a.max())
    if hi - lo < 1e-12:
        return np.zeros(a.shape, dtype=np.uint8)
    return ((a - lo) / (hi - lo) * 255.0).astype(np.uint8)


def save_gray(a_u8: np.ndarray, path: Path):
    Image.fromarray(a_u8, mode='L').save(path)


def save_bool(mask: np.ndarray, path: Path):
    Image.fromarray((mask.astype(np.uint8) * 255), mode='L').save(path)


def save_labels(labels: np.ndarray, path: Path):
    # deterministic pseudo-color per component id
    maxid = int(labels.max())
    rgb = np.zeros((*labels.shape, 3), dtype=np.uint8)
    for lid in range(1, maxid + 1):
        col = ((lid * 73) % 256, (lid * 151) % 256, (lid * 199) % 256)
        rgb[labels == lid] = col
    Image.fromarray(rgb, mode='RGB').save(path)


# --- Driver ------------------------------------------------------------------
def analyze(mission: str, missions_dir: Path, out_root: Path) -> dict:
    pak = missions_dir / f"{mission}.pak"
    if not pak.is_file():
        return {"mission": mission, "error": f"not found: {pak}"}
    packets = read_packets(pak)
    md = locate_mapdata(packets)
    if md is None:
        return {"mission": mission, "error": "no MapData packet matched signature"}
    pkt_idx, side, blocks = md

    water_elev = read_water_elevation(missions_dir / f"{mission}.fit")
    layers = extract_layers(side, blocks, water_elev)
    masks = derive_masks(layers["elev"], layers["water"], layers["overlay"])
    pyramids, pyramid_mask = detect_pyramid_islands(layers["elev"], masks["land"], layers["water"])
    foot, objects, obj_pkt = read_object_footprints(packets, side)

    protected_hard = layers["overlay"] | foot | layers["water"]
    protected_soft = masks["flat_playable"] & ~protected_hard

    out_dir = out_root / mission
    masks_dir = out_dir / "masks"
    masks_dir.mkdir(parents=True, exist_ok=True)

    save_gray(_norm_u8(layers["elev"]), masks_dir / "height.png")
    save_gray(_norm_u8(np.clip(masks["slope_deg"] / 90.0, 0, 1) * 255), masks_dir / "slope.png")
    save_gray(_norm_u8(masks["curvature"]), masks_dir / "curvature.png")
    save_gray(_norm_u8(masks["blockiness"]), masks_dir / "blockiness.png")
    save_bool(layers["water"], masks_dir / "water.png")
    save_bool(masks["shoreline"], masks_dir / "shoreline.png")
    lbl, _ = label_components(masks["land"])
    save_labels(lbl, masks_dir / "islands.png")
    save_bool(pyramid_mask, masks_dir / "pyramid_island_candidates.png")
    save_bool(masks["cliff"], masks_dir / "cliff_candidates.png")
    save_bool(masks["flat_playable"], masks_dir / "flat_playable.png")
    save_bool(layers["overlay"], masks_dir / "roads_overlays.png")
    save_bool(foot, masks_dir / "building_footprints.png")
    save_bool(protected_hard, masks_dir / "protected_hard.png")
    save_bool(protected_soft, masks_dir / "protected_soft.png")

    cells = side * side
    sd = masks["slope_deg"]
    hist_edges = [0, 5, 10, 20, 35, 50, 90]
    hist = {f"{hist_edges[i]}-{hist_edges[i+1]}deg":
            int(np.sum((sd >= hist_edges[i]) & (sd < hist_edges[i + 1])))
            for i in range(len(hist_edges) - 1)}

    report = {
        "mission": mission,
        "pak": str(pak),
        "mapdata_packet": pkt_idx,
        "object_packet": obj_pkt,
        "grid_side": side,
        "world_units_per_vertex": WORLD_UNITS_PER_VERTEX,
        "map_extent_wu": (side - 1) * WORLD_UNITS_PER_VERTEX,
        "water_elevation_wu": water_elev,
        "elevation_wu": {"min": float(layers["elev"].min()),
                         "max": float(layers["elev"].max()),
                         "mean": round(float(layers["elev"].mean()), 2)},
        "slope_histogram_cells": hist,
        "coverage_pct": {
            "water": round(100 * layers["water"].sum() / cells, 2),
            "shoreline": round(100 * masks["shoreline"].sum() / cells, 2),
            "overlay_roads": round(100 * layers["overlay"].sum() / cells, 2),
            "cliff_candidates": round(100 * masks["cliff"].sum() / cells, 2),
            "flat_playable": round(100 * masks["flat_playable"].sum() / cells, 2),
            "building_footprints": round(100 * foot.sum() / cells, 2),
            "protected_hard": round(100 * protected_hard.sum() / cells, 2),
            "editable_soft": round(100 * protected_soft.sum() / cells, 2),
        },
        "object_count": len(objects),
        "pyramid_island_candidates_top10": pyramids[:10],
        "thresholds": {"cliff_slope_deg": CLIFF_SLOPE_DEG,
                       "flat_slope_deg": FLAT_SLOPE_DEG,
                       "pyramid_max_area_cells": PYRAMID_MAX_AREA,
                       "footprint_radius_cells": BUILDING_FOOTPRINT_RADIUS},
        "masks_dir": str(masks_dir),
    }
    (out_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="MISSION-TERRAIN-ANALYZER-1 (read-only)")
    ap.add_argument("missions", nargs="*", default=["mc2_01"],
                    help="mission names (no .pak). Default: mc2_01")
    add_missions_dir_arg(ap)
    ap.add_argument("--out", default="tests/terrain/beautify",
                    help="output root (per-mission subdir created)")
    args = ap.parse_args()

    try:
        missions_dir = resolve_from_args(args, missions=args.missions).dir
    except StockDataError as e:
        print(f"[analyzer] ERROR {e}", file=sys.stderr)
        return 4
    out_root = Path(args.out)

    rc = 0
    for m in args.missions:
        rep = analyze(m, missions_dir, out_root)
        if rep.get("error"):
            print(f"[analyzer] {m}: ERROR {rep['error']}", file=sys.stderr)
            rc = 1
            continue
        cov = rep["coverage_pct"]
        print(f"[analyzer] {m}: grid={rep['grid_side']}^2 "
              f"objs={rep['object_count']} water={cov['water']}% "
              f"cliff={cov['cliff_candidates']}% protected={cov['protected_hard']}% "
              f"pyramids={len(rep['pyramid_island_candidates_top10'])} "
              f"-> {rep['masks_dir']}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
