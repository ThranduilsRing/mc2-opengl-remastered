#!/usr/bin/env python3
"""One-command local procedural building review and model override installation."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(HERE))
sys.path.insert(0,str(ROOT/'scripts'))
sys.path.insert(0,str(ROOT/'tools/blender_pipeline'))
from core import DEFAULT_OUT, make_job, read_json, write_json, inside
from blender_runner import find_blender_exe
import console_child

PBR=Path('G:/mc2-assets/pbr/scifi-kit')
REF=Path('G:/mc2-assets/ref/quality-bar')
SHEETS=Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/sheets/thumbs')
INSTALL=Path('G:/mc2-installs/mc2-win64-mapgen')


def run_blender(out,arguments):
    out.parent.mkdir(parents=True,exist_ok=True)
    cmd=[str(find_blender_exe()),'--background','--factory-startup','--disable-autoexec','--python-exit-code','1','--python',str(HERE/'blender_build.py'),'--',*arguments]
    print('[scifi]', ' '.join(cmd),flush=True)
    with out.open('w',encoding='utf-8') as log:
        result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,**console_child.child_kwargs(redirected=True))
    if result.returncode:
        print(out.read_text(encoding='utf-8',errors='replace')[-6500:])
        raise RuntimeError(f'Blender failed ({result.returncode}); log: {out}')


def prepare(out):
    """Copy only CC0 maps from the owner's read-only library; no network access."""
    source=Path('C:/Users/Joe/Downloads/GameAsset')
    result={};evidence=[]
    for zone,name in [('paint','PaintedMetal002'),('corrugated','CorrugatedSteel006A')]:
        dest=PBR/name;dest.mkdir(parents=True,exist_ok=True)
        maps={}
        for channel,suffix in [('color','Color'),('normal','NormalGL'),('roughness','Roughness'),('metallic','Metalness')]:
            src=source/'Materials'/f'{name}_1K-PNG'/f'{name}_1K-PNG_{suffix}.png'
            if not src.is_file():raise FileNotFoundError(src)
            target=dest/src.name
            if not target.is_file():shutil.copy2(src,target)
            maps[channel]=str(target)
            evidence.append({'source':str(src),'copy':str(target),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'license':'CC0 / ambientCG (local inventory)'})
        result[zone]=maps
    archive=source/'Terrain/concrete_wall_003_2k.gltf.zip'
    dest=PBR/'concrete_wall_003';dest.mkdir(parents=True,exist_ok=True);maps={}
    with zipfile.ZipFile(archive) as z:
        for channel,token in [('color','_diff_'),('normal','_nor_gl_'),('roughness','_rough_')]:
            candidates=[n for n in z.namelist() if token in n and n.lower().endswith(('.png','.jpg','.exr'))]
            if not candidates:raise ValueError(f'{archive}: missing {token}')
            name=sorted(candidates,key=lambda n:not n.endswith('.png'))[0];target=dest/Path(name).name
            if not target.is_file():target.write_bytes(z.read(name))
            maps[channel]=str(target);evidence.append({'source':str(archive)+'::'+name,'copy':str(target),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'license':'CC0 / Poly Haven (local inventory)'})
    result['concrete']=maps
    write_json(out/'texture_sources.json',result);write_json(PBR/'provenance.json',evidence)
    if not (out/'reference/bt_industrial.png').is_file():
        (out/'reference').mkdir(parents=True,exist_ok=True)
        run_blender(out/'logs/reference.log',['--reference',str(out/'reference/bt_industrial.png')])
    return result


def glb_read(path):
    data=Path(path).read_bytes();length=struct.unpack_from('<I',data,12)[0]
    return json.loads(data[20:20+length]),data[20+length:]


def glb_write(path,g,tail):
    body=json.dumps(g,separators=(',',':')).encode();body+=b' '*((-len(body))%4)
    Path(path).write_bytes(struct.pack('<4sII',b'glTF',2,20+len(body)+len(tail))+struct.pack('<I4s',len(body),b'JSON')+body+tail)


