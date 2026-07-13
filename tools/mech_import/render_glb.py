"""Headless Blender: render a GLB to PNG from front + 3/4 views to eyeball geometry.
    blender --background --python render_glb.py -- --src=x.glb --out=shot.png
"""
import bpy, sys, math

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
A = dict(a.lstrip("-").split("=", 1) for a in argv)
SRC, OUT = A["src"], A["out"]

bpy.ops.wm.read_factory_settings(use_empty=True)
if SRC.lower().endswith(".obj"):
    bpy.ops.wm.obj_import(filepath=SRC)
else:
    bpy.ops.import_scene.gltf(filepath=SRC)

# Frame all geometry.
import mathutils
mins = mathutils.Vector((1e9,)*3); maxs = mathutils.Vector((-1e9,)*3)
for o in bpy.context.scene.objects:
    if o.type == "MESH":
        for c in o.bound_box:
            wc = o.matrix_world @ mathutils.Vector(c)
            for i in range(3):
                mins[i] = min(mins[i], wc[i]); maxs[i] = max(maxs[i], wc[i])
center = (mins + maxs) / 2
size = max((maxs - mins))

# Sun + camera (3/4 front).
light = bpy.data.lights.new("L", "SUN"); lo = bpy.data.objects.new("L", light)
bpy.context.collection.objects.link(lo); lo.rotation_euler = (math.radians(55), 0, math.radians(30))
cam = bpy.data.cameras.new("C"); co = bpy.data.objects.new("C", cam)
bpy.context.collection.objects.link(co); bpy.context.scene.camera = co
cam.type = "ORTHO"; cam.ortho_scale = size * 1.4
d = size * 2
co.location = (center.x + d*0.7, center.y - d*0.7, center.z + d*0.4)
dirv = (center - co.location).normalized()
co.rotation_euler = dirv.to_track_quat("-Z", "Y").to_euler()

sc = bpy.context.scene
sc.render.engine = "BLENDER_WORKBENCH"
sc.display.shading.light = "STUDIO"
sc.display.shading.color_type = "TEXTURE"  # show albedo texture, not flat grey
sc.render.image_settings.file_format = "PNG"
sc.render.resolution_x = 512; sc.render.resolution_y = 512
sc.render.film_transparent = False
sc.render.filepath = OUT
bpy.ops.render.render(write_still=True)
print(f"[RENDER] wrote {OUT} (size={size:.2f})", flush=True)
