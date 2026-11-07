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


@pytest.mark.parametrize("sm,rc", [({"verdict": "PASS"}, 0), ({"verdict": "FAIL"}, 1), ({}, 1), ({"gate_rc": 1}, 1)])
def test_driver_exits_zero_only_on_pass(sm, rc):
    assert laptop_gate.exit_code(sm) == rc


def test_ref_sums_cover_both_pinned_kits_and_refcheck_flags_a_bad_file(tmp_path, monkeypatch):
    text = laptop_gate.ref_sums(ROOT)
    rels = [line.split("  ", 1)[1] for line in text.splitlines()]
    assert any(r.startswith("packages/mc2-industrial-kit/") for r in rels)
    assert any(r.startswith("packages/mc2-vehicle-kit/") for r in rels)
    assert "scifi-kit/mapping/inventory.csv" in rels
    monkeypatch.setattr(laptop_gate, "REF", str(tmp_path))
    (tmp_path / "a.txt").write_bytes(b"x")
    good = hashlib.sha256(b"x").hexdigest()
    (tmp_path / "S").write_text(f"{good}  a.txt\n{good}  b.txt\n", encoding="utf-8")
    assert laptop_gate.r_refcheck(tmp_path / "S") == ["ref mismatch: b.txt"]
    assert laptop_gate.r_refcheck(tmp_path / "none") == [f"missing {tmp_path / 'none'}"]


def test_toolchain_problems_names_each_missing_tool_and_skips_sccache_when_the_cache_is_off(tmp_path, monkeypatch):
    present = tmp_path / "vcvars64.bat"
    present.write_text("")

    def no_cmake():
        raise SystemExit(1)
    cn = type("cn", (), {"VCVARS": str(present), "NINJA": str(tmp_path / "ninja.exe"),
                         "SCCACHE_EXE": str(tmp_path / "sccache.exe"), "find_cmake": staticmethod(no_cmake)})
    monkeypatch.setattr(laptop_gate, "JOBS", str(tmp_path / "JOBS"))
    off = laptop_gate.toolchain_problems(cn, {"MC2_COMPILE_CACHE": "0"})
    assert off == [f"missing NINJA: {tmp_path / 'ninja.exe'}", f"missing JOBS file: {tmp_path / 'JOBS'}",
                   "missing cmake (find_cmake: VS2022 BuildTools or PATH)"]
    assert f"missing SCCACHE_EXE: {tmp_path / 'sccache.exe'}" in laptop_gate.toolchain_problems(cn, {})
    assert laptop_gate.gate_env()["MC2_COMPILE_CACHE"] == "0"


def test_published_problems_fails_a_stale_or_unrelated_before_install():
    history = {("2d042f852b04", "claude/stage-g125"), ("c0eba1a0ac1c", "claude/stage-g125")}
    anc = lambda a, b: (a, b) in history  # noqa: E731
    assert laptop_gate.published_problems("2d042f852b04", "2d042f852b04", "claude/stage-g125", anc) == []
    stale = laptop_gate.published_problems("c0eba1a0ac1c", "2d042f852b04", "claude/stage-g125", anc)
    assert len(stale) == 1 and "!= the PC's published v0.4 2d042f852b04" in stale[0]
    assert "not an ancestor" in laptop_gate.published_problems("deadbeef0000", "", "claude/stage-g125", anc)[0]
    assert laptop_gate.published_problems(None, "x", None, anc) == ["published install mc2.exe has no MC2FP1 fingerprint"]
    assert laptop_gate.manifest_delta({"a": {"sha256": "1"}, "b": {"sha256": "2"}}, {"a": {"sha256": "1"}}) == ["b"]
