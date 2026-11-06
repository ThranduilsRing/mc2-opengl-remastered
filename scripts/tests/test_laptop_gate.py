"""laptop_gate: payload delta pack and deploy_payload --from-payload sha256 checks."""
import csv
import hashlib
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import laptop_gate  # noqa: E402
import deploy_payload  # noqa: E402


def _manifest(path, files):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["manifest_version", "v1"])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp", "mtime_ns"])
        for rel, data in files.items():
            w.writerow([rel, hashlib.sha256(data).hexdigest(), len(data), "abc", "t", 0])


def test_pack_ships_only_files_that_differ(tmp_path, monkeypatch):
    install, gate = tmp_path / "install", tmp_path / "gate"
    (install / "shaders").mkdir(parents=True)
    files = {"mc2.exe": b"new", "shaders/a.glsl": b"same", "shaders/b.glsl": b"changed"}
    for rel, data in files.items():
        (install / rel).write_bytes(data)
    _manifest(install / ".deployed_manifest.csv", files)
    (gate / "in" / "7").mkdir(parents=True)
    (gate / "out" / "7").mkdir(parents=True)
    _manifest(gate / "in" / "7" / "pc.manifest.csv", {"mc2.exe": b"old", "shaders/a.glsl": b"same", "shaders/b.glsl": b"old"})
    monkeypatch.setattr(laptop_gate, "G", gate.as_posix())
    monkeypatch.setattr(laptop_gate, "INSTALL", install.as_posix())
    laptop_gate.r_pack("7")
    import tarfile
    names = sorted(tarfile.open(gate / "out" / "7" / "payload.tar").getnames())
    assert names == [".deployed_manifest.csv", "files/mc2.exe", "files/shaders/b.glsl"]
    assert "2 files" in (gate / "out" / "7" / "payload.sha256").read_text()


def test_from_payload_rejects_a_tampered_file(tmp_path):
    payload, target = tmp_path / "payload", tmp_path / "target"
    (payload / "files").mkdir(parents=True)
    target.mkdir()
    _manifest(payload / ".deployed_manifest.csv", {"mc2.exe": b"good"})
    (payload / "files" / "mc2.exe").write_bytes(b"evil")
    with pytest.raises(SystemExit):
        deploy_payload.deploy_from_payload(payload, target, tmp_path, "0" * 40)
    assert not (target / "mc2.exe").exists()


def test_from_payload_rejects_drifted_unchanged_file(tmp_path):
    payload, target = tmp_path / "payload", tmp_path / "target"
    (payload / "files").mkdir(parents=True)
    target.mkdir()
    _manifest(payload / ".deployed_manifest.csv", {"shaders/a.glsl": b"expected"})
    (target / "shaders").mkdir()
    (target / "shaders" / "a.glsl").write_bytes(b"x")
    with pytest.raises(SystemExit):
        deploy_payload.deploy_from_payload(payload, target, tmp_path, "0" * 40)
