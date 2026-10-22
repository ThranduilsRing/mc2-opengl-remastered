"""Headless source, LOD bake, parts library and fixed Cycles review scene."""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0,str(Path(__file__).resolve().parent))
from core import read_json, write_json, inside
from parts import MeshKit, assemble


def cycles(samples=24):
    scene=bpy.context.scene
    scene.render.engine='CYCLES'
    scene.cycles.samples=samples
    scene.cycles.use_denoising=True
    scene.render.threads_mode='FIXED';scene.render.threads=12
    # Device discovery is local and falls back to CPU without changing the art.
    prefs=bpy.context.preferences.addons['cycles'].preferences
    for backend in ['OPTIX','CUDA','HIP']:
        try:
            prefs.compute_device_type=backend;prefs.get_devices()
            gpu=[d for d in prefs.devices if d.type!='CPU']
            if gpu:
                for d in prefs.devices:d.use=d.type!='CPU'
                scene.cycles.device='GPU'
                print('[scifi] Cycles',backend,[d.name for d in gpu],flush=True)
                break
        except (TypeError,RuntimeError):
            continue
    scene.view_settings.view_transform='AgX'
    return scene


def material(name,color,texture=None,normal=None):
    mat=bpy.data.materials.new(name);mat.use_nodes=True
    nt=mat.node_tree;bs=nt.nodes.get('Principled BSDF')
    bs.inputs['Base Color'].default_value=(*color,1)
    bs.inputs['Metallic'].default_value=0
    bs.inputs['Roughness'].default_value=.86
    bs.inputs['Specular IOR Level'].default_value=.18
    if texture:
        coords=nt.nodes.new('ShaderNodeTexCoord')
        scale=nt.nodes.new('ShaderNodeVectorMath');scale.operation='SCALE';scale.inputs[3].default_value=.32
        nt.links.new(coords.outputs['Object'],scale.inputs[0])
        tex=nt.nodes.new('ShaderNodeTexImage');tex.image=bpy.data.images.load(str(texture),check_existing=True);tex.projection='BOX';tex.projection_blend=.18
        nt.links.new(scale.outputs[0],tex.inputs['Vector'])
        bw=nt.nodes.new('ShaderNodeRGBToBW');nt.links.new(tex.outputs['Color'],bw.inputs[0])
        ramp=nt.nodes.new('ShaderNodeValToRGB')
        ramp.color_ramp.elements[0].position=.10;ramp.color_ramp.elements[0].color=(*(c*.83 for c in color),1)
        ramp.color_ramp.elements[1].position=.90;ramp.color_ramp.elements[1].color=(*(min(1,c*1.06) for c in color),1)
        nt.links.new(bw.outputs[0],ramp.inputs[0])
        # Broad base grime lives in material colour, never a fallback specular.
        geom=nt.nodes.new('ShaderNodeNewGeometry');sep=nt.nodes.new('ShaderNodeSeparateXYZ')
        nt.links.new(geom.outputs['Position'],sep.inputs[0])
        grime=nt.nodes.new('ShaderNodeMapRange');grime.inputs['From Min'].default_value=.15;grime.inputs['From Max'].default_value=1.25;grime.inputs['To Min'].default_value=.62;grime.inputs['To Max'].default_value=1
        nt.links.new(sep.outputs['Z'],grime.inputs['Value'])
        mix=nt.nodes.new('ShaderNodeMixRGB');mix.blend_type='MULTIPLY';mix.inputs[0].default_value=1
        nt.links.new(ramp.outputs[0],mix.inputs[1]);nt.links.new(grime.outputs['Result'],mix.inputs[2]);nt.links.new(mix.outputs[0],bs.inputs['Base Color'])
        if normal:
            ntex=nt.nodes.new('ShaderNodeTexImage');ntex.image=bpy.data.images.load(str(normal),check_existing=True);ntex.image.colorspace_settings.name='Non-Color';ntex.projection='BOX';ntex.projection_blend=.18
            nt.links.new(scale.outputs[0],ntex.inputs['Vector'])
            nm=nt.nodes.new('ShaderNodeNormalMap');nm.inputs['Strength'].default_value=.22
            nt.links.new(ntex.outputs[0],nm.inputs['Color']);nt.links.new(nm.outputs[0],bs.inputs['Normal'])
    return mat


