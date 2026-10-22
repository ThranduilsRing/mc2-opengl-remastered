"""Selection-based stock target specs, local matching and model_override packaging.

py -3 tools/assets/replace.py spec hangar
py -3 tools/assets/replace.py search wirefence
py -3 tools/assets/replace.py package hangar --candidate G:/mc2-assets/candidates/foo.glb --id modern-hangar
py -3 tools/assets/replace.py rework hangar --id rework-hangar
"""
from __future__ import annotations

import argparse
from dataclasses import replace as dataclass_replace
import hashlib
import json
import math
from pathlib import Path
import re
import sys

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.assets.common import (INSTALL_ROOT, OUT_ROOT, Mesh, bounds, output_path,
    parse_tgl, stock_install, texture_image, transform_normals, write_json)
from tools.assets.candidates import Candidate, export_runtime_gltf, load_candidate, load_gltf

CANDIDATES = Path("G:/mc2-assets/candidates")


def select(data, selection):
    selection = str(selection).lower()
    matches = [r for r in data["objects"] if selection == r["id"] or any(
        selection == str(t["type_id"]) or selection == t["type_name"].lower() for t in r["types"])]
    if len(matches) != 1:
        raise ValueError(f"selection must identify one appearance ({len(matches)} matches): {selection}")
    return matches[0]


def target_spec(entry, state="intact"):
    role = "destroyed" if state == "destroyed" else "intact"
    base = next((m for m in entry["models"] if m["role"] == role and m["lod"] == 0 and "bounds" in m), None)
    if base is None:
        raise ValueError(f"{entry['id']} has no decoded {role} LOD0")
    intact = next(m for m in entry["models"] if m["role"] == "intact" and m["lod"] == 0)
    peak = max((m["instances"] for m in entry["missions"]), default=0)
    tri_cap = max(base["triangles"], min(12000, max(64, int(250000/max(peak, 1)))))
    klass = "tree" if entry["category"] == "tree" else "staticProp"
    sections = entry["ini_sections"]
    animated = any(name.lower().startswith("animation:") for name in sections)
    for fields in sections.values():
        animated |= any(key.lower() == "animationnodeid" and str(value).lower() not in ("", "none")
                        or key.lower() == "spinme" and str(value).lower() in ("true", "t", "1")
                        for key, value in fields.items())
    supported = (not animated and entry["category"] not in
                 ("mech", "vehicle", "tree", "unreferenced", "gate", "turret") and state == "intact")
    destroyed = [m for m in entry["models"] if m["role"] == "destroyed"]
    return {"schema": "mc2-target-spec-1", "id": entry["id"], "state": state,
        "category": entry["category"], "selection_key": f"{klass}:{Path(intact['tgl']).stem}",
        "stock_tgl": base["tgl"], "stock_sha256": base.get("sha256"),
        "bounds": base["bounds"], "footprint_wu": base["bounds"]["footprint_wu"],
        "height_wu": base["bounds"]["height_wu"], "pivot_wu": [0, 0, 0],
        "base_center_wu": [(base["bounds"]["min"][0]+base["bounds"]["max"][0])/2,
                           base["bounds"]["min"][1], (base["bounds"]["min"][2]+base["bounds"]["max"][2])/2],
        "orientation": {"target_space": "TG X lateral, +Y up, -Z nominal forward; world units",
            "runtime_axis": 0, "runtime_mapping": "TG=(-glTF.x,-glTF.y,+glTF.z)",
            "cook": "bake all scene transforms, then inverse runtime axis0 into POSITION/NORMAL; identity nodes; scale=1",
            "auto_ground_static_prop": False,
            "semantic_front": "stock axes only; door/front direction needs visual review; declare candidate forward_axis"},
        "collision": {"authority": "stock object2 type fields and stock shape; renderOnly=true preserves gameplay",
            "type_fields": [{"type_id": t["type_id"], "fields": t.get("collision_fields", {})} for t in entry["types"]],
            "visual_extent_radius_wu": base["bounds"]["extent_radius_wu"],
            "ui_bounds_pixels_not_world_units": entry["ini_sections"].get("bounds", {})},
        "lods": [{"role": m["role"], "lod": m["lod"], "distance_ini_wu": m["distance_wu"],
                  "tgl": m["tgl"], "triangles": m.get("triangles"), "missing": "error" in m}
                 for m in entry["models"] if m["role"] == role],
        "lod_runtime": "staticProp override has one render shape; declared stock distances multiplied by 5; pack optional LODs as review assets, not promises of activation",
        "destroyed_requirement": {"required": bool(destroyed), "models": [m["tgl"] for m in destroyed],
            "runtime_geometry_override_supported": False,
            "policy": "damage stays stock in current BldgAppearance; an intact package requires explicit stock-damage fallback, or rework textures for both states; destroyed candidates can be cooked for review only"},
        "shadow": {"eligible": klass == "staticProp", "stock_shadow_models": [m["tgl"] for m in entry["models"] if "shadow" in m["role"]],
            "contract": "retain caster geometry; alpha texture does not disqualify fences/gates. TG gate excludes spotlights/windows/HUD/clamped/alphaTestOn/translucent shapes. Depth-only path may produce solid fence shadows; preview is not a shadow oracle"},
        "instances": entry["instances"], "peak_mission_instances": peak,
        "budgets": {"stock_triangles": base["triangles"], "max_triangles_lod0": tri_cap,
            "policy": "250k triangles / peak mission count, clamped 64..12000 and never below stock; conservative potential-instance proxy, not measured visibility",
            "max_texture_edge": 512 if peak >= 100 else 1024, "max_texture_bytes_rgba_with_mips": 16*1024*1024,
            "texture_policy": "textures shared by instances; total unique RGBA mip-chain estimate capped at 16 MiB"},
        "validation": {"relative_bounds_tolerance": .1, "absolute_bounds_tolerance_wu": .5,
                       "max_stretch_ratio": 1.25},
        "package_supported": supported,
        "runtime_limit": "intact static props only; animated gate/turret nodes need authored rig review; vehicles/mechs use their existing importer; vegetation uses tree cooker",
        "missions": entry["missions"]}


