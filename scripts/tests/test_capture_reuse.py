"""capture-before reuse: a hit only on the complete reference identity with artifacts that still verify."""
import hashlib
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_reuse  # noqa: E402


@pytest.fixture(autouse=True)
def _reuse_on(monkeypatch):
    monkeypatch.setenv(capture_reuse.ENABLE_ENV, "1")


def _world(tmp_path):
    root = tmp_path / "repo"
    for h in capture_reuse.HARNESS:
        (root / h).parent.mkdir(parents=True, exist_ok=True)
        (root / h).write_text("harness")
    (root / "tests").mkdir()
    (root / "tests" / "pose_a.json").write_text("{}")
    install = tmp_path / "install"
    install.mkdir()
    (install / "mc2.exe").write_bytes(b"exe-v1")
    (install / "shaders").mkdir()
    (install / "shaders" / "terrain.frag.spv").write_bytes(b"spv-v1")
    (install / ".deployed_manifest.csv").write_text("# h\nrelpath,sha256,bytes\nmc2.exe,aa,6\n")
    out = tmp_path / "out"
    cmd = ["py", "capture.py", "--install", str(install), "--bookmarks", "pose_a", "--out", str(out)]
    return root, install, out, cmd


def _capture(out):
    pose = out / "pose_a"
    pose.mkdir(parents=True)
    png = pose / "primary.png"
    png.write_bytes(b"png-bytes")
    (out / "capture_run.json").write_text(json.dumps({
        "n_requested": 1, "n_captured": 1,
        "poses": [{"bookmark": "pose_a", "pose_dir": str(pose), "primary_png": str(png),
                   "primary_sha256": hashlib.sha256(b"png-bytes").hexdigest()}]}))


def test_round_trip_hits_and_the_restored_set_verifies_in_its_own_place(tmp_path):
    root, _install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    assert capture_reuse.restore(cmd, root, cache, "gpuA") is None   # nothing stored yet
    _capture(out)
    assert capture_reuse.store(cmd, root, cache, "gpuA")
    import shutil
    shutil.rmtree(out)
    note = capture_reuse.restore(cmd, root, cache, "gpuA")
    assert note and "reused" in note
    assert capture_reuse.verify_artifacts(out) is None
    assert json.loads((out / "capture_run.json").read_text())["reused_from_identity"]


def test_gate_planning_is_read_only_and_execution_restores_previous_after(tmp_path, monkeypatch):
    import merge_gate as gate
    import functools
    import os
    root, _install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    _capture(out)
    monkeypatch.setattr(capture_reuse, "gpu_identity", lambda: "gpuA")
    assert capture_reuse.store(cmd, root, cache, env=dict(os.environ, MC2_IN_GATE="1",
                              MC2_GATE_REASON="--declared-view", MC2_GATE_TIER="0",
                              MC2_SMOKE_ARTIFACT_ROOT=str(tmp_path / "old-window")))
    before = tmp_path / "before"
    before.mkdir()
    (before / "keep.txt").write_text("existing proof")
    reference = [str(before) if arg == str(out) else arg for arg in cmd]
    scheduled = [("capture-before", reference, {}), ("capture-after", cmd, {}),
                 ("compare-captures", ["compare", "--compare-only", str(before), str(out)], {})]
    monkeypatch.setattr(gate, "ROOT", root)
    monkeypatch.setattr(gate, "gate_env", lambda *a: {"MC2_IN_GATE": "1"})
    monkeypatch.setattr(capture_reuse, "available", functools.partial(capture_reuse.available, cache=cache))
    monkeypatch.setattr(capture_reuse, "restore", functools.partial(capture_reuse.restore, cache=cache))
    snapshot = lambda: {str(p): p.read_bytes() for p in tmp_path.rglob("*") if p.is_file()}
    original = snapshot()
    steps = gate.merge_visuals(scheduled)
    assert [n for n, _, _ in steps] == ["capture-after", "compare-captures"]
    gate.execute_window(steps, "head", dry_run=True)
    assert snapshot() == original
    monkeypatch.setattr(gate, "execute_parallel", lambda *a, **k: 0)
    assert gate.execute_window(steps, "head") == 0
    assert capture_reuse.verify_artifacts(before) is None
    assert not (before / "keep.txt").exists()


