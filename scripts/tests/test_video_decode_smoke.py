"""video_decode_smoke.judge: offline verdicts on the log lines (the game run itself is scripts/video_decode_smoke.py)."""
import sys

import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import video_decode_smoke as v  # noqa: E402


def test_crash_signature_fails():
    log = "[VIDEO] init: opened a.bik\n[SIM_OVERLAP v1] VIOLATION sim-thread-called-render-api site=gos_NewEmptyTexture count=1 abort=1\n"
    ok, why = v.judge(log)
    assert not ok and "gos_NewEmptyTexture" in why


def test_skipped_decode_fails():
    ok, why = v.judge("[VIDEO] init: smoke mode active, skipping movie 'V1A'\n")
    assert not ok and "never decoded" in why


def test_played_passes():
    ok, _ = v.judge("[VIDEO] init: opened ./data/movies/V1A.bik (320x240, fps=15.00, audio=1)\n")
    assert ok


FIXTURES = Path(__file__).resolve().parent / "fixtures"


def test_judge_on_abort_shaped_log_fixture():
    """Log shaped like the engine's real emit sites (sim_overlap.cpp violation(), mc2movie.cpp): a movie opened, then a hard abort."""
    ok, why = v.judge((FIXTURES / "video_smoke_fail.txt").read_text())
    assert not ok and "gos_NewEmptyTexture" in why


def test_judge_on_pass_shaped_log_fixture():
    ok, _ = v.judge((FIXTURES / "video_smoke_pass.txt").read_text())
    assert ok


def test_end_row_real_fail_log_names_removeall_violation():
    """Trimmed real log of the mission-end row run on the P0-1 revert (resultsDone on the sim worker) at MC2_SIM_OVERLAP_AUDIT=2."""
    ok, why = v.judge((FIXTURES / "video_smoke_end_fail.txt").read_text(), require_results=True)
    assert not ok and "ForceGroupBar::removeAll" in why


def test_end_row_real_pass_log_reaches_results():
    """Trimmed real log of the same row on the fixed tree: video opened, resultsDone complete, countdown early_exit accepted."""
    text = (FIXTURES / "video_smoke_end_pass.txt").read_text()
    ok, _ = v.judge(text, require_results=True)
    assert ok
    ok, why = v.judge(text.replace("resultsDone complete", "resultsDone"), require_results=True)
    assert not ok


def test_graveyard_real_fail_log_names_drain_violation():
    """Trimmed real log of the graveyard ordering row on an R1-revert build (retireRenderPacket before waitWorker) at AUDIT=2."""
    ok, why = v.judge((FIXTURES / "video_smoke_graveyard_fail.txt").read_text(), min_retires=200)
    assert not ok and "graveyard drain" in why


def test_graveyard_real_pass_log_audits_drains():
    """Trimmed real log of the same row on the fixed tree: 200+ audited drain calls, zero violation."""
    text = (FIXTURES / "video_smoke_graveyard_pass.txt").read_text()
    ok, why = v.judge(text, min_retires=200)
    assert ok, why
    ok, why = v.judge(text.replace("retire_calls=4200", "retire_calls=40").replace("retire_calls=2000", "retire_calls=40").replace("retire_calls=100", "retire_calls=40"), min_retires=200)
    assert not ok and "ordering proof did not run" in why


def test_early_exit_only_accepts_the_bare_countdown_exit():
    text = (FIXTURES / "video_smoke_end_pass.txt").read_text()
    assert v.early_exit_only(text)
    assert v.early_exit_only(text + "| mc2_01 | FAIL | engine_reported_fail: reason=early_exit stage=atexit |\n")


@pytest.mark.parametrize("extra", [
    "[runner] [DEPLOY_FINGERPRINT] MISMATCH: exe sha differs\n",
    "| mc2_01 | FAIL | engine_reported_fail, shader_error |\n",
    "[VK] shader compile failed: terrain.comp\n",
    "[runner] [ORDER-LIVENESS] HARD FAIL: 1 check failed\n",
    "> **RUN INVALID**\n",
])
def test_early_exit_with_mixed_failure_is_not_accepted(extra):
    text = (FIXTURES / "video_smoke_end_pass.txt").read_text()
    assert not v.early_exit_only(text + extra)
