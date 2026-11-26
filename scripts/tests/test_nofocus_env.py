#!/usr/bin/env python3
"""Every env the harness hands to a spawned game that carries MC2_NO_LAUNCHER must
also carry MC2_LAUNCHER_ENV_JSON=0, or the engine loads launcher_env.json and the
run no longer measures the default configuration.

Run:
  py -3 -m pytest scripts/tests/test_nofocus_env.py -v
"""
from __future__ import annotations

import sys
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from smoke_lib import nofocus  # noqa: E402


@pytest.mark.skipif(sys.platform != "win32", reason="Windows priority inheritance")
@pytest.mark.parametrize("flags", [0, 0x40, 0x80, 0x08008000])
def test_engine_spawn_from_idle_parent_is_below_normal(monkeypatch, flags, capsys):
    import ctypes
    from ctypes import wintypes
    from smoke_lib import exe_guard, host_lease

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.GetPriorityClass.argtypes = [wintypes.HANDLE]
    kernel.GetPriorityClass.restype = wintypes.DWORD
    kernel.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.SetPriorityClass.restype = wintypes.BOOL
    parent = kernel.GetCurrentProcess()
    saved = kernel.GetPriorityClass(parent)
    monkeypatch.setattr(nofocus, "_vet_engine_exe", lambda *_: None)
    monkeypatch.setattr(nofocus, "_hold_host_lease", lambda *_: None)
    monkeypatch.setattr(nofocus, "_record_child", lambda *_: None)
    monkeypatch.setattr(exe_guard, "engine_kind", lambda _: "mc2")
    monkeypatch.setattr(host_lease, "held_device", lambda: None)
    monkeypatch.setattr(host_lease, "apply_affinity", lambda *_: None)
    proc = None
    try:
        assert kernel.SetPriorityClass(parent, 0x40)
        proc = nofocus.spawn([sys.executable, "-c", "import sys; sys.stdin.read()"],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, creationflags=flags)
        assert kernel.GetPriorityClass(int(proc._handle)) == 0x4000
        assert kernel.GetPriorityClass(parent) == 0x40
        assert "class=0x4000 expected=0x4000" in capsys.readouterr().err
    finally:
        assert kernel.SetPriorityClass(parent, saved)
        if proc is not None:
            proc.communicate(timeout=10)


def test_constant_pairs_no_launcher_with_json_kill_switch():
    assert nofocus.NOFOCUS_ENV.get("MC2_NO_LAUNCHER") == "1"
    assert nofocus.NOFOCUS_ENV.get("MC2_LAUNCHER_ENV_JSON") == "0"


@pytest.mark.parametrize("default,expected", [(False, "3"), (True, None)])
def test_engine_default_workers_reaches_child_unset(monkeypatch, default, expected):
    from unittest.mock import Mock
    from smoke_lib import exe_guard, host_lease
    monkeypatch.setattr(nofocus, "_vet_engine_exe", lambda *_: None)
    monkeypatch.setattr(nofocus, "_hold_host_lease", lambda *_: None)
    monkeypatch.setattr(nofocus, "_record_child", lambda *_: None)
    monkeypatch.setattr(exe_guard, "engine_kind", lambda _: "mc2")
    monkeypatch.setattr(host_lease, "held_device", lambda: None)
    monkeypatch.setattr(host_lease, "apply_affinity", lambda *_: None)
    monkeypatch.setattr(host_lease, "frame_jobs_workers", lambda: 3)
    child = Mock(spec=["pid"])
    popen = Mock(return_value=child)
    monkeypatch.setattr(nofocus.subprocess, "Popen", popen)
    nofocus.spawn(["mc2.exe"], env={}, engine_default_workers=default)
    assert popen.call_args.kwargs["env"].get("MC2_FRAME_JOBS_WORKERS") == expected


def test_spawned_env_carries_both(monkeypatch):
    for k in ("MC2_NO_LAUNCHER", "MC2_LAUNCHER_ENV_JSON", "MC2_HARNESS_ALLOW_FOREGROUND"):
        monkeypatch.delenv(k, raising=False)
    env = nofocus.apply_env({})
    assert env["MC2_NO_LAUNCHER"] == "1"
    assert env["MC2_LAUNCHER_ENV_JSON"] == "0"


def test_normal_frontend_retains_saved_settings_and_background_safety(monkeypatch):
    monkeypatch.delenv("MC2_HARNESS_ALLOW_FOREGROUND", raising=False)
    env = nofocus.apply_env({"MC2_LAUNCHER_ENV_JSON": "1", "MC2_WINDOW_OFFSCREEN": "0",
                             "MC2_NO_CURSOR_GRAB": "0"})
    assert env["MC2_LAUNCHER_ENV_JSON"] == "1"
    assert env["MC2_WINDOW_OFFSCREEN"] == env["MC2_NO_CURSOR_GRAB"] == "1"


def test_user_present_session_also_carries_both(monkeypatch):
    monkeypatch.setenv("MC2_HARNESS_ALLOW_FOREGROUND", "1")
    env = nofocus.apply_env({})
    assert env["MC2_NO_LAUNCHER"] == "1"
    assert env["MC2_LAUNCHER_ENV_JSON"] == "0"


def test_tool_capture_preserves_parent_environment(monkeypatch, tmp_path):
    import run_tool_capture

    exe = tmp_path / "mc2_asset_viewer.exe"
    exe.touch()
    monkeypatch.setenv("SystemRoot", "C:/Windows")
    monkeypatch.setenv("TEMP", "parent-temp")
    monkeypatch.setenv("MC2_TOOL_CAPTURE_FRAME", "99")
    monkeypatch.setattr(sys, "argv", ["run_tool_capture", "asset_viewer", "--exe", str(exe),
                                     "--out", str(tmp_path / "capture.png"), "--env", "TEMP=override-temp"])
    monkeypatch.setattr(run_tool_capture.host_lease, "ensure_held", lambda *_: None)

    class SpawnChecked(Exception):
        pass

    def check_spawn(argv, *, env, **kwargs):
        assert {k.upper(): v for k, v in env.items()}["SYSTEMROOT"] == "C:/Windows"
        assert env["TEMP"] == "override-temp"
        assert env["MC2_TOOL_CAPTURE_FRAME"] == "12"
        raise SpawnChecked

    monkeypatch.setattr(run_tool_capture.nofocus, "spawn", check_spawn)
    with pytest.raises(SpawnChecked):
        run_tool_capture.main()
