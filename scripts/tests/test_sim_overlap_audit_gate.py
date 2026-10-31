"""The overlap audit is gate-visible: a VIOLATION line fails smoke and determinism runs and is quoted."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))
from scripts.smoke_lib import gates, logparse
import sim_determinism as sim

CLEAN = ("[INSTR v1] enabled: test\n[SMOKE v1] event=summary result=pass\n"
         "[CONTENT_SUMMARY] mission=mc2_10 total=0 texture=0 shape=0 objtype=0 ammo=0 movetype=0 sound=0 missiondata=0\n")
LINE = "[SIM_OVERLAP v1] VIOLATION kind=owner site=ObjectTypeManager::get load"


def evaluate(text):
    return gates.evaluate(logparse.parse_log(text), gates.GateConfig(), exit_code=0, walltime_s=1)


def test_violation_line_fails_smoke_and_is_quoted():
    assert evaluate(CLEAN).passed
    verdict = evaluate(CLEAN + LINE + "\n")
    assert not verdict.passed
    assert "sim_overlap_violation" in verdict.buckets
    assert any(LINE in d for d in verdict.details)


def test_non_violation_overlap_lines_stay_green():
    assert evaluate(CLEAN + "[SIM_OVERLAP v1] stale_view_skip class=1 watch=2 total=1\n").passed


def test_determinism_detects_violation():
    assert sim.overlap_violation(CLEAN) == ""
    assert sim.overlap_violation(CLEAN + LINE + "\n").startswith("[SIM_OVERLAP v1] VIOLATION")
    assert {"mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24"} <= set(sim.OVERLAP_AUDIT_MISSIONS)
