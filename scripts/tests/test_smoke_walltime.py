"""walltime stop rule: a slow load (cook, big map) must not eat the play window or be killed while still printing."""
from smoke_lib import stall  # noqa: E402


def test_pdb_grace_only_between_missions():
    import ast
    from pathlib import Path
    tree = ast.parse((Path(__file__).parents[1] / "run_smoke.py").read_text(encoding="utf-8"))
    loop = next(n for n in ast.walk(tree) if isinstance(n, ast.For) and ast.unparse(n.iter) == "selected")
    grace = ast.Module(body=[loop.body[-1]], type_ignores=[])
    selected = [object(), object()]
    sleeps = []
    import time
    from unittest.mock import patch
    with patch.object(time, "sleep", sleeps.append):
        for e in selected:
            exec(compile(grace, "grace", "exec"), {"e": e, "selected": selected, "_t": time})
    assert sleeps == [2]


def test_load_past_cap_with_moving_log_is_not_killed():
    assert not stall.walltime_exceeded(140, cap=130, duration=10, first_frame_at=None, idle_s=2)


def test_load_past_cap_with_silent_log_is_killed():
    assert stall.walltime_exceeded(200, cap=130, duration=10, first_frame_at=None, idle_s=61)


def test_load_hard_cap_stops_a_log_that_never_presents():
    assert stall.walltime_exceeded(stall.LOAD_HARD_CAP_S + 1, cap=130, duration=10, first_frame_at=None, idle_s=1)


def test_before_cap_never_kills():
    assert not stall.walltime_exceeded(100, cap=130, duration=10, first_frame_at=None, idle_s=500)


def test_play_window_counts_from_first_frame():
    # mc2_04: first frame at 119.8 s, duration 10: old cap 130 killed it; now it gets duration + grace after the frame.
    assert not stall.walltime_exceeded(131, cap=130, duration=10, first_frame_at=119.8, idle_s=0)
    assert not stall.walltime_exceeded(119.8 + 10 + stall.PLAY_GRACE_S, cap=130, duration=10, first_frame_at=119.8, idle_s=0)
    assert stall.walltime_exceeded(119.8 + 10 + stall.PLAY_GRACE_S + 1, cap=130, duration=10, first_frame_at=119.8, idle_s=0)


def test_early_first_frame_keeps_old_cap():
    assert not stall.walltime_exceeded(60, cap=130, duration=10, first_frame_at=5, idle_s=0)
    assert stall.walltime_exceeded(131, cap=130, duration=10, first_frame_at=5, idle_s=0)
