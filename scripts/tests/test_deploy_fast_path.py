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
import json
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def test_input_hashes_cover_kit_and_example_mod_but_ignore_game_cpp(tmp_path):
    import subprocess
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True,
                   **dp.console_child.child_kwargs(redirected=True))
    kit = tmp_path / "data/kit/industrial-kit.json"
    mod = tmp_path / "examples/mods/test/mod.json"
    game = tmp_path / "mclib/game.cpp"
    for path in (kit, mod, game):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("original")
    before = dp.deploy_inputs(tmp_path)
    game.write_text("code-only")
    assert dp.deploy_inputs(tmp_path) == before
    kit.write_text("changed")
    assert dp.deploy_inputs(tmp_path) != before
    kit.write_text("original")
    mod.write_text("changed")
    assert dp.deploy_inputs(tmp_path) != before


@pytest.fixture
def recorded_payload(tmp_path):
    target = tmp_path / "install"
    source = tmp_path / "source.glb"
    source.write_bytes(b"kit")
    path = target / "mods/mc2-industrial-kit/model.glb"
    path.parent.mkdir(parents=True)
    path.write_bytes(b"kit")
    rel = path.relative_to(target).as_posix()
    hashes = {rel: (dp.sha256_file(path), 3)}
    inputs = {"data/kit/industrial-kit.json": "pinned-input-hash"}
    dp.write_manifest(target, hashes, "a" * 40)
    dp.record_deploy_inputs(target, inputs, hashes, [(str(source), rel, "kit")])
    return target, inputs, path, source


def test_unchanged_inputs_skip_kit(recorded_payload):
    target, inputs, _, _ = recorded_payload
    assert dp.reusable_payload(None, target, inputs)[0]


def test_changed_input_requires_full_deploy(recorded_payload):
    target, inputs, _, _ = recorded_payload
    assert not dp.reusable_payload(None, target, inputs | {"new": "changed"})[0]


def test_malformed_reuse_evidence_requires_full_deploy(recorded_payload):
    target, inputs, _, _ = recorded_payload
    path = target / dp.REUSE_NAME
    saved = json.loads(path.read_text())
    saved["outputs"] = "broken inventory"
    path.write_text(json.dumps(saved))
    assert not dp.reusable_payload(None, target, inputs)[0]


@pytest.mark.parametrize("missing", [dp.MANIFEST_NAME, dp.REUSE_NAME, "mods/mc2-industrial-kit/model.glb"])
def test_missing_evidence_requires_full_deploy(recorded_payload, missing):
    target, inputs, _, _ = recorded_payload
    (target / missing).unlink()
    assert not dp.reusable_payload(None, target, inputs)[0]


@pytest.mark.parametrize("source", [False, True])
def test_sampled_mismatch_requires_full_deploy(recorded_payload, source):
    target, inputs, output, original = recorded_payload
    path = original if source else output
    path.write_bytes(b"bad")
    # Model byte drift that metadata alone cannot detect: forge the saved stamp.
    saved = json.loads((target / dp.REUSE_NAME).read_text())
    s = path.stat()
    stamp = [s.st_size, s.st_mtime_ns, s.st_ctime_ns]
    if source:
        saved["sources"][str(path)][0] = stamp
    else:
        saved["outputs"][path.relative_to(target).as_posix()] = stamp
    (target / dp.REUSE_NAME).write_text(json.dumps(saved))
    assert "mismatch" in dp.reusable_payload(None, target, inputs)[1]


def test_deploy_phase_timeout_names_phase_and_exits(tmp_path):
    import subprocess
    result = subprocess.run([sys.executable, "-c",
        "import sys,time; sys.path.insert(0,'scripts'); import deploy_payload as d; "
        "exec('with d.deploy_phase(\"injected stall\", .05):\\n time.sleep(10)')"],
        cwd=ROOT, capture_output=True, text=True, timeout=30,
        **dp.console_child.child_kwargs(redirected=True))
    assert result.returncode == 8
    assert "phase injected stall timed out" in result.stdout


def test_code_only_keeps_static_payload_and_does_not_cook(tmp_path, monkeypatch):
    src, target, rel = _setup(tmp_path, monkeypatch)
    hashes = _deploy(src, target, rel, cache={})
    dp.write_manifest(target, hashes, "a" * 40)
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda *a: pytest.fail("terrain recooked"))
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda *a: pytest.fail("normals recooked"))
    monkeypatch.setattr(dp, "verify_engine_normals_present", lambda *a: None)
    retained = dp.deploy([], target, False, cache=hashes, code_only=True)
    assert retained == hashes  # retained rows carry the earlier dest stamp unchanged
    assert (Path(target) / rel).read_bytes() == Path(src).read_bytes()


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


def test_mip_publish_replaces_link_only_after_copy(tmp_path, monkeypatch):
    src, target, _ = _setup(tmp_path, monkeypatch)
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    rel = "data/tgl/128/a_cp_island_tree_01_1.tga"
    dst = Path(target) / rel
    dst.parent.mkdir(parents=True)
    seed = tmp_path / "seed.tga"
    seed.write_bytes(b"seed-bytes")
    os.link(seed, dst)
    copy = dp.shutil.copyfile
    def checked_copy(source, destination):
        assert dst.exists() and dst.stat().st_nlink == 2
        assert Path(destination) != dst
        return copy(source, destination)
    monkeypatch.setattr(dp.shutil, "copyfile", checked_copy)
    _deploy(src, target, rel, cache={})
    assert dst.read_bytes() == Path(src).read_bytes()
    assert seed.read_bytes() == b"seed-bytes"
    assert dst.stat().st_nlink == seed.stat().st_nlink == 1


