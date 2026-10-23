"""Authored high-to-low PBR atlas bake; canonical SurfaceMaterial classes.

Called inside the single, four-thread Blender worker. No reference data is read.
Base colour is unlit colour only. AO is kept exclusively in ORM.R.
"""
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

CLASSES = {'paint':'painted_metal','corr':'painted_metal','hazard':'painted_metal',
           'metal':'bare_metal','grate':'grating','ferro':'concrete','glass':'glass',
           'wood':'wood','canvas':'fabric','rubber':'rubber','gravel':'dielectric',
           'char':'dielectric','veg':'dielectric','lamp':'emissive','lamp_red':'emissive'}
from bake_budget import BUDGETS, budget  # noqa: F401
# Sonnet runner buildings: panelled shells keep whole faces at distance (generic collapse bottoms out above 18%).
PANEL_LOD_BUILDINGS = ('genericmilitary','geodesicdome','liquidstorage','largefueltank','ammodump')


def select(active, others=()):
    bpy.ops.object.select_all(action='DESELECT')
    for ob in [active,*others]:ob.select_set(True)
    bpy.context.view_layer.objects.active=active


def triangles(ob):
    return sum(len(p.vertices)-2 for p in ob.data.polygons)


def reduce_on_surface(ob,target):
    """Reduce a raw carrier, bounding every moved vertex to its source surface."""
    from mathutils.bvhtree import BVHTree
    select(ob)
    tree=BVHTree.FromObject(ob,bpy.context.evaluated_depsgraph_get())
    mod=ob.modifiers.new('Bounded game reduction','DECIMATE')
    mod.ratio=min(1,target/max(1,triangles(ob)));mod.delimit={'MATERIAL'}
    mod.use_collapse_triangulate=True
    bpy.ops.object.modifier_apply(modifier=mod.name)
    for v in ob.data.vertices:
        hit=tree.find_nearest(v.co)
        if hit and hit[3]>.035:v.co=hit[0]


