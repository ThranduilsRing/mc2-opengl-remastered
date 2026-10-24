#!/usr/bin/env python3
# scripts/tests/test_deploy_fast_path.py
"""Tests for deploy_payload's size+mtime_ns fast path and --full-verify.

An unchanged file must not be re-copied or re-hashed; a changed file must be;
--full-verify must restore the old copy-everything behaviour. Terrain materials
are stubbed (they have their own tests in test_install_hygiene.py).

Run:
  py -3 -m pytest scripts/tests/test_deploy_fast_path.py -v
"""
from __future__ import annotations

import os
import sys
from pathlib import Path
import types
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def _setup(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    src = tmp_path / "src.txt"
    src.write_bytes(b"payload-v1")
    target = tmp_path / "target"
    target.mkdir()
    rel = "data/src.txt"
    return str(src), str(target), rel


def _deploy(src, target, rel, *, cache, full_verify=False, preserve=()):
    return dp.deploy([(src, rel, "support")], target, False, preserve=preserve,
                     cache=cache, full_verify=full_verify)


def _cache_after(src, target, rel, values, src_commit="abc123"):
    dp.write_manifest(target, values, src_commit)
    return dp.read_manifest(target)


def test_unchanged_file_is_skipped_and_manifest_round_trips(tmp_path, monkeypatch):
    src, target, rel = _setup(tmp_path, monkeypatch)
    hashes = _deploy(src, target, rel, cache={})
    cache = _cache_after(src, target, rel, hashes)
    assert cache[rel][2] is not None, "deploy must record the source mtime_ns"
    target_file = Path(target) / rel
    mtime_ns = target_file.stat().st_mtime_ns

    def no_copy(*args, **kwargs):
        raise AssertionError("unchanged source must not be re-copied")

    monkeypatch.setattr(dp.shutil, "copyfile", no_copy)
    skipped = _deploy(src, target, rel, cache=cache)
    assert skipped[rel][:2] == cache[rel][:2], "skip must reuse the recorded sha256/size"
    assert target_file.stat().st_mtime_ns == mtime_ns


def test_changed_file_is_copied_and_rehashed(tmp_path, monkeypatch):
    src, target, rel = _setup(tmp_path, monkeypatch)
    cache = _cache_after(src, target, rel, _deploy(src, target, rel, cache={}))
    Path(src).write_bytes(b"payload-v2")
    os.utime(src, ns=(os.stat(src).st_atime_ns, cache[rel][2] + 1_000_000))

    copied = _deploy(src, target, rel, cache=cache)
    target_file = Path(target) / rel
    assert target_file.read_bytes() == b"payload-v2"
    assert copied[rel][0] == dp.sha256_file(str(target_file))
    assert copied[rel][0] != cache[rel][0]
    assert copied[rel][1] == len(b"payload-v2")


def test_full_verify_copies_even_unchanged_files(tmp_path, monkeypatch):
    src, target, rel = _setup(tmp_path, monkeypatch)
    cache = _cache_after(src, target, rel, _deploy(src, target, rel, cache={}))
    copies = []
    real_copy = dp.shutil.copyfile

    def counting_copy(a, b, *args, **kwargs):
        copies.append(a)
        return real_copy(a, b, *args, **kwargs)

    monkeypatch.setattr(dp.shutil, "copyfile", counting_copy)
    _deploy(src, target, rel, cache=cache)
    assert copies == [], "fast path must skip the unchanged file"
    _deploy(src, target, rel, cache=cache, full_verify=True)
    assert copies == [src], "--full-verify must copy the unchanged file again"


def test_lane_worktree_must_name_its_install():
    assert dp.implicit_target_refusal("A:/Games/mc2-flash3", None)
    assert dp.implicit_target_refusal("a:\\games\\mc2-lane7", None)
    assert dp.implicit_target_refusal("A:/Games/mc2-flash3", "E:/mc2-installs/flash3") is None
    assert dp.implicit_target_refusal("A:/Games/mc2-main", None) is None
    assert dp.implicit_target_refusal("D:/work/checkout", None) is None


def test_executable_same_size_replacement_is_repaired_despite_cache(tmp_path, monkeypatch):
    src, target, _ = _setup(tmp_path, monkeypatch)
    items=[(src, "mc2.exe", "exe")]
    hashes=dp.deploy(items, target, False)
    cache=_cache_after(src, target, "mc2.exe", hashes)
    installed=Path(target)/"mc2.exe"
    installed.write_bytes(b"x"*installed.stat().st_size)
    dp.deploy(items, target, False, cache=cache)
    assert installed.read_bytes()==Path(src).read_bytes()


@pytest.mark.parametrize("full_verify", [False, True])
def test_preserved_executable_must_match_source(tmp_path, monkeypatch, full_verify):
    src, target, _ = _setup(tmp_path, monkeypatch)
    installed=Path(target)/"mc2.exe"
    installed.write_bytes(b"x"*Path(src).stat().st_size)
    with pytest.raises(SystemExit):
        dp.deploy([(src,"mc2.exe","exe")],target,False,preserve=["mc2.exe"],full_verify=full_verify)
    installed.write_bytes(Path(src).read_bytes())
    assert "mc2.exe" in dp.deploy([(src,"mc2.exe","exe")],target,False,preserve=["mc2.exe"],full_verify=full_verify)


@pytest.mark.parametrize("bad_target", ["sha", "bytes"])
def test_installed_exe_refuses_before_manifest_even_if_copy_retains_it(tmp_path, monkeypatch, bad_target):
    head="a"*40
    def image(sha):
        return ("MC2FP1|"+sha+"|0|claude/test|2026-10-24T12:00:00|A:/Games/test|FP1END").encode()
    build=tmp_path/"build"; build.mkdir()
    target=tmp_path/"target"; target.mkdir()
    (build/"mc2.exe").write_bytes(image(head[:12]))
    (target/"mc2.exe").write_bytes(image("b"*12) if bad_target=="sha" else image(head[:12])+b"tampered")
    monkeypatch.setattr(dp,"check_exe_fresh",lambda *a,**k: None)
    monkeypatch.setattr(dp,"git_head",lambda *a: head)
    monkeypatch.setattr(dp,"enumerate_payload",lambda *a,**k: [])
    monkeypatch.setattr(dp,"check_free_space_for_payload",lambda *a: None)
    monkeypatch.setattr(dp,"check_hardlink_isolation",lambda *a: None)
    monkeypatch.setattr(dp,"deploy",lambda *a,**k: {})
    monkeypatch.setattr(dp,"write_manifest",lambda *a: pytest.fail("published success for wrong installed exe"))
    args=types.SimpleNamespace(allow_stale_exe=False,build64_root=None,require_clean=True,
                              full_verify=False,allow_stale_pdb=False,preserve=[])
    with pytest.raises(SystemExit):
        dp.run_deploy(args,str(tmp_path),str(build),str(target),"mc2.exe","mc2.pdb")
