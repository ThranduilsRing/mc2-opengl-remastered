# tests/smoke/test_runner.py
import sys
import pytest
from pathlib import Path
from scripts.smoke_lib.runner import run_one, RunConfig

FIX = Path(__file__).parent / "fixtures"

@pytest.fixture(autouse=True)
def isolate_fake_diagnostics(monkeypatch):
    # Fake children do not emit Vulkan validation or diagnostic-state artifacts.
    # Keep developer runtime settings from adding unrelated verdict buckets.
    for name in ("MC2_VULKAN_VALIDATION", "MC2_DEBUG_STATE_DUMP", "MC2_DIAG_TAGS"):
        monkeypatch.delenv(name, raising=False)

def test_run_fake_pass(tmp_path):
    cfg = RunConfig(
        exe=[sys.executable, str(FIX / "fake_mc2_pass.py")],
        profile="stock", stem="mc2_05", duration=1,
        heartbeat_timeout_load_s=180, heartbeat_timeout_play_s=180,
        grace_s=180, env_extra={},
    )
    result = run_one(cfg)
    assert result.verdict.passed
    assert result.summary.smoke_summary_result == "pass"
    assert not result.killed_by_timeout

def test_run_fake_timeout(tmp_path):
    # Use a second fixture script that hangs regardless of --duration, so the
    # runner's walltime cap is what trips. duration=1 grace=0 ⇒ cap=1s total.
    cfg = RunConfig(
        exe=[sys.executable, str(FIX / "fake_mc2_hang.py")],
        profile="stock", stem="mc2_05", duration=1,
        heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
        grace_s=0, env_extra={},
    )
    result = run_one(cfg)
    assert result.killed_by_timeout
    assert "timeout" in result.verdict.buckets


# ---------------------------------------------------------------------------
# SMOKE-SYNTHETIC-FIXTURES-ALL-BUCKETS — end-to-end bucket coverage.
#
# Each case spawns fake_mc2_fault.py (one fault per bucket) through the REAL
# run_one() path so the full spawn -> capture -> parse_log -> evaluate chain is
# exercised, not just the gate logic on a hand-built LogSummary. Every fault is
# engineered to isolate exactly one bucket; we assert that bucket is present,
# the run did not pass, and no foreign bucket leaked in.
# ---------------------------------------------------------------------------

def _run_fault(fault, *, load_to=180, play_to=180):
    cfg = RunConfig(
        exe=[sys.executable, str(FIX / "fake_mc2_fault.py"), "--fault", fault],
        profile="stock", stem="mc2_05", duration=1,
        heartbeat_timeout_load_s=load_to, heartbeat_timeout_play_s=play_to,
        # Idle-priority startup and reader scheduling must not add timeout/freeze
        # buckets to fixtures testing a different fault.
        # The dedicated timeout fixture above keeps its zero-grace wall cap.
        grace_s=180, env_extra={},
    )
    return run_one(cfg)


def test_run_fault_instrumentation_missing():
    r = _run_fault("instrumentation_missing")
    assert r.verdict.buckets == ["instrumentation_missing"]
    assert not r.verdict.passed


def test_run_fault_crash_silent():
    r = _run_fault("crash_silent")
    assert r.exit_code != 0
    assert r.verdict.buckets == ["crash_silent"]
    assert not r.verdict.passed


def test_run_fault_crash_no_summary():
    r = _run_fault("crash_no_summary")
    assert r.summary.crash_handler_hit
    assert r.verdict.buckets == ["crash_no_summary"]
    assert not r.verdict.passed


def test_run_fault_engine_reported_fail():
    r = _run_fault("engine_reported_fail")
    assert r.summary.smoke_summary_result == "fail"
    assert r.verdict.buckets == ["engine_reported_fail"]
    assert not r.verdict.passed


def test_run_fault_pool_null():
    r = _run_fault("pool_null")
    assert r.summary.pool_nulls == 1
    assert r.verdict.buckets == ["pool_null"]
    assert not r.verdict.passed


def test_run_fault_asset_oob():
    r = _run_fault("asset_oob")
    assert r.summary.asset_oob == 1
    assert r.verdict.buckets == ["asset_oob"]
    assert not r.verdict.passed


def test_run_fault_shader_error():
    r = _run_fault("shader_error")
    assert r.summary.shader_errors >= 1
    assert r.verdict.buckets == ["shader_error"]
    assert not r.verdict.passed


def test_run_fault_shader_error_gl_prop():
    r = _run_fault("shader_error_gl_prop")
    assert r.summary.shader_errors >= 2
    assert r.verdict.buckets == ["shader_error"]
    assert not r.verdict.passed


def test_run_fault_shader_error_vk():
    r = _run_fault("shader_error_vk")
    assert r.summary.shader_errors >= 1
    assert r.verdict.buckets == ["shader_error"]
    assert not r.verdict.passed


def test_run_fault_missing_file():
    r = _run_fault("missing_file")
    assert r.summary.missing_files >= 1
    assert r.verdict.buckets == ["missing_file"]
    assert not r.verdict.passed


def test_run_fault_heartbeat_freeze_load():
    # Load heartbeat seen, mission_ready never reached. With load timeout 0 the
    # positive gap to end-of-run trips the freeze deterministically.
    r = _run_fault("heartbeat_freeze_load", load_to=0)
    assert r.summary.mission_ready_ms is None
    assert r.summary.heartbeats_load > 0
    assert r.verdict.buckets == ["heartbeat_freeze_load"]
    assert not r.verdict.passed


def test_run_fault_heartbeat_freeze_play():
    # mission_ready reached but no play-phase heartbeat at all -> definite freeze.
    r = _run_fault("heartbeat_freeze_play")
    assert r.summary.mission_ready_ms is not None
    assert r.verdict.buckets == ["heartbeat_freeze_play"]
    assert not r.verdict.passed


def test_fault_fixture_allows_slow_python_startup_and_teardown(tmp_path, monkeypatch):
    real = FIX / "fake_mc2_fault.py"
    delayed = tmp_path / "fake_mc2_fault.py"
    delayed.write_text("import atexit, runpy, time\natexit.register(lambda: time.sleep(4))\ntime.sleep(7)\nrunpy.run_path(" + repr(str(real)) + ", run_name='__main__')\n", encoding="utf-8")
    monkeypatch.setitem(globals(), "FIX", tmp_path)
    result = _run_fault("missing_file")
    assert not result.killed_by_timeout
    assert result.summary.missing_files >= 1
    assert result.verdict.buckets == ["missing_file"]
