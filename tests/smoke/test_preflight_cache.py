# tests/smoke/test_preflight_cache.py
"""SMOKE-LATENCY-WINS-1 win #4: preflight result cache.

Verifies scripts/smoke_lib/preflight_cache.py caches/invalidates on the
right signals -- HEAD sha, deploy manifest mtime/size, and shader tree
file-count/max-mtime -- without reimplementing any hashing.
"""
import json
import subprocess
import time
from pathlib import Path

import pytest

from scripts.smoke_lib import preflight_cache as pc


def _git_init_with_commit(root: Path) -> str:
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.email", "t@example.com"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.name", "test"], cwd=root, check=True)
    (root / "README.txt").write_text("x", encoding="utf-8")
    subprocess.run(["git", "add", "."], cwd=root, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "init"], cwd=root, check=True)
    out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


@pytest.fixture
def worktree(tmp_path):
    root = tmp_path / "worktree"
    root.mkdir()
    (root / "shaders").mkdir()
    _git_init_with_commit(root)
    return root


@pytest.fixture
def deploy_dir(tmp_path):
    d = tmp_path / "deploy"
    d.mkdir()
    return d


def _write_manifest(deploy_dir: Path, content: str = "manifest_version,1\n"):
    (deploy_dir / ".deployed_manifest.csv").write_text(content, encoding="utf-8")


def test_no_manifest_yields_no_fingerprint(worktree, deploy_dir):
    # No manifest present -> compute_fingerprint returns None (uncacheable).
    assert pc.compute_fingerprint(deploy_dir, worktree) is None


def test_fingerprint_stable_when_nothing_changes(worktree, deploy_dir):
    _write_manifest(deploy_dir)
    fp1 = pc.compute_fingerprint(deploy_dir, worktree)
    fp2 = pc.compute_fingerprint(deploy_dir, worktree)
    assert fp1 is not None
    assert fp1 == fp2


def test_cache_hit_replays_results(worktree, deploy_dir, tmp_path):
    _write_manifest(deploy_dir)
    fp = pc.compute_fingerprint(deploy_dir, worktree)
    cache_path = tmp_path / ".preflight_cache.json"
    results = {"coherence": {"text": "OK"}, "bom": {"text": "OK", "returncode": 0}}
    pc.save_cache(cache_path, fp, results)

    hit = pc.check_cache_hit(cache_path, fp)
    assert hit == results


def test_cache_miss_on_head_change(worktree, deploy_dir, tmp_path):
    _write_manifest(deploy_dir)
    fp = pc.compute_fingerprint(deploy_dir, worktree)
    cache_path = tmp_path / ".preflight_cache.json"
    pc.save_cache(cache_path, fp, {"coherence": {"text": "OK"}})

    # New commit changes HEAD -> fingerprint changes -> cache miss.
    (worktree / "file2.txt").write_text("y", encoding="utf-8")
    subprocess.run(["git", "add", "."], cwd=worktree, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "second"], cwd=worktree, check=True)

    fp2 = pc.compute_fingerprint(deploy_dir, worktree)
    assert fp2 != fp
    assert pc.check_cache_hit(cache_path, fp2) is None


def test_cache_miss_on_manifest_change(worktree, deploy_dir, tmp_path):
    _write_manifest(deploy_dir)
    fp = pc.compute_fingerprint(deploy_dir, worktree)
    cache_path = tmp_path / ".preflight_cache.json"
    pc.save_cache(cache_path, fp, {"coherence": {"text": "OK"}})

    # Manifest re-deployed (mtime/size change) -> fingerprint changes -> miss.
    time.sleep(0.05)
    _write_manifest(deploy_dir, content="manifest_version,1\nextra,row\n")

    fp2 = pc.compute_fingerprint(deploy_dir, worktree)
    assert fp2 != fp
    assert pc.check_cache_hit(cache_path, fp2) is None


def test_cache_miss_on_shader_tree_change(worktree, deploy_dir, tmp_path):
    _write_manifest(deploy_dir)
    fp = pc.compute_fingerprint(deploy_dir, worktree)
    cache_path = tmp_path / ".preflight_cache.json"
    pc.save_cache(cache_path, fp, {"bom": {"text": "OK", "returncode": 0}})

    # A new shader file appears (e.g. --fix ran, or a shader was edited) ->
    # shaders_fingerprint's file count changes -> cache miss.
    (worktree / "shaders" / "new.frag").write_text("void main(){}", encoding="utf-8")

    fp2 = pc.compute_fingerprint(deploy_dir, worktree)
    assert fp2 != fp
    assert pc.check_cache_hit(cache_path, fp2) is None


def test_check_cache_hit_returns_none_for_missing_file(tmp_path):
    cache_path = tmp_path / "nonexistent.json"
    assert pc.check_cache_hit(cache_path, {"anything": 1}) is None


def test_check_cache_hit_returns_none_for_corrupt_json(tmp_path):
    cache_path = tmp_path / ".preflight_cache.json"
    cache_path.write_text("{not valid json", encoding="utf-8")
    assert pc.check_cache_hit(cache_path, {"anything": 1}) is None


def test_save_cache_is_atomic_no_leftover_tmp(worktree, deploy_dir, tmp_path):
    _write_manifest(deploy_dir)
    fp = pc.compute_fingerprint(deploy_dir, worktree)
    cache_path = tmp_path / ".preflight_cache.json"
    pc.save_cache(cache_path, fp, {"a": 1})
    assert cache_path.is_file()
    assert not cache_path.with_suffix(".json.tmp").exists()
    data = json.loads(cache_path.read_text(encoding="utf-8"))
    assert data["fingerprint"] == fp
    assert data["results"] == {"a": 1}


def test_bom_fail_result_cached_but_invalidates_after_fix(worktree, deploy_dir, tmp_path):
    """A cached BOM-FAIL must not silently mask a still-present defect: once
    the offending file is fixed (which changes its mtime, and here the file
    is removed which changes shader file count), the fingerprint changes and
    the stale FAIL result is never replayed for the now-clean tree."""
    _write_manifest(deploy_dir)
    bad = worktree / "shaders" / "bad.frag"
    bad.write_text("﻿void main(){}", encoding="utf-8")
    fp_bad = pc.compute_fingerprint(deploy_dir, worktree)
    cache_path = tmp_path / ".preflight_cache.json"
    pc.save_cache(cache_path, fp_bad, {"bom": {"text": "FAIL", "returncode": 1}})

    hit = pc.check_cache_hit(cache_path, fp_bad)
    assert hit is not None and hit["bom"]["returncode"] == 1

    # Simulate `--fix`: BOM stripped in place (file still exists, mtime changes).
    time.sleep(0.05)
    bad.write_text("void main(){}", encoding="utf-8")
    fp_fixed = pc.compute_fingerprint(deploy_dir, worktree)
    assert fp_fixed != fp_bad
    assert pc.check_cache_hit(cache_path, fp_fixed) is None
