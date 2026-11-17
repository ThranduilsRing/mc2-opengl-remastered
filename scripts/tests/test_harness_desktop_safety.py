#!/usr/bin/env python3
"""The smoke/capture harness must never put a window or a pointer move on the user's desktop.

Owner incident 2026-10-10: a tier-1 smoke fell onto the golden install (an exe that predates
the background-launch fix) and came up fullscreen, while cursor restores fought the user for
the mouse. These tests pin the fixes with NO launch: a fake manifest and fake exe files, a
mocked user32, and run_smoke.py invoked only up to its refusal (or --resolve-only).

Run:
  py -3 -m pytest scripts/tests/test_harness_desktop_safety.py -v
"""
from __future__ import annotations

import csv
import ctypes
import os
import subprocess
import sys
import types
import pytest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT))

import console_child  # noqa: E402
from smoke_lib import exe_guard, nofocus, win_input  # noqa: E402
from scripts.smoke_lib import deploy_lease  # noqa: E402

STALE_SHA = "d662802367d8a5b6cfc1ef03a38053f61a1bd598"
FRESH_SHA = "3f06e16200342dbc00df971e4ed8ede893076697"


def _head():
    return subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


def _install(tmp_path, name, sha, with_mark=True):
    d = tmp_path / name
    d.mkdir()
    mark = exe_guard.OFFSCREEN_MARK if with_mark else b""
    (d / "mc2.exe").write_bytes(b"MZ" + b"\0" * 100 + mark + b"\0" * 50)
    with open(d / ".deployed_manifest.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["manifest_version", "v1", "", "", ""])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp"])
        if sha:
            w.writerow(["mc2.exe", "00", "1", sha, "2026-10-10T00:00:00"])
    return d / "mc2.exe"


def _ancestor_of(descendants):
    return lambda base, sha: sha in descendants


# ---------------------------------------------------------------- exe guard

def _fresh(sha=FRESH_SHA):
    return lambda p: (sha, "")


def _no_fingerprint(p):
    return None, "banner-absent: no [BUILD_FINGERPRINT v1] literal"


def test_read_src_commit_from_manifest(tmp_path):
    exe = _install(tmp_path, "mc2-win64-x", FRESH_SHA)
    assert exe_guard.read_src_commit(exe.parent) == FRESH_SHA
    assert exe_guard.read_src_commit(tmp_path) is None


def test_pre_imgui_pointer_fix_exe_is_refused(tmp_path):
    old = "b4fe64851"
    exe = _install(tmp_path, "pre-fix", old)
    verdict = exe_guard.check_exe(exe, env={}, commit_of=_fresh(old))
    assert not verdict.ok and exe_guard.MIN_COMMIT in verdict.refusal()


def test_current_exe_with_descendant_embedded_commit_passes(tmp_path):
    exe = _install(tmp_path, "mc2-win64-x", FRESH_SHA)
    v = exe_guard.check_exe(exe, env={}, is_ancestor=_ancestor_of({FRESH_SHA}), commit_of=_fresh())
    assert v.ok, v.reasons
    assert v.banner() == f"[SMOKE] exe={exe.resolve()} src_commit={FRESH_SHA}"


def test_ancestry_uses_the_commit_embedded_in_the_exe_not_the_deploy_manifest(tmp_path):
    # deploy_payload records git HEAD at deploy time: a stale exe copied from a current tree has a fresh manifest.
    exe = _install(tmp_path, "mc2-win64-x", FRESH_SHA)
    v = exe_guard.check_exe(exe, env={}, is_ancestor=_ancestor_of({FRESH_SHA}), commit_of=_fresh(STALE_SHA))
    assert not v.ok
    assert STALE_SHA in v.refusal() and v.manifest_commit == FRESH_SHA


def test_exe_without_an_embedded_fingerprint_is_refused_whatever_the_manifest_says(tmp_path):
    exe = _install(tmp_path, "mc2-win64-x", FRESH_SHA)
    v = exe_guard.check_exe(exe, env={}, is_ancestor=_ancestor_of({FRESH_SHA}), commit_of=_no_fingerprint)
    assert not v.ok and "fingerprint" in v.refusal() and "mc2-win64-x" in v.refusal()


