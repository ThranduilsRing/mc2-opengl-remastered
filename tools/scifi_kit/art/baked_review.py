"""Render exported GLBs, including all sides for counts and the farthest LOD."""
from pathlib import Path

import bpy


def run(job,stage):
    source=Path(job['source']);out=source/'review';out.mkdir(parents=True,exist_ok=True)
    for name in job['assemblies']:
        framing=None
        for lod in job.get('variants',(0,2,'destroyed')):
            stage.reset();stage.MAT_CACHE.clear()
            suffix='destroyed_lod0' if lod=='destroyed' else f'pristine_lod{lod}'
            bpy.ops.import_scene.gltf(filepath=str(source/'pbr'/f'{name}_{suffix}.glb'))
            meshes=[ob for ob in bpy.context.scene.objects if ob.type=='MESH']
            cam=stage.look_scene(bpy.context.scene)
            if framing is None:framing=stage.frame_camera(cam,meshes,direction=(-1,1.55,1.1),margin=1.10,aspect=1.4)
            if lod=='destroyed':stage.frame_camera(cam,meshes,direction=(-1,1.55,1.1),margin=1.32,aspect=1.4)
            else:cam.data.ortho_scale,cam.location,cam.rotation_euler=framing
            label='destroyed' if lod=='destroyed' else f'lod{lod}'
            stage.render(out/f'{name}_{label}.png',(700,500),24)
            if lod==0:
                for side,direction in [('front',(0,1,.04)),('left',(-1,0,.04)),
                                       ('rear',(0,-1,.04)),('right',(1,0,.04))]:
                    stage.frame_camera(cam,meshes,direction=direction,margin=1.14,aspect=1.4)
                    stage.render(out/f'{name}_count_{side}.png',(700,500),16)
