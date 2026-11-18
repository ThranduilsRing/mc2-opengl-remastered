"""Trigger: deploy_payload.py; hardlinked-target replace under a lock, and the --verdict acceptance matrix."""
import json
import os
import re
import sys
from types import SimpleNamespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy_payload as dp
import pytest


@pytest.fixture
def linked(tmp_path):
    pool, dst, src = tmp_path / "pool.dll", tmp_path / "x.dll", tmp_path / "new.dll"
    pool.write_bytes(b"old")
    os.link(pool, dst)
    src.write_bytes(b"new")
    return pool, dst, src


def _refuse_replace(monkeypatch, src, dst, times):
    """os.replace(src, dst) raises PermissionError the first `times` calls."""
    real, left = os.replace, [times]

    def replace(a, b, *args, **k):
        if os.path.abspath(a) == os.path.abspath(src) and os.path.abspath(b) == os.path.abspath(dst) and left[0]:
            left[0] -= 1
            raise PermissionError(13, "in use")
        return real(a, b, *args, **k)
    monkeypatch.setattr(os, "replace", replace)


def test_refused_hardlink_is_renamed_aside_and_pool_untouched(linked, monkeypatch, tmp_path):
    pool, dst, src = linked
    _refuse_replace(monkeypatch, src, dst, 1)
    dp.replace_file(str(src), str(dst))
    assert dst.read_bytes() == b"new"
    assert pool.read_bytes() == b"old" and pool.stat().st_nlink == 1
    assert not list(tmp_path.glob("x.dll.old-*"))


def test_held_aside_is_recorded_and_deleted_by_next_deploy_foreign_survives(linked, monkeypatch, tmp_path):
    pool, dst, src = linked
    foreign = [tmp_path / "mc2.exe.old-1234", tmp_path / "options.cfg.old-2024"]
    for f in foreign:
        f.write_bytes(b"owner")
    _refuse_replace(monkeypatch, src, dst, 1)
    real_unlink = os.unlink
    monkeypatch.setattr(os, "unlink", lambda p, *a, **k: (_ for _ in ()).throw(PermissionError(13, "held")))
    dp.replace_file(str(src), str(dst))
    assert dst.read_bytes() == b"new"
    held = list(tmp_path.glob("x.dll.old-[0-9]*"))
    assert len(held) == 1 and re.search(r"\.old-\d+-\d+$", held[0].name)
    assert json.loads((tmp_path / dp.ASIDE_RECEIPT).read_text()) == [str(held[0])]
    dp.sweep_asides(str(tmp_path))  # still held: stays listed
    assert held[0].exists() and (tmp_path / dp.ASIDE_RECEIPT).exists()
    monkeypatch.setattr(os, "unlink", real_unlink)
    dp.sweep_asides(str(tmp_path))  # the next deploy
    assert not list(tmp_path.glob("x.dll.old-[0-9]*")) and pool.read_bytes() == b"old"
    assert not (tmp_path / dp.ASIDE_RECEIPT).exists()
    assert all(f.read_bytes() == b"owner" for f in foreign)


def test_receipt_cannot_delete_foreign_or_outside_paths(tmp_path):
    inside, other = tmp_path / "keep.txt", tmp_path.parent / f"{tmp_path.name}-outside.old-5"
    inside.write_bytes(b"x")
    other.write_bytes(b"y")
    (tmp_path / dp.ASIDE_RECEIPT).write_text(json.dumps([str(inside), str(other)]))
    dp.sweep_asides(str(tmp_path))
    assert inside.exists() and other.exists()