def orientation_matrix(metadata, yaw=0):
    axes = {"x": [1, 0, 0], "y": [0, 1, 0], "z": [0, 0, 1],
            "-x": [-1, 0, 0], "-y": [0, -1, 0], "-z": [0, 0, -1]}
    up = np.array(axes[metadata.get("up_axis", "y")], dtype=float)
    forward = np.array(axes[metadata.get("forward_axis", "+z").lstrip("+")], dtype=float)
    if abs(up @ forward) > 1e-6:
        raise ValueError("candidate up and forward axes must be perpendicular")
    right = np.cross(forward, up)
    matrix = np.array([right, up, -forward])
    r = math.radians(float(yaw))
    rotation = np.array([[math.cos(r), 0, math.sin(r)], [0, 1, 0], [-math.sin(r), 0, math.cos(r)]])
    return rotation @ matrix


def metadata_for(path):
    path = Path(path)
    sidecar = path.with_suffix(".asset.json")
    metadata = json.loads(sidecar.read_text(encoding="utf-8")) if sidecar.is_file() else {}
    return metadata


def orient(candidate, metadata, yaw=0):
    m = orientation_matrix(metadata, yaw)
    mesh = dataclass_replace(candidate.mesh, vertices=candidate.mesh.vertices @ m.T,
                             normals=transform_normals(candidate.mesh.normals, m))
    return Candidate(mesh, candidate.images, candidate.source), m


