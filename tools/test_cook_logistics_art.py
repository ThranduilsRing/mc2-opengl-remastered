"""Offline loader/cook checks: coordinate frame, atlas isolation, packed input."""
import io
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import cook_logistics_art as cook


def test_shipped_art_decodes_with_loader_dimensions_and_transparent_border():
    art = Path(__file__).resolve().parents[1]/'data/art'
    maps = sorted((art/'missionmaps').glob('*.png'))
    assert len(maps) == 43
    assert {f'mc2_{mission:02d}' for mission in range(1, 25)} <= {p.stem for p in maps}
    for path in maps:
        assert path.stat().st_size <= 1024 * 1024
        with Image.open(path) as image:
            image.load()
            assert image.mode in ('P', 'RGB') and image.size == (1024, 1024)
    icons = list((art/'mechicons').rglob('*.png'))
    assert art/'mechicons/starslayer.png' in icons
    assert art/'mechicons/hollander.png' in icons
    for path in icons:
        with Image.open(path) as image:
            assert image.mode == 'RGBA'
            pixels = np.array(image)
            if '@' in path.stem:
                # Per-size HUD cell (hud/<side>/<stem>@WxH, tools/extract_hud_tier_icons.py): the v0.4 sheet cell drawn 1:1,
                # border already cropped, so it carries the cell size instead of a transparent ring.
                w, h = (int(v) for v in path.stem.rsplit('@', 1)[1].split('x'))
                assert image.size == (w, h), path
                continue
            assert not pixels[0].any() and not pixels[-1].any(), path
            assert not pixels[:, 0].any() and not pixels[:, -1].any(), path


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


def test_csv_crcrlf_and_named_icon_output(tmp_path):
    class Source:
        def read(self, key):
            if key.endswith('.csv') and not key.endswith('asset_sizes.csv'):
                return b'BaseChassisInfo,\r\r\nMechParts,0\r\r\n'
            if key.endswith('.fit'):
                return b'[Static1] // icon\nl Width=25\nl Height=30\nst FileName="mcui_gn_mechicons.tga"\n'
            raise FileNotFoundError(key)
        def keys(self, prefix, suffix):
            return ['data/objects/starslayer.csv']
        def image(self, key):
            if not key.endswith('mcui_gn_mechicons.tga'):
                raise FileNotFoundError(key)
            return Image.new('RGBA', (50, 60), (70, 80, 90, 255))
    icons = cook.split_icons(Source(), tmp_path)
    assert [name for name, _ in icons] == ['starslayer']
    image = Image.open(tmp_path/'data/art/mechicons/starslayer.png')
    assert image.size == (27, 32)
    assert image.getpixel((0, 0)) == (0, 0, 0, 0)


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
    assert image.getpixel((0, 0)) == (36, 99, 132)  # northwest water stays northwest
    assert image.getpixel((8, 480))[0] > 180       # southwest colormap stays southwest
    assert image.getpixel((8, 255)) == (94, 104, 114)  # bilinear colormap boundary
    assert image.getpixel((384, 384)) == (255, 0, 0)  # +X,-Y building -> southeast


def test_diagonal_coast_has_one_pixel_coverage_not_cell_steps(tmp_path):
    assets = cook.Assets(tmp_path)
    y, x = np.mgrid[:60, :60]
    mask = np.array(cook.water_mask(assets, 'diagonal', x + y - 50.03, 0, 1024))
    assert mask.shape == (1024, 1024)
    # The contour moves a pixel for every row, rather than jumping by 17px cells.
    edge = (mask[:700] >= 128).sum(axis=1)
    assert np.all(np.diff(edge) == -1)
    transition = ((mask[:700] > 0) & (mask[:700] < 255)).sum(axis=1)
    assert np.all((transition >= 1) & (transition <= 2))


def test_visual_heights_override_sim_and_keep_corner_extent(tmp_path):
    folder = tmp_path/'data/missions/test.beauty'
    folder.mkdir(parents=True)
    y, x = np.mgrid[:237, :237]
    visual = (x / 4 + y / 4 - 50.03).astype('<f4')
    (folder/'visual_height_4x.r32').write_bytes(visual.tobytes())
    assets = cook.Assets(tmp_path)
    actual = np.array(cook.water_mask(assets, 'test', np.full((60, 60), 100.), 0, 1024))
    expected = np.array(cook.water_mask(assets, 'missing', np.mgrid[:60, :60].sum(axis=0) - 50.03, 0, 1024))
    assert np.abs(actual.astype(int) - expected).max() <= 1
    assert actual[0, 0] == 255 and actual[-1, -1] == 0
    (folder/'visual_height_4x.r32').write_bytes(b'bad')
    with pytest.raises(ValueError, match='dimensions'):
        cook.water_mask(assets, 'test', np.zeros((60, 60)), 0, 1024)


def test_road_tiles_are_filtered_together_without_cell_seams():
    n = 60
    vertices = np.zeros((n, n, 32), dtype=np.uint8)
    vertices[:, :, 12:16] = np.array([10.], dtype='<f4').view('u1')
    vertices[:, :, 16:20] = np.array([255 << 16], dtype='<u4').view('u1')
    pak = packet_file([(0, vertices.tobytes())])
    tile = np.full((64, 64, 4), 255, dtype=np.uint8)
    tile[:, :, 3] = np.arange(64, dtype=np.uint8)[None, :] * 4

    class Source:
        def read(self, key):
            if key.endswith('.pak'):
                return pak
            if key.endswith('.fit'):
                return b'[Water]\nf Elevation=-100\n'
            if key.endswith('buildings.csv'):
                return b''
            raise FileNotFoundError(key)
        def image(self, key):
            if key == 'road':
                return Image.fromarray(tile)
            return Image.new('RGBA', (60, 60), (10, 20, 30, 255))

    actual = cook.cook_map(Source(), 'test', 128, {255: 'road'})
    roads = Image.fromarray(np.tile(tile, (n, n, 1))).resize((128, 128), Image.Resampling.BILINEAR)
    expected = Image.new('RGB', (128, 128), (10, 20, 30))
    expected.paste(roads, (0, 0), roads)
    assert np.array_equal(actual, expected)


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


def test_magenta_placeholder():
    assert cook.transparent_border(Image.new('RGBA', (25, 30), (255, 0, 255, 255))).getbbox() is None
    assert cook.transparent_border(Image.new('RGBA', (25, 30), (226, 0, 127, 255))).getbbox() is None


if __name__ == '__main__':
    raise SystemExit(pytest.main([__file__, *sys.argv[1:]]))
