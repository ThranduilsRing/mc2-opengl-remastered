"""Report semantics: tie ranks, compbas IDs, shared ammunition and lab totals."""
import pytest

from tools.bv_calibration_report import ranks, spearman, variants, wire


def test_tie_aware_rank_correlation():
    assert ranks([9, 1, 1, 5]) == [4, 1.5, 1.5, 3]
    assert spearman([(1, 3), (2, 2), (3, 1)]) == pytest.approx(-1)
    assert spearman([(1, 5), (1, 5), (3, 9)]) == pytest.approx(1)
    assert spearman([(1, 2), (1, 3)]) is None
    assert spearman([]) is None


def test_variant_uses_compbas_id_and_splits_ammo_per_weapon():
    rows = [[""] * 12 for _ in range(23)]
    rows[0][0] = "BaseChassisInfo"
    rows[2][1] = "Fixture"
    for index, value in {3: 20, 5: 80, 6: 19, 9: 50, 10: 1000}.items():
        rows[index][4] = str(value)
    rows[16] = ["BaseInternalStructure"] + [10] * 8
    rows[22] = ["VariantName", "Fixture Prime"]
    for ordinal in (100, 100, 200, 7, 10, 25):
        rows.append(["Item0MasterId", "9999", "0", "0", str(ordinal)])
    comps = []
    for ordinal, kind, shots, heat, damage, br, ammo in (
        (100, "BallisticWeapon", 0, 2, 4, 100, 200),
        (200, "Ammo", 40, 0, 0, 0, 0),
        (7, "HeatSink", 0, 0, 4, 0, 0),
        (10, "JumpJet", 0, 7, 0, 0, 0),
        (25, "Bulk", 0, 0, 32, 0, 0),
    ):
        c = ["0"] * 23
        for index, value in {0: ordinal, 1: kind, 4: shots, 5: heat, 7: damage, 8: br, 22: ammo}.items():
            c[index] = str(value)
        comps.append(c)
    v, = variants({"compbas": comps, "fixture": rows})
    assert v["armor"] == 112
    assert v["structure"] == 80
    assert v["heat"] == 17
    assert v["jump"] == 1
    assert v["weapons"] == [(100, 2, 20), (100, 2, 20)]
    assert v["legacy_cv"] == 250
    assert wire(v).endswith("100.0 2.0 ammo 20.0 100.0 2.0 ammo 20.0")
