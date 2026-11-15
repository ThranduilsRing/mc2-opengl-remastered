"""Trigger: tools/import_campaign.py, transfer_mc2x_campaign.py, portable deployment entries."""
import importlib.util
import json
from pathlib import Path
import sys
import struct
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("campaign_import", ROOT / "tools/import_campaign.py")
imp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imp)


@pytest.fixture
def sample(tmp_path):
    source, deploy = tmp_path / "download", tmp_path / "game"
    deploy.mkdir()
    (deploy / "mc2.exe").touch()
    for name, data in {
        "data/campaign/demo.fit": b'[Campaign]\nst CampaignName = "Synthetic Campaign"\n[Group0Mission0]\nst FileName = "demo01"\n',
        "data/missions/demo01.fit": b'FITini\nObjectNumber = 12',
        "data/missions/demo01.pak": b'synthetic packet fixture',
    }.items():
        p = source / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    (deploy / "mods/mc2x-compat/data").mkdir(parents=True)
    (source / "data/objects").mkdir()
    (source / "data/objects/compbas.csv").write_bytes(b"shared ammo")
    return source, deploy


def test_folder_zip_same_identity_idempotent_and_read_only(sample, tmp_path):
    source, deploy = sample
    before = imp.digest(imp.source_files(source))
    result = imp.import_campaign(source, deploy)
    assert result["campaigns"][0]["first_mission"] == "demo01"
    metadata = json.loads((deploy / "mods" / result["mod"] / "mod.json").read_text())
    assert metadata["type"] == "campaign" and metadata["dependencies"] == ["mc2x-compat"]
    assert metadata["name"] == "Synthetic Campaign"
    assert metadata["description"] and metadata["author"] and metadata["credits"]
    archive = tmp_path / "renamed.zip"
    with zipfile.ZipFile(archive, "w") as z:
        for name, p in imp.source_files(source).items():
            z.write(p, "wrapper/" + name)
    assert imp.import_campaign(archive, deploy)["status"] == "unchanged"
    assert imp.digest(imp.source_files(source)) == before
    (deploy / "mods" / result["mod"] / "data/missions/demo01.fit").write_text("edited")
    with pytest.raises(ValueError, match="changed or is incomplete"):
        imp.import_campaign(source, deploy)


@pytest.mark.parametrize("name", ["../outside", "/absolute", "C:/drive", "data/NUL.txt", "data/x:ads", "data/trailing."])
def test_unsafe_zip_rejected(sample, tmp_path, name):
    source, deploy = sample
    archive = tmp_path / "bad.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr(name, "bad")
    with pytest.raises(ValueError, match="Unsafe"):
        imp.import_campaign(archive, deploy)
    assert not list((deploy / "mods").glob(".campaign-import-*"))


def test_incomplete_and_wrong_folder(sample):
    source, deploy = sample
    (source / "data/missions/demo01.pak").unlink()
    with pytest.raises(ValueError, match="demo01.pak"):
        imp.import_campaign(source, deploy)
    with pytest.raises(ValueError, match="campaign root"):
        imp.import_campaign(source / "data/missions", deploy)


def test_missing_compat_does_not_install(sample):
    source, deploy = sample
    imp.shutil.rmtree(deploy / "mods/mc2x-compat/data")
    (deploy / "mods/mc2x-compat").rmdir()
    with pytest.raises(ValueError, match="Missing mc2x-compat"):
        imp.import_campaign(source, deploy)
    assert list((deploy / "mods").iterdir()) == []


def test_source_change_same_size_is_not_silently_skipped(sample):
    source, deploy = sample
    imp.import_campaign(source, deploy)
    (source / "data/missions/demo01.fit").write_bytes(b'FITini\nObjectNumber = 13')
    with pytest.raises(ValueError, match="Different version"):
        imp.import_campaign(source, deploy)


def test_duplicate_zip_case_rejected(sample, tmp_path):
    _, deploy = sample
    archive = tmp_path / "duplicates.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("data/test", "a")
        z.writestr("DATA/TEST", "b")
    with pytest.raises(ValueError, match="Duplicate"):
        imp.import_campaign(archive, deploy)


