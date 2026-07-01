# tools/unit_converter/test_shadow_compare.py
"""Tests for UNIT-DEFINITION-SHADOW-COMPARE-1: the engine-vs-converter facet
diff. Run: py -3 -m pytest tools/unit_converter

Covers: input adapters (engine dump rows / converter report), the per-facet
diff (match / mismatch / engine-only / converter-only), the verdict, the waived
list, and a PYTHONHASHSEED determinism guard on the JSON report.
"""
import json
import os
import subprocess
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))  # so `import unit_converter` works

from unit_converter import shadow_compare as SC  # noqa: E402


# --- fixture builders (neutral dicts mirroring the two real inputs) ---------

def _engine_dump(rows):
    return {"unitProfile": {"version": 1, "rows": rows}}


def _row(chassis, jump, loco, dmg, object_id=1):
    return {
        "objectId": object_id,
        "cap_jump_baseline": jump,
        "cap_jump_current": jump,
        "can_perform_jump": jump,
        "chassis": chassis,
        "locomotion": loco,
        "damage_schema": dmg,
    }


def _converter_report(units):
    return {"tool": "LEGACY-UNIT-CONVERTER-1", "units": units}


def _cunit(unit_id, jump, loco, dmg):
    return {"unit_id": unit_id, "jump": jump,
            "locomotion": loco, "damage_schema": dmg}


# --- join key ---------------------------------------------------------------

def test_chassis_to_unit_id_lowercases_and_namespaces():
    assert SC.chassis_to_unit_id("Atlas") == "core:atlas"
    assert SC.chassis_to_unit_id("  ARMOREDCAR ") == "core:armoredcar"


# --- adapters ---------------------------------------------------------------

def test_load_engine_facets_maps_rows():
    dump = _engine_dump([_row("atlas", 1, "MechWalker", "Mech8Part")])
    facets = SC.load_engine_facets(dump)
    assert "core:atlas" in facets
    f = facets["core:atlas"]
    assert f.jump is True
    assert f.locomotion == "MechWalker"
    assert f.damage_schema == "Mech8Part"


def test_load_engine_facets_skips_rows_without_chassis():
    dump = _engine_dump([_row("", 0, "Wheeled", "VehicleFacings")])
    assert SC.load_engine_facets(dump) == {}


def test_load_engine_facets_absent_section_is_empty():
    assert SC.load_engine_facets({}) == {}


def test_load_converter_facets_maps_units():
    rep = _converter_report([_cunit("core:atlas", True, "MechWalker", "Mech8Part")])
    facets = SC.load_converter_facets(rep)
    assert facets["core:atlas"].locomotion == "MechWalker"


# --- the diff: agreement ----------------------------------------------------

def test_compare_all_match_is_pass():
    engine = SC.load_engine_facets(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part"),
        _row("armoredcar", 0, "Wheeled", "VehicleFacings"),
    ]))
    conv = SC.load_converter_facets(_converter_report([
        _cunit("core:atlas", True, "MechWalker", "Mech8Part"),
        _cunit("core:armoredcar", False, "Wheeled", "VehicleFacings"),
    ]))
    rep = SC.compare(engine, conv)
    assert rep.verdict == "PASS"
    assert rep.units_compared == 2
    assert rep.matches == 6          # 2 units x 3 facets
    assert not rep.mismatches
    assert rep.per_facet_matches == {"jump": 2, "locomotion": 2, "damage_schema": 2}


# --- the diff: disagreement (the whole point of the tool) -------------------

def test_compare_locomotion_mismatch_is_finding():
    engine = SC.load_engine_facets(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part")]))
    conv = SC.load_converter_facets(_converter_report([
        _cunit("core:atlas", True, "Wheeled", "Mech8Part")]))  # WRONG loco
    rep = SC.compare(engine, conv)
    assert rep.verdict == "MISMATCH"
    assert len(rep.mismatches) == 1
    d = rep.mismatches[0]
    assert d.facet == "locomotion"
    assert d.engine == "MechWalker" and d.converter == "Wheeled"
    # the other two facets still matched
    assert rep.per_facet_matches["jump"] == 1
    assert rep.per_facet_matches["damage_schema"] == 1


def test_compare_jump_mismatch_is_finding():
    engine = SC.load_engine_facets(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part")]))
    conv = SC.load_converter_facets(_converter_report([
        _cunit("core:atlas", False, "MechWalker", "Mech8Part")]))
    rep = SC.compare(engine, conv)
    assert rep.verdict == "MISMATCH"
    assert rep.mismatches[0].facet == "jump"


# --- set differences --------------------------------------------------------

def test_engine_only_and_converter_only_are_not_mismatches():
    engine = SC.load_engine_facets(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part"),
        _row("madcat", 1, "MechWalker", "Mech8Part")]))
    conv = SC.load_converter_facets(_converter_report([
        _cunit("core:atlas", True, "MechWalker", "Mech8Part"),
        _cunit("core:thor", True, "MechWalker", "Mech8Part")]))
    rep = SC.compare(engine, conv)
    assert rep.units_compared == 1              # only atlas is common
    assert rep.engine_only == ["core:madcat"]
    assert rep.converter_only == ["core:thor"]
    assert rep.verdict == "PASS"                # set diffs are not mismatches