def fit_candidate(candidate, spec, metadata=None, *, yaw=0, stretch=False):
    candidate, matrix = orient(candidate, metadata or {}, yaw)
    current = bounds(candidate.mesh.vertices)
    sizes, desired = np.array(current["size"]), np.array(spec["bounds"]["size"])
    active = sizes >= 1e-8
    absolute = spec["validation"]["absolute_bounds_tolerance_wu"]
    if not active.any() or np.any((~active) & (desired > 2*absolute)):
        raise ValueError("candidate is planar on an axis with a substantial target extent")
    # A card fence or flattened damage mesh may legitimately be planar. Fit
    # nonzero dimensions and let final bounds validation decide the thin axis.
    ratios = np.ones(3)
    ratios[active] = desired[active]/sizes[active]
    if np.any(ratios[active] <= 0):
        raise ValueError("non-planar source cannot fit a zero-thickness target; author a flattened export")
    ratios[~active] = np.exp(np.log(ratios[active]).mean())
    distortion = float(ratios.max()/ratios.min())
    if stretch and distortion > spec["validation"]["max_stretch_ratio"]:
        raise ValueError(f"requested stretch {distortion:.3f} exceeds limit")
    scale = ratios if stretch else np.full(3, np.exp(np.log(ratios).mean()))
    points = candidate.mesh.vertices*scale
    lo, hi = points.min(axis=0), points.max(axis=0)
    anchor = np.array([(lo[0]+hi[0])/2, lo[1], (lo[2]+hi[2])/2])
    delta = np.array(spec["base_center_wu"])-anchor
    points += delta
    fitted = Candidate(dataclass_replace(candidate.mesh, vertices=points,
        normals=transform_normals(candidate.mesh.normals, np.diag(scale))), candidate.images, candidate.source)
    return fitted, {"orientation_matrix": matrix.tolist(), "scale_xyz": scale.tolist(),
                    "translation_wu": delta.tolist(), "stretch": stretch, "requested_yaw_degrees": yaw,
                    "metadata": metadata or {}, "semantic_orientation_verified": bool(metadata and metadata.get("orientation_reviewed"))}


def validate(candidate, spec, transform=None):
    errors, warnings = [], []
    try:
        b = bounds(candidate.mesh.vertices)
    except ValueError as error:
        return {"valid": False, "errors": [str(error)], "warnings": []}
    budget = spec["budgets"]
    if candidate.mesh.triangles > budget["max_triangles_lod0"]:
        errors.append(f"triangle budget: {candidate.mesh.triangles} > {budget['max_triangles_lod0']}")
    tolerance = np.maximum(np.array(spec["bounds"]["size"])*spec["validation"]["relative_bounds_tolerance"], spec["validation"]["absolute_bounds_tolerance_wu"])
    for edge in ("min", "max"):
        if np.any(np.abs(np.array(b[edge])-spec["bounds"][edge]) > tolerance):
            errors.append(f"{edge} bounds do not match target within {tolerance.tolist()}")
    tex_bytes = 0
    for image in candidate.images:
        if max(image.size) > budget["max_texture_edge"]:
            errors.append("texture edge exceeds budget (cook can resize before final validation)")
        tex_bytes += image.width*image.height*4*4/3
    if tex_bytes > budget["max_texture_bytes_rgba_with_mips"]:
        errors.append("unique texture memory exceeds budget")
    if not candidate.images or candidate.mesh.materials.min() < 0 or candidate.mesh.materials.max() >= len(candidate.images):
        errors.append("missing material texture")
    if not np.isfinite(candidate.mesh.uv).all():
        errors.append("nonfinite UVs")
    if transform:
        matrix = np.array(transform["orientation_matrix"])
        if not np.allclose(matrix @ matrix.T, np.eye(3), atol=1e-6) or not np.isclose(np.linalg.det(matrix), 1):
            errors.append("orientation is not a proper rotation")
        if not transform.get("semantic_orientation_verified"):
            warnings.append("mathematical orientation passes; candidate front/up needs owner visual review")
    warnings.append("headless validation only; no runtime pixels/shadow certification")
    return {"valid": not errors, "errors": errors, "warnings": warnings, "bounds": b,
            "triangles": candidate.mesh.triangles, "texture_bytes_rgba_with_mips": math.ceil(tex_bytes)}