def test_exe_without_offscreen_capability_is_refused_naming_install_and_commit(tmp_path):
    exe = _install(tmp_path, "mc2-win64-x", FRESH_SHA, with_mark=False)
    v = exe_guard.check_exe(exe, env={}, is_ancestor=_ancestor_of({FRESH_SHA}), commit_of=_fresh())
    assert not v.ok
    msg = v.refusal()
    assert "mc2-win64-x" in msg and FRESH_SHA in msg and "MC2_WINDOW_OFFSCREEN" in msg


def test_exe_built_before_the_fix_commit_is_refused(tmp_path):
    exe = _install(tmp_path, "mc2-win64-x", STALE_SHA)
    v = exe_guard.check_exe(exe, env={}, is_ancestor=_ancestor_of(set()), commit_of=_fresh(STALE_SHA))
    assert not v.ok
    assert STALE_SHA in v.refusal() and exe_guard.MIN_COMMIT in v.refusal()


def test_dirty_suffix_is_stripped_for_the_ancestry_test(tmp_path):
    exe = _install(tmp_path, "mc2-win64-x", FRESH_SHA)
    v = exe_guard.check_exe(exe, env={}, is_ancestor=_ancestor_of({FRESH_SHA}), commit_of=_fresh(FRESH_SHA + "-dirty"))
    assert v.ok, v.reasons
    assert v.src_commit.endswith("-dirty")


def test_editor_exe_is_judged_by_the_offscreen_capability_alone(tmp_path):
    d = tmp_path / "mc2-win64-x"
    d.mkdir()
    (d / "Mission Editor.exe").write_bytes(b"MZ" + exe_guard.OFFSCREEN_MARK)
    assert exe_guard.engine_kind(d / "Mission Editor.exe") == "editor"
    assert exe_guard.check_exe(d / "Mission Editor.exe", env={}, commit_of=_no_fingerprint).ok
    (d / "EditRel.exe").write_bytes(b"MZ")
    assert not exe_guard.check_exe(d / "EditRel.exe", env={}, commit_of=_no_fingerprint).ok


def test_owner_override_waives_staleness_only(tmp_path):
    old = _install(tmp_path, "mc2-win64-x", STALE_SHA, with_mark=False)
    v = exe_guard.check_exe(old, env={"MC2_OWNER_VISIBLE_OK": "1"}, is_ancestor=_ancestor_of(set()),
                            commit_of=_no_fingerprint)
    assert v.ok
    golden = _install(tmp_path, "mc2-win64-v0.4c", FRESH_SHA)
    v = exe_guard.check_exe(golden, env={"MC2_OWNER_VISIBLE_OK": "1"}, is_ancestor=_ancestor_of({FRESH_SHA}),
                            commit_of=_fresh())
    assert not v.ok and "golden" in v.refusal()


def test_golden_install_is_never_a_smoke_target_even_when_current(tmp_path):
    golden = _install(tmp_path, "mc2-win64-v0.4c", FRESH_SHA)
    anc = _ancestor_of({FRESH_SHA})
    assert not exe_guard.check_exe(golden, purpose="smoke", env={}, is_ancestor=anc, commit_of=_fresh()).ok
    assert exe_guard.check_exe(golden, purpose="capture", env={}, is_ancestor=anc, commit_of=_fresh()).ok


def test_nofocus_spawn_and_run_refuse_a_stale_engine_exe_before_any_process_is_created(tmp_path, monkeypatch):
    exe = _install(tmp_path, "mc2-win64-stale", STALE_SHA, with_mark=False)

    def _boom(*a, **k):
        raise AssertionError("a process was created for a refused exe")

    monkeypatch.setattr(subprocess, "Popen", _boom)
    monkeypatch.setattr(subprocess, "run", _boom)
    for launch in (nofocus.spawn, nofocus.run):
        try:
            launch([str(exe), "--mission", "m"])
        except nofocus.ExeRefused as e:
            assert "mc2-win64-stale" in str(e)
        else:
            raise AssertionError("expected ExeRefused")


def test_nofocus_does_not_vet_non_engine_programs(monkeypatch):
    seen = []
    monkeypatch.setattr(subprocess, "Popen",
                        lambda argv, **k: seen.append(argv) or types.SimpleNamespace(pid=1, poll=lambda: 0))
    nofocus.spawn([sys.executable, "-c", "pass"])
    assert seen


