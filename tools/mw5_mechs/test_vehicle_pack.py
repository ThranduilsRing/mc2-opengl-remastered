"""The VTOL texture rewrite must preserve geometry and animation buffers."""
import json
import struct

from tools.content_pipeline.gltf_io import load_document
from tools.mw5_mechs.build_vehicle_pack import write_glb


def test_vtol_rewrite_retains_animation_and_unaligned_binary(tmp_path):
    doc = {'asset': {'version': '2.0'}, 'buffers': [{'byteLength': 5}],
           'nodes': [{'name': 'mw5_rotor_a'}], 'scenes': [{'nodes': [0]}], 'scene': 0,
           'animations': [{'name': 'atlas_moveCoreIdle', 'channels': [], 'samplers': []}]}
    path = tmp_path/'warrior.glb'
    write_glb(path, doc, b'12345')
    restored, buffers, _ = load_document(path)
    assert restored == doc
    assert buffers[0][:5] == b'12345'
    assert len(path.read_bytes()) == struct.unpack_from('<I', path.read_bytes(), 8)[0]


def test_recipe_hosts_and_articulation_are_unambiguous():
    from pathlib import Path
    recipe = json.loads((Path(__file__).parent/'recipes/vehicles.json').read_text())
    hosts = [v['replaces'] for v in recipe['vehicles'] if not v.get('variant_of')]
    assert len(hosts) == len(set(hosts))
    for v in recipe['vehicles']:
        parents = set()
        for c in v['components']:
            assert len(c['node']) < 24
            assert c['node'] not in parents
            if c.get('parent'):
                assert c['parent'] in parents
            if c.get('at'):
                assert c['at'].split(':')[0] in parents
            parents.add(c['node'])
