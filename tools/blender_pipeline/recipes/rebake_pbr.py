"""
rebake_pbr.py — headless Blender recipe: UV repack + PBR texture rebake.

BLENDER-ASSET-PIPELINE-1 (EXPERIMENTAL). Joins the asset into one mesh, packs
a fresh UV atlas, and Cycles-bakes albedo / normal / ORM at the target
resolution. PNGs land in --tex-dir for the .mcasset sidecar path
(MC2_BUILDING_PBR); the output GLB gets the baked baseColor + normal +
metallicRoughness wired into a single Principled material.

CAUTION (capability-survey #6 / docs/asset-pipeline.md §6): in-game static-prop
normal/ORM was ruled "on ~= off" under MC2's current lighting model. Bakes are
still useful for the asset viewer / future lighting work, but do not expect an
in-game payoff for props until the lighting model changes.

Prerequisite: the input GLB must have resolvable textures (embedded, or
external URIs next to the GLB) — baking from missing images produces garbage.

Run (normally via batch_driver.py):
  blender --background --factory-startup --python rebake_pbr.py -- \
      --in=src.glb --out=dst.glb --tex-dir=tex/ --name=quonset --res=1024

Args:
  --in=PATH       input GLB (required)
  --out=PATH      output GLB (required)
  --tex-dir=DIR   directory for baked PNGs (required)
  --name=BASE     texture base name -> <BASE>_albedo.png / _normal.png / _orm.png
  --res=N         bake resolution (default 1024)
  --samples=N     Cycles samples (default 16 — bakes are mostly noise-free)
  --margin=N      bake margin px (default 4)
  --uv=MODE       smart (default: Smart UV Project into a new atlas) | keep
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _recipe_util import (  # noqa: E402
    arg, emit_stats, export_glb, import_glb, mesh_objects, reset_scene,
    script_args,
)


def _join_meshes():
    """Join all mesh objects into one; returns the joined object."""
    import bpy
    meshes = mesh_objects()
    for o in meshes:
        o.data = o.data.copy()  # make single-user so join can't corrupt shares
    with bpy.context.temp_override(active_object=meshes[0],
                                   selected_objects=meshes,
                                   selected_editable_objects=meshes):
        if len(meshes) > 1:
            bpy.ops.object.join()
    return meshes[0]


def _new_bake_uv(obj, mode):
    """Add the bake target UV layer. Original layer stays the render source."""
    import bpy
    me = obj.data
    # Blender 5.x glTF import stores custom normals as a 'custom_normal'
    # float2 corner attribute that LEAKS into me.uv_layers — if left in place
    # it hijacks the active-UV bookkeeping and the export drops TEXCOORD_0.
    # We re-derive normals anyway (subdiv/decimate/bake), so drop it.
    stale = me.attributes.get("custom_normal")
    if stale is not None:
        me.attributes.remove(stale)

    if mode == "keep" and me.uv_layers:
        return me.uv_layers.active.name
    original_name = me.uv_layers.active.name if me.uv_layers.active else None
    me.uv_layers.new(name="BakeUV")
    # UVLayer wrappers are index-based and go stale across collection changes
    # and edit-mode round-trips — ALWAYS re-fetch by name, never hold a ref.
    if me.uv_layers.get("BakeUV") is None:
        print("[recipe:error] could not create BakeUV layer", flush=True)
        raise SystemExit(2)
    me.uv_layers.active = me.uv_layers["BakeUV"]   # bake TARGET = active layer
    if original_name:
        me.uv_layers[original_name].active_render = True  # bake SOURCE sampling
    with bpy.context.temp_override(active_object=obj, selected_objects=[obj],
                                   selected_editable_objects=[obj]):
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=1.15192, island_margin=0.02)
        bpy.ops.object.mode_set(mode="OBJECT")
    obj.data.uv_layers.active = obj.data.uv_layers["BakeUV"]  # re-assert post-edit
    return "BakeUV"


def _add_target_node(mat, image, uv_name):
    """Give each material an active image-texture node pointing at the bake
    image, sampling the bake UV layer."""
    nt = mat.node_tree
    uv_node = nt.nodes.new("ShaderNodeUVMap")
    uv_node.uv_map = uv_name
    tex_node = nt.nodes.new("ShaderNodeTexImage")
    tex_node.image = image
    nt.links.new(uv_node.outputs["UV"], tex_node.inputs["Vector"])
    nt.nodes.active = tex_node
    return tex_node


def _bake(obj, mats, image, uv_name, bake_type, **bake_kwargs):
    import bpy
    if uv_name in obj.data.uv_layers:
        obj.data.uv_layers.active = obj.data.uv_layers[uv_name]  # bake target
    nodes = [_add_target_node(m, image, uv_name) for m in mats]
    with bpy.context.temp_override(active_object=obj, selected_objects=[obj],
                                   selected_editable_objects=[obj]):
        bpy.ops.object.bake(type=bake_type, **bake_kwargs)
    for m, n in zip(mats, nodes):
        m.node_tree.nodes.remove(n)


def _avg_metallic(mats):
    total, count = 0.0, 0
    for m in mats:
        for node in m.node_tree.nodes:
            if node.type == "BSDF_PRINCIPLED":
                sock = node.inputs.get("Metallic")
                if sock is not None and not sock.is_linked:
                    total += float(sock.default_value)
                    count += 1
    return total / count if count else 0.0


def _compose_orm(ao_img, rough_img, metallic, res, out_path):
    """ORM = AO in R, roughness in G, metallic (constant) in B."""
    import bpy
    import numpy as np
    n = res * res
    ao = np.empty(n * 4, dtype=np.float32)
    ro = np.empty(n * 4, dtype=np.float32)
    ao_img.pixels.foreach_get(ao)
    rough_img.pixels.foreach_get(ro)
    orm = np.empty(n * 4, dtype=np.float32)
    orm[0::4] = ao[0::4]
    orm[1::4] = ro[1::4]
    orm[2::4] = metallic
    orm[3::4] = 1.0
    img = bpy.data.images.new("mc2_orm", width=res, height=res, alpha=False)
    img.colorspace_settings.name = "Non-Color"
    img.pixels.foreach_set(orm)
    img.filepath_raw = out_path
    img.file_format = "PNG"
    img.save()
    return img


def main():
    import bpy

    args = script_args()
    in_glb = arg(args, "in", required=True)
    out_glb = arg(args, "out", required=True)
    tex_dir = arg(args, "tex-dir", required=True)
    name = arg(args, "name", "asset")
    res = arg(args, "res", 1024, int)
    samples = arg(args, "samples", 16, int)
    margin = arg(args, "margin", 4, int)
    uv_mode = arg(args, "uv", "smart")

    reset_scene()
    import_glb(in_glb)
    if not mesh_objects():
        print("[recipe:error] no mesh objects in input GLB", flush=True)
        raise SystemExit(2)

    os.makedirs(tex_dir, exist_ok=True)
    obj = _join_meshes()
    uv_name = _new_bake_uv(obj, uv_mode)

    mats = [s.material for s in obj.material_slots
            if s.material and s.material.node_tree is not None]
    if not mats:
        print("[recipe:error] no node-based materials to bake from", flush=True)
        raise SystemExit(2)

    missing = [i.name for i in bpy.data.images
               if i.source == "FILE" and not i.has_data]
    if missing:
        print(f"[recipe:warn] images missing on disk (bake will be flat): {missing}",
              flush=True)

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.render.bake.margin = margin

    def new_img(suffix, srgb):
        img = bpy.data.images.new(f"mc2_{suffix}", width=res, height=res, alpha=False)
        img.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
        return img

    albedo = new_img("albedo", srgb=True)
    normal = new_img("normal", srgb=False)
    rough = new_img("rough", srgb=False)
    ao = new_img("ao", srgb=False)

    _bake(obj, mats, albedo, uv_name, "DIFFUSE",
          pass_filter={"COLOR"}, use_clear=True)
    _bake(obj, mats, normal, uv_name, "NORMAL", use_clear=True)
    _bake(obj, mats, rough, uv_name, "ROUGHNESS", use_clear=True)
    _bake(obj, mats, ao, uv_name, "AO", use_clear=True)

    paths = {
        "albedo": os.path.join(tex_dir, f"{name}_albedo.png"),
        "normal": os.path.join(tex_dir, f"{name}_normal.png"),
        "orm": os.path.join(tex_dir, f"{name}_orm.png"),
    }
    for img, key in ((albedo, "albedo"), (normal, "normal")):
        img.filepath_raw = paths[key]
        img.file_format = "PNG"
        img.save()
    metallic = _avg_metallic(mats)
    _compose_orm(ao, rough, metallic, res, paths["orm"])

    # Rebuild a single baked material and make BakeUV the only UV layer so it
    # exports as TEXCOORD_0.
    me = obj.data
    for layer in [l for l in me.uv_layers if l.name != uv_name]:
        me.uv_layers.remove(layer)

    baked = bpy.data.materials.new(f"{name}_baked")
    if baked.node_tree is None:
        baked.use_nodes = True  # pre-Blender-6.0 compat (deprecated no-op in 5.x)
    nt = baked.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")

    alb_node = nt.nodes.new("ShaderNodeTexImage")
    alb_node.image = albedo
    nt.links.new(alb_node.outputs["Color"], bsdf.inputs["Base Color"])

    nrm_tex = nt.nodes.new("ShaderNodeTexImage")
    nrm_tex.image = normal
    nrm_map = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(nrm_tex.outputs["Color"], nrm_map.inputs["Color"])
    nt.links.new(nrm_map.outputs["Normal"], bsdf.inputs["Normal"])

    orm_img = bpy.data.images.get("mc2_orm")
    orm_node = nt.nodes.new("ShaderNodeTexImage")
    orm_node.image = orm_img
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    nt.links.new(orm_node.outputs["Color"], sep.inputs["Color"])
    nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])

    me.materials.clear()
    me.materials.append(baked)

    emit_stats("rebake_pbr", {
        "name": name, "res": res, "uv_mode": uv_mode, "uv_layer": uv_name,
        "materials_baked": len(mats), "metallic_const": round(metallic, 4),
        "missing_images": missing, "textures": {k: os.path.basename(v)
                                                for k, v in paths.items()},
    })

    os.makedirs(os.path.dirname(os.path.abspath(out_glb)), exist_ok=True)
    export_glb(out_glb)
    print(f"[recipe] wrote {out_glb}", flush=True)


if __name__ == "__main__":
    main()