def test_any_identity_change_is_a_miss(tmp_path):
    root, install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    _capture(out)
    capture_reuse.store(cmd, root, cache, "gpuA")
    assert capture_reuse.restore(cmd, root, cache, "gpuB") is None   # other GPU/driver
    (install / "mc2.exe").write_bytes(b"exe-v2")
    assert capture_reuse.restore(cmd, root, cache, "gpuA") is None   # other exe
    (install / "mc2.exe").write_bytes(b"exe-v1")
    (root / "tests" / "pose_a.json").write_text('{"x":1}')
    assert capture_reuse.restore(cmd, root, cache, "gpuA") is None   # other pose
    (root / "tests" / "pose_a.json").write_text("{}")
    assert capture_reuse.restore(cmd + ["--extra"], root, cache, "gpuA") is None   # other arguments
    assert capture_reuse.restore(cmd, root, cache, None if False else "") is None   # no GPU answer: no reuse


def test_a_tampered_or_incomplete_stored_set_is_never_reused(tmp_path):
    root, _install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    _capture(out)
    capture_reuse.store(cmd, root, cache, "gpuA")
    stored = next(p for p in cache.iterdir() if p.is_dir())
    (stored / "pose_a" / "primary.png").write_bytes(b"tampered")
    assert capture_reuse.restore(cmd, root, cache, "gpuA") is None
    (stored / "pose_a" / "primary.png").write_bytes(b"png-bytes")
    run = json.loads((stored / "capture_run.json").read_text())
    run["n_captured"] = 0
    (stored / "capture_run.json").write_text(json.dumps(run))
    assert capture_reuse.restore(cmd, root, cache, "gpuA") is None


def test_a_changed_shader_with_an_unchanged_exe_and_manifest_is_a_miss(tmp_path):
    # p31: the staged manifest has its shader rows removed, so only the real shader bytes can tell the sets apart.
    root, install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    _capture(out)
    assert capture_reuse.store(cmd, root, cache, "gpuA")
    (install / "shaders" / "terrain.frag.spv").write_bytes(b"spv-v2")
    assert capture_reuse.restore(cmd, root, cache, "gpuA") is None
    (install / "shaders" / "terrain.frag.spv").write_bytes(b"spv-v1")
    assert capture_reuse.restore(cmd, root, cache, "gpuA")


def test_an_install_without_shader_files_cannot_be_identified(tmp_path):
    root, install, out, cmd = _world(tmp_path)
    (install / "shaders" / "terrain.frag.spv").unlink()
    _capture(out)
    assert not capture_reuse.store(cmd, root, tmp_path / "cache", "gpuA")


def test_reuse_is_enabled_by_default_with_kill_switch(tmp_path, monkeypatch):
    root, _install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    _capture(out)
    assert capture_reuse.store(cmd, root, cache, "gpuA")
    monkeypatch.delenv(capture_reuse.ENABLE_ENV)
    assert capture_reuse.enabled()
    assert capture_reuse.restore(cmd, root, cache, "gpuA")
    monkeypatch.setenv(capture_reuse.ENABLE_ENV, "0")
    assert not capture_reuse.enabled()
    assert capture_reuse.restore(cmd, root, cache, "gpuA") is None
    assert not capture_reuse.store(cmd, root, cache, "gpuA")


def test_effective_device_and_capture_helpers_invalidate_reuse(tmp_path):
    root, _install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    _capture(out)
    assert capture_reuse.store(cmd, root, cache, "gpuA", env={"MC2_VK_DEVICE": "A"})
    assert capture_reuse.restore(cmd, root, cache, "gpuA", env={"MC2_VK_DEVICE": "A"})
    assert capture_reuse.restore(cmd, root, cache, "gpuA", env={"MC2_VK_DEVICE": "B"}) is None
    helper = root / "scripts/smoke_lib/helper.py"
    helper.parent.mkdir(parents=True)
    helper.write_text("changed")
    assert capture_reuse.restore(cmd, root, cache, "gpuA", env={"MC2_VK_DEVICE": "A"}) is None
