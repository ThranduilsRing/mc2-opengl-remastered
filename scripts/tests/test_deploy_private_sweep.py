"""Trigger: deploy_payload.py; temp-shaped names never establish ownership."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy_payload as dp
import exe_fingerprint


def test_published_deploy_preserves_preexisting_temps_and_owner_files(tmp_path, monkeypatch):
    payload, target = tmp_path / "payload", tmp_path / "target"
    (payload / "files").mkdir(parents=True)
    target.mkdir()
    (payload / "files/mc2.exe").write_bytes(b"exe")
    dp.write_manifest(payload, {"mc2.exe": (dp.sha256_file(payload / "files/mc2.exe"), 3, None)}, "test")
    probes = ("mc2.exe.private-99999999", ".deployed_manifest.csv.private-99999999", "owner.private",
              "mods/custom/model.private-12345", "savegame/owner.fit", "crashes/owner.dmp")
    for rel in probes:
        path = target / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"owner bytes")
    monkeypatch.setattr(exe_fingerprint, "extract", lambda _: {"present": True, "sha": "test"})
    monkeypatch.setattr(exe_fingerprint, "build_current", lambda *a: True)
    dp.deploy_from_payload(payload, target, tmp_path, "test")
    assert (target / "mc2.exe").read_bytes() == b"exe"
    assert all((target / rel).read_bytes() == b"owner bytes" for rel in probes)