def patch_textures(path,name):
    g,tail=glb_read(path)
    g['images']=[{'uri':name+'_basecolor.tga'},{'uri':name+'_normal.png'},{'uri':name+'_orm.png'}]
    g['samplers']=[{'magFilter':9729,'minFilter':9987,'wrapS':33071,'wrapT':33071}]
    g['textures']=[{'source':i,'sampler':0} for i in range(3)]
    for mat in g.get('materials',[]):
        mat['name']='scifi_'+name
        mat['pbrMetallicRoughness']={'baseColorFactor':[1,1,1,1],'baseColorTexture':{'index':0},'metallicFactor':0,'roughnessFactor':1,'metallicRoughnessTexture':{'index':2}}
        mat['normalTexture']={'index':1};mat['occlusionTexture']={'index':2};mat['alphaMode']='OPAQUE';mat['doubleSided']=False
        mat.pop('extensions',None)
    for key in ['extensionsUsed','extensionsRequired','extensions']:g.pop(key,None)
    glb_write(path,g,tail)


def validate(job):
    out=Path(job['out']);style=read_json(job['style_path']);stats=read_json(out/'stats.json')
    if stats['source_triangles']>style['budgets']['high_max']:raise ValueError('Source triangle budget exceeded')
    records=[]
    for lod in (0,1):
        path=out/f'{out.name}_lod{lod}.glb';g,_=glb_read(path)
        lo=[float('inf')]*3;hi=[-float('inf')]*3;tris=0
        for node in g.get('nodes',[]):
            if any(k in node for k in ['matrix','translation','rotation','scale']):raise ValueError(f'{path}: node transforms must be baked')
        for mesh in g['meshes']:
            for p in mesh['primitives']:
                if not {'POSITION','NORMAL','TEXCOORD_0'}<=p['attributes'].keys():raise ValueError('Missing mesh attributes')
                a=g['accessors'][p['attributes']['POSITION']]
                lo=[min(lo[i],a['min'][i]) for i in range(3)];hi=[max(hi[i],a['max'][i]) for i in range(3)]
                tris+=g['accessors'][p['indices']]['count']//3
        if tris>style['budgets'][f'lod{lod}_max']:raise ValueError(f'{path}: triangle budget exceeded')
        # Runtime TG=(-glTF.x,-glTF.y,+glTF.z), stock pivot retained.
        tglo=[-hi[0],-hi[1],lo[2]];tghi=[-lo[0],-lo[1],hi[2]];bounds=job['stock']['bounds']
        if any(tglo[i]<bounds['min'][i]-.02 or tghi[i]>bounds['max'][i]+.02 for i in range(3)):raise ValueError(f'{path}: outside stock bounds {tglo}..{tghi}')
        # Inspect actual binary vertices against concave footprint, not just AABB.
        _,tail=glb_read(path);binary=tail[8:];unit=style['scale']['world_units_per_metre'];cx=(bounds['min'][0]+bounds['max'][0])/2;cz=(bounds['min'][2]+bounds['max'][2])/2
        for mesh in g['meshes']:
            for p in mesh['primitives']:
                a=g['accessors'][p['attributes']['POSITION']];view=g['bufferViews'][a['bufferView']];start=view.get('byteOffset',0)+a.get('byteOffset',0);stride=view.get('byteStride',12)
                for i in range(a['count']):
                    x,_,z=struct.unpack_from('<3f',binary,start+i*stride)
                    if not inside([(-x-cx)/unit,(z-cz)/unit],job['footprint_m']):raise ValueError('Mesh vertex outside requested footprint')
        records.append({'lod':lod,'triangles':tris,'runtime_tg_bounds':{'min':tglo,'max':tghi},'identity_nodes':True,'uv':True,'all_vertices_inside_lot':True})
    # TGA type 2 is uncompressed true colour. Engine texture name comes from URI.
    tga=out/(out.name+'_basecolor.tga')
    if tga.read_bytes()[2]!=2:raise ValueError('Runtime albedo must be an uncompressed TGA')
    write_json(out/'validation.json',{'ok':True,'geometry':records,'basecolor_only_runtime':True,'stock_damage_retained':True})
    for record in records:stats[f'lod{record["lod"]}_triangles']=record['triangles']
    write_json(out/'stats.json',stats)
    print('[scifi] validated',out.name,[r['triangles'] for r in records],flush=True)


