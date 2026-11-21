#!/usr/bin/env python3
"""Unit tests for cook_grass_turf.py (GRASS-GROUND-NORMAL): format, mips, array slot, determinism, gap share."""
from __future__ import annotations

import os
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

_TOOLS_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_TOOLS_DIR))

import cook_grass_turf as turf      # noqa: E402
import cook_terrain_layers as ctl   # noqa: E402

_REPO = _TOOLS_DIR.parent.parent
_SRC = "G:/mc2-assets/pbr/dl-2026-10-23/ground"


def _ktx2_header(path):
    with open(path, "rb") as f:
        head = f.read(48)
    assert head[:12] == b"\xabKTX 20\xbb\r\n\x1a\n"
    vk_format, _type, w, h, _d, layers, faces, levels, _sc = struct.unpack("<9I", head[12:48])
    return vk_format, w, h, layers, faces, levels


def _flat(size, rng):
    """A noisy GL normal map standing in for a downloaded source."""
    n = np.zeros((size, size, 3))
    n[..., :2] = rng.normal(0.0, 0.2, (size, size, 2))
    n[..., 2] = 1.0
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return np.clip(np.rint((n + 1.0) * 127.5), 0, 255).astype(np.uint8)


class TestGrassTurf(unittest.TestCase):
    SIZE = 128

    def setUp(self):
        rng = np.random.default_rng(1)
        self.blade, self.dirt = _flat(self.SIZE, rng), _flat(self.SIZE, rng)

    def test_texels_are_valid_tangent_normals_with_height(self):
        t = turf.turf_texels(self.blade, self.dirt, size=self.SIZE)
        self.assertEqual(t.shape, (self.SIZE, self.SIZE, 4))
        self.assertEqual(t.dtype, np.uint8)
        n = t[..., :3].astype(np.float64) / 127.5 - 1.0
        self.assertTrue(np.all(n[..., 2] > 0.0))                       # never points into the ground
        self.assertLess(np.abs(np.linalg.norm(n, axis=-1) - 1.0).max(), 0.03)
        self.assertEqual((int(t[..., 3].min()), int(t[..., 3].max())), (0, 255))  # height spans the full alpha range

    def test_deterministic(self):
        a = turf.turf_texels(self.blade, self.dirt, size=self.SIZE)
        b = turf.turf_texels(self.blade, self.dirt, size=self.SIZE)
        self.assertTrue(np.array_equal(a, b))

    def test_gap_share_matches_the_owner_reference(self):
        m = turf.gap_mask(np.random.default_rng(turf.SEED), 512)
        self.assertGreater((m > 0.5).mean(), 0.18)
        self.assertLess((m > 0.5).mean(), 0.32)

    def test_ktx2_matches_the_engine_detail_normal_layout(self):
        t = turf.turf_texels(self.blade, self.dirt, size=self.SIZE)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, turf.OUT_NAME)
            ctl.write_bc7_ktx2(p, ctl.box_mips(t))
            vk_format, w, h, layers, faces, levels = _ktx2_header(p)
        # BC7_UNORM (145), square, single image, full mip chain: TerrainFieldStore::setMaterialNormals accepts it next to
        # the matN_normal.ktx2 sidecars only with the same format, size and chain.
        self.assertEqual((vk_format, w, h, layers, faces), (145, self.SIZE, self.SIZE, 0, 1))
        self.assertEqual(levels, int(np.log2(self.SIZE)) + 1)
        self.assertEqual(turf.SIZE, 2048)   # the engine normal layers are 2048

    def test_engine_slot_and_names_agree(self):
        header = (_REPO / "mclib" / "terrain_normal_load.h").read_text(encoding="utf-8")
        src = re.search(r'kGrassTurfSource = "([^"]+)"', header).group(1)
        self.assertEqual(Path(src).with_suffix(".ktx2").as_posix(), "data/terrain_layers/" + turf.OUT_NAME)
        layers = (_REPO / "shaders" / "include" / "terrain_mat_layers.hglsl").read_text(encoding="utf-8")
        self.assertRegex(layers, r"#define MAT_LAYER_GRASS\s+1\b")
        deploy = (_REPO / "scripts" / "deploy_payload.py").read_text(encoding="utf-8")
        self.assertIn('"data/terrain_layers/' + turf.OUT_NAME + '"', deploy)

    @unittest.skipUnless(os.path.isdir(_SRC), "CC0 sources not on this host")
    def test_real_sources_cook_full_size(self):
        with tempfile.TemporaryDirectory() as d:
            p = turf.cook(_SRC, d)
            vk_format, w, h, _l, _f, levels = _ktx2_header(p)
        self.assertEqual((vk_format, w, h, levels), (145, 2048, 2048, 12))


if __name__ == "__main__":
    unittest.main()
