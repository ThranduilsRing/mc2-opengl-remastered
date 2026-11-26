"""Stock override loader inputs: atlas triplets, RGB paint keys and deploy coverage."""
from pathlib import Path
import sys

import numpy as np
import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import deploy_payload as dp


@pytest.mark.parametrize("kind,count", [("vehicle", 16)])
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
        print(f"{kind}/{stem}: albedo normal orm {edge}x{edge}")


def test_stock_mech_retextures_are_retired(tmp_path):
    import hashlib
    from tools.content_pipeline import cook_actor_pbr

    tree = "data/textures/mech_pbr"
    assert tree not in dp.GAME_LOOSE_ART_TREES
    assert tree not in cook_actor_pbr.TREES
    import json
    declared = json.loads((ROOT / "intentional-asset-changes.json").read_text())
    assert not any(rel.startswith(tree + "/") for rel in declared)
    assert not any(rel.startswith(tree + "/") for rel in dp.actor_pbr_payload(ROOT))
    assert len(dp.actor_pbr_payload(ROOT)) == 32
    retired = {rel: sha for rel, sha in dp.RETIRED_SEED_PAYLOAD.items() if rel.startswith(tree + "/")}
    assert len(retired) == 150
    assert not list((ROOT / tree).glob("*.png"))
    # Exercise the real retired path with matching, modified and unowned bytes.
    rel = tree + "/bushwacker_albedo.png"
    path = tmp_path / rel
    path.parent.mkdir(parents=True)
    path.write_bytes(b"retired")
    original = dp.RETIRED_SEED_PAYLOAD[rel]
    dp.RETIRED_SEED_PAYLOAD[rel] = hashlib.sha256(b"retired").hexdigest()
    modified = path.with_name("bushwacker_normal.png")
    modified.write_bytes(b"owner modified")
    unowned = path.with_name("owner.png")
    unowned.write_bytes(b"owner art")
    try:
        dp.prune_retired_seed_payload(tmp_path)
    finally:
        dp.RETIRED_SEED_PAYLOAD[rel] = original
    assert not path.exists()
    assert modified.read_bytes() == b"owner modified"
    assert unowned.read_bytes() == b"owner art"

