"""The render options as a pinned input of every proof (scripts/smoke_lib/render_options_pin.py): the table, the strict grammar, the A/B refusal."""
import importlib.util
import json
import re
import sys
from pathlib import Path

import pytest

from scripts.smoke_lib import render_options_pin as rop
from scripts.smoke_lib.report import Row, render_json, render_markdown

ROOT = Path(__file__).resolve().parents[2]


def values(**changes):
    v = {o.key: o.default for o in rop.table()}
    v.update(changes)
    return v


def test_table_has_the_engines_options_with_their_defaults():
    t = {o.key: o for o in rop.table()}
    assert len(t) == 31   # includes CameraView and AtmosphereQuality
    assert t["CameraView"].default == 2 and (t["CameraView"].lo, t["CameraView"].hi) == (0, 2)
    assert t["HdrOutput"].default == 0 and (t["HdrOutput"].lo, t["HdrOutput"].hi) == (0, 1)
    assert t["MechLabLocations"].default is True
    assert t["StockOverlayConcrete"].default is False
    assert t["TextureQuality"].default == -1 and (t["TextureQuality"].lo, t["TextureQuality"].hi) == (-1, 3)
    assert t["BaseQuality"].default == 2 and (t["BaseQuality"].lo, t["BaseQuality"].hi) == (0, 2)
    assert t["RemasteredBuildings"].default is False
    assert t["SpatialAA"].default == 1 and (t["SpatialAA"].lo, t["SpatialAA"].hi) == (0, 4)
    assert t["ShadowQuality"].default == 2 and (t["ShadowQuality"].lo, t["ShadowQuality"].hi) == (0, 2)
    assert t["CpuPhongRetire"].default is True and t["StaticPropPbr"].default is False and t["StaticPropAmbient"].default is True
    assert t["ImpactDecals"].default == 2 and t["ImpactDecals"].hi == 2
    assert t["WaterReflections"].default == 2 and (t["WaterReflections"].lo, t["WaterReflections"].hi) == (0, 2)
    assert t["TextureQuality"].default == -1 and (t["TextureQuality"].lo, t["TextureQuality"].hi) == (-1, 3)
    assert t["SunAxisFix"].default is False
    assert t["PostCASSharpness"].default == 0.5 and t["PostCASSharpness"].hi == 1.0
    assert t["TonemapMode"].kind == "long" and (t["TonemapMode"].lo, t["TonemapMode"].hi) == (0, 3)
    assert t["StaticPropIblSunRotateFloor"].default == 0.45


def test_water_detail_does_not_alias_bake_weather_scale():
    run = (ROOT / "GameOS/gameos/vulkan_water_island_run.cpp").read_text()
    bake = (ROOT / "GameOS/gameos/vulkan_water_reflect_bake.cpp").read_text()
    shader = (ROOT / "shaders/vulkan/water_geom.frag").read_text()
    assert "fsLocal.debug[1] = mc2::options::current().waterDetail" in run
    assert "params->SHALLOW_COLOR[3] = b.scale[2]" in bake
    assert "F.debug.y < 0.5" in shader and "F.debug.y > 0.5" in shader
    assert "F.SHALLOW_COLOR.w < 0.5" not in shader


def test_python_grammar_agrees_with_the_doctest_case_table():
    """tests/unit/test_gate.cpp lists the spellings the engine rejects; this module must reject exactly those, with the same reason."""
    src = (ROOT / "tests" / "unit" / "test_gate.cpp").read_text(encoding="utf-8")
    body = src[src.index("options.cfg values follow one strict grammar"):]
    cases = re.findall(r'\{"([bfl] \w+ =[^"]*)",\s*"(\w+)",\s*mo::Verdict::(Garbage|OutOfRange)\}', body)
    assert len(cases) >= 15
    for line, key, why in cases:
        rejected = []
        rop.apply_cfg(values(), "[MechCommander2]\r\n" + line + "\r\n", rejected)
        assert [r["key"] for r in rejected] == [key], line
        assert rejected[0]["why"] == ("garbage" if why == "Garbage" else "out_of_range"), line


