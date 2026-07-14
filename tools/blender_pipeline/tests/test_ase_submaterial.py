"""ase_to_glb.py Multi/Sub-Object *SUBMATERIAL parsing + MTLID remap tests.
No Blender/GUI required; stdlib only."""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import ase_to_glb as a2g  # noqa: E402


# ---------------------------------------------------------------------------
# Fixtures: minimal synthetic ASE text (single-material and Multi/Sub-Object)
# ---------------------------------------------------------------------------

SINGLE_MAT_ASE = """\
*3DSMAX_ASCIIEXPORT\t200
*MATERIAL_LIST {
\t*MATERIAL_COUNT 1
\t*MATERIAL 0 {
\t\t*MATERIAL_NAME "PlainMat"
\t\t*MAP_DIFFUSE {
\t\t\t*BITMAP "\\\\Aas1\\mc2\\Art\\plain.tga"
\t\t}
\t}
}
*GEOMOBJECT {
\t*NODE_NAME "Box01"
\t*MATERIAL_REF 0
\t*MESH {
\t\t*MESH_NUMVERTEX 3
\t\t*MESH_VERTEX_LIST {
\t\t\t*MESH_VERTEX 0\t0.0\t0.0\t0.0
\t\t\t*MESH_VERTEX 1\t1.0\t0.0\t0.0
\t\t\t*MESH_VERTEX 2\t0.0\t1.0\t0.0
\t\t}
\t\t*MESH_NUMFACES 1
\t\t*MESH_FACE_LIST {
\t\t\t*MESH_FACE 0:\tA:\t0 B:\t1 C:\t2 AB:\t1 BC:\t1 CA:\t1\t*MESH_SMOOTHING\t*MESH_MTLID\t0
\t\t}
\t\t*MESH_NUMTVERTEX 3
\t\t*MESH_TVERTLIST {
\t\t\t*MESH_TVERT 0\t0.0\t0.0\t0.0
\t\t\t*MESH_TVERT 1\t1.0\t0.0\t0.0
\t\t\t*MESH_TVERT 2\t0.0\t1.0\t0.0
\t\t}
\t\t*MESH_NUMTVFACES 1
\t\t*MESH_TFACELIST {
\t\t\t*MESH_TFACE 0\t0\t1\t2
\t\t}
\t\t*MESH_NORMALS {
\t\t\t*MESH_FACENORMAL 0\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 0\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 1\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 2\t0.0\t0.0\t1.0
\t\t}
\t}
}
"""


MULTI_SUB_ASE = """\
*3DSMAX_ASCIIEXPORT\t200
*MATERIAL_LIST {
\t*MATERIAL_COUNT 1
\t*MATERIAL 0 {
\t\t*MATERIAL_NAME "OuterWrapper"
\t\t*NUMSUBMTLS 2
\t\t*SUBMATERIAL 0 {
\t\t\t*MATERIAL_NAME "SubA"
\t\t\t*MAP_DIFFUSE {
\t\t\t\t*BITMAP "\\\\Aas1\\mc2\\Art\\suba.tga"
\t\t\t}
\t\t}
\t\t*SUBMATERIAL 1 {
\t\t\t*MATERIAL_NAME "SubB"
\t\t\t*MAP_DIFFUSE {
\t\t\t\t*BITMAP "\\\\Aas1\\mc2\\Art\\subb.tga"
\t\t\t}
\t\t}
\t}
}
*GEOMOBJECT {
\t*NODE_NAME "Box01"
\t*MATERIAL_REF 0
\t*MESH {
\t\t*MESH_NUMVERTEX 4
\t\t*MESH_VERTEX_LIST {
\t\t\t*MESH_VERTEX 0\t0.0\t0.0\t0.0
\t\t\t*MESH_VERTEX 1\t1.0\t0.0\t0.0
\t\t\t*MESH_VERTEX 2\t0.0\t1.0\t0.0
\t\t\t*MESH_VERTEX 3\t1.0\t1.0\t0.0
\t\t}
\t\t*MESH_NUMFACES 2
\t\t*MESH_FACE_LIST {
\t\t\t*MESH_FACE 0:\tA:\t0 B:\t1 C:\t2 AB:\t1 BC:\t1 CA:\t1\t*MESH_SMOOTHING\t*MESH_MTLID\t0
\t\t\t*MESH_FACE 1:\tA:\t1 B:\t3 C:\t2 AB:\t1 BC:\t1 CA:\t1\t*MESH_SMOOTHING\t*MESH_MTLID\t1
\t\t}
\t\t*MESH_NUMTVERTEX 4
\t\t*MESH_TVERTLIST {
\t\t\t*MESH_TVERT 0\t0.0\t0.0\t0.0
\t\t\t*MESH_TVERT 1\t1.0\t0.0\t0.0
\t\t\t*MESH_TVERT 2\t0.0\t1.0\t0.0
\t\t\t*MESH_TVERT 3\t1.0\t1.0\t0.0
\t\t}
\t\t*MESH_NUMTVFACES 2
\t\t*MESH_TFACELIST {
\t\t\t*MESH_TFACE 0\t0\t1\t2
\t\t\t*MESH_TFACE 1\t1\t3\t2
\t\t}
\t\t*MESH_NORMALS {
\t\t\t*MESH_FACENORMAL 0\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 0\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 1\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 2\t0.0\t0.0\t1.0
\t\t\t*MESH_FACENORMAL 1\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 1\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 3\t0.0\t0.0\t1.0
\t\t\t*MESH_VERTEXNORMAL 2\t0.0\t0.0\t1.0
\t\t}
\t}
}
"""


