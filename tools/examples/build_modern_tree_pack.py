#!/usr/bin/env python3
"""Modern tree pack: source GLBs -> content IR -> the common validated cooker."""
import argparse
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline.cook import cook

TREE_SCALE = 0.5

ASSET_FAMILIES = [
    {
        "name": "maple",
        "class": "tree",
        "replaces": "tree:maple1",
        "subdir": "uploads_files_4996646_Broadleaf_GLTF",
        "lods": [
            {"filename": "maple1.glb", "distance_m": 0},
            {"filename": "maple2.glb", "distance_m": 600},
            {"filename": "maple3.glb", "distance_m": 1200},
        ],
    },
    {
        "name": "poplar",
        "class": "tree",
        "replaces": "tree:pine1",
        "subdir": "uploads_files_4996646_Broadleaf_GLTF",
        "lods": [
            {"filename": "poplar1.glb", "distance_m": 0},
            {"filename": "poplar2.glb", "distance_m": 600},
            {"filename": "poplar3.glb", "distance_m": 1200},
            {"filename": "poplar4.glb", "distance_m": 2000},
        ],
    },
    {
        "name": "white_poplar",
        "class": "tree",
        "replaces": "tree:maple2",
        "subdir": "uploads_files_4996646_Broadleaf_GLTF",
        "lods": [
            {"filename": "whitePoplar1.glb", "distance_m": 0},
            {"filename": "whitePoplar2.glb", "distance_m": 600},
            {"filename": "whitePoplar3.glb", "distance_m": 1200},
            {"filename": "whitePoplar4.glb", "distance_m": 2000},
        ],
    },
    {
        "name": "crane_big",
        "class": "staticprop",
        "replaces": "staticprop:crane",
        "subdir": "uploads_files_5382247_GlTF_models/GlTF_models/Crane_Big_Small/Crane_big",
        "lods": [
            {"filename": "crane_big.glb", "distance_m": 0},
        ],
    },
]

COVERAGE_ALIASES = {
    "tree:palm1":  "poplar",
    "tree:palms":  "poplar",
    "tree:palmx":  "poplar",
    "tree:palm1x": "poplar",
    "tree:oak1":   "white_poplar",
}


def recipe(asset_root, families=ASSET_FAMILIES, scale=TREE_SCALE):
    assets = []
    for family in families:
        sources = [Path(asset_root) / family["subdir"] / lod["filename"] for lod in family["lods"]]
        assets.append({"frontend": "gltf", "id": family["name"], "replaces": family["replaces"],
                       "kind": "tree" if family["class"] == "tree" else "prop", "intact": str(sources[0]),
                       "lods": [[str(p), lod["distance_m"]] for p, lod in zip(sources[1:], family["lods"][1:])],
                       "frame": "gltf", "wu_per_unit": scale, "unclassified": "legacy"})
    by_name = {f["name"]: f["replaces"] for f in families}
    return {"mod_id": "modern-tree-pack-v1", "name": "Modern Tree Pack v1", "assets": assets,
            "aliases": {key: by_name[name] for key, name in COVERAGE_ALIASES.items() if name in by_name}}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--asset-root", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--family")
    ap.add_argument("--tree-scale", type=float, default=TREE_SCALE)
    ap.add_argument("--clean", action="store_true", help="replace the output package")
    ap.add_argument("--deploy-root", type=Path, help="optional stock texture lookup root")
    args = ap.parse_args()
    families = [f for f in ASSET_FAMILIES if not args.family or f["name"] == args.family]
    if not families:
        ap.error("unknown family")
    spec = recipe(args.asset_root, families, args.tree_scale)
    if args.deploy_root:
        spec["texture_roots"] = [str(args.deploy_root / "data/tgl/128")]
    print(cook(spec, args.out, overwrite=args.clean))


if __name__ == "__main__":
    main()
