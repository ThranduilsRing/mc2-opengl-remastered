#!/usr/bin/env python3
"""predict_cone_pools.py -- CONE-SPILL-POLICY-1 offline pool-policy validator.

Predicts, for every building/beacon spotlight fixture a mission places, whether
the new default pool policy (MC2_CONE_POOL_POLICY=1: upward-beam rejection +
extended-reach beam/terrain intersection) emits a ground pool and where --
WITHOUT launching the game.

Reconstruction route (mirrors the engine):
  * mission .pak            -> MapData heightfield + terrain-objects packet
                               (objType, x, y, z, rot) -- same packet layout as
                               tools/terrain_beautify/mission_terrain_analyzer.py
  * objects/object2.pak     -> packet[objType] is the object FIT; read its
                               AppearanceName
  * tgl/<appearance>.ase    -> GEOMOBJECT nodes named SpotLight_* (tgl.cpp:388
                               S_strnicmp match); node pivot + node-local cone
                               vertices reproduce mc2_cone_glow::localConeAxis
                               (centroid direction = beam axis, max projection
                               = authored coneLen)
  * MAX->MC2 swizzle        -> mc2 = (-max.x, max.z, max.y) (cone_glow.cpp:4)
  * building yaw            -> rotation about MC2 +z by the placement 'rot'

Policies compared per fixture:
  OLD (legacy, MC2_CONE_POOL_POLICY=0): pool ALWAYS emitted at
      apex + aim * min(coneLen, 200), z snapped to terrain.
  NEW (policy 1): aim.z > 0.15 -> NO pool (sky beam);
      else ray-march the heightfield over reach = K * coneLen; first crossing
      -> pool AT the intersection; no crossing (or beam leaves the map) -> no
      pool.

Acceptance (CONE-SPILL-POLICY-1): mc2_10 non-upward tower fixtures -> KEPT;
mc2_23 straight-up beacons -> 0 pools. Tune --k until both hold.

Usage:
  py -3 scripts/predict_cone_pools.py --missions mc2_10 mc2_23 --k 4
  py -3 scripts/predict_cone_pools.py --missions mc2_10 --k 2 --verbose

Read-only; pure stdlib.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "mc2data"))
from stock_missions import add_missions_dir_arg, resolve_from_args  # noqa: E402

# --- PacketFile (mclib/packet.h) --------------------------------------------
PACKET_MAGIC = 0xFEEDFACE
TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1
ST_RAW, ST_FWF, ST_LZD, ST_HF, ST_ZLIB, ST_NUL = 0, 1, 2, 3, 4, 7
PCV_SIZE = 32
OFF_ELEV = 12
WU_PER_VERT = 128.0
VALID_SIDES = (60, 80, 100, 120)
OBJREC_SIZE = 40  # 10*4B: objTypeNum,x,y,z,rot,dmg,team,parent,pad,pad
                  # (matches tools/terrain_beautify/mission_terrain_analyzer.py)


def lzw_decompress(src: bytes) -> bytes:
    """Port of mclib/lzdecomp.cpp (same as tools/pak_dump.py)."""
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
            old_chain = lit
            old_suffix = lit
            continue
        stack = []
        cur = code
        if cur >= free_index:  # KwKwK case
            stack.append(old_suffix)
            cur = old_chain
        while cur >= 258:
            stack.append(hash_suffix[cur])
            cur = hash_chain[cur]
        stack.append(cur & 0xFF)
        first = cur & 0xFF
        for b in reversed(stack):
            dest.append(b)
        if free_index < MAX_DICT:
            hash_chain[free_index] = old_chain
            hash_suffix[free_index] = first
            free_index += 1
            if free_index >= max_index and code_width < 12:
                code_width += 1
                max_index = 1 << code_width
        old_chain = code
        old_suffix = first
    return bytes(dest)


def read_packets(path: Path):
    data = path.read_bytes()
    magic, first = struct.unpack_from("<II", data, 0)
    if magic != PACKET_MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08X}")
    n = first // 4 - 2
    table = [struct.unpack_from("<I", data, 8 + i * 4)[0] for i in range(n)]
    out = []
    for i in range(n):
        t = table[i] >> TYPE_SHIFT
        off = table[i] & OFFSET_MASK
        nxt = len(data)
        for j in range(i + 1, n):
            if (table[j] >> TYPE_SHIFT) != ST_NUL:
                nxt = table[j] & OFFSET_MASK
                break
        out.append((i, t, b"" if t == ST_NUL else data[off:nxt]))
    return out


def decode_packet(t: int, payload: bytes):
    try:
        if t == ST_RAW or t == ST_FWF:
            return payload
        if t == ST_ZLIB:
            import zlib
            (ulen,) = struct.unpack_from("<I", payload, 0)
            return zlib.decompress(payload[4:])[:ulen]
        if t == ST_LZD:
            (ulen,) = struct.unpack_from("<I", payload, 0)
            return lzw_decompress(payload[4:])[:ulen]
    except Exception:
        return None
    return None


# --- terrain heightfield ------------------------------------------------------
class Heightfield:
    def __init__(self, side: int, elev):
        self.side = side
        self.elev = elev  # row-major side*side floats
        wu = side * WU_PER_VERT
        self.tlx = -wu / 2.0
        self.tly = wu / 2.0

    def on_map(self, x: float, y: float) -> bool:
        c = (x - self.tlx) / WU_PER_VERT
        r = (self.tly - y) / WU_PER_VERT
        return 0.0 <= c <= self.side - 1 and 0.0 <= r <= self.side - 1

    def sample(self, x: float, y: float) -> float:
        c = (x - self.tlx) / WU_PER_VERT
        r = (self.tly - y) / WU_PER_VERT
        c = min(max(c, 0.0), self.side - 1.001)
        r = min(max(r, 0.0), self.side - 1.001)
        c0, r0 = int(c), int(r)
        fc, fr = c - c0, r - r0
        e = self.elev
        s = self.side
        a = e[r0 * s + c0]
        b = e[r0 * s + c0 + 1]
        cc = e[(r0 + 1) * s + c0]
        d = e[(r0 + 1) * s + c0 + 1]
        return (a * (1 - fc) + b * fc) * (1 - fr) + (cc * (1 - fc) + d * fc) * fr


def load_mission(pak: Path):
    packets = read_packets(pak)
    hf = None
    objects = []
    for idx, t, payload in packets:
        dec = decode_packet(t, payload)
        if dec is None or len(dec) < 4:
            continue
        # MapData?
        if hf is None and len(dec) % PCV_SIZE == 0:
            cells = len(dec) // PCV_SIZE
            side = math.isqrt(cells)
            if side * side == cells and side in VALID_SIDES:
                elev = [struct.unpack_from("<f", dec, r * PCV_SIZE + OFF_ELEV)[0]
                        for r in range(cells)]
                hf = Heightfield(side, elev)
                continue
        # objects packet? count + count*20 bytes
        cnt = struct.unpack_from("<i", dec, 0)[0]
        if cnt > 0 and 4 + cnt * OBJREC_SIZE == len(dec) and not objects:
            for i in range(cnt):
                objType, x, y, z, rot = struct.unpack_from(
                    "<i4f", dec, 4 + i * OBJREC_SIZE)
                objects.append((objType, x, y, z, rot))
    return hf, objects


# --- object2 fit --------------------------------------------------------------
def appearance_name(fit_bytes: bytes):
    try:
        txt = fit_bytes.decode("latin1")
    except Exception:
        return None
    for line in txt.splitlines():
        s = line.strip()
        low = s.lower()
        if low.startswith("st appearancename"):
            if '"' in s:
                return s.split('"')[1]
    return None


# --- ASE spotlight nodes ------------------------------------------------------
def parse_ase_spotlights(ase_path: Path, verbose=False):
    """Return [(nodeName, pivotMAX(3), axisLocal(3), lenLocal)] for SpotLight_*
    GEOMOBJECT nodes -- reproducing mc2_cone_glow::localConeAxis on node-local
    vertices."""
    out = []
    txt = ase_path.read_text(errors="ignore")
    i = 0
    n = len(txt)
    while True:
        g = txt.find("*GEOMOBJECT", i)
        if g < 0:
            break
        nxt = txt.find("*GEOMOBJECT", g + 1)
        block = txt[g:nxt if nxt > 0 else n]
        i = g + 1
        # node name
        nn = block.find("*NODE_NAME")
        if nn < 0:
            continue
        try:
            name = block[nn:].split('"', 2)[1]
        except IndexError:
            continue
        if not name.lower().startswith("spotlight_"):
            continue
        # NODE_TM rows
        rows = {}
        for key in ("*TM_ROW0", "*TM_ROW1", "*TM_ROW2", "*TM_ROW3"):
            k = block.find(key)
            if k < 0:
                rows = None
                break
            vals = block[k + len(key):].split(None, 3)[:3]
            rows[key] = tuple(float(v) for v in vals)
        if not rows:
            continue
        r0, r1, r2, t = rows["*TM_ROW0"], rows["*TM_ROW1"], rows["*TM_ROW2"], rows["*TM_ROW3"]
        # vertices (ASE stores world/object-space verts; node-local =
        # (world - t) . Rinv where world = local.x*r0 + local.y*r1 + local.z*r2 + t)
        verts = []
        for line in block.splitlines():
            ls = line.strip()
            if ls.startswith("*MESH_VERTEX "):
                p = ls.split()
                if len(p) >= 5:
                    verts.append((float(p[2]), float(p[3]), float(p[4])))
        if len(verts) < 3 or len(verts) > 512:
            continue
        # invert R (3x3, rows r0,r1,r2; world = local . R)
        m = [list(r0), list(r1), list(r2)]
        det = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
               - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
        if abs(det) < 1e-12:
            continue
        inv = [[0.0] * 3 for _ in range(3)]
        for a in range(3):
            for b in range(3):
                sub = [[m[r][c] for c in range(3) if c != a]
                       for r in range(3) if r != b]
                cof = sub[0][0] * sub[1][1] - sub[0][1] * sub[1][0]
                inv[a][b] = ((-1) ** (a + b)) * cof / det
        local = []
        for wx, wy, wz in verts:
            dx, dy, dz = wx - t[0], wy - t[1], wz - t[2]
            # local = (world - t) . Rinv  (row vector times matrix)
            lx = dx * inv[0][0] + dy * inv[1][0] + dz * inv[2][0]
            ly = dx * inv[0][1] + dy * inv[1][1] + dz * inv[2][1]
            lz = dx * inv[0][2] + dy * inv[1][2] + dz * inv[2][2]
            local.append((lx, ly, lz))
        # localConeAxis: centroid direction + max projection
        cx = sum(v[0] for v in local) / len(local)
        cy = sum(v[1] for v in local) / len(local)
        cz = sum(v[2] for v in local) / len(local)
        clen = math.sqrt(cx * cx + cy * cy + cz * cz)
        if clen < 0.01:
            continue
        ax, ay, az = cx / clen, cy / clen, cz / clen
        maxp = max(v[0] * ax + v[1] * ay + v[2] * az for v in local)
        if maxp < 0.5:
            continue
        out.append((name, t, (ax, ay, az), maxp, (r0, r1, r2)))
        if verbose:
            print(f"    node {name}: axisLocal=({ax:.3f},{ay:.3f},{az:.3f}) "
                  f"len={maxp:.1f} pivotMAX={t}")
    return out


def max_to_mc2(v):
    # RAW ASE -> MC2 is the IDENTITY: tgl.cpp:1069-1076 stores
    # tgl = (-ase.x, ase.z, ase.y) ("MAX frame", y-up) and the engine's
    # MAX->MC2 swizzle (cone_glow.cpp:4) is mc2 = (-tgl.x, tgl.z, tgl.y),
    # which composes to mc2 = (ase.x, ase.y, ase.z). ASE is z-up like MC2.
    return (v[0], v[1], v[2])


def rot_z(v, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2])


def march(hf: Heightfield, apex, aim, reach: float):
    """First above->below terrain crossing along apex + aim*t, t in (0, reach].
    Returns (x,y,z) or None. Mirrors mc2ConeGroundHit (incl. off-map reject)
    but with a finer fixed step for offline accuracy."""
    steps = 512
    dt = reach / steps
    px, py, pz = apex
    if not hf.on_map(px, py):
        return None
    pf = pz - hf.sample(px, py)
    for i in range(1, steps + 1):
        t = dt * i
        cx = apex[0] + aim[0] * t
        cy = apex[1] + aim[1] * t
        cz = apex[2] + aim[2] * t
        if not hf.on_map(cx, cy):
            return None
        cf = cz - hf.sample(cx, cy)
        if pf > 0.0 and cf <= 0.0:
            # bisect
            lo, hi = t - dt, t
            for _ in range(20):
                mid = (lo + hi) / 2.0
                mx = apex[0] + aim[0] * mid
                my = apex[1] + aim[1] * mid
                mz = apex[2] + aim[2] * mid
                if mz - hf.sample(mx, my) > 0.0:
                    lo = mid
                else:
                    hi = mid
            mid = (lo + hi) / 2.0
            hx = apex[0] + aim[0] * mid
            hy = apex[1] + aim[1] * mid
            return (hx, hy, hf.sample(hx, hy))
        pf = cf
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--missions", nargs="+", default=["mc2_10", "mc2_23"])
    ap.add_argument("--srcdata", default=None,
                    help="mc2-srcdata root; default: parent of the verified stock missions dir")
    add_missions_dir_arg(ap)
    ap.add_argument("--k", type=float, default=4.0,
                    help="extended-reach multiplier (MC2_CONE_POOL_REACH)")
    ap.add_argument("--up-thresh", type=float, default=0.15)
    ap.add_argument("--apex-lift", type=float, default=0.0,
                    help="extra apex height (debug)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    src = Path(args.srcdata) if args.srcdata else resolve_from_args(args, missions=args.missions).dir.parent
    obj2 = read_packets(src / "objects" / "object2.pak")
    obj2_dec = {}

    def obj_fit(idx: int):
        if idx not in obj2_dec:
            if 0 <= idx < len(obj2):
                _, t, payload = obj2[idx]
                obj2_dec[idx] = decode_packet(t, payload)
            else:
                obj2_dec[idx] = None
        return obj2_dec[idx]

    ase_cache = {}
    grand_ok = True
    for mission in args.missions:
        pak = src / "missions" / f"{mission}.pak"
        hf, objects = load_mission(pak)
        if hf is None:
            print(f"{mission}: NO heightfield -- cannot validate")
            grand_ok = False
            continue
        print(f"\n=== {mission}  (side={hf.side}, objects={len(objects)}, "
              f"k={args.k}, up_thresh={args.up_thresh}) ===")
        rows = []
        for objType, x, y, z, rot in objects:
            fit = obj_fit(objType)
            if not fit:
                continue
            app = appearance_name(fit)
            if not app:
                continue
            ase = src / "tgl" / f"{app.lower()}.ase"
            if app.lower() not in ase_cache:
                ase_cache[app.lower()] = (
                    parse_ase_spotlights(ase, args.verbose)
                    if ase.is_file() else [])
            spots = ase_cache[app.lower()]
            if not spots:
                continue
            rotdeg = math.degrees(rot) if abs(rot) <= 2 * math.pi + 0.01 else rot
            for name, pivMAX, axisLocal, lenLocal, R in spots:
                # axis local -> MAX world (node rotation): world = local . R
                aw = tuple(axisLocal[0] * R[0][i] + axisLocal[1] * R[1][i]
                           + axisLocal[2] * R[2][i] for i in range(3))
                al = math.sqrt(sum(c * c for c in aw)) or 1.0
                aw = tuple(c / al for c in aw)
                aim = rot_z(max_to_mc2(aw), rotdeg)
                piv = rot_z(max_to_mc2(pivMAX), rotdeg)
                ground = hf.sample(x, y)
                apex = (x + piv[0], y + piv[1],
                        ground + piv[2] + args.apex_lift)
                coneLen = lenLocal
                # OLD policy: always a pool
                oldSpill = min(coneLen, 200.0)
                op = (apex[0] + aim[0] * oldSpill, apex[1] + aim[1] * oldSpill)
                old_pos = (op[0], op[1],
                           hf.sample(op[0], op[1]) if hf.on_map(*op) else float("nan"))
                # NEW policy
                if aim[2] > args.up_thresh:
                    verdict, pos = "OFF-up", None
                else:
                    hit = march(hf, apex, aim, coneLen * args.k)
                    if hit:
                        verdict, pos = "ON", hit
                    else:
                        verdict, pos = "OFF-miss", None
                rows.append((objType, app, name, aim[2], coneLen,
                             verdict, pos, old_pos))
        if not rows:
            print("  (no spotlight fixtures found)")
            continue
        hdr = (f"  {'objType':>7} {'appearance':<18} {'node':<16} "
               f"{'aim.z':>7} {'coneLen':>8} {'NEW':<9} "
               f"{'new pool pos':<28} {'OLD pool pos (always ON)':<28}")
        print(hdr)
        print("  " + "-" * (len(hdr) - 2))
        kept = up_off = miss_off = 0
        for objType, app, name, aimz, clen, verdict, pos, old in rows:
            ps = (f"({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.1f})" if pos else "-")
            olds = (f"({old[0]:.0f},{old[1]:.0f},{old[2]:.1f})"
                    if old and old[2] == old[2] else "(off-map)")
            print(f"  {objType:>7} {app:<18} {name:<16} {aimz:>7.3f} "
                  f"{clen:>8.1f} {verdict:<9} {ps:<28} {olds:<28}")
            if verdict == "ON":
                kept += 1
            elif verdict == "OFF-up":
                up_off += 1
            else:
                miss_off += 1
        print(f"  summary: fixtures={len(rows)} pools_kept={kept} "
              f"suppressed_up={up_off} suppressed_miss={miss_off}")
        # acceptance heuristics
        nonup = [r for r in rows if r[3] <= args.up_thresh]
        nonup_kept = sum(1 for r in nonup if r[5] == "ON")
        print(f"  non-upward fixtures kept: {nonup_kept}/{len(nonup)}")
    sys.exit(0 if grand_ok else 1)


if __name__ == "__main__":
    main()
