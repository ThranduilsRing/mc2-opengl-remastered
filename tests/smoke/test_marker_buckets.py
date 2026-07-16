# tests/smoke/test_marker_buckets.py
# META-FIX INFRA-3: table-driven marker buckets — proves the mechanism can
# fail a run (the "self-test leg" for the smoke-side marker table) and that
# the design-C split_path_parity row is armed with the right polarity.

from scripts.smoke_lib.logparse import LogSummary, MARKER_BUCKETS, PerfRow, parse_log
from scripts.smoke_lib.gates import GateConfig, evaluate

CFG = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
                 duration_s=30)


def _base():
    return LogSummary(instr_banner_seen=True, smoke_banner_seen=True,
                      mission_resolve_seen=True, smoke_summary_result="pass",
                      mission_ready_ms=5000.0, heartbeats_play=30,
                      last_heartbeat_wall_s_play=30.5,
                      perf=PerfRow(avg_fps=60))


def test_split_path_parity_row_registered():
    assert any(mb.bucket == "split_path_parity" and mb.severity == "red"
               for mb in MARKER_BUCKETS)


def test_parse_counts_match0_line():
    s = parse_log("[SPLIT_PATH_PARITY] site=terrain_material_load "
                  "pair=tess_vs_modern_shadow tess=0 shadow=0 match=0\n")
    assert s.marker_counts.get("split_path_parity") == 1


def test_parse_ignores_match1_line():
    s = parse_log("[SPLIT_PATH_PARITY] site=terrain_material_load "
                  "pair=tess_vs_modern_shadow tess=0 shadow=1 match=1\n")
    assert s.marker_counts.get("split_path_parity", 0) == 0


def test_red_marker_fails_run():
    s = _base()
    s.marker_counts = {"split_path_parity": 1}
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "split_path_parity" in v.buckets


def test_marker_override_downgrades_to_advisory():
    s = _base()
    s.marker_counts = {"split_path_parity": 1}
    cfg = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
                     duration_s=30,
                     marker_overrides={"split_path_parity": True})
    v = evaluate(s, cfg, exit_code=0, walltime_s=31)
    assert v.passed and "split_path_parity" not in v.buckets
    assert any("advisory" in d for d in v.details)
