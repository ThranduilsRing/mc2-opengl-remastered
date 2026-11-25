"""Shader iteration retains binary provenance and never legitimizes unrelated drift."""
import csv
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy_payload as dp
import manifest_schema as ms
from scripts.smoke_lib import fingerprint


@pytest.fixture
def lane(tmp_path, monkeypatch):
    build, install = tmp_path / "build", tmp_path / "install"
    for root in (build, install):
        (root / "shaders/vulkan").mkdir(parents=True)
        (root / "shaders/vulkan/cloud.frag.spv").write_bytes(b"old-spv")
    (build / "build.ninja").write_text("# fixture\n")
    (install / "mc2.exe").write_bytes(b"unchanged-binary")
    (install / "data.fit").write_bytes(b"old-data")
    names = ("mc2.exe", "data.fit", "shaders/vulkan/cloud.frag.spv")
    dp.write_manifest(install, {n: (*dp.hash_with_stamp(install / n, dp.sha256_file)[:2],
                                  None, (install / n).stat().st_mtime_ns) for n in names}, "a" * 40)
    monkeypatch.setattr(dp, "git_head", lambda _: "b" * 40)
    run = dp.subprocess.run
    def compile_shader(argv, **kwargs):
        if argv[-1] != "mc2_vulkan_shaders":
            return run(argv, **kwargs)
        assert kwargs["env"]["MC2_NINJA_TREE"] == "build"
        (build / "shaders/vulkan/cloud.frag.spv").write_bytes(b"new-spv")
    monkeypatch.setattr(dp.subprocess, "run", compile_shader)
    return tmp_path, build, install


def manifest_rows(install):
    with (install / dp.MANIFEST_NAME).open(newline="") as f:
        return list(csv.reader(f))


def test_shader_push_preserves_other_rows_and_tags_capture(lane):
    root, build, install = lane
    before = manifest_rows(install)
    (install / "data.fit").write_bytes(b"drift-must-still-fail")
    dp.deploy_shaders_only(root, build, install, "mc2.exe")
    after = manifest_rows(install)
    assert before[:4] == after[:4]  # headers, data and executable provenance/stamps
    assert dp.read_manifest(install)["data.fit"][0] != dp.sha256_file(install / "data.fit")
    assert (install / "mc2.exe").read_bytes() == b"unchanged-binary"
    shader = "shaders/vulkan/cloud.frag.spv"
    assert dp.read_manifest(install)[shader][0] == dp.sha256_file(install / shader)
    dirty = ms.shader_dirty(install / "mc2.exe")
    assert dirty == {"state": "shader-dirty", "shaders": {shader: dp.sha256_file(install / shader)}}
    exe = ms.exe_block(install / "mc2.exe")
    assert exe["shader_dirty"] == dirty
    fp = {"sha": "a" * 12, "dirty": 0}
    lines, bad = fingerprint.check_fingerprint(fp, "a" * 40, exe_path=install / "mc2.exe")
    assert not bad and "shader-dirty" in lines[0]
    _, bad = fingerprint.check_fingerprint(fp, None, exe_path=install / "mc2.exe",
                                         reference={"mode": "commit", "sha": "a" * 40})
    assert bad
    committed = dict(exe, embedded_fingerprint={"present": True, **fp})
    assert ms.binary_provenance(committed, {"commit": "a" * 40})["verdict"] == "shader-dirty"
    saved = (install / dp.MANIFEST_NAME).read_bytes()
    dp.deploy_shaders_only(root, build, install, "mc2.exe")
    assert (install / dp.MANIFEST_NAME).read_bytes() == saved
    dp.write_manifest(install, dp.read_manifest(install), "b" * 40)
    assert ms.shader_dirty(install / "mc2.exe") is None


@pytest.mark.parametrize("failure", ["compile", "exe", "shader", "inventory"])
def test_failure_never_rewrites_manifest(lane, monkeypatch, failure):
    root, build, install = lane
    if failure == "compile":
        def broken(*args, **kwargs):
            raise subprocess.CalledProcessError(1, args[0])
        monkeypatch.setattr(dp.subprocess, "run", broken)
    elif failure == "inventory":
        (build / "shaders/vulkan/extra.frag.spv").write_bytes(b"extra")
    else:
        path = install / ("mc2.exe" if failure == "exe" else "shaders/vulkan/cloud.frag.spv")
        path.write_bytes(b"tampered")
    before = (install / dp.MANIFEST_NAME).read_bytes()
    with pytest.raises((SystemExit, subprocess.CalledProcessError)):
        dp.deploy_shaders_only(root, build, install, "mc2.exe")
    assert (install / dp.MANIFEST_NAME).read_bytes() == before
