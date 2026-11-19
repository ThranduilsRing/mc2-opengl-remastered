"""Every engine launch (nofocus.spawn, the one chokepoint) carries a private MC2_SAVE_DIR, never the player's savegame folder."""
from __future__ import annotations

import sys
from pathlib import Path
from unittest.mock import Mock

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from smoke_lib import exe_guard, host_lease, nofocus  # noqa: E402

REAL = Path.home() / ".mechcommander2" / "savegame"


@pytest.fixture()
def popen(monkeypatch, tmp_path):
    monkeypatch.setenv("MC2_LANE_SCRATCH", str(tmp_path / "scratch"))
    monkeypatch.delenv("MC2_SAVE_DIR", raising=False)
    monkeypatch.setattr(nofocus, "_vet_engine_exe", lambda *_: None)
    monkeypatch.setattr(nofocus, "_hold_host_lease", lambda *_: None)
    monkeypatch.setattr(nofocus, "_record_child", lambda *_: None)
    monkeypatch.setattr(exe_guard, "engine_kind", lambda a: "mc2" if str(a).endswith("mc2.exe") else None)
    monkeypatch.setattr(host_lease, "held_device", lambda: None)
    monkeypatch.setattr(host_lease, "apply_affinity", lambda *_: None)
    monkeypatch.setattr(host_lease, "frame_jobs_workers", lambda: 0)
    p = Mock(return_value=Mock(spec=["pid"]))
    monkeypatch.setattr(nofocus.subprocess, "Popen", p)
    return p


def test_engine_spawn_without_save_dir_gets_private_one(popen, tmp_path):
    nofocus.spawn(["mc2.exe"], env={})
    d = Path(popen.call_args.kwargs["env"]["MC2_SAVE_DIR"])
    assert d.is_dir() and (tmp_path / "scratch") in d.parents and d != REAL


def test_ambient_env_is_not_the_real_folder(popen, monkeypatch):
    nofocus.spawn(["mc2.exe"])   # env=None: ambient os.environ, no MC2_SAVE_DIR
    assert Path(popen.call_args.kwargs["env"]["MC2_SAVE_DIR"]) != REAL


def test_caller_value_wins(popen, tmp_path):
    nofocus.spawn(["mc2.exe"], env={"MC2_SAVE_DIR": str(tmp_path / "mine")})
    assert popen.call_args.kwargs["env"]["MC2_SAVE_DIR"] == str(tmp_path / "mine")


@pytest.mark.parametrize("bad", ["", str(REAL), str(REAL) + "/", str(REAL).upper(), str(REAL / "sub"), str(REAL.parent), str(REAL.parent.parent)])
def test_empty_or_real_value_is_replaced(popen, bad):
    nofocus.spawn(["mc2.exe"], env={"MC2_SAVE_DIR": bad})
    d = popen.call_args.kwargs["env"]["MC2_SAVE_DIR"]
    assert d and Path(d) != REAL


def test_empty_auto_dir_is_removed_at_cleanup(popen):
    nofocus.spawn(["mc2.exe"], env={})
    d = Path(popen.call_args.kwargs["env"]["MC2_SAVE_DIR"])
    nofocus.cleanup_auto_save_dirs()
    assert not d.exists()


def test_via_menu_launch_is_redirected(popen, tmp_path):
    from smoke_lib import via_menu
    proc = popen.return_value
    proc.stdout = iter(())
    proc.poll = lambda: 0
    via_menu.run_via_menu(tmp_path / "mc2.exe", tmp_path / "out", timeout_s=1, settle_s=0)
    assert Path(popen.call_args.kwargs["env"]["MC2_SAVE_DIR"]) != REAL
    assert popen.call_args.kwargs["env"]["MC2_SAVE_DIR"]


def test_non_engine_child_untouched(popen):
    nofocus.spawn(["python.exe"], env={})
    assert "MC2_SAVE_DIR" not in popen.call_args.kwargs["env"]


def test_cement_check_declares_save_dir_to_capture(monkeypatch, tmp_path):
    sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
    import quickload_cement_check as qcc
    run = Mock(return_value=Mock(returncode=0))
    monkeypatch.setattr(qcc.subprocess, "run", run)
    qcc.capture("", str(tmp_path / "o"), load=False)
    args = run.call_args.args[0]
    assert args[args.index("--env-declare") + 1] == "MC2_SAVE_DIR=" + str(tmp_path / "o" / "savegame")
