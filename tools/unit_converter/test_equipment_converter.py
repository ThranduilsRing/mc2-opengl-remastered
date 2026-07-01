# tools/unit_converter/test_equipment_converter.py
"""LEGACY-COMPBAS-CONVERTER-1 (equipment arc, slice 4) tests.

Covers extraction, emit block/token shape, determinism (PYTHONHASHSEED guard),
legacyMasterId->id alias mapping, and graceful malformed-row handling. STOCK-ONLY
fixture (byte-identical to mc2srcdata/objects/compbas.csv).
"""
import json
import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from unit_converter import equipment_emit as emit          # noqa: E402
from unit_converter import equipment_extract as extract     # noqa: E402
from unit_converter import equipment_model as EM            # noqa: E402
from unit_converter import equipment_converter as cli       # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(_HERE, "tests", "fixtures", "stock")
COMPBAS = os.path.join(FIXTURE, "objects", "compbas.csv")


@pytest.fixture(scope="module")
def converted():
    rows = extract.load_compbas_rows(COMPBAS)
    return extract.extract_all(rows)


# --- extraction -----------------------------------------------------------

def test_stock_compbas_present():
    assert os.path.isfile(COMPBAS)


def test_converts_expected_component_count(converted):
    defs, waivers = converted
    # 256 CSV rows: 1 header + 255 component slots. 125 undefined + 50 removed
    # are waived -> 80 live components converted.
    assert len(defs) == 80
    assert len(waivers) == 175


def test_per_kind_counts(converted):
    defs, _ = converted
    from collections import Counter
    c = Counter(d.kind for d in defs)
    assert c[EM.EK_WEAPON] == 37
    assert c[EM.EK_AMMO] == 21
    assert c[EM.EK_SENSOR] == 10
    assert c[EM.EK_JUMPJET] == 1
    assert c[EM.EK_ENGINE] == 1


def test_every_def_has_valid_core_id(converted):
    from unit_converter import qualified_id
    defs, _ = converted
    for d in defs:
        assert d.equip_id.startswith("core:")
        assert qualified_id.is_valid_qualified_id(d.equip_id)


def test_weapon_facet_present_only_for_weapons(converted):
    defs, _ = converted
    for d in defs:
        if d.kind == EM.EK_WEAPON:
            assert d.weapon is not None
        else:
            assert d.weapon is None


# --- legacyMasterId ALIAS ------------------------------------------------

def test_legacy_master_id_is_row_ordinal(converted):
    defs, _ = converted
    by_id = {d.equip_id: d for d in defs}
    # PPC is compbas row 145.
    assert by_id["core:ppc"].legacy_master_id == 145
    # Jump Jets is row 10.
    assert by_id["core:jump_jets"].legacy_master_id == 10


def test_legacy_master_id_alias_map_is_bijective(converted):
    defs, _ = converted
    ids = [d.legacy_master_id for d in defs]
    assert len(ids) == len(set(ids)), "legacyMasterId must be unique per component"
    canon = [d.equip_id for d in defs]
    assert len(canon) == len(set(canon)), "canonical ids must be unique"


# --- fireExecutor default mapping ----------------------------------------

def test_fire_executor_defaults(converted):
    defs, _ = converted
    by_id = {d.equip_id: d for d in defs}
    assert by_id["core:ppc"].weapon.fire_executor == EM.FX_BEAM_DIRECT
    # A ballistic autocannon -> projectile_ballistic.
    ac10 = by_id["core:autocannon_ac10"]
    assert ac10.weapon.category == EM.WT_BALLISTIC
    assert ac10.weapon.fire_executor == EM.FX_PROJECTILE_BALLISTIC
    # Long Tom Cannon carries the Artillery field -> artillery_indirect.
    long_tom = by_id["core:long_tom_cannon"]
    assert long_tom.weapon.fire_executor == EM.FX_ARTILLERY_INDIRECT
    # A streak SRM -> missile_streak.
    streak = by_id["core:streak_srm2_pack"]
    assert streak.weapon.category == EM.WT_MISSILE
    assert streak.weapon.fire_executor == EM.FX_MISSILE_STREAK


