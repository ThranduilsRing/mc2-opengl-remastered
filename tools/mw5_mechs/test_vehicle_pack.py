"""The VTOL texture rewrite must preserve geometry and animation buffers."""
import json
import struct

from tools.content_pipeline.gltf_io import load_document
from tools.mw5_mechs.build_vehicle_pack import air_variant, material_aliases, write_glb


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


def test_attack_igor_uses_a_real_stock_role_and_retains_base_pack(tmp_path):
    tgl = tmp_path/'data/tgl'
    tgl.mkdir(parents=True)
    (tgl/'assaultcopter.ini').write_text('st Source = "mw5_igor_attack"\nst AnimHost = "assaultcopter"\n')
    air_variant(tmp_path, {'id': 'igor_attack', 'replaces': 'vehicle:assaultcopter'},
                {'id': 'igor', 'replaces': 'vehicle:heavycopter'})
    variant = tmp_path/'variants/mw5-igor-attack'
    ini = (variant/'data/tgl/heavycopter.ini').read_text()
    assert 'Source = "mw5_igor_attack"' in ini
    assert 'AnimHost = "heavycopter"' in ini
    assert json.loads((variant/'mod.json').read_text())['dependencies'] == ['mw5-vehicles']


def test_material_aliases_follow_authored_inputs_instead_of_names():
    from tools.content_pipeline.ir import Material
    materials = [Material.for_class('dummy_material_0', 'painted_metal'),
                 Material.for_class('Warrior_Solid_MTI.001', 'painted_metal'),
                 Material.for_class('different_finish', 'painted_metal', roughness=.2)]
    assert material_aliases(materials) == {'dummy_material_0': 'dummy_material_0',
        'Warrior_Solid_MTI.001': 'dummy_material_0', 'different_finish': 'different_finish'}
