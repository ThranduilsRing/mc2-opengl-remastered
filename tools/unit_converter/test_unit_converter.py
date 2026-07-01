# tools/unit_converter/test_unit_converter.py
"""Tests for LEGACY-UNIT-CONVERTER-1: extraction, emit format (vs the engine
loader tokens), and determinism. Run: py -3 -m pytest tools/unit_converter

Uses the bundled stock fixtures (atlas mech + armoredcar vehicle + compbas).
"""
import json
import os
import subprocess
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))  # so `import unit_converter` works

from unit_converter import emit, model as M          # noqa: E402
from unit_converter.extract import extract_mech, extract_vehicle  # noqa: E402
from unit_converter.fitparse import ChassisCsv, CompBas, FitIni    # noqa: E402

FIXTURES = os.path.join(_HERE, "tests", "fixtures", "stock")


@pytest.fixture(scope="module")
def compbas():
    return CompBas.load(os.path.join(FIXTURES, "objects", "compbas.csv"))


@pytest.fixture(scope="module")
def atlas(compbas):
    return extract_mech(
        os.path.join(FIXTURES, "objects", "atlas.csv"),
        os.path.join(FIXTURES, "tgl", "atlas.ini"),
        compbas,
    )


@pytest.fixture(scope="module")
def armoredcar(compbas):
    return extract_vehicle(
        os.path.join(FIXTURES, "objects", "armoredcar.fit"),
        os.path.join(FIXTURES, "tgl", "armoredcar.ini"),
        compbas,
    )


# --- parser --------------------------------------------------------------

def test_fitini_parses_blocks_and_types():
    fit = FitIni.parse(
        'FITini\n[Header]\nst FileType="GroundVehicleType"\n'
        "[InternalStructure]\nuc Front=1\nuc Turret=1\nFITend\n"
    )
    assert fit.get("Header", "FileType") == "GroundVehicleType"
    assert fit.get("InternalStructure", "Front") == 1
    assert fit.has_block("InternalStructure")


def test_fitini_strips_comments():
    fit = FitIni.parse("FITini\n[Nodes]\nl NumWeapon = 5  // five weapons\nFITend\n")
    assert fit.get("Nodes", "NumWeapon") == 5


def test_chassis_csv_labels_and_first_variant():
    csvf = ChassisCsv.load(os.path.join(FIXTURES, "objects", "atlas.csv"))
    assert csvf.label("MechName") == "Atlas"
    assert csvf.label("Tonnage") == "100"
    assert csvf.first_variant_name() == "Atlas Prime"
    # first-variant scoping excludes later variants' items
    all_items = csvf.item_rows(first_variant_only=False)
    prime_items = csvf.item_rows(first_variant_only=True)
    assert len(prime_items) < len(all_items)


def test_compbas_weapon_type_lookup(compbas):
    assert compbas.type_of(143) == "EnergyWeapon"    # ER Laser
    assert compbas.type_of(114) == "BallisticWeapon"  # MG Array
    assert compbas.type_of(10) == "JumpJet"


# --- mech extraction -----------------------------------------------------

def test_mech_identity_and_facets(atlas):
    assert atlas.identity.unit_id == "core:atlas"
    assert atlas.identity.kind == "mech"
    assert atlas.identity.display_name == "Atlas Prime"
    assert atlas.identity.tonnage == 100.0
    assert atlas.locomotion.model == M.LOCO_MECH_WALKER
    assert atlas.damage.schema == M.DMG_MECH_8PART
    assert len(atlas.damage.locations) == 8


def test_mech_presentation_sockets(atlas):
    # tgl atlas.ini declares NumWeapon=5 weapon nodes
    assert len(atlas.presentation.sockets) == 5
    names = [s.name for s in atlas.presentation.sockets]
    assert "weapon_rightarm" in names
    assert atlas.presentation.model_name == "Atlas"


def test_mech_prime_has_no_jump(atlas):
    # Atlas Prime installs no jumpjet component -> jump False (later variants do)
    assert atlas.capabilities.jump is False


def test_mech_hardpoints_named_not_indexed(atlas):
    assert len(atlas.hardpoints) == 5
    for hp in atlas.hardpoints:
        assert hp.hardpoint_id.startswith("atlas_")
        assert not hp.hardpoint_id[-1:].isupper()  # names, lower-cased tags


