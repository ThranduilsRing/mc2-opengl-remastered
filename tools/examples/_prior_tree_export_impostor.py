import bpy, sys, math
from mathutils import Vector

# Bake the lush tree canopy to an alpha cutout texture, then build a 2-card
# cross-quad impostor GLB sized to the LOD0 bounds. The cross collapses ~27
# overlapping leaf cards -> 2 quads (kills alpha-card overdraw) and ~27 recipes
# -> 2 per instance. Texture is written to the engine tgl dir so
# LoadOverrideRenderShapeTextures resolves it by name (a_ prefix => alpha).

SRC = r"C:\Users\Joe\Downloads\tree_small_02_1k.gltf\tree_small_02_1k.gltf"
SCALE = 32.0
TEX_NAME = "a_tree_lush_impostor"          # a_ => engine alpha convention
TEX_DIR  = r"A:\Games\mc2-opengl\mc2-win64-v0.3\data\tgl\128"
TEX_PATH = TEX_DIR + "\\" + TEX_NAME + ".tga"
OUT_GLB  = r"A:\Games\mc2-model-override-recon\data\model_overrides\source\trees\tree_lush_impostor.glb"
RES = 512

def log(*a): print("[IMPOSTOR]", *a, flush=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=SRC)
meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
log("imported meshes", len(meshes))
for o in meshes:
    o.scale = (SCALE, SCALE, SCALE)
bpy.context.view_layer.update()

# Combined world-space bbox of the scaled tree.
mn = Vector(( 1e18,  1e18,  1e18))
mx = Vector((-1e18, -1e18, -1e18))
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ Vector(c)
        mn.x=min(mn.x,w.x); mn.y=min(mn.y,w.y); mn.z=min(mn.z,w.z)
        mx.x=max(mx.x,w.x); mx.y=max(mx.y,w.y); mx.z=max(mx.z,w.z)
ctr = (mn+mx)*0.5
H = mx.z-mn.z
W = max(mx.x-mn.x, mx.y-mn.y)
log("bbox min",mn[:],"max",mx[:],"H",H,"W",W)

# ---- Render setup: ortho front view, transparent film, alpha cutout ----
scn = bpy.context.scene
scn.render.engine = 'BLENDER_EEVEE'
scn.view_settings.view_transform='Standard'  # true albedo, not AgX-washed
scn.view_settings.look='None'
scn.render.film_transparent = True
scn.render.resolution_x = RES
scn.render.resolution_y = RES
scn.render.image_settings.file_format = 'TARGA'
scn.render.image_settings.color_mode = 'RGBA'
scn.render.filepath = TEX_PATH

# Flat-ish lighting so the canopy shows albedo (impostor wants color, not shading).
world = bpy.data.worlds.new("w"); scn.world = world
world.use_nodes = True
bg = world.node_tree.nodes.get("Background")
if bg: bg.inputs[0].default_value = (1,1,1,1); bg.inputs[1].default_value = 1.2

# Ortho camera looking +Y (front), Z up in image.
cam_data = bpy.data.cameras.new("cam"); cam_data.type='ORTHO'
cam_data.ortho_scale = max(W, H) * 1.04
cam = bpy.data.objects.new("cam", cam_data); scn.collection.objects.link(cam)
dist = (mx.y-mn.y) + W*2 + 100.0
cam.location = (ctr.x, mn.y - dist, ctr.z)
cam.rotation_euler = (math.radians(90), 0, 0)   # -Z -> +Y
scn.camera = cam

bpy.ops.render.render(write_still=True)
log("rendered impostor texture ->", TEX_PATH)

# ---- Build the 2-card cross impostor mesh ----
for o in list(bpy.context.scene.objects):
    if o.type == 'MESH':
        bpy.data.objects.remove(o, do_unlink=True)

hw = W*0.5
z0, z1 = mn.z, mx.z
# Card A in X-Z plane (faces +/-Y); Card B in Y-Z plane (faces +/-X). Centered at ctr.x/ctr.y.
verts = [
    # card A (vary X, Z)
    (ctr.x-hw, ctr.y, z0), (ctr.x+hw, ctr.y, z0), (ctr.x+hw, ctr.y, z1), (ctr.x-hw, ctr.y, z1),
    # card B (vary Y, Z)
    (ctr.x, ctr.y-hw, z0), (ctr.x, ctr.y+hw, z0), (ctr.x, ctr.y+hw, z1), (ctr.x, ctr.y-hw, z1),
]
faces = [(0,1,2,3),(4,5,6,7)]
me = bpy.data.meshes.new("impostor"); me.from_pydata(verts, [], faces); me.update()
# UVs: full texture on each card.
me.uv_layers.new(name="UVMap")
uvl = me.uv_layers.active.data
quad_uv = [(0,0),(1,0),(1,1),(0,1)]
for fi in range(2):
    for vi in range(4):
        uvl[fi*4+vi].uv = quad_uv[vi]
obj = bpy.data.objects.new("tree_lush_impostor", me)
bpy.context.scene.collection.objects.link(obj)

# Material: base-color image NAMED a_tree_lush_impostor.* so DeriveMC2TextureName
# yields a_tree_lush_impostor.tga. Pixels here are irrelevant (engine loads the
# .tga from data/tgl/128); only the name matters. Alpha CLIP.
mat = bpy.data.materials.new(TEX_NAME); mat.use_nodes = True
mat.blend_method = 'CLIP'
nt = mat.node_tree
bsdf = nt.nodes.get("Principled BSDF")
img = bpy.data.images.load(TEX_PATH)   # the baked TGA (name -> a_tree_lush_impostor.tga)
tex = nt.nodes.new("ShaderNodeTexImage"); tex.image = img
nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
nt.links.new(tex.outputs["Color"],bsdf.inputs["Emission Color"]); bsdf.inputs["Emission Strength"].default_value=1.0
nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
obj.data.materials.append(mat)

bpy.ops.object.select_all(action='DESELECT')
obj.select_set(True); bpy.context.view_layer.objects.active = obj
bpy.ops.export_scene.gltf(filepath=OUT_GLB, export_format='GLB',
                          use_selection=True, export_apply=True, export_yup=True,
                          export_image_format='AUTO')
log("exported impostor GLB ->", OUT_GLB)
log("DONE")
