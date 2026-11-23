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
    "data/tgl/_aoktxtmp/ao.uastc.ktx2": b"ao",
    "data/tgl/_aoktxtmp/nested/normal.pre.png": b"normal",
    "data/tgl/_aoktxtmp/work.tmp": b"temporary",
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


@pytest.mark.parametrize("after_deploy", [False, True])
@pytest.mark.parametrize("shared", [False, True])
@pytest.mark.parametrize("publish", [False, True])
def test_payload_refresh_after_game_and_editor_preserves_old_hardlinks(tmp_path, monkeypatch, after_deploy, shared, publish):
    # Trigger paths: scripts/seed_parity.py, scripts/deploy_payload.py.
    import deploy_payload as dp
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    payload = {"mc2.exe": b"new game", "mc2.pdb": b"symbols", "runtime.dll": b"dll",
               "shaders/vulkan/main.spv": b"shader", "EditRel.exe": b"editor"}
    source = tree(tmp_path / "build", payload)
    ref = tree(tmp_path / "v04", {"options.cfg": b"new options"})
    monkeypatch.setattr(dp, "TARGET_PRESETS", {"game": {"target_dir": str(ref if shared else tmp_path / "other")}})
    import exe_fingerprint
    monkeypatch.setattr(exe_fingerprint, "extract", lambda p: {"present": True, "sha": "fixture"})
    monkeypatch.setattr(exe_fingerprint, "build_current", lambda *a: True)
    for names in (("mc2.exe", "mc2.pdb", "runtime.dll", "shaders/vulkan/main.spv"),
                  ("EditRel.exe", "runtime.dll", "shaders/vulkan/main.spv")):
        if shared and "mc2.pdb" not in names:
            (ref / "mc2.pdb").write_bytes(b"altered symbols")
        items = [(str(source / rel), rel, "pdb" if rel.endswith(".pdb") else "exe") for rel in names]
        hashes = dp.deploy(items, str(ref), False, full_verify=True)
        dp.write_manifest(str(ref), hashes, "fixture")
    assert (ref / "mc2.exe").read_bytes() == payload["mc2.exe"]
    if not shared:
        assert set(dp.read_manifest(ref)) == set(names)
    if publish:
        package = tree(tmp_path / "package/files", {"mc2.exe": payload["mc2.exe"]}).parent
        dp.write_manifest(package, {"mc2.exe": (sp._sha(source / "mc2.exe"), len(payload["mc2.exe"]), None)}, "fixture")
        dp.deploy_from_payload(package, ref, tmp_path, "fixture")
        payload = {rel: data for rel, data in payload.items() if rel.endswith((".exe", ".pdb"))}
    if not shared:
        assert set(dp.read_manifest(ref)) == ({"mc2.exe"} if publish else set(names))
        return
    assert set(dp.read_manifest(ref)) == set(payload)
    assert dp.read_manifest(ref)["mc2.pdb"][2] is None
    assert dp.read_manifest(ref)["mc2.pdb"] == (sp._sha(source / "mc2.pdb"), len(payload["mc2.pdb"]), None)
    (ref / "mc2.pdb").write_bytes(payload["mc2.pdb"])
    seed = tree(tmp_path / "seed", {"mc2.exe": b"old game", ".deployed_manifest.csv": b"old manifest"})
    old = tmp_path / "old-lane"
    old.mkdir()
    for rel in ("mc2.exe", ".deployed_manifest.csv"):
        os.link(seed / rel, old / rel)
    before = {p: p.read_bytes() for p in ref.rglob("*") if p.is_file()}
    assert sp.main(["--ref", str(ref), "--refresh-seed", str(seed), "--dry-run"]) == 0
    assert (seed / "mc2.exe").read_bytes() == b"old game"
    assert not (seed / sp.SNAPSHOT_NAME).exists()
    if after_deploy:
        assert sp.after_v04_deploy(ref, seed, ref)
    else:
        assert sp.main(["--ref", str(ref), "--refresh-seed", str(seed)]) == 0
    assert all((seed / rel).read_bytes() == data for rel, data in payload.items())
    assert (seed / ".deployed_manifest.csv").read_bytes() == (ref / ".deployed_manifest.csv").read_bytes()
    assert (old / "mc2.exe").read_bytes() == b"old game"
    assert (old / ".deployed_manifest.csv").read_bytes() == b"old manifest"
    assert before == {p: p.read_bytes() for p in ref.rglob("*") if p.is_file()}
    assert sp.refresh_seed(seed, ref) == 0
    sp.require_seed(seed)


def test_failed_atomic_copy_keeps_seed_file_and_cleans_temp(tmp_path, monkeypatch):
    # Trigger path: scripts/seed_parity.py.
    source = tree(tmp_path / "ref", {"mc2.exe": b"new"})
    seed = tree(tmp_path / "seed", {"mc2.exe": b"old"})
    os.link(seed / "mc2.exe", tmp_path / "old-exe")
    def failed_replace(src, dst):
        assert Path(src).suffix == ".tmp"
        assert (seed / "mc2.exe").read_bytes() == b"old"
        raise OSError("replace failed")
    monkeypatch.setattr(sp.os, "replace", failed_replace)
    with pytest.raises(OSError, match="replace failed"):
        sp._atomic_copy(source / "mc2.exe", seed / "mc2.exe")
    assert (seed / "mc2.exe").read_bytes() == (tmp_path / "old-exe").read_bytes() == b"old"
    assert list(seed.iterdir()) == [seed / "mc2.exe"]


@pytest.mark.parametrize("failure", ["missing-exe", "mismatch", "mid-loop", "escape"])
def test_refresh_failure_keeps_publication_records(tmp_path, monkeypatch, capsys, failure):
    # Trigger paths: scripts/seed_parity.py, scripts/deploy_payload.py.
    import deploy_payload as dp
    ref = tree(tmp_path / "ref", {"mc2.exe": b"new", "mc2.pdb": b"pdb"})
    seed = tree(tmp_path / "seed", {"mc2.exe": b"old", ".deployed_manifest.csv": b"old manifest", sp.SNAPSHOT_NAME: b"old snapshot"})
    rows = {rel: (sp._sha(ref / rel), (ref / rel).stat().st_size) for rel in ("mc2.exe", "mc2.pdb")}
    if failure == "missing-exe":
        rows.pop("mc2.exe")
    elif failure == "mismatch":
        rows["mc2.pdb"] = ("wrong", 3)
    elif failure == "escape":
        rows["../outside"] = ("wrong", 3)
    dp.write_manifest(ref, rows, "fixture")
    copy = sp._atomic_copy
    if failure == "mid-loop":
        def fail_second(src, dst):
            if src.name == "mc2.pdb":
                raise OSError("mid-loop failure")
            copy(src, dst)
        monkeypatch.setattr(sp, "_atomic_copy", fail_second)
    assert not sp.after_v04_deploy(ref, seed, ref)
    assert "seed NOT refreshed" in capsys.readouterr().out
    assert (seed / ".deployed_manifest.csv").read_bytes() == b"old manifest"
    assert (seed / sp.SNAPSHOT_NAME).read_bytes() == b"old snapshot"
    assert (seed / "mc2.exe").read_bytes() == (b"new" if failure == "mid-loop" else b"old")
