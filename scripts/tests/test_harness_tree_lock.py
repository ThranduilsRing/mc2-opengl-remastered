"""build_contract_harnesses serializes configure+build per mc2_tests tree (W138 laptop gate: the contracts board's
audio_music_stream row and the harnesses step built build64-tests at once and MSBuild failed with C1041 on the shared PDB)."""
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import build_contract_harnesses as bch
from ninja_build import TreeLock


def _fake_build(monkeypatch):
    calls = []
    monkeypatch.setattr(bch, "cmake_exe", lambda: "cmake")
    monkeypatch.setattr(bch.bd, "configure_args", lambda *a, **k: ["-S", "src"])
    monkeypatch.setattr(bch.bd, "build_args", lambda *a, **k: ["cmake", "--build"])
    monkeypatch.setattr(bch.subprocess, "run", lambda cmd, **k: calls.append(cmd) or SimpleNamespace(returncode=0, stdout="", stderr=""))
    return calls


def test_a_held_tree_blocks_the_build_and_a_free_one_builds(monkeypatch, tmp_path):
    calls = _fake_build(monkeypatch)
    monkeypatch.setattr(bch, "TREE_LOCK_WAIT_S", 0)
    holder = TreeLock(str(tmp_path), "build64-tests")
    assert holder.acquire(0)
    try:
        ok, detail = bch.build_and_run_unit_tests("RelWithDebInfo", run=False, build_root=tmp_path)
        assert not ok and "another build held build64-tests" in detail and calls == []
    finally:
        holder.close()
    ok, detail = bch.build_and_run_unit_tests("RelWithDebInfo", run=False, build_root=tmp_path)
    assert ok and detail.startswith("built in") and len(calls) == 2
    # the lock is released after the build: a later holder gets it at once
    later = TreeLock(str(tmp_path), "build64-tests")
    assert later.acquire(0)
    later.close()


def test_a_build_failure_still_releases_the_tree(monkeypatch, tmp_path):
    _fake_build(monkeypatch)
    monkeypatch.setattr(bch.subprocess, "run", lambda cmd, **k: SimpleNamespace(returncode=1, stdout="x error C1041: pdb", stderr=""))
    ok, detail = bch.build_and_run_unit_tests("RelWithDebInfo", run=False, build_root=tmp_path)
    assert not ok and detail.startswith("configure failed:")
    later = TreeLock(str(tmp_path), "build64-tests")
    assert later.acquire(0)
    later.close()
