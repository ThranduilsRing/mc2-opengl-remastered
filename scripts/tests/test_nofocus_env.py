#!/usr/bin/env python3
"""Every env the harness hands to a spawned game that carries MC2_NO_LAUNCHER must
also carry MC2_LAUNCHER_ENV_JSON=0, or the engine loads launcher_env.json and the
run no longer measures the default configuration.

Run:
  py -3 -m pytest scripts/tests/test_nofocus_env.py -v
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from smoke_lib import nofocus  # noqa: E402


def test_constant_pairs_no_launcher_with_json_kill_switch():
    assert nofocus.NOFOCUS_ENV.get("MC2_NO_LAUNCHER") == "1"
    assert nofocus.NOFOCUS_ENV.get("MC2_LAUNCHER_ENV_JSON") == "0"


def test_spawned_env_carries_both(monkeypatch):
    for k in ("MC2_NO_LAUNCHER", "MC2_LAUNCHER_ENV_JSON", "MC2_HARNESS_ALLOW_FOREGROUND"):
        monkeypatch.delenv(k, raising=False)
    env = nofocus.apply_env({})
    assert env["MC2_NO_LAUNCHER"] == "1"
    assert env["MC2_LAUNCHER_ENV_JSON"] == "0"


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
