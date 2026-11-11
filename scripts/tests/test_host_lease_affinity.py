"""smoke_lib.host_lease._refresh_game_affinity: a WinError 5 (access denied) on ANOTHER lane's engine must be
skipped with one log line per pid, not raised: a capture run died here after a 20-minute lease wait (2026-11-10)."""
import ctypes
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts.smoke_lib import host_lease  # noqa: E402


class _Fn:
    """A kernel32 function slot: callable, and accepts the .argtypes/.restype assignments the module makes."""

    def __init__(self, fn):
        self.fn = fn
        self.argtypes = None
        self.restype = None

    def __call__(self, *args):
        return self.fn(*args)


class _FakeKernel:
    def __init__(self, handles):
        self.handles = handles          # pid -> handle (missing/0 = OpenProcess refused)
        self.opened = []
        self.OpenProcess = _Fn(self._open)
        self.CloseHandle = _Fn(lambda handle: None)

    def _open(self, access, inherit, pid):
        self.opened.append(pid)
        return self.handles.get(pid, 0)


def _pin_setup(monkeypatch, record, handles):
    """Drive _refresh_game_affinity against fake lease records and a fake kernel32 (no real process)."""
    monkeypatch.delenv("MC2_NO_CPU_PIN", raising=False)
    monkeypatch.setattr(host_lease, "redirected", lambda: False)
    monkeypatch.setattr(host_lease.sys, "platform", "win32")
    monkeypatch.setattr(host_lease, "game_masks", lambda: [0x3, 0xC])
    monkeypatch.setattr(host_lease, "_holder_alive", lambda r: bool(r))
    monkeypatch.setattr(host_lease.child_registry, "process_create_time", lambda pid: 999)
    monkeypatch.setattr(host_lease.child_registry, "pid_alive", lambda pid: True)
    monkeypatch.setattr(host_lease, "_records", lambda: [(None, record)])
    monkeypatch.setattr(ctypes, "WinDLL", lambda *a, **k: _FakeKernel(handles))


def test_skips_foreign_pid_the_setter_refuses(monkeypatch, capsys):
    """PermissionError from the affinity setter on one of another lane's engines: one log line, the rest still pinned."""
    foreign = {"pid": 31337, "affinity_mask": 0x3,
               "engines": [{"pid": 4242, "created": 999}, {"pid": 4243, "created": 999}]}
    _pin_setup(monkeypatch, foreign, {4242: 0x1001, 4243: 0x1002})
    pinned = []

    def setter(handle, pid, mask):
        if pid == 4242:
            raise PermissionError(5, "access denied")
        pinned.append((pid, mask))

    monkeypatch.setattr(host_lease, "_set_affinity", setter)
    host_lease._refresh_game_affinity()          # must not raise
    assert pinned == [(4243, 0xF)]
    err = capsys.readouterr().err
    assert "skipped pid 4242" in err
    assert err.count("skipped") == 1


def test_still_raises_for_own_engine(monkeypatch):
    """The same refusal on THIS lease's own child is not swallowed."""
    ours = {"pid": os.getpid(), "affinity_mask": 0x3,
            "engines": [{"pid": 4242, "created": 999}]}
    _pin_setup(monkeypatch, ours, {4242: 0x1001})
    monkeypatch.setattr(host_lease, "_set_affinity",
                        lambda handle, pid, mask: (_ for _ in ()).throw(PermissionError(5, "access denied")))
    with pytest.raises(PermissionError):
        host_lease._refresh_game_affinity()


def test_skips_foreign_pid_that_cannot_be_opened(monkeypatch, capsys):
    """OpenProcess refused for a live foreign engine: logged and skipped, not raised, nothing pinned."""
    foreign = {"pid": 31337, "affinity_mask": 0x3,
               "engines": [{"pid": 4242, "created": 999}]}
    _pin_setup(monkeypatch, foreign, {})
    calls = []
    monkeypatch.setattr(host_lease, "_set_affinity", lambda handle, pid, mask: calls.append(pid))
    host_lease._refresh_game_affinity()          # must not raise
    assert calls == []
    assert "skipped pid 4242" in capsys.readouterr().err
