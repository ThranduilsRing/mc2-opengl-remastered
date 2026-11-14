"""Blender (background) render of a terrain_export GLB.  Run:
  blender -b --python tools/terrain_export/render_blender.py -- --glb X.glb --out X.png
      [--target wx wy] [--look-deg 0] [--dist 3000] [--pitch 35] [--fov 30] [--samples 24] [--res 1280 720]
World frame: wx east, wy north, wu.  --look-deg = compass heading the camera looks toward (0 = north, 90 = east).
Imported scene is Z-up, scale = export scale (default 0.1 => 1 unit = 10 wu).  Sun matches the mission .fit
(LightDirPitch 50, LightDirYaw 11).  Cycles HIP on the second (non-display) XTX (blender_gpu.py); no silent fallback.
"""
import argparse
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser()
ap.add_argument("--glb", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--target", type=float, nargs=2, default=[2453.0, -1984.0])
ap.add_argument("--look-deg", type=float, default=0.0)
ap.add_argument("--dist", type=float, default=3000.0)
ap.add_argument("--pitch", type=float, default=35.0)
ap.add_argument("--fov", type=float, default=30.0)
ap.add_argument("--samples", type=int, default=24)
ap.add_argument("--res", type=int, nargs=2, default=[1280, 720])
ap.add_argument("--scale", type=float, default=0.1)
ap.add_argument("--hide-props", action="store_true")
ap.add_argument("--sun-pitch", type=float, default=50.0)
ap.add_argument("--sun-yaw", type=float, default=11.0)
ap.add_argument("--cam-bookmark", type=float, nargs=6, metavar=("X", "Y", "Z", "ROT", "PROJ", "ALT"),
                help="game bookmark camera: focus pos.x pos.y cameraShiftZ, rot(yaw deg), proj(pitch deg), alt(range wu); "
                     "FOV is the game's 40 deg HORIZONTAL (sensor_fit HORIZONTAL); use --res = capture aspect")
ap.add_argument("--yaw-sign", type=float, default=-1.0, help="debug: sign on sin(yaw) in the eye offset")
ap.add_argument("--cycles-device", default="OPTIX", help="unused: always the second GPU via blender_gpu.py")
a = ap.parse_args(argv)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=a.glb)
sc = bpy.context.scene
S = a.scale

if a.hide_props:
    for o in list(bpy.data.objects):
        if o.name.startswith("obj_"):
            bpy.data.objects.remove(o, do_unlink=True)
# water: make it a simple glossy translucent surface
for o in bpy.data.objects:
    if o.name.startswith("water"):
        for ms in o.material_slots:
            nt = ms.material.node_tree
            bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
            bsdf.inputs["Roughness"].default_value = 0.08
            bsdf.inputs["Alpha"].default_value = 0.7

# world: sky colour similar to the in-game haze
w = bpy.data.worlds.new("w")
w.use_nodes = True
w.node_tree.nodes["Background"].inputs[0].default_value = (0.62, 0.62, 0.58, 1)
w.node_tree.nodes["Background"].inputs[1].default_value = 0.9
sc.world = w

sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
sun.data.energy = 4.0
sun.data.angle = math.radians(2)
sc.collection.objects.link(sun)
# light travels toward -Z rotated; pitch = elevation above horizon, yaw compass of the sun position
el = math.radians(a.sun_pitch)
yaw = math.radians(a.sun_yaw)
sun.rotation_euler = (math.pi / 2 - el, 0, -yaw + math.pi)

cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.sensor_fit = "HORIZONTAL"
cam.data.angle = math.radians(40.0 if a.cam_bookmark else a.fov)
cam.data.clip_end = 100000
# terrain height under target (raycast down) for the look-at point
tx, ty = a.target[0] * S, a.target[1] * S
hit = sc.ray_cast(bpy.context.view_layer.depsgraph, Vector((tx, ty, 5000)), Vector((0, 0, -1)))
tz = hit[1].z if hit[0] else 0.0
if a.cam_bookmark:
    fx, fy, fz, rot, proj, alt = a.cam_bookmark
    yaw, pit = math.radians(rot), math.radians(proj)
    ln = alt * math.cos(pit)
    # engine: eye = focus - polarToVector; world y is north, x east
    ex = fx + a.yaw_sign * ln * math.sin(yaw)
    ey = fy + ln * math.cos(yaw)
    ez = fz + alt * math.sin(pit)
    cam.location = Vector((ex * S, ey * S, ez * S))
    cam.rotation_euler = (Vector((fx * S, fy * S, fz * S)) - cam.location).to_track_quat("-Z", "Y").to_euler()
    print("CAM eye_wu", ex, ey, ez, "focus_wu", fx, fy, fz)
hd = math.radians(a.look_deg)
fwd = Vector((math.sin(hd), math.cos(hd), 0))
d = a.dist * S
p = math.radians(a.pitch)
if not a.cam_bookmark:
    cam.location = Vector((tx, ty, tz)) - fwd * d * math.cos(p) + Vector((0, 0, d * math.sin(p)))
    dirv = Vector((tx, ty, tz)) - cam.location
    cam.rotation_euler = dirv.to_track_quat("-Z", "Y").to_euler()

sc.render.engine = "CYCLES"
sc.cycles.samples = a.samples
sc.cycles.use_denoising = True
sc.render.resolution_x, sc.render.resolution_y = a.res
sc.render.filepath = a.out
sc.view_settings.view_transform = "Standard"
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "blender_pipeline"))
import blender_gpu  # noqa: E402
blender_gpu.apply(sc)   # second (non-display) GPU only; fails loud, never the display GPU
bpy.ops.render.render(write_still=True)
print("RENDERED", a.out)
