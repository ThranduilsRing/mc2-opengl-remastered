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


@pytest.fixture(autouse=True)
def no_declared_assets(monkeypatch):
    monkeypatch.setattr(sp, "declared_assets", lambda: {})


@pytest.mark.parametrize("after_deploy", [False, True])
def test_declared_assets_checked_repaired_and_snapshotted(tmp_path, monkeypatch, after_deploy):
    # Trigger paths: scripts/seed_parity.py, intentional-asset-changes.json.
    assets = {"data/model_overrides/maple/tree.glb": b"glb", "data/tgl/128/maple.tga": b"tga"}
    ref = tree(tmp_path / "v04", {**SHARED, **assets})
    seed = tree(tmp_path / "seed", SHARED)
    hashes = {rel: sp._sha(ref / rel) for rel in assets}
    monkeypatch.setattr(sp, "declared_assets", lambda: hashes)
    for state in ("missing", "content"):
        with pytest.raises(SystemExit, match="data/(model_overrides|tgl)"):
            sp.main(["--ref", str(ref), "--check", str(seed)])
        if after_deploy:
            assert sp.after_v04_deploy(ref, seed, ref)
        else:
            assert sp.main(["--ref", str(ref), "--refresh-seed", str(seed)]) == 0
        assert sp.main(["--ref", str(ref), "--check", str(seed)]) == 0
        assert hashes.items() <= sp.read_snapshot(seed).items()
        sp.require_seed(seed)
        for rel in assets:
            (seed / rel).write_bytes(b"bad")
    (ref / rel).write_bytes(b"bad")
    with pytest.raises(SystemExit, match="declared asset hash mismatch"):
        sp.refresh_seed(seed, ref)


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


def test_v04_drift_after_the_snapshot_does_not_fail_a_lane_install_but_seed_drift_does(tmp_path, capsys):
    ref = tree(tmp_path / "v04", {**SHARED, **NOT_SHARED})
    seed = tree(tmp_path / "seed", {"mc2.exe": b"seed exe"})
    sp.refresh_seed(seed, ref)
    assert sp.read_snapshot(seed) and set(sp.read_snapshot(seed)) == set(SHARED)
    sp.require_seed(seed)
    (ref / "options.cfg").write_bytes(b"v0.4 drifted after the refresh")      # live v0.4 moves on: not a lane's problem
    (ref / "data/textures/new.txm").write_bytes(b"new")
    sp.require_seed(seed)
    assert "matches its parity snapshot" in capsys.readouterr().out
    (seed / "data/textures/1.2.txm").write_bytes(b"corrupt")                  # the seed differs from its own snapshot
    (seed / "orgprefs.cfg").unlink()
    with pytest.raises(SystemExit) as exc:
        sp.require_seed(seed)
    msg = str(exc.value)
    assert "content data/textures/1.2.txm" in msg and "missing orgprefs.cfg" in msg and f"--refresh-seed {seed.as_posix()}" in msg


def test_seed_without_a_snapshot_falls_back_to_live_v04_and_names_the_refresh(tmp_path, monkeypatch, capsys):
    ref = tree(tmp_path / "v04", SHARED)
    seed = tree(tmp_path / "seed", {**SHARED, "options.cfg": b"old"})
    monkeypatch.setenv("MC2_SEED_PARITY_REF", str(ref))
    with pytest.raises(SystemExit) as exc:
        sp.require_seed(seed)
    assert "--refresh-seed" in str(exc.value) and "no .parity-snapshot.json" in capsys.readouterr().out


def test_v04_deploy_refreshes_seed_and_snapshot_or_prints_the_command(tmp_path, capsys):
    ref = tree(tmp_path / "v04", SHARED)
    seed = tree(tmp_path / "seed", {"mc2.exe": b"x"})
    assert not sp.after_v04_deploy(tmp_path / "some-lane", seed, ref)          # only a v0.4 target refreshes
    assert sp.read_snapshot(seed) is None
    assert sp.after_v04_deploy(ref, seed, ref) and set(sp.read_snapshot(seed)) == set(SHARED)
    assert not sp.after_v04_deploy(ref, tmp_path / "no-seed", ref)
    assert f"run: py -3 scripts/seed_parity.py --refresh-seed {(tmp_path / 'no-seed').as_posix()}" in capsys.readouterr().out


def test_lane_install_build_checks_the_seed_before_building(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location("lean_iso_pre", ROOT / "scripts/make-isolated-deploy.py")
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    seen = []
    monkeypatch.setattr(iso.seed_parity, "require_seed", lambda s: seen.append(("seed", s)) or (_ for _ in ()).throw(SystemExit(3)))
    monkeypatch.setattr(iso, "verified_seed", lambda *a, **k: seen.append("verified") or {})
    monkeypatch.setattr(iso, "same_volume", lambda *a: True)
    monkeypatch.setattr(iso, "check_not_system_drive", lambda *a: None)
    src = tmp_path / "seed"
    src.mkdir()
    with pytest.raises(SystemExit):
        iso.main(["--from", str(src), "--to", str(tmp_path / "lane")])
    assert seen == [("seed", src.resolve())]           # failed before the seed was even verified, nothing built
    assert not (tmp_path / "lane").exists()