def rank_candidate(spec, candidate, metadata):
    candidate, _ = orient(candidate, metadata)
    size = np.maximum(bounds(candidate.mesh.vertices)["size"], 1e-6)
    target = np.maximum(spec["bounds"]["size"], 1e-6)
    unit = float(metadata.get("wu_per_unit", 1))
    if unit <= 0 or not math.isfinite(unit):
        raise ValueError("wu_per_unit must be finite and positive")
    logs = np.log(target/(size*unit))
    shape = float(np.std(logs))
    footprint = abs(math.log((size[0]*size[2]*unit*unit)/(target[0]*target[2])))
    aspect = abs(math.log((size[0]/size[2])/(target[0]/target[2])))
    category = metadata.get("category", "unknown")
    category_penalty = 0 if category == spec["category"] else (1 if category == "unknown" else 4)
    budget_penalty = max(0, candidate.mesh.triangles/spec["budgets"]["max_triangles_lod0"]-1)
    score = 4*shape+aspect+.15*footprint+category_penalty+budget_penalty
    return {"score": round(score, 6), "shape_error": shape, "footprint_log_error": footprint,
            "aspect_log_error": aspect, "category": category, "category_penalty": category_penalty,
            "uniform_scale_estimate": float(np.exp(logs.mean())*unit),
            "triangles": candidate.mesh.triangles, "bounds": bounds(candidate.mesh.vertices),
            "license": metadata.get("license", "unrecorded"),
            "destroyed_candidate": metadata.get("destroyed"),
            "near_match": category_penalty == 0 and shape < .15 and budget_penalty == 0}


def search(spec, library=CANDIDATES):
    library = Path(library).resolve()
    results, errors = [], []
    if library.exists():
        for path in sorted(library.rglob("*")):
            if path.suffix.lower() not in (".glb", ".gltf", ".obj", ".fbx"):
                continue
            try:
                if path.suffix.lower() == ".fbx":
                    raise ValueError("convert local FBX first with convert-fbx")
                if not path.resolve().is_relative_to(library):
                    raise ValueError("candidate symlink escapes library")
                candidate = load_candidate(path)
                results.append({"source": str(path), **rank_candidate(spec, candidate, metadata_for(path))})
            except (OSError, ValueError, KeyError, IndexError) as error:
                errors.append({"source": str(path), "error": str(error)})
    return {"selection": spec["id"], "state": spec["state"], "library": str(library),
            "library_exists": library.is_dir(), "results": sorted(results, key=lambda r: (r["score"], r["source"])),
            "errors": errors, "downloads_performed": False}


def safe_mod_id(mod_id):
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,62}", mod_id):
        raise ValueError("mod id must be 1..63 lowercase letters, digits, _ or -")
    return mod_id


def package(entry, spec, candidate_path, mod_id, **kwargs):
    import tempfile
    destination = output_path(OUT_ROOT/safe_mod_id(mod_id), mod=True)
    if destination.exists():
        raise FileExistsError(f"refusing to overwrite package: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{mod_id}-", dir=destination.parent) as folder:
        staging = output_path(folder, mod=True)
        _package(entry, spec, candidate_path, mod_id, destination=staging, **kwargs)
        staging.rename(destination)
    return destination


