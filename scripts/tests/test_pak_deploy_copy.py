"""ensure_stock_pak: the install always gets a private copy of the cached pak (st_nlink == 1), and a digest mismatch repacks."""
from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402

DATA = b"pak-bytes" * 100


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _setup(monkeypatch, tmp_path, digest):
    cache = tmp_path / "cache" / "k"
    cache.mkdir(parents=True)
    (cache / "stock.mc2pak").write_bytes(DATA)
    (cache / "stock.mc2pak.sha256").write_text(digest)
    monkeypatch.setattr(dp, "PAK_CACHE_ROOT", tmp_path / "cache")
    monkeypatch.setattr(dp, "stock_pak_key", lambda *_: "k")
    monkeypatch.setattr(dp, "_mc2pak_python", lambda: ["py"])
    target = tmp_path / "install"
    target.mkdir()
    return cache, target


def test_pak_is_private_copy_and_second_deploy_skips(monkeypatch, tmp_path):
    cache, target = _setup(monkeypatch, tmp_path, _sha(DATA))
    hashes = {}
    dp.ensure_stock_pak(tmp_path, target, hashes)
    dst = target / dp.PAK_REL
    assert dst.read_bytes() == DATA and dst.stat().st_nlink == 1 and not os.path.samefile(dst, cache / "stock.mc2pak")
    assert hashes[dp.PAK_REL][0] == _sha(DATA)
    assert (target / dp.PAK_STATUS_NAME).read_text().startswith("OK key=k")
    before = dst.stat().st_ino
    copies = []
    real_copy2 = dp.shutil.copy2
    monkeypatch.setattr(dp.shutil, "copy2", lambda *a, **k: copies.append(a) or real_copy2(*a, **k))
    dp.ensure_stock_pak(tmp_path, target, {})
    assert not copies and dst.stat().st_ino == before  # size + digest match and nlink == 1: no recopy (copy2 keeps mtime, so count calls and inode)


def test_existing_hardlink_is_replaced_by_a_copy(monkeypatch, tmp_path):
    cache, target = _setup(monkeypatch, tmp_path, _sha(DATA))
    dst = target / dp.PAK_REL
    dst.parent.mkdir()
    os.link(cache / "stock.mc2pak", dst)
    assert dst.stat().st_nlink == 2
    dp.ensure_stock_pak(tmp_path, target, {})
    assert dst.stat().st_nlink == 1 and (cache / "stock.mc2pak").stat().st_nlink == 1


def test_digest_mismatch_repacks(monkeypatch, tmp_path):
    cache, target = _setup(monkeypatch, tmp_path, "0" * 64)
    packed = b"repacked" * 50
    calls = []

    class Result:
        returncode, stdout, stderr = 0, "", ""

    def fake_run(cmd, **_kw):
        calls.append(cmd)
        Path(cmd[cmd.index("--out") + 1]).write_bytes(packed)
        return Result()

    monkeypatch.setattr(dp.subprocess, "run", fake_run)
    dp.ensure_stock_pak(tmp_path, target, {})
    assert len(calls) == 1
    dst = target / dp.PAK_REL
    assert dst.read_bytes() == packed and dst.stat().st_nlink == 1
    assert (cache / "stock.mc2pak.sha256").read_text() == _sha(packed)


def test_pack_timeout_falls_back_with_status_and_no_exit(monkeypatch, tmp_path):
    cache, target = _setup(monkeypatch, tmp_path, "0" * 64)

    def fake_run(cmd, **kw):
        assert kw["timeout"] == dp.PAK_PACK_TIMEOUT_S
        raise dp.subprocess.TimeoutExpired(cmd, kw["timeout"])

    monkeypatch.setattr(dp.subprocess, "run", fake_run)
    hashes = {}
    dp.ensure_stock_pak(tmp_path, target, hashes)  # returns (rc 0 path): the caller still writes the manifest
    assert dp.PAK_REL not in hashes
    assert (target / dp.PAK_STATUS_NAME).read_text().startswith("FALLBACK mc2pak pack timed out")


def test_pause_stops_the_phase_clock():
    before = dp._paused_seconds()
    with dp.phase_paused():
        with dp.phase_paused():
            pass
        assert dp._PAUSE["depth"] == 1
    assert dp._PAUSE["depth"] == 0 and dp._paused_seconds() >= before
