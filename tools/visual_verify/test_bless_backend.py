"""bless.py --backend: a golden named for a backend accepts only runs that asked for it, were drawn by it alone and declared its environment.

WHY: the VK golden set replaces the GL one before GL is deleted (vk_golden_plan.md); a VK golden minted from a GL-drawn or unfrozen run would
grade every later VK candidate against the wrong picture. The synthetic runs and binary come from bless_selftest.py.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_THIS = Path(__file__).resolve()
sys.path.insert(0, str(_THIS.parent))

import bless as blesslib  # noqa: E402
import bless_selftest as st  # noqa: E402
import regions as regionlib  # noqa: E402

VK = {"requested": "vulkan", "drew": ["vulkan"]}
VK_ENV = {"MC2_SMOKE_FREEZE_ANIM": "1", "MC2_RENDER_BACKEND": "vulkan", "MC2_VK_SHARED_GBUFFER": "1"}


@pytest.fixture()
def world(tmp_path, monkeypatch):
    rdir = tmp_path / "regions"
    rdir.mkdir()
    (rdir / f"{st.BM}.json").write_text(json.dumps(st.MANIFEST), encoding="utf-8")
    monkeypatch.setattr(regionlib, "REGIONS_DIR", rdir)
    exe = st._make_binary(tmp_path / "engine.bin", st.BINARY_COMMIT)
    return tmp_path, exe


def _runs(root: Path, exe: Path, backend: dict, declared: dict, tag: str) -> "list[Path]":
    dirs = []
    for i in (1, 2, 3):
        rd = st._make_run(root, f"{tag}{i}", 1, exe=exe, backend=backend)
        cap_path = rd / "capture_run.json"
        cap = json.loads(cap_path.read_text(encoding="utf-8"))
        cap["declared_env"] = declared
        cap_path.write_text(json.dumps(cap), encoding="utf-8")
        dirs.append(rd)
    return dirs


def test_vulkan_golden_records_its_backend(world):
    root, exe = world
    g = blesslib.bless(_runs(root, exe, VK, VK_ENV, "vk"), root / "golden", named_backend="vulkan")
    assert g["backend"] == "vulkan"
    saved = json.loads((root / "golden" / "golden.json").read_text(encoding="utf-8"))
    assert saved["backend"] == "vulkan" and saved["capture_env"]["backend"] == VK


def test_unnamed_golden_records_the_requested_backend(world):
    root, exe = world
    g = blesslib.bless(_runs(root, exe, {"requested": "gl", "drew": ["gl"]}, {"MC2_SMOKE_FREEZE_ANIM": "1"}, "gl"), root / "golden")
    assert g["backend"] == "gl"


@pytest.mark.parametrize("backend, declared, needle", [
    ({"requested": "gl", "drew": ["gl"]}, VK_ENV, "asked for 'gl'"),
    ({"requested": "vulkan", "drew": ["vulkan", "gl"]}, VK_ENV, "drew it"),
    (VK, {k: v for k, v in VK_ENV.items() if k != "MC2_SMOKE_FREEZE_ANIM"}, "MC2_SMOKE_FREEZE_ANIM=1"),
    (VK, {"MC2_SMOKE_FREEZE_ANIM": "1", "MC2_RENDER_BACKEND": "vulkan"}, "MC2_VK_SHARED_GBUFFER=1"),
])
def test_vulkan_golden_refuses_wrong_runs(world, backend, declared, needle):
    root, exe = world
    with pytest.raises(SystemExit) as e:
        blesslib.bless(_runs(root, exe, backend, declared, "bad"), root / "golden", named_backend="vulkan")
    assert needle in str(e.value)
    assert not (root / "golden" / "golden.json").exists()


def test_unknown_backend_is_refused():
    with pytest.raises(SystemExit, match="unknown backend"):
        blesslib.require_backend([{}], "metal")


def test_golden_records_the_harness_seed(world):
    root, exe = world
    runs = _runs(root, exe, VK, VK_ENV, "seed")
    for rd in runs:
        cap = json.loads((rd / "capture_run.json").read_text(encoding="utf-8"))
        cap["harness_env"] = {"MC2_DETERMINISTIC_RNG": "1", "MC2_SMOKE_FIXED_TIMESTEP": "1"}
        (rd / "capture_run.json").write_text(json.dumps(cap), encoding="utf-8")
    g = blesslib.bless(runs, root / "golden", named_backend="vulkan")
    assert g["capture_env"]["harness_env"]["MC2_DETERMINISTIC_RNG"] == "1"


def test_capture_harness_env_drops_the_per_pose_paths():
    sys.path.insert(0, str(_THIS.parent))
    import capture as caplib  # noqa: E402
    poses = [{"harness_set": {"MC2_DETERMINISTIC_RNG": "1", "MC2_VISUAL_CAPTURE_DIR": "x", "MC2_VISUAL_BOOKMARK_CAPTURE": "y"}}]
    assert caplib._harness_env(poses) == {"MC2_DETERMINISTIC_RNG": "1"}
    assert caplib._harness_env([{}]) == {}