def test_fst_detection_and_malformed_input(sample):
    source, _ = sample
    files = imp.source_files(source)
    offset = 8 + 266 * len(files)
    headers, payload = b"", b""
    for name, path in files.items():
        raw = path.read_bytes()
        headers += struct.pack("<IIII250s", offset, len(raw), len(raw), 0, name.encode())
        payload += raw
        offset += len(raw)
    archive = source / "mission.fst"
    archive.write_bytes(struct.pack("<II", 0xFADDECAF, len(files)) + headers + payload)
    for path in files.values():
        path.unlink()
    _, names, campaigns = imp.inspect_source(source)
    assert campaigns[0]["first_mission"] == "demo01" and "data/missions/demo01.pak" in names
    archive.write_bytes(b"bad")
    with pytest.raises(ValueError, match="Invalid FST"):
        imp.inspect_source(source)


def test_zip_saved_beside_game_and_inno_extraction(sample, monkeypatch):
    source, deploy = sample
    archive = deploy / "download.zip"
    with zipfile.ZipFile(archive, "w") as z:
        for name, path in imp.source_files(source).items():
            z.write(path, name)
    assert imp.import_campaign(archive, deploy)["status"] == "installed"
    installer = source.parent / "download.exe"
    installer.write_bytes(b"synthetic installer")
    monkeypatch.setattr(imp.shutil, "which", lambda name: None)
    (deploy / "innoextract.exe").touch()
    def extract(args, **kwargs):
        from types import SimpleNamespace
        assert args[0] == str(deploy / "innoextract.exe")
        imp.shutil.copytree(source, Path(args[3]) / "app")
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(imp.subprocess, "run", extract)
    assert imp.import_campaign(installer, deploy)["status"] == "unchanged"


def test_data_root_pack_and_missing_dependency_on_repeat(sample):
    source, deploy = sample
    result = imp.import_campaign(source / "data", deploy)
    assert result["status"] == "installed"
    imp.shutil.rmtree(deploy / "mods/mc2x-compat/data")
    with pytest.raises(ValueError, match="Missing mc2x-compat"):
        imp.import_campaign(source, deploy)


@pytest.mark.parametrize("same", [False, True])
def test_loose_override_uses_bytes_and_first_dependency(sample, same):
    source, deploy = sample
    raw = (source / "data/missions/demo01.fit").read_bytes()
    ref = deploy / "mods/mc2x-compat/data/missions/demo01.fit"
    ref.parent.mkdir()
    ref.write_bytes(raw if same else b"different campaign")
    result = imp.import_campaign(source, deploy)
    out = deploy / "mods" / result["mod"] / "data/missions/demo01.fit"
    assert out.exists() != same
    if not same:
        assert out.read_bytes() == raw


def test_fst_delta_preserves_different_bytes(tmp_path, monkeypatch):
    import transfer_mc2x_campaign as transfer
    ref, out = tmp_path / "ref", tmp_path / "out"
    (ref / "missions").mkdir(parents=True)
    (ref / "missions/demo.fit").write_bytes(b"base")
    monkeypatch.setattr(transfer.fst, "fst_entries", lambda _: [(0, 0, 0, "data/missions/demo.fit", False, b"")])
    monkeypatch.setattr(transfer.fst, "decompress_entry", lambda *args: b"override")
    assert transfer.extract_fst("fixture", str(out), refs=[str(ref)]) == 1
    assert (out / "data/missions/demo.fit").read_bytes() == b"override"
    assert not transfer.in_refs("data/missions/demo.fit", [str(ref), str(out / "data")], b"override")
    assert transfer.copy_loose_delta(str(ref), [str(ref)], str(out / "data")) == 1
    assert (out / "data/missions/demo.fit").read_bytes() == b"base"
    monkeypatch.setattr(transfer.fst, "fst_entries", lambda _: [(0, 0, 0, "data/objects/mc2xcomp.csv", False, b"")])
    assert transfer.extract_fst_whitelist("fixture", str(out), transfer.MISC_ROSTER) == 1
    assert (out / "data/objects/compbas.csv").read_bytes() == b"override"


@pytest.mark.parametrize("names", [
    ["compbas.csv"], ["mc2xcomp.csv", "compbas.csv"], ["compbas.csv", "mc2xcomp.csv"]])
