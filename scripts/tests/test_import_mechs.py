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


def test_material_defaults_extracts_palette_from_player_install(tmp_path):
    def extract(_env, game, kind, request, out, work):
        assert game == tmp_path / "game" and kind == "mw5"
        assert any("ClanMech_PaintedMetal_MTL" in p for p in request["packages"])
        out.mkdir()
        for name, values in [("PaintedMetal_MTL", {"MetalRoughness": .1}),
                             ("ClanMech_PaintedMetal_MTL", {m + " " + f: .7 for m in
                              ("Base Metal", "Gun Metal", "Black Metal", "Steel", "Aluminum", "Matte", "Anodized", "Sensor")
                              for f in ("Roughness", "Metallic")})]:
            params = {"RuntimeEntries": {"ParameterInfos": [{"Name": k} for k in values]},
                      "ScalarValues": list(values.values()), "RuntimeEntries[1]": {"ParameterInfos": []}, "VectorValues": []}
            (out / (name + ".exports.json")).write_text(json.dumps([
                {"Properties": {"CachedExpressionData": {"Parameters": params}}}]))
    env = {}
    with patch.object(importer, "extract", side_effect=extract):
        importer.material_defaults(env, tmp_path / "game", tmp_path)
    result = json.loads(Path(env["MW5_MATERIAL_DEFAULTS"]).read_text())
    assert result["S"]["Base Metal Roughness"] == .7
    assert result["S"]["MetalRoughness"] == .1


def test_clans_without_mapping_skips_without_launching(tmp_path):
    with patch.object(importer, "extract") as extract:
        report = []
        importer.clans(tmp_path, tmp_path, {}, [(Path("r.json"), {"family": "timberwolf"})], report)
    extract.assert_not_called()
    assert report[0][2] == "SKIP" and "MW5_USMAP" in report[0][3]


def test_mw5_native_normal_provenance_does_not_require_a_bt_dump(tmp_path):
    package = {"chassisId": "mw5:archer", "featureBits": ["HAS_NORMAL"], "materials": {
        "albedo": {"ktx2": "alb.ktx2"}, "normal": {"ktx2": "nrm.ktx2", "container": "bc7",
        "logicalFormat": "linear-normal-xyz", "source": "native:Archer_Body_NRM"}}}
    path = tmp_path / "archer.package.json"
    path.write_text(json.dumps(package))
    for name in ("alb.ktx2", "nrm.ktx2"):
        (tmp_path / name).touch()
    command = [sys.executable, importer.ROOT / "tools/bt2018_import/check_mech_packages.py",
               "--deploy", tmp_path, "--dump", tmp_path]
    importer.run(command, tmp_path / "check.log")
    package["chassisId"] = "bt2018:archer"
    path.write_text(json.dumps(package))
    with pytest.raises(RuntimeError):
        importer.run(command, tmp_path / "check-bt.log")


def test_preflight_checks_optional_workstation_wrapper_dependency(tmp_path):
    with patch.object(importer, "tool", return_value=None), \
         patch.object(importer.shutil, "which", return_value=None), \
         patch.object(importer.Path, "is_file", return_value=True), \
         patch.object(importer.importlib.util, "find_spec", side_effect=lambda name: None if name == "psutil" else True):
        _, errors = importer.preflight({"mw5": tmp_path, "bt": tmp_path}, tmp_path)
    assert any("pip install psutil" in e for e in errors["mw5"])
    assert not any("psutil" in e for e in errors["bt"])


def test_report_excludes_roster_metadata(tmp_path, monkeypatch, capsys):
    install = tmp_path / "install"
    (install / "data").mkdir(parents=True)
    monkeypatch.setenv("TEMP", str(tmp_path))
    with patch.object(importer, "detect", return_value={"bt": tmp_path / "game"}), \
         patch.object(importer, "preflight", return_value=({}, {"bt": ["missing export"]})):
        assert importer.main(["--install", str(install)]) == 1
    output = capsys.readouterr().out
    assert "bt  archer  FAIL" in output
    assert "bt  _policy" not in output
    assert "bt  _source" not in output


def test_report_routes_clan_recipe_to_clans_only(tmp_path, monkeypatch, capsys):
    install = tmp_path / "install"
    (install / "data").mkdir(parents=True)
    monkeypatch.setenv("TEMP", str(tmp_path))
    with patch.object(importer, "detect", return_value={"mw5": tmp_path / "mercs", "clans": tmp_path / "clans"}), \
         patch.object(importer, "preflight", return_value=({}, {"mw5": ["missing tool"], "clans": ["missing tool"]})):
        assert importer.main(["--install", str(install)]) == 1
    output = capsys.readouterr().out
    assert "clans  timberwolf  FAIL" in output
    assert "mw5  timberwolf" not in output
    assert "mw5  atlas  FAIL" in output
    assert "clans  atlas" not in output


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
                       [(Path(n + ".json"), {"family": n}) for n in ("bad", "good")]
                       + [(Path("clan.json"), {"family": "timberwolf", "game": "clans"})], report)
    assert calls == ["bad.json", "good.json"]
    assert [row[2] for row in report] == ["FAIL", "PASS"]
    validate.assert_called_once()
    assert (tmp_path / "install/mods/mw5_mechs/mod.json").read_text() == "good.json"


