# tools/unit_converter/test_weapon_shadow_compare.py
"""Tests for WEAPON-SHADOW-COMPARE-1: the engine-vs-converter equipment diff.
Run: py -3 -m pytest tools/unit_converter

Covers: input adapters (engine masterComponents dump / converter equipment
report), the per-field diff (match / mismatch / engine-only / converter-only),
weapon-only field gating, float tolerance, the verdict, the waived list, and a
PYTHONHASHSEED determinism guard on the JSON report.
"""
import json
import os
import subprocess
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))  # so `import unit_converter` works

from unit_converter import weapon_shadow_compare as WC  # noqa: E402


# --- fixture builders (neutral dicts mirroring the two real inputs) ---------

def _engine_dump(rows):
    return {"masterComponents": {"version": 1, "rows": rows}}


def _erow(master_id, *, kind="Weapon", is_weapon=True, category="Energy",
          damage=10.0, heat=3.0, recycle=4.0, range_bucket="long",
          combat_value=5.0, ammo_master_id=0, crit_slots=1):
    return {
        "masterId": master_id, "kind": kind, "is_weapon": is_weapon,
        "category": category, "damage": damage, "heat": heat,
        "recycle": recycle, "range_bucket": range_bucket,
        "combat_value": combat_value, "ammo_master_id": ammo_master_id,
        "crit_slots": crit_slots,
    }


def _converter_report(components):
    return {"tool": "LEGACY-COMPBAS-CONVERTER-1", "components": components}


def _cweapon(master_id, *, category="Energy", damage=10.0, heat=3.0,
             recycle=4.0, range_bucket="long", combat_value=5.0,
             ammo_master_id=0, crit_slots=1):
    return {
        "equip_id": f"core:comp{master_id}", "name": f"W{master_id}",
        "kind": "Weapon", "tags": [], "legacy_master_id": master_id,
        "weapon": {
            "category": category, "fire_executor": "beam_direct",
            "damage": damage, "heat": heat, "recycle": recycle,
            "range_bucket": range_bucket, "combat_value": combat_value,
            "ammo_category": "", "ammo_master_id": ammo_master_id,
            "crit_slots": crit_slots,
        },
    }


def _cnonweapon(master_id, kind="Heatsink", crit_slots=1, combat_value=0.0):
    return {
        "equip_id": f"core:comp{master_id}", "name": f"C{master_id}",
        "kind": kind, "tags": [], "legacy_master_id": master_id,
        "combat_value": combat_value, "crit_slots": crit_slots,
    }


# --- adapters ---------------------------------------------------------------

def test_load_engine_components_maps_rows():
    dump = _engine_dump([_erow(6, category="Ballistic", ammo_master_id=71)])
    facets = WC.load_engine_components(dump)
    assert set(facets) == {6}
    f = facets[6]
    assert f.kind == "Weapon"
    assert f.is_weapon is True
    assert f.category == "Ballistic"
    assert f.ammo_master_id == 71


def test_load_converter_components_reads_nested_weapon():
    rep = _converter_report([_cweapon(6, category="Ballistic", ammo_master_id=71)])
    facets = WC.load_converter_components(rep)
    assert facets[6].category == "Ballistic"
    assert facets[6].ammo_master_id == 71
    assert facets[6].is_weapon is True


def test_load_converter_drops_sentinel_negative_ids():
    rep = _converter_report([_cnonweapon(-1)])
    assert WC.load_converter_components(rep) == {}


def test_load_converter_nonweapon_has_no_weapon_fields():
    rep = _converter_report([_cnonweapon(5, kind="Heatsink")])
    f = WC.load_converter_components(rep)[5]
    assert f.is_weapon is False
    assert f.category == ""


# --- the diff: clean match --------------------------------------------------

def test_compare_all_match_is_pass():
    dump = _engine_dump([_erow(6)])
    rep = _converter_report([_cweapon(6)])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    assert r.verdict == "PASS"
    assert r.components_compared == 1
    assert r.field_matches == len(WC.COMMON_FACETS) + len(WC.WEAPON_FACETS)
    assert not r.mismatches


def test_compare_float_tolerance():
    # 10.0 vs 10.00001 must be treated equal.
    dump = _engine_dump([_erow(6, damage=10.0)])
    rep = _converter_report([_cweapon(6, damage=10.00001)])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    assert r.verdict == "PASS"


# --- the diff: mismatch surfacing (the reason the gate exists) --------------

def test_compare_damage_mismatch_surfaced():
    dump = _engine_dump([_erow(6, damage=10.0)])
    rep = _converter_report([_cweapon(6, damage=99.0)])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    assert r.verdict == "MISMATCH"
    assert len(r.mismatches) == 1
    m = r.mismatches[0]
    assert m.master_id == 6 and m.field == "damage"
    assert m.engine == 10.0 and m.converter == 99.0
    assert r.per_field_mismatches["damage"] == 1


