"""The one validated cooker: IR Asset(s) -> mounted-mod package.

Layout of a package (a mod directory; the engine mounts it through the normal
mod VFS order, so a missing package means stock):
  mod.json
  data/model_overrides/models.json              engine schema, materials v2
  data/model_overrides/<asset>/<cond>_<state>_lod<N>.glb
  data/model_overrides/<asset>/<cond>_<state>_shadow.glb
  data/model_overrides/<asset>/detail/<slot>_{normal,orm}.ktx2
  data/tgl/<tier>/<texture>.{tga,ktx2}           authored base colour, real tiers
  content/<asset>.mc2ir.json                    the IR (reproducibility, not loaded)
  content/report.json                           provenance, validation, cache keys
Stock-policy base colour is never copied: the GLB names the stock texture, so
the VFS (and any texture mod) supplies the same bytes the stock prop uses.
"""
from __future__ import annotations

import types
import json
import re
import shutil
import copy
from pathlib import Path

import numpy as np

from . import cache, gltf_io, ir, texcook, validate

PACKAGE_VERSION = "mc2-content-package/1"
COOKER_VERSION = "mc2-content-cooker/9"  # glass panes carry stable UV1 night-window IDs
GEOMETRY_CACHE = cache.CacheKind("model-", ".glb")
SKIN_CACHE = cache.CacheKind("skin-", ".glb")
DETAIL_EDGE = 2048       # largest per-object map edge; each map keeps its own power-of-two extent up to this
SAFE = re.compile(r"[a-z0-9][a-z0-9_-]{0,62}")


def _safe(name):
    if not SAFE.fullmatch(name):
        raise ValueError(f"unsafe package/asset id {name!r} (lowercase letters, digits, _ -)")
    return name


