"""run_visual_capture: the exit code, the manifest and the log use ONE predicate (fail_reasons)."""
import inspect
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import run_visual_capture as rvc  # noqa: E402


def _ok():
    return {"all_present": True, "engine_capture_fired": True,
            "backend": {"disagree": []}, "render_options": {"disagree": []}}


def test_clean_result_has_no_reasons():
    assert rvc.fail_reasons(_ok()) == []


@pytest.mark.parametrize("mut", [
    {"all_present": False},
    {"engine_capture_fired": False},
    {"render_options": {"disagree": ["shadow quality differs"]}},
    {"backend": {"disagree": ["drawn by gl"]}},
])
def test_each_evidence_gap_is_a_failure(mut):
    r = {**_ok(), **mut}
    assert rvc.fail_reasons(r)


def test_exit_codes_come_from_fail_reasons_not_a_hand_copied_subset():
    src = inspect.getsource(rvc.main)
    assert "return 0 if not fail_reasons(result) else 1" in src
    assert 'result["backend"]["disagree"]) else 1' not in src
