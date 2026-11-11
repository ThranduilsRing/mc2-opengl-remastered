"""--acceptance bench: a few frames with incomplete GPU timestamps (load-in, a stall) no longer abort the whole run."""
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import run_battle_bench as bench  # noqa: E402


def _summarize(tmp_path, frames, bad):
    rows = [{"type": "world", "frame": 1, "sim_s": 0, "movers": 50, "units_alive": 50, "mechs": 48, "destroyed": 7},
            {"type": "world", "frame": 10, "sim_s": 3, "movers": 50, "units_alive": 49, "mechs": 48, "destroyed": 8},
            {"type": "orders", "sim_s": 1, "attacks": 48}]
    for frame in range(10, 10 + frames):
        ok = frame not in bad
        rows += [{"type": "gpu", "frame": frame, "ms": 1, "recorded": 3, "valid": 3 if ok else 2, "passes": {"VKIsl.Terrain": 1}},
                 {"type": "cpu", "frame": frame, "ms": 1, "ticks": 1, "tick_total_ms": 1},
                 {"type": "tick", "frame": frame, "ms": 1}]
    (tmp_path / "commands.txt.ack").write_text("spawn clone 0 0 -> ok (spawned id=1 hp=1.000)\n", encoding="utf-8")
    (tmp_path / "frames.jsonl").write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    return bench.summarize(tmp_path, "[TIMING v1] event=mission_ready elapsed_ms=1.0", {"spawn": {"count": 1}})


def test_small_fraction_of_incomplete_gpu_frames_is_tolerated_and_counted(tmp_path, capsys):
    # W148 quiet window: 12 of 5613 frames (0.2%) had incomplete timestamps and aborted the bench.
    report = _summarize(tmp_path, 2000, bad=set(range(22, 34)))   # 12 of 2000 = 0.6%
    assert report["gpu_incomplete_frames"] == 12 and report["gpu_frame"]["n"] == 1988


def test_incomplete_frames_are_dropped_from_the_stats_and_logged(tmp_path, capsys):
    report = _summarize(tmp_path, 2000, bad={20, 21, 22, 500})
    assert report["gpu_incomplete_frames"] == 4 and report["gpu_frame"]["n"] == 1996
    assert "4 of 2000" in capsys.readouterr().out


def test_large_fraction_of_incomplete_gpu_frames_is_refused(tmp_path):
    with pytest.raises(ValueError, match="incomplete"):
        _summarize(tmp_path, 200, bad=set(range(10, 30)))   # 10%


def test_no_complete_gpu_frame_is_refused(tmp_path):
    with pytest.raises(ValueError, match="incomplete"):
        _summarize(tmp_path, 5, bad=set(range(10, 15)))
