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
    (target / "shaders" / "owner_override.frag").write_bytes(b"owner")   # W130 review: not baked SPIR-V, never swept
    (target / "shaders" / "vulkan" / "notes.txt").write_bytes(b"owner")
    return target


def test_deploy_preserves_unmanifested_shader_files(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    b64 = _build(tmp_path)
    target = _target_with_unlisted_dead_spv(tmp_path)
    items = [(str(p), f"shaders/vulkan/{p.name}", "shader") for p in dp.baked_spv_files(b64)]
    hashes = dp.deploy(items, str(target), False)
    assert set(hashes) == {"shaders/vulkan/live.frag.spv"}
    assert (target / "shaders" / "vulkan" / "dead.frag.spv").read_bytes() == b"old"
    assert (target / "shaders" / "vulkan" / "live.frag.spv").is_file()
    assert (target / "data" / "keep.txt").is_file()
    assert (target / "shaders" / "owner_override.frag").is_file() and (target / "shaders" / "vulkan" / "notes.txt").is_file()


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
    assert (target / "shaders" / "vulkan" / "dead.frag.spv").read_bytes() == b"old"
    assert (target / "data" / "keep.txt").is_file()


def test_shader_inventory_honours_manifest_prune(tmp_path, monkeypatch):
    """W152: unchanged owned water_reflection.comp retires; modified/unowned sources remain extras."""
    source = tmp_path / "source"
    source.mkdir()
    build = _build(source)
    live = build / "shaders" / "vulkan" / "live.frag.spv"
    rel = "shaders/vulkan/water_reflection.comp"
    shipped = [(str(live), "shaders/vulkan/live.frag.spv", "shader")]
    monkeypatch.setattr(dp, "enumerate_payload", lambda *args, **kwargs: shipped)
    real_deploy = dp.deploy
    deployed = []
    def deploy(*args, **kwargs):
        deployed.append(args[0])
        assert kwargs == {"code_only": True, "verify_normals": False}
        return real_deploy(*args, **kwargs)
    monkeypatch.setattr(dp, "deploy", deploy)
    for ownership in ("unchanged", "modified", "unowned"):
        target = tmp_path / ownership
        retired = target / rel
        retired.parent.mkdir(parents=True)
        retired.write_bytes(b"retired water reflection shader")
        if ownership != "unowned":
            dp.write_manifest(str(target), {rel: (dp.sha256_file(retired), retired.stat().st_size)}, "old-head")
        if ownership == "modified":
            retired.write_bytes(b"owner-edited water reflection shader")
        before = {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()}
        problems = dp.shader_inventory_problems(source, target)
        if ownership == "unchanged":
            assert problems == []
        else:
            assert len(problems) == 1
            assert f"['{rel}'] extra, [] missing" in problems[0]
        assert {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()} == before
    assert deployed == [shipped] * 3