def test_marker_found_across_read_chunk_boundary(tmp_path):
    p = tmp_path / "mc2.exe"
    p.write_bytes(b"a" * 13 + exe_guard.OFFSCREEN_MARK + b"b" * 7)
    assert exe_guard.exe_has_offscreen(p, chunk=16)
    p.write_bytes(b"a" * 40)
    assert not exe_guard.exe_has_offscreen(p, chunk=16)


def test_real_git_ancestry_of_fix_commit():
    assert exe_guard.git_is_ancestor(exe_guard.MIN_COMMIT, "HEAD") is True
    assert exe_guard.git_is_ancestor(exe_guard.MIN_COMMIT, STALE_SHA) is False


# ------------------------------------------------- run_smoke / capture (no launch)

def _smoke(args, env_extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MC2_OWNER_")}
    env.update(env_extra or {})
    return subprocess.run([sys.executable, str(ROOT / "scripts" / "run_smoke.py"), *args],
                          capture_output=True, text=True, timeout=120, env=env,
                          **console_child.child_kwargs(redirected=True))


def test_run_smoke_refuses_an_unfingerprinted_exe_and_names_the_install(tmp_path):
    exe = _install(tmp_path, "mc2-win64-stale", STALE_SHA, with_mark=False)
    r = _smoke(["--lite", "--no-lease", "--exe", str(exe), "--resolve-only"], {"MC2_OWNER_NO_LEASE": "1"})
    assert r.returncode == 7, r.stderr
    assert f"[SMOKE] exe={exe.resolve()} src_commit=unknown" in r.stderr
    assert "mc2-win64-stale" in r.stderr and "fingerprint" in r.stderr


def test_headless_smoke_arguments_resolve_without_a_launch(tmp_path):
    # Was lane_merge --pre-gate's "headless-smoke" row (deleted with the merge queue): argument resolution is an ordinary tool test.
    head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()   # a real commit: the guard proves ancestry
    exe = _install(tmp_path, "mc2-win64-lane", head)
    r = _smoke(["--headless", "--ticks", "10", "--no-lease", "--exe", str(exe), "--resolve-only"], {"MC2_OWNER_NO_LEASE": "1"})
    # The headless arguments parse and resolve to the named install (argparse would exit 2); the fake exe then stops at the guard.
    assert r.returncode != 2 and f"[SMOKE] exe={exe.resolve()}" in r.stderr, r.stderr


def test_run_smoke_no_lease_is_owner_only(tmp_path):
    exe = _install(tmp_path, "mc2-win64-ok", FRESH_SHA)
    r = _smoke(["--no-lease", "--exe", str(exe), "--mission", "mc2_01", "--resolve-only"])
    assert r.returncode == 6
    assert "MC2_OWNER_NO_LEASE" in r.stderr and "--clear-dead" in r.stderr


def test_tier1_never_picks_an_install_on_its_own():
    r = _smoke(["--tier", "tier1", "--resolve-only"])
    assert r.returncode == 2
    assert "explicit install" in r.stderr


def test_run_smoke_refuses_golden_for_smoke(tmp_path):
    exe = _install(tmp_path, "mc2-win64-v0.4c", FRESH_SHA)
    r = _smoke(["--lite", "--no-lease", "--exe", str(exe), "--resolve-only"],
               {"MC2_OWNER_NO_LEASE": "1", "MC2_OWNER_VISIBLE_OK": "1"})
    assert r.returncode == 7 and "golden" in r.stderr


def test_capture_refuses_a_stale_exe_before_any_launch(tmp_path):
    exe = _install(tmp_path, "mc2-win64-stale", STALE_SHA, with_mark=False)
    (exe.parent / "saved_bookmarks").mkdir()
    (exe.parent / "saved_bookmarks" / "a.json").write_text("{}")
    env = {k: v for k, v in os.environ.items() if not k.startswith("MC2_OWNER_")}
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "visual_verify" / "capture.py"),
                        "--install", str(exe.parent), "--out", str(tmp_path / "out")],
                       capture_output=True, text=True, timeout=120, env=env,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode == 7, r.stderr
    assert "mc2-win64-stale" in r.stderr and "Traceback" not in r.stderr


# ------------------------------------------------------------- lease selection

def test_unattended_lease_walk_excludes_the_golden_install():
    assert deploy_lease._WALK_LANES == ("0.4",)


