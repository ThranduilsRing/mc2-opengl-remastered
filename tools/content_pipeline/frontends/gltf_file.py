"""Any glTF/GLB (Blender headless exports, the sci-fi building kit, catalog
candidates, FBX converted by the documented Blender adapter, BT2018 GLBs) -> IR.

The source declares its frame and units, either in a sibling <model>.asset.json
or in the recipe: {"frame": "gltf"|"mc2_runtime", "wu_per_unit": 1, "yaw_degrees": 0,
"license": ..., "source_url": ...}. Per-part material classes come from the glTF
material extras (mc2_material_class) or an explicit recipe map {material name:
class | {class, metallic, roughness, ...}}; never from colour.
"""
from __future__ import annotations

import json
import tempfile
from pathlib import Path

import numpy as np

from .. import gltf_io, ir

SIDE_KEYS = ("frame", "wu_per_unit", "yaw_degrees", "license", "source_url", "provider")


def sidecar(path):
    side = Path(path).with_suffix(".asset.json")
    meta = json.loads(side.read_text(encoding="utf-8")) if side.is_file() else {}
    # The catalog sidecar spells the engine frame as up_axis "-y" (inverse axis0).
    if "frame" not in meta and meta.get("up_axis") == "-y":
        meta["frame"] = "mc2_runtime"
    return meta


def _apply_classes(materials, classes):
    for i, m in enumerate(materials):
        spec = classes.get(m.name) or classes.get(str(i))
        if spec is None:
            continue
        spec = {"class": spec} if isinstance(spec, str) else dict(spec)
        cls = spec.pop("class")
        keep = {k: getattr(m, k) for k in ("alpha_mode", "alpha_cutoff", "double_sided", "base_color_policy",
                                          "base_color", "base_color_factor", "normal", "orm", "emissive",
                                          "emissive_factor", "normal_strength", "ao_strength")}
        keep.update({k: v for k, v in spec.items() if k in keep or k in ("metallic", "roughness", "review_note",
                                                                          "detail_uv_scale")})
        materials[i] = ir.Material.for_class(m.name, cls, reviewed=True, **keep)
    return materials


def read_scene(path, meta, extract_dir, classes=None, allow_skin=False):
    classes = classes or {}

    def class_of(name, extras):
        spec = classes.get(name)
        return spec if isinstance(spec, str) else (spec or {}).get("class")
    scene, materials, info = gltf_io.read_gltf(
        path, class_of=class_of, frame=meta.get("frame", "gltf"), wu_per_unit=float(meta.get("wu_per_unit", 1.0)),
        yaw_degrees=float(meta.get("yaw_degrees", 0.0)), extract_dir=extract_dir, allow_skin=allow_skin,
        texture_roots=tuple(meta.get("texture_roots", ())), unclassified=meta.get("unclassified", "error"))
    frame = gltf_io.frame_matrix(meta.get("frame", "gltf"), float(meta.get("wu_per_unit", 1)),
                                 float(meta.get("yaw_degrees", 0)))
    for node in scene.nodes:
        props = node.extras
        if props.get("mc2_light_schema") != "mc2-building-light/1":
            continue
        position = frame @ np.array([*props["mc2_light_position_gltf_m"], 1.0])
        direction = frame[:3, :3] @ np.asarray(props["mc2_light_direction_gltf"], float)
        length = np.linalg.norm(direction)
        radius = float(props["mc2_light_range_m"]) * float(np.linalg.norm(frame[:3, 0]))
        if not np.isfinite(position).all() or not np.isfinite(length) or length < 1e-8 or not 0 < radius < 1e6:
            raise ValueError(f"invalid light socket {node.name}")
        props.update(position_tg_wu=position[:3].tolist(), direction_tg=(direction / length).tolist(), range_wu=radius)
    return scene, _apply_classes(materials, classes or {}), info


def merge_materials(base, extra, scene):
    """Append extra's slots to base and renumber scene primitives."""
    offset = len(base)
    for mesh in scene.meshes:
        for p in mesh.primitives:
            p.material += offset
    return base + extra