def test_mech_loadout_binds_up_to_socket_count(atlas):
    bound = [e for e in atlas.loadout if e.hardpoint_id]
    # more stock weapons than model sockets -> only socket-count many bind,
    # the rest are surfaced as waivers (the recon's least-used-node crux)
    assert len(bound) == len(atlas.presentation.sockets)
    assert any("no compatible hardpoint" in w for w in atlas.waivers)


# --- vehicle extraction --------------------------------------------------

def test_vehicle_identity_and_facets(armoredcar):
    assert armoredcar.identity.unit_id == "core:armoredcar"
    assert armoredcar.identity.kind == "vehicle"
    assert armoredcar.locomotion.model == M.LOCO_WHEELED
    assert armoredcar.damage.schema == M.DMG_VEHICLE_FACINGS
    assert set(armoredcar.damage.locations) == {"Front", "Left", "Right", "Rear", "Turret"}
    assert armoredcar.capabilities.jump is False


def test_vehicle_loadout_and_turret_hardpoint(armoredcar):
    # armoredcar mounts one weapon (SRM/MG, MasterID 114 -> Ballistic tag)
    assert len(armoredcar.loadout) == 1
    entry = armoredcar.loadout[0]
    assert entry.master_id == 114
    assert entry.hardpoint_id == "armoredcar_turret_1"
    # stock vehicle tgl has no weapon nodes -> socket-less hardpoint waiver
    assert any("no model socket" in w for w in armoredcar.waivers)


def test_vehicle_ui_provenance(armoredcar):
    assert armoredcar.ui.icon_index == 2
    assert armoredcar.ui.name_string_id == 30302


# --- emit format (must match engine loader tokens) -----------------------

def test_emit_unitprofile_tokens(atlas):
    text = emit.emit_unit_fit(atlas)
    # exact tokens code/unitprofile_fit.cpp::populateProfileFromFit reads
    assert "[UnitProfile]" in text
    assert "b Jump=FALSE" in text
    assert 'st Locomotion="MechWalker"' in text
    assert 'st DamageSchema="Mech8Part"' in text


def test_emit_presentation_sockets_format(atlas):
    text = emit.emit_presentation_fit(atlas)
    # exact tokens mclib/mech3d.cpp PresentationSockets loader reads
    assert "[PresentationSockets]" in text
    assert "l NumSockets = 5" in text
    assert "[Socket0]" in text
    assert 'st SocketName = "weapon_rightarm"' in text


def test_emit_uiprofile_tokens(atlas):
    text = emit.emit_ui_fit(atlas)
    assert "[UIProfile]" in text
    assert 'st Icon="MCL_GN_MECHICONATLAS"' in text
    assert "l NameID=28919" in text


def test_emit_roundtrips_through_the_parser(atlas):
    # generated unit.fit must be re-parseable by the same FitIni parser (a
    # proxy for engine consumability of the [UnitProfile] block)
    fit = FitIni.parse(emit.emit_unit_fit(atlas))
    assert fit.get("UnitProfile", "Locomotion") == "MechWalker"
    assert fit.get("UnitProfile", "Jump") is False


def test_write_unit_creates_expected_files(atlas, tmp_path):
    unit_dir = emit.write_unit(atlas, str(tmp_path))
    for fname in ("unit.fit", "presentation.fit", "ui.fit", "hardpoints.fit", "loadout.fit"):
        assert os.path.isfile(os.path.join(unit_dir, fname))


# --- determinism ---------------------------------------------------------

def test_emit_is_byte_stable(atlas):
    a = emit.emit_all(atlas)
    b = emit.emit_all(atlas)
    assert a == b


def test_deterministic_under_hash_randomization():
    """Re-run the full CLI in a fresh interpreter with two different
    PYTHONHASHSEED values; JSON report must be byte-identical (mirrors the
    campaign_gen determinism guard)."""
    script = (
        "import sys, os; "
        f"sys.path.insert(0, {json.dumps(os.path.dirname(_HERE))}); "
        "from unit_converter.unit_converter import convert, build_report; "
        "import json as J; "
        f"d = convert({json.dumps(FIXTURES)}, 'atlas', 'armoredcar'); "
        "print(J.dumps(build_report(d), sort_keys=True))"
    )
    env1 = dict(os.environ, PYTHONHASHSEED="0")
    env2 = dict(os.environ, PYTHONHASHSEED="98765")
    out1 = subprocess.check_output([sys.executable, "-c", script], env=env1)
    out2 = subprocess.check_output([sys.executable, "-c", script], env=env2)
    assert out1 == out2
    assert json.loads(out1)["units_converted"] == 2
