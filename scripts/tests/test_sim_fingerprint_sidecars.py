"""sim_determinism.data_fingerprint counts a deploy-cooked render-only tree sidecar at its stock cook size (W138h)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import sim_determinism as sim  # noqa: E402

STOCK = b"s" * 22
UPSCALED = b"u" * 140


def _install(tmp_path, ktx2=None):
    inst = tmp_path / "inst"
    (inst / "data" / "tgl" / "128").mkdir(parents=True)
    (inst / "data" / "tgl" / "128" / "a_palm.tga").write_bytes(b"stock")
    if ktx2 is not None:
        (inst / "data" / "tgl" / "128" / "a_palm.ktx2").write_bytes(ktx2)
    return inst


def _pins(tmp_path, monkeypatch, **row):
    pins = tmp_path / "tree-upscale.json"
    pins.write_text(json.dumps({"a_palm": dict({"ktx2_sha256": "00" * 32}, **row)}))
    monkeypatch.setattr(sim, "TREE_UPSCALE_PINS", str(pins))


def _digest(inst):
    return sim.data_fingerprint(str(inst), {"overlay": {}, "stock_digest": ""})["stock_digest"]


def test_deploy_upscale_leaves_the_stock_digest_where_the_stock_cook_put_it(tmp_path, monkeypatch):
    _pins(tmp_path, monkeypatch, bytes=len(UPSCALED), stock_bytes=len(STOCK))
    stock = _digest(_install(tmp_path / "a", STOCK))
    assert _digest(_install(tmp_path / "b", UPSCALED)) == stock
    # The stock entry stays counted: an install without the sidecar, or with another size, is a different stock part.
    assert _digest(_install(tmp_path / "c")) != stock
    assert _digest(_install(tmp_path / "d", b"x" * 7)) != stock
    # Anything else loose in the tree still moves it: only the pinned stems are exempt.
    inst = _install(tmp_path / "e", UPSCALED)
    (inst / "data" / "tgl" / "128" / "a_oak.ktx2").write_bytes(b"not pinned")
    assert _digest(inst) != stock


def test_pin_without_stock_size_counts_the_file_as_it_is(tmp_path, monkeypatch):
    _pins(tmp_path, monkeypatch, bytes=len(UPSCALED))
    assert _digest(_install(tmp_path / "a", STOCK)) != _digest(_install(tmp_path / "b", UPSCALED))


def test_missing_pin_file_exempts_nothing(tmp_path):
    assert sim.render_only_sidecars(str(tmp_path / "absent.json")) == frozenset()
    bad = tmp_path / "bad.json"
    bad.write_text("{nope")
    assert sim.render_only_sidecars(str(bad)) == frozenset()


def test_repo_pins_cover_the_shipped_sidecars():
    sizes = sim.render_only_sidecar_sizes()
    assert "data/tgl/128/a_palm.ktx2" in sim.render_only_sidecars()
    assert all(up and stock for up, stock in sizes.values()), "every shipped pin names its upscaled and stock cook sizes"
