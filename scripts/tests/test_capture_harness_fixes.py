"""capture-harness-fixes: bookmark name lookup, stage copy retry, short stage root, bookmark parity, engine bookmark-load retry,
affinity ACCESS_DENIED tolerance. Temp dirs and fakes only; no game."""
from __future__ import annotations

import os
import stat
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402
import capture  # noqa: E402
import run_visual_capture as rvc  # noqa: E402
from smoke_lib import host_lease  # noqa: E402


# --- 2. bookmark lookup -----------------------------------------------------------------------------------------------------------
def _exe_with_bookmark(tmp_path, name="mc2_01_saved_205644.json"):
    inst = tmp_path / "inst"
    (inst / "saved_bookmarks").mkdir(parents=True)
    (inst / "saved_bookmarks" / name).write_text("{}")
    (inst / "mc2.exe").write_bytes(b"x")
    return inst / "mc2.exe"


@pytest.mark.parametrize("spec", ["mc2_01_saved_205644", "mc2_01_saved_205644.json", "MC2_01_Saved_205644"])
def test_bookmark_name_resolves_in_install_saved_bookmarks(tmp_path, monkeypatch, spec):
    monkeypatch.setattr(rvc, "BOOKMARK_DIR", tmp_path / "no_such_dir")
    exe = _exe_with_bookmark(tmp_path)
    path, _ = rvc.resolve_bookmarks(spec, "mc2_01", exe)
    assert path is not None and path.name == "mc2_01_saved_205644.json"


def test_bookmark_explicit_path_and_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(rvc, "BOOKMARK_DIR", tmp_path / "bk")
    exe = _exe_with_bookmark(tmp_path)
    f = tmp_path / "x.json"
    f.write_text("{}")
    assert rvc.resolve_bookmarks(str(f), "m", exe)[0] == f
    path, searched = rvc.resolve_bookmarks("nope", "m", exe)
    assert path is None and len(searched) == 2


def test_bookmark_dir_wins_for_mission_default(tmp_path, monkeypatch):
    bk = tmp_path / "bk"
    bk.mkdir()
    (bk / "mc2_01.json").write_text("{}")
    monkeypatch.setattr(rvc, "BOOKMARK_DIR", bk)
    assert rvc.resolve_bookmarks(None, "mc2_01", tmp_path / "mc2.exe")[0] == bk / "mc2_01.json"


# --- 1. stage copy robustness -----------------------------------------------------------------------------------------------------
def test_copy_writable_clears_readonly_bit(tmp_path):
    src = tmp_path / "s.bin"
    src.write_bytes(b"a")
    os.chmod(src, stat.S_IREAD)
    dst = tmp_path / "d.bin"
    ab.copy_writable(src, dst)
    assert dst.read_bytes() == b"a" and os.access(dst, os.W_OK)
    ab.copy_writable(src, dst)  # overwriting a previous read-only copy works too
    os.chmod(src, stat.S_IWRITE)


def test_copy_writable_retries_a_locked_file(tmp_path, monkeypatch):
    import shutil
    src = tmp_path / "s.bin"
    src.write_bytes(b"a")
    real, calls = shutil.copy2, []

    def flaky(s, d):
        calls.append(1)
        if len(calls) < 3:
            raise PermissionError(13, "locked")
        return real(s, d)
    monkeypatch.setattr(shutil, "copy2", flaky)
    monkeypatch.setattr(ab.time, "sleep", lambda s: None)
    ab.copy_writable(src, tmp_path / "d.bin")
    assert len(calls) == 3 and (tmp_path / "d.bin").read_bytes() == b"a"


def test_copy_writable_gives_up_after_tries(tmp_path, monkeypatch):
    import shutil
    src = tmp_path / "s.bin"
    src.write_bytes(b"a")
    monkeypatch.setattr(shutil, "copy2", lambda s, d: (_ for _ in ()).throw(PermissionError(13, "locked")))
    monkeypatch.setattr(ab.time, "sleep", lambda s: None)
    with pytest.raises(PermissionError):
        ab.copy_writable(src, tmp_path / "d.bin", tries=2)


# --- shorter default stage root ---------------------------------------------------------------------------------------------------
def test_stage_root_shortened_only_when_too_long(tmp_path):
    inst = tmp_path / "inst"
    deep = inst / ("d" * 60) / ("e" * 60)
    deep.mkdir(parents=True)
    (deep / ("f" * 60 + ".bin")).write_bytes(b"x")
    long_root = tmp_path / ("l" * 80) / "_ab-stage" / "ab_20261110_094851"
    got = ab.shorten_stage_root(long_root, inst)
    assert got != long_root and len(str(got)) + 1 + ab.longest_install_rel(inst) <= ab.MAX_STAGE_PATH
    assert got.parent.name == "_ab-stage"


