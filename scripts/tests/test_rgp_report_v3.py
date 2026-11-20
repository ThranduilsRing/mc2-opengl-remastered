"""Unit tests for tools/rgp/rgp_report_v3.py (RGP per-pass attribution). Golden tests run on decoded archived captures when
MC2_RGP_V3_WORKDIRS points at a dir holding the decoded workdirs (rgp_report_v2.py extract+decode); they skip otherwise."""
import csv
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "rgp"))
import rgp_report_v3 as v3  # noqa: E402

V2_SUMMARY = Path("E:/mc2-archive-2026-10/proofs/rgp/rgp-sweep-1119-6ee6e97f4/sweep_summary_v2.csv")
WORKDIRS = os.environ.get("MC2_RGP_V3_WORKDIRS")


def test_clock_is_piecewise():
    clk = v3.Clock([(0, 0), (1000, 100000), (2000, 300000)])          # second half runs at half speed
    assert clk.ms(500) == pytest.approx(0.5) and clk.ms(1500) == pytest.approx(2.0)
    assert clk.ms(1000) == pytest.approx(1.0) and clk.ms(2000) == pytest.approx(3.0)
    assert v3.union_ms([(0, 1000), (900, 1100), (1500, 1600)], clk) == pytest.approx(1.0 + 0.2 + 0.2)   # overlap merged, then each in its own segment
    assert clk.global_k() == pytest.approx(1.5e-3)                      # v2's single scale differs from the local ones


def test_classify_gap_carry_and_anchor():
    passes = [dict(name="A", path="A", depth=0, t0=100, t1=200, idx=0), dict(name="B", path="B", depth=0, t0=900, t1=950, idx=1)]
    anchors = {}
    assert v3.classify(passes, 150, 7, anchors, major=False)[1] == "inferred" and 7 not in anchors   # a stray wave seeds no anchor
    assert v3.classify(passes, 160, 7, anchors, major=True)[1] == "inferred" and anchors[7]["name"] == "A"
    assert v3.classify(passes, 500, 7, anchors, major=True) == (passes[0], "tail")                     # late launch of an anchored shader
    assert v3.classify(passes, 500, 8, anchors, major=True) == ("(unlabelled: A..B)", "gap")           # unknown shader between markers
    assert v3.gap_label(passes, 50) == "(unlabelled: start..A)"


def test_occupancy_mean_and_hitch():
    rows = [dict(time=t, start=s, cu="0", simd="0") for t, s in ((0, "1"), (0, "1"), (10, "0"), (20, "0"))]
    occ = v3.Occupancy(rows)
    assert occ.mean([[0, 10]]) == pytest.approx(2.0) and occ.mean([[10, 20]]) == pytest.approx(1.0) and occ.cap == 16
    assert v3.hitch_check([(8.5, 7.0), (8.6, 7.1), (8.4, 7.0), (12.0, 11.3)], 11.27) == pytest.approx((8.6, 12.0, 12.0 / 8.6))


@pytest.mark.skipif(not WORKDIRS, reason="decoded archived captures not available (MC2_RGP_V3_WORKDIRS)")
@pytest.mark.parametrize("pose", ["mc2_10_corner_far_2160", "mc2_01_corner_far_2160", "mc2_05_corner_far_2160", "mc2_02_corner_far_2160"])
def test_golden_archived_captures(pose):
    a = v3.analyse(f"{WORKDIRS}/{pose}")
    v2 = {r[0]: float(r[1]) for r in csv.reader(V2_SUMMARY.open())}[pose]
    assert a["busy_v2_scale_ms"] == pytest.approx(v2, rel=0.01)           # same waves, same scale as v2
    assert max(r["incl_ms"] for r in a["rows"] if r["depth"] == 0) < 0.6 * a["busy_ms"]   # no frame-sized lump
    if pose.startswith("mc2_10"):
        gap = [r for r in a["rows"] if r["name"] == "(unlabelled: Post.HzbCull..Props.Cull)"]
        assert gap and 3.0 < gap[0]["excl_ms"] < 3.5                      # the 3.4 ms mech-colour burst, now its own row
        assert any(f.startswith("HITCH_FRAME") for f in a["flags"]) and any(f.startswith("BURST") for f in a["flags"])
        assert a["typical_busy_ms"] < 6.0
    terrain = next(r for r in a["rows"] if r["name"] == "Terrain")
    assert terrain["incl_ms"] > 1.0                                       # terrain shaders land in Terrain, not LightLists
    assert not any(r["name"] == "LightLists" and r["excl_ms"] > 0.1 for r in a["rows"])
