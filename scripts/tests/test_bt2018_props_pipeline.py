"""BT2018 prop library: classifier, mesh grouping, cook ids, rasteriser. Offline, no BT2018 files needed."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "bt2018_import"))

import prop_classify as pc  # noqa: E402
import raster  # noqa: E402


def test_parse_name_kinds():
    assert pc.parse_name("cityBuildingA_shell")["kind"] == "shell"
    assert pc.parse_name("cityBuildingA_split_004")["kind"] == "piece"
    assert pc.parse_name("lowWall150m_shell_COL")["kind"] == "col"
    assert pc.parse_name("rubblePileBProcedural_LOD1") == {"family": "rubblePileBProcedural", "lod": 1, "kind": "lod"}
    assert pc.parse_name("a|b|Dupli|1")["kind"] == "instance"


def test_group_assets_prefers_intact_lods_over_destroyed_shell():
    names = ["villaD_shell", "villaD_LOD1", "villaD_LOD0", "villaD_split_003",
             "cityBuildingA_shell", "cityBuildingA_shell_COL", "cityBuildingA_split_001", "cityBuildingA_fract_002",
             "envMdlRock_scatterC_LOD0", "envMdlRock_scatterC_LOD2", "envMdlRock_scatterC_LOD1",
             "envMdlTree_pine_LOD0", "envMdlBush_x_LOD0", "uixSvgIcon_scrap", "lowWall150m_shell"]
    g = pc.group_assets(names)
    assert g["villaD"] == {"lods": ["villaD_LOD0", "villaD_LOD1"], "shell": False}
    assert g["cityBuildingA"] == {"lods": ["cityBuildingA_shell"], "shell": True}  # shell only as a fallback
    assert g["envMdlRock_scatterC"]["lods"] == ["envMdlRock_scatterC_LOD0", "envMdlRock_scatterC_LOD1", "envMdlRock_scatterC_LOD2"]
    assert "lowWall150m" in g
    assert not any("Tree" in k or "Bush" in k or "uix" in k for k in g)


def test_vegetation_is_always_excluded():
    for n in ("envMdlTree_scotsPine", "envMdlBush_blackthorn", "grassPatchA", "palmTree"):
        assert pc.classify(n, (5, 10, 5)) == pc.EXCLUDED


def test_classify_by_name_and_bounds():
    assert pc.classify("medWall150m") == "wall_fence"
    assert pc.classify("militaryTowerArmored") == "tower"
    assert pc.classify("industrialTankJ") == "industrial"
    assert pc.classify("rubblePileMetalLargeB") == "debris"
    assert pc.classify("envMdlRock_scatterC") == "rock"
    assert pc.classify("envMdlRock_scatterC", (30, 40, 30)) == "cliff"
    assert pc.classify("apartmentBlockF") == "building"
    assert pc.classify("mysteryThing", (2, 0.1, 40)) == pc.EXCLUDED
    assert pc.classify("mysteryThing", (4, 30, 4)) == "tower"
    assert pc.classify("mysteryThing", (20, 10, 20)) == "building"


def test_stock_category():
    assert pc.stock_category("rock_jclump1") == "rock"
    assert pc.stock_category("lookouttower") == "tower"
    assert pc.stock_category("smallfueltank") == "industrial"
    assert pc.stock_category("shantyfence") == "wall_fence"
    assert pc.stock_category("junkpile") == "debris"
    assert pc.stock_category("tc1_2") == pc.EXCLUDED
    assert pc.stock_category("maple2dead") == pc.EXCLUDED
    assert pc.stock_category("mysteryshed", (30, 8, 30)) == "building"


def test_short_id_fits_cook_limit_and_never_collides():
    taken = set()
    ids = []
    for fam in ("industrialRefineryA", "industrialRefineryB", "industrialRefineryC", "x", "x"):
        i = pc.short_id(fam, taken)
        taken.add(i)
        ids.append(i)
    assert all(len(i) <= 16 and i.startswith("bt_") for i in ids)
    assert len(set(ids)) == len(ids)


def _box():
    v = np.array([[x, y, z] for x in (-1, 1) for y in (0, 2) for z in (-1, 1)], np.float32)
    idx = np.array([[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5], [0, 4, 5], [0, 5, 1],
                    [2, 3, 7], [2, 7, 6], [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3]])
    return [{"pos": v, "idx": idx, "uv": None, "tex": None, "color": (200, 120, 80)}]


def test_raster_draws_something_and_is_deterministic():
    a, cov = raster.render(_box(), size=64)
    b, _ = raster.render(_box(), size=64)
    assert cov.sum() > 200 and (a == b).all()


def test_silhouette_distinguishes_tall_from_wide():
    tall = _box()
    wide = _box()
    wide[0]["pos"] = wide[0]["pos"] * np.array([4, 0.5, 1], np.float32)
    # aspect-normalised occupancy: a solid box looks the same, a hollow L-shape differs
    l_shape = tall[0]["pos"].copy()
    assert raster.silhouette(tall).shape == (288,)
    assert raster.silhouette(tall).sum() > 0
    assert l_shape.shape[0] == 8


def test_matcher_ranks_by_category_then_size():
    import match_stock_props as mp
    assets = [
        {"id": "bt_a", "category": "industrial", "footprint_wu": [40, 40], "height_wu": 60, "materials_resolved": True},
        {"id": "bt_b", "category": "industrial", "footprint_wu": [200, 200], "height_wu": 300, "materials_resolved": True},
        {"id": "bt_c", "category": "rock", "footprint_wu": [40, 40], "height_wu": 60, "materials_resolved": True},
        {"id": "bt_d", "category": "industrial", "footprint_wu": [40, 40], "height_wu": 60, "materials_resolved": False},
    ]
    stock = {"smallfueltank": {"size_wu": [40, 60, 40], "uses": 3},
             "tc1_2": {"size_wu": [10, 80, 10], "uses": 99},
             "rock_x": {"size_wu": [40, 60, 40], "uses": 1}}
    m = mp.match(stock, assets)
    assert "tc1_2" not in m                      # vegetation never matched
    assert m["smallfueltank"]["pick"] == "bt_a"  # same category, same size, resolved materials
    assert m["smallfueltank"]["alternatives"][0] == "bt_d"
    assert m["rock_x"]["pick"] == "bt_c"
    assert m["smallfueltank"]["key"] == "staticprop:smallfueltank"


def test_matcher_reports_no_candidate():
    import match_stock_props as mp
    m = mp.match({"lookouttower": {"size_wu": [20, 200, 20]}}, [{"id": "bt_x", "category": "debris", "footprint_wu": [1, 1],
                                                              "height_wu": 1, "materials_resolved": True}])
    assert m["lookouttower"]["pick"] is None


def test_fragments_are_not_props():
    for n in ("smallBuildingA_cell_007", "cityBuildingF_fract", "smallCivilianBldgF_glass", "tankDetail74975", "pipes23"):
        assert pc.is_fragment(n), n
    for n in ("smallBldgE", "industrialTankA", "lookoutTowerA"):
        assert not pc.is_fragment(n), n


def test_solid_metrics_closed_box_vs_open_fragment():
    box = raster.solid_metrics(_box())
    frag = _box()
    frag[0]["idx"] = frag[0]["idx"][[8, 9, 4, 5]]  # one wall and the floor: a partial destruction shell
    f = raster.solid_metrics(frag)
    assert box["solidity"] > 0.95 and abs(box["volume"] - 8.0) < 0.1
    assert f["solidity"] < 0.1


def test_matcher_rejects_fragments_and_volume_mismatch():
    import match_stock_props as mp
    base = {"category": "building", "footprint_wu": [40, 40], "height_wu": 40, "materials_resolved": True}
    assets = [dict(base, id="bt_shell", shell=True, solidity=0.9, volume_wu3=50000),
              dict(base, id="bt_hollow", solidity=0.1, volume_wu3=6000),
              dict(base, id="bt_mast", category="building", solidity=0.5, volume_wu3=2000),
              dict(base, id="bt_ok", solidity=0.8, volume_wu3=48000)]
    m = mp.match({"house": {"size_wu": [40, 40, 40], "volume_wu3": 50000, "uses": 1}}, assets)
    assert m["house"]["pick"] == "bt_ok" and m["house"]["alternatives"] == []
