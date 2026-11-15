"""Trigger: deploy_payload.py and asset_parity.py; diagnostics never become payload."""
import sys
import os
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import asset_parity as ap
import deploy_payload as dp

BAD = ("logs/a.txt", "debug_state/state.json", "nested/a.orig_bak", "LAUNCHER.LOG",
       "data/tgl/_aoktxtmp/model.glb", "data/texture.tga.bak-20260704", "data/texture.tga.orig",
       "tgl.fst.slim-2026-06-12", "dev_shell_out/a.txt", "terrain_gen_out/a.txt", "stdout_smoke.txt",
       "cache/content-model/model-0123456789abcdef.mci", "cache/content-model/industrial-cook.json",
       "harness_bay_stdout.txt", "harness_encyclopedia_stderr.txt")


def test_deploy_filters_and_prunes_artifacts_without_touching_sources(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda _: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda _: None)
    source, target = tmp_path / "source", tmp_path / "target"
    source.write_bytes(b"asset")
    items = [(str(source), rel, "support") for rel in (*BAD, "data/tgl/live.glb")]
    for rel in BAD:
        path = target / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"old diagnostic")
    hashes = dp.deploy(items, str(target), False)
    assert set(hashes) == {"data/tgl/live.glb"}
    assert all(not (target / rel).exists() for rel in BAD)
    assert source.read_bytes() == (target / "data/tgl/live.glb").read_bytes()
    assert all(ap.is_excluded(rel) for rel in BAD)
    assert not ap.is_excluded("data/tgl/live.glb")


def test_bootstrap_omits_nested_artifacts(tmp_path):
    src, target = tmp_path / "src", tmp_path / "target"
    for rel in (*BAD, "data/a.rdc", "assets/live.png"):
        path = src / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x")
    dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    assert [p.relative_to(target).as_posix() for p in target.rglob("*") if p.is_file()] == ["assets/live.png"]


def test_prune_preserves_crash_evidence(tmp_path):
    crash = tmp_path / "crashes/a.dmp"
    crash.parent.mkdir()
    crash.write_bytes(b"evidence")
    with pytest.raises(SystemExit):
        dp.retire_unowned(str(tmp_path), {})
    assert crash.read_bytes() == b"evidence"


@pytest.mark.parametrize("owned", [False, True])
def test_prune_protects_owner_files_and_preserve(tmp_path, owned):
    paths = ("savegame/manual_bak/save.fit.bak-today", "mods/custom/backup/model.glb.orig",
             "data/savegame/manual_bak/save.log", "options.cfg", "prefs.cfg", "captures/owner.png",
             "_pngcap/owner.png", "_rdoc/owner.rdc", "owner.rdc", "data/tgl/manual_bak/model.glb",
             "data/captures/model.glb", "notes.log", "logs/preserved.txt", "terrain_gen_out/changed.txt")
    rows = {}
    for rel in paths:
        path = tmp_path / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"owner")
        rows[rel] = (dp.sha256_file(path), 5, None)
    if owned:
        dp.write_manifest(str(tmp_path), rows, "test")
    (tmp_path / paths[-1]).write_bytes(b"modified by owner")
    keep = {rel: rows[rel] for rel in ("data/tgl/manual_bak/model.glb", "data/captures/model.glb", "notes.log")}
    preserve = ("LOGS/",) if owned else ("LOGS/", "terrain_gen_out/changed.txt")
    dp.retire_unowned(str(tmp_path), keep, preserve=preserve)
    assert all((tmp_path / rel).is_file() for rel in paths)
    for rel in ("data/tgl/manual_bak/model.glb", "data/captures/model.glb", "old_bak/model.glb", "notes.log"):
        assert not ap.is_excluded(rel)


@pytest.mark.parametrize("name", ["mc2-win64-v0.4c", "MC2-WIN64-0.4C"])
@pytest.mark.parametrize("mode", [[], ["--allow-new-target"], ["--write-manifest-only"],
                                  ["--seed-owner-test", "--seed-source", "unused"]])
def test_cli_refuses_golden_before_any_write(tmp_path, monkeypatch, capsys, name, mode):
    monkeypatch.setattr(sys, "argv", ["deploy_payload.py", str(tmp_path / name), *mode])
    with pytest.raises(SystemExit):
        dp.main()
    assert "golden install is never a deploy target" in capsys.readouterr().err
    assert not (tmp_path / name).exists()


def test_prune_protects_mod_backup_through_directory_alias(tmp_path):
    backup = tmp_path / "mods/custom/model.glb.orig"
    backup.parent.mkdir(parents=True)
    backup.write_bytes(b"owner")
    alias = tmp_path / "terrain_gen_out"
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(str(backup.parent), str(alias))
    else:
        alias.symlink_to(backup.parent, target_is_directory=True)
    dp.write_manifest(str(tmp_path), {"terrain_gen_out/model.glb.orig": (dp.sha256_file(backup), 5, None)}, "test")
    dp.retire_unowned(str(tmp_path), {})
    assert backup.read_bytes() == b"owner"
