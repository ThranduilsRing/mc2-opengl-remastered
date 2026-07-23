# tools/unit_converter/test_example_mods_roundtrip.py
"""MOD-ROUNDTRIP-SMOKE-1 — offline roundtrip proof for the examples/mods/ mods.

For each GOOD example mod (sample_equipment, sample_variant) this asserts
mc2_modlint exits 0 (clean) on its facts-JSON companion. For sample_bad_mod it
asserts modlint exits 1 AND that each intended reason-code token appears in the
modder-first, file-qualified output. It also checks every mod's authored
qualified id is a valid, mod-authorable namespace (the "bad namespace" guard the
Python qualified_id layer enforces).

This is the OFFLINE half of the roundtrip: it proves the mod content resolves and
validates through the SAME pure validator the engine uses, without launching the
game. (The engine source-flip is an overlay-not-add path — see
examples/mods/README.md — so a live in-mission spawn of a NEW namespaced def is a
documented follow-on, not exercised here.)

Run: py -3 -m pytest tools/unit_converter/test_example_mods_roundtrip.py
"""
import json
import os
import subprocess
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, os.pardir, os.pardir))  # worktree root
sys.path.insert(0, os.path.dirname(_HERE))

from unit_converter import qualified_id as Q  # noqa: E402

_MODS = os.path.join(_ROOT, "examples", "mods")


def _find_modlint():
    """Locate a built mc2_modlint.exe; skip the roundtrip if none is built."""
    for cfg in ("RelWithDebInfo", "Debug", "Release"):
        for base in ("build64-tests", "build64"):
            p = os.path.join(_ROOT, base, cfg, "mc2_modlint.exe")
            if os.path.isfile(p):
                return p
    p = os.path.join(_ROOT, "build64-tests", "mc2_modlint")  # non-Windows
    return p if os.path.isfile(p) else None


_MODLINT = _find_modlint()
_needs_modlint = pytest.mark.skipif(
    _MODLINT is None, reason="mc2_modlint.exe not built (build the mc2_modlint target)"
)


def _run_modlint(mode, facts_path):
    out = subprocess.run(
        [_MODLINT, mode, facts_path],
        capture_output=True, text=True,
    )
    return out.returncode, out.stdout + out.stderr


# --- authoring hygiene: mods exist and declare the shipped schema -----------

@pytest.mark.parametrize("mod_id", [
    "sample_equipment", "sample_variant", "sample_bad_mod", "core_unlocks",
])
def test_mod_json_uses_shipped_schema(mod_id):
    mj = os.path.join(_MODS, mod_id, "mod.json")
    assert os.path.isfile(mj), f"missing {mj}"
    with open(mj, encoding="utf-8") as f:
        m = json.load(f)
    assert m["schema"] == "mc2-mod/1"
    assert m["id"] == mod_id
    assert m["type"] in ("assets", "campaign", "dependency")


# --- qualified-id namespace guard (the "bad namespace" catch) ---------------

def test_authored_ids_are_mod_authorable():
    """Every id the good mods author must be a valid NON-core (mod-authorable) id;
    a bare/reserved/malformed namespace is rejected here."""
    for good in ("sample:hot_medium_laser", "sample:atlas_laserboat"):
        assert Q.is_valid_qualified_id(good)
        assert Q.is_mod_authorable(good), f"{good} must be mod-authorable"
    # Reserved core + malformed namespaces the guard must REJECT for a mod.
    assert not Q.is_mod_authorable("core:laser")          # reserved stock ns
    assert not Q.is_mod_authorable("Bad Namespace:x")     # illegal charset
    assert Q.parse_qualified_id("a:b:c") is None          # too many separators


# --- GOOD mods: modlint clean (exit 0) --------------------------------------

@_needs_modlint
def test_sample_equipment_clean():
    facts = os.path.join(_MODS, "sample_equipment", "modlint",
                         "hot_medium_laser.facts.json")
    rc, out = _run_modlint("--equipment", facts)
    assert rc == 0, out
    assert "RESULT: clean" in out
    # modder-first: the header names the file the facts came from.
    assert "hot_medium_laser.facts.json" in out


@_needs_modlint
def test_sample_variant_clean():
    facts = os.path.join(_MODS, "sample_variant", "modlint",
                         "atlas_laserboat.facts.json")
    rc, out = _run_modlint("--unit", facts)
    assert rc == 0, out
    assert "RESULT: clean" in out
    assert "atlas_laserboat.facts.json" in out


# --- BAD mod: modlint finds exactly the intended failures (exit 1) ----------

@_needs_modlint
def test_sample_bad_unit_findings():
    facts = os.path.join(_MODS, "sample_bad_mod", "modlint", "bad_unit.facts.json")
    rc, out = _run_modlint("--unit", facts)
    assert rc == 1, out
    for token in (
        "UPV_HARDPOINT_WEAPON_CLASS_REJECTED",  # Energy weapon into Ballistic-only hardpoint
        "UPV_HARDPOINT_SOCKET_MISSING",         # missing hardpoint socket
        "UPV_UI_ASSET_MISSING",                 # missing icon
    ):
        assert token in out, f"expected {token} in:\n{out}"
    # modder-first surface: id + file both present.
    assert "sample:broken_striker" in out
    assert "bad_unit.facts.json" in out


@_needs_modlint
def test_sample_bad_equipment_findings():
    facts = os.path.join(_MODS, "sample_bad_mod", "modlint", "bad_equipment.facts.json")
    rc, out = _run_modlint("--equipment", facts)
    assert rc == 1, out
    for token in (
        "EQV_WEAPON_NO_FIRE_EXECUTOR",   # unbound/empty fire executor
        "EQV_WEAPON_NO_CATEGORY",        # missing category
        "EQV_AMMO_CATEGORY_MISSING",     # ammo-using but no ammo category
        "EQV_WEAPON_RANGE_INVALID",      # short>med>long
    ):
        assert token in out, f"expected {token} in:\n{out}"
    assert "sample:broken_laser" in out
    assert "bad_equipment.facts.json" in out
