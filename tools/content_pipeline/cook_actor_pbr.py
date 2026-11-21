#!/usr/bin/env python3
"""Stock mech/vehicle PBR normal + ORM maps cooked to sibling BC7 KTX2 with a full mip chain.

data/textures/{mech,vehicle}_pbr/<stem>_normal.png and <stem>_orm.png get <stem>_normal.ktx2 / <stem>_orm.ktx2 beside them
(BC7 UNORM, linear). The texture loader prefers a same-stem sidecar over its PNG (mclib/texture_assets.cpp), and a loose
PNG shipped by a mod still wins over the base sidecar. Albedo stays PNG (the CPU repaint reads it).
Normals use mip_chain("normal") (renormalised), ORM mip_chain("orm") (linear box). Pins: data/kit/actor-pbr-pins.json.

  py -3 tools/content_pipeline/cook_actor_pbr.py [--root DIR] [--pins] [--only STEM_SUBSTR] [--jobs N]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))
from tools.content_pipeline import texcook  # noqa: E402

TREES = ("data/textures/mech_pbr", "data/textures/vehicle_pbr")
KINDS = {"_normal": "normal", "_orm": "orm"}
PINS = "data/kit/actor-pbr-pins.json"


def sources(root: Path):
    for tree in TREES:
        for png in sorted((root / tree).glob("*.png")):
            for suffix, role in KINDS.items():
                if png.stem.endswith(suffix):
                    yield png, role


def cook_one(png: Path, role: str) -> tuple[str, str]:
    rgba = texcook.load_rgba(png)
    data = texcook.cook_ktx2_levels(texcook.mip_chain(rgba, role), texcook.VK_BC7_UNORM, "BC7_UNORM", quick=True)
    out = png.with_suffix(".ktx2")
    if not out.is_file() or out.read_bytes() != data:
        out.write_bytes(data)
    return out.as_posix(), hashlib.sha256(data).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(REPO))
    ap.add_argument("--pins", action="store_true", help="rewrite the pin file from the cooked bytes")
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=2)
    a = ap.parse_args()
    root = Path(a.root)
    todo = [(p, r) for p, r in sources(root) if a.only in p.stem]
    with ThreadPoolExecutor(max(1, a.jobs)) as pool:
        done = list(pool.map(lambda pr: cook_one(*pr), todo))
    rows = {Path(o).relative_to(root).as_posix(): {"ktx2_sha256": sha} for o, sha in done}
    print(f"[ACTOR_PBR_COOK] cooked={len(rows)}")
    if a.pins:
        pins = root / PINS
        pins.parent.mkdir(parents=True, exist_ok=True)
        pins.write_text(json.dumps(dict(sorted(rows.items())), indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