def materials(style,seed,texture_manifest):
    p=style['palettes'][seed%len(style['palettes'])]
    colors={'armour':p['armour'],'trim':p['trim'],'accent':p['accent'],'concrete':[.37,.38,.34],'roof':[.19,.23,.23],'steel':[.095,.12,.13],'recess':[.027,.038,.042],'glass':[.075,.18,.21],'lamp':[.75,.73,.56]}
    result={}
    for name,c in colors.items():
        source=texture_manifest.get('concrete' if name=='concrete' else 'corrugated' if name=='roof' else 'paint',{}) if name not in ['glass','recess','lamp'] else {}
        result[name]=material(name,c,source.get('color'),source.get('normal'))
    hazard=material('hazard stripes',[.7,.46,.1]);nt=hazard.node_tree
    geom=nt.nodes.new('ShaderNodeNewGeometry');sep=nt.nodes.new('ShaderNodeSeparateXYZ');nt.links.new(geom.outputs['Position'],sep.inputs[0])
    add=nt.nodes.new('ShaderNodeMath');add.operation='ADD';nt.links.new(sep.outputs['X'],add.inputs[0]);nt.links.new(sep.outputs['Z'],add.inputs[1])
    ping=nt.nodes.new('ShaderNodeMath');ping.operation='PINGPONG';ping.inputs[1].default_value=.4;nt.links.new(add.outputs[0],ping.inputs[0])
    ramp=nt.nodes.new('ShaderNodeValToRGB');ramp.color_ramp.interpolation='CONSTANT';ramp.color_ramp.elements[0].color=(.04,.05,.045,1);ramp.color_ramp.elements[1].position=.20;ramp.color_ramp.elements[1].color=(*p['accent'],1);nt.links.new(ping.outputs[0],ramp.inputs[0]);nt.links.new(ramp.outputs[0],nt.nodes.get('Principled BSDF').inputs['Base Color'])
    result['hazard']=hazard
    return result


def mesh_object(kit,name,mats,bevel=.045):
    me=bpy.data.meshes.new(name);me.from_pydata(kit.vertices,[],kit.faces);me.update()
    keys=list(mats)
    for m in mats.values():me.materials.append(m)
    for p,key in zip(me.polygons,kit.materials):p.material_index=keys.index(key)
    uv=me.uv_layers.new(name='SourceUV')
    for p in me.polygons:
        axis=max(range(3),key=lambda i:abs(p.normal[i]))
        axes=[i for i in range(3) if i!=axis]
        for li in p.loop_indices:
            co=me.vertices[me.loops[li].vertex_index].co
            uv.data[li].uv=(co[axes[0]]*.32,co[axes[1]]*.32)
    ob=bpy.data.objects.new(name,me);bpy.context.collection.objects.link(ob)
    bpy.context.view_layer.objects.active=ob;ob.select_set(True)
    if bevel:
        mod=ob.modifiers.new('Physical edge catches','BEVEL');mod.width=bevel;mod.segments=2;mod.limit_method='ANGLE';mod.angle_limit=.5
        bpy.ops.object.modifier_apply(modifier=mod.name)
    # Consistent normals also fixes reference-facing barrel end caps.
    bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.mesh.normals_make_consistent(inside=False);bpy.ops.object.mode_set(mode='OBJECT')
    ob.select_set(False)
    return ob


def triangles(ob):
    return sum(len(p.vertices)-2 for p in ob.data.polygons)


def select(ob,others=()):
    bpy.ops.object.select_all(action='DESELECT')
    for o in [ob,*others]:o.select_set(True)
    bpy.context.view_layer.objects.active=ob