def _write(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    tmp.replace(path)


def _texture_name(asset_id, slot, material):
    alpha = material.alpha_mode != "OPAQUE"
    return f"{'a_' if alpha else ''}cp_{asset_id}_{slot}.tga"


def stock_texture_name(material):
    """A stock-policy slot names the stock image; the importer resolves it by stem."""
    return Path(material.base_color.path).name.lower() if material.base_color else None


def _detail(material: ir.Material, which: str, report: dict, compress: bool):
    """Per-object map at its own extent (power of two, at most DETAIL_EDGE); resampled only when it must be."""
    ref = getattr(material, which)
    rgba = texcook.load_rgba(ref.path)
    source = ref.sha256          # key on the resolved source bytes
    rgba2 = texcook.resize_pow2(rgba, DETAIL_EDGE)
    if rgba2.shape != rgba.shape:
        report.setdefault("resampled", []).append(f"{material.name}:{which}")
    return texcook.cook_ktx2(rgba2, "base_color" if which == "albedo" else which, compress=compress,
                             source_sha256=source if rgba2.shape == rgba.shape else None)


ENGINE_NODE_PREFIXES = ("spotlight_", "litwin", "_pab", "los_")   # tgl_types.cpp / msl_load.cpp name rules


def isolate_engine_nodes(asset: ir.Asset):
    """Spotlight cones, lit windows, passability and LOS helpers are engine-special nodes driven by
    the legacy light tags; authored material inputs must not reach them. Their primitives move to a
    legacy_prelit twin of the slot (same base colour), so the stock response is kept."""
    twins = {}
    for _, _, lods in asset.variants():
        for lod in lods:
            scene = lod.scene
            for node in scene.nodes:
                if node.mesh is None or not node.name.lower().startswith(ENGINE_NODE_PREFIXES):
                    continue
                for prim in scene.meshes[node.mesh].primitives:
                    m = asset.materials[prim.material]
                    if not m.authored():
                        continue
                    if prim.material not in twins:
                        twins[prim.material] = len(asset.materials)
                        asset.materials.append(ir.Material(name=f"{m.name}_legacy", base_color=m.base_color,
                                                           base_color_policy=m.base_color_policy,
                                                           alpha_mode=m.alpha_mode, alpha_cutoff=m.alpha_cutoff,
                                                           double_sided=m.double_sided))
                    prim.material = twins[prim.material]
    return twins


def cook_base_colors(asset, out, entry, compress, runtime_names=None):
    # Base colour: stock slots name stock textures; authored slots get real tiers.
    aid = _safe(asset.id)
    names = {}
    shared = {}
    for slot, m in enumerate(asset.materials):
        if m.base_color is None and m.base_color_policy == "stock":
            continue
        if m.base_color is not None and (m.base_color_policy == "stock" or m.base_color.passthrough):
            names[slot] = stock_texture_name(m)
            continue
        key = (m.base_color.sha256 if m.base_color else None, tuple(m.base_color_factor), m.alpha_mode, m.alpha_cutoff)
        if runtime_names is None and key in shared:
            names[slot] = shared[key]
            continue
        rgba = texcook.load_rgba(m.base_color.path) if m.base_color else np.full((1, 1, 4), 255, np.uint8)
        if tuple(m.base_color_factor) != (1.0, 1.0, 1.0, 1.0):
            f = np.asarray(m.base_color_factor, dtype=np.float64)
            lin = texcook.srgb_to_linear(rgba[..., :3] / 255.0) * f[:3]
            rgba = np.concatenate([texcook.linear_to_srgb(lin) * 255, rgba[..., 3:4] * f[3]], axis=2)
            rgba = np.clip(np.rint(rgba), 0, 255).astype(np.uint8)
        if m.alpha_mode == "OPAQUE":
            rgba = rgba.copy()
            rgba[..., 3] = 255
        name = (runtime_names or {}).get(slot) or _texture_name(aid, slot, m)
        names[slot] = name
        shared[key] = name
        cutoff = m.alpha_cutoff if m.alpha_mode == "MASK" else None
        for tier, img in texcook.base_color_tiers(rgba, texel_scale=int(asset.frontend.get("texel_scale", 1))).items():
            tdir = out / "data" / "tgl" / str(tier)
            texcook.write_tga(tdir / name, img)
            _write(tdir / Path(name).with_suffix(".ktx2"),
                   texcook.cook_ktx2(img, "base_color", alpha_cutoff=cutoff, compress=compress,
                       source_sha256=ir.sha256_bytes((bytes.fromhex(m.base_color.sha256) if m.base_color else b"") + img.tobytes())))
            entry["files"].setdefault("base_color", []).append(f"data/tgl/{tier}/{name}")

    return names


def cook_asset(asset: ir.Asset, out: Path, *, compress=True, max_triangles=None, report=None):
    """Cook one asset into the package at out; returns its models.json record."""
    asset.check()
    if asset.skeleton is not None:
        # A rigid-skin package retains its source skeleton, vertex order and clips.
        # Native loading lowers that selected state through the same IR cooker as stock/props;
        # static transform baking here would change the pose contract owned by the mech lane.
        aid = _safe(asset.id)
        source = Path(asset.frontend["runtime_source"])
        key = cache.cache_key(COOKER_VERSION, {"kind": "rigid_skin"}, [s.sha256 for s in asset.provenance])
        data = cache.cached(cache.DEFAULT_DIR, SKIN_CACHE, key, source.read_bytes)
        # Preserve the original image names so the unchanged rigid-skin importer
        # resolves its atlas through the normal texture VFS. All tiers use the common cook.
        doc, _, _ = gltf_io.load_document(source)
        runtime_names = {}
        for slot, material in enumerate(doc.get("materials", [])):
            texture = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
            if texture is None:
                continue
            image_index = doc["textures"][texture["index"]]["source"]
            image = doc["images"][image_index]
            raw = image.get("uri") or image.get("name") or f"*{image_index}"
            stem = re.sub(r"[^a-z0-9_-]", "_", Path(raw.replace("\\", "/")).stem.lower())[:251]
            alpha = material.get("alphaMode", "OPAQUE") != "OPAQUE"
            runtime_names[slot] = ("a_" if alpha and not stem.startswith("a_") else "") + stem + ".tga"
        cook_base_colors(asset, out, {"files": {}}, compress, runtime_names)
        _write(out / "data/tgl" / f"{aid}.glb", data)
        side = asset.frontend.get("runtime_package")
        if side:
            _write(out / "data/tgl" / f"{aid}.package.json", Path(side).read_bytes())
        ir.save(asset, out / "content" / f"{aid}.mc2ir.json")
        if report is not None:
            report.setdefault("assets", []).append({"id": aid, "frontend": asset.frontend,
                "ir_fingerprint": ir.fingerprint(asset), "cache_key": f"{key:016x}",
                "runtime_import": aid, "skeleton": asset.skeleton.joints, "clips": asset.skeleton.clips})
        return None
    isolate_engine_nodes(asset)
    hull = {}
    if asset.kind == "building" and asset.frontend.get("repair_inward_hull"):
        for cond, state, lods in asset.variants():
            for i, lod in enumerate(lods):
                faces = ir.inward_hull_faces(lod.scene)
                if faces:
                    key = f"{cond}_{state}_lod{i}"
                    count = sum(len(t) for _, _, t in faces)
                    # Repair only authored intact chains: a destroyed chain may be stock rubble whose
                    # look must stay byte-for-byte stock.
                    repair = asset.frontend.get("repair_inward_hull") and state == "intact"
                    hull[key] = {"repaired": ir.flip_faces(lod.scene, faces)} if repair else {"unrepaired": count}
    aid = _safe(asset.id)
    mo = out / "data" / "model_overrides"
    folder = mo / aid
    report = report if report is not None else {}
    entry = {"id": aid, "replaces": asset.replaces, "ir_fingerprint": ir.fingerprint(asset),
             "frontend": asset.frontend, "provenance": [s.__dict__ for s in asset.provenance],
             "files": {}, "validation": {}}
    if hull:
        # Outer-skin triangles facing into the building are culled from outside (a see-through wall).
        entry["inward_hull_faces"] = hull

    names = cook_base_colors(asset, out, entry, compress)

    # Work-directory image paths are deliberately absent from content identity. Resolved
    # source digests, geometry, material policy and cooker version determine cache entries.
    def identity(value):
        if isinstance(value, dict):
            return {k: identity(v) for k, v in value.items() if k not in ("path", "frontend")}
        if isinstance(value, list):
            return [identity(v) for v in value]
        return value

    content = ir.sha256_bytes(json.dumps(identity(ir.to_json(asset)), sort_keys=True).encode())

    def glb(scene, stem, state):
        path = folder / f"{stem}.glb"
        key = cache.cache_key(COOKER_VERSION, {"chain": stem, "names": names}, [content])
        data = cache.read_entry(cache.DEFAULT_DIR, GEOMETRY_CACHE, key)
        hit = data is not None
        if data is None:
            data = gltf_io.write_runtime_glb(scene, asset.materials, names)
            cache.write_entry(cache.DEFAULT_DIR, GEOMETRY_CACHE, key, data)
        entry.setdefault("geometry_cache", {})[stem] = {"key": f"{key:016x}", "hit": hit}
        _write(path, data)
        issues = validate.validate_runtime_glb(path, max_triangles=max_triangles)
        entry["validation"][stem] = [list(i) for i in issues]
        if validate.errors(issues):
            raise ValueError(f"{aid}/{stem}: " + "; ".join(validate.errors(issues)))
        return f"{aid}/{stem}.glb"

    def chain(cond, state, lods):
        rec = {"source": glb(lods[0].scene, f"{cond}_{state}_lod0", state)}
        lights = [dict(n.extras, name=n.name) for n in lods[0].scene.nodes
                  if n.extras.get("mc2_light_schema") == "mc2-building-light/1"]
        if lights:
            rec["lightSockets"] = lights
        if len(lods) > 1:
            rec["lods"] = [{"lod": i, "source": glb(l.scene, f"{cond}_{state}_lod{i}", state),
                            "distance": float(l.distance)} for i, l in enumerate(lods) if i > 0]
        return rec

    record = {"type": "model", "replaces": asset.replaces, "renderOnly": True, "fallback": "stock", "scale": 1.0}
    record.update(chain("pristine", "intact", asset.states["intact"]))
    if "destroyed" in asset.states:
        record["destroyed"] = chain("pristine", "destroyed", asset.states["destroyed"])
    if asset.shadows:
        # Vulkan static shadows cast from the render shape itself (intact or destroyed override), so a
        # separate stand-in would be ignored; never emit data the runtime silently drops.
        entry["shadow_standins_not_emitted"] = sorted(asset.shadows)
    if asset.conditions:
        record["conditions"] = {}
        for cond, states in asset.conditions.items():
            c = chain(cond, "intact", states["intact"])
            if "destroyed" in states:
                c["destroyed"] = chain(cond, "destroyed", states["destroyed"])
            record["conditions"][cond] = c

    slots = []
    detail_paths = {}
    for slot, m in enumerate(asset.materials):
        if not m.authored():
            continue
        # Stock-policy albedo keeps the stock coverage policy (soft fringe); authored
        # MASK is a depth-writing cutout; OPAQUE never alpha-tests.
        alpha = "STOCK" if m.base_color_policy == "stock" else m.alpha_mode
        row = {"slot": slot, "reviewed": m.reviewed, "class": m.material_class, "alphaMode": alpha,
               "normalStrength": m.normal_strength, "aoStrength": m.ao_strength,
               "roughness": m.roughness, "metallic": m.metallic, "uvScale": list(m.detail_uv_scale)}
        if alpha == "MASK":
            row["alphaCutoff"] = m.alpha_cutoff
        if m.base_color is not None and m.base_color_policy != "stock" and not m.base_color.passthrough and alpha == "OPAQUE":
            rel = f"{aid}/detail/{slot}_albedo.ktx2"
            _write(mo / rel, _detail(types.SimpleNamespace(name=m.name, albedo=m.base_color), "albedo", entry, compress))
            row["albedo"] = rel
        for which in ("normal", "orm"):
            if getattr(m, which) is not None:
                key = (which, getattr(m, which).sha256)
                if key not in detail_paths:
                    rel = f"{aid}/detail/{slot}_{which}.ktx2"
                    _write(mo / rel, _detail(m, which, entry, compress))
                    detail_paths[key] = rel
                row[which] = detail_paths[key]
        slots.append(row)
    if slots:
        record["materials"] = {"version": 2, "slots": slots}
    ir.save(asset, out / "content" / f"{aid}.mc2ir.json")
    report.setdefault("assets", []).append(entry)
    return record


def build_package(assets: list[ir.Asset], out, *, mod_id, name=None, description="", compress=True,
                  max_triangles=None, overwrite=False, aliases=None):
    out = Path(out)
    _safe(mod_id)
    if out.exists():
        if not overwrite:
            raise FileExistsError(f"refusing to overwrite package {out}")
        shutil.rmtree(out)
    report = {"version": PACKAGE_VERSION, "cooker": COOKER_VERSION, "contract": texcook.CONTRACT_VERSION, "mod": mod_id}
    records = [cook_asset(a, out, compress=compress, max_triangles=max_triangles, report=report) for a in assets]
    records = [r for r in records if r is not None]
    by_key = {r["replaces"].lower(): r for r in records}
    for replaces, target in (aliases or {}).items():
        if target.lower() not in by_key:
            raise ValueError(f"alias {replaces}: missing target {target}")
        record = copy.deepcopy(by_key[target.lower()])
        record["replaces"] = replaces
        records.append(record)
    _write(out / "data" / "model_overrides" / "models.json",
           (json.dumps({"overrides": records}, indent=2) + "\n").encode("utf-8"))
    _write(out / "mod.json", (json.dumps({"id": mod_id, "name": name or mod_id, "version": "1.0.0",
                                          "description": description}, indent=2) + "\n").encode("utf-8"))
    problems = check_package(out)
    report["package_check"] = problems
    _write(out / "content" / "report.json", (json.dumps(report, indent=2, default=str) + "\n").encode("utf-8"))
    if problems:
        raise ValueError("package check failed: " + "; ".join(problems))
    return out


def check_package(out) -> list[str]:
    """The rules mclib/model_override_registry.cpp enforces, plus file presence."""
    out = Path(out)
    mo = out / "data" / "model_overrides"
    problems = []
    try:
        root = json.loads((mo / "models.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return [f"models.json unreadable: {e}"]
    if not isinstance(root, dict) or not isinstance(root.get("overrides"), list):
        return ["models.json needs an 'overrides' array (the engine drops bare lists)"]

    def src_ok(where, rel):
        if not isinstance(rel, str) or not rel.lower().endswith((".glb", ".gltf")) or rel.startswith(("/", "\\")) \
                or ".." in rel or ":" in rel:
            problems.append(f"{where}: unsafe source {rel!r}")
        elif not (mo / rel).is_file():
            problems.append(f"{where}: missing {rel}")

    def chain_ok(where, rec):
        src_ok(where, rec.get("source"))
        last = 0
        for lod in rec.get("lods", []):
            if lod.get("lod", 0) <= last:
                problems.append(f"{where}: LODs must ascend from 1")
            last = lod.get("lod", 0)
            src_ok(f"{where} lod{last}", lod.get("source"))

    for e in root["overrides"]:
        key = e.get("replaces", "<none>")
        if e.get("type") != "model" or e.get("renderOnly") is not True or e.get("fallback") != "stock" \
                or float(e.get("scale", 1.0)) != 1.0:
            problems.append(f"{key}: type/renderOnly/fallback/scale violate the override MVP rules")
        chain_ok(key, e)
        if "destroyed" in e:
            chain_ok(f"{key} destroyed", e["destroyed"])
        for st, rel in e.get("shadow", {}).items():
            if st not in ir.STATES:
                problems.append(f"{key}: unknown shadow state {st}")
            src_ok(f"{key} shadow {st}", rel)
        for cond, c in e.get("conditions", {}).items():
            if cond not in ir.CONDITIONS or cond == ir.DEFAULT_CONDITION:
                problems.append(f"{key}: unknown condition {cond}")
            chain_ok(f"{key} {cond}", c)
            if "destroyed" in c:
                chain_ok(f"{key} {cond} destroyed", c["destroyed"])
        mats = e.get("materials")
        if mats is not None:
            if mats.get("version") not in (1, 2) or not isinstance(mats.get("slots"), list):
                problems.append(f"{key}: materials need version 1|2 and slots")
                continue
            for row in mats["slots"]:
                if row.get("class") not in ir.MATERIAL_CLASSES[1:] or row.get("reviewed") is not True:
                    problems.append(f"{key}: slot {row.get('slot')} needs a reviewed authored class")
                if row.get("class") not in ir.METALLIC_CLASSES and row.get("metallic", 0) != 0:
                    problems.append(f"{key}: slot {row.get('slot')} class cannot be metallic")
                for which in ("normal", "orm", "albedo"):
                    rel = row.get(which)
                    if rel is None:
                        continue
                    if not rel.endswith((".ktx2", ".tga")) or not (mo / rel).is_file():
                        problems.append(f"{key}: slot {row.get('slot')} {which} missing/unsupported {rel}")
                    elif rel.endswith(".ktx2"):
                        vk, w, h, levels = texcook.read_ktx2((mo / rel).read_bytes())
                        want = texcook.ROLES["base_color" if which == "albedo" else which].vk_format
                        if vk not in (want, texcook.VK_RGBA8_UNORM) or not all(0 < e <= DETAIL_EDGE and e & (e - 1) == 0 for e in (w, h)):
                            problems.append(f"{key}: {rel} is vkFormat {vk} {w}x{h}, contract {want}, power-of-two edge <= {DETAIL_EDGE}")
    return problems
