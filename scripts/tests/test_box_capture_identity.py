"""Linux deploy/capture identity: real CSV hashes and fail-closed attribution."""
import csv
import hashlib
import json
import sys
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
sys.path.insert(0, str(ROOT / "scripts"))
import ab
import box_run
import run_visual_capture as rvc

COMMIT = "a" * 40


def install_payload(path):
    path.mkdir()
    (path / "mc2").write_bytes(
        f"MC2FP1|{COMMIT}|0|test|2026-11-09T12:00:00|/src|FP1END".encode())
    shader = path / "shaders/vulkan/test.spv"
    shader.parent.mkdir(parents=True)
    shader.write_bytes(b"spirv")
    data = path / "data/a.fit"
    data.parent.mkdir()
    data.write_bytes(b"data")
    (path / "stock.fst").write_bytes(b"archive")
    # A Windows seed's CSV must be replaced, never merged into this identity.
    (path / ".deployed_manifest.csv").write_text("mc2.exe,stale,1,wrong\n")
    box_run.write_payload_manifest(path, COMMIT)
    return path


def test_manifest_uses_production_format_and_installed_bytes(tmp_path):
    install = install_payload(tmp_path / "install")
    with (install / ab.MANIFEST).open(newline="") as stream:
        rows = list(csv.reader(stream))
    assert rows[0] == ["manifest_version", "v1", "", "", "", ""]
    assert rows[1] == ["relpath", "sha256", "bytes", "src_commit", "timestamp", "mtime_ns"]
    assert {r[0] for r in rows[2:]} == {"mc2", "shaders/vulkan/test.spv", "data/a.fit", "stock.fst"}
    for rel, sha, size, commit, timestamp, mtime in rows[2:]:
        path = install / rel
        assert sha == hashlib.sha256(path.read_bytes()).hexdigest()
        assert int(size) == path.stat().st_size
        assert commit == COMMIT and timestamp and int(mtime) == path.stat().st_mtime_ns


def test_writer_refuses_wrong_commit_without_replacing_manifest(tmp_path):
    install = install_payload(tmp_path / "install")
    before = (install / ab.MANIFEST).read_bytes()
    with pytest.raises(ValueError, match="commit"):
        box_run.write_payload_manifest(install, "b" * 40)
    assert (install / ab.MANIFEST).read_bytes() == before


@pytest.mark.parametrize("exe", ["mc2", "mc2.elf", "mc2.exe"])
def test_platform_payload_stamp_and_compare(tmp_path, monkeypatch, exe):
    install = install_payload(tmp_path / "install")
    if exe != "mc2":
        (install / "mc2").rename(install / exe)
        manifest = install / ab.MANIFEST
        manifest.write_text(manifest.read_text().replace("mc2,", exe + ","))
    identity = ab.payload_identity(install)
    monkeypatch.setattr(ab, "git_ok", lambda *args: True)
    for arm in ("a", "b"):
        directory = tmp_path / arm
        (directory / "day").mkdir(parents=True)
        Image.new("RGB", (32, 32)).save(directory / "day/day.png")
        ab.stamp_identity(directory, identity)
        assert ab.captured_identity(directory) == identity
    with pytest.raises(SystemExit) as verdict:
        ab.main(["--compare-only", str(tmp_path / "a"), str(tmp_path / "b"),
                 "--out-root", str(tmp_path / "compare")])
    assert verdict.value.code == 0
    assert (tmp_path / "compare/day/montage.jpg").is_file()


@pytest.mark.parametrize("fault", ["exe", "shader", "commit", "extra_shader", "ambiguous"])
def test_linux_payload_refuses_drift(tmp_path, fault):
    install = install_payload(tmp_path / "install")
    if fault == "commit":
        path = install / ab.MANIFEST
        path.write_text(path.read_text().replace(COMMIT, "b" * 40))
    else:
        rel = {"exe": "mc2", "shader": "shaders/vulkan/test.spv",
               "extra_shader": "shaders/vulkan/extra.spv", "ambiguous": "mc2.exe"}[fault]
        (install / rel).write_bytes(b"drift")
        if fault == "ambiguous":
            with (install / ab.MANIFEST).open("a") as stream:
                stream.write(f"mc2.exe,{'0' * 64},5,{COMMIT},t,0\n")
    with pytest.raises(ValueError):
        ab.payload_identity(install)


def test_visual_capture_stamps_and_preserves_sweep_metadata(tmp_path):
    install = install_payload(tmp_path / "install")
    out = tmp_path / "out"
    out.mkdir()
    (out / "capture_run.json").write_text(json.dumps({"poses": [{"bookmark": "day"}]}))
    result = dict(out_dir=str(out), mission="mc2_01", bookmarks_path="day.json",
                  all_present=True, engine_capture_fired=True, any_nondeterministic=False,
                  bookmarks=[], install_identity=ab.payload_identity(install))
    rvc.write_manifest(result, install / "mc2")
    assert ab.captured_identity(out) == result["install_identity"]
    assert json.loads((out / "capture_run.json").read_text())["poses"] == [{"bookmark": "day"}]
    sidecar = out / "capture_run.json"
    metadata = json.loads(sidecar.read_text())
    metadata["install_identity"]["commit"] = "b" * 40
    sidecar.write_text(json.dumps(metadata))
    with pytest.raises(ValueError, match="commit mismatches"):
        ab.captured_identity(out)
    (install / "shaders/vulkan/test.spv").write_bytes(b"drift")
    with pytest.raises(ValueError, match="manifest mismatch"):
        rvc.write_manifest(result, install / "mc2")


def test_box_ab_releases_gpu_before_rebuild_and_checks_deploy():
    remote = box_run.REMOTE
    before = remote.index('> "$OUT/ab_before.log"')
    release = remote.index('exec 9>&-', before)
    rebuild = remote.index('BUILD_SHA=$SHA; build_install', release)
    after = remote.index('> "$OUT/ab_after.log"', rebuild)
    assert before < release < rebuild < after
    assert 'DEPLOY FAILED: manifest' in remote
