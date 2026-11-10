"""A lane install carries the same pre-baked data as v0.4 (seed-parity lane)."""
import importlib.util
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import seed_parity as sp  # noqa: E402

SHARED = {
    "options.cfg": b"b ShadowCSM = TRUE\n",
    "orgprefs.cfg": b"org",
    "data/textures/1.2.txm": b"transition",
    "data/textures/1.2.ktx2": b"bc7",
    "data/textures/mc2_01.lst": b"1.2.txm\n",
    "data/textures/256/cement_1.tga": b"tga",
    "data/art/mechicons/anubis.png": b"png",
    "saved_bookmarks/mc2_01_saved_205644.json": b"{}",
}
NOT_SHARED = {
    "mc2.exe": b"exe", "shaders/a.spv": b"spv", "cache/content-model/model-1.mci": b"c",
    "data/textures/_txm_cache_bak/1.2.txm": b"bak", "data/textures/txm_backup/1.txm": b"bak",
    "data/textures/Random_Maps/a.jpg": b"jpg", "data/textures/a.tga.orig": b"orig", "mods/x/mod.json": b"m",
}


def tree(root, files):
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return root


def test_parity_classification():
    assert all(sp.is_parity(rel) for rel in SHARED)
    assert not any(sp.is_parity(rel) for rel in NOT_SHARED)
    assert sp.is_parity("DATA\\Textures\\1.2.TXM")


def test_check_names_missing_size_and_content_differences(tmp_path):
    ref = tree(tmp_path / "v04", {**SHARED, **NOT_SHARED})
    inst = tree(tmp_path / "lane", SHARED)
    assert sp.differing(inst, ref) == []
    sp.require_parity(inst, ref)
    (inst / "data/textures/1.2.txm").unlink()
    (inst / "options.cfg").write_bytes(b"old")                        # size differs
    (inst / "data/textures/1.2.ktx2").write_bytes(b"bc8")             # same size, other bytes
    assert sp.differing(inst, ref) == [
        ("content", "data/textures/1.2.ktx2"), ("missing", "data/textures/1.2.txm"), ("size", "options.cfg")]
    with pytest.raises(SystemExit) as exc:
        sp.require_parity(inst, ref)
    assert "missing data/textures/1.2.txm" in str(exc.value) and "--refresh-seed" in str(exc.value)


def test_files_outside_the_shared_set_never_fail_the_check(tmp_path):
    ref = tree(tmp_path / "v04", {**SHARED, **NOT_SHARED})
    inst = tree(tmp_path / "lane", {**SHARED, "mc2.exe": b"other build", "data/textures/new.txm": b"x"})
    assert sp.differing(inst, ref) == []


def test_refresh_copies_from_reference_and_never_writes_it(tmp_path):
    ref = tree(tmp_path / "v04", {**SHARED, **NOT_SHARED})
    seed = tree(tmp_path / "seed", {"options.cfg": b"old", "mc2.exe": b"seed exe"})
    before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in ref.rglob("*") if p.is_file()}
    os.link(seed / "options.cfg", tmp_path / "other-install-options.cfg")   # a linked seed file is replaced, not rewritten
    assert sp.refresh_seed(seed, ref) == len(SHARED)
    assert sp.differing(seed, ref) == []
    assert (seed / "mc2.exe").read_bytes() == b"seed exe"                   # payload is not parity
    assert not (seed / "data/textures/_txm_cache_bak").exists()
    assert (tmp_path / "other-install-options.cfg").read_bytes() == b"old"
    assert before == {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in ref.rglob("*") if p.is_file()}
    assert sp.refresh_seed(seed, ref) == 0
    with pytest.raises(SystemExit):
        sp.refresh_seed(ref, ref)
    with pytest.raises(SystemExit):
        sp.refresh_seed(ref / "inner", ref)


def test_missing_reference_is_skipped_not_failed(tmp_path, capsys):
    sp.require_parity(tmp_path, tmp_path / "no-such-v04")
    assert "SKIPPED" in capsys.readouterr().out


def test_lean_install_is_built_with_the_parity_set_and_checked(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location("lean_iso_parity", ROOT / "scripts/make-isolated-deploy.py")
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    guard = iso._load_guard()
    seed = tree(tmp_path / "seed", {**SHARED, **NOT_SHARED})
    manifest = {"mc2.exe": {"relpath": "mc2.exe"}}
    members = {rel for rel in {**SHARED, **NOT_SHARED} if iso.seed_member(rel, guard, manifest)}
    assert set(SHARED) <= members
    assert not {"data/textures/_txm_cache_bak/1.2.txm", "data/textures/Random_Maps/a.jpg", "data/textures/a.tga.orig",
                "cache/content-model/model-1.mci", "mods/x/mod.json"} & members
    links, copies, _, _ = iso.plan(seed, guard, manifest)
    dst = tmp_path / "lane"
    iso.build(seed, dst, links, copies)
    # data/textures is written by the game: private copies, never shared inodes with the seed or v0.4
    assert not (seed / "data/textures/1.2.txm").samefile(dst / "data/textures/1.2.txm")
    sp.require_parity(dst, seed)
    (dst / "data/textures/1.2.txm").write_bytes(b"regenerated")
    with pytest.raises(SystemExit):
        sp.require_parity(dst, seed)


def test_saved_bookmarks_are_a_mutated_root_and_leftovers_do_not_seed():
    spec = importlib.util.spec_from_file_location("lean_iso_bookmarks", ROOT / "scripts/make-isolated-deploy.py")
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    guard = iso._load_guard()
    assert "saved_bookmarks" in guard.MUTATED_ROOTS
    assert iso.seed_member("saved_bookmarks/mc2_01.json", guard, {})
    for leftover in ("saved_bookmarks/a.bak", "saved_bookmarks/a.tmp", "saved_bookmarks/run.log"):
        assert not iso.seed_member(leftover, guard, {}), leftover
