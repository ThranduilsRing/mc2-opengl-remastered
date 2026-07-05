#!/usr/bin/env python3
"""TERRAIN-SCATTER-MASK-1: mask-driven rock-prop scatter, cook-offline.

Reads a stock mission .pak, derives analyzer masks (slope/water/overlay/
shoreline/building-footprint), runs a seeded mask-weighted blue-noise
placement over the flat/rock-eligible cells, and (only on an explicit
`cook` invocation) appends the chosen placements as ordinary packet-1
terrain-object records into a NEW .pak (never mutates the stock file).

Design doc: .claude/TERRAIN-SCATTER-MASK-1-RECON.md (authoritative).

USER+ADVISOR RULINGS (locked, v0):
  1. Mask source = analyzer-derived only (slope/water/overlay-footprint/
     shoreline). No painted .beauty/scatter_*.png override yet (v1); the
     mask-ingest interface below is written so a PNG can slot in later
     (see `load_density_mask`).
  2. Prop = MarbleCliffScatter (FitID 1189, BUILDING-class, yaw-only, no
     tilt; a dedicated clone of hand-placeable MarbleCliff FitID 1188, same
     AppearanceName="marblecliff" -- keeps scatter re-cooks from ever
     touching hand placements, which share the 1188 type).
  3. Cap = 500 HARD (tool-enforced); --count for a lower request.
  4. Dedicated scatter objTypeNum(s) reserved below; re-cook = delete
     prior scatter records by objTypeNum, then re-emit.
  5. Output = hash-gated NEW pak only; stock is never mutated in place.

Object record format (code/objmgr.cpp:1259-1302, 40 bytes, little-endian):
  objTypeNum(i32), x(f32), y(f32), z(f32), rotation(f32),
  damage(i32), teamId(i32), parentId(i32), pad(i32), pad(i32)

Subcommands:
  plan   (default-safe) -- pak + masks -> deterministic manifest JSON + preview
         PNG. Never touches the .pak. Reports candidate/rejected/final counts
         and a rejection-reason breakdown.
  cook   -- pak-append from a manifest. Refuses unless: baseHash of the
         input .pak matches the manifest's recorded source hash, objTypeNum
         is in RESERVED_SCATTER_OBJTYPENUMS, count <= HARD_CAP, and either
         --prop-verified or --allow-unverified-prop is passed.
  revert -- given a cooked .pak, strip all records whose objTypeNum is in
         RESERVED_SCATTER_OBJTYPENUMS -> new .pak.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, decode_packet, locate_mapdata, extract_layers, derive_masks,
    read_object_footprints, read_water_elevation, _norm_u8,
    WORLD_UNITS_PER_VERTEX, OBJREC_SIZE, PACKET_MAGIC, TYPE_SHIFT, OFFSET_MASK,
    ST_RAW, ST_ZLIB,
)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pak_append  # noqa: E402

# --- reserved objTypeNums (user ruling #4) -----------------------------------
# v0 originally reused MarbleCliff itself (FitID 1188, hand-placeable) as the
# scatter prop, which meant a re-cook's "delete by objTypeNum" step could nuke
# hand placements sharing that same type. Fixed by installing a DEDICATED
# scatter clone -- "MarbleCliffScatter" (FitID/objTypeNum 1189, same
# AppearanceName="marblecliff" so it renders identically) -- via
# tools/install_cliff_dressing.py --with-scatter-clone. Scatter cooks now
# exclusively use 1189; 1188 (hand-placeable MarbleCliff) is never touched by
# a re-cook. If a further scatter prop type is later installed, its
# FitID/objTypeNum must be appended to this tuple and documented here (do not
# reuse a hand-placed building's type).
# TERRAIN-TREE-SCATTER-V0: three dedicated TREE-class scatter clones were
# added (tools/install_tree_scatter.py) -- "PineScatter1/2/3", FitID/
# objTypeNum 1190/1191/1192, reusing stock AppearanceName="Pine1"/"Pine2"/
# "Pine3" (shapes+icons already fully resolved via tgl.fst/art.fst, no new
# art). Same rationale as the rock clone: a dedicated objTypeNum per class
# means a re-cook's delete-by-type step can never touch hand placements of
# the stock Pine1/2/3 catalog entries (FitID 210/352/353), which share the
# same AppearanceName but are NOT reserved scatter types.
PINE_SCATTER_OBJTYPENUMS = (1190, 1191, 1192)

RESERVED_SCATTER_OBJTYPENUMS = (1189,) + PINE_SCATTER_OBJTYPENUMS
MARBLECLIFF_OBJTYPENUM = 1189

# Per-channel prop lists: "rock" scatters a single objTypeNum; "tree" picks
# randomly per-instance across all three PineScatter clones for silhouette
# variety (no per-instance scale field exists on the 40-byte record -- see
# TERRAIN-SCATTER-MASK-1-RECON.md landmine #2 -- so variety must come from
# prop *type*, not scale).
PROP_CHANNELS = {
    "rock": (MARBLECLIFF_OBJTYPENUM,),
    "tree": PINE_SCATTER_OBJTYPENUMS,
}

HARD_CAP = 500
DEFAULT_MIN_DIST_WU = 256.0  # blue-noise minimum spacing, world units
DEFAULT_SLOPE_MAX_DEG = 40.0  # reject if steeper (single-point grounding risk) -- rock channel
# Tree channel favors a moderate slope band (per TERRAIN-TREE-SCATTER-ASSET-
# RECON-1 recommendation: conifers on gaea_peaks-style slopes read best in a
# 5-25 degree band -- too flat looks planted-in-a-field, too steep is both
# visually wrong for a rooted tree and a single-point-grounding risk same as
# rock). Trees EXCLUDE outside this band; rock instead prefers the shoulder
# around the cliff band (see load_density_mask).
DEFAULT_TREE_SLOPE_MIN_DEG = 5.0
DEFAULT_TREE_SLOPE_MAX_DEG = 25.0


# --- deterministic seeded RNG helpers (PYTHONHASHSEED-proof: no dict/set
#     iteration order dependence, no builtin hash() use) --------------------
def _cell_rng(seed: int, row: int, col: int) -> np.random.Generator:
    """Per-cell RNG stream derived from a fixed seed + integer coords via
    SplitMix64-style mixing -- deterministic across processes/interpreters
    regardless of PYTHONHASHSEED (no dict ordering or hash() involved)."""
    x = (seed * 0x9E3779B97F4A7C15 + row * 0xBF58476D1CE4E5B9 + col * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 27
    x = (x * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 31
    return np.random.default_rng(x & 0xFFFFFFFF)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


# --- mask ingest (interface built for a future painted-PNG override) --------
def load_density_mask(side: int, masks: dict, png_path: Path | None,
                       channel: str = "rock") -> np.ndarray:
    """Return a [0,1] float density grid (side x side). v0: purely
    analyzer-derived (no painted override). If png_path is given (v1, NOT
    used yet per user ruling #1) it would be decoded + resized to `side` and
    substituted here -- the call site below never needs to change.

    channel="rock" (default): favors cliff/steep-but-not-too-steep terrain,
    i.e. the shoulder around the cliff band, biased toward protected_soft
    (flat_playable minus hard exclusions) at lower weight so rocks aren't
    ONLY glued to cliff faces.

    channel="tree": favors the moderate DEFAULT_TREE_SLOPE_MIN_DEG..
    DEFAULT_TREE_SLOPE_MAX_DEG band (conifer-on-slope look, per
    TERRAIN-TREE-SCATTER-ASSET-RECON-1) with a flat_playable bonus so trees
    aren't ONLY glued to the slope band either -- density peaks inside the
    band and falls off outside it (both flatter and steeper read as wrong for
    a rooted tree)."""
    if png_path is not None:
        img = Image.open(png_path).convert("L").resize((side, side), Image.BILINEAR)
        return np.asarray(img, dtype=np.float64) / 255.0
    slope = masks["slope_deg"]
    flat_bonus = masks["flat_playable"].astype(np.float64) * 0.25
    if channel == "tree":
        # Triangular density peaking mid-band: ramp up from MIN to the
        # midpoint, ramp down from the midpoint to MAX. Zero outside the
        # band (the exclusion mask below is the hard cutoff; this just
        # shapes preference within the eligible band).
        mid = (DEFAULT_TREE_SLOPE_MIN_DEG + DEFAULT_TREE_SLOPE_MAX_DEG) / 2.0
        rising = np.clip((slope - DEFAULT_TREE_SLOPE_MIN_DEG) / (mid - DEFAULT_TREE_SLOPE_MIN_DEG), 0.0, 1.0)
        falling = np.clip((DEFAULT_TREE_SLOPE_MAX_DEG - slope) / (DEFAULT_TREE_SLOPE_MAX_DEG - mid), 0.0, 1.0)
        band = np.minimum(rising, falling)
        density = np.clip(band + flat_bonus, 0.0, 1.0)
        return density
    cliff_band = np.clip((slope - 15.0) / (DEFAULT_SLOPE_MAX_DEG - 15.0), 0.0, 1.0)
    density = np.clip(cliff_band + flat_bonus, 0.0, 1.0)
    return density


def compute_exclusion(layers: dict, masks: dict, foot: np.ndarray,
                       channel: str = "rock") -> dict:
    """Per-cell hard-exclusion reasons (recon `protected_hard` = overlay |
    building_footprints | water), plus the slope gate as its own reason so
    the rejection breakdown in the report is legible.

    channel="rock": excludes slope > DEFAULT_SLOPE_MAX_DEG (too steep for
    single-point grounding).
    channel="tree": excludes OUTSIDE [DEFAULT_TREE_SLOPE_MIN_DEG,
    DEFAULT_TREE_SLOPE_MAX_DEG] -- the slope-gate "flip" from the recon:
    rock/cliff dressing excludes steep terrain, trees on gaea_peaks-style
    slopes instead favor (and are gated to) a moderate band, excluding both
    too-flat and too-steep cells.

    Water/shoreline/concrete(overlay)/building-footprint exclusion is
    UNCHANGED across channels per the recon ("water/shore/concrete exclusion
    stays") -- only the slope gate differs by channel."""
    slope = masks["slope_deg"]
    if channel == "tree":
        slope_excl = (slope < DEFAULT_TREE_SLOPE_MIN_DEG) | (slope > DEFAULT_TREE_SLOPE_MAX_DEG)
    else:
        slope_excl = slope > DEFAULT_SLOPE_MAX_DEG
    return {
        "water": layers["water"],
        "overlay": layers["overlay"],
        "building_footprint": foot,
        "slope": slope_excl,
    }


# --- blue-noise mask-weighted placement --------------------------------------
def plan_placements(side: int, elev: np.ndarray, density: np.ndarray,
                     exclusion: dict, count: int, seed: int,
                     min_dist_wu: float, prop_pool: tuple[int, ...] | None = None) -> dict:
    """Deterministic seeded candidate walk in row-major cell order (NOT set/
    dict iteration) -> mask-weighted accept/reject -> greedy min-distance
    blue-noise thinning. Returns dict with accepted placements (row,col,x,y,
    z,yaw[,objTypeNum]) and a rejection reason tally.

    prop_pool: optional tuple of objTypeNums (e.g. PINE_SCATTER_OBJTYPENUMS).
    When given, each ACCEPTED instance draws one objTypeNum uniformly at
    random from the pool via the SAME per-cell RNG stream already used for
    the density roll + yaw (so the pick is deterministic for a given seed,
    same PYTHONHASHSEED-proof guarantee as the rest of the walk) -- this is
    how "per-channel prop lists...random pick per instance for variety" is
    implemented without a per-instance scale field (which the 40-byte record
    doesn't have; variety comes from prop TYPE, not scale, per recon landmine
    #2). When prop_pool is None (rock channel, single prop), no objTypeNum
    key is added and the caller supplies one fixed objTypeNum at cook time,
    matching the pre-existing single-prop behavior byte-for-byte."""
    hard_excl = np.zeros((side, side), dtype=bool)
    for m in exclusion.values():
        hard_excl |= m

    wu_map_side = side * WORLD_UNITS_PER_VERTEX
    tlx = -wu_map_side / 2.0
    tly = wu_map_side / 2.0

    rejects = {"water": 0, "overlay": 0, "building_footprint": 0, "slope": 0,
               "spacing": 0, "density_roll": 0, "cap": 0}
    accepted = []
    accepted_xy = []  # for min-distance check

    min_dist_sq = min_dist_wu * min_dist_wu

    for row in range(side):
        for col in range(side):
            if len(accepted) >= count:
                break
            if hard_excl[row, col]:
                # attribute to the first matching reason, in a fixed order
                if exclusion["water"][row, col]:
                    rejects["water"] += 1
                elif exclusion["overlay"][row, col]:
                    rejects["overlay"] += 1
                elif exclusion["building_footprint"][row, col]:
                    rejects["building_footprint"] += 1
                else:
                    rejects["slope"] += 1
                continue
            d = float(density[row, col])
            if d <= 0.0:
                rejects["density_roll"] += 1
                continue
            rng = _cell_rng(seed, row, col)
            roll = rng.random()
            if roll > d:
                rejects["density_roll"] += 1
                continue
            x = tlx + col * WORLD_UNITS_PER_VERTEX
            y = tly - row * WORLD_UNITS_PER_VERTEX
            too_close = False
            for (ax, ay) in accepted_xy:
                dx, dy = x - ax, y - ay
                if dx * dx + dy * dy < min_dist_sq:
                    too_close = True
                    break
            if too_close:
                rejects["spacing"] += 1
                continue
            yaw = float(rng.random() * 2.0 * math.pi)
            z = float(elev[row, col])
            placement = {"row": row, "col": col, "x": x, "y": y, "z": z, "yaw": yaw}
            if prop_pool:
                # same rng stream, drawn AFTER yaw so single-prop channels
                # (prop_pool=None) are byte-identical to pre-existing runs.
                pick_idx = int(rng.integers(0, len(prop_pool)))
                placement["objTypeNum"] = prop_pool[pick_idx]
            accepted.append(placement)
            accepted_xy.append((x, y))
        if len(accepted) >= count:
            break

    # Anything beyond `count` we never reached is not a rejection -- it is
    # simply un-sampled (report distinguishes "candidate" = accepted+rejected
    # cells actually visited before the walk stopped).
    candidates_visited = len(accepted) + sum(rejects.values())
    return {"accepted": accepted, "rejects": rejects,
            "candidates_visited": candidates_visited}


# --- manifest -----------------------------------------------------------------
def write_manifest(out_dir: Path, mission: str, pak_path: Path, side: int,
                    seed: int, count_requested: int, objtypenum: int,
                    result: dict, thresholds: dict,
                    channel: str = "rock", prop_pool: tuple[int, ...] | None = None) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "format": "mc2-scatter-manifest/1",
        "mission": mission,
        "source_pak": str(pak_path),
        "baseHash": sha256_file(pak_path),
        "grid_side": side,
        "seed": seed,
        "count_requested": count_requested,
        "count_final": len(result["accepted"]),
        # objTypeNum: fixed prop for single-prop channels (rock); for a
        # multi-prop channel (tree) this is the FALLBACK/primary value only --
        # each placement's own "objTypeNum" key (set when prop_pool is used)
        # is authoritative. Kept at manifest level for backward-compat with
        # single-channel consumers/tests that only read this key.
        "objTypeNum": objtypenum,
        "channel": channel,
        "prop_pool": list(prop_pool) if prop_pool else None,
        "thresholds": thresholds,
        "rejects": result["rejects"],
        "candidates_visited": result["candidates_visited"],
        "placements": result["accepted"],
    }
    path = out_dir / f"{mission}.scatter_manifest.json"
    path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return path


def render_preview(out_dir: Path, mission: str, elev: np.ndarray,
                    exclusion: dict, placements: list, side: int) -> Path:
    """Height/mask composite with placement points overlaid (recon step 6,
    terrain_workbench.py conventions: PIL + numpy only)."""
    base = _norm_u8(elev)
    rgb = np.stack([base, base, base], axis=-1).copy()
    hard = np.zeros((side, side), dtype=bool)
    for m in exclusion.values():
        hard |= m
    rgb[hard] = (rgb[hard] * 0.35 + np.array([120, 30, 30]) * 0.65).astype(np.uint8)

    scale = max(1, 512 // side)
    img = Image.fromarray(rgb, "RGB").resize((side * scale, side * scale), Image.NEAREST)
    draw = ImageDraw.Draw(img)
    for p in placements:
        cx, cy = p["col"] * scale + scale // 2, p["row"] * scale + scale // 2
        r = max(2, scale // 3)
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(60, 220, 90), outline=(0, 0, 0))
    path = out_dir / f"{mission}.scatter_preview.png"
    img.save(path)
    return path


# --- packet-1 rewriter (models pak_append.py cmd_replace + write_pak) -------
def _decode_object_packet(packets, side: int):
    """Find packet-1-shaped payload (count + N*40B) same signature as
    analyzer.read_object_footprints, but also return which PacketRecord index
    and its on-disk storage type so we can write back correctly."""
    for rec in packets:
        dec = decode_packet(rec.storage_type, rec.payload)
        if dec is None or len(dec) < 4:
            continue
        count = struct.unpack_from('<i', dec, 0)[0]
        if count <= 0 or 4 + count * OBJREC_SIZE != len(dec):
            continue
        return rec.index, rec.storage_type, dec
    return None, None, None


def _encode_records(records: list[tuple]) -> bytes:
    out = struct.pack('<i', len(records))
    for (objType, x, y, z, rot, dmg, team, parent, p0, p1) in records:
        out += struct.pack('<i4f5i', objType, x, y, z, rot, dmg, team, parent, p0, p1)
    return out


def _read_records(dec: bytes) -> list[tuple]:
    count = struct.unpack_from('<i', dec, 0)[0]
    recs = []
    for i in range(count):
        base = 4 + i * OBJREC_SIZE
        recs.append(struct.unpack_from('<i4f5i', dec, base))
    return recs


def cook_scatter(pak_path: Path, manifest_path: Path, out_pak: Path,
                  prop_verified: bool, allow_unverified: bool) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    objtypenum = manifest["objTypeNum"]
    placements = manifest["placements"]

    # Validate every objTypeNum that will actually be written: the
    # manifest-level fallback (single-prop channels, e.g. rock) AND any
    # per-placement override (multi-prop channels, e.g. tree's random pick
    # across PineScatter1/2/3) -- both must be reserved scatter types so a
    # re-cook's delete-by-type step stays exhaustive.
    per_placement_types = {p.get("objTypeNum", objtypenum) for p in placements}
    bad_types = per_placement_types - set(RESERVED_SCATTER_OBJTYPENUMS)
    if bad_types:
        raise ValueError(
            f"objTypeNum(s) {sorted(bad_types)} not in RESERVED_SCATTER_OBJTYPENUMS "
            f"{RESERVED_SCATTER_OBJTYPENUMS} -- refusing cook.")
    if len(placements) > HARD_CAP:
        raise ValueError(f"manifest has {len(placements)} placements > HARD_CAP={HARD_CAP} -- refusing cook.")
    if not prop_verified and not allow_unverified:
        raise ValueError(
            "refusing cook: pass --prop-verified (MarbleCliff editor visual "
            "check passed) or --allow-unverified-prop to override.")

    cur_hash = sha256_file(pak_path)
    if cur_hash != manifest["baseHash"]:
        raise ValueError(
            f"baseHash mismatch: manifest expects {manifest['baseHash'][:12]}..., "
            f"target .pak is {cur_hash[:12]}... (refusing -- wrong/modified pak).")

    packets = pak_append.read_packets(pak_path)
    side = manifest["grid_side"]

    # locate object packet using the same signature scan as the analyzer,
    # but on pak_append.PacketRecord objects (storage_type/payload fields).
    from mission_terrain_analyzer import decode_packet as _decode
    obj_idx, storage_type, dec = None, None, None
    for rec in packets:
        d = _decode(rec.storage_type, rec.payload)
        if d is None or len(d) < 4:
            continue
        count = struct.unpack_from('<i', d, 0)[0]
        if count <= 0 or 4 + count * OBJREC_SIZE != len(d):
            continue
        obj_idx, storage_type, dec = rec.index, rec.storage_type, d
        break
    if obj_idx is None:
        raise ValueError("no packet-1-shaped (terrain-objects) packet found in this .pak")

    existing = _read_records(dec)
    # re-cook idempotency: strip any prior records of our reserved type(s).
    kept = [r for r in existing if r[0] not in RESERVED_SCATTER_OBJTYPENUMS]
    removed = len(existing) - len(kept)

    new_records = [
        (int(p.get("objTypeNum", objtypenum)), float(p["x"]), float(p["y"]), float(p["z"]), float(p["yaw"]),
         0, -1, -1, 0, 0)
        for p in placements
    ]
    all_records = kept + new_records
    new_payload_dec = _encode_records(all_records)

    if storage_type == pak_append.ST_ZLIB:
        new_payload = pak_append.make_zlib_payload(new_payload_dec)
    elif storage_type == pak_append.ST_RAW:
        new_payload = new_payload_dec
    else:
        raise ValueError(f"unsupported terrain-objects packet storage type {storage_type}")

    packets[obj_idx] = pak_append.PacketRecord(obj_idx, storage_type, new_payload)
    out_pak.parent.mkdir(parents=True, exist_ok=True)
    pak_append.write_pak(packets, out_pak)

    return {
        "out_pak": str(out_pak),
        "objTypeNum": objtypenum,
        "prior_scatter_removed": removed,
        "new_scatter_written": len(new_records),
        "final_object_count": len(all_records),
        "object_packet_index": obj_idx,
    }


def revert_scatter(pak_path: Path, out_pak: Path) -> dict:
    packets = pak_append.read_packets(pak_path)
    from mission_terrain_analyzer import decode_packet as _decode
    obj_idx, storage_type, dec = None, None, None
    for rec in packets:
        d = _decode(rec.storage_type, rec.payload)
        if d is None or len(d) < 4:
            continue
        count = struct.unpack_from('<i', d, 0)[0]
        if count <= 0 or 4 + count * OBJREC_SIZE != len(d):
            continue
        obj_idx, storage_type, dec = rec.index, rec.storage_type, d
        break
    if obj_idx is None:
        raise ValueError("no packet-1-shaped (terrain-objects) packet found in this .pak")

    existing = _read_records(dec)
    kept = [r for r in existing if r[0] not in RESERVED_SCATTER_OBJTYPENUMS]
    removed = len(existing) - len(kept)
    new_payload_dec = _encode_records(kept)
    if storage_type == pak_append.ST_ZLIB:
        new_payload = pak_append.make_zlib_payload(new_payload_dec)
    elif storage_type == pak_append.ST_RAW:
        new_payload = new_payload_dec
    else:
        raise ValueError(f"unsupported terrain-objects packet storage type {storage_type}")

    packets[obj_idx] = pak_append.PacketRecord(obj_idx, storage_type, new_payload)
    out_pak.parent.mkdir(parents=True, exist_ok=True)
    pak_append.write_pak(packets, out_pak)
    return {"out_pak": str(out_pak), "removed": removed, "remaining": len(kept)}


# --- driver / CLI --------------------------------------------------------------
def _load_common(mission: str, missions_dir: Path):
    pak = missions_dir / f"{mission}.pak"
    if not pak.is_file():
        raise FileNotFoundError(str(pak))
    packets = read_packets(pak)
    md = locate_mapdata(packets)
    if md is None:
        raise ValueError(f"{mission}: no MapData packet matched signature")
    pkt_idx, side, blocks = md
    water_elev = read_water_elevation(missions_dir / f"{mission}.fit")
    layers = extract_layers(side, blocks, water_elev)
    masks = derive_masks(layers["elev"], layers["water"], layers["overlay"])
    foot, objects, obj_pkt = read_object_footprints(packets, side)
    return pak, side, layers, masks, foot


def cmd_plan(args) -> int:
    missions_dir = Path(args.missions_dir)
    out_dir = Path(args.out)
    count = min(args.count, HARD_CAP)
    if args.count > HARD_CAP:
        print(f"[scatter] WARNING requested count {args.count} > HARD_CAP {HARD_CAP}; clamping.",
              file=sys.stderr)

    channel = args.channel
    prop_pool = PROP_CHANNELS.get(channel, (args.objtypenum,))
    fallback_objtypenum = prop_pool[0]

    pak, side, layers, masks, foot = _load_common(args.mission, missions_dir)
    exclusion = compute_exclusion(layers, masks, foot, channel=channel)
    density = load_density_mask(side, masks, Path(args.density_png) if args.density_png else None,
                                 channel=channel)

    slope_max = DEFAULT_TREE_SLOPE_MAX_DEG if channel == "tree" else DEFAULT_SLOPE_MAX_DEG
    thresholds = {"slope_max_deg": slope_max, "min_dist_wu": args.min_dist,
                  "hard_cap": HARD_CAP}
    if channel == "tree":
        thresholds["slope_min_deg"] = DEFAULT_TREE_SLOPE_MIN_DEG
    # single-prop channels (rock) pass prop_pool=None so plan_placements
    # never adds a per-placement objTypeNum key -- byte-identical to the
    # pre-multi-channel behavior for existing rock callers/tests.
    walk_pool = prop_pool if len(prop_pool) > 1 else None
    result = plan_placements(side, layers["elev"], density, exclusion, count, args.seed,
                              args.min_dist, prop_pool=walk_pool)

    manifest_path = write_manifest(out_dir, args.mission, pak, side, args.seed, args.count,
                                    fallback_objtypenum, result, thresholds,
                                    channel=channel, prop_pool=prop_pool if walk_pool else None)
    preview_path = render_preview(out_dir, args.mission, layers["elev"], exclusion,
                                   result["accepted"], side)

    rejects = result["rejects"]
    top = sorted(rejects.items(), key=lambda kv: kv[1], reverse=True)
    top_str = ", ".join(f"{k}={v}" for k, v in top if v > 0) or "none"
    print(f"[scatter] plan {args.mission}: final={len(result['accepted'])} "
          f"candidates_visited={result['candidates_visited']} rejects[{top_str}] "
          f"-> {manifest_path.name}, {preview_path.name}")
    return 0


def cmd_cook(args) -> int:
    try:
        stats = cook_scatter(Path(args.pak), Path(args.manifest), Path(args.out),
                              args.prop_verified, args.allow_unverified_prop)
    except ValueError as e:
        print(f"[scatter] cook refused: {e}", file=sys.stderr)
        return 1
    print(f"[scatter] cook: objTypeNum={stats['objTypeNum']} "
          f"removed_prior={stats['prior_scatter_removed']} "
          f"written={stats['new_scatter_written']} "
          f"final_object_count={stats['final_object_count']} -> {stats['out_pak']}")
    return 0


def cmd_revert(args) -> int:
    stats = revert_scatter(Path(args.pak), Path(args.out))
    print(f"[scatter] revert: removed={stats['removed']} remaining={stats['remaining']} "
          f"-> {stats['out_pak']}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="TERRAIN-SCATTER-MASK-1: mask-driven scatter cook")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_plan = sub.add_parser("plan", help="dry-run: masks + blue-noise -> manifest + preview (default-safe)")
    p_plan.add_argument("mission")
    p_plan.add_argument("--missions-dir", default="A:/Games/Carver5-feasibility/data/missions")
    p_plan.add_argument("--out", default="_harness_out/scatter")
    p_plan.add_argument("--count", type=int, default=200)
    p_plan.add_argument("--seed", type=int, default=1)
    p_plan.add_argument("--min-dist", type=float, default=DEFAULT_MIN_DIST_WU)
    p_plan.add_argument("--objtypenum", type=int, default=MARBLECLIFF_OBJTYPENUM,
                         help="single-prop fallback objTypeNum; ignored for channels with a prop_pool (e.g. tree)")
    p_plan.add_argument("--channel", choices=sorted(PROP_CHANNELS.keys()), default="rock",
                         help="prop channel: rock (MarbleCliffScatter, slope>MAX excluded) or "
                              "tree (PineScatter1/2/3 random pick per instance, slope band "
                              f"[{DEFAULT_TREE_SLOPE_MIN_DEG},{DEFAULT_TREE_SLOPE_MAX_DEG}] favored/gated)")
    p_plan.add_argument("--density-png", default=None,
                         help="v1 override (NOT used in v0): a painted density PNG, resized to grid side")
    p_plan.set_defaults(func=cmd_plan)

    p_cook = sub.add_parser("cook", help="append manifest placements into a NEW .pak (hash-gated)")
    p_cook.add_argument("--pak", required=True)
    p_cook.add_argument("--manifest", required=True)
    p_cook.add_argument("--out", required=True)
    p_cook.add_argument("--prop-verified", action="store_true",
                         help="MarbleCliff editor visual check has been performed")
    p_cook.add_argument("--allow-unverified-prop", action="store_true",
                         help="override the --prop-verified gate")
    p_cook.set_defaults(func=cmd_cook)

    p_revert = sub.add_parser("revert", help="strip reserved-objTypeNum scatter records -> new .pak")
    p_revert.add_argument("--pak", required=True)
    p_revert.add_argument("--out", required=True)
    p_revert.set_defaults(func=cmd_revert)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