@pytest.mark.parametrize("cross_volume", [False, True])
def test_base_install_stages_data_and_fsts(tmp_path, cross_volume):
    install, stage = tmp_path / "install", tmp_path / "stage"
    (install / "data/nested").mkdir(parents=True)
    files = {"data/nested/base.fit": b"base data", "objects.fst": b"archive"}
    for rel, content in files.items():
        (install / rel).write_bytes(content)
    with patch.object(importer.os, "link", side_effect=OSError("different volumes")
                      if cross_volume else importer.os.link) as link:
        importer.base_install(install, stage)
    assert link.call_count == len(files)
    for rel, content in files.items():
        assert (stage / rel).read_bytes() == (install / rel).read_bytes() == content
        assert (stage / rel).samefile(install / rel) is not cross_volume


def test_bt_only_rebuild_keeps_other_chassis_and_matching_strings(tmp_path):
    import re
    from tools.bt2018_mechs import compile as compiler

    install = tmp_path / "install"
    target = install / "mods/bt2018_mechs"
    def convert(_bundle, name, conv, _dump):
        (conv / (name + ".glb")).touch()
        return {"btname": name}
    def build(cmd, *_args):
        if Path(cmd[1]).name != "build_pack.py":
            return
        pack = Path(cmd[cmd.index("--out") + 1])
        (pack / "content").mkdir(parents=True)
        (pack / "data/objects").mkdir(parents=True)
        (pack / "data/defs/text/en_us").mkdir(parents=True)
        profiles = [{"family": n, "stem": "bt2018_" + n, "name": n.title(), "details": n + " details"}
                    for n in sorted(cmd[cmd.index("--only") + 1].split(","))]
        (pack / "data/defs/text/en_us/bt2018_mechs.fit").write_text(compiler.emit_strings(profiles))
        for i, profile in enumerate(profiles):
            n = profile["family"]
            (pack / f"content/{n}.profile.json").write_text(json.dumps(profile))
            ids = compiler.string_ids(profile, i)
            (pack / f"data/objects/bt2018_{n}.csv").write_text(
                f"DescIndex,{ids['name']},unchanged\nAbbrIndex,{ids['details']},unchanged\n")
            (pack / (n + ".glb")).write_text(str(cmd[cmd.index("--conversions") + 1]))
        metadata = {k: {} for k in ("generated", "assets", "stock_references_unmodified", "native_clip_counts", "clip_manifest")}
        metadata["profiles"] = {p["family"]: p["name"] for p in profiles}
        (pack / "content/report.json").write_text(json.dumps(metadata))
    with patch("tools.bt2018_import.convert_all_bt2018.convert_bundle", side_effect=convert), \
         patch.object(importer, "run", side_effect=build), patch.object(importer, "validate") as validate:
        for i, names in enumerate((["atlas", "warhammer"], ["archer", "atlas"])):
            work = tmp_path / str(i)
            work.mkdir()
            report = []
            importer.battletech(tmp_path / "game", install, work, {"KTX": "ktx"}, names, report)
            assert all(row[2] == "PASS" for row in report)
            strings = {int(sid): text for text, sid in re.findall(
                r'text = "([^"]*)"\s+legacyId = (\d+)', (target / "data/defs/text/en_us/bt2018_mechs.fit").read_text())}
            expected = {"atlas", "warhammer"} | set(names)
            assert {p.stem.removeprefix("bt2018_") for p in (target / "data/objects").glob("*.csv")} == expected
            for n in expected:
                csv = (target / f"data/objects/bt2018_{n}.csv").read_text()
                ids = [int(s) for s in re.findall(r"Index,(\d+),unchanged", csv)]
                assert [strings[s] for s in ids] == [n.title(), n + " details"]
            assert len(strings) == 2 * len(expected)
        assert (target / "warhammer.glb").read_text() == str(tmp_path / "0/conversions")
        assert (target / "atlas.glb").read_text() == str(tmp_path / "1/conversions")
        metadata = json.loads((target / "content/report.json").read_text())
        assert set(metadata["profiles"]) == expected
        from tools.bt2018_mechs.build_pack import sha256
        assert all(sha256(target / rel) == digest for rel, digest in metadata["generated"].items())
        assert validate.call_count == 2
        before = {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()}
        validate.side_effect = RuntimeError("invalid package")
        work = tmp_path / "bad"
        work.mkdir()
        with pytest.raises(RuntimeError, match="invalid package"):
            importer.battletech(tmp_path / "game", install, work, {"KTX": "ktx"}, ["atlas"], [])
        assert {p.relative_to(target): p.read_bytes() for p in target.rglob("*") if p.is_file()} == before
