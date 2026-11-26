import json
from pathlib import Path
import sys
from unittest.mock import patch
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import import_mechs as importer


def game(root, kind):
    (root / importer.LAYOUTS[kind]).mkdir(parents=True)
    return root.resolve()


def test_incompatible_material_is_rejected_before_building():
    with pytest.raises(RuntimeError, match="Base Metal Roughness.*Values will not be guessed"):
        importer.check_material_defaults({"S": {"MetalRoughness": 0.4}, "V": {}})


def test_detect_steam_libraries_epic_and_common(tmp_path):
    steam, library, epic, common = [tmp_path / n for n in ("steam", "library", "epic", "games")]
    (steam / "steamapps").mkdir(parents=True)
    (steam / "steamapps/libraryfolders.vdf").write_text(
        '"libraryfolders" { "1" { "path" "' + str(library).replace("\\", "\\\\") + '" } }')
    mercs = game(library / "steamapps/common/Mercs", "mw5")
    (library / "steamapps/appmanifest_784080.acf").write_text('"AppState" { "installdir" "Mercs" }')
    clans = game(tmp_path / "Clans", "clans")
    epic.mkdir()
    (epic / "bad.item").write_text("broken metadata")
    (epic / "clans.item").write_text(json.dumps({"InstallLocation": str(clans)}))
    bt = game(common / "BATTLETECH", "bt")
    assert importer.detect([steam], epic, [common]) == {"mw5": mercs, "clans": clans, "bt": bt}


def test_detection_does_not_accept_named_but_missing_game(tmp_path):
    (tmp_path / "games/MechWarrior 5 Clans").mkdir(parents=True)
    assert importer.detect([], tmp_path / "missing", [tmp_path / "games"]) == {}


def test_preflight_reports_all_missing_tools_without_launching(tmp_path):
    with patch.object(importer, "tool", return_value=None), \
         patch.object(importer.shutil, "which", return_value=None), \
         patch.object(importer.importlib.util, "find_spec", return_value=None), \
         patch.object(importer, "run") as run:
        _, errors = importer.preflight({"mw5": tmp_path, "clans": tmp_path, "bt": tmp_path}, tmp_path)
    assert any(".NET 8 SDK" in e for e in errors["clans"])
    assert any("KTX-Software" in e for e in errors["mw5"])
    assert any("UE Viewer (MIT)" in e for e in errors["mw5"])
    assert any("requirements.txt" in e for e in errors["bt"])
    assert not any("UE Viewer" in e for e in errors["bt"])
    run.assert_not_called()


def test_build_failure_is_reported_for_each_unreal_game(tmp_path):
    def fail(cmd, *_args, **_kwargs):
        if cmd[0] == "dotnet":
            raise RuntimeError("NuGet restore unavailable")
    with patch.object(importer, "tool", return_value="ktx.exe"), \
         patch.object(importer.shutil, "which", return_value="dotnet"), \
         patch.object(importer.importlib.util, "find_spec", return_value=True), \
         patch.object(importer, "run", side_effect=fail):
        _, errors = importer.preflight({"mw5": tmp_path, "clans": tmp_path, "bt": tmp_path}, tmp_path)
    assert any("NuGet restore" in e for e in errors["mw5"])
    assert any("NuGet restore" in e for e in errors["clans"])
    assert errors["bt"] == []


def test_mercs_continues_after_failure_and_publishes_only_validated_pack(tmp_path):
    stage = tmp_path / "base/mods/mw5_mechs"
    stage.mkdir(parents=True)
    calls = []
    def build(cmd, *_args, **_kwargs):
        name = str(cmd[cmd.index("--recipe") + 1])
        calls.append(name)
        stage.mkdir(parents=True, exist_ok=True)
        (stage / "mod.json").write_text(name)
        if name == "bad.json":
            raise RuntimeError("missing game asset")
    with patch.object(importer, "material_defaults"), patch.object(importer, "base_install"), \
         patch.object(importer, "run", side_effect=build), patch.object(importer, "validate") as validate:
        report = []
        importer.mercs(tmp_path / "game", tmp_path / "install", tmp_path, {},
                       [(Path(n + ".json"), {"family": n}) for n in ("bad", "good")], report)
    assert calls == ["bad.json", "good.json"]
    assert [row[2] for row in report] == ["FAIL", "PASS"]
    validate.assert_called_once()
    assert (tmp_path / "install/mods/mw5_mechs/mod.json").read_text() == "good.json"
