import struct

import numpy as np
import pytest

from tools.assets.catalog import OBJECT, object_records, priority
from tools.assets.common import bounds, model_refs, model_sections, parse_tgl
from tools.assets.mapgen_data import fit_sections, packet


def tgl_fixture():
    texture = b"a_test.tga\0".ljust(256, b"\0")+struct.pack("<IIB3x", 0, 0, 1)
    raw = struct.pack("<III", 0xBAFDECAF, 1, 1)+texture
    raw += struct.pack("<II", 1, 3)
    for p in ((0, 0, 0), (2, 0, 0), (0, 3, 1)):
        raw += struct.pack("<6fI", *p, 0, 1, 0, 0xFFFFFFFF)
    raw += struct.pack("<I5I9f", 1, 0, 1, 2, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0)
    raw += struct.pack("<I6f", 0, 10, 20, 30, 10, 20, 30)
    return raw+b"body\0".ljust(25, b"\0")+b"None\0".ljust(25, b"\0")


def test_tgl_corner_uv_and_world_node_pivot():
    mesh = parse_tgl(tgl_fixture())
    assert mesh.triangles == 1
    assert mesh.textures == ["a_test.tga"]
    np.testing.assert_allclose(mesh.vertices, [[10, 20, 30], [12, 20, 30], [10, 23, 31]])
    np.testing.assert_allclose(mesh.uv, [[0, 0], [1, 0], [0, 1]])
    assert bounds(mesh.vertices)["footprint_wu"] == [2, 1]


def test_tgl_rejects_truncation_bad_version_and_bad_indices():
    raw = tgl_fixture()
    for invalid in (raw[:-1], b"bad!"+raw[4:]):
        with pytest.raises(ValueError):
            parse_tgl(invalid)
    offset = 12+268+8+3*28+4
    invalid = raw[:offset]+struct.pack("<I", 100)+raw[offset+4:]
    with pytest.raises(ValueError, match="out-of-range"):
        parse_tgl(invalid)


def test_ini_distinguishes_destroyed_shadow_and_lod():
    ini = '''FITini
[TGLData]
st FileName0="Hangar"
st FileName1="HangarL1"
f Distance1=1000
st ShadowName="HangarX"
[TGLDamage]
st FileName="HangarDam"
[TGLDestructEffect]
st FileName="Hangar_Dest"
'''
    refs = model_refs(fit_sections(ini))
    assert [(r["role"], r["lod"]) for r in refs] == [("intact", 0), ("intact", 1), ("shadow", 0), ("destroyed", 0)]
    assert refs[1]["distance_wu"] == 1000
    assert refs[3]["tgl"] == "data/tgl/hangardam.tgl"


def test_packet_and_object_decoding_match_mapgen_layout():
    record = OBJECT.pack(559, 1, 2, 3, 90, 0, 1, -1, 0, 0)
    payload = struct.pack("<i", 1)+record
    pak = struct.pack("<III", 0xFEEDFACE, 12, 12)+payload
    assert object_records(packet(pak, 0))[0][0] == 559
    with pytest.raises(ValueError, match="count/size"):
        object_records(payload[:-1])


def test_stock_wirefence_continued_shadow_filename():
    ini = '[TGLData]\nst FileName="WireFence"\n[TGLDamage]\nst FileName="WireFenceDam"\nst ShadowName="wirefence\r\nDamX"\n'
    refs = model_refs(model_sections(ini))
    assert refs[-1]["tgl"] == "data/tgl/wirefencedamx.tgl"
    assert refs[0]["tgl"] == "data/tgl/wirefence.tgl"


def test_priority_uses_usage_size_and_both_quality_signals():
    b = bounds(np.array([[0, 0, 0], [10, 10, 10]]))
    low = priority(5, b, 20, [[64, 64]])
    assert low > priority(5, b, 200, [[512, 512]])
    assert priority(10, b, 20, [[64, 64]]) == pytest.approx(low*2, abs=.002)
    assert priority(0, b, 20, [[64, 64]]) == 0