def _cache_after(src, target, rel, values, src_commit="abc123"):
    dp.write_manifest(target, values, src_commit)
    return dp.read_manifest(target)


@pytest.mark.parametrize("rel", ["data/plain.bin", "mc2.exe", "data/a.mcasset.json"])
def test_failed_publish_keeps_previous_payload(tmp_path, monkeypatch, rel):
    src, target, _ = _setup(tmp_path, monkeypatch)
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    dst = Path(target) / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(b"previous")
    def refuse(staged, live):
        assert Path(staged).read_bytes() == Path(src).read_bytes()
        assert dst.read_bytes() == b"previous"
        raise OSError("publish interrupted")
    monkeypatch.setattr(dp, "replace_file", refuse)
    with pytest.raises(OSError, match="publish interrupted"):
        _deploy(src, target, rel, cache={})
    assert dst.read_bytes() == b"previous"
    assert set(dst.parent.iterdir()) == {dst}


def test_partial_copy_keeps_previous_payload(tmp_path, monkeypatch):
    src, target, rel = _setup(tmp_path, monkeypatch)
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    dst = Path(target) / rel
    dst.parent.mkdir()
    dst.write_bytes(b"previous")
    def interrupted(source, destination):
        Path(destination).write_bytes(b"partial")
        raise OSError("disk full")
    monkeypatch.setattr(dp.shutil, "copyfile", interrupted)
    with pytest.raises(OSError, match="disk full"):
        _deploy(src, target, rel, cache={})
    assert dst.read_bytes() == b"previous"
    assert set(dst.parent.iterdir()) == {dst}


def test_failed_manifest_serialization_keeps_previous_manifest(tmp_path):
    path = tmp_path / dp.MANIFEST_NAME
    path.write_bytes(b"previous")
    with pytest.raises(IndexError):
        dp.write_manifest(tmp_path, {"invalid": ()}, "abc")
    assert path.read_bytes() == b"previous"
    assert set(tmp_path.iterdir()) == {path}


def test_concurrent_reuse_receipts_have_private_temps(recorded_payload, monkeypatch):
    target, inputs, output, source = recorded_payload
    hashes = {output.relative_to(target).as_posix(): (dp.sha256_file(output), 3)}
    items = [(str(source), next(iter(hashes)), "kit")]
    replace = dp.os.replace
    pending = []
    def overlapping(src, dst):
        pending.append(str(src))
        if len(pending) == 1:
            dp.record_deploy_inputs(target, inputs, hashes, items)
        replace(src, dst)
    monkeypatch.setattr(dp.os, "replace", overlapping)
    dp.record_deploy_inputs(target, inputs, hashes, items)
    assert len(set(pending)) == 2
    assert dp.reusable_payload("unused", target, inputs)[0]


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
    monkeypatch.setattr(dp, "sha256_file", lambda path: pytest.fail("unchanged bytes rehashed"))
    skipped = _deploy(src, target, rel, cache=cache)
    assert skipped[rel][:2] == cache[rel][:2], "skip must reuse the recorded sha256/size"
    assert target_file.stat().st_mtime_ns == mtime_ns


@pytest.mark.parametrize('restore_mtime', [False, True])
def test_same_size_corruption_invalidates_receipt(tmp_path, monkeypatch, restore_mtime):
    src, target, rel = _setup(tmp_path, monkeypatch)
    cache = _cache_after(src, target, rel, _deploy(src, target, rel, cache={}))
    installed = Path(target) / rel
    before = installed.stat()
    installed.write_bytes(b"x" * installed.stat().st_size)
    if restore_mtime:
        os.utime(installed, ns=(before.st_atime_ns, before.st_mtime_ns))
    _deploy(src, target, rel, cache=cache)
    assert installed.read_bytes() == Path(src).read_bytes()


def test_unchanged_exe_is_hashed_but_not_copied(tmp_path, monkeypatch):
    src, target, _ = _setup(tmp_path, monkeypatch)
    items = [(src, "mc2.exe", "exe")]
    cache = _cache_after(src, target, "mc2.exe", dp.deploy(items, target, False))
    monkeypatch.setattr(dp.shutil, "copyfile", lambda *a: pytest.fail("unchanged exe copied"))
    calls = []
    original = dp.sha256_file
    monkeypatch.setattr(dp, "sha256_file", lambda path: calls.append(path) or original(path))
    dp.deploy(items, target, False, cache=cache)
    assert src in calls and str(Path(target) / "mc2.exe") in calls


def test_normalized_sidecar_receipt_is_distinct_from_source_hash(tmp_path, monkeypatch):
    from kit_payload import HashReceipts
    src, target, _ = _setup(tmp_path, monkeypatch)
    Path(src).write_bytes(b'asset\r\n')
    hashes = HashReceipts(Path(target) / '.deploy-receipts.json')
    raw = hashes.digest(src)
    hashes.save()
    rel = 'data/asset.mcasset.json'
    _deploy(src, target, rel, cache={})
    assert (Path(target) / rel).read_bytes() == b'asset\n'
    assert raw != dp.sha256_file(str(Path(target) / rel))
    monkeypatch.setattr(dp.shutil, 'copyfile', lambda *a: pytest.fail('unchanged sidecar copied'))
    monkeypatch.setattr(Path, 'read_bytes', lambda *a: pytest.fail('unchanged sidecar rehashed'))
    _deploy(src, target, rel, cache={})


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
    args=types.SimpleNamespace(allow_stale_exe=False,build64_root=None,require_clean=True,classic_assets=True,
                              full_verify=False,allow_stale_pdb=False,preserve=[])
    with pytest.raises(SystemExit):
        dp.run_deploy(args,str(tmp_path),str(build),str(target),"mc2.exe","mc2.pdb")
