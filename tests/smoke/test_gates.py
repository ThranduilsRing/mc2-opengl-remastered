from scripts.smoke_lib.logparse import LogSummary, PerfRow
from scripts.smoke_lib.gates import evaluate, GateConfig, Verdict

CFG = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
                 duration_s=30)

def _base():
    # walltime_s=31 in the tests below; set last play heartbeat at 30.5s
    # wallclock so the default 3s play gate is satisfied.
    return LogSummary(instr_banner_seen=True, smoke_banner_seen=True,
                      mission_resolve_seen=True, smoke_summary_result="pass",
                      mission_ready_ms=5000.0, heartbeats_play=30,
                      last_heartbeat_wall_s_play=30.5,
                      perf=PerfRow(avg_fps=60))

def test_clean_pass():
    v = evaluate(_base(), CFG, exit_code=0, walltime_s=31)
    assert v.passed and v.buckets == []

def test_gl_error_fails():
    s = _base(); s.gl_errors = 1
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "gl_error" in v.buckets

def test_missing_instr_banner_fails():
    s = _base(); s.instr_banner_seen = False
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "instrumentation_missing" in v.buckets

def test_shader_error_fails():
    s = _base(); s.shader_errors = 2
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "shader_error" in v.buckets

def test_crash_no_summary():
    s = _base(); s.crash_handler_hit = True; s.smoke_summary_result = None
    v = evaluate(s, CFG, exit_code=-1, walltime_s=31)
    assert "crash_no_summary" in v.buckets

def test_heartbeat_freeze_play():
    s = _base()
    # Last play heartbeat at wallclock 20s; walltime 31s => 11s gap > 3s cfg.
    s.last_heartbeat_wall_s_play = 20.0
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "heartbeat_freeze_play" in v.buckets

def test_timeout_bucket_when_walltime_cap_hit():
    s = _base()
    v = evaluate(s, CFG, exit_code=-9, walltime_s=90, killed_by_timeout=True)
    assert v.buckets == ["timeout"]

def test_multiple_buckets_reported():
    s = _base(); s.gl_errors = 1; s.asset_oob = 1
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert set(v.buckets) >= {"gl_error", "asset_oob"}