def test_accepted_spellings_and_first_key_wins():
    v = values()
    rej = []
    rop.apply_cfg(v, "[MechCommander2]\r\nb CpuPhongRetire = false\r\nb SunAxisFix = TRUE\r\nb VfxLitParticles = 1\r\nl TonemapMode = 3\r\n"
                     "f PostCASSharpness = 0.25\r\nb StaticPropAmbient =   False  \r\nb CpuPhongRetire = TRUE\r\n[Keys]\r\nb SunElevationFix = TRUE\r\n", rej)
    assert rej == []
    assert v["CpuPhongRetire"] is False and v["SunAxisFix"] is True and v["VfxLitParticles"] is True and v["StaticPropAmbient"] is False
    assert v["TonemapMode"] == 3 and v["PostCASSharpness"] == 0.25
    assert v["SunElevationFix"] is False      # another block


def test_record_reads_the_install_then_the_override_case_insensitively(tmp_path):
    (tmp_path / "options.cfg").write_text("[MechCommander2]\r\nb StaticPropAmbient = FALSE\r\nb CpuPhongRetire = banana\r\n", encoding="latin-1")
    rec = rop.record(tmp_path, {"MC2_OPTIONS_OVERRIDE": "cpuphongretire=0, TonemapMode=9, Nope=1"})
    assert rec["values"]["StaticPropAmbient"] is False
    assert rec["values"]["CpuPhongRetire"] is False                     # the override beats the (rejected) file value
    assert rec["values"]["TonemapMode"] == 0
    assert {(r["key"], r["why"]) for r in rec["rejected"]} == {("CpuPhongRetire", "garbage"), ("TonemapMode", "out_of_range"), ("Nope", "unknown_key")}
    assert rop.record(tmp_path / "missing", {})["values"] == values()   # no file: the defaults


def test_matching_options_pass_and_a_difference_is_a_hard_failure():
    a, b = {"values": values()}, {"values": values()}
    rop.require_same(a, b)
    b = {"values": values(SunAxisFix=True, PostCASSharpness=0.75)}
    with pytest.raises(rop.OptionsMismatch) as e:
        rop.require_same(a, b, a_name="BEFORE", b_name="AFTER")
    msg = str(e.value)
    assert "SunAxisFix: A=False B=True" in msg and "PostCASSharpness" in msg and "BEFORE" in msg and "--vary" in msg
    rop.require_same(a, b, vary=["SunAxisFix", "postcassharpness"])     # named on purpose, any case
    with pytest.raises(rop.OptionsMismatch):
        rop.require_same(a, b, vary=["SunAxisFix"])                        # the other key still differs
    with pytest.raises(ValueError):
        rop.require_same(a, b, vary=["NotAnOption"])


def test_a_run_without_a_record_is_refused():
    with pytest.raises(rop.OptionsMismatch):
        rop.require_same({"values": values()}, {"values": None})
    with pytest.raises(rop.OptionsMismatch):
        rop.require_same(None, {"values": values()})


def test_floats_compare_at_engine_precision():
    rop.require_same({"values": values(StaticPropIblSunRotateFloor=0.45)}, {"values": values(StaticPropIblSunRotateFloor=0.44999998807907104)})


def test_old_capture_missing_option_uses_default():
    old = values()
    del old["RemasteredBuildings"]
    rop.require_same({"values": old}, {"values": values()})
    with pytest.raises(rop.OptionsMismatch, match="RemasteredBuildings"):
        rop.require_same({"values": old}, {"values": values(RemasteredBuildings=True)})


def test_historical_capture_logs_all_defaulted_options(capsys):
    old = values()
    del old["RemasteredBuildings"]
    del old["PostCASSharpness"]
    rop.require_same({"values": old}, {"values": values()})
    log = capsys.readouterr().err
    assert "A defaulted missing options:" in log
    assert "RemasteredBuildings" in log and "PostCASSharpness" in log