def test_compare_ammo_master_id_mismatch_surfaced():
    # A mis-parsed ammo column is exactly what this gate catches.
    dump = _engine_dump([_erow(6, category="Ballistic", ammo_master_id=71)])
    rep = _converter_report([_cweapon(6, category="Ballistic", ammo_master_id=999)])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    assert r.verdict == "MISMATCH"
    assert any(m.field == "ammo_master_id" for m in r.mismatches)


def test_compare_range_bucket_mismatch_surfaced():
    dump = _engine_dump([_erow(6, range_bucket="long")])
    rep = _converter_report([_cweapon(6, range_bucket="short")])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    assert any(m.field == "range_bucket" for m in r.mismatches)


def test_compare_combat_value_compared_for_nonweapons():
    dump = _engine_dump([_erow(5, kind="Heatsink", is_weapon=False,
                               category="", damage=0.0, heat=0.0, recycle=0.0,
                               range_bucket="", combat_value=2.0, crit_slots=1)])
    rep = _converter_report([_cnonweapon(5, kind="Heatsink", combat_value=7.0)])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    assert any(m.field == "combat_value" for m in r.mismatches)


# --- weapon-only field gating -----------------------------------------------

def test_weapon_only_fields_skipped_when_not_both_weapons():
    # engine says weapon, converter says non-weapon -> is_weapon mismatch, but
    # damage/heat/... are NOT compared (would flood spurious diffs).
    dump = _engine_dump([_erow(6, is_weapon=True, damage=10.0)])
    rep = _converter_report([_cnonweapon(6, kind="Heatsink")])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    fields = {m.field for m in r.mismatches}
    assert "is_weapon" in fields or "kind" in fields
    assert "damage" not in fields


# --- set arithmetic ---------------------------------------------------------

def test_engine_only_and_converter_only():
    dump = _engine_dump([_erow(6), _erow(7)])
    rep = _converter_report([_cweapon(6), _cweapon(8)])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    assert r.engine_only == [7]
    assert r.converter_only == [8]
    assert r.components_compared == 1


# --- report shape -----------------------------------------------------------

def test_to_dict_shape_and_waived():
    dump = _engine_dump([_erow(6)])
    rep = _converter_report([_cweapon(6)])
    d = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep)).to_dict()
    assert d["tool"] == "WEAPON-SHADOW-COMPARE-1"
    assert d["verdict"] == "PASS"
    for w in ("fire_executor", "ammo_category", "presentation"):
        assert w in d["waived_fields"]


def test_render_text_lists_mismatch():
    dump = _engine_dump([_erow(6, damage=10.0)])
    rep = _converter_report([_cweapon(6, damage=99.0)])
    r = WC.compare(WC.load_engine_components(dump),
                   WC.load_converter_components(rep))
    txt = WC.render_text(r)
    assert "MISMATCH" in txt
    assert "masterId 6 damage" in txt


# --- CLI + determinism ------------------------------------------------------

def _write(tmp_path, name, obj):
    p = tmp_path / name
    p.write_text(json.dumps(obj), encoding="utf-8")
    return str(p)


def test_cli_json_pass_exit0(tmp_path):
    ed = _write(tmp_path, "dump.json", _engine_dump([_erow(6)]))
    cr = _write(tmp_path, "conv.json", _converter_report([_cweapon(6)]))
    r = subprocess.run(
        [sys.executable, os.path.join(_HERE, "weapon_shadow_compare.py"),
         "--engine-dump", ed, "--converter-report", cr, "--json"],
        capture_output=True, text=True)
    assert r.returncode == 0
    out = json.loads(r.stdout)
    assert out["verdict"] == "PASS"


def test_cli_strict_mismatch_exit1(tmp_path):
    ed = _write(tmp_path, "dump.json", _engine_dump([_erow(6, damage=10.0)]))
    cr = _write(tmp_path, "conv.json", _converter_report([_cweapon(6, damage=99.0)]))
    r = subprocess.run(
        [sys.executable, os.path.join(_HERE, "weapon_shadow_compare.py"),
         "--engine-dump", ed, "--converter-report", cr, "--strict"],
        capture_output=True, text=True)
    assert r.returncode == 1


def test_json_report_hashseed_deterministic(tmp_path):
    ed = _write(tmp_path, "dump.json", _engine_dump([_erow(6), _erow(7)]))
    cr = _write(tmp_path, "conv.json",
                _converter_report([_cweapon(6), _cweapon(8)]))
    outs = []
    for seed in ("0", "1"):
        env = dict(os.environ, PYTHONHASHSEED=seed)
        r = subprocess.run(
            [sys.executable, os.path.join(_HERE, "weapon_shadow_compare.py"),
             "--engine-dump", ed, "--converter-report", cr, "--json"],
            capture_output=True, text=True, env=env)
        assert r.returncode == 0
        outs.append(r.stdout)
    assert outs[0] == outs[1]
