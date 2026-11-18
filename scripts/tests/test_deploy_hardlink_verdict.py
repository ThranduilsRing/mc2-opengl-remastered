"""Trigger: deploy_payload.py; hardlinked-target replace under a lock, and the --verdict acceptance matrix."""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy_payload as dp
import pytest


def _locked(monkeypatch, victim):
    real = os.replace

    def replace(src, dst, *a, **k):
        if os.path.abspath(dst) == os.path.abspath(victim):
            raise PermissionError(13, "mapped")
        return real(src, dst, *a, **k)
    monkeypatch.setattr(os, "replace", replace)


def test_locked_hardlink_is_renamed_aside_and_pool_untouched(tmp_path, monkeypatch):
    pool, dst, src = tmp_path / "pool.dll", tmp_path / "x.dll", tmp_path / "new.dll"
    pool.write_bytes(b"old")
    os.link(pool, dst)
    src.write_bytes(b"new")
    real = os.replace

    def replace(a, b, *args, **k):
        if os.path.abspath(b) == os.path.abspath(dst) and os.path.abspath(a) == os.path.abspath(src) and dst.exists():
            raise PermissionError(13, "mapped")
        return real(a, b, *args, **k)
    monkeypatch.setattr(os, "replace", replace)
    dp.replace_file(str(src), str(dst))
    assert dst.read_bytes() == b"new"
    assert pool.read_bytes() == b"old" and pool.stat().st_nlink == 1
    assert not list(tmp_path.glob("x.dll.old-*"))


def test_unlink_of_aside_failing_leaves_old_file_for_sweep(tmp_path, monkeypatch):
    pool, dst, src = tmp_path / "pool.dll", tmp_path / "x.dll", tmp_path / "new.dll"
    pool.write_bytes(b"old")
    os.link(pool, dst)
    src.write_bytes(b"new")
    real = os.replace

    def replace(a, b, *args, **k):
        if os.path.abspath(b) == os.path.abspath(dst) and os.path.abspath(a) == os.path.abspath(src) and dst.exists():
            raise PermissionError(13, "mapped")
        return real(a, b, *args, **k)
    monkeypatch.setattr(os, "replace", replace)
    monkeypatch.setattr(os, "unlink", lambda p, *a, **k: (_ for _ in ()).throw(PermissionError(13, "mapped")))
    dp.replace_file(str(src), str(dst))
    left = list(tmp_path.glob("x.dll.old-*"))
    assert len(left) == 1 and dst.read_bytes() == b"new"
    monkeypatch.undo()
    dp.sweep_old_links(str(tmp_path))
    assert not list(tmp_path.glob("x.dll.old-*")) and pool.read_bytes() == b"old"


def test_rename_failure_names_the_file(tmp_path, monkeypatch):
    pool, dst, src = tmp_path / "pool.dll", tmp_path / "x.dll", tmp_path / "new.dll"
    pool.write_bytes(b"old")
    os.link(pool, dst)
    src.write_bytes(b"new")

    def replace(a, b, *args, **k):
        raise PermissionError(13, "mapped")
    monkeypatch.setattr(os, "replace", replace)
    with pytest.raises(PermissionError, match="hardlinked.*running process"):
        dp.replace_file(str(src), str(dst))
    assert pool.read_bytes() == b"old"


def test_unshared_permission_error_still_raises(tmp_path, monkeypatch):
    dst, src = tmp_path / "x.dll", tmp_path / "new.dll"
    dst.write_bytes(b"old")
    src.write_bytes(b"new")
    _locked(monkeypatch, dst)
    with pytest.raises(PermissionError):
        dp.replace_file(str(src), str(dst))
    assert dst.read_bytes() == b"old"


HEAD = "a" * 40


@pytest.mark.parametrize("verdict,head,exe_sha,ok", [
    ("PASS", HEAD, "match", True),
    ("PASS", HEAD, "other", False),
    ("PASS", "b" * 40, "match", False),
    ("FAIL", HEAD, "match", False),
])
def test_verdict_path_matrix(tmp_path, verdict, head, exe_sha, ok):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"exe")
    sha = dp.sha256_file(exe)
    vp = tmp_path / "stage" / "gate-verdict.json"
    vp.parent.mkdir()
    vp.write_text(json.dumps({"verdict": verdict, "head": head,
                              "artifacts": {"exe_sha256": sha if exe_sha == "match" else "0" * 64}}))
    why = dp.verdict_exe_refusal(str(tmp_path / "nowhere"), str(exe), HEAD, str(vp))
    assert (why is None) == ok


def test_verdict_path_missing_refuses(tmp_path):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"exe")
    assert dp.verdict_exe_refusal(str(tmp_path), str(exe), HEAD, str(tmp_path / "none.json"))
