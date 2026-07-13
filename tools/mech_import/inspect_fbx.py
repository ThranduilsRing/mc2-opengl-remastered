"""Headless Blender: dump objects/materials/images/UVs of an FBX or GLB.
Run via blender_runner. Recon only — mutates nothing.
    blender --background --python inspect_fbx.py -- --src=<path>
"""
import bpy, sys

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
args = dict(a.lstrip("-").split("=", 1) for a in argv)
SRC = args["src"]

bpy.ops.wm.read_factory_settings(use_empty=True)
low = SRC.lower()
if low.endswith(".fbx"):
    bpy.ops.import_scene.fbx(filepath=SRC)
elif low.endswith((".glb", ".gltf")):
    bpy.ops.import_scene.gltf(filepath=SRC)
elif low.endswith(".obj"):
    bpy.ops.wm.obj_import(filepath=SRC)

objs = list(bpy.context.scene.objects)
meshes = [o for o in objs if o.type == "MESH"]
arms = [o for o in objs if o.type == "ARMATURE"]
print(f"[INSPECT] objs={len(objs)} meshes={len(meshes)} armatures={len(arms)}", flush=True)

print("[INSPECT] --- mesh objects (name | tris | uv_layers | material_slots) ---", flush=True)
for o in meshes:
    o.data.calc_loop_triangles()
    uvs = [uv.name for uv in o.data.uv_layers]
    mats = [ (s.material.name if s.material else "<none>") for s in o.material_slots ]
    print(f"  {o.name} | tris={len(o.data.loop_triangles)} | uv={uvs} | mats={mats}", flush=True)

print("[INSPECT] --- materials (name | base_color_image) ---", flush=True)
for m in bpy.data.materials:
    img = None
    if m.use_nodes:
        for n in m.node_tree.nodes:
            if n.type == "TEX_IMAGE" and n.image:
                img = n.image.name + " (" + (n.image.filepath or "<no path>") + ")"
                break
    print(f"  {m.name} | {img}", flush=True)

print("[INSPECT] --- images ---", flush=True)
for im in bpy.data.images:
    print(f"  {im.name} | size={tuple(im.size)} | filepath={im.filepath!r}", flush=True)
print("[INSPECT] done", flush=True)
