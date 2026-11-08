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
