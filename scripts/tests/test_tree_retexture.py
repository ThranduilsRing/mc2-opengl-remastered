"""stock-tree-upscale: pins, deploy wiring and sidecar decode of the generated 1024 tree atlases.

The sidecars are generated (tools/tree_retexture/gen_stock_trees.py), not tracked; the decode checks run where they exist
(after a cook or a deploy) and texconv is present, and skip otherwise.
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "tools"))
import deploy_payload  # noqa: E402
from content_pipeline import texcook  # noqa: E402
sys.path.insert(0, str(REPO / "tools/tree_retexture"))
import gen_stock_trees as gen  # noqa: E402

PINS = json.loads((REPO / "data/kit/tree-upscale.json").read_text())
STOCK = json.loads((REPO / "data/kit/stock-textures.json").read_text())["sha256"]


def test_every_pin_replaces_a_stock_tga_of_the_same_stem():
    assert PINS, "no pinned atlases"
    for stem, row in PINS.items():
        assert stem in gen.ATLASES
        assert STOCK[f"data/tgl/128/{stem}.tga"] == row["source_sha256"], stem
        assert row["size"] == 1024 and row["bytes"] > 1024 * 1024
        assert row["coverage"] >= row["stock_coverage"] * 0.95, f"{stem}: canopy thinner than stock"


def test_recipes_and_pins_agree():
    assert set(PINS) == set(gen.ATLASES)
    for recipe in gen.ATLASES.values():
        if recipe.get("ramp"):
            stops = gen.ramp_colors(recipe["ramp"])
            assert np.all(np.diff(gen.luma(stops)) >= 0), "ramp must rise in value"


def test_foliage_ramps_lift_stock_values():
    # Night fix, content side: the leaf ramps sit above the stock leaf albedo (about 30-45/255).
    for name in ("crown", "palm"):
        assert gen.luma(gen.ramp_colors(name)).mean() >= 60.0


def test_deploy_ships_exactly_the_pinned_sidecars():
    rows = deploy_payload.tree_upscale_payload(str(REPO))
    assert rows == {f"data/tgl/128/{s}.ktx2": r["ktx2_sha256"] for s, r in PINS.items()}


def test_failed_cook_removes_stale_sidecars(tmp_path):
    (tmp_path / "data/kit").mkdir(parents=True)
    (tmp_path / "data/tgl/128").mkdir(parents=True)
    (tmp_path / "data/kit/tree-upscale.json").write_text(json.dumps({"a_palm": {"ktx2_sha256": "0" * 64}}))
    stale = tmp_path / "data/tgl/128/a_palm.ktx2"
    stale.write_bytes(b"stale")

    class R:
        returncode = 1
        stdout = stderr = ""
    deploy_payload.ensure_tree_upscale(str(tmp_path), cooker=lambda *a, **k: R())
    assert not stale.exists(), "a sidecar that is not the pinned bytes must not ship"


def _dds_bc7(w, h, level0):
    hdr = bytearray(148)
    hdr[0:4] = b"DDS "
    struct.pack_into("<7I", hdr, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, len(level0), 0, 1)
    struct.pack_into("<2I", hdr, 76, 32, 0x4)
    hdr[84:88] = b"DX10"
    struct.pack_into("<5I", hdr, 128, 98, 3, 0, 1, 0)        # DXGI_FORMAT_BC7_UNORM, TEXTURE2D
    return bytes(hdr) + level0


@pytest.mark.parametrize("stem", sorted(PINS))
def test_sidecar_decodes(stem, tmp_path):
    path = REPO / "data/tgl/128" / f"{stem}.ktx2"
    if not path.is_file():
        pytest.skip("not generated here (deploy cooks it)")
    data = path.read_bytes()
    import hashlib
    assert hashlib.sha256(data).hexdigest() == PINS[stem]["ktx2_sha256"]
    vk, w, h, levels = texcook.read_ktx2(data)
    assert vk in (145, 146) and w == h == 1024           # square BC7: what mclib/texture_assets.cpp load() accepts
    assert len(levels) == 11
    for i, lv in enumerate(levels):
        e = max(1, 1024 >> i)
        assert len(lv) == ((e + 3) // 4) ** 2 * 16
    if not texcook.TEXCONV.is_file():
        pytest.skip("texconv absent: header checks only")
    src = tmp_path / "l0.dds"
    src.write_bytes(_dds_bc7(w, h, levels[0]))
    subprocess.run([str(texcook.TEXCONV), "-nologo", "-y", "-f", "R8G8B8A8_UNORM", "-ft", "png", "-o", str(tmp_path), str(src)],
                   check=True, capture_output=True)
    from PIL import Image
    rgba = np.asarray(Image.open(tmp_path / "l0.png").convert("RGBA"))
    cov = float((rgba[..., 3] >= 128).mean())
    assert abs(cov - PINS[stem]["coverage"]) < 0.01
    opaque = rgba[rgba[..., 3] >= 128][:, :3].astype(np.float32)
    clear = rgba[rgba[..., 3] < 128][:, :3].astype(np.float32)
    assert gen.luma(opaque).mean() > 35.0                    # brighter than the stock 30-45 leaf albedo floor
    # Dilated fringe: transparent texels carry leaf/bark colour, not a black halo.
    assert gen.luma(clear).mean() > 0.6 * gen.luma(opaque).mean()
