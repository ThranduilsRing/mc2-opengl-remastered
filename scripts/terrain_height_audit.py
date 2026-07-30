#!/usr/bin/env python3
"""TERRAIN-HEIGHT-AUDIT-SCRIPT-1: empirical heightfield audit.

Reads MC2 mission .pak files, locates the MapData heightfield packet by
the realVerticesMapSide^2 * sizeof(PostcompVertex) size signature, and
reports per-mission metrics that calibrate any future visual-resample /
displacement work (slice 5 plan).

Read-only. No engine binary required. No gameplay or render mutation.

Background — referenced from docs/terrain-height-audit.md (slice 3 recon):
  - PacketFile format: mclib/packet.h, mclib/packet.cpp.
    Header: uint32 magic 0xFEEDFACE, uint32 firstPacketOffset.
    numPackets = firstPacketOffset/4 - 2.
    Each packet entry (uint32): top 3 bits storage type, low 29 bits offset.
    Storage types: RAW=0, FWF=1, LZD=2, HF=3, ZLIB=4, NUL=7.
    LZD and ZLIB payloads start with uint32 unpacked-size, body at +4.
  - PostcompVertex (mclib/vertex.h:32-60): 32 bytes total (16-byte aligned
    per source-comment "Additional Storage to pull into 16 Byte Alignment"):
      offset  0  Vector3D vertexNormal   (3 floats, packed, no Stuff padding)
      offset 12  float    elevation      <-- the field we want
      offset 16  uint32   textureData    (overlay TXM hi-word, base TXM lo-word)
      offset 20  uint32   localRGBLight  (aRGB)
      offset 24  uint32   terrainType
      offset 28  4× BYTE  selected/water/shadow/highlighted
  - MapData packet size invariant (mclib/terrain.cpp:322):
      realVerticesMapSide = sqrt(packetSize / sizeof(PostcompVertex))
      realVerticesMapSide ∈ {60, 80, 100, 120}
  - World scale (mclib/terrain.cpp:101-107): worldUnitsPerVertex = 128.0.
"""
from __future__ import annotations

import argparse
import datetime as dt
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# STOCK-DATA-TRUTH-1: DEFAULT_MISSIONS_DIR used to be
# A:/Games/Carver5-feasibility/data/missions, which differs from the shipped
# mission.fst on ALL 24 campaign maps -- so every audit report this script
# produced by default graded terrain the engine never loads. There is no
# baked-in default any more; the resolver proves whatever it uses.
# scripts/terrain_grounding_audit.py imports the helpers below, so fixing them
# here fixes both.
sys.path.insert(0, str(ROOT / "tools" / "mc2data"))
from stock_missions import (  # noqa: E402
    StockDataError, add_missions_dir_arg, resolve_from_args,
)

DEFAULT_TIER1 = ["mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24"]
DEFAULT_OUT_DIR = ROOT / "tests" / "terrain"

# --- PacketFile format constants (mclib/packet.h) -----------------------------

PACKET_MAGIC = 0xFEEDFACE
TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1

ST_RAW  = 0x0
ST_FWF  = 0x1
ST_LZD  = 0x2
ST_HF   = 0x3
ST_ZLIB = 0x4
ST_NUL  = 0x7

STORAGE_NAME = {
    ST_RAW: "RAW", ST_FWF: "FWF", ST_LZD: "LZD",
    ST_HF: "HF",   ST_ZLIB: "ZLIB", ST_NUL: "NUL",
}

# --- PostcompVertex constants (mclib/vertex.h) --------------------------------

POSTCOMP_VERTEX_SIZE = 32  # Verified: 320000-byte mc2_01 packet 0 → 100×100 grid.
ELEV_OFFSET = 12
WORLD_UNITS_PER_VERTEX = 128.0
VALID_GRID_SIDES = (60, 80, 100, 120)

# --- LZW decoder (port of mclib/lzdecomp.cpp; mirrors dev/squelch/fix_squelch.py).

