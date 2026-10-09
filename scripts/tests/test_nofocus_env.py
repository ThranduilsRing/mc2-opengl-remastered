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