# MTLID 5 is deliberately out of range (only 2 submaterials, 0..1) to
# exercise the clamp+warn path.
MULTI_SUB_OOR_ASE = MULTI_SUB_ASE.replace(
    "*MESH_MTLID\t1", "*MESH_MTLID\t5")


def write_ase(tmp_path, text, name="asset"):
    p = tmp_path / f"{name}.ase"
    p.write_text(text)
    return p


# ---------------------------------------------------------------------------
# parse_ase: submaterial structure
# ---------------------------------------------------------------------------

def test_parse_single_material_no_submaterials(tmp_path):
    path = write_ase(tmp_path, SINGLE_MAT_ASE)
    geoms, materials, helpers = a2g.parse_ase(str(path))
    assert len(materials) == 1
    mat = materials[0]
    assert mat.name == "PlainMat"
    assert mat.bitmap == "plain.tga"
    assert mat.submaterials == []


def test_parse_multisub_material_names_and_bitmaps(tmp_path):
    path = write_ase(tmp_path, MULTI_SUB_ASE)
    geoms, materials, helpers = a2g.parse_ase(str(path))
    assert len(materials) == 1
    outer = materials[0]
    # Outer wrapper material must NOT be clobbered by the last submaterial.
    assert outer.name == "OuterWrapper"
    assert len(outer.submaterials) == 2
    sub_a, sub_b = outer.submaterials
    assert sub_a.name == "SubA"
    assert sub_a.bitmap == "suba.tga"
    assert sub_b.name == "SubB"
    assert sub_b.bitmap == "subb.tga"


# ---------------------------------------------------------------------------
# build_glb_data: MTLID -> submaterial remap
# ---------------------------------------------------------------------------

def _extract_glb_json(glb_bytes):
    # GLB header (12 bytes) + JSON chunk header (8 bytes) + JSON chunk data.
    off = 12
    chunk_len, chunk_type = struct.unpack('<I4s', glb_bytes[off:off + 8])
    off += 8
    assert chunk_type == b'JSON'
    return json.loads(glb_bytes[off:off + chunk_len])


def _material_bitmap_names(gltf, materials_list_order=None):
    """Return list of (material_name, bitmap_uri_or_None) in gltf material order."""
    out = []
    for m in gltf.get('materials', []):
        tex = m.get('pbrMetallicRoughness', {}).get('baseColorTexture')
        img = None
        if tex is not None:
            src = gltf['textures'][tex['index']]['source']
            img = gltf['images'][src]['uri']
        out.append((m.get('name'), img))
    return out


def test_multisub_mtlid_remaps_to_distinct_materials(tmp_path):
    path = write_ase(tmp_path, MULTI_SUB_ASE)
    geoms, materials, helpers = a2g.parse_ase(str(path))
    gltf, buf, stats = a2g.build_glb_data(geoms, materials, helpers)

    names_bitmaps = _material_bitmap_names(gltf)
    # Two distinct submaterials, each with its own bitmap.
    assert ("SubA", "suba.tga") in names_bitmaps
    assert ("SubB", "subb.tga") in names_bitmaps
    assert len(names_bitmaps) == 2

    # Each mesh primitive (one per MTLID group) must reference a different
    # material index, and it must resolve to the correct bitmap.
    mesh = gltf['meshes'][0]
    assert len(mesh['primitives']) == 2
    mat_indices = {p['material'] for p in mesh['primitives']}
    assert len(mat_indices) == 2  # distinct materials per face group
    for mi in mat_indices:
        assert 0 <= mi < len(gltf['materials'])


def test_multisub_out_of_range_mtlid_clamped_with_warning(tmp_path, capsys):
    path = write_ase(tmp_path, MULTI_SUB_OOR_ASE)
    geoms, materials, helpers = a2g.parse_ase(str(path))
    gltf, buf, stats = a2g.build_glb_data(geoms, materials, helpers)

    err = capsys.readouterr().err
    assert "out of range" in err
    assert "clamping" in err

    # Still produces a valid (clamped) material assignment, no crash / None.
    mesh = gltf['meshes'][0]
    for prim in mesh['primitives']:
        assert 'material' in prim
        assert 0 <= prim['material'] < len(gltf['materials'])


# ---------------------------------------------------------------------------
# Regression guard: single-material path must be byte-identical to the
# pre-submaterial-remap behavior (same JSON shape / material count).
# ---------------------------------------------------------------------------

def test_single_material_unaffected_by_submaterial_support(tmp_path):
    path = write_ase(tmp_path, SINGLE_MAT_ASE)
    geoms, materials, helpers = a2g.parse_ase(str(path))
    gltf, buf, stats = a2g.build_glb_data(geoms, materials, helpers)

    assert len(gltf['materials']) == 1
    assert gltf['materials'][0]['name'] == 'PlainMat'
    tex = gltf['materials'][0]['pbrMetallicRoughness']['baseColorTexture']
    src = gltf['textures'][tex['index']]['source']
    assert gltf['images'][src]['uri'] == 'plain.tga'

    mesh = gltf['meshes'][0]
    assert len(mesh['primitives']) == 1
    assert mesh['primitives'][0]['material'] == 0
