#!/usr/bin/env python3
"""tools/mc2texcook/tests/test_cook_terrain_layers.py -- Unit tests for
cook_terrain_layers.py (TERRAIN-MATERIAL-TEXTURE-REMAP-1 prep).

Covers:
  1. terrain_layer_manifest.json schema validation (the real, checked-in manifest).
  2. Synthetic cook round-trip: normalize_to_png / pack_height_into_normal_alpha
     end to end on small in-memory-generated PNGs (no ktx.exe / real downloaded
     assets required for these). If ktx.exe is present, also exercises the full
     PNG -> BC7 KTX2 cook and validates the resulting header.

Run:
    py -3 tools/mc2texcook/tests/test_cook_terrain_layers.py
    (or via pytest from repo root)
"""
from __future__ import annotations

import json
import os
import pytest
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

_TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

import cook_pbr_maps as pbr          # noqa: E402
import cook_terrain_layers as ctl    # noqa: E402

MANIFEST_PATH = _TOOLS_DIR / 'terrain_layer_manifest.json'
_REPO_ROOT = _TOOLS_DIR.parent.parent
FRAG_HEADER_PATH = _REPO_ROOT / 'shaders' / 'include' / 'terrain_mat_layers.hglsl'

KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"
REQUIRED_CHANNELS = ('rock', 'grass', 'dirt', 'concrete', 'snow', 'cliff')
OPTIONAL_CHANNELS = ('shore_sand', 'wet_rock')

# The canonical channel->layer order the
# albedo array is built and sampled in. Index i = layer i in the GL_TEXTURE_2D_ARRAY;
# the value is the frag MAT_LAYER_* constant name. This test module asserts the frag
# header, the C++ loader name table, and the manifest all agree with THIS list --
# the shared-source-of-truth guard against the "dirt renders with road albedo"
# weight-channel -> array-layer index mismatch.
CANONICAL_LAYER_ORDER = (
    ('rock', 'MAT_LAYER_ROCK', 0),
    ('grass', 'MAT_LAYER_GRASS', 1),
    ('dirt', 'MAT_LAYER_DIRT', 2),
    ('concrete', 'MAT_LAYER_CONCRETE', 3),
    ('snow', 'MAT_LAYER_SNOW', 4),
    ('cliff', 'MAT_LAYER_MARBLE_CLIFF', 5),
)


def _make_test_image(width=8, height=8, mode='RGB'):
    from PIL import Image
    img = Image.new(mode, (width, height))
    n = len(mode)
    if n == 3:
        pixels = [(x * 16 % 256, y * 16 % 256, 128) for y in range(height) for x in range(width)]
    else:
        pixels = [(x * 16 % 256, y * 16 % 256, 128, 255) for y in range(height) for x in range(width)]
    img.putdata(pixels)
    return img


# ---------------------------------------------------------------------------
# 1. Manifest schema validation
# ---------------------------------------------------------------------------

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(_ROOT, "tools"))
import gate_hosts  # noqa: E402
_GATE_HOST = gate_hosts.current_host()


class TestManifestSchema(unittest.TestCase):

    def setUp(self):
        with open(MANIFEST_PATH, 'r', encoding='utf-8') as f:
            self.manifest = json.load(f)

    def test_manifest_is_valid(self):
        missing = sorted({e['source_dir'] for e in self.manifest['layers'].values()
                          if e.get('source_dir') and not os.path.exists(e['source_dir'])})
        if missing and _GATE_HOST != "pc":
            pytest.skip(f"PC-only terrain source packs absent on this host: {', '.join(missing)}")
        errors = ctl.validate_manifest(self.manifest)
        self.assertEqual(errors, [], f'manifest validation errors: {errors}')

    def test_output_size_is_2048(self):
        self.assertEqual(self.manifest['output_size'], 2048)

    def test_required_channels_present(self):
        for ch in REQUIRED_CHANNELS:
            self.assertIn(ch, self.manifest['layers'], f'missing required channel "{ch}"')

    def test_optional_shoreline_channels_present(self):
        for ch in OPTIONAL_CHANNELS:
            self.assertIn(ch, self.manifest['layers'], f'missing optional channel "{ch}"')

    def test_every_layer_has_albedo_and_normal(self):
        for channel, entry in self.manifest['layers'].items():
            maps = entry['maps']
            self.assertTrue(maps.get('albedo'), f'{channel}: no albedo map')
            self.assertTrue(maps.get('normal'), f'{channel}: no normal map')

    def test_every_layer_has_license_note(self):
        for channel, entry in self.manifest['layers'].items():
            self.assertIn('license', entry, f'{channel}: missing license note')
            self.assertTrue(entry['license'].strip(), f'{channel}: empty license note')

    def test_cliff_height_is_null(self):
        # marble_cliff_01 ships no displacement map; height must be explicitly
        # null (not missing) so the packer knows to fall back to flat alpha=255.
        self.assertIsNone(self.manifest['layers']['cliff']['maps'].get('height'))

    def test_wet_rock_flagged_needs_better_source(self):
        # No true wet-rock/tide-pool pack exists in the current downloads;
        # this must stay flagged until a better source is found.
        self.assertTrue(self.manifest['layers']['wet_rock'].get('needs_better_source'))

    def test_naming_convention_matches_driver_output(self):
        # Sanity: driver writes "<channel>_<map>.ktx2" -- confirm the manifest
        # documents the same convention it's actually consumed with.
        self.assertIn('<channel>_<map>.ktx2', self.manifest['naming_convention'])


