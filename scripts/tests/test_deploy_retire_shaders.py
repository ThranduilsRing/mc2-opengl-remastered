"""W129: the build's output list owns shaders/ in a deployed install.

merge_gate.stage_published copied v0.4's shaders/ (with 20 dead .spv no CMake rule produces any more) into the candidate and
dropped their manifest rows, so deploy's manifest-driven retire loop never saw them and verify_published_identity failed
"published shader inventory != HEAD build" on the laptop gate. The PC shipped them because the deploy globbed the build's
shaders/vulkan directory, stale outputs included.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def _build(tmp_path, with_ninja=True):
    b64 = tmp_path / "build64-ninja"
    spv = b64 / "shaders" / "vulkan"
    spv.mkdir(parents=True)
    for name in ("live.frag.spv", "dead.frag.spv"):
        (spv / name).write_bytes(name.encode())
    if with_ninja:
        (b64 / "build.ninja").write_text("build shaders\\vulkan\\live.frag.spv: CUSTOM_COMMAND src\n")
    return b64


def test_stale_spv_in_build_dir_is_not_a_build_output(tmp_path):
    assert [p.name for p in dp.baked_spv_files(_build(tmp_path))] == ["live.frag.spv"]


def test_tree_without_build_ninja_falls_back_to_listing(tmp_path):
    names = [p.name for p in dp.baked_spv_files(_build(tmp_path, with_ninja=False))]
    assert names == ["dead.frag.spv", "live.frag.spv"]


def _target_with_unlisted_dead_spv(tmp_path):
    target = tmp_path / "install"
    (target / "shaders" / "vulkan").mkdir(parents=True)
    (target / "shaders" / "vulkan" / "dead.frag.spv").write_bytes(b"old")  # copied in, no manifest row
    (target / "data").mkdir()
    (target / "data" / "keep.txt").write_bytes(b"user")  # outside shaders/, no row: never touched
    return target


def test_deploy_prunes_shader_files_the_payload_does_not_carry(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    b64 = _build(tmp_path)
    target = _target_with_unlisted_dead_spv(tmp_path)
    items = [(str(p), f"shaders/vulkan/{p.name}", "shader") for p in dp.baked_spv_files(b64)]
    hashes = dp.deploy(items, str(target), False)
    assert set(hashes) == {"shaders/vulkan/live.frag.spv"}
    assert not (target / "shaders" / "vulkan" / "dead.frag.spv").exists()
    assert (target / "shaders" / "vulkan" / "live.frag.spv").is_file()
    assert (target / "data" / "keep.txt").is_file()


def test_partial_payload_without_spv_leaves_shaders_alone(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    target = _target_with_unlisted_dead_spv(tmp_path)
    src = tmp_path / "readme.txt"
    src.write_bytes(b"x")
    dp.deploy([(str(src), "readme.txt", "support")], str(target), False)
    assert (target / "shaders" / "vulkan" / "dead.frag.spv").is_file()


def test_retire_unowned_drops_rows_missing_from_the_new_manifest(tmp_path):
    """deploy --from-payload path: a row the payload manifest dropped is retired from the install (unchanged copy only)."""
    target = _target_with_unlisted_dead_spv(tmp_path)
    (target / "old.dll").write_bytes(b"dll")
    (target / "edited.txt").write_bytes(b"v1")
    dp.write_manifest(str(target), {"old.dll": (dp.sha256_file(str(target / "old.dll")), 3),
                                    "edited.txt": (dp.sha256_file(str(target / "edited.txt")), 2)}, "abc123")
    (target / "edited.txt").write_bytes(b"v2")  # modified since the deploy: preserved
    dp.retire_unowned(str(target), {"mc2.exe": {}}, own_shaders=True)
    assert not (target / "old.dll").exists()
    assert (target / "edited.txt").is_file()
    assert not (target / "shaders" / "vulkan" / "dead.frag.spv").exists()
    assert (target / "data" / "keep.txt").is_file()