def test_stage_root_untouched_when_it_fits(tmp_path):
    inst = tmp_path / "inst"
    inst.mkdir()
    (inst / "mc2.exe").write_bytes(b"x")
    root = tmp_path / "_ab-stage" / "ab"
    assert ab.shorten_stage_root(root, inst) == root


# --- 3. bookmark parity -----------------------------------------------------------------------------------------------------------
def _side(tmp_path, name, files):
    d = tmp_path / name / "saved_bookmarks"
    d.mkdir(parents=True)
    for k, v in files.items():
        (d / k).write_bytes(v)
    return tmp_path / name


def test_bookmark_parity(tmp_path):
    a = _side(tmp_path, "a", {"m1.json": b"1", "m2.json": b"2"})
    b = _side(tmp_path, "b", {"m1.json": b"1", "m2.json": b"changed"})
    assert ab.bookmark_parity_problems(a, a, "m1,m2") == []
    probs = ab.bookmark_parity_problems(a, b, "m1,m2")
    assert len(probs) == 1 and "m2" in probs[0] and "differ" in probs[0]
    c = _side(tmp_path, "c", {"m1.json": b"1"})
    probs = ab.bookmark_parity_problems(a, c, "m1,m2")
    assert len(probs) == 1 and "side B has no" in probs[0]
    assert ab.bookmark_parity_problems(a, c, "absent_everywhere") == []


def test_main_refuses_unseeded_side_before_capturing(tmp_path, monkeypatch):
    a = _side(tmp_path, "a", {"m1.json": b"1"})
    b = _side(tmp_path, "b", {"m1.json": b"2"})
    monkeypatch.setattr(ab, "run_capture", lambda *x, **k: pytest.fail("captured with unequal bookmarks"))
    monkeypatch.setattr(ab, "resolve_installs", lambda args, out: (a, b))
    monkeypatch.setattr(ab, "disk_guard", lambda *x: None)
    with pytest.raises(SystemExit) as ei:
        ab.main(["--bookmarks", "m1", "--install-a", str(a), "--install-b", str(b), "--out-root", str(tmp_path / "o")])
    assert "not seeded identically" in str(ei.value) or "differ" in str(ei.value)


def test_engine_bookmark_load_failure_is_retried_once(tmp_path):
    bmk = tmp_path / "m.json"
    bmk.write_text("{}")
    log = tmp_path / "run.log"
    log.write_text("[VISUAL_CAPTURE v1.5] bookmark load FAILED path=x\n")
    calls = []

    def fake_run_one(**kw):
        calls.append(1)
        if len(calls) == 1:
            return {"bookmarks": [{"present": False}], "log": str(log)}
        return {"bookmarks": [{"present": True}], "log": str(log)}
    msgs = []
    res = capture.run_pose(log=msgs.append, run_one=fake_run_one, bookmarks_path=bmk)
    assert len(calls) == 2 and res["bookmarks"][0]["present"] and "retry 1/1" in msgs[0]


def test_other_missing_poses_are_not_retried(tmp_path):
    bmk = tmp_path / "m.json"
    log = tmp_path / "run.log"
    log.write_text("nothing special\n")
    calls = []
    capture.run_pose(run_one=lambda **kw: calls.append(1) or {"bookmarks": [], "log": str(log)}, bookmarks_path=bmk)
    assert len(calls) == 1


# --- 1. affinity ACCESS_DENIED on a neighbour must not abort the launch ----------------------------------------------------------
@pytest.mark.skipif(sys.platform != "win32", reason="Windows affinity")
def test_refresh_game_affinity_tolerates_access_denied(monkeypatch, capsys):
    monkeypatch.delenv("MC2_NO_CPU_PIN", raising=False)
    monkeypatch.setattr(host_lease, "redirected", lambda: False)
    monkeypatch.setattr(host_lease, "game_masks", lambda: [0x3, 0xC])
    monkeypatch.setattr(host_lease, "_holder_alive", lambda r: True)
    pid = os.getpid()
    created = host_lease.child_registry.process_create_time(pid)
    monkeypatch.setattr(host_lease, "_records", lambda: [(None, {"affinity_mask": 3, "engines": [{"pid": pid, "created": created}]})])

    def denied(handle, pid, mask):
        raise PermissionError(13, "Access is denied.")
    monkeypatch.setattr(host_lease, "_set_affinity", denied)
    host_lease._refresh_game_affinity()
    assert "skipped pid" in capsys.readouterr().err   # host-lease-affinity-skip (W147): one line per skipped foreign pid
