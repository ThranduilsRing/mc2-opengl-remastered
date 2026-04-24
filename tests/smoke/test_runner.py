# tests/smoke/test_runner.py
import sys
from pathlib import Path
from scripts.smoke_lib.runner import run_one, RunConfig

FIX = Path(__file__).parent / "fixtures"

def test_run_fake_pass(tmp_path):
    cfg = RunConfig(
        exe=[sys.executable, str(FIX / "fake_mc2_pass.py")],
        profile="stock", stem="mc2_01", duration=1,
        heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
        grace_s=5, env_extra={},
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
        profile="stock", stem="mc2_01", duration=1,
        heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
        grace_s=0, env_extra={},
    )
    result = run_one(cfg)
    assert result.killed_by_timeout
    assert "timeout" in result.verdict.buckets