def new_image(name,res,srgb=False):
    im=bpy.data.images.new(name,width=res,height=res,alpha=False)
    im.colorspace_settings.name='sRGB' if srgb else 'Non-Color'
    return im


def save_image(im,path,fmt='PNG'):
    im.filepath_raw=str(path);im.file_format=fmt;im.save()


def constrain_lod(ob,job):
    # Quadric collapse may move an outer pad vertex beyond its original lot.
    # Project those few vertices back into the conservative wing union.
    for v in ob.data.vertices:
        x,y,z=v.co
        if not inside([x,y],job['footprint_m']):
            choices=[(max(a,min(c,x)),max(b,min(d,y))) for a,b,c,d in job['wings_m']]
            x,y=min(choices,key=lambda p:(p[0]-x)**2+(p[1]-y)**2)
        v.co=(x,y,max(0,min(job['height_m'],z)))
    ob.data.update()


def lod_bake(high,job,style,out):
    scene=cycles(8);scene.render.bake.margin=6
    low=high.copy();low.data=high.data.copy();bpy.context.collection.objects.link(low);low.name='GameLOD0'
    select(low)
    mod=low.modifiers.new('RTS triangle budget','DECIMATE');mod.ratio=min(1,style['budgets']['lod0_max']*.96/triangles(high))
    bpy.ops.object.modifier_apply(modifier=mod.name)
    constrain_lod(low,job)
    bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.uv.smart_project(angle_limit=1.15192,island_margin=.004);bpy.ops.object.mode_set(mode='OBJECT')
    # Single target material; all source zones are projected from the high mesh.
    target=material('scifi_'+out.name,[.3,.3,.3]);low.data.materials.clear();low.data.materials.append(target)
    for p in low.data.polygons:p.material_index=0
    nt=target.node_tree;node=nt.nodes.new('ShaderNodeTexImage');nt.nodes.active=node
    res=style['budgets']['atlas_resolution']
    images={k:new_image(out.name+'_'+k,res,k=='basecolor') for k in ['basecolor','normal','ao','roughness']}
    high.hide_render=False;low.hide_render=False
    select(low,[high])
    scene.render.bake.use_selected_to_active=True
    scene.render.bake.cage_extrusion=.18
    scene.render.bake.max_ray_distance=.40
    for key,btype in [('basecolor','DIFFUSE'),('normal','NORMAL'),('ao','AO'),('roughness','ROUGHNESS')]:
        print('[scifi] bake',key,flush=True);node.image=images[key]
        kw={'type':btype,'use_clear':True}
        if btype=='DIFFUSE':kw['pass_filter']={'COLOR'}
        bpy.ops.object.bake(**kw)
        save_image(images[key],out/(out.name+'_'+key+'.png'))
    scene.render.bake.use_selected_to_active=False
    pixels={k:np.array(im.pixels[:],dtype=np.float32).reshape(res,res,4) for k,im in images.items()}
    orm=new_image(out.name+'_orm',res)
    packed=np.ones((res,res,4),dtype=np.float32);packed[:,:,0]=pixels['ao'][:,:,0];packed[:,:,1]=np.clip(pixels['roughness'][:,:,0],.72,1);packed[:,:,2]=0
    orm.pixels.foreach_set(packed.ravel());save_image(orm,out/(out.name+'_orm.png'))
    # Legacy diffuse-only runtime gets restrained baked contact occlusion.
    albedo=images['basecolor'];base=pixels['basecolor'];base[:,:,:3]*=(.72+.28*packed[:,:,0:1]);albedo.pixels.foreach_set(base.ravel())
    save_image(albedo,out/(out.name+'_basecolor.tga'),'TARGA_RAW')
    bs=nt.nodes.get('Principled BSDF');node.image=images['basecolor'];nt.links.new(node.outputs[0],bs.inputs['Base Color'])
    normaltex=nt.nodes.new('ShaderNodeTexImage');normaltex.image=images['normal'];nm=nt.nodes.new('ShaderNodeNormalMap');nt.links.new(normaltex.outputs[0],nm.inputs['Color']);nt.links.new(nm.outputs[0],bs.inputs['Normal'])
    roughtex=nt.nodes.new('ShaderNodeTexImage');roughtex.image=images['roughness'];nt.links.new(roughtex.outputs[0],bs.inputs['Roughness'])
    high.hide_render=True
    far=low.copy();far.data=low.data.copy();far.name='GameLOD1';bpy.context.collection.objects.link(far)
    select(far);mod=far.modifiers.new('Far LOD budget','DECIMATE');mod.ratio=min(1,style['budgets']['lod1_max']*.96/triangles(low));bpy.ops.object.modifier_apply(modifier=mod.name);constrain_lod(far,job);far.hide_render=True
    return low,far


