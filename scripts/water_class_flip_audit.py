#!/usr/bin/env python3
"""FABLE-META-4: does CPU-PHONG-RETIRE-1 flip the MOVEMENT MAP's water class?

WHY THIS EXISTS
---------------
`6d9360046` (OBJECT-BURIAL-AUDIT-1) proved `a75339bb0` (CPU-PHONG-RETIRE-1) is
CORRECT for object grounding: 72,153 authored object Z values agree with the
PHONG-RETIRED elevation to 1e-4 wu. That is a POINTWISE test on STATIC objects.
It says nothing about consumers that read elevation through a THRESHOLD.

There is one, it is default-ON, and it runs once per mission load:

  mclib/move.cpp:1057-1062          (GameMap build)
      land->cellToWorld(row, col, worldPos);
      long cellWaterType = TerrainRuntime::sampleWaterClass(worldPos);
      if (cellWaterType == WATER_TYPE_SHALLOW) setShallowWater(row, col, true);
      if (cellWaterType == WATER_TYPE_DEEP)    setDeepWater(row, col, true);

  mclib/terrain.cpp:4873-4882       (Terrain::getWater)
      elevation = getTerrainElevation(worldPos);       <-- the Phong term
      if (elevation < waterElevation - shallowDepth) return 2;   // DEEP
      if (elevation < waterElevation)               return 1;    // SHALLOW
      return 0;

  code/mover.cpp:4598               (ground movers: DEEP blocks)
      return (TerrainRuntime::sampleWaterClass(worldPos) < 2);
  code/missiongui.cpp:7295          (player move-order cursor validity)

So retiring the Phong term rewrites the movement map's water classification.
This measures by how much, OFFLINE -- shipped mission `.pak` heightfield +
shipped `.fit` `[Water]` block. No engine, no launch, no GL.

NOTE ON DIRECTION: the Phong term was ADDED by `4684ec0d` (2026-04-10). Before
that, terrainElevation was the plain triangle-linear surface, which is what
`a75339bb0` restores. So a flipped cell is being RESTORED to its stock movement
class, not broken. The finding is that a port-era commit had been silently
altering water blocking on 34 of 37 maps for three and a half months, and that
the burial audit cannot see it.

USAGE
-----
  py -3 scripts/water_class_flip_audit.py                 # every mc2_* map
  py -3 scripts/water_class_flip_audit.py mc2_m01 mc2_02  # named maps
  py -3 scripts/water_class_flip_audit.py --deltas mc2_m01  # |delta| stats
  py -3 scripts/water_class_flip_audit.py --selftest

Mission data is auto-resolved and byte-verified by tools/mc2data/stock_missions
(STOCK-DATA-TRUTH-1); pass --missions-dir to override.

COLUMNS
-------
  cells   total movement cells on the map (side*3 squared, MAPCELL_DIM = 3)
  eval    cells inside the +-MARGIN band around the two water thresholds; the
          rest cannot cross one (max measured |phong-plain| is 18.9 wu)
  FLIPS   cells whose water class differs between the two elevations
  d<->s   deep <-> shallow: the class that BLOCKS/UNBLOCKS ground movers
  w<->l   water <-> land
  +deep   plain says DEEP, phong did not  -> newly BLOCKED to ground movers
  -deep   phong said DEEP, plain does not -> previously-blocked water walkable

RESULT at the time of writing (37 stock maps, 34 non-zero):
  cells 4,597,200  flips 10,357  d<->s 6,568  w<->l 3,789
  +deep 1,759      -deep 5,212
  worst: mc2_m01 1797/330, mc2_02 704/66, mc2_20 667/137, mc2_17 465/160
  magnitudes among flips (mc2_m01): median 0.979 wu, p90 2.575, max 6.470;
  only 6.0% below 0.1 wu, so these are geometry, not float noise.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_ROOT / "tools" / "mc2data"))

from object_burial_audit import (  # noqa: E402
    Heightfield, load_mission, WORLD_UNITS_PER_VERTEX,
)
from stock_missions import (  # noqa: E402
    StockDataError, add_missions_dir_arg, resolve_from_args,
)

# mclib/terrain.cpp:117 -- worldUnitsPerCell = worldUnitsPerVertex / MAPCELL_DIM
MAPCELL_DIM = 3

# wu. Skip band: a vertex cell whose four corners all sit further than this from
# both thresholds cannot have a cell that crosses one. Max measured
# |phong - plain| across the corpus is 18.9 wu (a75339bb0's commit message), so
# 30 is a safe margin. Raise it, never lower it.
MARGIN = 30.0


def parse_water(fit_path: Path) -> tuple[float | None, float | None]:
    """`[Water] f Elevation` / `f ShallowDepth` -- mclib/terrain.cpp:4641-4661."""
    txt = fit_path.read_text(errors="replace")
    block = None
    elev = shallow = None
    for line in txt.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            block = s[1:-1].lower()
            continue
        if block != "water" or "=" not in s:
            continue
        lhs, rhs = s.split("=", 1)
        key = lhs.strip().split()[-1].lower()
        try:
            val = float(rhs.strip())
        except ValueError:
            continue
        if key == "elevation":
            elev = val
        elif key == "shallowdepth":
            shallow = val
    return elev, shallow


def wclass(e: float, we: float, sd: float) -> int:
    """mclib/terrain.cpp:4879-4882, verbatim."""
    if e < we - sd:
        return 2
    if e < we:
        return 1
    return 0


def _cells(hf: Heightfield):
    """Yield (x, y) for every movement cell, in the engine's own mapping.

    mclib/terrain.cpp:549-553:
      cellRowToWorldCoord[r] = worldUnitsMapSide/2 - r*worldUnitsPerCell
      cellColToWorldCoord[c] = c*worldUnitsPerCell - worldUnitsMapSide/2
    """
    wupc = WORLD_UNITS_PER_VERTEX / MAPCELL_DIM
    half = hf.world_units_map_side / 2.0
    for r in range(hf.side * MAPCELL_DIM):
        y = half - r * wupc
        for c in range(hf.side * MAPCELL_DIM):
            yield (c * wupc - half, y)


def audit(stem: str, missions_dir: Path, collect_deltas: bool = False) -> dict:
    pak = missions_dir / f"{stem}.pak"
    fit = missions_dir / f"{stem}.fit"
    if not pak.is_file() or not fit.is_file():
        return {"mission": stem, "note": "missing .pak or .fit"}
    we, sd = parse_water(fit)
    if we is None or sd is None:
        return {"mission": stem, "note": "no [Water] block"}

    hf, _ = load_mission(pak)
    side = hf.side
    wupc = WORLD_UNITS_PER_VERTEX / MAPCELL_DIM
    half = hf.world_units_map_side / 2.0
    lo, hi = we - sd - MARGIN, we + MARGIN
    E = hf.elev

    r = dict(mission=stem, side=side, water_elev=we, shallow=sd,
             total_cells=(side * MAPCELL_DIM) ** 2, evaluated=0, flips=0,
             flip_deep_shallow=0, flip_water_land=0,
             newly_deep=0, newly_notdeep=0)
    deltas: list[float] = []

    for vr in range(side - 1):
        rb, rb2 = vr * side, (vr + 1) * side
        for vc in range(side - 1):
            q = (E[rb + vc], E[rb + vc + 1], E[rb2 + vc], E[rb2 + vc + 1])
            if min(q) > hi or max(q) < lo:
                continue
            for dr in range(MAPCELL_DIM):
                y = half - (vr * MAPCELL_DIM + dr) * wupc
                for dc in range(MAPCELL_DIM):
                    x = (vc * MAPCELL_DIM + dc) * wupc - half
                    ep = hf.elevation(x, y, phong=True)
                    el = hf.elevation(x, y, phong=False)
                    r["evaluated"] += 1
                    kp, kl = wclass(ep, we, sd), wclass(el, we, sd)
                    if kp == kl:
                        continue
                    r["flips"] += 1
                    if {kp, kl} == {1, 2}:
                        r["flip_deep_shallow"] += 1
                    if (kp == 0) != (kl == 0):
                        r["flip_water_land"] += 1
                    if kl == 2 and kp != 2:
                        r["newly_deep"] += 1
                    if kp == 2 and kl != 2:
                        r["newly_notdeep"] += 1
                    if collect_deltas:
                        deltas.append(abs(ep - el))
    if collect_deltas:
        r["deltas"] = sorted(deltas)
    return r


def selftest() -> int:
    """Negative controls for the CLASSIFIER. The elevation port's own controls
    (flat / uniform-slope / curved) live in object_burial_audit.py --selftest;
    this asserts only the thresholding and the cell mapping."""
    fails = 0

    def check(label, ok, detail=""):
        nonlocal fails
        print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f"  -- {detail}" if detail else ""))
        if not ok:
            fails += 1

    print("[water-class-flip] selftest")
    # Thresholds, mclib/terrain.cpp:4879-4882: strict <, so exactly-at is drier.
    check("below (we - sd) is DEEP", wclass(9.0, 20.0, 10.0) == 2)
    check("exactly (we - sd) is SHALLOW, not deep", wclass(10.0, 20.0, 10.0) == 1)
    check("between is SHALLOW", wclass(15.0, 20.0, 10.0) == 1)
    check("exactly we is LAND, not shallow", wclass(20.0, 20.0, 10.0) == 0)
    check("above we is LAND", wclass(21.0, 20.0, 10.0) == 0)
    # shallowDepth 0 collapses the shallow band: nothing can be class 1.
    check("shallowDepth 0 -> no SHALLOW band", wclass(-1e9, 5.0, 0.0) == 2
          and wclass(4.9, 5.0, 0.0) == 2 and wclass(5.0, 5.0, 0.0) == 0)
    # Cell mapping: corners of the grid must land on the map's world extent.
    class _HF:
        side = 4
        world_units_map_side = 4 * WORLD_UNITS_PER_VERTEX
    pts = list(_cells(_HF()))
    half = _HF.world_units_map_side / 2.0
    wupc = WORLD_UNITS_PER_VERTEX / MAPCELL_DIM
    check("cell count == (side*3)^2", len(pts) == (4 * MAPCELL_DIM) ** 2,
          f"{len(pts)}")
    check("first cell is top-left (-half, +half)", pts[0] == (-half, half),
          f"{pts[0]}")
    check("row step is -worldUnitsPerCell in y",
          abs((pts[0][1] - pts[4 * MAPCELL_DIM][1]) - wupc) < 1e-6)
    # MARGIN must exceed the largest |phong - plain| a75339bb0 measured.
    check("MARGIN > 18.9 wu (a75339bb0 max)", MARGIN > 18.9, f"{MARGIN}")
    print("[water-class-flip] selftest " + ("OK" if not fails else f"{fails} FAILED"))
    return 1 if fails else 0


HDR = (f"{'mission':10s} {'side':>5s} {'wElev':>9s} {'shDep':>7s} "
       f"{'cells':>9s} {'eval':>8s} {'FLIPS':>7s} {'d<->s':>7s} "
       f"{'w<->l':>7s} {'+deep':>7s} {'-deep':>7s}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    add_missions_dir_arg(ap)
    ap.add_argument("missions", nargs="*", help="mission stems (default: mc2_*)")
    ap.add_argument("--deltas", action="store_true",
                    help="also report |phong-plain| percentiles among flips")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    try:
        res = resolve_from_args(args, missions=args.missions or None)
    except StockDataError as e:
        print(f"[water-class-flip] ERROR {e}", file=sys.stderr)
        return 2
    missions_dir = Path(res.dir)
    stems = args.missions or sorted(p.stem for p in missions_dir.glob("mc2_*.pak"))

    print(HDR)
    tot = dict(cells=0, flips=0, ds=0, wl=0, nd=0, nn=0)
    for s in stems:
        r = audit(s, missions_dir, collect_deltas=args.deltas)
        if "note" in r:
            print(f"{s:10s} {r['note']}")
            continue
        print(f"{r['mission']:10s} {r['side']:5d} {r['water_elev']:9.3f} "
              f"{r['shallow']:7.3f} {r['total_cells']:9d} {r['evaluated']:8d} "
              f"{r['flips']:7d} {r['flip_deep_shallow']:7d} "
              f"{r['flip_water_land']:7d} {r['newly_deep']:7d} "
              f"{r['newly_notdeep']:7d}")
        if args.deltas and r.get("deltas"):
            d = r["deltas"]
            n = len(d)
            def p(f):
                return d[min(n - 1, int(f * n))]
            print(f"           |delta| among flips: median={p(.50):.3f} "
                  f"p90={p(.90):.3f} max={d[-1]:.3f} wu   "
                  f"frac<0.1wu={sum(1 for v in d if v < 0.1) / n:.3f}")
        tot['cells'] += r['total_cells']; tot['flips'] += r['flips']
        tot['ds'] += r['flip_deep_shallow']; tot['wl'] += r['flip_water_land']
        tot['nd'] += r['newly_deep']; tot['nn'] += r['newly_notdeep']
        sys.stdout.flush()
    print("-" * len(HDR))
    print(f"TOTAL cells={tot['cells']} flips={tot['flips']} "
          f"deep<->shallow={tot['ds']} water<->land={tot['wl']} "
          f"newly_deep={tot['nd']} newly_not_deep={tot['nn']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
