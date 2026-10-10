"""tidy_lock: one tidy tool per build tree, and a cooperative stop request (no builds)."""
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tidy_lock as tl  # noqa: E402


def test_lock_is_taken_and_released(tmp_path):
    with tl.TidyLock(str(tmp_path)):
        rec = json.loads((tmp_path / tl.LOCK_NAME).read_text())
        assert rec["pid"] == os.getpid()
    assert not (tmp_path / tl.LOCK_NAME).exists()


def test_a_live_holder_refuses_a_second_tool(tmp_path):
    (tmp_path / tl.LOCK_NAME).write_text(json.dumps({"pid": os.getppid(), "created": None, "argv": ["run_tidy_headers.py"], "since": 0}))
    with pytest.raises(tl.TidyBusy, match="held by pid"):
        with tl.TidyLock(str(tmp_path)):
            pass
    assert (tmp_path / tl.LOCK_NAME).exists()            # the holder's lock is left alone


def test_a_dead_holder_is_replaced(tmp_path, monkeypatch):
    (tmp_path / tl.LOCK_NAME).write_text(json.dumps({"pid": 4000000, "created": None, "argv": [], "since": 0}))
    monkeypatch.setattr(tl.child_registry, "pid_alive", lambda pid: pid == os.getpid())
    with tl.TidyLock(str(tmp_path)):
        assert json.loads((tmp_path / tl.LOCK_NAME).read_text())["pid"] == os.getpid()


def test_stop_request_needs_a_live_holder(tmp_path):
    assert "nothing to stop" in tl.request_stop(str(tmp_path))
    assert not tl.stop_requested(str(tmp_path))
    with tl.TidyLock(str(tmp_path)):
        assert "stop requested" in tl.request_stop(str(tmp_path))
        assert tl.stop_requested(str(tmp_path))
        tl.clear_stop(str(tmp_path))
        assert not tl.stop_requested(str(tmp_path))