def _package(entry, spec, candidate_path, mod_id, *, root=INSTALL_ROOT, yaw=0,
            stretch=False, allow_stock_damage=False, review_only=False, destination=None):
    mod_id = safe_mod_id(mod_id)
    if not spec["package_supported"] and not review_only:
        raise ValueError(spec["runtime_limit"]+"; destroyed geometry requires --review-only")
    if spec["destroyed_requirement"]["required"] and not (allow_stock_damage or review_only):
        raise ValueError("damage stays stock at runtime; use --allow-stock-damage explicitly or --review-only")
    metadata = metadata_for(candidate_path)
    if not metadata.get("license") or not metadata.get("source_url"):
        raise ValueError("candidate .asset.json must record license and source_url (local provenance accepted)")
    if metadata.get("category") != spec["category"]:
        raise ValueError("candidate category must match target")
    candidate = load_candidate(candidate_path)
    candidate, transform = fit_candidate(candidate, spec, metadata, yaw=yaw, stretch=stretch)
    # Size-limit before checking final texture budgets.
    images = []
    for img in candidate.images:
        img = img.copy()
        img.thumbnail((spec["budgets"]["max_texture_edge"],)*2)
        images.append(img)
    candidate = Candidate(candidate.mesh, images, candidate.source)
    validation = validate(candidate, spec, transform)
    if not validation["valid"]:
        raise ValueError("candidate failed validation: "+"; ".join(validation["errors"]))
    out = output_path(destination or OUT_ROOT/mod_id, mod=True)
    if out.exists() and destination is None:
        raise FileExistsError(f"refusing to overwrite package: {out}")
    folder = out/"data"/"model_overrides"/"cooked"
    gltf, images, texnames = export_runtime_gltf(candidate, folder, mod_id, spec["budgets"]["max_texture_edge"])
    # Read exported data, simulate the engine's actual default mapping, validate again.
    roundtrip = load_gltf(gltf)
    engine_mesh = dataclass_replace(roundtrip.mesh, vertices=roundtrip.mesh.vertices*[-1, -1, 1],
                                   normals=roundtrip.mesh.normals*[-1, -1, 1])
    cooked = Candidate(engine_mesh, roundtrip.images, gltf)
    final = validate(cooked, spec, transform)
    if not final["valid"]:
        raise ValueError("cooked round-trip failed: "+"; ".join(final["errors"]))
    record = {"type": "model", "class": "staticProp", "replaces": spec["selection_key"],
              "source": "cooked/"+gltf.name, "scale": 1.0, "renderOnly": True, "fallback": "stock"}
    # Never emit a fake damage override that the runtime would silently ignore.
    write_json(folder.parent/("models.review.json" if review_only else "models.json"), {"overrides": [record]})
    if not review_only:
        write_json(out/"mod.json", {"schema": "mc2-mod/1", "id": mod_id,
            "name": "Modernized "+spec["id"], "version": "1.0.0", "dependencies": []})
    report = {"review_only": review_only, "stock_damage_fallback": allow_stock_damage,
              "candidate_sha256": hashlib.sha256(Path(candidate_path).read_bytes()).hexdigest(),
              "metadata": metadata, "transform": transform, "validation": final,
              "runtime_textures": texnames, "source": str(candidate_path),
              "damage_note": spec["destroyed_requirement"]["policy"]}
    write_json(out/"target-spec.json", spec); write_json(out/"validation.json", report)
    from tools.assets.render import comparison, mesh_textures
    stock = stock_install(root)
    stock_mesh = parse_tgl(stock.read(spec["stock_tgl"])[0])
    comparison(stock_mesh, cooked.mesh, mesh_textures(stock_mesh, stock), cooked.images, out/"preview.png")
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--catalog", type=Path, default=Path("docs/assets/catalog.json"))
    p.add_argument("--root", type=Path, default=INSTALL_ROOT)
    sub = p.add_subparsers(dest="command", required=True)
    for command in ("spec", "search", "package", "rework"):
        child = sub.add_parser(command)
        child.add_argument("selection")
        child.add_argument("--state", choices=("intact", "destroyed"), default="intact")
        if command in ("spec", "search"):
            child.add_argument("--out", type=Path)
        if command == "search":
            child.add_argument("--library", type=Path, default=CANDIDATES)
        if command in ("package", "rework"):
            child.add_argument("--id", required=True)
        if command == "package":
            child.add_argument("--candidate", type=Path, required=True)
            child.add_argument("--yaw", type=float, default=0)
            child.add_argument("--stretch", action="store_true")
            child.add_argument("--allow-stock-damage", action="store_true")
            child.add_argument("--review-only", action="store_true")
        if command == "rework":
            child.add_argument("--factor", type=int, default=4)
    convert = sub.add_parser("convert-fbx")
    convert.add_argument("source", type=Path); convert.add_argument("destination", type=Path)
    convert.add_argument("--blender", type=Path, required=True)
    args = p.parse_args(argv)
    try:
        if args.command == "convert-fbx":
            from tools.assets.candidates import convert_fbx
            print(convert_fbx(args.source, args.destination, args.blender)); return 0
        data = json.loads(args.catalog.read_text(encoding="utf-8"))
        entry = select(data, args.selection)
        spec = target_spec(entry, args.state)
        if args.command in ("spec", "search"):
            value = spec if args.command == "spec" else search(spec, args.library)
            if args.out:
                write_json(args.out, value)
                print(args.out)
            else:
                print(json.dumps(value, indent=2))
        elif args.command == "package":
            print(package(entry, spec, args.candidate, args.id, root=args.root, yaw=args.yaw,
                          stretch=args.stretch, allow_stock_damage=args.allow_stock_damage, review_only=args.review_only))
        else:
            from tools.assets.rework import rework
            print(rework(entry, args.id, root=args.root, factor=args.factor))
        return 0
    except (ValueError, OSError, KeyError, IndexError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
