"""Trigger: deploy_payload.py; temp-shaped names never establish ownership."""
import sys
import os
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy_payload as dp
import exe_fingerprint
import kit_payload
import pytest


@pytest.mark.parametrize("collision", ["mc2.exe.payload-tmp", ".deployed_manifest.csv.payload-tmp"])
def test_published_deploy_preserves_preexisting_temps_and_owner_files(tmp_path, monkeypatch, collision):
    payload, target = tmp_path / "payload", tmp_path / "target"
    (payload / "files").mkdir(parents=True)
    target.mkdir()
    (payload / "files/mc2.exe").write_bytes(b"exe")
    dp.write_manifest(payload, {"mc2.exe": (dp.sha256_file(payload / "files/mc2.exe"), 3, None)}, "test")
    probes = (collision, "mc2.exe.private-99999999", ".deployed_manifest.csv.private-99999999", "owner.private",
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


def test_seed_delta_preserves_own_pid_temp(tmp_path, monkeypatch):
    source, target = tmp_path / "source", tmp_path / "target"
    source.write_bytes(b"shared")
    target.mkdir()
    dst = target / "asset.bin"
    os.link(source, dst)
    probe = target / f"asset.bin.private-{os.getpid()}"
    probe.write_bytes(b"owner bytes")
    monkeypatch.setattr(kit_payload, "cook_cache_root", lambda: tmp_path / "cache")
    monkeypatch.setattr(dp, "seed_receipt_rows", lambda: {})
    dp.prepare_seed_delta([(str(source), dst.name, "support")], target, full_verify=True, allow_legacy=True)
    assert probe.read_bytes() == b"owner bytes"
    assert dst.read_bytes() == source.read_bytes() == b"shared" and not os.path.samefile(source, dst)
