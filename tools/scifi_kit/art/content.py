"""Validate/bundle authored GLBs against the parked content-pipeline contract.

Reads that branch without editing it. Writes IR, a cooker recipe, light metadata
and validation evidence under the supplied E: output. No install or game access.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys

import numpy as np

import counts
import drafts
from lights import from_kit


def document(path):
    raw=Path(path).read_bytes();length,kind=struct.unpack_from('<II',raw,12)
    assert kind==0x4e4f534a
    return json.loads(raw[20:20+length]),raw[20+length:]


def write_document(path,doc,tail):
    body=json.dumps(doc,separators=(',',':')).encode();body+=b' '*(-len(body)%4)
    Path(path).write_bytes(struct.pack('<III',0x46546c67,2,20+len(body)+len(tail))+
                          struct.pack('<II',len(body),0x4e4f534a)+body+tail)


def repair_tangents(doc,tail):
    """Supply an orthonormal frame where the exporter emits zero UV tangents.

    Prefer the incident triangles' UV derivatives. Only a UV-degenerate vertex
    uses a deterministic normal-orthogonal fallback. Positions/UVs stay intact.
    """
    binary=bytearray(tail);done=set();repaired=0
    def array(index):
        a=doc['accessors'][index];v=doc['bufferViews'][a['bufferView']]
        width={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[a['type']]
        dtype=np.dtype({5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1'}[a['componentType']])
        return np.ndarray((a['count'],width),dtype=dtype,buffer=binary,
            offset=8+v.get('byteOffset',0)+a.get('byteOffset',0),
            strides=(v.get('byteStride',width*dtype.itemsize),dtype.itemsize))
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            attrs=p['attributes'];aid=attrs.get('TANGENT')
            if aid is None or aid in done:continue
            done.add(aid);t=array(aid);n=array(attrs['NORMAL']).astype(float)
            n/=np.maximum(np.linalg.norm(n,axis=1,keepdims=True),1e-20)
            tangent=t[:,:3].astype(float);tangent-=n*np.sum(n*tangent,axis=1,keepdims=True)
            lens=np.linalg.norm(tangent,axis=1);bad=(lens<1e-7)|(abs(lens-1)>.001)|(~np.isin(t[:,3],[-1,1]))
            if not bad.any():continue
            repaired+=int(bad.sum())
            pos=array(attrs['POSITION']).astype(float);uv=array(attrs['TEXCOORD_0']).astype(float)
            idx=array(p['indices']).reshape(-1,3).astype(int)
            dp1=pos[idx[:,1]]-pos[idx[:,0]];dp2=pos[idx[:,2]]-pos[idx[:,0]]
            du1=uv[idx[:,1]]-uv[idx[:,0]];du2=uv[idx[:,2]]-uv[idx[:,0]]
            det=du1[:,0]*du2[:,1]-du1[:,1]*du2[:,0];ok=abs(det)>1e-12
            ts=np.zeros_like(pos);bs=np.zeros_like(pos)
            ft=(dp1[ok]*du2[ok,1,None]-dp2[ok]*du1[ok,1,None])/det[ok,None]
            fb=(-dp1[ok]*du2[ok,0,None]+dp2[ok]*du1[ok,0,None])/det[ok,None]
            for corner in range(3):
                np.add.at(ts,idx[ok,corner],ft);np.add.at(bs,idx[ok,corner],fb)
            ts-=n*np.sum(n*ts,axis=1,keepdims=True)
            tangent[bad]=ts[bad]
            zero=np.linalg.norm(tangent,axis=1)<1e-7
            axes=np.eye(3)[np.argmin(abs(n[zero]),axis=1)]
            tangent[zero]=np.cross(axes,n[zero])
            tangent/=np.maximum(np.linalg.norm(tangent,axis=1,keepdims=True),1e-20)
            signs=np.where(np.sum(np.cross(n,tangent)*bs,axis=1)<0,-1,1)
            t[bad,:3]=tangent[bad];t[bad,3]=signs[bad]
    doc.setdefault('extras',{})['mc2_tangent_frame_repairs']=doc.get('extras',{}).get('mc2_tangent_frame_repairs',0)+repaired
    return bytes(binary)


def light_node(spec,offset):
    # Blender Z-up -> standard glTF Y-up. Direction is stored in the node extras
    # in the same source frame; the source node retains the true light position.
    x,y,z=spec['position_m']
    ox,oy,oz=offset
    dx,dy,dz=spec['direction']
    extra={f'mc2_light_{k}':v for k,v in spec.items() if k not in ('name','position_m','direction')}
    extra.update(mc2_light_schema='mc2-building-light/1',
                 mc2_light_position_gltf_m=[float(x+ox),float(z+oz),float(y-oy)],
                 mc2_light_direction_gltf=[dx,dz,dy])
    return dict(name=spec['name'],translation=[float(x+ox),float(z+oz),float(y-oy)],extras=extra)


def source_lights(path,name,condition):
    doc,tail=document(path)
    # The exported sockets are leaf empties. Rebuild them deterministically from
    # the pure authoring schema, including end-window points added after bake.
    old=doc.get('nodes',[]);keep=[i for i,n in enumerate(old) if not n.get('name','').startswith('LIGHT_')]
    remap={oldi:newi for newi,oldi in enumerate(keep)}
    nodes=[]
    for i in keep:
        node=old[i]
        if 'children' in node:node['children']=[remap[c] for c in node['children'] if c in remap]
        nodes.append(node)
    for scene in doc['scenes']:scene['nodes']=[remap[i] for i in scene['nodes'] if i in remap]
    specs=from_kit(drafts.build(name,condition),condition)
    for spec in specs:
        node=light_node(spec,counts.origin(name))
        spec['position_gltf_m']=node['extras']['mc2_light_position_gltf_m']
        spec['direction_gltf']=node['extras']['mc2_light_direction_gltf']
        spec['position_tg_wu']=[v*4 for v in spec['position_gltf_m']]
        spec['direction_tg']=spec['direction_gltf']
        spec['range_wu']=spec['range_m']*4
        doc['scenes'][doc.get('scene',0)]['nodes'].append(len(nodes))
        nodes.append(node)
    doc['nodes']=nodes
    if not doc.get('extras',{}).get('mc2_stock_axis_alignment'):
        # Counts/studies use centred TG (X,depth,height). Blender's Y-up exporter
        # maps depth to -Z. Mirror only the mesh's local Z to restore TG +depth;
        # retain the measured stock-pivot translation. The IR bakes negative
        # determinant transforms with corrected winding/tangent handedness.
        for node in nodes:
            if 'mesh' in node:
                scale=list(node.get('scale',[1,1,1]));scale[2]*=-1;node['scale']=scale
    doc.setdefault('extras',{}).update(mc2_stock_asset=name,mc2_count_schema='mc2-stock-counts/1',
                                     mc2_feature_count_contract=counts.CONTRACTS[name],
                                     mc2_stock_axis_alignment='TG-depth/1')
    tail=repair_tangents(doc,tail)
    write_document(path,doc,tail)
    return specs


def shared_quonset_variant(source,target,condition):
    """Share our OWN clean geometry/UV buffers; use newly baked rusty atlases."""
    doc,tail=document(source)
    for collection in ('materials','meshes','nodes'):
        for item in doc.get(collection,[]):
            if 'name' in item:item['name']=item['name'].replace('quonsetclean','quonset')
    for node in doc['nodes']:
        if 'mesh' in node:
            x,y,z=counts.origin('quonset');node['translation']=[x,z,-y]
    doc['images']=[dict(uri=f'quonset_{condition}_{suffix}.png') for suffix in ('basecolor','normal','orm')]
    doc['textures']=[dict(source=i) for i in range(3)]
    for mat in doc['materials']:
        cls=mat['extras']['mc2_material_class']
        mat['pbrMetallicRoughness']=dict(baseColorTexture=dict(index=0),baseColorFactor=[1,1,1,1],
            metallicRoughnessTexture=dict(index=2),metallicFactor=1 if cls in ('bare_metal','grating') else 0,
            roughnessFactor=1)
        mat['normalTexture']=dict(index=1);mat['occlusionTexture']=dict(index=2)
    doc.setdefault('extras',{})['mc2_shared_original_geometry']=str(source)
    write_document(target,doc,tail);source_lights(target,'quonset',condition)
    return sum(doc['accessors'][p['indices']]['count']//3 for m in doc['meshes'] for p in m['primitives'])


def deduplicate(asset):
    # Each LOD/condition import appends slots. Coalesce identical atlas/class
    # slots so the cooker does not encode the same atlas once per LOD or trim.
    unique=[];seen={};remap={}
    for index,mat in enumerate(asset.materials):
        key=(mat.material_class,mat.alpha_mode,mat.metallic,mat.roughness,
             tuple((getattr(mat,k).sha256 if getattr(mat,k) else None) for k in ('base_color','normal','orm')))
        if key not in seen:seen[key]=len(unique);unique.append(mat)
        remap[index]=seen[key]
    for _,_,lods in asset.variants():
        for lod in lods:
            for mesh in lod.scene.meshes:
                for p in mesh.primitives:p.material=remap[p.material]
    asset.materials=unique
    return asset.check()


def prepare(source,pipeline,out,only=None):
    sys.dont_write_bytecode=True
    sys.path.insert(0,str(pipeline))
    from tools.content_pipeline import gltf_io,ir,validate
    from tools.content_pipeline.frontends import gltf_file
    out.mkdir(parents=True,exist_ok=True)
    recipe=dict(mod_id='mc2-stock-kit-r5',name='MC2 stock art upgrades',
                out=(out/'package').as_posix(),assets=[])
    assets=[];proof=[];lights={}
    for name in (only or drafts.DRAFTS):
        folder=source/'pbr'
        if not (folder/f'{name}_pristine_lod0.glb').is_file():continue        # params-driven assets join only passes that baked them
        files={cond:[folder/f'{name}_{cond}_lod{i}.glb' for i in range(3)]
               for cond in ('pristine','worn','abandoned','partial','destroyed')}
        evidence=dict(asset=name,files=[],lights={})
        for cond,paths in files.items():
            for path in paths:
                specs=source_lights(path,name,cond)
                issues=validate.validate_gltf(path)
                if validate.errors(issues):raise ValueError(f'{path}: {validate.errors(issues)}')
                doc,_=document(path)
                if not doc.get('images'):raise ValueError(f'{path}: no baked atlas')
                _,buffers,_=gltf_io.load_document(path)
                geometry=hashlib.sha256();triangles=0
                for mesh in doc['meshes']:
                    for p in mesh['primitives']:
                        if not {'POSITION','NORMAL','TEXCOORD_0','TANGENT'}<=set(p['attributes']):
                            raise ValueError(f'{path}: missing tangent-space vertex data')
                        for field in ('POSITION','NORMAL','TANGENT','TEXCOORD_0'):
                            geometry.update(gltf_io.read_accessor(doc,buffers,p['attributes'][field]).tobytes())
                        indices=gltf_io.read_accessor(doc,buffers,p['indices'])
                        geometry.update(indices.tobytes());triangles+=len(indices)//3
                evidence['files'].append(dict(path=path.as_posix(),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                             geometry_sha256=geometry.hexdigest(),triangles=triangles,issues=issues))
            evidence['lights'][cond]=len(specs)
            lights.setdefault(name,{})[cond]=specs
        model=next(m for m in counts.catalog()[name]['models'] if m['role']=='intact' and m.get('lod')==0)
        spec=dict(frontend='gltf',id=name,replaces='staticProp:'+Path(model['tgl']).stem,
                  kind='building' if name in drafts.BUILDINGS else 'prop',
                  intact=files['pristine'][0].as_posix(),destroyed=files['destroyed'][0].as_posix(),
                  lods=[[p.as_posix(),distance] for p,distance in zip(files['pristine'][1:],(350,700))],
                  frame='gltf',wu_per_unit=4,texel_scale=4,repair_inward_hull=True,
                  license='Original kit geometry/shaders; CC0 Poly Haven detail',source_url='local:tools/scifi_kit/art',
                  conditions={c:dict(intact=files[c][0].as_posix(),
                                     lods=[[p.as_posix(),d] for p,d in zip(files[c][1:],(350,700))],
                                     destroyed=files['destroyed'][0].as_posix()) for c in ('worn','abandoned')})
        recipe['assets'].append(spec)
        meta={k:spec[k] for k in ('frame','wu_per_unit','license','source_url')}
        asset=gltf_file.to_ir(name,spec['replaces'],spec['intact'],kind=spec['kind'],
                lods=[tuple(x) for x in spec['lods']],destroyed=spec['destroyed'],
                conditions=spec['conditions'],meta=meta,work_dir=out/'work'/name,
                texel_scale=4,repair_inward_hull=True)
        # The parked frontend exposes one destroyed LOD; IR supports the full chain.
        for path,distance in zip(files['destroyed'][1:],(350,700)):
            scene,mats,_=gltf_file.read_scene(path,meta,out/'work'/name/'images')
            asset.materials=gltf_file.merge_materials(asset.materials,mats,scene)
            asset.states['destroyed'].append(ir.Lod(scene,distance))
        # Authoritative original maps avoid exporter MR repacking losing AO when
        # a dielectric slot's metallic factor is zero.
        for mat in asset.materials:
            cond=next(c for c in ('pristine','worn','abandoned','destroyed') if mat.name.startswith(name+'_'+c+'_'))
            for attr,suffix in [('base_color','basecolor'),('normal','normal'),('orm','orm')]:
                matref=ir.TextureRef.of(folder/f'{name}_{cond}_{suffix}.png',attr)
                setattr(mat,attr,matref)
            mat.base_color_factor=(1,1,1,1);mat.roughness=1
            mat.metallic=1 if mat.material_class in ('bare_metal','grating') else 0
        asset=deduplicate(asset)
        for states in asset.conditions.values():
            states['destroyed']=copy.deepcopy(asset.states['destroyed'])
        # Socket is attach in current IR; light semantics are a versioned sidecar
        # because that branch has no native light kind/property payload yet.
        asset.sockets=[ir.Socket(s['name'],'attach',s['name']) for s in lights[name]['pristine']]
        asset.frontend['light_socket_schema']='mc2-building-light/1'
        asset.frontend['light_sockets']=lights[name]
        asset.frontend['stock_feature_count_contract']=counts.CONTRACTS[name]
        asset.frontend['intermediate_partial']=[p.as_posix() for p in files['partial']]
        asset.check();ir.save(asset,out/'ir'/f'{name}.json');assets.append(asset)
        evidence.update(material_slots=len(asset.materials),ir=(out/'ir'/f'{name}.json').as_posix())
        proof.append(evidence)
    clean=next((p for p in proof if p['asset']=='quonsetclean'),None)
    rusty=next((p for p in proof if p['asset']=='quonset'),None)
    matches=[a['geometry_sha256']==b['geometry_sha256'] for a,b in zip(clean['files'],rusty['files'])] if clean and rusty else []
    if not all(matches):
        bad=[Path(a['path']).name for a,ok in zip(clean['files'],matches) if not ok]
        raise ValueError('clean/rusty quonset geometry or UVs differ: '+', '.join(bad))
    (out/'recipe.json').write_text(json.dumps(recipe,indent=2))
    (out/'light-sockets.json').write_text(json.dumps(dict(schema='mc2-building-light/1',assets=lights),indent=2))
    (out/'validation.json').write_text(json.dumps(proof,indent=2))
    return assets


def main():
    import psutil
    proc=psutil.Process();proc.cpu_affinity(list(range(psutil.cpu_count()))[2:4]);proc.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,required=True)
    ap.add_argument('--pipeline',type=Path,default=Path('A:/Games/mc2-flash4'))
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--cook',action='store_true')
    ap.add_argument('--only',help='comma list of assets (batch validation)')
    args=ap.parse_args();assets=prepare(args.source,args.pipeline,args.out,args.only.split(',') if args.only else None)
    if args.cook:
        from tools.content_pipeline import package
        package.build_package(assets,args.out/'package',mod_id='mc2-stock-kit-r5',name='MC2 stock art upgrades')
    print(f'{len(assets)} assets validated in IR; {args.out}',flush=True)


if __name__=='__main__':main()
