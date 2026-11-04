"""video_decode_smoke.judge: offline verdicts on the log lines (the game run itself is scripts/video_decode_smoke.py)."""
import sys
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
