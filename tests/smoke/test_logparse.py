from pathlib import Path
from scripts.smoke_lib.logparse import parse_log, LogSummary

FIX = Path(__file__).parent / "fixtures"

def test_pass_log_counts_zero_faults():
    text = (FIX / "fake_engine_pass.log").read_text()
    # Synthesize per-line wallclocks at 0.1s intervals for this test.
    wall = [0.1 * i for i in range(len(text.splitlines()))]
    s = parse_log(text, line_wallclocks=wall)
    assert s.instr_banner_seen
    assert s.smoke_summary_result == "pass"
    assert s.gl_errors == 0
    assert s.pool_nulls == 0
    assert s.asset_oob == 0
    assert s.heartbeats_play > 0
    assert s.mission_ready_ms == 6800.1
    assert s.last_heartbeat_wall_s_play is not None
    assert s.perf.avg_fps == 59.8

def test_gl_error_detected():
    s = parse_log((FIX / "fake_engine_gl_error.log").read_text())
    assert s.gl_errors == 1

def test_freeze_load_no_mission_ready():
    text = (FIX / "fake_engine_freeze_load.log").read_text()
    wall = [0.1 * i for i in range(len(text.splitlines()))]
    s = parse_log(text, line_wallclocks=wall)
    assert s.mission_ready_ms is None
    assert s.heartbeats_load >= 1
    assert s.heartbeats_play == 0
    assert s.smoke_summary_result is None
    assert s.last_heartbeat_wall_s_load is not None

def test_crash_no_summary_detected():
    log = (
        "[INSTR v1] enabled: smoke=1\n"
        "[TIMING v1] event=mission_ready elapsed_ms=5000\n"
        "CRASH: unhandled exception at 0xdeadbeef\n"
    )
    s = parse_log(log)
    assert s.crash_handler_hit is True
    assert s.smoke_summary_result is None
