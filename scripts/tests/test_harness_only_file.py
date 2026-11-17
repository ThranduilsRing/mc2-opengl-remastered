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


# Trigger: tools/build_contract_harnesses.py configure reuse.
def test_warm_tree_tracks_arguments_and_missing_cache(tmp_path):
    command = ["cmake", "-S", "src", "-B", str(tmp_path)]
    reused, receipt, signature = bch.prepare_unit_tree(tmp_path, command)
    assert not reused
    (tmp_path / "CMakeCache.txt").write_text("configured")
    receipt.write_text(signature)
    stamp = receipt.stat().st_mtime_ns
    assert bch.prepare_unit_tree(tmp_path, command)[0]
    assert receipt.stat().st_mtime_ns == stamp
    changed = command + ["-DMC2_ASAN=ON"]
    assert not bch.prepare_unit_tree(tmp_path, changed)[0]
    assert not bch.prepare_unit_tree(tmp_path, changed)[0]  # failed configure cannot warm it
    (tmp_path / "CMakeCache.txt").unlink()
    assert not bch.prepare_unit_tree(tmp_path, command)[0]

# Trigger: tools/build_contract_harnesses.py source selection and configure arguments.
def test_selected_owners_cover_shared_stub_consumers():
    assert bch.case_owner(["camera_frustum_harness"]) == ["tests/unit/test_camera_frustum_harness.cpp"]
    for stem in ("test_part_archive", "test_authored_route", "test_abl_position", "test_tg_type_node_dtor"):
        assert "tests/unit/" + stem + ".cpp" in bch.case_owner(bch.expand_file_selection([stem]))


def test_selection_changes_invalidate_configure_receipt(tmp_path):
    full = ["cmake", "-DMC2_UNIT_ONLY_SOURCES="]
    selected = ["cmake", "-DMC2_UNIT_ONLY_SOURCES=test_part_archive.cpp"]
    (tmp_path / "CMakeCache.txt").write_text("configured")
    _, receipt, signature = bch.prepare_unit_tree(tmp_path, full)
    receipt.write_text(signature)
    assert not bch.prepare_unit_tree(tmp_path, selected)[0]
    receipt.write_text(bch.prepare_unit_tree(tmp_path, selected)[2])
    assert bch.prepare_unit_tree(tmp_path, selected)[0]
    assert not bch.prepare_unit_tree(tmp_path, full)[0]