def export_game(ob,path,job):
    # Engine importer reads raw mesh vertices and drops node transforms.
    # Authoring Z-up -> TG=(-x,-y,z) inverse: glTF=(-x,-height,depth).
    copy=ob.copy();copy.data=ob.data.copy();bpy.context.collection.objects.link(copy)
    unit=read_json(job['style_path'])['scale']['world_units_per_metre']
    bounds=job['stock']['bounds'];cx=(bounds['min'][0]+bounds['max'][0])/2;cz=(bounds['min'][2]+bounds['max'][2])/2;base=bounds['min'][1]
    for v in copy.data.vertices:
        x,y,z=v.co;v.co=(-x*unit-cx,-z*unit-base,y*unit+cz)
    # This basis has negative determinant: reverse winding as well as normals.
    select(copy);bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.mesh.flip_normals();bpy.ops.object.mode_set(mode='OBJECT')
    # Collapse can retain zero-area slivers. Validate before exporter caches loops.
    copy.data.validate(clean_customdata=False);copy.data.update()
    bpy.ops.export_scene.gltf(filepath=str(path),export_format='GLB',use_selection=True,export_yup=False,export_apply=True,export_image_format='NONE',export_normals=True,export_texcoords=True,export_materials='EXPORT')
    bpy.data.objects.remove(copy,do_unlink=True)


def setup_review(job,style):
    scene=cycles(style['presentation']['samples'])
    scene.world=bpy.data.worlds.new('Soft sky');scene.world.use_nodes=True
    scene.world.node_tree.nodes['Background'].inputs[0].default_value=(.65,.73,.85,1)
    scene.world.node_tree.nodes['Background'].inputs[1].default_value=.65
    ground=MeshKit();w,d=job['stock']['bounds']['size'][::2];unit=style['scale']['world_units_per_metre'];w/=unit;d/=unit
    ground.box((0,0,-.10),(w*8,d*8,.18),'ground')
    ground.box((0,-d*.75,-.01),(w*4,d*.30,.04),'road')
    polygon=job['footprint_m']
    for a,b in zip(polygon,polygon[1:]+polygon[:1]):
        dx,dy=b[0]-a[0],b[1]-a[1]
        with ground.at(((a[0]+b[0])/2,(a[1]+b[1])/2,0),math.atan2(dy,dx)):
            ground.box((0,0,.19),(math.hypot(dx,dy),.055,.025),'line')
    mesh_object(ground,'Review ground / road / footprint',{'ground':material('ground',[.25,.26,.245]),'road':material('road',[.115,.13,.135]),'line':material('lot paint',[.65,.67,.61])},0)
    ld=bpy.data.lights.new('Broad sun','SUN');ld.energy=2.6;ld.angle=math.radians(12)
    ld.angle=math.radians(style['presentation']['sun_angle_degrees'])
    light=bpy.data.objects.new('Broad sun',ld);scene.collection.objects.link(light);light.rotation_euler=(math.radians(28),math.radians(-24),math.radians(-30))
    camera_data=bpy.data.cameras.new('Review camera');cam=bpy.data.objects.new('Review camera',camera_data);scene.collection.objects.link(cam);scene.camera=cam
    cam.data.type='ORTHO';scene.render.image_settings.file_format='PNG';scene.render.film_transparent=False
    return cam,w,d