def lzw_decompress(src: bytes) -> bytes:
    HASH_CLEAR, HASH_EOF, HASH_FREE = 256, 257, 258
    BASE_BITS = 9
    MAX_DICT = 4096
    hash_chain  = [0] * MAX_DICT
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
    old_chain = 0
    old_suffix = 0

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
            old_chain = lit
            old_suffix = lit
            continue
        chain = code
        if chain >= free_index:
            hash_chain[chain]  = old_chain
            hash_suffix[chain] = old_suffix & 0xFF
        stack = []
        cur = chain
        while cur >= 256:
            stack.append(hash_suffix[cur])
            cur = hash_chain[cur]
        stack.append(cur & 0xFF)
        terminal = cur & 0xFF
        old_suffix = terminal
        for c in reversed(stack):
            dest.append(c)
        if free_index < MAX_DICT:
            hash_chain[free_index]  = old_chain
            hash_suffix[free_index] = old_suffix
        free_index += 1
        old_chain = chain
        if free_index >= max_index and code_width < 12:
            code_width += 1
            max_index <<= 1
    return bytes(dest)


# --- PacketFile reader --------------------------------------------------------

def read_packets(path: Path):
    data = path.read_bytes()
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic != PACKET_MAGIC:
        raise ValueError(f"{path.name}: expected magic 0xFEEDFACE, got {magic:#x} "
                         "(checksum-armed PacketFile not supported by this audit)")
    first_off = struct.unpack_from('<I', data, 4)[0]
    num = (first_off // 4) - 2
    entries = [struct.unpack_from('<I', data, 8 + 4 * i)[0] for i in range(num)]
    out = []
    for i in range(num):
        t = (entries[i] >> TYPE_SHIFT) & 0x7
        off = entries[i] & OFFSET_MASK
        end = (entries[i+1] & OFFSET_MASK) if i + 1 < num else len(data)
        out.append((i, t, data[off:end]))
    return out


def decode_packet(packet_index: int, storage_type: int, payload: bytes) -> bytes | None:
    """Decode a single packet payload to raw bytes. Returns None on unsupported type."""
    if storage_type == ST_RAW:
        return payload
    if storage_type == ST_LZD:
        if len(payload) < 4:
            return None
        try:
            return lzw_decompress(payload[4:])
        except Exception:
            return None
    if storage_type == ST_ZLIB:
        if len(payload) < 4:
            return None
        try:
            return zlib.decompress(payload[4:])
        except zlib.error:
            return None
    if storage_type == ST_NUL:
        return b""
    # FWF / HF unhandled.
    return None


# --- MapData locator ----------------------------------------------------------

def _is_valid_mapdata_size(decoded_len: int) -> int | None:
    """Return realVerticesMapSide if decoded_len matches an MC2 heightfield, else None."""
    if decoded_len <= 0 or decoded_len % POSTCOMP_VERTEX_SIZE != 0:
        return None
    cells = decoded_len // POSTCOMP_VERTEX_SIZE
    side_f = math.isqrt(cells)
    if side_f * side_f != cells:
        return None
    if side_f in VALID_GRID_SIDES:
        return side_f
    return None


def locate_mapdata(packets):
    """Scan all packets; return list of (idx, type, side, decoded_bytes) for matches."""
    candidates = []
    for idx, t, payload in packets:
        decoded = decode_packet(idx, t, payload)
        if decoded is None:
            continue
        side = _is_valid_mapdata_size(len(decoded))
        if side is not None:
            candidates.append((idx, t, side, decoded))
    return candidates


# --- Metrics ------------------------------------------------------------------

def compute_metrics(side: int, blocks: bytes) -> dict:
    """Compute heightfield metrics from a side x side PostcompVertex array."""
    # Extract elevation column. struct iter is slower than reshaping but
    # avoids the numpy dependency.
    elevations = [
        struct.unpack_from('<f', blocks, i * POSTCOMP_VERTEX_SIZE + ELEV_OFFSET)[0]
        for i in range(side * side)
    ]
    n = len(elevations)
    emin = min(elevations)
    emax = max(elevations)
    mean = sum(elevations) / n
    var = sum((e - mean) ** 2 for e in elevations) / n
    stdev = math.sqrt(var)

    # 2D access by row/col.
    def H(r, c): return elevations[r * side + c]

    # Adjacent deltas across all 4 axial neighbours within bounds.
    deltas = []
    slopes_deg = []
    for r in range(side):
        for c in range(side):
            h = H(r, c)
            if c + 1 < side:
                d = abs(H(r, c+1) - h); deltas.append(d)
                slopes_deg.append(math.degrees(math.atan(d / WORLD_UNITS_PER_VERTEX)))
            if r + 1 < side:
                d = abs(H(r+1, c) - h); deltas.append(d)
                slopes_deg.append(math.degrees(math.atan(d / WORLD_UNITS_PER_VERTEX)))

    deltas.sort()
    slopes_deg.sort()

    def pct(series, p):
        if not series:
            return 0.0
        k = max(0, min(len(series) - 1, int(round(p * (len(series) - 1)))))
        return series[k]

    # "Blockiness" metric: variance of the discrete second-difference along
    # rows + columns. Low value = smooth surface, high value = stair-step
    # quantization on the source grid.
    sd_sq_sum = 0.0
    sd_n = 0
    for r in range(side):
        for c in range(1, side - 1):
            sd = H(r, c+1) - 2.0 * H(r, c) + H(r, c-1)
            sd_sq_sum += sd * sd
            sd_n += 1
    for c in range(side):
        for r in range(1, side - 1):
            sd = H(r+1, c) - 2.0 * H(r, c) + H(r-1, c)
            sd_sq_sum += sd * sd
            sd_n += 1
    sd_var = sd_sq_sum / max(1, sd_n)

    map_extent_wu = (side - 1) * WORLD_UNITS_PER_VERTEX
    raw_height_bytes = n * POSTCOMP_VERTEX_SIZE

    # Memory estimates if we upload the heightfield as a render-side R32F
    # texture (slice 5 plan). 1x = source resolution; 2x / 4x / 8x =
    # candidate resampled sizes.
    r32f_bytes = lambda scale: (side * scale) ** 2 * 4

    return {
        "grid_side": side,
        "samples": n,
        "world_units_per_vertex": WORLD_UNITS_PER_VERTEX,
        "map_extent_wu": map_extent_wu,
        "elevation": {
            "min": emin, "max": emax, "range": emax - emin,
            "mean": mean, "stdev": stdev,
        },
        "adjacent_delta_wu": {
            "n": len(deltas),
            "min": deltas[0] if deltas else 0.0,
            "p50": pct(deltas, 0.50),
            "p90": pct(deltas, 0.90),
            "p99": pct(deltas, 0.99),
            "max": deltas[-1] if deltas else 0.0,
        },
        "slope_deg": {
            "n": len(slopes_deg),
            "p50": pct(slopes_deg, 0.50),
            "p90": pct(slopes_deg, 0.90),
            "p99": pct(slopes_deg, 0.99),
            "max": slopes_deg[-1] if slopes_deg else 0.0,
        },
        "blockiness_second_diff_var": sd_var,
        "raw_heightfield_bytes": raw_height_bytes,
        "r32f_estimate_bytes": {
            "1x": r32f_bytes(1),
            "2x": r32f_bytes(2),
            "4x": r32f_bytes(4),
            "8x": r32f_bytes(8),
        },
    }


# --- Report ------------------------------------------------------------------

def format_bytes(n: int) -> str:
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.2f} MB"
    if n >= 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n} B"


