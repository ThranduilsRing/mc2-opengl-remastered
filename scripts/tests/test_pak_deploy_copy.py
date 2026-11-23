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
    monkeypatch.setattr(dp, "pak_defect", lambda *_: None)  # these fixtures are opaque bytes; pak_defect has its own tests
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


# --- an incomplete pak is refused and never replaces the pak that is there (2026-11-22: textures.fst preallocated, 73 MB pak) ---

import json  # noqa: E402
import struct  # noqa: E402
import zipfile  # noqa: E402


def _fst(path: Path, entries: int, zeroed=False) -> None:
    toc = struct.pack("<II", 0xCADDECAF, entries) + b"".join(
        struct.pack("<IIII250s", 8 + entries * 266, 1, 1, 0, f"n{i}".encode()) for i in range(entries))
    path.write_bytes(bytes(len(toc) + 1) if zeroed else toc + b"x")


def _real_install(tmp_path, zeroed_textures=False):
    target = tmp_path / "install"
    target.mkdir()
    (target / "system.cfg").write_text('[FastFiles]\nl NumFastFiles=2\nst File0="a.fst"\nst File1="textures.fst"\n')
    _fst(target / "a.fst", 3)
    _fst(target / "textures.fst", 5, zeroed=zeroed_textures)
    return target


def _pak(path: Path, target: Path, entries_for_textures=5, names=8) -> Path:
    sources = [{"name": n, "size": (target / n).stat().st_size, "entries": c,
                "toc_hash": dp._toc_hash((target / n).read_bytes()[:8 + c * 266])}
               for n, c in (("a.fst", 3), ("textures.fst", entries_for_textures))]
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mc2pak.json", json.dumps({"format": 1, "sources": sources}))
        for i in range(names):
            z.writestr(f"data/{i}.bin", b"x")
    return path


def test_pak_defect_accepts_a_pak_matching_the_installed_fsts(tmp_path):
    target = _real_install(tmp_path)
    assert dp.pak_defect(_pak(tmp_path / "p.mc2pak", target), target) is None


def test_pak_defect_refuses_a_pak_packed_from_a_preallocated_fst(tmp_path):
    target = _real_install(tmp_path)
    pak = _pak(tmp_path / "p.mc2pak", target, entries_for_textures=0, names=4)  # what the 73 MB pak recorded
    assert "textures.fst" in dp.pak_defect(pak, target)
    _fst(target / "textures.fst", 5, zeroed=True)  # the install itself is the unfinished copy
    assert "not a whole FST" in dp.pak_defect(_pak(tmp_path / "q.mc2pak", target), target)


def test_pak_defect_refuses_unreadable_and_too_small_paks(tmp_path):
    target = _real_install(tmp_path)
    junk = tmp_path / "junk.mc2pak"
    junk.write_bytes(b"not a zip")
    assert "unreadable" in dp.pak_defect(junk, target)
    assert "fewer than" in dp.pak_defect(_pak(tmp_path / "s.mc2pak", target, names=2), target)


def test_defective_pack_keeps_the_old_pak_and_is_never_cached(monkeypatch, tmp_path):
    target = _real_install(tmp_path)
    (target / "paks").mkdir()
    old = _pak(target / dp.PAK_REL, target).read_bytes()  # a good kept pak (a bad one is moved aside, tested below)
    monkeypatch.setattr(dp, "PAK_CACHE_ROOT", tmp_path / "cache")
    monkeypatch.setattr(dp, "stock_pak_key", lambda *_: "k")
    monkeypatch.setattr(dp, "_mc2pak_python", lambda: ["py"])
    bad = tmp_path / "bad.mc2pak"
    _pak(bad, target, entries_for_textures=0, names=4)

    class Result:
        returncode, stdout, stderr = 0, "", ""

    def fake_run(cmd, **_kw):
        Path(cmd[cmd.index("--out") + 1]).write_bytes(bad.read_bytes())
        return Result()

    monkeypatch.setattr(dp.subprocess, "run", fake_run)
    hashes = {}
    dp.ensure_stock_pak(tmp_path, target, hashes)
    assert (target / dp.PAK_REL).read_bytes() == old  # the working pak stays
    assert (target / dp.PAK_STATUS_NAME).read_text().startswith("FALLBACK packed pak refused")
    assert hashes[dp.PAK_REL][0] == _sha(old) and hashes[dp.PAK_REL][3] == (target / dp.PAK_REL).stat().st_mtime_ns
    assert not (tmp_path / "cache" / "k" / "stock.mc2pak").exists()


def test_poisoned_cache_entry_is_dropped_not_copied(monkeypatch, tmp_path):
    target = _real_install(tmp_path)
    cache = tmp_path / "cache" / "k"
    cache.mkdir(parents=True)
    _pak(cache / "stock.mc2pak", target, entries_for_textures=0, names=4)
    (cache / "stock.mc2pak.sha256").write_text(dp.sha256_file(cache / "stock.mc2pak"))
    monkeypatch.setattr(dp, "PAK_CACHE_ROOT", tmp_path / "cache")
    monkeypatch.setattr(dp, "stock_pak_key", lambda *_: "k")
    monkeypatch.setattr(dp, "_mc2pak_python", lambda: ["py"])
    dp.ensure_stock_pak(tmp_path, target, {})
    assert not (target / dp.PAK_REL).exists() and not (cache / "stock.mc2pak").exists()
    assert (target / dp.PAK_STATUS_NAME).read_text().startswith("FALLBACK cached pak refused")