@pytest.mark.parametrize("shape", ["preset_target", "allow_new_target", "positional"])
def test_v04_publish_needs_a_verdict_in_every_argument_shape(tmp_path, monkeypatch, shape):
    v04 = tmp_path / "v04"
    v04.mkdir()
    monkeypatch.setitem(dp.TARGET_PRESETS["game"], "target_dir", str(v04))
    assert dp.is_v04_game_publish(str(v04), "mc2.exe") and dp.is_v04_game_publish(str(v04), "MC2")
    assert not dp.is_v04_game_publish(str(tmp_path / "other"), "mc2.exe")
    assert not dp.is_v04_game_publish(str(v04), "Mission Editor.exe")
    kw = {"target": "game" if shape == "preset_target" else None, "allow_new_target": shape == "allow_new_target",
          "allow_unverified_exe": False, "verdict": None, "require_clean": False}
    build = tmp_path / "build"
    build.mkdir()
    (build / "mc2.exe").write_bytes(b"exe")
    monkeypatch.setattr(dp, "git_head", lambda *a: HEAD)
    with pytest.raises(SystemExit):
        dp.run_deploy(SimpleNamespace(**kw), str(tmp_path), str(build), str(v04), "mc2.exe", "mc2.pdb")


def test_unverified_flag_on_v04_warns_and_forces_clean(tmp_path, monkeypatch, capsys):
    v04 = tmp_path / "v04"
    v04.mkdir()
    monkeypatch.setitem(dp.TARGET_PRESETS["game"], "target_dir", str(v04))
    monkeypatch.setattr(dp, "git_head", lambda *a: HEAD)
    monkeypatch.setattr(dp, "seed_build_dlls", lambda *a, **k: (_ for _ in ()).throw(RuntimeError("past the gate")))
    args = SimpleNamespace(target="game", allow_new_target=False, allow_unverified_exe=True, verdict=None, require_clean=False)
    with pytest.raises(RuntimeError, match="past the gate"):
        dp.run_deploy(args, str(tmp_path), str(tmp_path), str(v04), "mc2.exe", "mc2.pdb")
    assert args.require_clean and "WARNING: --allow-unverified-exe" in capsys.readouterr().out
    assert json.loads((v04 / ".deployed_manifest.unverified.json").read_text())["allow_unverified_exe"] is True


def test_failed_second_replace_puts_the_old_entry_back(linked, monkeypatch):
    pool, dst, src = linked
    _refuse_replace(monkeypatch, src, dst, 2)
    with pytest.raises(PermissionError):
        dp.replace_file(str(src), str(dst))
    assert dst.read_bytes() == b"old"


def test_rename_failure_names_the_file(linked, monkeypatch):
    pool, dst, src = linked
    monkeypatch.setattr(os, "replace", lambda *a, **k: (_ for _ in ()).throw(PermissionError(13, "x")))
    with pytest.raises(PermissionError, match="hardlinked.*in use by another process"):
        dp.replace_file(str(src), str(dst))
    assert pool.read_bytes() == b"old"


def test_unshared_permission_error_still_raises(tmp_path, monkeypatch):
    dst, src = tmp_path / "x.dll", tmp_path / "new.dll"
    dst.write_bytes(b"old")
    src.write_bytes(b"new")
    _refuse_replace(monkeypatch, src, dst, 1)
    with pytest.raises(PermissionError):
        dp.replace_file(str(src), str(dst))
    assert dst.read_bytes() == b"old"


HEAD = "a" * 40


def _verdict(path, exe, verdict="PASS", head=HEAD, good=True):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"verdict": verdict, "head": head,
                                "artifacts": {"exe_sha256": dp.sha256_file(exe) if good else "0" * 64}}))


@pytest.mark.parametrize("kw,ok", [({}, True), ({"good": False}, False), ({"head": "b" * 40}, False),
                                   ({"verdict": "FAIL"}, False)])
def test_verdict_path_matrix(tmp_path, kw, ok):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"exe")
    vp = tmp_path / "stage" / "gate-verdict.json"
    _verdict(vp, exe, **kw)
    why = dp.verdict_exe_refusal(str(tmp_path / "nowhere"), str(exe), HEAD, str(vp))
    assert (why is None) == ok
    assert ok or "newest" not in why


def test_missing_verdict_path_refuses_even_with_default_pass(tmp_path):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"exe")
    _verdict(tmp_path / "build64-ninja" / "gate-verdict.json", exe)
    assert dp.verdict_exe_refusal(str(tmp_path), str(exe), HEAD) is None
    assert dp.verdict_exe_refusal(str(tmp_path), str(exe), HEAD, str(tmp_path / "none.json"))