def render_mission_section(name: str, path: Path, result: dict) -> str:
    if result.get("error"):
        return (f"### {name}\n\n"
                f"- Path: `{path}`\n"
                f"- **ERROR:** {result['error']}\n\n")
    m = result["metrics"]
    pkt = result["packet"]
    e = m["elevation"]
    d = m["adjacent_delta_wu"]
    s = m["slope_deg"]
    r32 = m["r32f_estimate_bytes"]

    out = []
    out.append(f"### {name}\n")
    out.append(f"- Path: `{path}`")
    out.append(f"- MapData packet: index {pkt['index']} "
               f"(storage {STORAGE_NAME.get(pkt['storage'], pkt['storage'])})")
    out.append(f"- Grid: {m['grid_side']}×{m['grid_side']} = {m['samples']} samples")
    out.append(f"- World scale: {m['world_units_per_vertex']:.1f} wu/vertex; "
               f"map extent ≈ {m['map_extent_wu']:.0f} wu/side")
    out.append(f"- Raw heightfield bytes: {format_bytes(m['raw_heightfield_bytes'])}\n")
    out.append("**Elevation (world units):**\n")
    out.append(f"| min | max | range | mean | stdev |")
    out.append(f"|---|---|---|---|---|")
    out.append(f"| {e['min']:.2f} | {e['max']:.2f} | {e['range']:.2f} | "
               f"{e['mean']:.2f} | {e['stdev']:.2f} |\n")
    out.append("**Adjacent-vertex delta (|Δh| per 128 wu step, axial only):**\n")
    out.append(f"| n | min | p50 | p90 | p99 | max |")
    out.append(f"|---|---|---|---|---|---|")
    out.append(f"| {d['n']} | {d['min']:.2f} | {d['p50']:.2f} | "
               f"{d['p90']:.2f} | {d['p99']:.2f} | {d['max']:.2f} |\n")
    out.append("**Slope (degrees, atan(Δh / 128)):**\n")
    out.append(f"| p50 | p90 | p99 | max |")
    out.append(f"|---|---|---|---|")
    out.append(f"| {s['p50']:.2f}° | {s['p90']:.2f}° | "
               f"{s['p99']:.2f}° | {s['max']:.2f}° |\n")
    out.append(f"**Blockiness (variance of 2nd-difference):** "
               f"{m['blockiness_second_diff_var']:.3f} (lower = smoother)\n")
    out.append("**Render-height texture memory estimate (R32F):**\n")
    out.append(f"| scale | size       | bytes |")
    out.append(f"|---|---|---|")
    for k in ("1x", "2x", "4x", "8x"):
        scale = int(k[:-1])
        out.append(f"| {k}    | {m['grid_side']*scale}×{m['grid_side']*scale}  "
                   f"| {format_bytes(r32[k])} |")
    out.append("")
    return "\n".join(out) + "\n"


