import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.content_pipeline import provenance, texcook
from tools.mc2texcook.cook_burnin_bc7 import cook


def test_records_preserve_siblings_and_track_recipe_and_content(tmp_path):
    a, b = tmp_path / "a.ktx2", tmp_path / "b.ktx2"
    a.write_bytes(b"a")
    b.write_bytes(b"b")
    for output in (a, b):
        provenance.record(output, source_hash="1" * 64, tool="test", version="v1", params={"tier": 128})
    first = (tmp_path / "cook.json").read_bytes()
    provenance.record(a, source_hash="1" * 64, tool="test", version="v1", params={"tier": 128})
    assert (tmp_path / "cook.json").read_bytes() == first
    provenance.record(a, source_hash="2" * 64, tool="test", version="v2", params={"tier": 256})
    doc = json.loads((tmp_path / "cook.json").read_bytes())["assets"]
    assert doc["b.ktx2"]["sourceHash"] == "1" * 64
    assert doc["a.ktx2"]["params"] == {"tier": 256}
    assert doc["a.ktx2"]["toolVersion"] == "v2"
    assert doc["a.ktx2"]["outputHash"] == provenance.sha256(b"a")


def test_legacy_batch_cli_emits_reproducible_source_receipt(tmp_path, monkeypatch):
    from tools.mc2texcook import batch_cook
    source = tmp_path / "source"
    source.mkdir()
    image = source / "test.tga"
    Image.new("RGBA", (8, 8), (120, 60, 30, 255)).save(image)
    for name in ("a", "b"):
        monkeypatch.setattr(sys, "argv", ["batch_cook", "--src", str(source),
                            "--dst", str(tmp_path / name), "--preset", "albedo"])
        assert batch_cook.main() == 0
    assert (tmp_path / "a/cook.json").read_bytes() == (tmp_path / "b/cook.json").read_bytes()
    record = json.loads((tmp_path / "a/cook.json").read_bytes())["assets"]["test.ktx2"]
    assert record["sourceHash"] == provenance.sha256(image.read_bytes())
    assert record["outputHash"] == provenance.sha256((tmp_path / "a/test.ktx2").read_bytes())


@pytest.mark.skipif(not texcook.TEXCONV.is_file(), reason="CPU texconv unavailable")
@pytest.mark.parametrize("preset", ["burnin", "base_color"])
def test_bc7_independent_cooks_match_and_fit_loader(tmp_path, preset):
    # Rectangular dimensions exercise sub-block levels down to 1x1.
    rgba = np.random.default_rng(1).integers(0, 256, (8, 16, 4), dtype=np.uint8)
    src = tmp_path / "test.burnin.tga"
    Image.fromarray(rgba).save(src)
    original = src.read_bytes()
    a = cook(src, tmp_path / "a/test.burnin.ktx2", preset)
    b = cook(src, tmp_path / "b/test.burnin.ktx2", preset)
    assert a.read_bytes() == b.read_bytes()
    assert (a.parent / "cook.json").read_bytes() == (b.parent / "cook.json").read_bytes()
    vk, w, h, levels = texcook.read_ktx2(a.read_bytes())
    assert (vk, w, h) == (145, 16, 8)   # base_color is UNORM: shaders read raw sRGB bytes at gamma 1.0
    assert [len(x) for x in levels] == ([128] if preset == "burnin" else [128, 32, 16, 16, 16])
    assert src.read_bytes() == original
    with pytest.raises(ValueError, match="overwrite"):
        cook(src, src)
