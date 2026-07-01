# tools/unit_converter/test_qualified_id.py
"""UNIT-ID-NAMESPACE-1 (Phase-3 slice 6): parse/validate/precedence for the
`namespace:name` convention (Python mirror of code/qualified_id.h), plus a
conformance check that every id the converter emits is a valid qualified id, plus
a multi-source override determinism test. Run: py -3 -m pytest tools/unit_converter
"""
import os
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))  # so `import unit_converter` works

from unit_converter import qualified_id as Q                # noqa: E402
from unit_converter.extract import (                        # noqa: E402
    extract_mech_variants, extract_vehicle,
)
from unit_converter.fitparse import CompBas                 # noqa: E402

_FIX = os.path.join(_HERE, "tests", "fixtures", "stock")


# --- parse / canonicalize ----------------------------------------------------

def test_parse_valid_canonicalizes():
    q = Q.parse_qualified_id("core:hunchback_hbk4g")
    assert q is not None
    assert q.ns == "core" and q.name == "hunchback_hbk4g"
    assert str(q) == "core:hunchback_hbk4g"
    assert q.is_core


def test_parse_lowercases():
    q = Q.parse_qualified_id("MyMod:UrbanMech_LAM_Test")
    assert str(q) == "mymod:urbanmech_lam_test"
    assert not q.is_core


def test_bare_name_defaults_to_core():
    q = Q.parse_qualified_id("atlas_prime")
    assert q is not None
    assert q.ns == "core" and q.name == "atlas_prime"
    assert q.is_core


@pytest.mark.parametrize("bad", [
    "", ":name", "ns:", "a:b:c", "1mod:name", "mod:9name",
    "my-mod:name", "mod:na me", "mod:na.me",
])
def test_invalid_rejected(bad):
    assert not Q.is_valid_qualified_id(bad)
    assert Q.parse_qualified_id(bad) is None
    assert Q.canonicalize_qualified_id(bad) is None


def test_length_boundaries():
    assert Q.is_valid_namespace("a" * 32)
    assert not Q.is_valid_namespace("a" * 33)
    assert Q.is_valid_name("a" * 64)
    assert not Q.is_valid_name("a" * 65)
    assert Q.is_valid_qualified_id("mod_2:hbk_4g_v2")


def test_canonicalize_round_trip():
    assert Q.canonicalize_qualified_id("CORE:Hunchback") == "core:hunchback"
    assert Q.canonicalize_qualified_id("hunchback") == "core:hunchback"
    canon = Q.canonicalize_qualified_id("MyMod:X_1")
    assert Q.canonicalize_qualified_id(canon) == canon


def test_core_reserved():
    assert not Q.is_mod_authorable("core:anything")
    assert not Q.is_mod_authorable("bare_name")   # bare -> core:, reserved
    assert Q.is_mod_authorable("mymod:anything")
    assert not Q.is_mod_authorable("bad!id")


# --- precedence --------------------------------------------------------------

def test_override_precedence():
    assert Q.overrides(1, Q.CORE_LOAD_ORDER)   # mod beats core
    assert Q.overrides(2, 1)                    # later beats earlier
    assert not Q.overrides(1, 2)                # earlier loses
    assert not Q.overrides(1, 1)               # equal: no override
    assert not Q.overrides(Q.CORE_LOAD_ORDER, 1)


def test_resolve_winning_order():
    assert Q.resolve_winning_order([0, 3, 1, 2]) == 3
    assert Q.resolve_winning_order([]) == Q.CORE_LOAD_ORDER
    assert Q.resolve_winning_order([5]) == 5


def test_merge_multi_source_override_deterministic():
    # core (0) defines atlas + hbk; modA (1) overrides hbk + adds urbie;
    # modB (2) overrides atlas. Winning order per id is deterministic.
    sources = [
        (Q.CORE_LOAD_ORDER, ["core:atlas", "core:hunchback"]),
        (1, ["core:hunchback", "moda:urbanmech"]),
        (2, ["core:atlas"]),
    ]
    winners = Q.merge_id_sources(sources)
    assert winners == {
        "core:atlas": 2,        # modB (last) wins
        "core:hunchback": 1,    # modA overrides core
        "moda:urbanmech": 1,
    }
    # Determinism: reversing source declaration order yields the same winners
    # (merge is by load-order value, not iteration order).
    winners_rev = Q.merge_id_sources(list(reversed(sources)))
    assert winners_rev == winners


def test_merge_rejects_invalid_id():
    with pytest.raises(ValueError):
        Q.merge_id_sources([(1, ["good:id", "bad id!"])])


# --- converter-emitted ids conform to the convention ------------------------

def _load_fixture_defs():
    compbas = CompBas.load(os.path.join(_FIX, "objects", "compbas.csv"))
    _chassis, mech_defs = extract_mech_variants(
        os.path.join(_FIX, "objects", "atlas.csv"),
        os.path.join(_FIX, "tgl", "atlas.ini"),
        compbas)
    veh = extract_vehicle(
        os.path.join(_FIX, "objects", "armoredcar.fit"),
        os.path.join(_FIX, "tgl", "armoredcar.ini"),
        compbas)
    return mech_defs + [veh]


def test_converter_emits_only_valid_core_ids():
    defs = _load_fixture_defs()
    assert defs, "expected fixture defs"
    for d in defs:
        # unit_id + chassis_id must both be valid qualified ids in the core ns.
        for field in (d.identity.unit_id, d.identity.chassis_id):
            assert Q.is_valid_qualified_id(field), field
            q = Q.parse_qualified_id(field)
            assert q.is_core, f"converter must emit core: ids, got {field}"