def test_clear_dead_removes_only_dead_owners_on_this_host(tmp_path, monkeypatch):
    monkeypatch.setattr(deploy_lease, "LEASE_REGISTRY_PATH", tmp_path / "leases.json")
    host = "H"
    deploy_lease._write_registry({
        "a": {"pid": 1, "hostname": host},
        "b": {"pid": 2, "hostname": host},
        "c": {"pid": 1, "hostname": "OTHER"},
    })
    gone = deploy_lease.clear_dead_leases(pid_alive=lambda pid: pid == 2, hostname=host)
    assert gone == ["a"]
    assert set(deploy_lease._read_registry()) == {"b", "c"}


# --------------------------------------------------------------- cursor guard

_NOW_MS = 1_000_000


class _FakeUser32:
    """user32 stand-in: idle time is scripted, every pointer write is recorded."""

    def __init__(self, idle_s):
        self.idle_ms = int(idle_s * 1000)
        self.writes = []
        setattr(self, "Set" + "CursorPos", self._write)

    def _write(self, x, y):
        self.writes.append((x, y))
        return 1

    def GetLastInputInfo(self, ref):
        ref._obj.dwTime = (_NOW_MS - self.idle_ms) & 0xFFFFFFFF
        return 1

    def GetCursorPos(self, ref):
        return 0


def _mock_desktop(monkeypatch, idle_s):
    fake = _FakeUser32(idle_s)
    monkeypatch.setattr(win_input, "_user32", lambda: fake)
    monkeypatch.setattr(ctypes.windll.kernel32, "GetTickCount", lambda: _NOW_MS, raising=False)
    return fake


@pytest.mark.skipif(os.name != "nt", reason="Windows user32 cursor and idle-time APIs")
def test_pointer_write_is_skipped_while_user_is_active(monkeypatch):
    for idle_s in (0.0, 1.0, 4.999):
        fake = _mock_desktop(monkeypatch, idle_s=idle_s)
        assert win_input.user_is_active()
        assert win_input.set_cursor_pos((10, 20)) is False
        assert fake.writes == []


@pytest.mark.skipif(os.name != "nt", reason="Windows user32 cursor and idle-time APIs")
def test_many_attempts_while_the_user_is_active_write_nothing(monkeypatch):
    fake = _mock_desktop(monkeypatch, idle_s=0.2)
    for _ in range(50):
        win_input.set_cursor_pos((1, 1))
    assert fake.writes == []


@pytest.mark.skipif(os.name != "nt", reason="Windows user32 cursor and idle-time APIs")
def test_pointer_write_proceeds_when_user_is_idle(monkeypatch):
    fake = _mock_desktop(monkeypatch, idle_s=60.0)
    assert not win_input.user_is_active()
    assert win_input.set_cursor_pos((10, 20)) is True
    assert fake.writes == [(10, 20)]


def test_unknown_idle_time_blocks_pointer_writes(monkeypatch):
    monkeypatch.setattr(win_input, "_user32", lambda: None)
    assert win_input.user_idle_seconds() is None
    assert win_input.user_is_active()


@pytest.mark.skipif(os.name != "nt", reason="Windows user32 cursor and idle-time APIs")
def test_failed_idle_query_never_writes_pointer(monkeypatch):
    fake = _mock_desktop(monkeypatch, idle_s=60.0)
    monkeypatch.setattr(fake, "GetLastInputInfo", lambda ref: 0)
    assert not win_input.set_cursor_pos((10, 20))
    assert fake.writes == []


def test_agent_launch_cannot_override_pointer_safety(monkeypatch):
    monkeypatch.delenv("MC2_HARNESS_ALLOW_FOREGROUND", raising=False)
    env = nofocus.apply_env({"MC2_NO_CURSOR_GRAB": "0", "MC2_WINDOW_OFFSCREEN": "0",
                             "MC2_WINDOWED": "0", "MC2_WIDTH": "1600"})
    assert env["MC2_NO_CURSOR_GRAB"] == "1"
    assert env["MC2_WINDOW_OFFSCREEN"] == "1"
    assert env["MC2_WINDOWED"] == "1"
    assert env["MC2_WIDTH"] == "1600"


@pytest.mark.skipif(os.name != "nt", reason="Windows user32 cursor API")
def test_nofocus_pointer_write_goes_through_the_guarded_write(monkeypatch):
    fake = _mock_desktop(monkeypatch, idle_s=1.0)
    assert nofocus.set_cursor_pos(5, 5, restoring=True) is False
    assert fake.writes == []


