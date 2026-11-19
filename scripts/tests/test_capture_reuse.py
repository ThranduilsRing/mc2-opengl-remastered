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


def _tree(path):
    return {str(p): (p.read_bytes() if p.is_file() else None) for p in sorted(path.rglob("*"))}


def test_previous_after_set_is_found_through_the_real_gate_plan(tmp_path, monkeypatch):
    """Window N stores capture-after from execute(); window N+1's main() plan must find it. Nothing here builds a
    capture command by hand: both rows come out of main() -> plan() -> affected_plan() -> install_plan()."""
    import functools
    import shutil
    import types
    from contextlib import nullcontext
    import merge_gate as gate
    from smoke_lib import host_lease
    root, candidate, _out, _cmd = _world(tmp_path)
    for pose in ("mc2_01_saved_205644", "mc2_10_saved_204523"):
        (root / "tests" / (pose + ".json")).write_text("{}")
    published = tmp_path / "published"
    cache = tmp_path / "cache"
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    monkeypatch.setattr(gate, "ROOT", root)
    monkeypatch.setattr(gate, "CANDIDATE", candidate)
    monkeypatch.setattr(gate, "PUBLISHED", published)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "gate_env", lambda *a: {"MC2_IN_GATE": "1"})
    monkeypatch.setattr(gate, "check_capture_commit", lambda *a: None)
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "game_batch", lambda: nullcontext({}))
    monkeypatch.setattr(host_lease, "gate_capture_lease", lambda *a: nullcontext())
    monkeypatch.setattr(capture_reuse, "gpu_identity", lambda: "gpuA")
    for fn in ("available", "restore", "store"):
        monkeypatch.setattr(capture_reuse, fn, functools.partial(getattr(capture_reuse, fn), cache=cache))
    real_execute = gate.execute

    def planned(proof):
        seen = []
        monkeypatch.setattr(gate, "execute", lambda steps, *a, **k: seen.extend(steps) or 0)
        assert gate.main(["--dry-run", "--base", "head", "--declared-view", "mc2_01_saved_205644",
                          "--proofs-dir", str(proof)]) == 0
        monkeypatch.setattr(gate, "execute", real_execute)
        return seen

    # Window N: no stored set yet, so the plan is after-only; its capture-after row runs through the real execute().
    first = planned(tmp_path / "proofN")
    assert "compare-captures" not in [n for n, _, _ in first]
    after = next(s for s in first if s[0] == "capture-after")
    assert "--env-declare" in after[1]   # the plan edit that split store from lookup before this fix

    def runner(command, cwd, env):
        _capture(Path(command[command.index("--out") + 1]))
        return types.SimpleNamespace(returncode=0, stdout="")
    assert real_execute([after], "head", runner=runner) == 0
    assert len(list(cache.iterdir())) == 1
    shutil.copytree(candidate, published)   # the window is published: v0.4 now runs what capture-after ran

    # Window N+1: the same planning path must now keep compare-captures, and a dry run must not write anything.
    before_plan = _tree(tmp_path)
    second = planned(tmp_path / "proofN1")
    assert [n for n, _, _ in second if "capture" in n] == ["capture-after", "compare-captures"]
    assert _tree(tmp_path) == before_plan
    compare = next(c for n, c, _ in second if n == "compare-captures")
    before = Path(compare[compare.index("--compare-only") + 1])
    assert before == tmp_path / "proofN1" / "before" and not before.exists()

    # Execution restores the set the plan found.
    monkeypatch.setattr(gate, "stage_published", lambda *a, **k: None)
    ran = []
    monkeypatch.setattr(gate, "execute_parallel", lambda steps, *a, **k: ran.extend(steps) or 0)
    assert gate.execute_window(second, "head", install=candidate) == 0
    assert capture_reuse.verify_artifacts(before) is None
    assert "compare-captures" in [n for n, _, _ in ran]
    assert not any("MC2_GATE_CAPTURE_REFERENCE" in e for _, _, e in ran)

    # A published install that no longer matches the stored run is after-only again.
    (published / "mc2.exe").write_bytes(b"exe-other")
    assert "compare-captures" not in [n for n, _, _ in planned(tmp_path / "proofN2")]


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


def test_lane_scratch_path_is_not_part_of_the_reuse_key(tmp_path, monkeypatch):
    """gate_env() hands every child a per-worktree MC2_LANE_SCRATCH; a set stored from one slot must still hit from another."""
    root, _install, out, cmd = _world(tmp_path)
    cache = tmp_path / "cache"
    monkeypatch.setenv("MC2_LANE_SCRATCH", str(tmp_path / "slotA" / "gate-scratch"))
    _capture(out)
    assert capture_reuse.store(cmd, root, cache, "gpuA")
    import shutil
    shutil.rmtree(out)
    monkeypatch.setenv("MC2_LANE_SCRATCH", str(tmp_path / "slotB" / "gate-scratch"))
    assert capture_reuse.restore(cmd, root, cache, "gpuA")