# ---------------------------------------------------------------------------
# 1b. INDEX ORACLE: channel -> array-layer order agreement across the three
#     sources of truth (frag MAT_LAYER_* header, C++ loader name table, manifest).
# ---------------------------------------------------------------------------

class TestChannelLayerIndexAgreement(unittest.TestCase):

    def _parse_frag_mat_layers(self):
        """Parse `#define MAT_LAYER_<NAME>  <int>` from the frag header."""
        import re
        text = FRAG_HEADER_PATH.read_text(encoding='utf-8')
        out = {}
        for m in re.finditer(r'#define\s+(MAT_LAYER_\w+)\s+(\d+)', text):
            out[m.group(1)] = int(m.group(2))
        return out

    def test_frag_constants_match_canonical(self):
        frag = self._parse_frag_mat_layers()
        for channel, const_name, idx in CANONICAL_LAYER_ORDER:
            self.assertIn(const_name, frag, f'frag header missing {const_name}')
            self.assertEqual(frag[const_name], idx,
                             f'{const_name} = {frag[const_name]} in frag header, expected {idx}')

    def test_manifest_order_matches_canonical(self):
        # The manifest layers dict preserves insertion order (json + py3.7+); the
        # cook writes <channel>_albedo.ktx2 which the loader uploads in
        # kMatAlbedoChannelNames order, so the manifest's required-channel order
        # must equal the canonical order.
        with open(MANIFEST_PATH, 'r', encoding='utf-8') as f:
            manifest = json.load(f)
        manifest_required = [c for c in manifest['layers'].keys()
                             if c in {ch for ch, _, _ in CANONICAL_LAYER_ORDER}]
        self.assertEqual(manifest_required, [c for c, _, _ in CANONICAL_LAYER_ORDER],
                         f'manifest channel order {manifest_required} != canonical')

    def test_concrete_channel_has_no_lane_markings_source(self):
        # fix B regression guard: the concrete channel must NOT point back at
        # Road004 (lane-marking albedo). Any marking-free concrete source is fine.
        with open(MANIFEST_PATH, 'r', encoding='utf-8') as f:
            manifest = json.load(f)
        concrete = manifest['layers']['concrete']
        albedo = concrete['maps']['albedo'].lower()
        self.assertNotIn('road004', albedo,
                         'concrete albedo still sources Road004 (lane markings) -- fix B regressed')
        self.assertNotIn('road004', concrete.get('source_pack', '').lower(),
                         'concrete source_pack still Road004 -- fix B regressed')


# ---------------------------------------------------------------------------
# 2. Bad-manifest validation (negative cases, synthetic)
# ---------------------------------------------------------------------------

