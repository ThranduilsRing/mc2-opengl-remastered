"""Orchestrator fix-up (Fable review of 319fec62d): every row laptop_sim_rows() returns must be accepted by sim_plan(),
and capture-after is advisory at tier 0 per the GATE DIET line."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate  # noqa: E402


def test_laptop_sim_rows_are_valid_sim_plan_rows(tmp_path):
    rows = gate.laptop_sim_rows(["code/mission_update.cpp"])
    assert rows
    steps = gate.sim_plan(str(tmp_path / "install"), tmp_path / "proof", rows)
    assert sorted(s[0] for s in steps) == sorted(rows)


def test_capture_after_is_advisory_only_at_tier0():
    assert gate.advisory_status("capture-after", "FAIL", 0).startswith(gate.ADVISORY_PREFIX)
    assert gate.advisory_status("capture-after", "FAIL", 1) == "FAIL"
