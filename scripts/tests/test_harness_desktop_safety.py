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
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT))

import console_child  # noqa: E402
from smoke_lib import cursor_guard, exe_guard, nofocus  # noqa: E402
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
    monkeypatch.setattr(nofocus, "park_window_offscreen", lambda *a, **k: None)
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
    monkeypatch.setattr(cursor_guard, "_user32", lambda: fake)
    monkeypatch.setattr(ctypes.windll.kernel32, "GetTickCount", lambda: _NOW_MS, raising=False)
    monkeypatch.setattr(cursor_guard, "_user_active_logged", False)
    return fake


def test_pointer_write_is_skipped_while_user_is_active(monkeypatch, capsys):
    fake = _mock_desktop(monkeypatch, idle_s=1.0)
    assert cursor_guard.user_is_active()
    assert cursor_guard.set_cursor_pos((10, 20)) is False
    assert fake.writes == []
    assert capsys.readouterr().err.count("[CURSOR_GUARD] user active, restore skipped") == 1


def test_skip_line_is_emitted_once_per_run_not_per_sample(monkeypatch, capsys):
    fake = _mock_desktop(monkeypatch, idle_s=0.2)
    for _ in range(50):
        cursor_guard.set_cursor_pos((1, 1))
    assert fake.writes == []
    assert capsys.readouterr().err.count("[CURSOR_GUARD] user active, restore skipped") == 1


def test_pointer_write_proceeds_when_user_is_idle(monkeypatch, capsys):
    fake = _mock_desktop(monkeypatch, idle_s=60.0)
    assert not cursor_guard.user_is_active()
    assert cursor_guard.set_cursor_pos((10, 20)) is True
    assert fake.writes == [(10, 20)]
    assert "user active" not in capsys.readouterr().err


def test_unknown_idle_time_does_not_block_a_headless_restore(monkeypatch):
    monkeypatch.setattr(cursor_guard, "_user32", lambda: None)
    assert cursor_guard.user_idle_seconds() is None
    assert not cursor_guard.user_is_active()


def test_nofocus_and_park_paths_go_through_the_guarded_write(monkeypatch):
    fake = _mock_desktop(monkeypatch, idle_s=1.0)
    assert nofocus.set_cursor_pos(5, 5, restoring=True) is False
    monkeypatch.setattr(cursor_guard, "_PARK_ORIGIN", (7, 7))
    monkeypatch.setattr(cursor_guard, "_PARK_ORIGIN_SAVED", True)
    assert cursor_guard.restore_parked_cursor() is False
    assert fake.writes == []


def test_watchdog_restore_is_inert_while_user_is_active(monkeypatch):
    fake = _mock_desktop(monkeypatch, idle_s=0.5)
    g = cursor_guard.CursorGuard()
    g.anchor = (0, 0)
    assert g._restore_to_anchor() is None
    g._restore()
    assert fake.writes == []


# ------------------------------------------------- MC2_CURSOR_GUARD_OFF (test isolation)

def test_cursor_guard_construction_under_pytest_requires_the_off_switch(monkeypatch):
    monkeypatch.delenv("MC2_CURSOR_GUARD_OFF", raising=False)
    try:
        cursor_guard.CursorGuard()
    except RuntimeError as e:
        assert "MC2_CURSOR_GUARD_OFF" in str(e)
    else:
        raise AssertionError("CursorGuard() under pytest must refuse without MC2_CURSOR_GUARD_OFF=1")


def test_conftest_sets_the_off_switch_for_the_whole_session():
    assert os.environ.get("MC2_CURSOR_GUARD_OFF") == "1"


def test_off_switch_makes_every_entry_point_a_noop_with_no_win32_call(monkeypatch, capsys):
    class _Poison:
        def __getattr__(self, name):
            raise AssertionError(f"win32 call {name} reached with MC2_CURSOR_GUARD_OFF=1")

    monkeypatch.setenv("MC2_CURSOR_GUARD_OFF", "1")
    monkeypatch.setattr(ctypes, "windll", types.SimpleNamespace(user32=_Poison(), kernel32=_Poison()), raising=False)
    monkeypatch.setattr(cursor_guard, "_guard_off_logged", False)
    g = cursor_guard.CursorGuard().start()
    assert g.active is False
    g.set_owner_pid(1)
    g.stop()
    assert cursor_guard.get_cursor_pos() is None
    assert cursor_guard.set_cursor_pos((1, 1)) is False
    assert cursor_guard.unclip_cursor() is False
    assert cursor_guard.get_clip_rect() is None
    assert cursor_guard.find_foreign_game_pids() == []
    assert cursor_guard.park_cursor_if_edge() in ("inert", "disabled")
    assert cursor_guard.restore_parked_cursor() is False
    assert capsys.readouterr().err.count("MC2_CURSOR_GUARD_OFF=1, all pointer and clip calls disabled") == 1


def test_every_win32_entry_raises_under_pytest_without_the_off_switch(monkeypatch):
    monkeypatch.delenv("MC2_CURSOR_GUARD_OFF", raising=False)
    for call in (cursor_guard.get_cursor_pos, cursor_guard.unclip_cursor, cursor_guard.get_clip_rect,
                 cursor_guard.find_foreign_game_pids, lambda: cursor_guard.set_cursor_pos((1, 1))):
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
    assert nofocus.park_window_offscreen(1, force=True) is None


def test_root_pytest_ini_and_conftest_exist_so_any_folder_gets_the_switch():
    assert (ROOT / "pytest.ini").is_file()
    assert "MC2_CURSOR_GUARD_OFF" in (ROOT / "conftest.py").read_text(encoding="utf-8")
