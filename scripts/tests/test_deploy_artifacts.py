"""Trigger: deploy_payload.py and asset_parity.py; diagnostics never become payload."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import asset_parity as ap
import deploy_payload as dp

BAD = ("logs/a.txt", "captures/a.png", "debug_state/state.json", "_rdoc/a.png",
       "nested/old_bak/a.glb", "nested/a.orig_bak", "nested/a.rdc", "nested/a.LOG",
       "mods/mc2-industrial-kit/content/a.mc2ir.json")


def test_deploy_filters_and_prunes_artifacts_without_touching_sources(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda _: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda _: None)
    source, target = tmp_path / "source", tmp_path / "target"
    source.write_bytes(b"asset")
    items = [(str(source), rel, "support") for rel in (*BAD, "data/tgl/live.glb")]
    for rel in BAD:
        path = target / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"old diagnostic")
    hashes = dp.deploy(items, str(target), False)
    assert set(hashes) == {"data/tgl/live.glb"}
    assert all(not (target / rel).exists() for rel in BAD)
    assert not (target / "nested/old_bak").exists()
    assert source.read_bytes() == (target / "data/tgl/live.glb").read_bytes()
    assert all(ap.is_excluded(rel) for rel in BAD)
    assert not ap.is_excluded("data/tgl/live.glb")


def test_bootstrap_omits_nested_artifacts(tmp_path):
    src, target = tmp_path / "src", tmp_path / "target"
    for rel in (*BAD, "data/nested/old_bak/a.glb", "data/a.rdc", "assets/live.png"):
        path = src / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x")
    dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    assert [p.relative_to(target).as_posix() for p in target.rglob("*") if p.is_file()] == ["assets/live.png"]


def test_prune_preserves_crash_evidence(tmp_path):
    crash = tmp_path / "crashes/a.dmp"
    crash.parent.mkdir()
    crash.write_bytes(b"evidence")
    with pytest.raises(SystemExit):
        dp.retire_unowned(str(tmp_path), {})
    assert crash.read_bytes() == b"evidence"
