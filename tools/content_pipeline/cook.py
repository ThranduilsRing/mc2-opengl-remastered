#!/usr/bin/env python3
"""cook.py -- the one entry point: recipe JSON -> frontends -> IR -> validated cook -> mod package.

  py -3 tools/content_pipeline/cook.py RECIPE.json [--out DIR] [--overwrite] [--no-compress]

Recipe:
  {"mod_id": "cp-hangar-fence", "name": "...", "out": "G:/mc2-assets/out/content-pipeline/<id>",
   "srcdata": "A:/Games/mc2-srcdata/tgl",                  stock ASE/INI directory
   "texture_roots": ["<mod>/data/tgl/128", "<install>/data/tgl/128"],   VFS order, highest first
   "conditions": {"mc2_01": {"hangar": "worn"}},           optional per-mission picks
   "assets": [ {"frontend": "stock_ase", ...} | {"frontend": "gltf", ...} ]}

stock_ase asset: {"prop": "hangar", "kind": "building", "slots": [reviewed detail rows], "destroyed": true,
                  "excludeNodes": [exact reviewed mesh names that keep legacy lighting, e.g. "LitWin_Quonset2"]}
gltf asset:      {"id", "replaces", "kind", "intact": GLB, "frame": "gltf"|"mc2_runtime", "wu_per_unit",
                  "classes": {material name: class | {class, metallic, roughness}},
                  "lods": [[GLB, distance]], "destroyed": GLB | {"stock_ase": "<ase stem>"},
                  "conditions": {"worn": {"intact": GLB, "destroyed": GLB}}, "license", "source_url"}
Exit 0 on a validated package, 2 on a recipe/validation error.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline import ir, package  # noqa: E402
from tools.content_pipeline.frontends import bt2018, gltf_file, stock_ase, stock_meshdump  # noqa: E402


def build_asset(spec, recipe, work):
    roots = [Path(r) for r in recipe.get("texture_roots", [])]
    front = spec.get("frontend")
    if front == "bt2018":
        return bt2018.to_ir(spec["intact"], spec.get("package"), classes=spec.get("classes"),
                            work_dir=work / spec.get("id", "mech"))
    if front == "stock_meshdump":
        return stock_meshdump.to_ir(spec["source"], asset_id=spec.get("id"), replaces=spec.get("replaces"),
                                    kind=spec.get("kind", "prop"), texture_roots=roots)
    if front == "stock_ase":
        return stock_ase.to_ir(spec["prop"], srcdata=recipe["srcdata"], texture_roots=roots,
                               slots=spec.get("slots", []), kind=spec.get("kind", "building"),
                               work_dir=work / spec["prop"], destroyed=spec.get("destroyed", True),
                               exclude=spec.get("excludeNodes", []))
    if front == "gltf":
        destroyed = spec.get("destroyed")
        dependencies = []
        if isinstance(destroyed, dict) and "stock_ase" in destroyed:
            source = Path(recipe["srcdata"]) / f"{destroyed['stock_ase']}.ase"
            dependencies.append(ir.Source.of("geometry_destroyed", source))
            scene, mats, _ = stock_ase.scene_from_ase(source, roots, work / spec["id"])
            destroyed = (scene, mats)
        meta = {k: spec[k] for k in ("frame", "wu_per_unit", "yaw_degrees", "license", "source_url", "provider", "unclassified")
                if k in spec}
        meta["texture_roots"] = [str(r) for r in [Path(spec["intact"]).parent, *roots, *map(Path, spec.get("texture_roots", []))]]
        asset = gltf_file.to_ir(spec["id"], spec["replaces"], spec["intact"], kind=spec.get("kind", "building"),
                               lods=[tuple(x) for x in spec.get("lods", [])], destroyed=destroyed,
                               conditions=spec.get("conditions"), classes=spec.get("classes"),
                               meta=meta, work_dir=work / spec["id"], base_color=spec.get("base_color"),
                               texel_scale=spec.get("texel_scale", 1), repair_inward_hull=spec.get("repair_inward_hull", False))
        asset.provenance.extend(dependencies)
        return asset
    raise ValueError(f"unknown frontend {front!r} (stock_ase | gltf)")


def cook(recipe, out=None, overwrite=False, compress=True):
    out = Path(out or recipe["out"])
    with tempfile.TemporaryDirectory(prefix="cp-work-") as tmp:
        assets = [build_asset(spec, recipe, Path(tmp)) for spec in recipe["assets"]]
        result = package.build_package(assets, out, mod_id=recipe["mod_id"], name=recipe.get("name"),
                                       description=recipe.get("description", ""), compress=compress,
                                       overwrite=overwrite, aliases=recipe.get("aliases"))
    if recipe.get("conditions"):
        path = result / "data" / "model_overrides" / "conditions.json"
        path.write_text(json.dumps({"version": 1, "missions": recipe["conditions"]}, indent=2) + "\n", encoding="utf-8")
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("recipe", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--no-compress", action="store_true", help="RGBA8 KTX2 (tests / no texconv)")
    args = ap.parse_args(argv)
    try:
        out = cook(json.loads(args.recipe.read_text(encoding="utf-8")), args.out, args.overwrite, not args.no_compress)
    except (ValueError, KeyError, OSError, RuntimeError) as e:
        print(f"cook: {e}", file=sys.stderr)
        return 2
    print(f"cook: package {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
