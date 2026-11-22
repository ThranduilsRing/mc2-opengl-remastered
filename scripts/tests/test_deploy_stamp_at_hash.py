"""dest_mtime_ns is the stamp taken WHEN a row's hash is taken: never at manifest-write time, never for bytes that changed after
the hash, never inherited unverified (deploy_payload.hash_with_stamp / verified_stamp / restamp_rewritten / write_manifest)."""
from __future__ import annotations

import csv
import json
import os
import sys
import types
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def _rows(target):
    with open(Path(target) / dp.MANIFEST_NAME, newline="") as f:
        return {r[0]: r for r in list(csv.reader(f))[2:]}


def _same_size_rewrite(path, content, keep_mtime):
    st = path.stat()
    path.write_bytes(content)
    if keep_mtime:
        os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))


def _deployed(tmp_path, monkeypatch, rel="data/a.bin", data=b"aaaa"):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    src = tmp_path / "src.bin"
    src.write_bytes(data)
    target = tmp_path / "target"
    target.mkdir()
    hashes = dp.deploy([(str(src), rel, "support")], str(target), False, cache={})
    return src, target, rel, hashes


def test_deploy_rows_carry_the_stamp_taken_with_the_hash(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    assert len(hashes[rel]) == 4 and hashes[rel][3] == (target / rel).stat().st_mtime_ns
    # unchanged second deploy (the skip path) records the stamp too
    again = dp.deploy([(str(_src), rel, "support")], str(target), False, cache={})
    assert again[rel][3] == (target / rel).stat().st_mtime_ns


def test_file_changed_between_hash_and_manifest_write_is_not_blessed(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    _same_size_rewrite(target / rel, b"bbbb", keep_mtime=False)  # a step after the hash rewrote it
    dp.write_manifest(str(target), hashes, "a" * 40)
    rep = dp.staleness_report(str(target))
    assert rep["stale"] == [rel] and rep["skipped"] == 0  # the old write-time stat stamped these bytes and skipped the hash


def test_row_without_a_stamp_is_written_without_one(tmp_path):
    (tmp_path / "f.bin").write_bytes(b"x")
    dp.write_manifest(str(tmp_path), {"f.bin": ("0" * 64, 1, None)}, "a" * 40)
    assert _rows(tmp_path)["f.bin"][6] == ""


def test_step_that_rewrites_a_file_is_caught(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    _same_size_rewrite(target / rel, b"bbbb", keep_mtime=False)
    assert dp.restamp_rewritten(str(target), hashes) == [rel]
    assert hashes[rel][3] is None and hashes[rel][0] == dp.sha256_file(str(_src))  # verified hash kept, never re-blessed
    dp.write_manifest(str(target), hashes, "a" * 40)
    assert dp.staleness_report(str(target))["stale"] == [rel]


def test_step_that_rewrites_identical_bytes_is_restamped(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    old = hashes[rel][3]
    _same_size_rewrite(target / rel, b"aaaa", keep_mtime=False)
    os.utime(target / rel, ns=(1, old + 7_000_000_000))
    assert dp.restamp_rewritten(str(target), hashes) == [rel]
    assert hashes[rel][3] == old + 7_000_000_000
    dp.write_manifest(str(target), hashes, "a" * 40)
    rep = dp.staleness_report(str(target))
    assert rep["stale"] == [] and rep["skipped"] == 1


def test_untouched_rows_are_not_rehashed_by_restamp(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    monkeypatch.setattr(dp, "sha256_file", lambda p: pytest.fail("hashed an untouched file"))
    assert dp.restamp_rewritten(str(target), hashes) == []


def test_verify_only_rehashes_despite_a_stamp(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    dp.write_manifest(str(target), hashes, "a" * 40)
    _same_size_rewrite(target / rel, b"bbbb", keep_mtime=True)  # same size, same mtime: only a full hash sees it
    assert dp.staleness_report(str(target))["stale"] == []  # the stamp fast path trusts it
    seen = []
    real = dp.staleness_report
    monkeypatch.setattr(dp, "staleness_report", lambda *a, **k: (seen.append(k.get("full")), real(*a, **k))[1])
    with pytest.raises(SystemExit):
        dp.verify_only(str(target), strict=True)
    assert seen == [True]


def test_carried_row_with_same_size_different_content_stays_stale(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    dp.write_manifest(str(target), hashes, "a" * 40)
    _same_size_rewrite(target / rel, b"bbbb", keep_mtime=False)  # same size, new bytes, new mtime
    carried = dp.carry_rows(dp.read_manifest(str(target)), str(target))
    dp.write_manifest(str(target), carried, "a" * 40)
    rep = dp.staleness_report(str(target))
    assert rep["stale"] == [rel] and rep["skipped"] == 0


def test_carry_never_stamps_unverified_rows(tmp_path, monkeypatch):
    _src, target, rel, hashes = _deployed(tmp_path, monkeypatch)
    dp.write_manifest(str(target), {rel: hashes[rel][:3]}, "a" * 40)  # row without a stamp
    carried = dp.carry_rows(dp.read_manifest(str(target)), str(target))
    assert carried[rel][3] is None


def test_verified_stamp_only_when_the_bytes_match(tmp_path):
    f = tmp_path / "f.bin"
    f.write_bytes(b"abc")
    digest = dp.sha256_file(str(f))
    assert dp.verified_stamp(str(f), digest) == f.stat().st_mtime_ns
    assert dp.verified_stamp(str(f), "0" * 64) is None
    assert dp.verified_stamp(str(tmp_path / "missing"), digest) is None


def test_hash_with_stamp_drops_the_stamp_when_the_file_moves_during_the_hash(tmp_path):
    f = tmp_path / "f.bin"
    f.write_bytes(b"abc")

    def racing_hasher(path):
        _same_size_rewrite(f, b"xyz", keep_mtime=False)
        os.utime(f, ns=(1, f.stat().st_mtime_ns + 9_000_000_000))
        return "h"

    assert dp.hash_with_stamp(str(f), racing_hasher)[2] is None


def _retire(tmp_path, rel, content, row_stamp, digest=None):
    """A target whose manifest carries `rel` (an editor-only row a game deploy does not own)."""
    target = tmp_path / "t"
    (target / rel).parent.mkdir(parents=True, exist_ok=True)
    (target / rel).write_bytes(content)
    dp.write_manifest(str(target), {rel: (digest or dp.sha256_file(str(target / rel)), len(content), None, row_stamp)}, "a" * 40)
    keep = {}
    dp.retire_unowned(str(target), keep, deploying_target="game")
    return keep


def test_retire_unowned_carries_the_earlier_stamp(tmp_path):
    keep = _retire(tmp_path, "Mission Editor.exe", b"editor", 12345)
    assert keep["Mission Editor.exe"][3] == 12345


def test_retire_unowned_stamps_an_unstamped_carried_row_only_when_it_still_matches(tmp_path):
    keep = _retire(tmp_path, "Mission Editor.exe", b"editor", None)
    assert keep["Mission Editor.exe"][3] == (tmp_path / "t" / "Mission Editor.exe").stat().st_mtime_ns


def test_retire_unowned_never_stamps_a_carried_row_whose_bytes_changed(tmp_path):
    keep = _retire(tmp_path, "Mission Editor.exe", b"editor", None, digest="0" * 64)
    assert keep["Mission Editor.exe"][3] is None


def test_manifest_only_stamps_hashed_rows_and_only_matching_carried_rows(tmp_path, monkeypatch):
    target = tmp_path / "t"
    target.mkdir()
    (target / "p.bin").write_bytes(b"payload")
    (target / "carried-ok.bin").write_bytes(b"ok")
    (target / "carried-bad.bin").write_bytes(b"changed")
    dp.write_manifest(str(target), {"carried-ok.bin": (dp.sha256_file(str(target / "carried-ok.bin")), 2, None),
                                    "carried-bad.bin": ("0" * 64, 7, None)}, "a" * 40)
    monkeypatch.setattr(dp, "enumerate_payload", lambda *a, **k: [(str(target / "p.bin"), "p.bin", "support")])
    dp.write_manifest_only(str(tmp_path), str(tmp_path), "mc2.exe", "mc2.pdb", str(target))
    rows = _rows(target)
    assert rows["p.bin"][6] == str((target / "p.bin").stat().st_mtime_ns)
    assert rows["carried-ok.bin"][6] == str((target / "carried-ok.bin").stat().st_mtime_ns)
    assert rows["carried-bad.bin"][6] == ""


def test_unstamped_notice_prints_once_per_process_and_is_true(tmp_path):
    target = tmp_path / "t"
    target.mkdir()
    (target / "a.bin").write_bytes(b"a")
    dp.write_manifest(str(target), {"a.bin": (dp.sha256_file(str(target / "a.bin")), 1, None)}, "a" * 40)
    notes = []
    first = dp.staleness_report(str(target), notice=notes.append)
    dp.staleness_report(str(target), notice=notes.append)
    assert first["hashed"] == 1 and len(notes) == 1
    assert "redeploy to record stamps" not in notes[0] and "no dest_mtime_ns" in notes[0]


# --- deploy_from_payload: the destination's own bytes are hashed and stamped, never the payload's column ----------------

def _payload(tmp_path, monkeypatch, files, staged, inherited_stamp="999"):
    payload = tmp_path / "payload"
    (payload / "files").mkdir(parents=True)
    target = tmp_path / "install"
    target.mkdir()
    with open(payload / ".deployed_manifest.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["manifest_version", "v1"])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp", "mtime_ns", "dest_mtime_ns"])
        for rel, data in files.items():
            w.writerow([rel, dp.hashlib.sha256(data).hexdigest(), len(data), "c" * 40, "2026-11-22T00:00:00", "5", inherited_stamp])
            if rel in staged:
                (payload / "files" / rel).write_bytes(data)
            else:
                (target / rel).parent.mkdir(parents=True, exist_ok=True)
                (target / rel).write_bytes(data)
    import exe_fingerprint
    monkeypatch.setattr(exe_fingerprint, "extract", lambda p: dict(present=True, sha="c" * 40, dirty=0))
    monkeypatch.setattr(exe_fingerprint, "build_current", lambda *a: True)
    return payload, target


def test_payload_deploy_records_destination_stamps_not_inherited_ones(tmp_path, monkeypatch):
    files = {"mc2.exe": b"exe-bytes", "data/untouched.bin": b"untouched"}
    payload, target = _payload(tmp_path, monkeypatch, files, staged={"mc2.exe"})
    dp.deploy_from_payload(payload, target, tmp_path, "c" * 40)
    rows = _rows(target)
    for rel in files:
        assert rows[rel][6] == str((target / rel).stat().st_mtime_ns) != "999"
    assert dp.staleness_report(str(target))["skipped"] == 2


def test_payload_deploy_refuses_a_copied_file_that_does_not_match(tmp_path, monkeypatch):
    files = {"mc2.exe": b"exe-bytes"}
    payload, target = _payload(tmp_path, monkeypatch, files, staged={"mc2.exe"})
    real = dp.shutil.copy2

    def corrupting(src, dst, *a, **k):
        real(src, dst, *a, **k)
        Path(dst).write_bytes(b"EXE-BYTES")  # same size, different bytes: the old code never re-read the destination

    monkeypatch.setattr(dp.shutil, "copy2", corrupting)
    with pytest.raises(SystemExit):
        dp.deploy_from_payload(payload, target, tmp_path, "c" * 40)


# --- _run_deploy wiring: skip_kit carry, code_only, retire_unowned reach write_manifest with the right rows ----------------

def _run_deploy_harness(tmp_path, monkeypatch, *, skip_kit, reuse, previous):
    head = "a" * 40
    image = ("MC2FP1|" + head[:12] + "|0|claude/test|2026-10-24T12:00:00|A:/Games/test|FP1END").encode()
    build = tmp_path / "build"
    build.mkdir()
    target = tmp_path / "target"
    target.mkdir()
    (build / "mc2.exe").write_bytes(image)
    (target / "mc2.exe").write_bytes(image)
    (target / "mods").mkdir()
    (target / "mods/old.bin").write_bytes(b"old-mod")
    if previous:
        dp.write_manifest(str(target), previous(target), head)
    (target / dp.REUSE_NAME).write_text(json.dumps({"sources": {}}))
    written, order = {}, []
    for name, value in (("check_exe_fresh", None), ("seed_build_dlls", None), ("check_free_space_for_payload", None),
                        ("check_hardlink_isolation", None), ("check_install_complete", []), ("ensure_stock_pak", None),
                        ("deploy_inputs", {}), ("enumerate_payload", []), ("viewer_payload", [])):
        monkeypatch.setattr(dp, name, lambda *a, _v=value, **k: _v)
    monkeypatch.setattr(dp, "git_head", lambda *a: head)
    monkeypatch.setattr(dp, "record_deploy_inputs",
                        lambda tgt, *a: (Path(tgt) / dp.REUSE_NAME).write_text(json.dumps({"sources": {}})))
    monkeypatch.setattr(dp, "prepare_seed_delta", lambda *a, **k: {})
    monkeypatch.setattr(dp, "reusable_payload", lambda *a: (reuse, "test"))
    monkeypatch.setattr(dp.subprocess, "run", lambda *a, **k: types.SimpleNamespace(stdout="", returncode=0))
    import asset_parity
    monkeypatch.setattr(asset_parity, "enforce", lambda *a, **k: None)
    monkeypatch.setattr(asset_parity, "intentional_changes", lambda *a, **k: set())
    from smoke_lib import render_options_pin
    monkeypatch.setattr(render_options_pin, "record", lambda *a: {"values": {"RemasteredBuildings": False}})
    seen = {}

    def fake_deploy(items, target_dir, *a, **k):
        seen["code_only"] = k.get("code_only")
        order.append("deploy")
        return {"mc2.exe": (dp.sha256_file(str(Path(target_dir) / "mc2.exe")), 1, None, Path(target_dir, "mc2.exe").stat().st_mtime_ns)}

    real_restamp = dp.restamp_rewritten

    def spy_restamp(target_dir, hashes):
        order.append("restamp")
        return real_restamp(target_dir, hashes)

    def fake_write(target_dir, hashes, commit):
        order.append("write")
        written.update(hashes)

    monkeypatch.setattr(dp, "deploy", fake_deploy)
    monkeypatch.setattr(dp, "restamp_rewritten", spy_restamp)
    monkeypatch.setattr(dp, "write_manifest", fake_write)
    args = types.SimpleNamespace(allow_stale_exe=False, build64_root=None, require_clean=False, classic_assets=False,
                                 full_verify=False, allow_stale_pdb=False, preserve=[], skip_kit=skip_kit,
                                 allow_new_target=False, no_asset_sync=False, target="lane", no_example_mods=False)
    dp.run_deploy(args, str(tmp_path), str(build), str(target), "mc2.exe", "mc2.pdb")
    return target, written, order, seen


def _previous_mod_row(stamp, digest=None):
    def previous(target):
        return {"mods/old.bin": (digest or dp.sha256_file(str(target / "mods/old.bin")), 7, None, stamp)}
    return previous


def test_skip_kit_carried_row_is_restamped_only_because_its_bytes_still_match(tmp_path, monkeypatch):
    target, written, order, _seen = _run_deploy_harness(tmp_path, monkeypatch, skip_kit=True, reuse=False,
                                                        previous=_previous_mod_row(4242))
    # the earlier stamp (4242) no longer describes the file: restamp_rewritten re-hashed it, the hash matched, so it is stamped now
    assert written["mods/old.bin"][3] == (target / "mods/old.bin").stat().st_mtime_ns
    assert order == ["deploy", "restamp", "write"]


def test_skip_kit_carried_row_whose_bytes_changed_is_never_blessed(tmp_path, monkeypatch):
    _t, written, _order, _seen = _run_deploy_harness(tmp_path, monkeypatch, skip_kit=True, reuse=False,
                                                     previous=_previous_mod_row(4242, digest="0" * 64))
    assert written["mods/old.bin"][3] is None and written["mods/old.bin"][0] == "0" * 64


def test_skip_kit_carried_row_without_a_stamp_stays_unstamped(tmp_path, monkeypatch):
    _t, written, _order, _seen = _run_deploy_harness(tmp_path, monkeypatch, skip_kit=True, reuse=False,
                                                     previous=_previous_mod_row(None))
    assert written["mods/old.bin"][3] is None


def test_code_only_deploy_passes_code_only_and_restamps_before_the_manifest(tmp_path, monkeypatch):
    _t, _written, order, seen = _run_deploy_harness(tmp_path, monkeypatch, skip_kit=False, reuse=True,
                                                    previous=_previous_mod_row(4242))
    assert seen["code_only"] is True and order.index("restamp") < order.index("write")


def test_retire_unowned_receives_the_deploy_hashes_and_target_kind(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(dp, "retire_unowned", lambda *a, **k: calls.append((a, k)))
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    src = tmp_path / "s.bin"
    src.write_bytes(b"s")
    target = tmp_path / "t"
    target.mkdir()
    hashes = dp.deploy([(str(src), "data/s.bin", "support")], str(target), False, cache={}, deploying_target="editor")
    (args, kwargs), = calls
    assert args[1] is hashes and kwargs["deploying_target"] == "editor"
