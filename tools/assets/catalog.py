"""Inventory stock appearances and rank their modernization value without the game.

Run: py -3 tools/assets/catalog.py [--sheets]
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import math
from pathlib import Path
import re
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.assets.common import (INSTALL_ROOT, INSTALLS, Install, bounds, fit_sections,
                                 model_refs, model_sections, output_path, parse_tgl, stock_install,
                                 texture_image, write_json)
from tools.assets.mapgen_data import catalog as type_catalog, packet

# Reused from tools/mapgen/extract.py at e322675cec833f1926caed91e08ecdc830e68f58.
OBJECT = struct.Struct("<i4f5i")


def object_records(raw):
    if len(raw) < 4:
        raise ValueError("missing object count")
    count, = struct.unpack_from("<i", raw)
    if count < 0 or len(raw) != 4 + count*OBJECT.size:
        raise ValueError("mission object count/size mismatch")
    records = list(OBJECT.iter_unpack(raw[4:]))
    if any(not all(math.isfinite(v) for v in r[1:5]) for r in records):
        raise ValueError("nonfinite mission object position")
    return records


def priority(usage, b, triangles, texture_sizes):
    # Isometric projected-area proxy; no claims of a measured camera screen size.
    x, y, z = b["size"]
    area = x*z*.5 + y*(x+z)*.70710678
    tex = min((min(s) for s in texture_sizes), default=128)
    poverty = .5*(128/max(tex, 1)) + .5*math.sqrt(200/max(triangles, 1))
    return round(usage * math.sqrt(max(area, 0)) * poverty, 3)


def category(aliases, name):
    classes = [v["class"] for v in aliases]
    if "mech" in classes:
        return "mech"
    for cls in ("fence", "wall", "gate", "turret", "vehicle", "building", "bridge", "rock", "tree", "prop"):
        if cls in classes:
            return cls
    if any(w in name for w in ("fence", "sandbag")):
        return "fence"
    if "hangar" in name:
        return "building"
    return "prop" if aliases else "unreferenced"


def build_catalog(root=INSTALL_ROOT):
    stock = stock_install(root)
    types, provenance = type_catalog(stock)
    aliases = defaultdict(list)
    object_pak = stock.read("data/objects/object2.pak")[0]
    for number, values in types.items():
        if values["object_class"] not in (0, 1, 2, 3, 11, 15, 18, 21, 22, 23):
            continue  # projectiles, effects and abstract object types have no model
        sections = fit_sections(packet(object_pak, number))
        collision = {section: {k: v for k, v in fields.items() if k in
                     ("extentradius", "destroyedobject", "ximpasse", "yimpasse", "setimpassable",
                      "impassablewhendestroyed", "lowtemplate", "hightemplate", "blocklineoffire")}
                     for section, fields in sections.items()}
        aliases[values["appearance"].lower()].append({"type_id": number, **values,
            "collision_fields": {k: v for k, v in collision.items() if v}})
    records, errors, referenced = [], [], set()
    cache = {}

    def describe(ref):
        name = ref["tgl"]
        if name not in cache:
            info = {"tgl": name, "ase_present": stock.has(ref["ase"])}
            try:
                raw, source = stock.read(name)
                mesh = parse_tgl(raw)
                info.update(mesh.summary(), source=source, sha256=hashlib.sha256(raw).hexdigest())
                tx = []
                # List every texture tier, even shadow slots and shared atlases.
                for texture in dict.fromkeys(mesh.textures):
                    matches = [p for p in stock.files if p.startswith("data/tgl/") and p.split("/")[-1] == texture]
                    variants = []
                    for path in sorted(matches):
                        b, origin = stock.read(path)
                        from PIL import Image
                        import io
                        try:
                            image = Image.open(io.BytesIO(b))
                            variants.append({"file": path, "resolution": list(image.size), "source": origin})
                        except (OSError, ValueError) as e:
                            variants.append({"file": path, "error": str(e)})
                    tx.append({"name": texture, "variants": variants, "missing": not bool(matches)})
                info["texture_files"] = tx
            except (OSError, ValueError, struct.error) as e:
                info["error"] = str(e)
            cache[name] = info
        return {**ref, **cache[name]}

    ini_names = {n[len("data/tgl/"):-4] for n in stock.files if n.startswith("data/tgl/") and n.endswith(".ini")}
    for name in sorted(ini_names | aliases.keys()):
        ini = f"data/tgl/{name}.ini"
        sections, source = {}, None
        if stock.has(ini):
            raw, source = stock.read(ini)
            sections = model_sections(raw)
        refs = model_refs(sections)
        # An object may have a direct TGL but no INI in incomplete stock data.
        if not refs and stock.has(f"data/tgl/{name}.tgl"):
            refs = [{"role": "intact", "lod": 0, "distance_wu": 0,
                     "tgl": f"data/tgl/{name}.tgl", "ase": f"data/tgl/{name}.ase"}]
        if not refs and not aliases[name]:
            continue  # animation/arm configuration INIs without object geometry
        referenced.update(r["tgl"] for r in refs)
        models = [describe(r) for r in refs]
        base = next((m for m in models if m["role"] == "intact" and m["lod"] == 0), None)
        entry = {"id": name, "category": category(aliases[name], name), "types": aliases[name],
                 "ini": ini if source else None, "ini_source": source,
                 "ini_sections": sections, "models": models, "missions": [], "instances": 0,
                 "priority": 0, "coverage": "ok" if base and "bounds" in base else "missing_base"}
        records.append(entry)
    # Every binary shape is accounted for; these include ENC previews, arms,
    # effects and unused damage models, so keep them out of the object ranking.
    orphans = [describe({"role": "unassigned", "lod": 0, "distance_wu": 0,
                        "tgl": n, "ase": n[:-4]+".ase"}) for n in sorted(stock.files)
               if n.startswith("data/tgl/") and n.endswith(".tgl") and n not in referenced]
    by_name = {r["id"]: r for r in records}
    # Preserve modified geometry under the same selection key as a separate
    # binding; mods commonly re-export all TGLs, so byte identity alone is too strict.
    hash_to_ids = defaultdict(set)
    path_to_ids = defaultdict(set)
    for r in records:
        for m in r["models"]:
            if m["role"] == "intact" and m["lod"] == 0 and "sha256" in m:
                hash_to_ids[m["sha256"]].add(r["id"])
                path_to_ids[m["tgl"]].add(r["id"])
    installs = {}
    dependency = None
    for label, folder in INSTALLS.items():
        try:
            if not (Path(root)/folder).is_dir():
                raise FileNotFoundError(Path(root)/folder)
            fallback = dependency if label == "MCO-Carver5" and dependency else stock
            install = stock if label == "stock" else Install(Path(root)/folder, fallback=fallback)
            if label == "MechCommanderOmnitech":
                dependency = install
            itypes, sources = type_catalog(install)
            binding = {}
            modified = {}

            def resolve(number):
                if number in binding:
                    return binding[number]
                info = itypes.get(number)
                ids = set()
                if info:
                    appearance = info["appearance"].lower()
                    ini = f"data/tgl/{appearance}.ini"
                    refs = model_refs(model_sections(install.read(ini)[0])) if install.has(ini) else []
                    path = next((r["tgl"] for r in refs if r["role"] == "intact" and r["lod"] == 0), f"data/tgl/{appearance}.tgl")
                    if install.has(path):
                        digest = hashlib.sha256(install.read(path)[0]).hexdigest()
                        exact = hash_to_ids.get(digest, set())
                        all_ids = exact or path_to_ids.get(path, set())
                        # Choose the appearance alias if present; otherwise a single
                        # canonical stock ID. Never multiply counts by alias count.
                        ids = {appearance} if appearance in all_ids else set(sorted(all_ids)[:1])
                        modified[number] = not bool(exact)
                        for ident in ids:
                            bindings = by_name[ident].setdefault("corpus_bindings", [])
                            if not any(b["corpus"] == label and b["sha256"] == digest for b in bindings):
                                bindings.append({"corpus": label, "tgl": path, "sha256": digest,
                                                 "geometry": "byte_identical" if exact else "modified_same_selection_key"})
                binding[number] = ids
                return ids

            names = sorted(n for n in install.files if n.startswith("data/missions/") and n.endswith(".pak"))
            stats = {"root": str(install.root), "missions": 0, "objects": 0, "parts": 0,
                     "matched_stock_instances": 0, "unresolved_type_ids": {},
                     "modified_geometry_instances": 0,
                     "custom_or_missing_instances": 0, "errors": [], **sources}
            unresolved = Counter()
            for filename in names:
                mission = filename[len("data/missions/"):-4]
                try:
                    pak, origin = install.read(filename)
                    objects = object_records(packet(pak, 1))
                    counts = Counter((r[0], "terrain") for r in objects)
                    fitfile = filename[:-4]+".fit"
                    if install.has(fitfile):
                        sections = fit_sections(install.read(fitfile)[0])
                        for section, fields in sections.items():
                            if re.fullmatch(r"part\d+", section) and "objectnumber" in fields and fields.get("exists", 1):
                                counts[(fields["objectnumber"], "part")] += 1
                    matched = defaultdict(Counter)
                    for (number, kind), count in counts.items():
                        stats["objects" if kind == "terrain" else "parts"] += count
                        ids = resolve(number)
                        if not ids:
                            if number not in itypes:
                                unresolved[number] += count
                            else:
                                stats["custom_or_missing_instances"] += count
                        for ident in ids:
                            matched[ident][kind] += count
                            if modified.get(number):
                                matched[ident]["modified"] += count
                                stats["modified_geometry_instances"] += count
                    for ident, kinds in matched.items():
                        total = kinds["terrain"] + kinds["part"]
                        by_name[ident]["missions"].append({"corpus": label, "mission": mission,
                            "instances": total, "terrain_instances": kinds["terrain"],
                            "modified_geometry_instances": kinds["modified"],
                            "part_instances": kinds["part"], "source": origin})
                        by_name[ident]["instances"] += total
                        stats["matched_stock_instances"] += total
                    stats["missions"] += 1
                except (OSError, ValueError, struct.error) as e:
                    stats["errors"].append({"mission": mission, "error": str(e)})
            stats["unresolved_type_ids"] = dict(sorted(unresolved.items()))
            stats["archive_collisions"] = install.collisions
            installs[label] = stats
            print(f"{label}: {stats['missions']} missions, {stats['matched_stock_instances']} stock instances, {len(stats['errors'])} errors", flush=True)
        except (OSError, ValueError, struct.error) as e:
            errors.append({"corpus": label, "error": str(e)})
    for r in records:
        base = next((m for m in r["models"] if m["role"] == "intact" and m["lod"] == 0 and "bounds" in m), None)
        if base:
            sizes = [v["resolution"] for t in base["texture_files"] for v in t["variants"] if "resolution" in v]
            r["priority"] = priority(r["instances"], base["bounds"], base["triangles"], sizes)
    return {"schema": "mc2-assets-1", "stock_source": provenance,
            "stock_definition": "stock FSTs plus pristine mc2srcdata-fresh object2.pak/Buildings.csv; no dev loose overrides",
            "usage_definition": "mission terrain objects + existing FIT parts (active and scripted inactive); alternatives are potential instances; not simultaneous visibility; each installed corpus counted separately. Mod re-exports matched by stock LOD0 selection key, with modified geometry counts and SHA256 bindings retained",
            "priority_formula": "instances * sqrt(0.5*x*z + 0.70710678*y*(x+z)) * (0.5*128/min_texture_edge + 0.5*sqrt(200/triangles)); TG XYZ size; projected-area proxy",
            "installs": installs, "objects": records, "unassigned_models": orphans, "errors": errors,
            "coverage": {"appearances": len(records), "with_base": sum(r["coverage"] == "ok" for r in records),
                         "stock_tgl_files": sum(n.endswith(".tgl") for n in stock.files),
                         "referenced_tgl_files": len(referenced & stock.files.keys()),
                         "unassigned_tgl_files": len(orphans),
                         "model_errors": sum("error" in m for m in cache.values()),
                         "missions": sum(s["missions"] for s in installs.values())}}


def markdown(data):
    rows = ["# Stock model catalog", "", "Generated by `py -3 tools/assets/catalog.py --sheets`.", "",
            data["stock_definition"], "", data["usage_definition"], "", "Priority: `"+data["priority_formula"]+"`.", "",
            "The score is an authoring heuristic, not a frame-cost measurement. Mechs are listed separately. Unassigned binaries include shadows, encyclopedia models, limbs and effects.", "",
            "Coverage: `"+str(data["coverage"])+"`.", "", "| Corpus | Missions | Stock instances | Custom/missing | Errors |", "|---|---:|---:|---:|---:|"]
    for label, s in data["installs"].items():
        rows.append(f"| {label} | {s['missions']} | {s['matched_stock_instances']} | {s['custom_or_missing_instances']} | {len(s['errors'])} |")
    for title, entries in (("Modernization order", [r for r in data["objects"] if r["category"] not in ("mech", "tree")]),
                           ("Vegetation", [r for r in data["objects"] if r["category"] == "tree"]),
                           ("Mechs", [r for r in data["objects"] if r["category"] == "mech"])):
        rows += ["", "## "+title, "", "| Model / type aliases | Category | Instances | Tris | Size X/Y/Z (WU) | Texture | Priority |", "|---|---|---:|---:|---|---|---:|"]
        for r in sorted(entries, key=lambda r: (-r["priority"], r["id"])):
            b = next((m for m in r["models"] if m["role"] == "intact" and m["lod"] == 0 and "bounds" in m), {})
            dims = " × ".join(f"{n:.1f}" for n in b.get("bounds", {}).get("size", [])) or r["coverage"]
            resolutions = sorted({"×".join(map(str, v["resolution"])) for t in b.get("texture_files", []) for v in t["variants"] if "resolution" in v})
            aliases = ", ".join(sorted({t['type_name'] for t in r['types']}))
            rows.append(f"| {r['id']} ({aliases}) | {r['category']} | {r['instances']} | {b.get('triangles', '—')} | {dims} | {', '.join(resolutions)} | {r['priority']:.1f} |")
    rows += ["", "## Data quality", "", "Full references, LOD distances, destroyed variants, texture paths, per-mission counts, node pivots and all decode failures are in [catalog.json](catalog.json).", "",
             "Archive collisions and unresolved type IDs are preserved per corpus. Missing models have zero priority; they are not fabricated. Missing declared stock LODs are retained. Mod geometry changes under stock selection keys are explicitly recorded; custom keys are excluded.", ""]
    return "\n".join(rows)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, default=INSTALL_ROOT)
    p.add_argument("--out", type=Path, default=Path("docs/assets"))
    p.add_argument("--sheets", action="store_true")
    a = p.parse_args(argv)
    out = output_path(a.out)
    data = build_catalog(a.root)
    write_json(out/"catalog.json", data)
    (out/"catalog.md").write_text(markdown(data), encoding="utf-8")
    print(data["coverage"], flush=True)
    if a.sheets:
        from tools.assets.render import sheets
        sheets(data, stock_install(a.root), out/"sheets")
    return int(bool(data["errors"]) or any(s["errors"] for s in data["installs"].values()))


if __name__ == "__main__":
    raise SystemExit(main())
