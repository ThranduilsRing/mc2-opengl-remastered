#!/usr/bin/env python3
# scripts/tests/test_deploy_staleness.py
"""Tests for deploy_payload.staleness_report (SMOKE-FINGERPRINT-FULL-COVERAGE-1).

Pure filesystem compare against .deployed_manifest.csv — no game launch, no git.
Proves the stale-shader / missing-file / no-manifest / bad-version paths that the
run_smoke default [DEPLOY_STALENESS] advisory relies on.

Run:
  py -3 -m pytest scripts/tests/test_deploy_staleness.py -v
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def _make_target(tmp_path, files: dict, manifest_files=None, src_commit="abc123"):
    """Create a deploy dir with `files` {relpath: content} and a manifest that
    records `manifest_files` (defaults to all files) with their real sha256."""
    d = tmp_path / "target"
    for rel, content in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
    rows = [["manifest_version", dp.MANIFEST_VERSION],
            ["relpath", "sha256", "bytes", "src_commit", "timestamp"]]
    for rel in (manifest_files if manifest_files is not None else list(files)):
        p = d / rel
        h = dp.sha256_file(str(p)) if p.is_file() else "0" * 64
        rows.append([rel, h, str(p.stat().st_size if p.is_file() else 0),
                     src_commit, "2026-06-22T00:00:00"])
    with open(d / dp.MANIFEST_NAME, "w", newline="") as f:
        csv.writer(f).writerows(rows)
    return d


def test_fresh_all_match(tmp_path):
    d = _make_target(tmp_path, {
        "mc2.exe": "exe-bytes",
        "shaders/gos_terrain.frag": "frag-v1",
        "mc2.pdb": "pdb-bytes",
    })
    rep = dp.staleness_report(str(d))
    assert rep["has_manifest"] and rep["version_ok"]
    assert rep["ok"] == 3
    assert rep["stale"] == [] and rep["missing"] == []
    assert rep["src_commit"] == "abc123"


def test_stale_shader_detected(tmp_path):
    d = _make_target(tmp_path, {
        "mc2.exe": "exe-bytes",
        "shaders/gos_terrain.frag": "frag-v1",
    })
    # Edit a shader AFTER the manifest was written -> sha drift.
    (d / "shaders" / "gos_terrain.frag").write_text("frag-v2-EDITED", encoding="utf-8")
    rep = dp.staleness_report(str(d))
    assert rep["stale"] == ["shaders/gos_terrain.frag"]
    assert rep["missing"] == []
    assert rep["ok"] == 1


def test_missing_file_detected(tmp_path):
    d = _make_target(tmp_path, {
        "mc2.exe": "exe-bytes",
        "shaders/gos_terrain.frag": "frag-v1",
    })
    (d / "shaders" / "gos_terrain.frag").unlink()
    rep = dp.staleness_report(str(d))
    assert rep["missing"] == ["shaders/gos_terrain.frag"]
    assert rep["stale"] == []
    assert rep["ok"] == 1


def test_no_manifest(tmp_path):
    d = tmp_path / "bare"
    d.mkdir()
    (d / "mc2.exe").write_text("x", encoding="utf-8")
    rep = dp.staleness_report(str(d))
    assert rep["has_manifest"] is False
    assert rep["stale"] == [] and rep["missing"] == []


def test_bad_manifest_version(tmp_path):
    d = _make_target(tmp_path, {"mc2.exe": "x"})
    with open(d / dp.MANIFEST_NAME, "w", newline="") as f:
        csv.writer(f).writerows([["manifest_version", "v999"],
                                 ["relpath", "sha256", "bytes"]])
    rep = dp.staleness_report(str(d))
    assert rep["has_manifest"] is True
    assert rep["version_ok"] is False


def test_verify_only_still_delegates(tmp_path, capsys):
    # Behavior-preserving: verify_only (non-strict) on a drifted tree still
    # exits 0 and prints the advisory line, now via staleness_report.
    d = _make_target(tmp_path, {"mc2.exe": "x", "shaders/a.frag": "v1"})
    (d / "shaders" / "a.frag").write_text("v2", encoding="utf-8")
    rc = dp.verify_only(str(d), strict=False)
    assert rc == 0
    out = capsys.readouterr().out
    assert "1 stale" in out
    assert "STALE:   shaders/a.frag" in out


def _stamp(d, rel_list=None):
    """Rewrite the fixture manifest with dest_mtime_ns stamps (what write_manifest records)."""
    path = d / dp.MANIFEST_NAME
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    for row in rows[2:]:
        row[:] = row[:5] + [""] + [str((d / row[0]).stat().st_mtime_ns)]
    with open(path, "w", newline="") as f:
        csv.writer(f).writerows(rows)


def _spy_hash(monkeypatch):
    hashed = []
    real = dp.sha256_file
    monkeypatch.setattr(dp, "sha256_file", lambda p: (hashed.append(Path(p).name), real(p))[1])
    return hashed


def test_stamp_match_skips_hash(tmp_path, monkeypatch):
    d = _make_target(tmp_path, {"a.txt": "aaa", "b.txt": "bbb"})
    _stamp(d)
    hashed = _spy_hash(monkeypatch)
    rep = dp.staleness_report(str(d))
    assert hashed == [] and rep["ok"] == 2 and rep["skipped"] == 2 and rep["stale"] == []


def test_size_change_is_hashed_and_stale(tmp_path, monkeypatch):
    import os
    d = _make_target(tmp_path, {"a.txt": "aaa", "b.txt": "bbb"})
    _stamp(d)
    st = (d / "a.txt").stat()
    (d / "a.txt").write_text("aaaa-longer", encoding="utf-8")
    os.utime(d / "a.txt", ns=(st.st_atime_ns, st.st_mtime_ns))  # same mtime, new size
    hashed = _spy_hash(monkeypatch)
    rep = dp.staleness_report(str(d))
    assert hashed == ["a.txt"] and rep["stale"] == ["a.txt"] and rep["ok"] == 1


def test_touched_same_bytes_is_fresh(tmp_path, monkeypatch):
    import os
    d = _make_target(tmp_path, {"a.txt": "aaa"})
    _stamp(d)
    st = (d / "a.txt").stat()
    os.utime(d / "a.txt", ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
    hashed = _spy_hash(monkeypatch)
    rep = dp.staleness_report(str(d))
    assert hashed == ["a.txt"] and rep["stale"] == [] and rep["ok"] == 1


def test_old_manifest_full_hash_with_notice(tmp_path, monkeypatch):
    d = _make_target(tmp_path, {"a.txt": "aaa", "b.txt": "bbb"})  # no dest_mtime_ns column
    hashed = _spy_hash(monkeypatch)
    notes = []
    rep = dp.staleness_report(str(d), notice=notes.append)
    assert sorted(hashed) == ["a.txt", "b.txt"] and rep["ok"] == 2
    assert len(notes) == 1 and "dest_mtime_ns" in notes[0]


def test_full_flag_hashes_despite_stamp(tmp_path, monkeypatch):
    d = _make_target(tmp_path, {"a.txt": "aaa"})
    _stamp(d)
    hashed = _spy_hash(monkeypatch)
    rep = dp.staleness_report(str(d), full=True)
    assert hashed == ["a.txt"] and rep["ok"] == 1


def test_write_manifest_records_dest_stamp(tmp_path):
    d = tmp_path / "t"
    d.mkdir()
    (d / "a.txt").write_text("aaa", encoding="utf-8")
    digest, size, stamp = dp.hash_with_stamp(str(d / "a.txt"), dp.sha256_file)
    dp.write_manifest(str(d), {"a.txt": (digest, size, 7, stamp)}, "deadbeef" * 5)
    row = list(csv.reader(open(d / dp.MANIFEST_NAME)))[2]
    assert row[6] == str((d / "a.txt").stat().st_mtime_ns)
    assert dp.staleness_report(str(d))["skipped"] == 1


def test_write_manifest_never_stamps_a_row_that_has_no_stamp(tmp_path):
    d = tmp_path / "t"
    d.mkdir()
    (d / "a.txt").write_text("aaa", encoding="utf-8")
    dp.write_manifest(str(d), {"a.txt": ("0" * 64, 3, 7), "b.txt": ("1" * 64, 3, 7, None)}, "deadbeef" * 5)
    rows = list(csv.reader(open(d / dp.MANIFEST_NAME)))[2:]
    assert [r[6] for r in rows] == ["", ""]  # the file exists, but no stat is taken at write time


def test_carried_rows_keep_earlier_stamp_not_a_new_one(tmp_path):
    d = _make_target(tmp_path, {"a.txt": "aaa"})
    _stamp(d)
    old = int(list(csv.reader(open(d / dp.MANIFEST_NAME)))[2][6])
    (d / "a.txt").write_text("changed-after-hash", encoding="utf-8")  # bytes change; the carried hash does not
    rows = dp.carry_rows(dp.read_manifest(str(d)), str(d))
    assert rows["a.txt"][3] == old
    dp.write_manifest(str(d), rows, "deadbeef" * 5)
    rep = dp.staleness_report(str(d))
    assert rep["stale"] == ["a.txt"] and rep["skipped"] == 0