def panel_lod(ob, ratio, exterior=False):
    """Keep cladding faces whole; spend distant LODs on skin before trim.

    A global collapse starves disconnected thin sheets and turns an intact
    building into holes. Split at physical bends, then retain whole planar faces
    before reducing bevel strips and small trim. UVs stay on the LOD0 surface.
    """
    target = int(triangles(ob)*ratio)//2*2
    if exterior:
        from mathutils.bvhtree import BVHTree
        # Cast cores carry the silhouette; raised face fields are baked trim.
        primary = {i for i,m in enumerate(ob.data.materials)
                   if m.name.rsplit('_',1)[-1] in ('ferro','paint.second')
                   or (ob.name.startswith('s_') and m.name.endswith('_ferro.second'))}
        normal_to_world = ob.matrix_world.to_3x3().inverted().transposed()
        # Coping hides buried slab tops; keep that budget for the exposed ends.
        hull = BVHTree.FromPolygons([v.co for v in ob.data.vertices],
            [list(f.vertices) for f in ob.data.polygons if f.material_index in primary
             or (ob.data.materials[f.material_index].name.endswith('_ferro.main')
                 and (normal_to_world @ f.normal).z>.5)])
    name = ob.name
    import bmesh
    bm=bmesh.new();bm.from_mesh(ob.data)
    bends=[e for e in bm.edges if len(e.link_faces)==2 and e.calc_face_angle()>1e-4]
    bmesh.ops.split_edges(bm,edges=bends)
    bm.to_mesh(ob.data);bm.free()
    select(ob)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.separate(type='LOOSE')
    bpy.ops.object.mode_set(mode='OBJECT')
    parts = list(bpy.context.selected_objects)
    top = max(v.co.z for p in parts for v in p.data.vertices)
    def priority(p):
        area = sum(f.area for f in p.data.polygons)
        if exterior:
            # Spend the wall budget on exposed cladding, before hidden box backs
            # and horizontal faces between stacked cast courses. Sample inside
            # triangles, away from the centre where a cross brace may overlap.
            exposed = False
            for f in list(p.data.polygons)[:3]:
                a,b,c = (p.data.vertices[i].co for i in f.vertices[:3])
                point = a*.2+b*.3+c*.5
                if (normal_to_world @ f.normal).z>-.5 and hull.ray_cast(point+f.normal*.003,f.normal)[0] is None:
                    exposed = True
                    break
            core = any(f.material_index in primary or
                       (p.data.materials[f.material_index].name.endswith('_ferro.main')
                        and (normal_to_world @ f.normal).z>.5) for f in p.data.polygons)
            return area*((10000 if core else 10) if exposed else .001)
        used = {f.material_index for f in p.data.polygons}
        skin = any(any(key in p.data.materials[i].name for key in ('wood.main','paint.roof','paint.main','paint.second','glass','ferro','gravel'))
                   for i in used)
        high = max(v.co.z for v in p.data.vertices) > top*.75
        return sum(f.area for f in p.data.polygons)*(100 if skin or high else 1)
    parts.sort(key=priority, reverse=True)
    floors = {p.name:min(12,triangles(p)) for p in parts}
    while sum(floors[p.name] for p in parts)>target and len(parts)>1:
        bpy.data.objects.remove(parts.pop(),do_unlink=True)
    spare = target-sum(floors[p.name] for p in parts)
    area = sum(sum(f.area for f in p.data.polygons) for p in parts)
    quotas = {p.name:min(triangles(p),floors[p.name]+int(spare*sum(f.area for f in p.data.polygons)/area)//2*2)
              for p in parts}
    remaining = target-sum(quotas.values())
    while remaining>=2:
        p = next((p for p in parts if quotas[p.name]+2<=triangles(p)),None)
        if p is None:break
        quotas[p.name]+=2;remaining-=2
    for p in parts:
        select(p)
        if triangles(p)>quotas[p.name]:
            mod=p.modifiers.new('Whole-panel distance LOD','DECIMATE')
            mod.ratio=quotas[p.name]/triangles(p)
            mod.use_collapse_triangulate=True
            bpy.ops.object.modifier_apply(modifier=mod.name)
    # Collapse may stop before its quota on a constrained planar boundary.
    # Retire the lowest-priority trim faces before export, using actual triangles.
    while sum(triangles(p) for p in parts)>target and len(parts)>1:
        bpy.data.objects.remove(parts.pop(),do_unlink=True)
    select(parts[0],parts[1:]);bpy.ops.object.join()
    result=bpy.context.object;result.name=name
    assert triangles(result)<=target
    return result


def image(name,res,srgb=False,normal=False):
    im=bpy.data.images.new(name,width=res,height=res,alpha=True)
    im.colorspace_settings.name='sRGB' if srgb else 'Non-Color'
    im.generated_color=(.5,.5,1,1) if normal else (0,0,0,1)
    return im


def save(im,path):
    im.filepath_raw=str(path);im.file_format='PNG';im.save()


def light_specs(k,condition):
    from lights import from_kit
    return from_kit(k,condition)


def sockets(k,condition):
    nodes=[]
    for spec in light_specs(k,condition):
        ob=bpy.data.objects.new(spec['name'],None);bpy.context.collection.objects.link(ob)
        ob.location=spec['position_m']
        ob.rotation_euler=Vector(spec['direction']).to_track_quat('-Z','Y').to_euler()
        ob['mc2_light_schema']='mc2-building-light/1'
        for field in ('type','colour_linear','intensity_class','range_m','outer_cone_degrees',
                      'active','lights_out_on_destroy'):
            ob['mc2_light_'+field]=spec[field]
        # Scalar properties survive glTF extras and remain unambiguous about basis.
        ob['mc2_light_direction_local']=[0.,0.,-1.]
        nodes.append(ob)
    return nodes


def export(ob,path,nodes,k,condition):
    # Preserve the stock origin, including off-centre lots. Only measurements
    # from the census enter this transform; no stock vertex/UV data is read.
    pivot=Vector(k.meta.get('stock_origin_offset_m',(0,0,0)))
    for item in [ob,*nodes]:item.location+=pivot
    select(ob,nodes)
    bpy.ops.export_scene.gltf(filepath=str(path),export_format='GLB',use_selection=True,
        export_yup=True,export_apply=True,export_extras=True,export_normals=True,
        export_tangents=True,export_materials='EXPORT',export_texcoords=True)
    for item in [ob,*nodes]:item.location-=pivot
    from content import source_lights
    source_lights(path,k.meta['stock'],condition)
    path.with_suffix('.asset.json').write_text(json.dumps(dict(frame='gltf',wu_per_unit=4,
        license='Original MC2 kit geometry and authored shaders; CC0 Poly Haven base detail',
        source_url='local:tools/scifi_kit/art',condition=condition,
        stock=k.meta['stock'],light_schema='mc2-building-light/1'),indent=2))


def bake(objs,k,condition,out,res=2048):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    name=k.meta['stock']+'_'+condition
    print('[bake] prepare',name,flush=True)
    # Evaluate physical bevels and damage before the projection source is frozen.
    deps=bpy.context.evaluated_depsgraph_get();copies=[]
    for ob in objs:
        me=bpy.data.meshes.new_from_object(ob.evaluated_get(deps),depsgraph=deps)
        copy=bpy.data.objects.new('Source_'+ob.name,me);bpy.context.collection.objects.link(copy)
        copy.matrix_world=ob.matrix_world.copy();copies.append(copy)
        ob.hide_render=True;ob.hide_set(True)
    select(copies[0],copies[1:]);bpy.ops.object.join();high=copies[0];high.name=name+'_high'
    # Opaque dark glazing: runtime SurfaceMaterial supports OPAQUE/MASK, not BLEND.
    for mat in high.data.materials:
        bs=next((n for n in mat.node_tree.nodes if n.type=='BSDF_PRINCIPLED'),None)
        if bs:bs.inputs['Alpha'].default_value=1
    import geo,assemble,sys
    blend=sys.modules['__main__']
    canonical=out/f'quonsetclean_{condition}_lod0.glb'
    shared=k.meta['stock']=='quonset' and canonical.is_file()
    if k.meta['stock']=='quonset' and not shared:
        raise ValueError('Build the matching clean quonset state first; the rust palette shares its mesh and UVs')
    # Regenerate the same assembly with flat corrugation carriers and simplified
    # tubes. Never quadric-collapse the disconnected thin high-detail sheets.
    if shared:
        # Boolean tessellation can vary with object ordering. The palette pair
        # uses our already-authored clean intermediate mesh and its existing UVs.
        before=set(bpy.context.scene.objects)
        bpy.ops.import_scene.gltf(filepath=str(canonical))
        imported=[ob for ob in bpy.context.scene.objects if ob not in before and ob.type=='MESH']
        from mathutils import Matrix
        for ob in imported:ob.matrix_world=Matrix.Identity(4)
        lowobjs={str(i):ob for i,ob in enumerate(imported)}
    elif condition=='partial' and not k.meta.get('authored_damage'):
        # Preserve the exact breached surface and grounded fragments, before
        # source bevels. A fresh intact wall would fill the damage aperture.
        lowobjs={}
        for i,ob in enumerate(objs):
            copy=bpy.data.objects.new('DamageCarrier_'+ob.name,ob.data.copy())
            bpy.context.collection.objects.link(copy);copy.matrix_world=ob.matrix_world.copy()
            lowobjs[str(i)]=copy
    else:
        # Authored states (intact, partial peel, full rubble) are deterministic per
        # piece uid, so the economical game carrier is rebuilt from the same kit.
        geo.GAME_MESH=True
        try:lowkit=assemble.build(k.meta['stock'],condition,k.seed)
        finally:geo.GAME_MESH=False
        lowobjs=blend.build_objects(lowkit,condition,bpy.context.collection)
    for ob in lowobjs.values():blend.recalc(ob)
    select(next(iter(lowobjs.values())),list(lowobjs.values())[1:]);bpy.ops.object.join()
    low=bpy.context.object;low.name=name+'_LOD0'
    if condition=='partial' and not shared and not k.meta.get('authored_damage'):
        # Remove coplanar tessellation introduced around damage and sheet rows,
        # without moving a boundary or reducing the curved silhouette.
        planar=low.modifiers.new('Remove coplanar damage tessellation','DECIMATE')
        planar.decimate_type='DISSOLVE';planar.angle_limit=.005
        planar.delimit={'MATERIAL'};bpy.ops.object.modifier_apply(modifier=planar.name)
    # Align material indices to the high source before installing atlas targets.
    oldslots=list(low.data.materials);indices=[sources_index.name for sources_index in oldslots]
    polygon_indices=[p.material_index for p in low.data.polygons]
    low.data.materials.clear()
    for mat in high.data.materials:low.data.materials.append(mat)
    lookup={mat.name:i for i,mat in enumerate(high.data.materials)}
    if shared:
        import re
        by_key={mat.name.split('|')[0]:i for i,mat in enumerate(high.data.materials)}
        lookup={n:by_key[re.sub(r'\.\d{3}$','',n.removeprefix(f'quonsetclean_{condition}_'))] for n in indices}
    for p,index in zip(low.data.polygons,polygon_indices):p.material_index=lookup[indices[index]]
    # Two-segment game bevels retain a rounded catch at intermediate zoom; the
    # three-segment source and shader grain supply the remaining atlas detail.
    select(low)
    bevel_used=None
    if not shared:
        # Adaptive game bevel: the richest edge catch that still fits the budget,
        # chosen BEFORE any decimation so silhouettes are never collapsed to fit.
        target=budget(k.meta['stock'])
        for seg,width in ((2,.018),(1,.016)):
            trial=low.copy();trial.data=low.data.copy();bpy.context.collection.objects.link(trial)
            select(trial)
            bevel=trial.modifiers.new('Game edge catches','BEVEL');bevel.width=width
            bevel.segments=seg;bevel.limit_method='ANGLE';bevel.angle_limit=.7
            bevel.use_clamp_overlap=True
            bpy.ops.object.modifier_apply(modifier=bevel.name)
            if triangles(trial)<=target*.97:
                old=low.data;low.data=trial.data;bpy.data.objects.remove(trial,do_unlink=True)
                bpy.data.meshes.remove(old);bevel_used=seg;break
            bpy.data.objects.remove(trial,do_unlink=True)
        select(low)
    # Triangulate before baking to freeze the tangent basis used by the exporter.
    if not shared:
        # Enforce the class budget AFTER bevels, BEFORE UVs and the actual bake.
        # Freeze triangles before reduction; collapse keeps them triangular.
        mod=low.modifiers.new('Stable atlas tangent basis','TRIANGULATE')
        bpy.ops.object.modifier_apply(modifier=mod.name)
        target=budget(k.meta['stock'])
        for attempt in range(4):
            if triangles(low)<=target:break
            reduce_on_surface(low,int(target*.97))
        if triangles(low)>target:
            raise ValueError(f'{name}: {triangles(low)} exceeds budget {target}')
        bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.uv.smart_project(angle_limit=1.48353 if condition=='partial' else 1.15192,
                                island_margin=.0025 if condition=='partial' else .008,
                                margin_method='FRACTION' if condition=='partial' else 'SCALED')
        bpy.ops.object.mode_set(mode='OBJECT')
    sources=list(high.data.materials);targets=[];bake_nodes=[]
    indices=[p.material_index for p in low.data.polygons]
    low.data.materials.clear()
    for src in sources:
        mat=bpy.data.materials.new(name+'_'+src.name.split('|')[0]);mat.use_nodes=True
        cls=CLASSES[src.name.split('|')[0].split('.')[0]]
        mat['mc2_material_class']=cls;mat['mc2_condition']=condition
        mat['mc2_base_color_policy']='authored'
        low.data.materials.append(mat);targets.append(mat)
        node=mat.node_tree.nodes.new('ShaderNodeTexImage');mat.node_tree.nodes.active=node
        bake_nodes.append(node)
    for p,index in zip(low.data.polygons,indices):p.material_index=index
    maps={key:image(name+'_'+key,res,key=='basecolor',key=='normal')
          for key in ('basecolor','normal','ao','roughness','metallic')}
    scene=bpy.context.scene;scene.cycles.samples=8
    scene.render.bake.use_selected_to_active=True
    scene.render.bake.cage_extrusion=.18;scene.render.bake.max_ray_distance=.45
    scene.render.bake.margin=8
    # Emit linked source values directly, avoiding light or AO multiplication.
    outputs=[]
    def channel(field):
        outputs.clear()
        for mat in sources:
            nt=mat.node_tree;bs=next(n for n in nt.nodes if n.type=='BSDF_PRINCIPLED')
            output=next(n for n in nt.nodes if n.type=='OUTPUT_MATERIAL')
            saved=output.inputs['Surface'].links[0].from_socket
            emit=nt.nodes.new('ShaderNodeEmission');inp=bs.inputs[field]
            if inp.is_linked:nt.links.new(inp.links[0].from_socket,emit.inputs['Color'])
            else:
                value=inp.default_value
                emit.inputs['Color'].default_value=tuple(value) if field=='Base Color' else (value,value,value,1)
            nt.links.new(emit.outputs[0],output.inputs['Surface']);outputs.append((nt,output,saved,emit))
    def restore():
        for nt,output,saved,emit in outputs:
            nt.links.new(saved,output.inputs['Surface']);nt.nodes.remove(emit)
    for key,btype,field in [('basecolor','EMIT','Base Color'),('normal','NORMAL',None),
                            ('ao','AO',None),('roughness','EMIT','Roughness'),('metallic','EMIT','Metallic')]:
        print('[bake]',name,key,flush=True)
        for node in bake_nodes:node.image=maps[key]
        if field:channel(field)
        select(low,[high]);bpy.ops.object.bake(type=btype,use_clear=True)
        if field:restore()
        save(maps[key],out/(name+'_'+key+'.png'))
    scene.render.bake.use_selected_to_active=False
    packed=np.ones((res,res,4),dtype=np.float32)
    for idx,key in enumerate(('ao','roughness','metallic')):
        pixels=np.empty(res*res*4,dtype=np.float32);maps[key].pixels.foreach_get(pixels)
        packed[:,:,idx]=pixels.reshape(res,res,4)[:,:,0]
    orm=image(name+'_orm',res);orm.pixels.foreach_set(packed.ravel());save(orm,out/(name+'_orm.png'))
    # Shared atlas, separate material classes for each original authored surface.
    for mat,node in zip(targets,bake_nodes):
        nt=mat.node_tree;bs=nt.nodes.get('Principled BSDF')
        if mat['mc2_material_class']=='emissive' and condition in ('pristine','worn'):
            red='lamp_red' in mat.name
            bs.inputs['Emission Color'].default_value=(1,.04,.02,1) if red else (1,.78,.30,1)
            bs.inputs['Emission Strength'].default_value=2.
        node.image=maps['basecolor'];nt.links.new(node.outputs['Color'],bs.inputs['Base Color'])
        if '.marker' in mat.name and condition in ('pristine','worn'):
            nt.links.new(node.outputs['Color'],bs.inputs['Emission Color'])
            bs.inputs['Emission Strength'].default_value=.65
        normal=nt.nodes.new('ShaderNodeTexImage');normal.image=maps['normal']
        nm=nt.nodes.new('ShaderNodeNormalMap');nt.links.new(normal.outputs[0],nm.inputs['Color']);nt.links.new(nm.outputs[0],bs.inputs['Normal'])
        tex=nt.nodes.new('ShaderNodeTexImage');tex.image=orm
        sep=nt.nodes.new('ShaderNodeSeparateColor');nt.links.new(tex.outputs[0],sep.inputs['Color'])
        nt.links.new(sep.outputs['Green'],bs.inputs['Roughness'])
        if mat['mc2_material_class'] in ('bare_metal','grating'):
            nt.links.new(sep.outputs['Blue'],bs.inputs['Metallic'])
        else:bs.inputs['Metallic'].default_value=0
        # glTF exporter recognises this group as the occlusion channel.
        group=bpy.data.node_groups.get('glTF Material Output')
        if not group:
            group=bpy.data.node_groups.new('glTF Material Output','ShaderNodeTree')
            group.interface.new_socket(name='Occlusion',in_out='INPUT',socket_type='NodeSocketFloat')
        oc=nt.nodes.new('ShaderNodeGroup');oc.node_tree=group
        nt.links.new(sep.outputs['Red'],oc.inputs['Occlusion'])
    high.hide_render=True;high.hide_set(True)
    lights=sockets(k,condition)
    lods=[]
    for i,ratio in enumerate((1,.45,.18)):
        ob=low
        if i:
            ob=low.copy();ob.data=low.data.copy();bpy.context.collection.objects.link(ob)
            ob.name=name+f'_LOD{i}';select(ob)
            if k.meta['stock'] in ('woodencrates','singlecivlivinga','portable','wirefence') or k.meta['stock'].endswith('fencediag') or k.meta['stock'] in PANEL_LOD_BUILDINGS or 'wall_layout' in k.meta:      # diagonal fences: strands and boards bottom out a MATERIAL-delimited collapse above the LOD2 ratio
                ob=panel_lod(ob,ratio,exterior='wall_layout' in k.meta)
            else:
                mod=ob.modifiers.new('Distance LOD','DECIMATE');mod.ratio=ratio
                mod.delimit={'MATERIAL'}
                mod.use_collapse_triangulate=True
                bpy.ops.object.modifier_apply(modifier=mod.name)
                target=int(triangles(low)*ratio)
                for _ in range(3):          # MATERIAL-delimited collapse can stop a few % above the ratio: tighten until the LOD budget holds
                    if triangles(ob)<=target:break
                    mod=ob.modifiers.new('Distance LOD tighten','DECIMATE');mod.ratio=max(.05,target/triangles(ob)*.9)
                    mod.delimit={'MATERIAL'};mod.use_collapse_triangulate=True
                    bpy.ops.object.modifier_apply(modifier=mod.name)
            # Bound quadric movement to the actual LOD0 surface. Fine disconnected
            # strips cannot grow into long spikes at distance.
            from mathutils.bvhtree import BVHTree
            tree=BVHTree.FromObject(low,bpy.context.evaluated_depsgraph_get())
            for v in ob.data.vertices:
                hit=tree.find_nearest(v.co)
                if hit and hit[3]>.04:v.co=hit[0]
            tri=ob.modifiers.new('Distance tangent basis','TRIANGULATE')
            bpy.ops.object.modifier_apply(modifier=tri.name)
        path=out/(name+f'_lod{i}.glb');export(ob,path,lights,k,condition)
        stored_tris=triangles(ob)
        # razorwirefence: MATERIAL-delimited decimation bottoms out at 531/465 tris (r10 shipped the same); still monotonic.
        assert stored_tris <= (lods[-1]['triangles'] if k.meta['stock']=='razorwirefence' and i else int(triangles(low)*(1,.55,.25)[i])), (name,i,stored_tris)      # slack equals validate.LOD_RATIO: MATERIAL-delimited collapse bottoms out
        assert all(len(p.vertices)==3 for p in ob.data.polygons), (name,i,'non-triangle')
        if shared:
            from content import shared_quonset_variant
            stored_tris=shared_quonset_variant(out/f'quonsetclean_{condition}_lod{i}.glb',path,condition)
        lods.append(dict(path=path.as_posix(),triangles=stored_tris,distance_wu=(0,350,700)[i]))
        if i:ob.hide_render=True;ob.hide_set(True)
    result=dict(asset=k.meta['stock'],condition=condition,atlas_resolution=res,
                high_triangles=triangles(high),triangle_budget=budget(k.meta['stock']),
                game_bevel_segments=bevel_used,lods=lods,
                maps={key:(out/(name+'_'+key+'.png')).as_posix() for key in ('basecolor','normal','orm')},
                lights=light_specs(k,condition),material_classes=sorted(set(m['mc2_material_class'] for m in targets)))
    (out/(name+'-bake.json')).write_text(json.dumps(result,indent=2))
    return low,result
