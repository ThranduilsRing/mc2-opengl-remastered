"""keep_display_on asks Windows for display + system awake on the calling thread and never raises."""
import ctypes
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke_lib import keep_awake as ka


def test_flags_and_result(monkeypatch):
    seen = []
    fake = SimpleNamespace(kernel32=SimpleNamespace(SetThreadExecutionState=lambda f: seen.append(f) or 1))
    monkeypatch.setattr(ka.sys, "platform", "win32")
    monkeypatch.setattr(ctypes, "windll", fake, raising=False)
    assert ka.keep_display_on() is True
    assert seen == [0x80000003]


def test_failure_is_false(monkeypatch):
    def boom(_):
        raise OSError("no")
    monkeypatch.setattr(ka.sys, "platform", "win32")
    monkeypatch.setattr(ctypes, "windll", SimpleNamespace(kernel32=SimpleNamespace(SetThreadExecutionState=boom)), raising=False)
    assert ka.keep_display_on() is False


def test_non_windows(monkeypatch):
    monkeypatch.setattr(ka.sys, "platform", "linux")
    assert ka.keep_display_on() is False
