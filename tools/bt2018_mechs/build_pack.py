#!/usr/bin/env python3
"""Build the personal-use BT2018 mech pack (a mod) from the owner's own files.

Tools, recipes and conversion tables live in the repo (tools/bt2018_mechs); the
extracted/converted assets stay in the owner's folders and are only COPIED into the
generated mod. Nothing in the target install's base data is modified.

  py -3 tools/bt2018_mechs/build_pack.py \
      --install G:/mc2-installs/mc2-win64-bt2018 \
      --conversions A:/Games/mc2-opengl/MC2Conversions \
      --texture-source A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl \
      [--mod-id bt2018_mechs] [--only marauder] [--check]

Writes <install>/mods/<mod-id>/ (or --out) and prints the files with sha256. --check
builds into a temp dir and compares hashes with the existing mod (reproducibility).
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import shutil
import sys
import tempfile
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.bt2018_mechs import compile as C  # noqa: E402
else:
    from . import compile as C
if str(Path(__file__).resolve().parents[2]) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

REPO = Path(__file__).resolve().parents[2]
MOD_SCHEMA = "mc2-mod/1"
CAPABILITIES = ["imported_mechs", "package_chassis", "equipment_add"]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_text(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def install_compbas_used(install: Path):
    sys.path.insert(0, str(REPO / "tools"))
    import list_free_ordinals as lfo  # noqa: E402
    raw = lfo.load_csv_bytes(str(install))
    used = set()
    for row in csv.reader(io.StringIO(raw.decode("latin-1"))):
        try:
            o = int(row[0].strip())
        except (ValueError, IndexError):
            continue
        if len(row) > 1 and row[1].strip().lower() != "undefined":
            used.add(o)
    return used


def install_variant_names(install: Path):
    """Every variant name the install's chassis CSVs declare (row 23+97n, column 2). Salvage and
    recovery resolve a mech by variant name, so a pack variant must never reuse one."""
    sys.path.insert(0, str(REPO / "tools" / "mc2x_import"))
    import fst  # noqa: E402
    texts = {}
    for f in sorted(install.glob("*.fst")):
        for off, comp, real, name, is_lz, data in fst.fst_entries(str(f)):
            key = name.lower().replace("\\", "/")
            if key.startswith("data/objects/") and key.endswith(".csv"):
                texts[key] = fst.decompress_entry(off, comp, real, is_lz, data).decode("latin-1")
    for p in (install / "data" / "objects").glob("*.csv"):
        texts[f"data/objects/{p.name.lower()}"] = p.read_text(encoding="latin-1")
    names = {}
    for key, text in texts.items():
        rows = list(csv.reader(io.StringIO(text.replace("\r\n", "\n").replace("\r", "\n"))))
        r = 22
        while r < len(rows) and len(rows[r]) > 1 and rows[r][0].strip().lower() == "variantname":
            names[rows[r][1].strip().lower()] = key
            r += 97
    return names


def install_legacy_string_ids(install: Path):
    ids = set()
    for p in (install / "data" / "defs" / "text").rglob("*.fit"):
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            s = line.strip()
            if s.startswith("legacyId"):
                try:
                    ids.add(int(s.split("=", 1)[1]))
                except ValueError:
                    pass
    return ids


def build(args, out: Path):
    wm, bp = C.load_tables()
    from tools.bt2018_mechs import roster
    recipes = roster.recipes(Path(args.conversions))
    if args.only:
        keep = {s.strip() for s in args.only.split(",")}
        recipes = [r for r in recipes if r["family"] in keep]
    if not recipes:
        sys.exit("no recipes selected")

    install = Path(args.install)
    conv = Path(args.conversions)
    tex_src = Path(args.texture_source) if args.texture_source else None
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    profiles, recipes_loaded = [], []
    for recipe in recipes:
        profiles.append(C.build_profile(recipe, wm, bp))
        recipes_loaded.append(recipe)

    # Explicit ordinals must be free in the target provider; string ids must be unused.
    used_ord = install_compbas_used(install)
    pack_defs = C.used_pack_equipment(profiles, wm)
    clash = [d["id"] for d in pack_defs if d["ordinal"] in used_ord]
    if clash:
        sys.exit(f"pack equipment ordinals already used in {install}: {clash}")
    stock_variants = install_variant_names(install)
    for p in profiles:
        for v in p["variants"]:
            owner = stock_variants.get(v["name"].strip().lower())
            if owner:
                sys.exit(f"{p['chassis_id']}: variant name '{v['name']}' already used by {owner} (salvage/recovery resolve by name)")
    used_sid = install_legacy_string_ids(install)
    for i, p in enumerate(profiles):
        sid = C.string_ids(p, i)
        if set(sid.values()) & used_sid:
            sys.exit(f"{p['chassis_id']}: string ids {sid} collide with installed text")

    files = {}
    stock_refs = {}
    for i, (p, recipe) in enumerate(zip(profiles, recipes_loaded)):
        stem, fam = p["stem"], p["family"]
        files[f"data/objects/{stem}.csv"] = C.emit_csv(p, recipe, i)
        files[f"data/defs/chassis/bt2018/{fam}/chassisdef.fit"] = C.emit_chassis_fit(p, recipe)
        host = recipe["mc2"]["model"]["host_ini"]
        host_path = install / "data" / "tgl" / f"{host}.ini"
        if not host_path.is_file():
            sys.exit(f"host appearance INI not found as a loose file: {host_path}")
        stock_refs[f"data/tgl/{host}.ini"] = sha256(host_path)
        files[f"data/tgl/{stem}.ini"] = C.emit_ini(p, recipe, host_path.read_text(encoding="latin-1"))

        model = recipe["mc2"]["model"]
        pack_path = model["pack_path"]
        base_pkg = C.load_json(conv / model["package_json"])
        from tools.bt2018_mechs import sockets as SK
        socket_joints = {}
        for loc_sockets in recipe["mc2"]["sockets"].values():
            for sname in loc_sockets:
                joint = base_pkg["nodes"].get(sname) or base_pkg["nodes"].get(sname.rstrip("0123456789"))
                if joint:
                    socket_joints[sname] = joint
        offsets = SK.socket_offsets(conv / model["source_glb"], socket_joints)
        files[f"data/tgl/{pack_path}.package.json"] = json.dumps(C.emit_package_json(base_pkg, recipe, offsets), indent=2) + "\n"
        write_text(out / "content" / f"{fam}.profile.json", json.dumps(p, indent=2, sort_keys=True) + "\n")

    files["data/defs/text/en_us/bt2018_mechs.fit"] = C.emit_strings(profiles)
    for d in pack_defs:
        name = d["id"].split(":", 1)[1]
        files[f"data/defs/equipment/bt2018/{name}/equipmentdef.fit"] = C.emit_equipment_def(d)

    for rel, text in files.items():
        write_text(out / rel, text)

    # Assets: copied from the owner's converted files. With --bundles each state plays the
    # family's own BT clip, baked from the player's bundle under the Atlas state name (BT's
    # override controller maps <family>_X onto atlas_X); a state without one keeps the
    # model-space Atlas retarget.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
    from bt2018_retarget_clips import retarget_file
    sources = {}
    assets = {}
    for p, recipe in zip(profiles, recipes_loaded):
        model = recipe["mc2"]["model"]
        dst = out / "data" / "tgl" / f"{model['pack_path']}.glb"
        dst.parent.mkdir(parents=True, exist_ok=True)
        retarget_file(str(conv / model["source_glb"]), str(conv / "atlas.glb"), str(dst))
        from tools.bt2018_mechs import glb_guard
        for name, n in glb_guard.dedupe_node_names(dst).items():
            print(f"[bt2018] {dst.name}: renamed {n} duplicate node(s) '{name}' (Assimp rejects repeats)")
        for name in glb_guard.opaque_mech_materials(dst):
            print(f"[bt2018] {dst.name}: material '{name}' set OPAQUE (BLEND/MASK makes the importer look for an a_ texture)")
        fam = recipe["family"].lower()
        if args.bundles and fam != "atlas":
            from tools.bt2018_extract import bake_clips as BK
            raw = Path(args.bundles)
            cand = [raw / f"chrprfmech_{fam}base-001"] + [raw / b for b in ("heavymetal", "flashpoint", "urbanwarfare")]
            native = next((n for n in (BK.native_clips(str(b), fam) for b in cand if b.is_file()) if n), {})
            BK.add_to_glb(str(dst), str(dst), {"atlas_" + k.split("_", 1)[1]: v for k, v in native.items()})
            sources[fam] = len(native)
        assets[str(dst.relative_to(out)).replace("\\", "/")] = sha256(dst)
        for t in model["textures"]:
            dst = out / "data" / "tgl" / Path(t).name
            shutil.copyfile(conv / t, dst)
            assets[str(dst.relative_to(out)).replace("\\", "/")] = sha256(dst)
        if tex_src:
            pkg = C.load_json(conv / model["package_json"])
            for slot in ("albedo", "ao", "normal"):
                k = pkg.get("materials", {}).get(slot, {}).get("ktx2")
                if k and (tex_src / k).is_file():
                    dst = out / "data" / "tgl" / k
                    shutil.copyfile(tex_src / k, dst)
                    assets[str(dst.relative_to(out)).replace("\\", "/")] = sha256(dst)
        # P6: icons rendered offline from the same model (generated, so hashed with the generated files).
        if not args.no_icons:
            from tools.bt2018_mechs import render_icons
            for name in render_icons.build_icons(conv / model["source_glb"], p["stem"], out / "data" / "art"):
                dst = out / "data" / "art" / name
                assets[f"data/art/{name}"] = sha256(dst)

    mod = {
        "schema": MOD_SCHEMA,
        "id": args.mod_id,
        "name": "BattleTech 2018 Mechs (personal use)",
        "version": "0.1.0",
        "author": "generated by tools/bt2018_mechs/build_pack.py",
        "type": "assets",
        "dependencies": [],
        "capabilities": CAPABILITIES,
        "description": "First-class BT2018 chassis (" + ", ".join(p["chassis_id"] for p in profiles) + ") generated from the owner's own BattleTech files. Add-on layer: mount with MC2_MOD_DEPS=" + args.mod_id + ". Not for redistribution.",
    }
    write_text(out / "mod.json", json.dumps(mod, indent=2) + "\n")

    report = {
        "schema": "bt2018-pack-report/1",
        "generated": {rel: hashlib.sha256(t.encode("utf-8")).hexdigest() for rel, t in sorted(files.items())},
        "assets": dict(sorted(assets.items())),
        "stock_references_unmodified": stock_refs,
        "native_clip_counts": sources,
        "balance_profile": bp["id"],
        "weapon_map_version": wm["version"],
        "profiles": {p["chassis_id"]: {"variants": [v["name"] for v in p["variants"]], "report": p["report"]} for p in profiles},
    }
    write_text(out / "content" / "report.json", json.dumps(report, indent=2) + "\n")
    from tools.bt2018_mechs import glb_guard
    broken = glb_guard.validate_pack_glbs(out)
    if broken:
        sys.exit("pack GLBs the engine importer would reject:\n" + "\n".join(f"  {f}: {'; '.join(b)}" for f, b in broken.items()))
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", required=True)
    ap.add_argument("--conversions", default="A:/Games/mc2-opengl/MC2Conversions")
    ap.add_argument("--texture-source", default="")
    ap.add_argument("--mod-id", default="bt2018_mechs")
    ap.add_argument("--out", default="")
    ap.add_argument("--only", default="")
    ap.add_argument("--no-icons", action="store_true", help="skip the offline icon render (P6)")
    ap.add_argument("--bundles", default="", help="BT assetbundles dir: bake native clips (py -3.12, UnityPy 1.25.0)")
    ap.add_argument("--check", action="store_true", help="rebuild into a temp dir and compare with the existing mod")
    args = ap.parse_args()
    out = Path(args.out) if args.out else Path(args.install) / "mods" / args.mod_id
    if args.check:
        with tempfile.TemporaryDirectory() as td:
            fresh = build(args, Path(td) / args.mod_id)
            prev = json.loads((out / "content" / "report.json").read_text(encoding="utf-8"))
            same = fresh["generated"] == prev["generated"] and fresh["assets"] == prev["assets"]
            print("reproducible" if same else "DIFFERS from the existing build")
            return 0 if same else 1
    report = build(args, out)
    for rel, h in {**report["generated"], **report["assets"]}.items():
        print(f"{h[:12]}  {rel}")
    for cid, p in report["profiles"].items():
        for line in p["report"]:
            print(f"[{cid}] {line}")
    print(f"[bt2018] wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
