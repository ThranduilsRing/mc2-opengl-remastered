"""Trigger: deploy_payload.py; hardlinked-target replace under a lock, and the --verdict acceptance matrix."""
import json
import os
import sys
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
    dp._ASIDES.clear()
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


def test_held_aside_is_kept_then_retried_and_foreign_old_files_survive(linked, monkeypatch, tmp_path):
    pool, dst, src = linked
    foreign = [tmp_path / "mc2.exe.old-1234", tmp_path / "options.cfg.old-2024"]
    for f in foreign:
        f.write_bytes(b"owner")
    _refuse_replace(monkeypatch, src, dst, 1)
    real_unlink = os.unlink
    monkeypatch.setattr(os, "unlink", lambda p, *a, **k: (_ for _ in ()).throw(PermissionError(13, "held")))
    dp.replace_file(str(src), str(dst))
    assert len(dp._ASIDES) == 1 and dst.read_bytes() == b"new"
    monkeypatch.setattr(os, "unlink", real_unlink)
    dp.retry_asides()
    assert not dp._ASIDES and not list(tmp_path.glob("x.dll.old-[0-9]*")) and pool.read_bytes() == b"old"
    assert all(f.read_bytes() == b"owner" for f in foreign)


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
