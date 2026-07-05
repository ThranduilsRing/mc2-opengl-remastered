#!/usr/bin/env python3
"""
MCP-ANTI-CHURN-1: mission_facts.py
One-call mission ground truth: pak grid geometry, water elevation, and which
deploy lanes carry the pak + beauty sidecars.

Grounded failure: agents re-derived the same facts every session (vertex side
counts, water elev, "is the control_map deployed to 0.5-testing?") by pak/dir
spelunking — this module answers all of it in one shot.

Format references (mirrors tools/terrain_beautify/mission_terrain_analyzer.py,
kept PURE STDLIB here so the MCP server never needs numpy/PIL):
  - PacketFile: mclib/packet.h  (magic 0xFEEDFACE, entry = type<<29 | offset)
  - MapData packet: realVerticesMapSide^2 * 32, side in {60,80,100,120}
  - worldUnitsPerVertex = 128 (mclib/terrain.cpp:101)
  - water level = [Water] "f Elevation" in the mission .fit (terrain.cpp:3740)
  - sidecars: data/missions/<stem>.beauty/{control_map.png, shoreline_mask.png,
    visual_height_4x.r32, sidecar.json} (control_map_tool.py / cook_shoreline.py
    / mclib/terrain.cpp load paths)

LZW-compressed packets are skipped (needs the analyzer's LZW impl); if the
MapData packet is LZW-only, side is reported null with a note.
"""
from __future__ import annotations

import math
import struct
import sys
import zlib
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

PACKET_MAGIC = 0xFEEDFACE
TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1
ST_RAW, ST_FWF, ST_LZD, ST_HF, ST_ZLIB, ST_NUL = 0x0, 0x1, 0x2, 0x3, 0x4, 0x7
PCV_SIZE = 32
OFF_ELEV = 12
WORLD_UNITS_PER_VERTEX = 128.0
VALID_GRID_SIDES = (60, 80, 100, 120)

SIDECAR_FILES = ("control_map.png", "shoreline_mask.png",
                 "visual_height_4x.r32", "sidecar.json")

# Fallback lane map — kept in lockstep with scripts/smoke_lib/deploy_lease.py
# DEPLOY_FOLDERS (imported live when available; this is only the net).
_FALLBACK_LANES: list[tuple[str, str]] = [
    ("0.4",         r"A:/Games/mc2-opengl/mc2-win64-v0.4"),
    ("0.4d-rc1",    r"A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1"),
    ("0.5.0",       r"A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0"),
    ("0.5-testing", r"A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"),
]


def deploy_lanes(repo_root: Path) -> list[tuple[str, str]]:
    """Canonical (name, path) deploy lanes; live-imported from deploy_lease."""
    try:
        scripts = str(Path(repo_root) / "scripts")
        if scripts not in sys.path:
            sys.path.insert(0, scripts)
        from smoke_lib.deploy_lease import DEPLOY_FOLDERS  # type: ignore
        return list(DEPLOY_FOLDERS)
    except Exception:
        return list(_FALLBACK_LANES)


# ---------------------------------------------------------------------------
# Pak parsing (pure stdlib)
# ---------------------------------------------------------------------------