def test_engine_report_must_agree_with_the_harness_reading():
    assert rop.check_engine(values(), None) == []
    assert rop.check_engine(values(), values()) == []
    assert rop.check_engine(values(), values(CpuPhongRetire=False)) == ["CpuPhongRetire: A=True B=False"]


def test_capture_ab_refuses_sets_with_different_options(tmp_path):
    sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
    sys.path.insert(0, str(ROOT / "scripts"))
    import capture_ab

    def put(name, vals):
        d = tmp_path / name
        d.mkdir()
        (d / "capture_run.json").write_text(json.dumps({"render_options": vals}), encoding="utf-8")

    for n in ("b1-vk", "b2-vk", "a1-vk"):
        put(n, values())
    put("a2-vk", values(VfxSoftParticles=True))
    with pytest.raises(capture_ab.render_options_pin.OptionsMismatch) as e:
        capture_ab.check_options(str(tmp_path), "b", "a", [])
    assert "VfxSoftParticles" in str(e.value)
    capture_ab.check_options(str(tmp_path), "b", "a", ["VfxSoftParticles"])
    (tmp_path / "a2-vk" / "capture_run.json").unlink()                         # an unrecorded set is refused too
    with pytest.raises(capture_ab.render_options_pin.OptionsMismatch):
        capture_ab.check_options(str(tmp_path), "b", "a", [])


def _tracy_module():
    spec = importlib.util.spec_from_file_location("tracy_capture_compare_under_test", ROOT / "tools" / "tracy_capture_compare.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_tracy_driver_refuses_traces_with_different_options(tmp_path):
    tcc = _tracy_module()
    a, b = tmp_path / "a.tracy", tmp_path / "b.tracy"
    sys.path.insert(0, str(ROOT / "scripts"))
    from smoke_lib import render_options_pin as rop2   # the module the driver itself uses
    rop2.write(tcc.options_sidecar(a), {"values": values()})
    rop2.write(tcc.options_sidecar(b), {"values": values(CpuPhongRetire=False)})
    with pytest.raises(SystemExit) as e:
        tcc.require_same_options(a, b, [], "BEFORE", "AFTER")
    assert "CpuPhongRetire" in str(e.value)
    tcc.require_same_options(a, b, ["CpuPhongRetire"])
    with pytest.raises(SystemExit):
        tcc.require_same_options(a, tmp_path / "never.tracy", [])             # no record
    assert tcc.vary_list(" A, ,B ") == ["A", "B"]


def test_smoke_report_records_the_options():
    from scripts.smoke_lib.gates import Verdict
    from scripts.smoke_lib.logparse import LogSummary
    row = Row(stem="mc2_01", verdict=Verdict(passed=True), summary=LogSummary(), render_options=values(SunAxisFix=True))
    md = render_markdown([row], tier="tier1", profile="stock", timestamp="t")
    assert "Render options:** SunAxisFix=True" in md
    assert render_json([row], tier="tier1", profile="stock", timestamp="t")["rows"][0]["render_options"]["SunAxisFix"] is True
    plain = Row(stem="mc2_01", verdict=row.verdict, summary=row.summary, render_options=values())
    assert "Render options:** all defaults" in render_markdown([plain], tier="tier1", profile="stock", timestamp="t")
    assert "Render options:** not recorded" in render_markdown([Row(stem="x", verdict=row.verdict, summary=row.summary)], tier="tier1", profile="stock", timestamp="t")


def test_high_reflection_stays_the_live_full_res_planar_pass():
    """High (ReflectionQuality 3) renders the live planar pass into a full-res image; fc2ee9c34 once forced it off and shrank the image to 1x1."""
    run = (ROOT / "GameOS/gameos/vulkan_water_island_run.cpp").read_text(encoding="utf-8")
    images = (ROOT / "GameOS/gameos/vulkan_water_island_images.cpp").read_text(encoding="utf-8")
    assert not re.search(r"^ {4}reflectThisFrame\s*=\s*false;", run, re.M)
    assert re.search(r"if \(reflect_bake_active\(\)\) reflectThisFrame = false;", run)
    assert "image.extent = live ? full : VkExtent3D{1,1,1};" in images
