"""Blender background cook: -- <stock source> <output art> <proof directory>."""
import csv, json, math, sys
from pathlib import Path
import bpy
from mathutils import Vector, Matrix
from PIL import Image, ImageDraw, ImageFont, ImageOps
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ase_to_glb import parse_ase
src, out, proof = map(Path, sys.argv[sys.argv.index('--') + 1:])
out.mkdir(parents=True, exist_ok=True); proof.mkdir(parents=True, exist_ok=True)
rows = list(csv.reader((src / 'art/compbas.csv').open()))[1:]
icons = {r[23].lower(): r for r in rows if len(r) > 28 and r[23].lower().endswith('.tga')}
sheet = Image.new('RGB', (3840, 2160), (23, 29, 35)); draw = ImageDraw.Draw(sheet)
font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 18, layout_engine=ImageFont.Layout.BASIC)
inventory = []
for index, (name, row) in enumerate(sorted(icons.items())):
    model = src / 'tgl' / (Path(name).stem + '.ase')
    old = Image.open(src / 'art' / name).convert('RGBA'); w, h = map(int, row[27:29])
    inventory.append(dict(id=row[0], name=row[2], file=name, stock=old.size, cells=[w,h], model=str(model) if model.exists() else None))
    if not model.exists(): continue
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene; scene.render.engine = 'CYCLES'; scene.cycles.samples = 64
    scene.cycles.use_denoising = False; scene.render.film_transparent = True
    scene.world = bpy.data.worlds.new('Studio'); scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs[0].default_value = (.22,.25,.3,1)
    scene.world.node_tree.nodes['Background'].inputs[1].default_value = .35
    geoms, materials, _ = parse_ase(model)
    mats = []
    for material in materials:
        mat = bpy.data.materials.new(material.name); mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get('Principled BSDF'); bsdf.inputs['Roughness'].default_value = .42
        texture = src / 'tgl/128' / material.bitmap.lower()
        assert texture.exists(), texture
        tex = mat.node_tree.nodes.new('ShaderNodeTexImage'); tex.image = bpy.data.images.load(str(texture))
        mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color']); mats.append(mat)
    points = [Vector(p) for g in geoms for p in g.positions]
    lo = Vector(tuple(min(p[i] for p in points) for i in range(3))); hi = Vector(tuple(max(p[i] for p in points) for i in range(3)))
    center = (lo+hi)/2; scale = 2/max(hi-lo); rotation = Matrix.Rotation(math.pi/2, 3, 'Y')
    objects = []
    for g in geoms:
        mesh = bpy.data.meshes.new(g.node_name); mesh.from_pydata([rotation @ ((Vector(p)-center)*scale) for p in g.positions], [], [f[:3] for f in g.faces])
        obj = bpy.data.objects.new(g.node_name, mesh); scene.collection.objects.link(obj); objects.append(obj)
        mesh.materials.append(mats[g.mat_ref]); uv = mesh.uv_layers.new()
        for face, tf in zip(mesh.polygons, g.tfaces):
            for loop, ti in zip(face.loop_indices, tf): uv.data[loop].uv = g.tverts[ti][:2]
    def aim(obj, position):
        obj.location = position; obj.rotation_euler = (-obj.location).to_track_quat('-Z','Y').to_euler()
    for label, pos, power, size in [('Key',(3,-4,5),450,4), ('Fill',(-4,-2,2),250,5), ('Rim',(1,4,4),500,3)]:
        light = bpy.data.lights.new(label,'AREA'); light.energy = power; light.shape = 'DISK'; light.size = size
        obj = bpy.data.objects.new(label,light); scene.collection.objects.link(obj); aim(obj,pos)
    camera = bpy.data.objects.new('Camera',bpy.data.cameras.new('Camera')); scene.collection.objects.link(camera); aim(camera,(4,-7,3))
    scene.camera = camera; camera.data.type = 'ORTHO'; bpy.context.view_layer.update()
    projected = [camera.matrix_world.inverted() @ v.co for obj in objects for v in obj.data.vertices]
    dx = max(p.x for p in projected)-min(p.x for p in projected); dy = max(p.y for p in projected)-min(p.y for p in projected)
    camera.location += camera.rotation_euler.to_matrix() @ Vector(((max(p.x for p in projected)+min(p.x for p in projected))/2,(max(p.y for p in projected)+min(p.y for p in projected))/2,0))
    aspect = (48*w)/(32*h); camera.data.ortho_scale = max(dx,dy*aspect)/min(1,aspect)*1.14
    scene.render.resolution_x = 48*w*4; scene.render.resolution_y = 32*h*4; scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'; scene.render.image_settings.color_mode = 'RGBA'
    scene.render.filepath = str(proof / (Path(name).stem+'.png')); bpy.ops.render.render(write_still=True)
    rendered = Image.open(scene.render.filepath).convert('RGBA')
    bbox = rendered.getbbox(); assert 0 < bbox[0] < bbox[2] < rendered.width and 0 < bbox[1] < bbox[3] < rendered.height, name
    canvas = Image.new('RGBA',(old.width*4,old.height*4)); canvas.paste(rendered,(0,0)); canvas.transpose(Image.Transpose.FLIP_TOP_BOTTOM).save(out/name, compression='tga_rle')
    assert canvas.getchannel('A').getextrema() == (0,255)
    x, y = (index%8)*480, (index//8)*420
    draw.text((x+12,y+8),row[2],font=font,fill='white'); draw.text((x+12,y+34),f'{w}x{h} cells | old / new',font=font,fill='#91a9bb')
    for offset, image in [(10,old.crop((0,0,48*w,32*h))), (245,rendered)]:
        image = ImageOps.contain(image,(220,340),Image.Resampling.LANCZOS); sheet.paste(image,(x+offset+(220-image.width)//2,y+68),image)
    print('COOKED',name,old.size,canvas.size,flush=True)
(proof/'inventory.json').write_text(json.dumps(inventory,indent=2))
sheet.save(proof/'old-new-4k.png')
assert all(r['model'] for r in inventory), 'Missing models: see inventory.json'
