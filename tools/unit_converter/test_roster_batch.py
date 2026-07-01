# tools/unit_converter/test_roster_batch.py
"""Tests for CONVERTER-ROSTER-BATCH-1: roster enumeration from the master index,
aggregate batch conversion report, graceful skip of missing/malformed rows, and
the roster-wide shadow-compare diff. Run: py -3 -m pytest tools/unit_converter

Uses the bundled stock fixtures (atlas mech + armoredcar vehicle + compbas) plus
a synthetic buildings_roster.csv master index that intentionally mixes:
  - the header row (Type == "Type")            -> skipped by enumeration
  - BUILDING / TREE species                    -> not convertible, skipped
  - Atlas (MECH) + ArmoredCar (VEHICLE)         -> convertible (sources present)
  - GhostTank / GhostMech                       -> convertible type, no source
                                                    -> SkipRecord with reason
  - a truncated row + a blank-name row          -> skipped by enumeration
"""
import json
import os
import subprocess
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))  # so `import unit_converter` works

from unit_converter import roster as R                    # noqa: E402
from unit_converter import shadow_compare as SC           # noqa: E402
from unit_converter import roster_shadow_compare as RSC   # noqa: E402

FIXTURES = os.path.join(_HERE, "tests", "fixtures")
SRCDATA = os.path.join(FIXTURES, "stock")
BUILDINGS = os.path.join(FIXTURES, "buildings_roster.csv")


# --- enumeration ---------------------------------------------------------

def test_enumerate_roster_keeps_only_mech_and_vehicle():
    entries = R.enumerate_roster(BUILDINGS)
    kinds = [(e.name, e.kind) for e in entries]
    # Atlas(MECH), ArmoredCar(VEHICLE), GhostTank(VEHICLE), GhostMech(MECH).
    # header / BUILDING / TREE / truncated / blank-name rows are all dropped.
    assert kinds == [
        ("Atlas", "mech"),
        ("ArmoredCar", "vehicle"),
        ("GhostTank", "vehicle"),
        ("GhostMech", "mech"),
    ]


def test_enumerate_lowercases_stem():
    entries = R.enumerate_roster(BUILDINGS)
    stems = {e.stem for e in entries}
    assert stems == {"atlas", "armoredcar", "ghosttank", "ghostmech"}


def test_enumerate_ignores_malformed_rows_without_crashing():
    # truncated (1 col) + blank-name rows must not appear and must not raise
    entries = R.enumerate_roster(BUILDINGS)
    assert all(e.name for e in entries)
    assert "Truncated" not in {e.name for e in entries}


# --- batch conversion + aggregate report ---------------------------------

def test_convert_roster_converts_present_and_skips_missing():
    result = R.convert_roster(SRCDATA, BUILDINGS)
    # LOADOUT-THREE-LAYER-1: atlas -> 3 variant defs + armoredcar -> 1 def.
    converted = {d.identity.unit_id for d in result.defs}
    assert converted == {
        "core:atlas_atlas_prime", "core:atlas_claymores_atlas",
        "core:atlas_renards_atlas", "core:armoredcar",
    }
    # one shared chassis emitted for the mech (vehicles have no chassisdef)
    assert {c.chassis_id for c in result.chassis} == {"core:atlas"}
    skipped = {s.stem for s in result.skipped}
    assert skipped == {"ghosttank", "ghostmech"}
    for s in result.skipped:
        assert "missing source" in s.reason  # reason is surfaced, not swallowed


def test_batch_report_counts_and_shape():
    result = R.convert_roster(SRCDATA, BUILDINGS)
    rep = R.build_batch_report(result)
    assert rep["tool"] == "CONVERTER-ROSTER-BATCH-1"
    # per-VARIANT def count: 3 atlas variants + 1 armoredcar
    assert rep["units_converted"] == 4
    assert rep["chassis_converted"] == 1
    assert rep["mechs"] == 3
    assert rep["vehicles"] == 1
    assert rep["units_skipped"] == 2
    # units sorted by unit_id; each carries the per-unit facets + waivers
    ids = [u["unit_id"] for u in rep["units"]]
    assert ids == sorted(ids)
    assert {u["unit_id"] for u in rep["units"]} == {
        "core:atlas_atlas_prime", "core:atlas_claymores_atlas",
        "core:atlas_renards_atlas", "core:armoredcar",
    }
    assert all("jump" in u and "locomotion" in u and "waivers" in u for u in rep["units"])
    # each unit carries its variant identity
    assert all("chassis_id" in u and "variant_name" in u for u in rep["units"])
    # skipped entries carry stem + kind + reason
    assert {s["stem"] for s in rep["skipped"]} == {"ghosttank", "ghostmech"}
    assert all(s["reason"] for s in rep["skipped"])