def _read_packets(data: bytes):
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != PACKET_MAGIC:
        raise ValueError(f"not a PacketFile (magic={magic:#x})")
    first_off = struct.unpack_from("<I", data, 4)[0]
    num = (first_off // 4) - 2
    ent = [struct.unpack_from("<I", data, 8 + 4 * i)[0] for i in range(num)]
    out = []
    for i in range(num):
        t = (ent[i] >> TYPE_SHIFT) & 0x7
        off = ent[i] & OFFSET_MASK
        end = (ent[i + 1] & OFFSET_MASK) if i + 1 < num else len(data)
        out.append((i, t, data[off:end]))
    return out


def _decode(t: int, payload: bytes) -> Optional[bytes]:
    if t == ST_RAW:
        return payload
    if t == ST_ZLIB:
        try:
            return zlib.decompress(payload[4:]) if len(payload) >= 4 else None
        except zlib.error:
            return None
    if t == ST_NUL:
        return b""
    return None  # LZD/FWF/HF unhandled here (see mission_terrain_analyzer.py)


def _locate_mapdata(packets):
    best = None
    skipped_lzd = False
    for idx, t, payload in packets:
        if t == ST_LZD:
            skipped_lzd = True
        dec = _decode(t, payload)
        if dec is None or len(dec) <= 0 or len(dec) % PCV_SIZE != 0:
            continue
        cells = len(dec) // PCV_SIZE
        side = math.isqrt(cells)
        if side * side == cells and side in VALID_GRID_SIDES:
            if best is None or len(dec) > len(best[2]):
                best = (idx, side, dec)
    return best, skipped_lzd


def parse_pak(pak_path: Path) -> dict:
    """Return {side, verts, world_size_wu, elev_min, elev_max, packets, note}."""
    result: dict = {"pak": str(pak_path), "side": None, "verts": None,
                    "world_size_wu": None, "elev_min": None, "elev_max": None,
                    "packets": None, "note": ""}
    try:
        data = Path(pak_path).read_bytes()
        packets = _read_packets(data)
    except (OSError, ValueError, struct.error) as e:
        result["note"] = f"pak parse failed: {e}"
        return result
    result["packets"] = len(packets)
    best, skipped_lzd = _locate_mapdata(packets)
    if best is None:
        result["note"] = ("no MapData packet matched"
                          + (" (LZW packet(s) skipped — use "
                             "mission_terrain_analyzer.py)" if skipped_lzd else ""))
        return result
    _idx, side, blocks = best
    result["side"] = side
    result["verts"] = side * side
    result["world_size_wu"] = side * WORLD_UNITS_PER_VERTEX
    lo, hi = float("inf"), float("-inf")
    for r in range(side * side):
        (e,) = struct.unpack_from("<f", blocks, r * PCV_SIZE + OFF_ELEV)
        if e < lo:
            lo = e
        if e > hi:
            hi = e
    result["elev_min"] = round(lo, 3)
    result["elev_max"] = round(hi, 3)
    return result


def read_water_elevation(fit_path: Path) -> Optional[float]:
    """[Water] f Elevation from the mission .fit; None if file absent."""
    fit_path = Path(fit_path)
    if not fit_path.is_file():
        return None
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


# ---------------------------------------------------------------------------
# Facts assembly
# ---------------------------------------------------------------------------

def _file_info(p: Path) -> Optional[dict]:
    try:
        st = p.stat()
    except OSError:
        return None
    return {
        "bytes": st.st_size,
        "mtime_utc": datetime.fromtimestamp(st.st_mtime, tz=timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
    }


def _lane_scan(lane_path: Path, stem: str) -> dict:
    md = lane_path / "data" / "missions"
    pak = md / f"{stem}.pak"
    fit = md / f"{stem}.fit"
    beauty = md / f"{stem}.beauty"
    sidecars = {}
    for name in SIDECAR_FILES:
        info = _file_info(beauty / name)
        if info:
            sidecars[name] = info
    return {
        "lane_exists": lane_path.is_dir(),
        "pak": _file_info(pak),
        "fit": _file_info(fit),
        "beauty_dir": beauty.is_dir(),
        "sidecars_present": sorted(sidecars),
        "sidecars": sidecars,
    }


def mission_facts(repo_root: Path, stem: str, pak_path: str = "") -> dict:
    """One-call mission ground truth. See module docstring."""
    stem = (stem or "").strip()
    if not stem:
        return {"error": "mission stem required (e.g. 'mc2_01')"}
    repo_root = Path(repo_root)

    lanes = {}
    parse_candidates: list[Path] = []
    if pak_path:
        parse_candidates.append(Path(pak_path))
    for name, folder in deploy_lanes(repo_root):
        info = _lane_scan(Path(folder), stem)
        lanes[name] = {"path": folder, **info}
        if info["pak"]:
            parse_candidates.append(Path(folder) / "data" / "missions" / f"{stem}.pak")
    # worktree-local pak (rare, but authoring lanes keep them)
    wt_pak = repo_root / "data" / "missions" / f"{stem}.pak"
    if wt_pak.is_file():
        parse_candidates.append(wt_pak)
        lanes["worktree"] = {"path": str(repo_root),
                             **_lane_scan(repo_root, stem)}

    geometry = None
    water = None
    for cand in parse_candidates:
        if not cand.is_file():
            continue
        geometry = parse_pak(cand)
        water = read_water_elevation(cand.with_suffix(".fit"))
        break

    return {
        "stem": stem,
        "geometry": geometry or {"note": f"no {stem}.pak found in any lane "
                                         "(mission may live inside an FST archive)"},
        "water_elevation": water,
        "world_units_per_vertex": WORLD_UNITS_PER_VERTEX,
        "lanes": lanes,
        "sidecar_convention": "data/missions/<stem>.beauty/"
                              + "{" + ",".join(SIDECAR_FILES) + "}",
    }
