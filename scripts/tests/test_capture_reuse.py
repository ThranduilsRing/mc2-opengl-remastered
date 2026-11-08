"""capture-before reuse: a hit only on the complete reference identity with artifacts that still verify."""
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_reuse  # noqa: E402


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
