import json
from pathlib import Path
import struct

import numpy as np
import pytest

from tools.anim_parity.audit import (Stock, agl, tgl, pose, check_nodes, gate_check,
                                     intersects_box, extremes, glb, gate_blocking, cooked_model)


@pytest.fixture
def gate():
    d = json.loads(Path(__file__).with_name('fixture.json').read_text())
    leaf = d['leaf']
    lo,hi = np.array(leaf['min']),np.array(leaf['max'])
    v = np.array([[lo[0],lo[1],lo[2]], [hi[0],lo[1],lo[2]],
                  [hi[0],hi[1],lo[2]], [lo[0],hi[1],lo[2]],
                  [lo[0],lo[1],hi[2]], [hi[0],lo[1],hi[2]],
                  [hi[0],hi[1],hi[2]], [lo[0],hi[1],hi[2]]])
    faces = np.array([[0,1,2],[0,2,3],[4,6,5],[4,7,6],[0,4,5],[0,5,1],
                      [3,2,6],[3,6,7],[0,3,7],[0,7,4],[1,5,6],[1,6,2]])
    model = {'Frame':dict(parent='None',pivot=np.zeros(3),triangles=np.empty((0,3,3))),
             leaf['name']:dict(parent=leaf['parent'],pivot=np.array(leaf['pivot']),triangles=v[faces])}
    a = d['opening']
    raw = struct.pack('<II',0xBADDECAF,1)+b'GateDoor\0'.ljust(25,b'\0')
    raw += struct.pack('<iIffii',-1,a['frames'],a['fps'],160,0,36)
    raw += np.array(a['positions'],dtype='<f4').tobytes()
    return model,agl(raw),raw,v,faces


def test_binary_track_and_sink_pose(gate):
    model,tracks,_,_,_ = gate
    assert tracks['GateDoor']['fps']==2
    assert pose(model,tracks,1)['GateDoor'][:,:,1].max()==-1
    assert pose(model,tracks,.5)['GateDoor'][:,:,1].max()==1.5
    assert not gate_check(model,tracks,{'GateDoor'})[0]


def test_truncated_or_bad_track_rejected(gate):
    raw = gate[2]
    with pytest.raises((ValueError,struct.error)):
        agl(raw[:-1])
    with pytest.raises(ValueError):
        agl(raw[:8]+raw[8:33]+struct.pack('<iIffii',-1,3,2,160,1,36)+raw[57:])


def test_tgl_disk_geometry_roundtrip(gate):
    _,_,_,v,faces = gate
    pivot = np.array([3.,4.,5.])
    raw = struct.pack('<III',0xBAFDECAF,1,0)+struct.pack('<II',1,len(v))
    for p in v:
        raw += struct.pack('<3f',*p)+bytes(16)
    raw += struct.pack('<I',len(faces))
    for f in faces:
        raw += struct.pack('<3I',*f)+bytes(44)
    raw += struct.pack('<I6f',0,*pivot,*pivot)+b'GateDoor'.ljust(25,b'\0')+b'None'.ljust(25,b'\0')
    parsed = tgl(raw)
    np.testing.assert_allclose(parsed['GateDoor']['triangles'],v[faces]+pivot)
    with pytest.raises(ValueError):
        tgl(raw+b'x')


def test_missing_empty_wrong_parent_and_pivot(gate):
    model = gate[0]
    assert check_nodes(model,model,{'GateDoor'}) == ([],[])
    assert 'missing name-bound node' in check_nodes(model,{'Frame':model['Frame']},{'GateDoor'})[0][0]
    broken = {**model,'GateDoor':dict(parent='bad',pivot=np.ones(3),triangles=np.empty((0,3,3)))}
    errors,warnings = check_nodes(model,broken,{'GateDoor'})
    assert any('parent' in e for e in warnings)
    assert any('pivot' in e for e in warnings)
    assert any('empty' in e for e in errors)


def test_static_parent_and_rotation_pivot_are_warnings(gate):
    model = gate[0]
    remake = {**model,'GateDoor':{**model['GateDoor'],'parent':'asset_pristine_LOD0','pivot':np.array([0,2,0])}}
    tracks = {'GateDoor':dict(frames=2,fps=2,p=np.empty((0,3)),q=np.array([[0,0,0,1],[0,1,0,0]]))}
    errors,warnings = check_nodes(model,remake,{'GateDoor'},tracks)
    assert not errors
    assert any('static parent name' in w for w in warnings)
    assert any('preserved by cooker' in w for w in warnings)


def test_name_binding_ignores_case_and_node_order(gate):
    model,tracks,*_ = gate
    remake = {'gatedoor':model['GateDoor'],'Frame':model['Frame']}
    errors,warnings = check_nodes(model,remake,{'GateDoor'})
    assert not errors
    assert any('case-insensitive' in w for w in warnings)
    np.testing.assert_allclose(pose(remake,tracks,1)['gatedoor'],pose(model,tracks,1)['GateDoor'])


def test_translation_pivot_replacement_is_real(gate):
    model,tracks,*_ = gate
    remake = {**model,'GateDoor':{**model['GateDoor'],'pivot':np.array([0,10,0])}}
    errors,_ = check_nodes(model,remake,{'GateDoor'},tracks)
    assert any('REAL/ART stock absolute translation' in e for e in errors)