def render_view(cam,path,extent,height,rts=False):
    scene=bpy.context.scene
    target=Vector((0,0,height*.33))
    cam.location=target+Vector((extent*1.25,-extent*1.65,extent*(2.05 if rts else 1.05)))
    cam.rotation_euler=(target-cam.location).to_track_quat('-Z','Y').to_euler()
    scene.render.resolution_x=780 if rts else 1000;scene.render.resolution_y=480 if rts else 720;scene.render.resolution_percentage=100
    cam.data.ortho_scale=extent*(3.9 if rts else 1.46)
    scene.render.filepath=str(path);bpy.ops.render.render(write_still=True)


def build(job):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    out=Path(job['out']);out.mkdir(parents=True,exist_ok=True)
    style=read_json(job['style_path']);texture_manifest=read_json(out.parent/'texture_sources.json')
    kit=assemble(job,style);mats=materials(style,job['seed'],texture_manifest)
    high=mesh_object(kit,'High source',mats,style['dimensions']['bevel_m']['value'])
    # Font geometry is original source detail, baked down with the facade.
    letters=[]
    for x0,y0,x1,y1 in job['wings_m']:
        margin=min(.95,(x1-x0)*.09,(y1-y0)*.09);wh=max(2.0,job['height_m']*job['rule']['height_fraction'])
        curve=bpy.data.curves.new('Unit signage','FONT');curve.body=job['rule']['label']+' / '+str(job['seed']);curve.align_x='CENTER';curve.align_y='CENTER';curve.size=min(.40,wh*.055);curve.resolution_u=2
        text=bpy.data.objects.new('Original unit lettering',curve);bpy.context.collection.objects.link(text);text.location=((x0+x1)/2,y0+margin-.305,wh-.48);text.rotation_euler=(math.pi/2,0,0);text.data.materials.append(material('sign ink',[.79,.80,.64]))
        bpy.context.view_layer.update()
        limit=min((x1-x0-2*margin)*.46,6.6)
        if text.dimensions.x>limit:text.scale.x*=limit/text.dimensions.x
        select(text);bpy.ops.object.convert(target='MESH');letters.append(bpy.context.object)
    select(high,letters);bpy.ops.object.join()
    top=max(v.co.z for v in high.data.vertices)
    if top>job['height_m']:
        for v in high.data.vertices:v.co.z*=job['height_m']/top
    low,far=lod_bake(high,job,style,out)
    for ob in [low,far]:
        export_game(ob,out/(out.name+('_lod0.glb' if ob==low else '_lod1.glb')),job)
    stats={'source_triangles':triangles(high),'lod0_triangles':triangles(low),'lod1_triangles':triangles(far),'parts':dict(kit.counts),'palette':style['palettes'][job['seed']%len(style['palettes'])]['name'],'bakes':{'method':'Cycles selected-to-active from evaluated beveled source to decimated game mesh','resolution':style['budgets']['atlas_resolution'],'normal':'tangent OpenGL +Y','orm':'R=high-source AO, G=authored roughness, B=0 (painted nonmetal)','basecolor_tga':'uncompressed RGB, 28% AO modulation for legacy diffuse-only path'},'stock':job['stock'],'wings_m':job['wings_m']}
    write_json(out/'stats.json',stats)
    high.hide_render=False;low.hide_render=True;far.hide_render=True
    cam,w,d=setup_review(job,style)
    render_view(cam,out/'beauty.png',max(w,d),job['height_m'])
    high.hide_render=True;low.hide_render=False
    render_view(cam,out/'rts.png',max(w,d),job['height_m'],True)
    low.hide_render=True;high.hide_render=False
    bpy.ops.wm.save_as_mainfile(filepath=str(out/(out.name+'_source.blend')),compress=True)
    print('[scifi] COMPLETE',out,stats['source_triangles'],stats['lod0_triangles'],stats['lod1_triangles'],flush=True)