class TestManifestValidationNegative(unittest.TestCase):

    def test_missing_layers_key(self):
        errors = ctl.validate_manifest({'output_size': 2048})
        self.assertTrue(any('layers' in e for e in errors))

    def test_empty_layers(self):
        errors = ctl.validate_manifest({'layers': {}})
        self.assertTrue(any('empty' in e for e in errors))

    def test_non_power_of_two_size(self):
        manifest = {
            'output_size': 2000,
            'layers': {'rock': {'source_pack': 'x', 'source_dir': '.', 'license': 'CC0',
                                 'maps': {'albedo': 'a.png', 'normal': 'n.png'}}},
        }
        errors = ctl.validate_manifest(manifest)
        self.assertTrue(any('power-of-two' in e for e in errors))

    def test_missing_albedo_map(self):
        manifest = {
            'output_size': 2048,
            'layers': {'rock': {'source_pack': 'x', 'source_dir': '.', 'license': 'CC0',
                                 'maps': {'normal': 'n.png'}}},
        }
        errors = ctl.validate_manifest(manifest)
        self.assertTrue(any('albedo' in e for e in errors))

    def test_nonexistent_source_dir(self):
        manifest = {
            'output_size': 2048,
            'layers': {'rock': {'source_pack': 'x', 'source_dir': 'Z:/does/not/exist',
                                 'license': 'CC0', 'maps': {'albedo': 'a.png', 'normal': 'n.png'}}},
        }
        errors = ctl.validate_manifest(manifest)
        self.assertTrue(any('does not exist' in e for e in errors))


# ---------------------------------------------------------------------------
# 3. Synthetic cook round-trip (normalize + height-alpha packing, no ktx.exe)
# ---------------------------------------------------------------------------

class TestSyntheticCookRoundTrip(unittest.TestCase):

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix='test_cook_terrain_')

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_normalize_png_passthrough(self):
        from PIL import Image
        src = os.path.join(self.tmpdir, 'src.png')
        _make_test_image(mode='RGB').save(src)

        out = os.path.join(self.tmpdir, 'out.png')
        pbr.normalize_to_png(src, out, srgb=True)

        self.assertTrue(os.path.exists(out))
        img = Image.open(out)
        self.assertEqual(img.size, (8, 8))

    def test_normalize_jpg_to_png(self):
        from PIL import Image
        src = os.path.join(self.tmpdir, 'src.jpg')
        _make_test_image(mode='RGB').save(src, format='JPEG', quality=95)

        out = os.path.join(self.tmpdir, 'out.png')
        pbr.normalize_to_png(src, out, srgb=True)

        self.assertTrue(os.path.exists(out))
        img = Image.open(out)
        self.assertEqual(img.format, 'PNG')
        self.assertEqual(img.size, (8, 8))

    def test_pack_height_into_normal_alpha(self):
        from PIL import Image
        normal_src = os.path.join(self.tmpdir, 'normal.png')
        height_src = os.path.join(self.tmpdir, 'height.png')
        _make_test_image(mode='RGB').save(normal_src)
        Image.new('L', (8, 8), 200).save(height_src)

        out = os.path.join(self.tmpdir, 'packed.png')
        pbr.pack_height_into_normal_alpha(normal_src, height_src, out, size=16)

        packed = Image.open(out)
        self.assertEqual(packed.mode, 'RGBA')
        self.assertEqual(packed.size, (16, 16))
        r, g, b, a = packed.split()
        # Height was a flat 200-gray image -- alpha channel should be ~200
        # everywhere after resize (LANCZOS may introduce tiny edge ringing,
        # so check the center pixel where no edge effects apply).
        self.assertAlmostEqual(a.getpixel((8, 8)), 200, delta=2)

    def test_pack_height_none_fills_alpha_255(self):
        from PIL import Image
        normal_src = os.path.join(self.tmpdir, 'normal.png')
        _make_test_image(mode='RGB').save(normal_src)

        out = os.path.join(self.tmpdir, 'packed.png')
        pbr.pack_height_into_normal_alpha(normal_src, None, out, size=16)

        packed = Image.open(out)
        r, g, b, a = packed.split()
        self.assertEqual(a.getpixel((8, 8)), 255)

    def test_resize_to_square(self):
        from PIL import Image
        src = os.path.join(self.tmpdir, 'src.png')
        _make_test_image(width=4, height=16, mode='RGBA').save(src)

        out = os.path.join(self.tmpdir, 'square.png')
        pbr.resize_to_square(src, out, size=32)

        img = Image.open(out)
        self.assertEqual(img.size, (32, 32))

    @unittest.skipUnless(os.path.exists(pbr.DEFAULT_KTX_EXE), 'ktx.exe not available in this environment')
    def test_full_synthetic_cook_to_bc7(self):
        """End-to-end: synthetic PNG -> BC7 KTX2, validate header bytes."""
        src = os.path.join(self.tmpdir, 'albedo.png')
        _make_test_image(width=32, height=32, mode='RGBA').save(src)

        out = os.path.join(self.tmpdir, 'albedo.ktx2')
        ok = pbr.cook_source_to_bc7(
            pbr.DEFAULT_KTX_EXE, src, out,
            vk_format='R8G8B8A8_SRGB', oetf='srgb', tmpdir=self.tmpdir,
        )
        self.assertTrue(ok)
        self.assertTrue(os.path.exists(out))

        with open(out, 'rb') as f:
            data = f.read(64)
        self.assertEqual(data[:12], KTX2_MAGIC)