def test_cooker_preserves_animated_parent_not_static_parent(gate):
    model,_,*_=gate
    model = {**model,'Child':dict(parent='GateDoor',pivot=np.array([0,2,0]),
                                triangles=model['GateDoor']['triangles']*.2)}
    tracks = {'GateDoor':dict(frames=2,fps=2,p=np.empty((0,3)),q=np.array([[0,0,0,1],[0,1,0,0]]))}
    model['GateDoor']['extras'] = {'mc2_anim_node':'GateDoor'}
    runtime = cooked_model(model)
    assert runtime['Child']['parent']=='GateDoor'
    inherited = pose(model,tracks,1)['Child']
    np.testing.assert_allclose(inherited, pose(runtime,tracks,1)['Child'], atol=1e-6)
    errors,_ = check_nodes(model,model,{'GateDoor','Child'},tracks)
    assert not errors
    broken = {n:{**v,'extras':{}} for n,v in model.items()}
    errors,_ = check_nodes(model,broken,{'GateDoor','Child'},tracks)
    assert any('REAL/PIPELINE' in e for e in errors)
    errors,_ = check_nodes(model,model,{'GateDoor','Child'})
    assert not errors


def test_kerb_collision_and_insufficient_travel(gate):
    model,tracks,*_ = gate
    kerb = np.array([[[-2,.1,-3],[2,.1,-3],[2,.1,3]], [[-2,.1,-3],[2,.1,3],[-2,.1,3]]])
    blocked = {**model,'Frame':dict(parent='None',pivot=np.zeros(3),triangles=kerb)}
    assert any('slot clearance' in e for e in gate_check(blocked,tracks,{'GateDoor'})[0])
    shallow = {'GateDoor':{**tracks['GateDoor'],'p':tracks['GateDoor']['p']*.5}}
    assert any('above ground' in e for e in gate_check(model,shallow,{'GateDoor'})[0])


def test_fused_leaf_retains_open_blocking(gate):
    model,tracks,*_=gate
    fused = {'Frame':dict(parent='None',pivot=np.zeros(3),triangles=model['GateDoor']['triangles'])}
    b = gate_blocking(model,fused,tracks,{'GateDoor'})
    assert b['stock_blocking_missing_fraction']==0
    assert b['remake_open_residual_fraction']==1
    b = gate_blocking(model,model,tracks,{'GateDoor'})
    assert b['remake_open_residual_fraction']==0


def test_box_sat_rejects_bbox_false_positive():
    # Triangle bounds touch the box, but its diagonal edge misses it.
    tris = np.array([[[0,0,0],[3,0,0],[0,3,0]]],float)
    assert not intersects_box(tris,np.array([2,2,-1]),np.array([3,3,1])).any()
    assert intersects_box(tris,np.array([.2,.2,-1]),np.array([.8,.8,1])).all()


def test_nested_rotation_and_loop_extremum(gate):
    model,_,*_=gate
    model = {**model,'Detail':dict(parent='GateDoor',pivot=np.array([0,2,0]),
                                 triangles=np.array([[[0,2,0],[1,2,0],[0,3,0]]],float))}
    tracks = {'GateDoor':dict(frames=3,fps=2,p=np.empty((0,3)),
                              q=np.array([[0,0,0,1],[0,0,1,0],[0,0,0,1]],float))}
    posed = pose(model,tracks,.5)
    np.testing.assert_allclose(posed['Detail'][0,0],[0,-2,0],atol=1e-8)
    assert 1 in [r['frame'] for r in extremes(model,model,tracks)]


def test_effective_loose_overrides_packed(tmp_path):
    name = b'data/tgl/test.ini'
    packed = b'packed'
    header = struct.pack('<II',0xCADDECAF,1)
    entry = struct.pack('<4I',274,len(packed),len(packed),0)+name.ljust(250,b'\0')
    (tmp_path/'tgl.fst').write_bytes(header+entry+packed)
    source = Stock(tmp_path)
    assert source.read(name.decode())==packed
    folder = tmp_path/'data/tgl'
    folder.mkdir(parents=True)
    (folder/'test.ini').write_bytes(b'loose')
    source = Stock(tmp_path)
    assert source.read(name.decode())==b'loose'
    assert '::' not in source.sources[name.decode()]['source']


def test_glb_world_transform_and_units(tmp_path):
    positions = np.array([[0,0,0],[1,0,0],[0,1,0]],dtype='<f4').tobytes()
    doc = dict(asset={'version':'2.0'},buffers=[{'byteLength':len(positions)}],
               bufferViews=[{'buffer':0,'byteLength':len(positions)}],
               accessors=[{'bufferView':0,'componentType':5126,'count':3,'type':'VEC3'}],
               meshes=[{'primitives':[{'attributes':{'POSITION':0}}]}],
               nodes=[{'name':'Frame','translation':[1,2,3],'children':[1]},
                      {'name':'GateDoor','translation':[0,1,0],'mesh':0}])
    j = json.dumps(doc).encode()
    j += b' '*((-len(j))%4)
    raw = struct.pack('<III',0x46546C67,2,28+len(j)+len(positions))
    raw += struct.pack('<II',len(j),0x4E4F534A)+j+struct.pack('<II',len(positions),0x004E4942)+positions
    path = tmp_path/'fixture_pristine_lod0.glb'
    path.write_bytes(raw)
    path.with_suffix('.asset.json').write_text(json.dumps({'frame':'gltf','wu_per_unit':4}))
    model = glb(path)
    np.testing.assert_allclose(model['GateDoor']['pivot'],[4,12,12])
    np.testing.assert_allclose(model['GateDoor']['triangles'][0],[[4,12,12],[8,12,12],[4,16,12]])
