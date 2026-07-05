#!/usr/bin/env python3
"""blender_bake_coastal_cliff.py -- reproducible Blender authoring of the REAL
cliff-wall mesh for the MC2 terrain mesh-decal (replaces the flat displaced-plane
proof of gen_cliff_wall_glb.py with a photogrammetry cliff-face segment).

Run the bpy body INSIDE Blender via the BlenderMCP socket helper:

  py -3 tools/blender_cmd.py execute_code - < tools/blender_bake_coastal_cliff.py

PREREQUISITES (do these first, they are interactive / network):
  1. PolyHaven integration enabled in the BlenderMCP addon.
  2. Download the cliff scan into the scene:
       py -3 tools/blender_cmd.py download_polyhaven_asset \
         '{"asset_id":"coastal_cliff_01","asset_type":"models","resolution":"2k"}'
     -> imports a dense ~462k-tri object 'coastal_cliff_01', a long coastal
        cliff strip: Blender-local X = length(~92), Y = face depth(~11),
        Z = height(~10.3), Z-up. The cliff FACE profile lives in the Y-Z plane.

WHAT THIS SCRIPT DOES (deterministic, no network):
  * duplicates a ~35-unit X-segment of the scan, deletes the rest,
  * decimates to ~12.7k tris (collapse+triangulate, RTS-sane),
  * bakes a NON-UNIFORM transform into the vertices so that, AFTER MC2's
    importer axis map (assimp_importer.cpp:90 case 0 = (-x,-y,z)) the wall
    stands upright:
        world +Y = height  (Blender Z, x SZ=29  -> ~290u)
        world  X = contour (Blender X, x SX=9.5 -> ~335u)
        world +Z = outward cliff face / depth (Blender -Y, x SY=13 -> ~125u)
    base sunk SINK=18u below y=0 (self-sinking skirt).
    Vertices are authored PRE-COMPENSATED: local=(-worldX,-worldY,worldZ) so the
    importer's (-x,-y,z) produces the upright world layout above.
  * recomputes OUTWARD normals (normals_make_consistent inside=False); the
    outward cliff-face region (world +Z) mean normal.z verifies > 0 so the
    static-prop backface cull (GL_BACK) keeps the face and calc_light lights it.
  * renames images to the marble_cliff_01 family + packs them, downscales to 1k,
  * names the single object/mesh '_PAB_CoastalCliffWall' (footprint skip),
  * exports a SINGLE-node GLB with export_yup=FALSE (so the baked MC2-local verts
    pass through unrotated -- yup=True would swap height onto Z and lay it down).

Then finish OUTSIDE Blender:
  py -3 tools/condition_coastal_cliff_glb.py <exported>.glb CliffWallGLB.glb
  py -3 tools/place_cliffwall_mc2_01.py --deploy-root <install> --glb CliffWallGLB.glb

Tunables below (segment window, per-axis scale, sink) are the knobs to iterate
scale/detail if the in-engine capture reads wrong-sized or too dense.
"""
import bpy, bmesh, os
from mathutils import Vector

# --- tunables ---
SRC_NAME = "coastal_cliff_01"
XLO, XHI = -15.0, 20.0     # Blender-X segment window (~35u wide)
DECIMATE_RATIO = 0.07      # 181k seg tris -> ~12.7k
SX, SY, SZ = 9.5, 13.0, 29.0   # contour / depth / height scale
SINK = 18.0                # world units base sinks below y=0
OUT = os.path.join(os.path.expanduser("~"), "AppData", "Local", "Temp",
                   "CoastalCliffWall.glb")

obj = bpy.data.objects[SRC_NAME]
bpy.ops.object.select_all(action='DESELECT')
obj.select_set(True); bpy.context.view_layer.objects.active = obj
bpy.ops.object.duplicate()
seg = bpy.context.active_object; seg.name = "_PAB_CoastalCliffWall"
me = seg.data; me.name = "_PAB_CoastalCliffWall"

# clip to X segment
bm = bmesh.new(); bm.from_mesh(me)
bmesh.ops.delete(bm, geom=[f for f in bm.faces
                           if not (XLO <= f.calc_center_median().x <= XHI)],
                 context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces],
                 context='VERTS')
bm.to_mesh(me); bm.free(); me.update()

# decimate
dec = seg.modifiers.new('Dec', 'DECIMATE')
dec.ratio = DECIMATE_RATIO; dec.use_collapse_triangulate = True
bpy.ops.object.modifier_apply(modifier='Dec')

# bake pre-compensated MC2-local vertices
xs = [v.co.x for v in me.vertices]; ys = [v.co.y for v in me.vertices]
zs = [v.co.z for v in me.vertices]
cx = (min(xs) + max(xs)) / 2; cy = (min(ys) + max(ys)) / 2; z0 = min(zs)
for v in me.vertices:
    bx = v.co.x - cx; by = v.co.y - cy; bz = v.co.z
    wx = bx * SX
    wy = (bz - z0) * SZ - SINK
    wz = (-by) * SY
    v.co = Vector((-wx, -wy, wz))     # local = (-worldX,-worldY,worldZ)
me.update()

# outward normals
bpy.ops.object.select_all(action='DESELECT')
seg.select_set(True); bpy.context.view_layer.objects.active = seg
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.quads_convert_to_tris(quad_method='BEAUTY')
bpy.ops.mesh.normals_make_consistent(inside=False)
bpy.ops.object.mode_set(mode='OBJECT')

# textures -> marble_cliff_01 family, packed + downscaled
ren = {SRC_NAME + '_diff': 'marble_cliff_01',
       SRC_NAME + '_nor_gl': 'marble_cliff_01_nor_gl',
       SRC_NAME + '_rough': 'marble_cliff_01_rough'}
for old, new in ren.items():
    if old in bpy.data.images:
        bpy.data.images[old].name = new
for i in bpy.data.images:
    if i.name.startswith('marble_cliff_01'):
        try: i.pack()
        except Exception: pass
        if i.size[0] > 1024:
            i.scale(1024, 1024)

seg.location = (0, 0, 0); seg.rotation_euler = (0, 0, 0); seg.scale = (1, 1, 1)
bpy.ops.export_scene.gltf(
    filepath=OUT, export_format='GLB', use_selection=True,
    export_apply=True, export_yup=False,
    export_normals=True, export_texcoords=True, export_materials='EXPORT',
    export_image_format='JPEG', export_cameras=False, export_lights=False,
    export_animations=False)

xs = [v.co.x for v in me.vertices]; ys = [v.co.y for v in me.vertices]
zs = [v.co.z for v in me.vertices]
print(f"EXPORTED {OUT} ({os.path.getsize(OUT)} bytes) tris={len(me.polygons)}")
print(f"  world after (-x,-y,z): width={max(xs)-min(xs):.0f} "
      f"height={max(ys)-min(ys):.0f} depth={max(zs)-min(zs):.0f} "
      f"base_y={-max(ys):.0f}")