def audit_one(mission_name: str, missions_dir: Path) -> tuple[Path, dict]:
    path = missions_dir / f"{mission_name}.pak"
    if not path.is_file():
        return path, {"error": f"file not found: {path}"}
    try:
        packets = read_packets(path)
    except Exception as ex:
        return path, {"error": f"read_packets failed: {ex}"}

    candidates = locate_mapdata(packets)
    if not candidates:
        return path, {"error": "no packet matched MapData size signature "
                               f"({VALID_GRID_SIDES})"}
    # Pick the largest matching packet — other accidental matches would be
    # smaller game-data structures.
    idx, t, side, blocks = max(candidates, key=lambda c: len(c[3]))
    metrics = compute_metrics(side, blocks)
    return path, {
        "metrics": metrics,
        "packet": {"index": idx, "storage": t, "candidates": len(candidates)},
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("missions", nargs="*", default=DEFAULT_TIER1,
                    help="Mission names (without .pak). Default: tier1.")
    add_missions_dir_arg(ap)
    ap.add_argument("--out", default=None,
                    help="Output Markdown path. Default: "
                         "tests/terrain/terrain_height_audit_<timestamp>.md")
    args = ap.parse_args()

    try:
        missions_dir = resolve_from_args(args, missions=args.missions).dir
    except StockDataError as e:
        print(f"[audit] ERROR {e}", file=sys.stderr)
        return 4

    results: list[tuple[str, Path, dict]] = []
    for name in args.missions:
        path, result = audit_one(name, missions_dir)
        results.append((name, path, result))
        if result.get("error"):
            print(f"[audit] {name}: ERROR {result['error']}", file=sys.stderr)
        else:
            m = result["metrics"]
            print(f"[audit] {name}: grid={m['grid_side']}^2 "
                  f"elev={m['elevation']['min']:.1f}..{m['elevation']['max']:.1f} "
                  f"slope_p99={m['slope_deg']['p99']:.1f}deg")

    DEFAULT_OUT_DIR.mkdir(parents=True, exist_ok=True)
    out_path = (Path(args.out) if args.out
                else DEFAULT_OUT_DIR / f"terrain_height_audit_"
                f"{dt.datetime.now().strftime('%Y%m%dT%H%M%S')}.md")

    head = []
    head.append(f"# Terrain Height Audit — empirical run\n")
    head.append(f"- Script: `scripts/terrain_height_audit.py` (TERRAIN-HEIGHT-AUDIT-SCRIPT-1)")
    head.append(f"- Run at: {dt.datetime.now().isoformat(timespec='seconds')}")
    head.append(f"- Missions dir: `{missions_dir}`")
    head.append(f"- Missions: {', '.join(args.missions)}\n")
    head.append("Read-only audit; reads only mission `.pak` files, makes no "
                "engine, gameplay, or render mutation. Format spec lives at "
                "[docs/terrain-height-audit.md](../../docs/terrain-height-audit.md).\n")

    body = []
    for name, path, result in results:
        body.append(render_mission_section(name, path, result))

    out_path.write_text("\n".join(head) + "\n" + "".join(body), encoding="utf-8")
    print(f"[audit] wrote {out_path}")

    return 0 if all(not r.get("error") for _, _, r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