def test_retire_unowned_never_prunes_the_stock_pak(tmp_path):
    target = tmp_path / "t"
    (target / "paks").mkdir(parents=True)
    (target / dp.PAK_REL).write_bytes(b"pak")
    dp.write_manifest(str(target), {dp.PAK_REL: (_sha(b"pak"), 3, None, None)}, "a" * 40)
    dp.retire_unowned(str(target), {})
    assert (target / dp.PAK_REL).read_bytes() == b"pak"


# --- review follow-ups 11-22: kept bad pak, source drift, toc_hash, payload publish, v0.4 publish status ---

def _setup_real(monkeypatch, tmp_path):
    target = _real_install(tmp_path)
    monkeypatch.setattr(dp, "PAK_CACHE_ROOT", tmp_path / "cache")
    monkeypatch.setattr(dp, "stock_pak_key", lambda *_: "k")
    monkeypatch.setattr(dp, "_mc2pak_python", lambda: ["py"])
    return target


def _timeout(cmd, **_kw):
    raise dp.subprocess.TimeoutExpired(cmd, 1)


def test_fallback_moves_an_already_bad_kept_pak_aside_and_drops_its_row(monkeypatch, tmp_path):
    target = _setup_real(monkeypatch, tmp_path)
    (target / "paks").mkdir()
    _pak(target / dp.PAK_REL, target, entries_for_textures=0, names=4)  # kept from an earlier bad deploy
    monkeypatch.setattr(dp.subprocess, "run", _timeout)
    hashes = {}
    dp.ensure_stock_pak(tmp_path, target, hashes)
    assert not (target / dp.PAK_REL).exists() and list((target / "paks").glob("stock.mc2pak.defect-*"))
    assert dp.PAK_REL not in hashes and "kept pak refused" in (target / dp.PAK_STATUS_NAME).read_text()


def test_fallback_keeps_a_good_kept_pak(monkeypatch, tmp_path):
    target = _setup_real(monkeypatch, tmp_path)
    (target / "paks").mkdir()
    _pak(target / dp.PAK_REL, target)
    monkeypatch.setattr(dp.subprocess, "run", _timeout)
    hashes = {}
    dp.ensure_stock_pak(tmp_path, target, hashes)
    assert (target / dp.PAK_REL).is_file() and dp.PAK_REL in hashes


def test_pak_defect_refuses_a_toc_hash_mismatch_at_equal_size_and_count(tmp_path):
    target = _real_install(tmp_path)
    pak = _pak(tmp_path / "p.mc2pak", target)
    other = target / "textures.fst"
    data = bytearray(other.read_bytes())
    data[8 + 250] ^= 0xFF  # same size, same entry count, different TOC
    other.write_bytes(bytes(data))
    assert "toc_hash" in dp.pak_defect(pak, target)


def test_a_source_fst_changing_during_the_pack_is_refused(monkeypatch, tmp_path):
    target = _setup_real(monkeypatch, tmp_path)

    class Result:
        returncode, stdout, stderr = 0, "", ""

    def fake_run(cmd, **_kw):
        _pak(Path(cmd[cmd.index("--out") + 1]), target)
        os.utime(target / "textures.fst", ns=(1, 1))  # touched while the pack ran
        return Result()

    monkeypatch.setattr(dp.subprocess, "run", fake_run)
    dp.ensure_stock_pak(tmp_path, target, {})
    assert "changed while packing" in (target / dp.PAK_STATUS_NAME).read_text()
    assert not (tmp_path / "cache" / "k" / "stock.mc2pak").exists()


def test_payload_publish_checks_the_incoming_pak_and_writes_the_status(monkeypatch, tmp_path):
    from test_deploy_stamp_at_hash import _payload
    files = {"mc2.exe": b"exe", dp.PAK_REL: b"bad pak"}
    payload, target = _payload(tmp_path, monkeypatch, files, staged={"mc2.exe"})  # the bad pak is already in the install
    _fst(target / "a.fst", 3)
    (target / "system.cfg").write_text('[FastFiles]\nl NumFastFiles=1\nst File0="a.fst"\n')
    dp.deploy_from_payload(payload, target, tmp_path, "c" * 40)
    assert not (target / dp.PAK_REL).exists() and list((target / "paks").glob("stock.mc2pak.defect-*"))
    assert (target / dp.PAK_STATUS_NAME).read_text().startswith("FALLBACK payload pak refused")
    assert dp.PAK_REL not in (target / dp.MANIFEST_NAME).read_text()


def test_v04_publish_requires_an_ok_pak_status(tmp_path):
    import pytest
    with pytest.raises(ValueError, match="missing"):
        dp.require_pak_ok(tmp_path)
    (tmp_path / dp.PAK_STATUS_NAME).write_text("FALLBACK mc2pak pack failed\n")
    with pytest.raises(ValueError, match="FALLBACK"):
        dp.require_pak_ok(tmp_path)
    (tmp_path / dp.PAK_STATUS_NAME).write_text("OK key=k\n")
    dp.require_pak_ok(tmp_path)
