"""sim_determinism.data_fingerprint leaves the deploy-cooked render-only tree sidecars out of the stock part (W138h)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import sim_determinism as sim  # noqa: E402


def _install(tmp_path):
    inst = tmp_path / "inst"
    (inst / "data" / "tgl" / "128").mkdir(parents=True)
    (inst / "data" / "tgl" / "128" / "a_palm.tga").write_bytes(b"stock")
    return inst


def test_pinned_tree_sidecar_does_not_move_the_stock_digest(tmp_path, monkeypatch):
    pins = tmp_path / "tree-upscale.json"
    pins.write_text(json.dumps({"a_palm": {"ktx2_sha256": "00" * 32}}))
    monkeypatch.setattr(sim, "TREE_UPSCALE_PINS", str(pins))
    inst = _install(tmp_path)
    manifest = {"overlay": {}, "stock_digest": ""}
    before = sim.data_fingerprint(str(inst), manifest)
    (inst / "data" / "tgl" / "128" / "a_palm.ktx2").write_bytes(b"cooked sidecar")
    after = sim.data_fingerprint(str(inst), manifest)
    assert after["stock_digest"] == before["stock_digest"]
    # Anything else loose in the tree still moves it: only the pinned stems are render-only.
    (inst / "data" / "tgl" / "128" / "a_oak.ktx2").write_bytes(b"not pinned")
    assert sim.data_fingerprint(str(inst), manifest)["stock_digest"] != before["stock_digest"]


def test_missing_pin_file_exempts_nothing(tmp_path):
    assert sim.render_only_sidecars(str(tmp_path / "absent.json")) == frozenset()
    bad = tmp_path / "bad.json"
    bad.write_text("{nope")
    assert sim.render_only_sidecars(str(bad)) == frozenset()


def test_repo_pins_cover_the_shipped_sidecars():
    assert "data/tgl/128/a_palm.ktx2" in sim.render_only_sidecars()
