#!/usr/bin/env python3
"""SIDECAR-FORMAT-1 (Arc B / B2): reversible, audited stock-mission terrain patch.

A "beauty sidecar" is a NON-DESTRUCTIVE patch package for a stock mission .pak.
It never edits the stock file: it stores a per-vertex elevation DELTA + the source
.pak's hash, and `apply` writes a NEW patched .pak. Refuses to apply to a .pak
whose hash does not match (wrong / already-modified terrain).

Safety contract (the reason B2 ships before B3 height edits):
  - Only vertices with a NON-ZERO delta are touched (elevation field + that cell's
    Z-up normal). Every other byte — including stock normals, textureData, terrain
    type, lighting, flags — is copied verbatim.
  - => A ZERO delta apply is BYTE-IDENTICAL to the source .pak (verify-roundtrip).
  - Packet-0 byte range is located with masked offsets (type<<29 | offset), so a
    non-RAW packet 0 is detected and rejected rather than silently corrupted.

Format references: see mission_terrain_analyzer.py (PacketFile / PostcompVertex /
worldToTile). Stock vertexNormal is Z-up (nz~1); B3 recompute uses that convention.

Package layout  <out>/<mission>.beauty/ :
    sidecar.json        metadata + baseHash (sha256 of source .pak)
    height_delta.r32    float32 [side*side] row-major world-unit elevation deltas
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, PCV_SIZE, OFFSET_MASK, TYPE_SHIFT,
    PACKET_MAGIC, ST_RAW, OFF_ELEV, WORLD_UNITS_PER_VERTEX,
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    h.update(Path(path).read_bytes())
    return h.hexdigest()


def _locate_packet0_range(raw: bytes) -> tuple[int, int, int]:
    """Return (start, end, storage_type) for packet 0 using MASKED offsets."""
    magic = struct.unpack_from('<I', raw, 0)[0]
    if magic != PACKET_MAGIC:
        raise ValueError(f"not a PacketFile (magic={magic:#x})")
    fpo = struct.unpack_from('<I', raw, 4)[0]
    n_pkt = fpo // 4 - 2
    if n_pkt < 2:
        raise ValueError("PacketFile needs >=2 packets")
    e0 = struct.unpack_from('<I', raw, 8)[0]
    e1 = struct.unpack_from('<I', raw, 12)[0]
    t0 = (e0 >> TYPE_SHIFT) & 0x7
    start = e0 & OFFSET_MASK
    end = e1 & OFFSET_MASK
    return start, end, t0


def _z_up_normal(elev: np.ndarray, r: int, c: int) -> tuple[float, float, float]:
    """Stock-convention Z-up surface normal at cell (r,c) from world-unit
    elevations. col = +east (x), row increases = +south (so north = -row).
    n = normalize(-dE/dEast, -dE/dNorth, 1)."""
    N = elev.shape[0]
    hl = elev[r, max(c - 1, 0)]
    hr = elev[r, min(c + 1, N - 1)]
    hu = elev[max(r - 1, 0), c]   # row-1 = north
    hd = elev[min(r + 1, N - 1), c]
    dE_east = (hr - hl) / (2.0 * WORLD_UNITS_PER_VERTEX)
    dE_north = -(hd - hu) / (2.0 * WORLD_UNITS_PER_VERTEX)
    nx, ny, nz = -dE_east, -dE_north, 1.0
    L = (nx * nx + ny * ny + nz * nz) ** 0.5
    return nx / L, ny / L, nz / L


# --- sidecar package I/O -----------------------------------------------------
def write_sidecar(out_dir: Path, mission: str, pak_path: Path,
                  delta: np.ndarray, note: str = "",
                  provenance: dict | None = None) -> Path:
    """Write a beauty sidecar package.

    `provenance` (DATA-PROVENANCE-CLOSE-1) records WHICH mission data the cook
    read and how that was proven -- `source_pak` alone is a machine path, which
    says nothing about whether those bytes are the ones the engine ships. The
    two sidecars committed before this lane (mc2_01, mc2_24) recorded
    `A:\\Games\\Carver5-feasibility\\...`, and their `baseHash` was that tree's
    pak. Since `apply_sidecar` REFUSES on a baseHash mismatch, they could not
    be applied to the shipped pak at all -- a functional defect, not just a
    provenance note. Omitted -> the key is absent, so old sidecars still read.
    """
    side = delta.shape[0]
    if delta.shape != (side, side):
        raise ValueError(f"delta must be square, got {delta.shape}")
    beauty = out_dir / f"{mission}.beauty"
    beauty.mkdir(parents=True, exist_ok=True)
    delta.astype("<f4").tofile(beauty / "height_delta.r32")
    nz = int(np.count_nonzero(delta))
    meta = {
        "format": "mc2-beauty-sidecar/1",
        "mission": mission,
        "source_pak": str(pak_path),
        "baseHash": sha256_file(pak_path),
        "grid_side": side,
        "world_units_per_vertex": WORLD_UNITS_PER_VERTEX,
        "delta_file": "height_delta.r32",
        "delta_dtype": "<f4",
        "changed_cells": nz,
        "max_abs_delta_wu": float(np.abs(delta).max()) if delta.size else 0.0,
        "note": note,
    }
    if provenance:
        meta["provenance"] = provenance
    (beauty / "sidecar.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return beauty


def read_sidecar(beauty: Path) -> tuple[dict, np.ndarray]:
    meta = json.loads((beauty / "sidecar.json").read_text(encoding="utf-8"))
    side = meta["grid_side"]
    delta = np.fromfile(beauty / meta["delta_file"], dtype=meta["delta_dtype"]).reshape(side, side)
    return meta, delta


# --- apply -------------------------------------------------------------------
def apply_sidecar(pak_path: Path, beauty: Path, out_pak: Path,
                  allow_hash_mismatch: bool = False) -> dict:
    meta, delta = read_sidecar(beauty)
    cur_hash = sha256_file(pak_path)
    if cur_hash != meta["baseHash"] and not allow_hash_mismatch:
        raise ValueError(
            f"baseHash mismatch: sidecar expects {meta['baseHash'][:12]}..., "
            f"target .pak is {cur_hash[:12]}... (refusing — wrong/modified terrain). "
            "Pass allow_hash_mismatch to override.")

    raw = bytearray(Path(pak_path).read_bytes())
    start, end, t0 = _locate_packet0_range(raw)
    if t0 != ST_RAW:
        raise ValueError(f"packet 0 storage type {t0} != RAW; in-place elevation "
                         "patch only supported on RAW packet 0")
    pkt = bytearray(raw[start:end])
    n = len(pkt) // PCV_SIZE
    side = int(round(n ** 0.5))
    if side * side != n or side != meta["grid_side"]:
        raise ValueError(f"packet0 grid {side} != sidecar grid {meta['grid_side']}")

    # Read current elevations (row-major).
    arr = np.frombuffer(bytes(pkt), dtype=np.uint8).reshape(n, PCV_SIZE)
    elev = arr[:, OFF_ELEV:OFF_ELEV + 4].copy().view(np.float32).reshape(side, side).astype(np.float64)
    new_elev = elev + delta
    changed = np.argwhere(delta != 0.0)

    # Patch ONLY changed cells: elevation@12 + normal@0 (Z-up, from new elevations).
    for (r, c) in changed:
        flat = r * side + c
        base = flat * PCV_SIZE
        struct.pack_into('<f', pkt, base + OFF_ELEV, float(new_elev[r, c]))
        nx, ny, nz = _z_up_normal(new_elev, r, c)
        struct.pack_into('<3f', pkt, base + 0, nx, ny, nz)

    raw[start:end] = pkt
    Path(out_pak).write_bytes(raw)
    return {
        "mission": meta["mission"],
        "grid_side": side,
        "changed_cells": int(len(changed)),
        "max_abs_delta_wu": float(np.abs(delta).max()) if delta.size else 0.0,
        "out_pak": str(out_pak),
        "baseHash_ok": cur_hash == meta["baseHash"],
        "byte_identical": raw == bytearray(Path(pak_path).read_bytes()),
    }


# --- CLI ---------------------------------------------------------------------
def _cmd_verify_roundtrip(args) -> int:
    """Zero-delta apply must be byte-identical to the source .pak."""
    pak = Path(args.pak)
    pk = read_packets(pak)
    md = locate_mapdata(pk)
    if md is None:
        print(f"[sidecar] no MapData packet in {pak}", file=sys.stderr)
        return 2
    _, side, _ = md
    import tempfile
    tmp = Path(tempfile.mkdtemp())
    write_sidecar(tmp, "roundtrip", pak, np.zeros((side, side), dtype=np.float32))
    out = tmp / "roundtrip.pak"
    stats = apply_sidecar(pak, tmp / "roundtrip.beauty", out)
    ident = sha256_file(pak) == sha256_file(out)
    print(f"[sidecar] verify-roundtrip {pak.name}: grid={side}^2 "
          f"changed={stats['changed_cells']} byte_identical={ident}")
    return 0 if ident else 1


def _cmd_apply(args) -> int:
    stats = apply_sidecar(Path(args.pak), Path(args.sidecar), Path(args.out),
                          allow_hash_mismatch=args.allow_hash_mismatch)
    print(f"[sidecar] apply: {stats['mission']} changed={stats['changed_cells']} "
          f"max|d|={stats['max_abs_delta_wu']:.2f}wu baseHash_ok={stats['baseHash_ok']} "
          f"-> {stats['out_pak']}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="MC2 beauty sidecar (reversible terrain patch)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    rt = sub.add_parser("verify-roundtrip", help="zero-delta apply == source (safety gate)")
    rt.add_argument("pak")
    rt.set_defaults(fn=_cmd_verify_roundtrip)
    ap2 = sub.add_parser("apply", help="apply a sidecar to a stock .pak -> new .pak")
    ap2.add_argument("pak"); ap2.add_argument("sidecar"); ap2.add_argument("out")
    ap2.add_argument("--allow-hash-mismatch", action="store_true")
    ap2.set_defaults(fn=_cmd_apply)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