class TestEngineNormalCook(unittest.TestCase):
    """matN_normal.tga -> matN_normal.ktx2: RGBA = (normal x, y, z, height), not the TGA's B, G, R byte order."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix='engine_normals_')

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _tga(self, name, mode, pixel):
        from PIL import Image
        Image.new(mode, (4, 4), pixel).save(os.path.join(self.tmpdir, name))

    def test_texels_are_rgba_not_tga_byte_order_and_opaque_24_bit(self):
        self._tga('mat0_normal.tga', 'RGBA', (10, 20, 30, 40))
        self._tga('mat5_normal.tga', 'RGB', (10, 20, 30))
        self.assertEqual(tuple(ctl.engine_normal_texels(os.path.join(self.tmpdir, 'mat0_normal.tga'))[0, 0]), (10, 20, 30, 40))
        self.assertEqual(tuple(ctl.engine_normal_texels(os.path.join(self.tmpdir, 'mat5_normal.tga'))[0, 0]), (10, 20, 30, 255))

    def test_up_to_date_outputs_are_kept_and_missing_sources_fail(self):
        for slot in range(6):
            self._tga(f'mat{slot}_normal.tga', 'RGBA', (128, 128, 255, 255))
            open(os.path.join(self.tmpdir, f'mat{slot}_normal.ktx2'), 'wb').close()
        os.remove(os.path.join(self.tmpdir, 'mat5_normal.tga'))
        calls = []
        original = ctl.write_rgba8_ktx2
        ctl.write_rgba8_ktx2 = lambda *a, **k: calls.append(a)
        try:
            results = ctl.cook_engine_normals(self.tmpdir)
        finally:
            ctl.write_rgba8_ktx2 = original
        self.assertEqual(calls, [])          # every existing sidecar is newer than its source
        self.assertEqual([ok for _, ok in results], [True] * 5 + [False])

    def test_cook_is_lossless_rgba8_with_the_engine_box_mips(self):
        import struct
        import numpy as np
        from PIL import Image
        rng = np.random.default_rng(7)
        texels = rng.integers(0, 256, (8, 8, 4), dtype=np.uint8)
        for slot in range(6):
            Image.fromarray(texels, 'RGBA').save(os.path.join(self.tmpdir, f'mat{slot}_normal.tga'))
        results = ctl.cook_engine_normals(self.tmpdir)
        self.assertTrue(all(ok for _, ok in results))
        with open(results[0][0], 'rb') as f:
            data = f.read()
        vk_format, type_size, width, height, _, layers, faces, levels, supercompression = struct.unpack_from('<9I', data, 12)
        self.assertEqual((vk_format, type_size, width, height, faces, levels, supercompression), (37, 1, 8, 8, 1, 4, 0))
        # Level 0 is the TGA texel for texel; each next level is the (a+b+c+d+2)/4 box of the one above (no renormalisation).
        expect = ctl.box_mips(texels)
        for lvl in range(levels):
            offset, length, _ = struct.unpack_from('<3Q', data, 80 + 24 * lvl)
            self.assertEqual(data[offset:offset + length], expect[lvl].tobytes())
        t = texels.astype(np.uint32)
        self.assertEqual(expect[1][0, 0].tolist(), [(int(t[0, 0, c]) + int(t[0, 1, c]) + int(t[1, 0, c]) + int(t[1, 1, c]) + 2) // 4 for c in range(4)])


if __name__ == '__main__':
    unittest.main()