def test_campaign_component_table_spellings(tmp_path, monkeypatch, names):
    import transfer_mc2x_campaign as transfer
    entries = [(0, 0, 0, "data/objects/" + name, False, name.encode()) for name in names]
    entries += [(0, 0, 0, "data/objects/VonLuckner.csv", False, b"authored profile"),
                (0, 0, 0, "data/art/gui.csv", False, b"old GUI")]
    monkeypatch.setattr(transfer.fst, "fst_entries", lambda _: entries)
    monkeypatch.setattr(transfer.fst, "decompress_entry", lambda off, comp, real, lz, data: data)
    transfer.extract_fst_whitelist("fixture", str(tmp_path), transfer.campaign_roster(entries))
    expected = b"mc2xcomp.csv" if "mc2xcomp.csv" in names else b"compbas.csv"
    assert (tmp_path / "data/objects/compbas.csv").read_bytes() == expected
    assert (tmp_path / "data/objects/vonluckner.csv").read_bytes() == b"authored profile"
    assert not (tmp_path / "data/art/gui.csv").exists()


@pytest.mark.parametrize("failure", ["rc", "missing"])
def test_failed_transfer_is_never_published(sample, monkeypatch, failure):
    source, deploy = sample
    original = imp.legacy._run_step
    def fail(step, args):
        if failure == "rc":
            return 7
        original(step, args)
        (Path(args[args.index("--out") + 1]) / "data/missions/demo01.pak").unlink()
    monkeypatch.setattr(imp.legacy, "_run_step", fail)
    with pytest.raises(ValueError, match="transfer failed: 7" if failure == "rc" else "demo01.pak.*download"):
        imp.import_campaign(source, deploy)
    assert [p.name for p in (deploy / "mods").iterdir()] == ["mc2x-compat"]


def test_new_compat_keeps_its_component_table(sample, monkeypatch):
    import shutil
    source, deploy = sample
    (source / "data/objects/compbas.csv").unlink()
    imp.shutil.rmtree(deploy / "mods/mc2x-compat/data")
    (deploy / "mods/mc2x-compat").rmdir()
    (source / "misc.fst").write_bytes(b"\xaf\xec\xdd\xfa" + bytes(300))
    entries = [(274, 0, 0, "data/objects/mc2xcomp.csv", False, bytes(300))]
    monkeypatch.setattr(imp.legacy.fst, "fst_entries", lambda _: entries)
    monkeypatch.setattr(imp.legacy.fst, "decompress_entry", lambda *args: b"authored ammo")
    monkeypatch.setattr(imp.legacy, "classify", lambda _: {"action": "convert", "needs_compat": "mc2x-compat"})
    monkeypatch.setattr(imp.legacy, "_plan", lambda *args: [{"kind": "build", "targets": ["mc2x-compat"], "args": []}])
    monkeypatch.setattr(imp.legacy, "_subst", lambda args, staged, deploy: [staged])
    def convert(step, args):
        if step["kind"] == "build":
            (Path(args[0]) / "mods/mc2x-compat/data").mkdir(parents=True)
        else:
            destination = Path(args[args.index("--out") + 1])
            shutil.copytree(source / "data", destination / "data")
        return 0
    monkeypatch.setattr(imp.legacy, "_run_step", convert)
    imp.import_campaign(source, deploy)
    assert (deploy / "mods/mc2x-compat/data/objects/compbas.csv").read_bytes() == b"authored ammo"


def test_campaign_object_holes_use_authored_definitions(tmp_path):
    sys.path.insert(0, str(ROOT / "tools/mco_import"))
    from build_object2 import read_pak, write_pak, RAW, NUL
    source, out, ref = tmp_path / "source", tmp_path / "out", tmp_path / "ref"
    (source / "data/art").mkdir(parents=True)
    (source / "data/objects").mkdir()
    (ref / "objects").mkdir(parents=True)
    slots = [(NUL, b"")] * 2100
    slots[1200] = (RAW, b"shared definition")
    write_pak(str(ref / "objects/object2.pak"), slots)
    (source / "data/art/buildings.csv").write_text(
        "name,group,id,type,fitid\nExisting,0,0,VEHICLE,1200\n"
        "JEdgar(Flamer),0,0,VEHICLE,2091\nUnavailable,0,0,VEHICLE,2092\n")
    definition = b"FITini\n[ObjectClass]\nl ObjectTypeNum = 1\nFITend"
    (source / "data/objects/JEdgar(Flamer).fit").write_bytes(definition)
    (source / "data/objects/Existing.fit").write_bytes(b"campaign override")
    imp.fill_campaign_object_slots(source, out, [str(ref)])
    _, result = read_pak(str(out / "data/objects/object2.pak"))
    assert result[1200] == slots[1200]
    assert result[2091] == (RAW, definition)
    assert result[2092] == (NUL, b"")
    assert read_pak(str(ref / "objects/object2.pak"))[1] == slots