def library(out,style,texture_manifest):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mats=materials(style,17,texture_manifest)
    prototypes={'wall_panel':{'window':True},'bay_door':{},'rollup_door':{'width':4.8,'height':4.6,'rollup':True},'infantry_door':{},'window_slit':{},'buttress':{},'flat_roof':{'width':6,'depth':5},'barrel_roof':{'width':6,'depth':5,'z':0,'rise':1.5},'roof_truss':{},'dome':{},'vent':{},'pipe':{},'antenna':{},'catwalk':{},'ladder':{},'signage':{},'pad':{'width':5,'depth':5},'stairs':{},'floodlight':{}}
    stats={}
    for i,(name,params) in enumerate(prototypes.items()):
        kit=MeshKit(style['dimensions']);getattr(kit,'bay_door' if name=='rollup_door' else name)(**params)
        ob=mesh_object(kit,name,mats);ob.location=((i%5)*15,(i//5)*20,0);ob.asset_mark();ob['parameters']=json.dumps(params);stats[name]={'parameters':params,'source_triangles':triangles(ob)}
        low=ob.copy();low.data=ob.data.copy();bpy.context.collection.objects.link(low);low.name=name+'_lod1';select(low);mod=low.modifiers.new('Part preview LOD','DECIMATE');mod.ratio=.25;bpy.ops.object.modifier_apply(modifier=mod.name);low.hide_render=True;low.hide_viewport=True;stats[name]['lod1_triangles']=triangles(low)
    out.mkdir(parents=True,exist_ok=True);write_json(out/'parts.json',stats)
    bpy.ops.wm.save_as_mainfile(filepath=str(out/'parts.blend'),compress=True)


def reference(out):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    style=read_json(Path(__file__).parent/'style/battletech.json')
    bpy.ops.wm.obj_import(filepath=style['sources']['bt_industrial']['path'],forward_axis='NEGATIVE_Z',up_axis='Y')
    objects=[o for o in bpy.context.scene.objects if o.type=='MESH']
    mat=material('REFERENCE ONLY clay',[.35,.39,.36])
    for ob in objects:
        ob.data.materials.clear();ob.data.materials.append(mat)
    scene=cycles(24);scene.world=bpy.data.worlds.new('Reference sky');scene.world.use_nodes=True;scene.world.node_tree.nodes['Background'].inputs[1].default_value=.8
    ld=bpy.data.lights.new('Sun','SUN');ld.energy=3;ld.angle=.20;sun=bpy.data.objects.new('Sun',ld);scene.collection.objects.link(sun);sun.rotation_euler=(.5,-.4,-.5)
    cd=bpy.data.cameras.new('Reference');cam=bpy.data.objects.new('Reference',cd);scene.collection.objects.link(cam);scene.camera=cam;cam.data.type='ORTHO'
    pts=[o.matrix_world@Vector(c) for o in objects for c in o.bound_box];mn=Vector([min(p[i] for p in pts) for i in range(3)]);mx=Vector([max(p[i] for p in pts) for i in range(3)]);target=(mn+mx)/2;extent=max(mx-mn)
    cam.location=target+Vector((extent,-extent*1.5,extent));cam.rotation_euler=(target-cam.location).to_track_quat('-Z','Y').to_euler();cam.data.ortho_scale=extent*1.6
    scene.render.resolution_x=800;scene.render.resolution_y=550;scene.render.resolution_percentage=100;scene.render.filepath=str(out);bpy.ops.render.render(write_still=True)


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--job');ap.add_argument('--library');ap.add_argument('--reference');args=ap.parse_args(sys.argv[sys.argv.index('--')+1:])
    if args.reference:reference(Path(args.reference))
    elif args.library:
        out=Path(args.library);library(out,read_json(Path(__file__).parent/'style/battletech.json'),read_json(out.parent/'texture_sources.json'))
    else:build(read_json(args.job))