# ------------------------------------------------- MC2_CURSOR_GUARD_OFF (test isolation)

def test_conftest_sets_the_off_switch_for_the_whole_session():
    assert os.environ.get("MC2_CURSOR_GUARD_OFF") == "1"


def test_off_switch_makes_every_entry_point_a_noop_with_no_win32_call(monkeypatch):
    class _Poison:
        def __getattr__(self, name):
            raise AssertionError(f"win32 call {name} reached with MC2_CURSOR_GUARD_OFF=1")

    monkeypatch.setenv("MC2_CURSOR_GUARD_OFF", "1")
    monkeypatch.setattr(ctypes, "windll", types.SimpleNamespace(user32=_Poison(), kernel32=_Poison()), raising=False)
    assert win_input.get_cursor_pos() is None
    assert win_input.set_cursor_pos((1, 1)) is False
    assert win_input.find_foreign_game_pids() == []


@pytest.mark.skipif(os.name != "nt", reason="Tripwire for Windows-only input entry points")
def test_every_win32_entry_raises_under_pytest_without_the_off_switch(monkeypatch):
    monkeypatch.delenv("MC2_CURSOR_GUARD_OFF", raising=False)
    for call in (win_input.get_cursor_pos, win_input.find_foreign_game_pids, lambda: win_input.set_cursor_pos((1, 1))):
        try:
            call()
        except RuntimeError as e:
            assert "MC2_CURSOR_GUARD_OFF" in str(e)
        else:
            raise AssertionError("expected the pytest tripwire")


def test_nofocus_window_calls_are_inert_with_the_off_switch(monkeypatch):
    monkeypatch.setenv("MC2_HARNESS_ALLOW_FOREGROUND", "1")
    monkeypatch.setenv("MC2_CURSOR_GUARD_OFF", "1")
    assert nofocus.raise_noactivate(12345) is False
    assert not hasattr(nofocus, "park_window_offscreen")


def test_root_pytest_ini_and_conftest_exist_so_any_folder_gets_the_switch():
    assert (ROOT / "pytest.ini").is_file()
    assert "MC2_CURSOR_GUARD_OFF" in (ROOT / "conftest.py").read_text(encoding="utf-8")


# ------------------------------------------------- kills: only orphans of a dead harness run

from smoke_lib import child_registry  # noqa: E402


def _rec(**kw):
    base = {"pid": 10, "exe": "a:/x/mc2.exe", "runner_pid": 99, "create_time": 123}
    base.update(kw)
    return base


def _may(rec, alive=lambda p: p == 10, created=lambda p: 123, image=lambda p: "a:/x/mc2.exe"):
    return child_registry.may_kill(10, alive=alive, created=created, image=image, rec=rec)


def test_orphan_of_a_dead_runner_may_be_ended():
    ok, why = _may(_rec())
    assert ok, why


def test_a_game_with_no_record_is_never_killed():
    ok, why = child_registry.may_kill(2_000_000_000)
    assert not ok and "no record" in why


def test_live_runner_reused_pid_or_other_image_blocks_the_kill():
    assert "runner" in _may(_rec(), alive=lambda p: True)[1]
    assert "creation time" in _may(_rec(), created=lambda p: 999)[1]
    assert "image path" in _may(_rec(), image=lambda p: "a:/other/mc2.exe")[1]
    assert not _may(_rec(), alive=lambda p: False)[0]


@pytest.mark.skipif(os.name != "nt", reason="Windows os.kill uses TerminateProcess; POSIX uses signal zero")
def test_pid_alive_never_signals_on_windows(monkeypatch):
    def _boom(*a, **k):
        raise AssertionError("os.kill is TerminateProcess on Windows; liveness must not call it")

    monkeypatch.setattr(os, "kill", _boom)
    assert child_registry.pid_alive(os.getpid()) is True
    assert child_registry.pid_alive(2_000_000_000) is False


