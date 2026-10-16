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
    s = _base(); s.pool_nulls = 1; s.asset_oob = 1
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert set(v.buckets) >= {"pool_null", "asset_oob"}

# --- SMOKE-GATES-BUCKET-COVERAGE-1: buckets the original suite never asserted ---

def test_crash_silent():
    # No engine smoke summary AND nonzero exit AND crash handler did NOT fire.
    # The single most important crash bucket; a regression mislabeling this as a
    # pass makes the whole smoke gate silently blind.
    s = _base(); s.smoke_summary_result = None; s.crash_handler_hit = False
    v = evaluate(s, CFG, exit_code=1, walltime_s=31)
    assert not v.passed and "crash_silent" in v.buckets
    assert "crash_no_summary" not in v.buckets  # distinct from the handler-fired case

def test_crash_silent_not_fired_on_clean_exit():
    # Guard the boundary: exit 0 with no summary must NOT be crash_silent.
    s = _base(); s.smoke_summary_result = None
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "crash_silent" not in v.buckets

def test_engine_reported_fail():
    s = _base(); s.smoke_summary_result = "fail"
    s.smoke_summary_reason = "objective_failed"; s.smoke_summary_stage = "play"
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "engine_reported_fail" in v.buckets

def test_heartbeat_freeze_load():
    # Never reached play (mission_ready_ms is None) but load heartbeats stalled:
    # last load heartbeat at 2s wallclock, walltime 90s => 88s gap > 60s load cfg.
    s = _base()
    s.mission_ready_ms = None
    s.last_heartbeat_wall_s_play = None
    s.heartbeats_load = 5
    s.last_heartbeat_wall_s_load = 2.0
    v = evaluate(s, CFG, exit_code=0, walltime_s=90)
    assert "heartbeat_freeze_load" in v.buckets
    assert "heartbeat_freeze_play" not in v.buckets  # play branch skipped when not ready

def test_pool_null():
    s = _base(); s.pool_nulls = 3
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "pool_null" in v.buckets

def test_missing_file():
    s = _base(); s.missing_files = 2
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "missing_file" in v.buckets

def test_asset_oob_respects_allow_flag():
    s = _base(); s.asset_oob = 4
    # Default: asset_oob is a failing bucket.
    assert "asset_oob" in evaluate(s, CFG, exit_code=0, walltime_s=31).buckets
    # allow_asset_oob suppresses it.
    cfg_allow = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
                           duration_s=30, allow_asset_oob=True)
    v = evaluate(s, cfg_allow, exit_code=0, walltime_s=31)
    assert "asset_oob" not in v.buckets


if __name__ == "__main__":
    # Lightweight self-runner (no pytest dependency): run every test_* and report.
    import sys
    fns = [(n, f) for n, f in sorted(globals().items())
           if n.startswith("test_") and callable(f)]
    failed = 0
    for name, fn in fns:
        try:
            fn()
            print(f"  [PASS] {name}")
        except AssertionError as e:
            failed += 1
            print(f"  [FAIL] {name}: {e}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"  [ERROR] {name}: {e!r}")
    print(f"test_gates: {'PASS' if not failed else 'FAIL'} "
          f"({len(fns)} tests, {failed} failed)")
    sys.exit(1 if failed else 0)