def to_ir(asset_id, replaces, intact, *, kind="building", lods=(), destroyed=None, shadows=None,
          conditions=None, classes=None, meta=None, work_dir=None, allow_skin=False, sockets=(), parts=(),
          base_color=None, texel_scale=1, repair_inward_hull=False):
    """intact/destroyed/LOD/condition entries are GLB paths (destroyed may also be a
    ready (Scene, materials) pair from another frontend, e.g. stock damage)."""
    work = Path(work_dir or tempfile.mkdtemp(prefix=f"cp-{asset_id}-"))
    meta = {**sidecar(intact), **(meta or {})}
    provenance = [ir.Source.of("geometry", intact, license=meta.get("license", "unrecorded"),
                               provider=meta.get("provider", ""), url=meta.get("source_url", ""))]
    scene, materials, info = read_scene(intact, meta, work / "images", classes, allow_skin)
    states = {"intact": [ir.Lod(scene)]}

    def load(entry, role):
        nonlocal materials
        if isinstance(entry, tuple):
            sc, mats = entry
        else:
            provenance.append(ir.Source.of(role, entry))
            sc, mats, _ = read_scene(entry, {**meta, **sidecar(entry)}, work / "images", classes, allow_skin)
        materials = merge_materials(materials, mats, sc)
        return sc

    for path, distance in lods:
        states["intact"].append(ir.Lod(load(path, "geometry_lod"), float(distance)))
    def damage_chain(entry, role):
        spec = entry if isinstance(entry, dict) else {"intact": entry}
        result = [ir.Lod(load(spec["intact"], role))]
        result += [ir.Lod(load(p, role + "_lod"), float(d)) for p, d in spec.get("lods", [])]
        return result

    if destroyed is not None:
        states["destroyed"] = damage_chain(destroyed, "geometry_destroyed")
    shadow_scenes = {st: load(p, f"shadow_{st}") for st, p in (shadows or {}).items()}
    cond = {}
    for name, spec in (conditions or {}).items():
        cond[name] = {"intact": [ir.Lod(load(spec["intact"], f"geometry_{name}"))]}
        for path, distance in spec.get("lods", []):
            cond[name]["intact"].append(ir.Lod(load(path, f"geometry_{name}_lod"), float(distance)))
        if spec.get("destroyed") is not None:
            cond[name]["destroyed"] = damage_chain(spec["destroyed"], f"geometry_{name}_destroyed")
    # An authoring tool may name a display texture (e.g. AO pre-multiplied for a diffuse-only path) in
    # the GLB; the recipe points the slot at the pure base colour instead. AO belongs in ORM.R.
    for name, path in (base_color or {}).items():
        slot = next((m for m in materials if m.name == name), None)
        if slot is None:
            raise ValueError(f"{asset_id}: base_color override names unknown material {name!r}")
        slot.base_color = ir.TextureRef.of(path, "base_color")
        slot.base_color_policy = "authored"
    for m in materials:
        for ref in (m.base_color, m.normal, m.orm, m.emissive):
            if ref is not None:
                provenance.append(ir.Source(ref.role, ref.path, ref.sha256, meta.get("license", "unrecorded")))
    skeleton = None
    if info["skins"]:
        skeleton = ir.SkeletonRef(ir.Source.of("skeleton", intact), info["joints"], info["clips"])
    sockets = list(sockets)
    for node in scene.nodes:
        if node.extras.get("mc2_light_schema") == "mc2-building-light/1":
            props = node.extras
            sockets.append(ir.Socket(node.name, "light", node.name, np.eye(4), dict(props)))
    return ir.Asset(id=asset_id, kind=kind, replaces=replaces, materials=materials, states=states,
                    shadows=shadow_scenes, conditions=cond, skeleton=skeleton, provenance=provenance,
                    frontend={"name": "gltf_file", "source": str(intact), "meta": meta, "texel_scale": texel_scale,
                              "repair_inward_hull": repair_inward_hull,
                              "unsupported_extensions_ignored": info["extensionsUsed"]},
                    sockets=list(sockets), parts=list(parts)).check()