def test_batch_report_total_waivers_is_sum():
    result = R.convert_roster(SRCDATA, BUILDINGS)
    rep = R.build_batch_report(result)
    assert rep["total_waivers"] == sum(len(d.waivers) for d in result.defs)


def test_batch_writes_sidecars_when_out_given(tmp_path):
    result = R.convert_roster(SRCDATA, BUILDINGS, out_root=str(tmp_path))
    # 1 chassisdef + 3 mech-variant dirs + 1 vehicle dir = 5 written paths
    assert len(result.written_dirs) == 5
    core = os.path.join(str(tmp_path), "core")
    # shared chassisdef emitted once at the chassis dir
    assert os.path.isfile(os.path.join(core, "atlas", "chassisdef.fit"))
    # each mech variant gets its own subdir under the chassis
    for vslug in ("atlas_prime", "claymores_atlas", "renards_atlas"):
        assert os.path.isfile(os.path.join(core, "atlas", vslug, "unit.fit"))
    # vehicle keeps the flat chassis dir
    assert os.path.isfile(os.path.join(core, "armoredcar", "unit.fit"))


# --- roster shadow-compare diff ------------------------------------------

def _dump(rows):
    return {"unitProfile": {"rows": rows}}


def test_roster_diff_all_match_is_pass():
    # engine agrees with the converter (variant-aware join) -> PASS
    result = R.convert_roster(SRCDATA, BUILDINGS)
    batch = R.build_batch_report(result)
    converter = SC.load_converter_facets(batch)
    dump = _dump([
        {"chassis": "atlas", "variant": "Atlas Prime", "cap_jump_baseline": False,
         "locomotion": "MechWalker", "damage_schema": "Mech8Part"},
        {"chassis": "armoredcar", "variant": "", "cap_jump_baseline": False,
         "locomotion": "Wheeled", "damage_schema": "VehicleFacings"},
    ])
    engine = SC.load_engine_facets(dump)
    rep = SC.compare(engine, converter)
    assert rep.verdict == "PASS"
    assert rep.units_compared == 2
    assert rep.per_facet_matches == {"jump": 2, "locomotion": 2, "damage_schema": 2}
    assert not rep.mismatches


def test_variant_aware_join_resolves_atlas_jump():
    # THE ATLAS FINDING, RESOLVED: the engine spawns Renard's Atlas (jumpjets ->
    # jump TRUE). Under the OLD chassis-stem join it diffed against Atlas PRIME
    # (jump FALSE) -> false mismatch. The variant-aware join now diffs against the
    # SAME converted variant (Renard's Atlas) -> jump matches, PASS.
    result = R.convert_roster(SRCDATA, BUILDINGS)
    batch = R.build_batch_report(result)
    converter = SC.load_converter_facets(batch)
    dump = _dump([
        {"chassis": "atlas", "variant": "Renard's Atlas", "cap_jump_baseline": True,
         "locomotion": "MechWalker", "damage_schema": "Mech8Part"},
    ])
    engine = SC.load_engine_facets(dump)
    # engine row joined to the per-variant converter id, not the chassis stem
    assert "core:atlas_renards_atlas" in engine
    rep = SC.compare(engine, converter)
    assert rep.verdict == "PASS"          # jump TRUE == converter Renard jump TRUE
    assert not rep.mismatches


def test_roster_diff_surfaces_facet_mismatch():
    # engine says Atlas Prime jumps, converter (Prime) says it doesn't -> FINDING
    result = R.convert_roster(SRCDATA, BUILDINGS)
    batch = R.build_batch_report(result)
    converter = SC.load_converter_facets(batch)
    dump = _dump([
        {"chassis": "atlas", "variant": "Atlas Prime", "cap_jump_baseline": True,
         "locomotion": "MechWalker", "damage_schema": "Mech8Part"},
    ])
    engine = SC.load_engine_facets(dump)
    rep = SC.compare(engine, converter)
    assert rep.verdict == "MISMATCH"
    assert len(rep.mismatches) == 1
    m = rep.mismatches[0]
    assert (m.unit_id, m.facet, m.engine, m.converter) == \
        ("core:atlas_atlas_prime", "jump", True, False)