def test_energy_weapons_have_no_ammo_category(converted):
    defs, _ = converted
    for d in defs:
        if d.weapon and d.weapon.category == EM.WT_ENERGY:
            assert d.weapon.ammo.ammo_category == ""


# --- emit shape -----------------------------------------------------------

def test_emit_weapon_blocks_and_tokens(converted):
    defs, _ = converted
    ppc = next(d for d in defs if d.equip_id == "core:ppc")
    text = emit.emit_equipment_fit(ppc)
    for tok in ("FITini", "FITend", "[Equipment]", 'st EquipmentId="core:ppc"',
                "l LegacyMasterId=145", "[WeaponFacet]", 'st Category="Energy"',
                'st FireExecutor="beam_direct"', "[WeaponCombatStats]",
                "[WeaponAmmo]", "[WeaponFit]", "[Presentation]"):
        assert tok in text, tok


def test_emit_non_weapon_has_no_weapon_blocks(converted):
    defs, _ = converted
    jj = next(d for d in defs if d.equip_id == "core:jump_jets")
    text = emit.emit_equipment_fit(jj)
    assert "[WeaponFacet]" not in text
    assert 'st Kind="JumpJet"' in text


def test_write_equipment_roundtrip(tmp_path, converted):
    defs, _ = converted
    ppc = next(d for d in defs if d.equip_id == "core:ppc")
    path = emit.write_equipment(ppc, str(tmp_path))
    assert path.endswith(os.path.join("core", "ppc", "equipmentdef.fit"))
    with open(path, encoding="latin-1") as fh:
        assert 'st EquipmentId="core:ppc"' in fh.read()


# --- malformed row handling ----------------------------------------------

def test_malformed_row_is_waived_not_crashed():
    ed, waiver = extract.extract_equipment(["notanumber", "EnergyWeapon", "Bad"])
    assert ed is None
    assert waiver and "malformed" in waiver


def test_undefined_and_removed_rows_waived():
    ed, w = extract.extract_equipment(["0", "undefined", "undefined"] + ["0"] * 26)
    assert ed is None and "undefined" in w
    ed, w = extract.extract_equipment(["2", "Removed", "XL Fusion Engine"] + ["0"] * 26)
    assert ed is None and "Removed" in w


def test_short_row_does_not_crash():
    # A truncated but well-formed weapon row extracts with zero-filled stats.
    ed, waiver = extract.extract_equipment(["200", "EnergyWeapon", "Stub Laser"])
    assert waiver is None
    assert ed is not None
    assert ed.kind == EM.EK_WEAPON
    assert ed.legacy_master_id == 200


# --- determinism (PYTHONHASHSEED guard) ----------------------------------

def _run_json(seed):
    env = dict(os.environ, PYTHONHASHSEED=str(seed))
    out = subprocess.check_output(
        [sys.executable, os.path.join(_HERE, "equipment_converter.py"), "--json"],
        env=env)
    return out


def test_json_report_byte_identical_across_hashseeds():
    a = _run_json(0)
    b = _run_json(424242)
    assert a == b, "JSON report must be deterministic across PYTHONHASHSEED"
    # sanity: it IS valid JSON with the expected top-level shape.
    obj = json.loads(a)
    assert obj["components_converted"] == 80
    assert obj["tool"] == "LEGACY-COMPBAS-CONVERTER-1"
    assert "legacy_master_id_to_id" in obj


def test_report_alias_map_matches_defs(converted):
    defs, waivers = converted
    report = cli.build_report(defs, waivers)
    amap = report["legacy_master_id_to_id"]
    assert amap["145"] == "core:ppc"
    assert amap["10"] == "core:jump_jets"
    assert len(amap) == len(defs)
