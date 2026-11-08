"""Offline loader/cook checks: coordinate frame, atlas isolation, packed input."""
import io
import struct
import zlib

import numpy as np
from PIL import Image
import pytest

from tools import cook_logistics_art as cook


def packet_file(payloads):
    offset = 8 + len(payloads)*4
    table, body = [], b''
    for kind, data in payloads:
        table.append(offset | kind << 29)
        body += data
        offset += len(data)
    return struct.pack('<II', 0xFEEDFACE, 8+len(table)*4) + struct.pack(f'<{len(table)}I', *table) + body


def test_packed_data_and_reject_corruption():
    data = b'current terrain data'*100
    compressed = struct.pack('<I', len(data)) + zlib.compress(data)
    assert cook.packets(packet_file([(4, compressed), (0, b'raw')])) == [(0, 0, data), (1, 0, b'raw')]
    with pytest.raises(ValueError):
        cook.packets(b'not a packet file')
    with pytest.raises(ValueError):
        cook.packets(packet_file([(4, struct.pack('<I', 1) + zlib.compress(data))]))


def test_fit_comments_and_shared_colormap_name():
    fit = '[Static1] // mech icon\r\nl Width = 40 // pixels\r\nst FileName = "mod.png"\r\n[Other]\r\nl Width = 99'
    assert cook.fit_value(fit, 'Static1', 'Width') == '40'
    assert cook.fit_value(fit, 'Static1', 'FileName') == 'mod.png'


def test_split_border_has_no_neighbour_and_keeps_hud_palette():
    atlas = Image.new('RGBA', (6, 3), (255, 0, 0, 255))
    atlas.paste((0, 0, 0, 255), (0, 0, 3, 3))
    atlas.putpixel((1, 1), (80, 80, 80, 255))
    bay = np.array(cook.transparent_border(atlas.crop((0, 0, 3, 3))))
    assert not bay[0].any() and not bay[-1].any()
    assert not bay[:, 0].any() and not bay[:, -1].any()
    assert not ((bay[:, :, 0] == 255) & (bay[:, :, 1] == 0)).any()
    assert tuple(bay[2, 2]) == (80, 80, 80, 255)
    hud = cook.transparent_border(atlas.crop((0, 0, 3, 3)), hud=True)
    assert hud.getpixel((1, 1)) == (0, 0, 0, 255)


def test_map_north_up_extent_water_and_building(monkeypatch):
    n = 60
    vertices = np.zeros((n, n, 32), dtype=np.uint8)
    vertices[:, :, 12:16] = np.array([10.], dtype='<f4').view('u1')
    vertices[0, 0, 12:16] = np.array([-10.], dtype='<f4').view('u1')
    objects = struct.pack('<Ii4f5i', 1, 42, n*128*.25, -n*128*.25, 0., 0., 0, 0, -1, 0, 0)
    pak = packet_file([(0, vertices.tobytes()), (0, objects)])
    class Source:
        def read(self, key):
            if key.endswith('.pak'):
                return pak
            if key.endswith('.fit'):
                return b'[Water]\nf Elevation = 0\n'
            if key.endswith('buildings.csv'):
                return b'test,0,0,BUILDING,42,,0,0,16711680,0\n'
            raise FileNotFoundError(key)
        def image(self, key):
            im = Image.new('RGBA', (60, 60), (10, 20, 30, 255))
            im.paste((200, 210, 220, 255), (0, 30, 30, 60))
            return im
    image = cook.cook_map(Source(), 'test', 512, {})
    assert image.size == (512, 512)
    assert image.getpixel((2, 2)) == (36, 99, 132)  # northwest water stays northwest
    assert image.getpixel((8, 480))[0] > 180       # southwest colormap stays southwest
    assert image.getpixel((384, 384)) == (255, 0, 0)  # +X,-Y building -> southeast


def test_loose_input_beats_base_and_source_is_read_only(tmp_path):
    base = tmp_path/'base'
    source = tmp_path/'mod'
    base.mkdir()
    source.mkdir()
    (base/'test.fit').write_bytes(b'base')
    (source/'test.fit').write_bytes(b'mod')
    assets = cook.Assets(source, cook.Assets(base))
    assert assets.read('TEST.FIT') == b'mod'
    assert (base/'test.fit').read_bytes() == b'base'
