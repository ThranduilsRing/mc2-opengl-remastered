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


def test_static_light_row_out_of_range_is_a_red_marker():
    bad = ("[STATIC_LIGHT_ROW v1] event=zero_row_reached_gpu class=out_of_range regIdx=7 typeID=3 leaves=1 "
           "lightIdx=2618 numLights=2 tableCount=2618 rngLightIdx=2618 multiCache=2618")
    hits = [b for b in logparse.MARKER_BUCKETS if b.pattern.search(bad)]
    assert [(h.bucket, h.severity) for h in hits] == [("static_light_row_out_of_range", "red")]
    assert not [b for b in logparse.MARKER_BUCKETS
                if b.pattern.search("[STATIC_LIGHT_ROW v1] event=census flushes=300 out_of_range=0")]
