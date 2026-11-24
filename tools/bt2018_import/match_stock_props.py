#!/usr/bin/env py -3
"""Rank BT2018 library assets for every stock MC2 prop key; write an owner-editable mapping.

  py -3 tools/bt2018_import/match_stock_props.py --library <lib>/catalog.json --stock stock_props.json --out mapping.json

stock_props.json: {"<appearance>": {"size_wu": [x, y_up, z], "uses": N, "volume_wu3", "solidity", "silhouette": [288 bits] | null}}
Cost = category compatibility + log size ratios (height, both footprint axes, solid volume) + silhouette
Hamming distance + a flat penalty for assets without resolved materials. Rejected outright: fragments
(a destruction shell, or a building-like asset whose column-filled solid is under SOLID_MIN of its bbox)
and candidates whose solid volume is outside VOLUME_RATIO of the stock prop's (an antenna for a tower).
Vegetation stock keys are skipped.
The mapping is the cook input: edit "pick" per stock key to override the auto choice.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prop_classify as pc  # noqa: E402

# Cost of using a candidate category for a stock category (0 = same). Missing pairs are not allowed.
COMPAT = {
    "building": {"building": 0, "military": 0.3, "industrial": 0.5},
    "military": {"military": 0, "building": 0.3, "industrial": 0.4, "tower": 0.6},
    "industrial": {"industrial": 0, "building": 0.5, "military": 0.4},
    "tower": {"tower": 0, "industrial": 0.5, "military": 0.5},
    "wall_fence": {"wall_fence": 0},
    "bridge": {"bridge": 0, "wall_fence": 0.8},
    "rock": {"rock": 0, "cliff": 0.4},
    "cliff": {"cliff": 0, "rock": 0.4},
    "debris": {"debris": 0, "rock": 0.6, "industrial": 0.8},
    "vehicle": {"vehicle": 0, "debris": 0.5},
}


SOLID_MIN = 0.22                 # column-filled volume / bbox volume below this is a partial shell
VOLUME_RATIO = (0.3, 3.5)        # allowed candidate / stock solid volume
_SOLID_CATS = ("building", "military", "industrial")


def _lr(a, b):
    return abs(math.log(max(a, 1e-3) / max(b, 1e-3)))


def cost(stock, cand, stock_cat):
    compat = COMPAT.get(stock_cat, {}).get(cand["category"])
    if compat is None:
        return None
    if cand.get("shell") or (cand["category"] in _SOLID_CATS and cand.get("solidity", 1.0) < SOLID_MIN):
        return None
    sx, sy, sz = stock["size_wu"]
    fa, fb = sorted((sx, sz)), sorted(cand["footprint_wu"])
    c = compat + _lr(cand["height_wu"], sy) + 0.75 * (_lr(fb[0], fa[0]) + _lr(fb[1], fa[1]))
    if stock.get("volume_wu3") and cand.get("volume_wu3") is not None:
        r = max(cand["volume_wu3"], 1e-3) / max(stock["volume_wu3"], 1e-3)
        if not VOLUME_RATIO[0] <= r <= VOLUME_RATIO[1]:
            return None
        c += 0.5 * abs(math.log(r))
    sil = stock.get("silhouette")
    if sil and cand.get("silhouette"):
        c += 0.8 * sum(a != b for a, b in zip(sil, cand["silhouette"])) / len(sil)
    if not cand.get("materials_resolved"):
        c += 0.25
    return c


def match(stock_props, assets, top=4):
    """-> {stock: {"category", "pick", "alternatives", "cost", "uses"}}; pick None when no candidate exists."""
    out = {}
    for name, info in sorted(stock_props.items()):
        cat = pc.stock_category(name, tuple(info["size_wu"]))
        if cat == pc.EXCLUDED:
            continue
        ranked = []
        for cand in assets:
            c = cost(info, cand, cat)
            if c is not None:
                ranked.append((c, cand["id"]))
        ranked.sort()
        picks = [i for _, i in ranked[:top]]
        out[name] = {"key": f"staticprop:{name}", "category": cat, "uses": info.get("uses", 0),
                     "pick": picks[0] if picks else None, "alternatives": picks[1:],
                     "cost": round(ranked[0][0], 3) if ranked else None}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--library", type=Path, required=True)
    ap.add_argument("--stock", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    assets = [a for a in json.loads(args.library.read_text(encoding="utf-8"))["assets"] if not pc.is_fragment(a["family"])]
    stock = json.loads(args.stock.read_text(encoding="utf-8"))
    m = match(stock, assets)
    args.out.write_text(json.dumps({"schema": "bt2018-prop-mapping/1", "map": m}, indent=1), encoding="utf-8")
    got = sum(1 for v in m.values() if v["pick"])
    print(f"stock keys {len(m)}  matched {got}  unmatched {[k for k, v in m.items() if not v['pick']]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
