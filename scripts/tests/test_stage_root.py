"""Staging defaults live on E: and a nearly full drive is refused before a copy starts."""
import os
import sys
import time
from collections import namedtuple
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools"))
import stage_root


@pytest.mark.parametrize("mod", ["mod_smoke", "battery", "reload_leak", "sim_trace_diff", "sound_smoke", "tools_smoke"])
def test_tools_no_longer_default_to_g(mod):
    text = (ROOT / "tools" / f"{mod}.py").read_text(encoding="utf-8")
    assert 'Path("G:/' not in text and "Path('G:/" not in text and '"G:/mc2-battery' not in text


def test_default_root_is_on_e():
    assert str(stage_root.default_root("mod_smoke")).replace("\\", "/").endswith("/mod_smoke-stage")


@pytest.mark.skipif(os.name != "nt", reason="Windows G: drive containment")
def test_g_staging_is_allowed_only_under_lane_temp(monkeypatch):
    usage = namedtuple("usage", "total used free")
    monkeypatch.setattr(stage_root.shutil, "disk_usage", lambda p: usage(0, 0, 20 * 2**30))
    monkeypatch.setenv("TMP", "G:/mc2-tmp")
    stage_root.preflight("G:/mc2-tmp/mod_smoke-stage")
    with pytest.raises(OSError, match="allowed only under G:/mc2-tmp"):
        stage_root.preflight("G:/mc2-battery/mod_smoke-stage")


def test_preflight_refuses_under_15_gb(tmp_path, monkeypatch):
    usage = namedtuple("usage", "total used free")
    monkeypatch.setattr(stage_root.shutil, "disk_usage", lambda p: usage(0, 0, 10 * 2**30))
    with pytest.raises(OSError, match="15 GB"):
        stage_root.preflight(tmp_path / "not" / "yet")
    monkeypatch.setattr(stage_root.shutil, "disk_usage", lambda p: usage(0, 0, 20 * 2**30))
    stage_root.preflight(tmp_path / "not" / "yet")


def test_prune_refuses_unmarked_root(tmp_path):
    old = tmp_path / "old"
    old.mkdir()
    t = time.time() - 3 * 86400
    os.utime(old, (t, t))
    assert stage_root.prune_old(tmp_path) == 0 and old.exists()
    (tmp_path / stage_root.STAGE_MARKER).write_text("")
    assert stage_root.prune_old(tmp_path) == 1


def test_prune_removes_only_stages_older_than_a_day(tmp_path_factory):
    tmp_path = tmp_path_factory.mktemp("x") / "t-stage"
    tmp_path.mkdir()
    old, new = tmp_path / "old", tmp_path / "new"
    old.mkdir()
    new.mkdir()
    (tmp_path / "keep.txt").write_text("x")
    t = time.time() - 3 * 86400
    os.utime(old, (t, t))
    assert stage_root.prune_old(tmp_path) == 1
    assert not old.exists() and new.exists() and (tmp_path / "keep.txt").exists()


def test_prune_preserves_persistent_cache(tmp_path):
    root = tmp_path / 'mod_smoke-stage'
    cache = root / '_cache'
    cache.mkdir(parents=True)
    old = time.time() - 3 * 86400
    os.utime(cache, (old, old))
    assert stage_root.prune_old(root, exclude=('_cache',)) == 0
    assert cache.is_dir()
