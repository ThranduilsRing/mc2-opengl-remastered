"""Stock override loader inputs: atlas triplets, RGB paint keys and deploy coverage."""
from pathlib import Path
import sys

import numpy as np
import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import deploy_payload as dp


@pytest.mark.parametrize("kind,count", [("mech", 30), ("vehicle", 16)])
def test_stock_pbr_loader_inputs(kind, count):
    tree = f"data/textures/{kind}_pbr"
    assert tree in dp.GAME_LOOSE_ART_TREES
    albedos = sorted((ROOT / tree).glob("*_albedo.png"))
    assert len(albedos) == count
    for albedo in albedos:
        stem = albedo.name.removesuffix("_albedo.png")
        maps = [albedo.with_name(f"{stem}_{suffix}.png") for suffix in ("albedo", "normal", "orm")]
        images = [np.asarray(Image.open(p).convert("RGB")) for p in maps]
        edge = 256 if stem == "jeep" else 512
        assert all(image.shape == (edge, edge, 3) for image in images), stem
        # Lossless albedo must retain paint-mask primaries. No lossy albedo cook is shipped.
        if kind == "mech":
            assert any(((images[0][:, :, c] >= 16) &
                        (images[0][:, :, (c + 1) % 3] == 0) &
                        (images[0][:, :, (c + 2) % 3] == 0)).any() for c in range(3)), stem
        print(f"{kind}/{stem}: albedo normal orm {edge}x{edge}")

