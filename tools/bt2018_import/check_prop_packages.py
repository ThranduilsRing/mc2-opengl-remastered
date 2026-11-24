#!/usr/bin/env py -3
"""Offline checker for BT2018 prop packages (extract_bt2018_props.py output).

  py -3 tools/bt2018_import/check_prop_packages.py <out dir> [--cooked <mod dir>] [--mapping mapping.json]

Per package: manifest schema and required fields, LOD GLBs exist with matching sha256 and decode
(glTF header, accessors in range, every material texture resolves to an embedded image), the scale is
sane (largest extent 0.5..250 m at 4 wu/m; terrain-sized meshes fail), triangle counts fall with LOD,
and every `replaces` key is '<class>:<name>'. With --cooked the mod's models.json must carry every
replaces key and every referenced file must exist. With --mapping every picked stock key must be replaced by
the package of its pick, and keys reviewed "none" (and unpicked keys) must stay stock. Exit 0 = no errors.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

REQUIRED = ("schema", "id", "category", "source", "units", "bounds", "footprint_wu", "lods", "materials", "replaces")
MIN_M, MAX_M = 0.5, 250.0


def read_glb(path):
    raw = Path(path).read_bytes()
    magic, version, length = struct.unpack_from("<III", raw, 0)
    if magic != 0x46546C67 or version != 2 or length != len(raw):
        raise ValueError("bad GLB header")
    jlen, jtype = struct.unpack_from("<II", raw, 12)
    if jtype != 0x4E4F534A:
        raise ValueError("first chunk is not JSON")
    return json.loads(raw[20:20 + jlen]), hashlib.sha256(raw).hexdigest()


def check_glb(doc, errs, where):
    if not doc.get("meshes") or not doc.get("materials"):
        errs.append(f"{where}: no meshes or materials")
        return
    nviews, nimg = len(doc["bufferViews"]), len(doc.get("images", []))
    for acc in doc["accessors"]:
        if acc["bufferView"] >= nviews:
            errs.append(f"{where}: accessor bufferView out of range")
    for i, m in enumerate(doc["materials"]):
        for key, tex in (("baseColor", m["pbrMetallicRoughness"].get("baseColorTexture")), ("normal", m.get("normalTexture")),
                         ("orm", m["pbrMetallicRoughness"].get("metallicRoughnessTexture"))):
            if tex is None:
                errs.append(f"{where}: material {i} has no {key} texture")
            elif doc["textures"][tex["index"]]["source"] >= nimg:
                errs.append(f"{where}: material {i} {key} image unresolved")
        if not m.get("extras", {}).get("mc2_material_class"):
            errs.append(f"{where}: material {i} has no mc2_material_class")
    for prim in doc["meshes"][0]["primitives"]:
        for need in ("POSITION", "NORMAL", "TEXCOORD_0"):
            if need not in prim["attributes"]:
                errs.append(f"{where}: primitive lacks {need}")


def check_package(pdir):
    errs = []
    path = Path(pdir) / "bt2018_prop_package.json"
    man = json.loads(path.read_text(encoding="utf-8"))
    for key in REQUIRED:
        if key not in man:
            errs.append(f"{man.get('id', pdir)}: manifest lacks {key}")
    if errs:
        return man, errs
    if man["schema"] != "bt2018-prop-package/1":
        errs.append(f"{man['id']}: unknown schema {man['schema']}")
    size_m = [s / man["units"]["wu_per_m"] for s in man["bounds"]["size_wu"]]
    if not (MIN_M <= max(size_m) <= MAX_M):
        errs.append(f"{man['id']}: largest extent {max(size_m):.1f} m outside {MIN_M}..{MAX_M} (scale or wrong mesh)")
    tris = [l["triangles"] for l in man["lods"]]
    if tris != sorted(tris, reverse=True):
        errs.append(f"{man['id']}: triangles do not fall with LOD {tris}")
    for lod in man["lods"]:
        glb = Path(pdir) / lod["glb"]
        if not glb.is_file():
            errs.append(f"{man['id']}: missing {lod['glb']}")
            continue
        doc, digest = read_glb(glb)
        if digest != lod["sha256"]:
            errs.append(f"{man['id']}: {lod['glb']} sha256 differs from the manifest")
        check_glb(doc, errs, f"{man['id']}/{lod['glb']}")
    for key in man["replaces"]:
        if key.count(":") != 1 or not all(key.split(":")):
            errs.append(f"{man['id']}: replaces {key!r} is not <class>:<name>")
    return man, errs


def check_cooked(mod, manifests):
    errs = []
    mod = Path(mod)
    records = json.loads((mod / "data" / "model_overrides" / "models.json").read_text(encoding="utf-8"))["overrides"]
    have = {r["replaces"].lower(): r for r in records}
    for man in manifests:
        for key in man["replaces"]:
            rec = have.get(key.lower())
            if rec is None:
                errs.append(f"cooked: {key} missing from models.json")
                continue
            for rel in [rec["source"]] + [l["source"] for l in rec.get("lods", [])]:
                if not (mod / "data" / "model_overrides" / rel).is_file():
                    errs.append(f"cooked: {key} references missing {rel}")
    return errs


def check_mapping(mapping, manifests):
    errs = []
    owner = {k.lower(): man["id"] for man in manifests for k in man["replaces"]}
    for name, m in sorted(mapping.items()):
        key = m["key"].lower()
        if m.get("pick") and m.get("review") != "none":
            if owner.get(key) != m["pick"]:
                errs.append(f"mapping: {m['key']} should be replaced by {m['pick']}, packages give {owner.get(key)}")
        elif key in owner:
            errs.append(f"mapping: {m['key']} is reviewed none/unpicked but {owner[key]} replaces it")
    return errs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("root", type=Path)
    ap.add_argument("--cooked", type=Path)
    ap.add_argument("--mapping", type=Path)
    args = ap.parse_args(argv)
    manifests, errors = [], []
    for pdir in sorted(p for p in args.root.iterdir() if (p / "bt2018_prop_package.json").is_file()):
        man, errs = check_package(pdir)
        manifests.append(man)
        errors += errs
        print(f"{'FAIL' if errs else 'ok  '} {man.get('id', pdir.name)} {man.get('bounds', {}).get('size_wu')} "
              f"lods={[l['triangles'] for l in man.get('lods', [])]}")
    if args.cooked:
        errors += check_cooked(args.cooked, manifests)
    if args.mapping:
        errors += check_mapping(json.loads(args.mapping.read_text(encoding="utf-8"))["map"], manifests)
    for e in errors:
        print("ERROR", e)
    print(f"{len(manifests)} packages, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
