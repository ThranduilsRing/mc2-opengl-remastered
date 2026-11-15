"""build_contract_harnesses --only <tests/unit file stem> expands to that file's TEST_CASE names."""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import build_contract_harnesses as bch  # noqa: E402


def test_file_stem_expands_to_its_cases():
    cases = bch.unit_file_cases("test_vk_rt_scene")
    assert cases and bch.unit_file_cases("test_vk_rt_scene.cpp") == cases
    out = bch.expand_file_selection(["test_vk_rt_scene", "SomeCaseName"])
    assert out[-1] == "SomeCaseName" and len(out) == len(cases) + 1
    assert not bch.unknown_selection(out[:-1])


def test_non_files_pass_through():
    assert bch.unit_file_cases("no_such_file_xyz") is None
    assert bch.expand_file_selection(["a*", "b"]) == ["a*", "b"]


# Trigger: tools/build_contract_harnesses.py selection/configure reuse.
def test_registered_wildcard_and_union_owners():
    assert bch.case_owner(["camera_frustum_harness"]) == ["tests/unit/test_camera_frustum_harness.cpp"]
    assert bch.case_owner(["camera_frustum_harness/*", "camera_frustum_harness"]) == bch.case_owner(["camera_frustum_harness"])
    assert len(bch.case_owner(["camera_frustum_harness", "job_system_harness"])) == 2


def test_warm_tree_tracks_arguments_and_selection(tmp_path):
    command = ["cmake", "-S", "src", "-B", str(tmp_path)]
    only = ["camera_frustum_harness"]
    reused, receipt, signature = bch.prepare_unit_tree(tmp_path, command, only)
    assert not reused
    (tmp_path / "CMakeCache.txt").write_text("configured")
    receipt.write_text(signature)
    selection = tmp_path / "mc2_unit_selection.cmake"
    stamp = selection.stat().st_mtime_ns
    assert bch.prepare_unit_tree(tmp_path, command, only)[0]
    assert selection.stat().st_mtime_ns == stamp
    assert bch.prepare_unit_tree(tmp_path, command, None)[0]  # full target shares the tree
    assert not bch.prepare_unit_tree(tmp_path, command + ["-DMC2_ASAN=ON"], only)[0]
    assert not bch.prepare_unit_tree(tmp_path, command, ["job_system_harness"])[0]
    assert not bch.prepare_unit_tree(tmp_path, command, ["job_system_harness"])[0]  # failed configure cannot warm it