def test_nofocus_records_only_engine_children(tmp_path, monkeypatch):
    from smoke_lib import host_lease

    seen = []
    pinned = []
    monkeypatch.setattr(host_lease, "apply_affinity", lambda proc: pinned.append(proc.pid))
    monkeypatch.setattr(child_registry, "record", lambda pid, exe, **k: seen.append((pid, str(exe))))
    monkeypatch.setattr(subprocess, "Popen", lambda argv, **k: types.SimpleNamespace(pid=7, poll=lambda: 0))
    nofocus.spawn([sys.executable, "-c", "pass"])
    assert seen == []
    exe = _install(tmp_path, "mc2-win64-ok", FRESH_SHA)
    monkeypatch.setattr(nofocus, "_vet_engine_exe", lambda argv: None)
    nofocus.spawn([str(exe)])
    assert seen == [(7, str(exe))]
    assert pinned == [7]


# ------------------------------------------------- synthetic input needs a user-present session

@pytest.mark.skipif(os.name != "nt", reason="Windows user32 keyboard input")
def test_game_auto_key_and_chord_do_nothing_in_a_background_session(monkeypatch, capsys):
    import game_auto

    class _Boom:
        def __getattr__(self, name):
            raise AssertionError(f"input injected: {name}")

    monkeypatch.delenv("MC2_HARNESS_ALLOW_FOREGROUND", raising=False)
    monkeypatch.setattr(game_auto, "user32", _Boom())
    monkeypatch.setattr(game_auto, "get_pyautogui", lambda: _Boom())
    game_auto.do_key("escape")
    game_auto.do_chord("ctrl+s")
    assert capsys.readouterr().out.count("skipped (background mode") == 2


@pytest.mark.skipif(os.name != "nt", reason="Windows user32 keyboard input")
def test_game_auto_keys_are_sent_when_the_session_opts_in(monkeypatch):
    import game_auto

    sent = []
    fake = types.SimpleNamespace(keybd_event=lambda *a: sent.append(a))
    monkeypatch.setenv("MC2_HARNESS_ALLOW_FOREGROUND", "1")
    monkeypatch.setattr(game_auto, "user32", fake)
    monkeypatch.setattr(game_auto.time, "sleep", lambda s: None)
    game_auto.do_key("escape")
    assert len(sent) == 2


def test_menu_canary_is_refused_without_the_foreground_opt_in(monkeypatch):
    from scripts import run_smoke

    monkeypatch.delenv("MC2_HARNESS_ALLOW_FOREGROUND", raising=False)
    assert "MC2_HARNESS_ALLOW_FOREGROUND" in run_smoke._menu_canary_refusal()
    assert run_smoke._run_menu_canary(Path("x"), Path("y"), Path("z"), False, 0) == 5
    monkeypatch.setenv("MC2_HARNESS_ALLOW_FOREGROUND", "1")
    assert run_smoke._menu_canary_refusal() is None


# ------------------------------------------------- renderdoc: launcher argv and the Qt GUI

def test_vetting_covers_every_argument_so_a_launcher_cannot_hide_the_engine(tmp_path, monkeypatch):
    exe = _install(tmp_path, "mc2-win64-stale", STALE_SHA, with_mark=False)
    monkeypatch.setattr(subprocess, "run", lambda *a, **k: (_ for _ in ()).throw(AssertionError("launched")))
    monkeypatch.setattr(subprocess, "Popen", lambda *a, **k: (_ for _ in ()).throw(AssertionError("launched")))
    try:
        nofocus.run(["renderdoccmd.exe", "capture", "-w", str(exe), "--mission", "m"])
    except nofocus.ExeRefused as e:
        assert "mc2-win64-stale" in str(e)
    else:
        raise AssertionError("expected ExeRefused for the engine inside a launcher argv")


def test_golden_install_is_refused_for_spawn_as_well_as_smoke(tmp_path):
    golden = _install(tmp_path, "mc2-win64-v0.4c", FRESH_SHA)
    anc = _ancestor_of({FRESH_SHA})
    assert not exe_guard.check_exe(golden, purpose="spawn", env={}, is_ancestor=anc, commit_of=_fresh()).ok


def test_qrenderdoc_worker_needs_a_user_present_session(monkeypatch):
    sys.path.insert(0, str(ROOT / "tools" / "renderdoc"))
    import _rdcommon

    monkeypatch.delenv("MC2_HARNESS_ALLOW_FOREGROUND", raising=False)
    try:
        _rdcommon.run_worker({"result_json": "x"})
    except SystemExit as e:
        assert "MC2_HARNESS_ALLOW_FOREGROUND" in str(e)
    else:
        raise AssertionError("expected SystemExit")