@pytest.mark.skipif(sys.platform != "win32", reason="Windows batch launcher")
def test_batch_preserves_relative_source_and_failure(tmp_path):
    import subprocess
    game = tmp_path / "game with spaces"
    (game / "tools").mkdir(parents=True)
    (tmp_path / "relative.zip").touch()
    (game / "tools/import_campaign.py").write_text(
        "import os, sys\nassert sys.argv[1] == 'relative.zip'\nassert os.path.isfile(sys.argv[1])\nsys.exit(7)\n")
    launcher = game / "import-campaign.bat"
    launcher.write_bytes((ROOT / "import-campaign.bat").read_bytes())
    run = subprocess.run(["cmd", "/d", "/c", "call", str(launcher), "relative.zip"], cwd=tmp_path,
                         input="\n", capture_output=True, text=True,
                         **imp.console_child.child_kwargs(redirected=True))
    assert run.returncode == 7, run.stdout + run.stderr


def test_existing_shared_components_repaired_on_unchanged_import(sample, tmp_path, capsys):
    source, deploy = sample
    result = imp.import_campaign(source, deploy)
    dep = deploy / "mods/mc2x-compat"
    # A mission-only pack must repair from the compatibility base's provenance.
    base = tmp_path / "full-base"
    (base / "data/objects").mkdir(parents=True)
    (base / "data/objects/mc2xcomp.csv").write_bytes(b"base ammo")
    (dep / "mc2x_import_report.json").write_text(json.dumps({"source_path": str(base)}))
    table = dep / "data/objects/compbas.csv"
    for corrupt in (None, b"broken"):
        if corrupt is None:
            table.unlink()
        else:
            table.write_bytes(corrupt)
        assert imp.import_campaign(source, deploy)["status"] == "unchanged"
        assert table.read_bytes() == b"base ammo"
        assert "Repaired shared component table" in capsys.readouterr().err
    assert imp.import_campaign(source, deploy)["mod"] == result["mod"]


@pytest.mark.parametrize("names", [["mc2xcomp.csv"], ["compbas.csv", "mc2xcomp.csv"]])
def test_loose_component_alias_and_priority(tmp_path, names):
    import transfer_mc2x_campaign as transfer
    source, out = tmp_path / "source", tmp_path / "out"
    (source / "objects").mkdir(parents=True)
    for name in names:
        (source / "objects" / name).write_bytes(name.encode())
    transfer.copy_loose_delta(str(source), [], str(out))
    assert (out / "objects/compbas.csv").read_bytes() == b"mc2xcomp.csv"
    assert not (out / "objects/mc2xcomp.csv").exists()


def test_imported_id_resolves_legacy_manifest_without_changing_stock(sample):
    from scripts.run_smoke import _modmission_env_guard
    from scripts.smoke_lib.manifest import Entry
    source, deploy = sample
    mod = imp.import_campaign(source, deploy)["mod"]
    entry = Entry(tier="tier3", stem="demo01", mod="OldFolder", deps="OldBase")
    ok, env, error = _modmission_env_guard(entry, deploy, {"MC2_ACTIVE_MOD": mod})
    assert ok and not error
    assert env == {"MC2_ACTIVE_MOD": mod, "MC2_MOD_DEPS": "mc2x-compat"}
    assert entry.mod == "OldFolder"
    assert _modmission_env_guard(Entry(tier="tier1", stem="mc2_01"), deploy, {}) == (True, {}, "")
    (deploy / "mods" / mod / "mod.json").write_text("{}")
    assert not _modmission_env_guard(entry, deploy, {"MC2_ACTIVE_MOD": mod})[0]


def test_batch_preserves_callers_relative_source_directory():
    batch = (ROOT / "import-campaign.bat").read_text().lower()
    assert "cd /d" not in batch and "pushd" not in batch