def montage(job):
    from PIL import Image,ImageDraw,ImageFont,ImageOps
    out=Path(job['out']);stats=read_json(out/'stats.json')
    im=Image.new('RGB',(1800,1390),(27,33,38));draw=ImageDraw.Draw(im)
    fontpath='C:/Windows/Fonts/segoeui.ttf'
    def text(x,y,value,size=22):draw.text((x,y),value,font=ImageFont.truetype(fontpath,size),fill=(224,228,224))
    def panel(path,box):
        src=Image.open(path).convert('RGB');thumb=ImageOps.contain(src,(box[2],box[3]));im.paste(thumb,(box[0]+(box[2]-thumb.width)//2,box[1]+(box[3]-thumb.height)//2))
    text(25,16,f'{job["kind"].upper()}  /  seed {job["seed"]}  /  {stats["palette"]}',32)
    text(25,62,'HIGH SOURCE / Cycles soft sun + sky',22);panel(out/'beauty.png',(10,95,1000,720))
    text(1030,62,'GAME LOD0 / MC2-style RTS zoom',22);panel(out/'rts.png',(1015,95,775,477))
    text(1030,575,'STOCK '+job['stock']['id']+' / catalog',19)
    stock=SHEETS/(job['stock']['id']+'.png')
    if not stock.is_file():raise FileNotFoundError(stock)
    panel(stock,(1030,607,330,240))
    text(1370,575,'BT2018 / REFERENCE ONLY',18);panel(out.parent/'reference/bt_industrial.png',(1370,607,410,240))
    text(25,835,f'Original geometry | source {stats["source_triangles"]:,} tris | game {stats["lod0_triangles"]:,} / far {stats["lod1_triangles"]:,} | 1024 normal / AO / ORM',23)
    text(25,873,'Owner quality references below: compare depth, edge hierarchy, materials and human-scale detail.',22)
    panel(REF/'reddit_chicago_rowhouses.jpg',(15,915,880,410));panel(REF/'reddit_chicago_houses.jpg',(905,915,880,410))
    text(25,1340,'DRAFT / NOT ART-APPROVED  |  Stock collision + damage remain authoritative  |  No fallback sheen',21)
    im.save(out/'montage.png')


def contact(out):
    from PIL import Image,ImageDraw,ImageOps
    paths=sorted(out.glob('*/montage.png'))
    if not paths:return
    im=Image.new('RGB',(1800,70+700*((len(paths)+1)//2)),(27,33,38));d=ImageDraw.Draw(im);d.text((20,20),'SCI-FI KIT / first drafts / open individual montages for full detail',fill='white')
    for i,p in enumerate(paths):im.paste(ImageOps.contain(Image.open(p),(900,695)),((i%2)*900,70+(i//2)*700))
    im.save(out/'contact-sheet.png')


def package(job,install=None):
    out=Path(job['out']);validate(job)
    bundle=out/'bundle';mo=bundle/'data/model_overrides';assets=mo/'scifi-kit'/out.name;assets.mkdir(parents=True,exist_ok=True)
    for pattern in ['*_lod?.glb','*_basecolor.tga','*_normal.png','*_orm.png']:
        for src in out.glob(pattern):shutil.copy2(src,assets/src.name)
    records=[]
    for tier in [32,64,128,256,512,1024]:
        from PIL import Image
        dest=bundle/'data/tgl'/str(tier);dest.mkdir(parents=True,exist_ok=True)
        Image.open(out/(out.name+'_basecolor.tga')).resize((tier,tier),Image.Resampling.LANCZOS).save(dest/(out.name+'_basecolor.tga'),compression=None)
    base=f'scifi-kit/{out.name}/{out.name}'
    record={'type':'model','replaces':'staticProp:'+job['stock']['lod0_key'],'source':base+'_lod0.glb','scale':1,'renderOnly':True,'fallback':'stock','lods':[{'lod':1,'source':base+'_lod1.glb','distance':1000}]}
    records.append(record);write_json(mo/'models.json',{'overrides':records})
    write_json(assets/'material.json',{'schema':'scifi-kit-authored-material-1','baseColor':out.name+'_basecolor.tga','normal':out.name+'_normal.png','orm':out.name+'_orm.png','normalSpace':'tangent OpenGL','ormChannels':['occlusion','roughness','metallic'],'fallbackSheen':False,'note':'Authored channel descriptor; not claiming the in-flight engine sidecar schema. Legacy runtime consumes base colour only.'})
    write_json(bundle/'mod.json',{'id':'scifi-kit-'+out.name,'name':'Sci-fi building draft '+out.name,'version':'0.1.0','description':'Original procedural mesh, local CC0 detail maps. Intact visual override only.'})
    (bundle/'LICENSE-NOTES.txt').write_text('Original procedural geometry. Local ambientCG / Poly Haven detail textures: CC0. BattleTech / MW5 assets and Reddit imagery are reference only and are NOT included in this bundle.\n',encoding='utf-8')
    write_json(assets/(out.name+'_lod0.asset.json'),{'category':'building','up_axis':'-y','forward_axis':'-z','wu_per_unit':1,'license':'original project geometry + CC0 texture detail','source_url':'local:tools/scifi_kit','orientation_reviewed':True,'note':'Engine-ready inverse-axis0 mesh. Use the catalog selection key; do not cook the axis transform a second time.'})
    if install:
        install=Path(install).resolve()
        if install!=INSTALL.resolve():raise ValueError('This harness installs only into G:/mc2-installs/mc2-win64-mapgen')
        if not (install/'mc2.exe').is_file():raise FileNotFoundError(install/'mc2.exe')
        target=install/'data/model_overrides/models.json'
        previous=read_json(target) if target.is_file() else {'overrides':[]}
        backup=out.parent/'install-backup/models.json'
        if not backup.is_file():write_json(backup,previous)
        keys={e['replaces'].lower() for e in records};previous['overrides']=[e for e in previous['overrides'] if e.get('replaces','').lower() not in keys]+records
        # Copy owned assets only; preserve unrelated registry records and textures.
        for src in (bundle/'data').rglob('*'):
            if src.is_file() and src!=mo/'models.json':
                dest=install/'data'/src.relative_to(bundle/'data');dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
        write_json(target,previous);print('[scifi] installed',record['replaces'],'into',install)
    return bundle


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command',choices=['prepare','library','review','montage','export','install','validate','capture'])
    ap.add_argument('--type',dest='kind');ap.add_argument('--seed',type=int);ap.add_argument('--out',type=Path,default=DEFAULT_OUT);ap.add_argument('--style',type=Path);ap.add_argument('--rules',type=Path);ap.add_argument('--footprint',type=Path,help='JSON array of XY vertices in centred metres')
    args=ap.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    rules=read_json(args.rules or HERE/'rules/buildings.json')
    if args.command=='capture':
        # Exactly the existing background harness; no direct game process launch.
        capture=out/'in-game'
        cmd=[sys.executable,str(ROOT/'scripts/run_visual_capture.py'),'--exe',str(INSTALL/'mc2.exe'),'--mission','mc2_01','--bookmarks',str(HERE/'capture-mc2_01.json'),'--out-dir',str(capture),'--duration','30','--no-kill','--env-declare','MC2_DEBUG_STATE_DUMP=1','--env-declare','MC2_MODOVERRIDE_TRACE=1']
        result=subprocess.run(cmd,**console_child.child_kwargs())
        if result.returncode:raise SystemExit(result.returncode)
        return
    if args.command=='prepare':prepare(out);return
    if args.command=='library':
        prepare(out);run_blender(out/'logs/library.log',['--library',str(out/'parts')]);return
    if args.command=='review':prepare(out)
    for kind in ([args.kind] if args.kind else rules['types']):
        for seed in ([args.seed] if args.seed is not None else rules['seeds']):
            job=make_job(kind,seed,out,args.style,args.rules,read_json(args.footprint) if args.footprint else None);path=Path(job['out']);path.mkdir(parents=True,exist_ok=True)
            if args.command=='review':
                write_json(path/'job.json',job);run_blender(out/'logs'/f'{path.name}.log',['--job',str(path/'job.json')])
                for lod in (0,1):patch_textures(path/f'{path.name}_lod{lod}.glb',path.name)
                validate(job);montage(job)
            elif args.command=='montage':montage(read_json(path/'job.json'))
            else:
                job=read_json(path/'job.json')
                if args.command=='validate':validate(job)
                else:package(job,INSTALL if args.command=='install' else None)
    contact(out)


if __name__=='__main__':main()