def test_roster_diff_reports_engine_only_and_converter_only():
    result = R.convert_roster(SRCDATA, BUILDINGS)
    batch = R.build_batch_report(result)
    converter = SC.load_converter_facets(batch)
    # dump has atlas Prime (converted) + legion (never converted; engine-only).
    # Other atlas variants + armoredcar are converted but absent -> converter-only.
    dump = _dump([
        {"chassis": "atlas", "variant": "Atlas Prime", "cap_jump_baseline": False,
         "locomotion": "MechWalker", "damage_schema": "Mech8Part"},
        {"chassis": "legion", "variant": "", "cap_jump_baseline": False,
         "locomotion": "Wheeled", "damage_schema": "VehicleFacings"},
    ])
    engine = SC.load_engine_facets(dump)
    rep = SC.compare(engine, converter)
    assert rep.engine_only == ["core:legion"]
    assert "core:armoredcar" in rep.converter_only


def test_merge_engine_dumps_unions_multi_mission():
    d1 = _dump([{"chassis": "atlas", "variant": "Atlas Prime", "cap_jump_baseline": False,
                 "locomotion": "MechWalker", "damage_schema": "Mech8Part"}])
    d2 = _dump([{"chassis": "armoredcar", "variant": "", "cap_jump_baseline": False,
                 "locomotion": "Wheeled", "damage_schema": "VehicleFacings"}])
    merged = RSC.merge_engine_dumps([d1, d2])
    assert set(merged) == {"core:atlas_atlas_prime", "core:armoredcar"}


def test_merge_engine_dumps_surfaces_cross_mission_conflict():
    # same chassis+variant, divergent facet across two missions -> #conflict entry
    d1 = _dump([{"chassis": "atlas", "variant": "Atlas Prime", "cap_jump_baseline": False,
                 "locomotion": "MechWalker", "damage_schema": "Mech8Part"}])
    d2 = _dump([{"chassis": "atlas", "variant": "Atlas Prime", "cap_jump_baseline": True,
                 "locomotion": "MechWalker", "damage_schema": "Mech8Part"}])
    merged = RSC.merge_engine_dumps([d1, d2])
    assert any(k.endswith("#conflict") for k in merged)


def test_roster_shadow_compare_run_end_to_end(tmp_path):
    # full run() path: batch-convert fixtures + diff a written dump
    dump_path = os.path.join(str(tmp_path), "hist.json")
    with open(dump_path, "w", encoding="utf-8") as fh:
        json.dump(_dump([
            {"chassis": "atlas", "variant": "Atlas Prime", "cap_jump_baseline": False,
             "locomotion": "MechWalker", "damage_schema": "Mech8Part"},
            {"chassis": "armoredcar", "variant": "", "cap_jump_baseline": False,
             "locomotion": "Wheeled", "damage_schema": "VehicleFacings"},
        ]), fh)
    rep, batch = RSC.run(SRCDATA, BUILDINGS, [dump_path])
    assert batch["units_converted"] == 4     # all 3 atlas variants + armoredcar
    assert rep.verdict == "PASS"
    assert rep.units_compared == 2


# --- determinism ---------------------------------------------------------

def test_batch_report_deterministic_under_hash_randomization():
    """Re-run the whole batch report in a fresh interpreter with two different
    PYTHONHASHSEED values; JSON must be byte-identical (mirrors the campaign_gen
    + LEGACY-UNIT-CONVERTER-1 determinism guard)."""
    script = (
        "import sys, os; "
        f"sys.path.insert(0, {json.dumps(os.path.dirname(_HERE))}); "
        "from unit_converter import roster as R; "
        "import json as J; "
        f"res = R.convert_roster({json.dumps(SRCDATA)}, {json.dumps(BUILDINGS)}); "
        "print(J.dumps(R.build_batch_report(res), sort_keys=True))"
    )
    env1 = dict(os.environ, PYTHONHASHSEED="0")
    env2 = dict(os.environ, PYTHONHASHSEED="424242")
    out1 = subprocess.check_output([sys.executable, "-c", script], env=env1)
    out2 = subprocess.check_output([sys.executable, "-c", script], env=env2)
    assert out1 == out2
    assert json.loads(out1)["units_converted"] == 4
