#!/usr/bin/env python3
"""predict_cement_adopt.py -- offline replica of the OVERLAY-CEMENT-ADOPT tile
verdict (gos_terrain_indirect.cpp:1826-1911), computed straight from a shipped
mission .pak. No engine launch.

Per tile (r,c) it reports one of:
  SOLID          all 4 corner vertex terrainTypes equal+cement, no overlay --
                 chunk pass renders the cement atlas (the "interior" tone).
  ADOPT          cement transition tile NOT protected: terrain_overlay.frag
                 swaps its albedo to the same cement atlas.
  SKIP-overlay   protected by artguard: carries an &Overlays id
                 (TileIsRoadArt clause 1, gos_terrain_indirect.cpp:1516).
  SKIP-noncorner protected by artguard: cement-ALPHA art with NO solid-cement
                 corner (TileIsRoadArt clauses at :1522-1529, the
                 MISSING-RUNWAY-1 class).
  .              not cement-classified at all (plain terrain).

Decision inputs, all present in the pak's PostcompVertex stream
(mclib/vertex.h: 32 bytes; textureData @16, terrainType @24):
  * corner types t0=(r,c) t1=(r,c+1) t2=(r+1,c+1) t3=(r+1,c)
    (mapdata.cpp:534-538 packing order);
  * cement type test: t==BASE_CEMENT_TYPE(10) or 13<=t<=20
    (mclib/terrtxm.h:42-44, isCementType at terrtxm.cpp:1302);
  * solid slot iff all 4 equal AND no overlay (terrtxm.cpp setTexture:1601);
    slot flags CEMENT for any cement corner, ALPHA for any non-cement corner
    (terrtxm.cpp:1615/1656+);
  * overlay present iff textureData hi-word != 0xffff (mapdata.cpp:540-545
    erases stale handles to 0xffff0000; engine also drops hi-words below
    firstOverlay -- pass --first-overlay to replicate if needed).

The engine's cemCorner() indexes g_cementLayerIndexBySlot by the VERTEX
terrainType (gos_terrain_indirect.cpp:1523-1527); that map holds solid
(cement&&!alpha) slots only (:2429-2437), so "corner is solid cement type"
is the faithful offline equivalent.

Usage:
  py -3 scripts/predict_cement_adopt.py mc2_24 [--missions-dir DIR]
      [--region r0,c0,r1,c1] [--json out.json]
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "mc2data"))

from terrain_height_audit import (  # noqa: E402
    POSTCOMP_VERTEX_SIZE, decode_packet, locate_mapdata, read_packets,
)
from stock_missions import add_missions_dir_arg, resolve_from_args  # noqa: E402

BASE_CEMENT_TYPE = 10   # mclib/terrtxm.h:42
START_CEMENT_TYPE = 13  # :43
END_CEMENT_TYPE = 20    # :44

GLYPH = {"SOLID": "#", "ADOPT": "a", "SKIP-overlay": "O",
         "SKIP-noncorner": "X", ".": "."}


def is_cement_type(t: int) -> bool:
    return t == BASE_CEMENT_TYPE or START_CEMENT_TYPE <= t <= END_CEMENT_TYPE


def load_blocks(pak: Path):
    packets = read_packets(pak)
    found = locate_mapdata(packets)
    if not found:
        raise SystemExit("no MapData packet found in %s" % pak)
    _idx, _t, side, blocks = found[0]
    return side, blocks


def tile_verdict(side, ttype, hiword, r, c, first_overlay):
    t = (ttype[r * side + c], ttype[r * side + c + 1],
         ttype[(r + 1) * side + c + 1], ttype[(r + 1) * side + c])
    hw = hiword[r * side + c]
    overlay = hw != 0xFFFF and hw >= first_overlay
    any_cem = any(is_cement_type(x) for x in t)
    solid = (t[0] == t[1] == t[2] == t[3]) and not overlay and is_cement_type(t[0])
    if solid:
        return "SOLID"
    if not (overlay or any_cem):
        return "."          # never cement-classified (terrtxm.cpp:1629-1648)
    # cement transition slot exists. TileIsRoadArt:
    if overlay:
        return "SKIP-overlay"
    # alpha iff any corner non-cement -- true here since not solid and no overlay
    # means mixed corners. Solid-cement corner test = engine cemCorner().
    solid_corner = any(is_cement_type(x) for x in t)  # corner in solid layer map
    if not solid_corner:
        return "SKIP-noncorner"
    return "ADOPT"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mission", help="mission stem, e.g. mc2_24")
    add_missions_dir_arg(ap)
    ap.add_argument("--region", help="r0,c0,r1,c1 tile window for the ASCII map")
    ap.add_argument("--first-overlay", type=int, default=0,
                    help="engine firstOverlay TXM index; hi-words below it are "
                         "treated as no-overlay (default 0 = any non-0xffff)")
    ap.add_argument("--json", help="write per-tile verdict grid JSON here")
    ap.add_argument("--tint-report", metavar="r0,c0,r1,c1",
                    help="CEMENT-EDGE-TINT-1 grading aid: census the region's "
                         "else-branch art tiles (SKIP-overlay + SKIP-noncorner "
                         "= fragments terrain_overlay.frag renders WITHOUT "
                         "albedo adoption) and print the predicted per-channel "
                         "capture ratio post/pre fix. Absolute mean RGB needs "
                         "the runtime-stitched decal texels (not in the pak), "
                         "so the gradable offline target is the RATIO: the fix "
                         "removes the vec3(0.82,0.80,0.76) multiply, so art "
                         "pixels brighten by exactly 1/that = "
                         "(1.2195, 1.2500, 1.3158); adjacent SOLID/ADOPT "
                         "cement pixels are unchanged (ratio 1.0).")
    ap.add_argument("--alpha-atlas-report", action="store_true",
                    help="CEMENT-ALPHA-LAND-1 (RUNWAY-CLOSE-1): model the "
                         "own-alpha admission. Every SKIP-noncorner tile (the "
                         "MISSING-RUNWAY-1 class: cement transition slot with "
                         "no solid-cement corner => engine bakes NO word => "
                         "bare dirt) gets a word pointing at its OWN slot's "
                         "appended alpha cell, so predicted residual dropped "
                         "tiles under MC2_CEMENT_ALPHA_ATLAS=1 is ZERO by "
                         "construction (the rescue is keyed on exactly the "
                         "drop condition, gos_terrain_indirect.cpp:1736-1745).")
    ap.add_argument("--flatlight-report", action="store_true",
                    help="RUNWAY-CLOSE-1 a1 model: read the mission .fit "
                         "lighting block and replicate Camera::getLight*( "
                         "flatIntensity=sin(LightDirPitch)) -> the "
                         "u_missionFlatLight upload (gamecam.cpp:459-465, "
                         "camera.cpp:4473). Prints the predicted runway/apron "
                         "ratio change from a full MC2_OVERLAY_ART_FLATLIGHT=0 "
                         "exemption. HONEST MODEL NOTE: the chunk cement apron "
                         "ALSO multiplies u_missionFlatLight "
                         "(terrain_lod_chunk.frag:999), so the exemption moves "
                         "overlay art AWAY from the apron by 1/flatLight, not "
                         "toward it; ratio 1.0 here would require the apron "
                         "to be full-brightness, which it is not.")
    args = ap.parse_args()

    res = resolve_from_args(args, missions=[args.mission])
    pak = Path(res.dir) / (args.mission + ".pak")
    side, blocks = load_blocks(pak)
    n = side * side
    ttype = [struct.unpack_from("<I", blocks, i * POSTCOMP_VERTEX_SIZE + 24)[0]
             for i in range(n)]
    hiword = [struct.unpack_from("<I", blocks, i * POSTCOMP_VERTEX_SIZE + 16)[0] >> 16
              for i in range(n)]

    grid = [["." for _ in range(side)] for _ in range(side)]
    counts: Counter = Counter()
    for r in range(side - 1):
        for c in range(side - 1):
            v = tile_verdict(side, ttype, hiword, r, c, args.first_overlay)
            grid[r][c] = v
            counts[v] += 1

    print("mission=%s side=%d pak=%s" % (args.mission, side, pak))
    for k in ("SOLID", "ADOPT", "SKIP-overlay", "SKIP-noncorner", "."):
        print("  %-15s %d" % (k, counts[k]))

    if args.region:
        r0, c0, r1, c1 = (int(v) for v in args.region.split(","))
        print("map [%d:%d, %d:%d]  (# solid, a adopt, O skip-overlay, "
              "X skip-noncorner, . none)" % (r0, r1, c0, c1))
        for r in range(r0, min(r1, side - 1)):
            print("  r%03d %s" % (r, "".join(GLYPH[grid[r][c]]
                                             for c in range(c0, min(c1, side - 1)))))

    if args.tint_report:
        r0, c0, r1, c1 = (int(v) for v in args.tint_report.split(","))
        reg: Counter = Counter()
        for r in range(r0, min(r1, side - 1)):
            for c in range(c0, min(c1, side - 1)):
                reg[grid[r][c]] += 1
        art = reg["SKIP-overlay"] + reg["SKIP-noncorner"]
        total = sum(reg.values())
        print("tint-report [%d:%d, %d:%d]  (CEMENT-EDGE-TINT-1)" % (r0, r1, c0, c1))
        for k in ("SOLID", "ADOPT", "SKIP-overlay", "SKIP-noncorner", "."):
            print("  %-15s %d" % (k, reg[k]))
        print("  else-branch art tiles (tint-affected): %d / %d region tiles"
              % (art, total))
        print("  predicted capture ratio post/pre on art pixels (a>0.5): "
              "R x1.2195  G x1.2500  B x1.3158   (= 1 / (0.82, 0.80, 0.76))")
        print("  predicted ratio on SOLID/ADOPT cement pixels: x1.0 (unchanged)")
        print("  NOTE: engine bit-31 coverage of SKIP-noncorner tiles needs "
              "MC2_CEMENT_ALPHA_ATLAS=1 (default OFF); tint removal covers "
              "them regardless (keyed on the adopt gate, not the word).")

    if args.alpha_atlas_report:
        # Engine analogue: [CEMENT_TRANSITION_DROP v1] dropped_no_layer.
        # Offline analogue: SKIP-noncorner (cement transition slot, no solid
        # cement corner => cemCorner() finds no donor layer => word==0).
        dropped = counts["SKIP-noncorner"]
        trans = counts["ADOPT"] + counts["SKIP-overlay"] + counts["SKIP-noncorner"]
        pct = (100.0 * dropped / trans) if trans else 0.0
        print("alpha-atlas-report (CEMENT-ALPHA-LAND-1 own-alpha admission)")
        print("  transition-class tiles:        %d" % trans)
        print("  corner-model dropped (word==0): %d  (%.1f%% of transition class)"
              % (dropped, pct))
        print("  MODEL LIMIT: the engine drops a tile when the corner TYPE maps "
              "to no SOLID (cement && !alpha) catalog SLOT; slot flags are not "
              "in the pak vertex stream, so this corner-TYPE model UNDER-detects "
              "drops (engine census mc2_10 278/320=87%, mc2_24 266/778=34% via "
              "[CEMENT_TRANSITION_DROP v1] -- that line is the authoritative "
              "BEFORE number and prints in every run, gate on or off).")
        print("  predicted dropped, gate ON:    0 BY CONSTRUCTION -- the rescue "
              "branch (gos_terrain_indirect.cpp:1736-1745) fires on exactly the "
              "drop condition: every dropped tile's OWN slot gets an appended "
              "alpha cell + bit23|bit30|bit31 word. Verify at runtime via "
              "alpha_rescued == dropped_no_layer.")

    if args.flatlight_report:
        import math, re
        fit = Path(res.dir) / (args.mission + ".fit")
        txt = fit.read_text(errors="replace")
        def key(name, default=None, conv=float):
            m = re.search(r"^\s*(?:uc|f|l|ul)\s+%s\s*=\s*([-0-9.]+)" % name,
                          txt, re.M)
            return conv(m.group(1)) if m else default
        lr, lg, lb = key("LightRed", 255), key("LightGreen", 255), key("LightBlue", 255)
        ar, ag, ab = key("AmbientRed", 0), key("AmbientGreen", 0), key("AmbientBlue", 0)
        pitch = key("LightDirPitch", 90.0)
        lz = math.sin(math.radians(pitch))
        def chan(l, a):
            if lz > 0.0:
                return min(255.0, l * lz + a) / 255.0
            return a / 255.0                        # camera.cpp:4488 ambient branch
        fl = (chan(lr, ar), chan(lg, ag), chan(lb, ab))
        print("flatlight-report (%s)" % fit.name)
        print("  LightRGB=(%g,%g,%g) AmbientRGB=(%g,%g,%g) LightDirPitch=%g "
              "-> lightDir.z=%.4f" % (lr, lg, lb, ar, ag, ab, pitch, lz))
        print("  u_missionFlatLight = (%.4f, %.4f, %.4f)" % fl)
        print("  a1 full-exemption multiplier on overlay art = 1/flatLight = "
              "(%.3f, %.3f, %.3f)" % tuple(1.0 / max(c, 1e-6) for c in fl))
        print("  predicted runway/apron ratio under MC2_OVERLAY_ART_FLATLIGHT=0:")
        print("    if apron were full-brightness (brief's model): 1.000 (converges)")
        print("    with apron * flatLight (code-true, chunk :999): %.3f/%.3f/%.3f "
              "(OVERSHOOTS by 1/flatLight)"
              % tuple(1.0 / max(c, 1e-6) for c in fl))
        print("  parity route without overshoot: MC2_CEMENT_ALPHA_ATLAS=1 "
              "(own-alpha word -> identical atlas*flatLight family)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"mission": args.mission, "side": side,
                       "counts": dict(counts), "grid": grid}, f)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