# --- report shape / waivers -------------------------------------------------

def test_report_lists_waived_facets():
    rep = SC.compare({}, {})
    d = rep.to_dict()
    assert set(d["waived_facets"]) == set(SC.WAIVED_FACETS)
    assert "hardpoints" in d["waived_facets"]
    assert d["verdict"] == "PASS"               # nothing to compare == vacuous pass


def test_to_dict_is_json_serializable():
    engine = SC.load_engine_facets(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part")]))
    conv = SC.load_converter_facets(_converter_report([
        _cunit("core:atlas", True, "Wheeled", "Mech8Part")]))
    rep = SC.compare(engine, conv)
    s = json.dumps(rep.to_dict(), sort_keys=True)   # must not raise
    assert json.loads(s)["verdict"] == "MISMATCH"


# --- same-chassis facet conflict surfaces rather than hides -----------------

def test_same_chassis_conflicting_movers_surface_as_conflict_key():
    engine = SC.load_engine_facets(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part", object_id=1),
        _row("atlas", 0, "Wheeled", "VehicleFacings", object_id=2)]))
    assert "core:atlas" in engine
    assert "core:atlas#conflict" in engine


# --- CLI + determinism ------------------------------------------------------

def test_cli_json_and_strict_exit(tmp_path):
    dump_p = tmp_path / "dump.json"
    conv_p = tmp_path / "conv.json"
    dump_p.write_text(json.dumps(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part")])), encoding="utf-8")
    conv_p.write_text(json.dumps(_converter_report([
        _cunit("core:atlas", True, "Wheeled", "Mech8Part")])), encoding="utf-8")

    script = (
        "import sys, os; "
        f"sys.path.insert(0, {json.dumps(os.path.dirname(_HERE))}); "
        "from unit_converter.shadow_compare import main; "
        f"sys.exit(main(['--engine-dump', {json.dumps(str(dump_p))}, "
        f"'--converter-report', {json.dumps(str(conv_p))}, '--json', '--strict']))"
    )
    r = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True)
    assert r.returncode == 1                      # --strict + mismatch
    payload = json.loads(r.stdout)
    assert payload["verdict"] == "MISMATCH"
    assert payload["facet_mismatches"] == 1


def test_deterministic_under_hash_randomization(tmp_path):
    """JSON report byte-identical across PYTHONHASHSEED values (mirrors the
    converter/campaign_gen determinism guard)."""
    dump_p = tmp_path / "dump.json"
    conv_p = tmp_path / "conv.json"
    dump_p.write_text(json.dumps(_engine_dump([
        _row("atlas", 1, "MechWalker", "Mech8Part"),
        _row("armoredcar", 0, "Wheeled", "VehicleFacings")])), encoding="utf-8")
    conv_p.write_text(json.dumps(_converter_report([
        _cunit("core:armoredcar", False, "Wheeled", "VehicleFacings"),
        _cunit("core:atlas", True, "MechWalker", "Mech8Part")])), encoding="utf-8")

    script = (
        "import sys, os, json as J; "
        f"sys.path.insert(0, {json.dumps(os.path.dirname(_HERE))}); "
        "from unit_converter.shadow_compare import "
        "load_engine_facets, load_converter_facets, compare; "
        f"e = load_engine_facets(J.load(open({json.dumps(str(dump_p))}))); "
        f"c = load_converter_facets(J.load(open({json.dumps(str(conv_p))}))); "
        "print(J.dumps(compare(e, c).to_dict(), sort_keys=True))"
    )
    env1 = dict(os.environ, PYTHONHASHSEED="0")
    env2 = dict(os.environ, PYTHONHASHSEED="98765")
    out1 = subprocess.check_output([sys.executable, "-c", script], env=env1)
    out2 = subprocess.check_output([sys.executable, "-c", script], env=env2)
    assert out1 == out2
    assert json.loads(out1)["verdict"] == "PASS"
