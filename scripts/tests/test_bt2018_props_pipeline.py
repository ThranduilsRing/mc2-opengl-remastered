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
    assert pc.stock_category("extractionmarker") == pc.EXCLUDED
    for name in ("dropshipliao", "crashnose", "straightpipe2", "l_palacecore", "hpg_office"):
        assert pc.stock_category(name) in pc.UNMATCHED_STOCK, name
    assert pc.stock_category("pipetank") == "industrial"
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
    for n in ("smallBuildingA_cell_007", "cityBuildingF_fract", "smallCivilianBldgF_glass", "tankDetail74975", "pipes23",
              "abstractBldg", "industrialLowRampA", "civilian_roof_D04", "spawnTankDoor"):
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


def test_mapping_drives_packages_and_checker():
    import check_prop_packages as cpp
    import match_stock_props as mp
    lib = {"assets": [{"id": "bt_house", "category": "building", "file": "f", "lod_meshes": ["h_LOD0", "h_LOD1"],
                       "materials": ["m"], "placement_scale": [1, 1, 1]}]}
    bind = {"materials": {"m": {"maps": {"_MainTex": "alb", "_BumpMap": "nrm"}}}}
    mapping = {"a": {"key": "staticprop:a", "pick": "bt_house"}, "b": {"key": "staticprop:b", "pick": "bt_house"},
               "c": {"key": "staticprop:c", "pick": "bt_house", "review": "none"}}
    props = mp.mapping_props(mapping, lib, bind)
    assert [p["replaces"] for p in props] == [["staticprop:a"], ["staticprop:b"]]
    assert props[0]["id"] != props[1]["id"]
    assert props[0]["materials"][0]["alb"] == "alb" and props[0]["lods"][1]["distance_wu"] == 600
    mp.check_ids(props)
    man = [{"id": p["id"], "source_id": p["source_id"], "replaces": list(p["replaces"])} for p in props]
    assert cpp.check_mapping(mapping, man) == []
    man[0]["replaces"].append("staticprop:c")
    assert len(cpp.check_mapping(mapping, man)) == 1


def test_stock_fit_pins_both_footprint_axes_and_clamps_height():
    import extract_bt2018_props as ex
    import pytest
    for height in (2, 20, 200):
        turn, scale, ratio = ex.fit_stock([80, height, 40], [40, 20, 80])
        assert turn
        assert scale[0] == scale[2]
        assert np.all((ratio[[0, 2]] >= 0.9) & (ratio[[0, 2]] <= 1.0))
        assert 0.6 <= ratio[1] <= 1.6
    with pytest.raises(ValueError, match="footprint mismatch"):
        ex.fit_stock([80, 20, 80], [40, 20, 80])


def test_albedo_guard_checks_stock_luminance_saturation_and_normals():
    import extract_bt2018_props as ex
    import pytest
    stock = {"luminance": 0.4, "saturation": 0.2}
    image = np.full((4, 4, 4), [90, 100, 110, 255], np.uint8)
    assert ex.check_albedo(image, stock)["luminance"] > 0.3
    for bad, message in (([128, 128, 255, 255], "normal map"),
                         ([250, 250, 250, 255], "luminance"),
                         ([0, 0, 0, 255], "luminance"), ([255, 0, 0, 255], "saturation")):
        with pytest.raises(ValueError, match=message):
            ex.check_albedo(np.full((4, 4, 4), bad, np.uint8), stock)
    image[0, 0] = [0, 0, 0, 0]  # unused atlas background must not bias the comparison
    assert ex.check_albedo(image, stock)["luminance"] > 0.3


def test_prop_recipe_keeps_full_detail_atlas_and_scale_audit(tmp_path, monkeypatch):
    import extract_bt2018_props as ex
    import json
    from tools.content_pipeline import cache, cook
    from tools.content_pipeline.tests.test_content_pipeline import make_gltf
    monkeypatch.setattr(cache, "DEFAULT_DIR", tmp_path / "cache")
    fit = {"scale": [0.75, 1.0, 0.75], "ratio": [1.0, 0.6, 0.95]}
    manifest = {"id": "bt_a", "replaces": ["staticprop:a"], "category": "building", "stock_fit": fit,
                "lods": [{"glb": "lod0.glb"}]}
    spec = ex.build_recipe([manifest], Path("packages"), {})["assets"][0]
    assert spec["stock_fit"] == fit
    assert spec["texel_scale"] == 8
    assert spec["lods"] == []
    src, *_ = make_gltf(tmp_path)
    spec["intact"] = str(src)
    recipe = {"mod_id": "test-fit", "assets": [spec], "out": str(tmp_path / "mod")}
    out = cook.cook(recipe, compress=False)
    registry = json.loads((out / "data/model_overrides/models.json").read_text())["overrides"][0]
    assert registry["stockFit"] == fit
    assert registry["scale"] == 1.0  # import scale is baked; runtime must not apply it twice
    assert all(0.9 <= registry["stockFit"]["ratio"][axis] <= 1.0 for axis in (0, 2))


def test_check_ids_rejects_long_or_duplicate_ids():
    import match_stock_props as mp
    import pytest
    with pytest.raises(SystemExit):
        mp.check_ids([{"id": "bt_" + "x" * 14}])
    with pytest.raises(SystemExit):
        mp.check_ids([{"id": "bt_a"}, {"id": "bt_a"}])


class _FakeInstall:
    """Top-down RGBA images by name, as UnityPy returns them."""

    def __init__(self, images):
        self.images = images

    def texture(self, name):
        from PIL import Image
        return Image.fromarray(self.images[name])


def _wall_bake(colors):
    """A 1 x 1 wall quad whose Unity v rises with height; clean texture red in its top rows (image row 0 = Unity v 1)."""
    import bake_materials as bm
    clean = np.zeros((64, 64, 4), np.uint8)
    clean[..., 3] = 255
    clean[:8, :, 0] = 255           # top rows red
    clean[8:, :, 2] = 255           # rest blue
    rough = np.full((64, 64, 4), (0, 255, 0, 255), np.uint8)
    tc = bm.TexCache(_FakeInstall({"clean": clean, "rough": rough}))
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)     # Unity uv: v up the wall
    tris = [np.array([[0, 1, 2], [0, 2, 3]])]
    mat = {"maps": {"_MainTex": ("clean", (1.0, 1.0), (0.0, 0.0)), "_BlendMainTex": ("rough", (1.0, 1.0), (0.0, 0.0))},
           "color": (1.0, 1.0, 1.0), "floats": {}}
    return bm.bake(uv, colors, tris, [mat], tc, size=128)


def test_bake_keeps_unity_uv_orientation_top_of_wall_is_top_of_texture():
    alb, nrm, orm, uvs = _wall_bake(np.ones(4, np.float32))
    top = uvs[0][3]                  # vertex (0, 1): top-left of the wall, glTF uv
    bottom = uvs[0][0]
    assert top[1] < bottom[1]        # glTF v runs down: the top of the wall sits higher in the image
    row = lambda v: int(v[1] * 128)  # noqa: E731
    col = int((top[0] + uvs[0][1][0]) / 2 * 128)
    assert alb[row(top) + 2, col, 0] > 200 and alb[row(top) + 2, col, 2] < 50       # red band at the top
    assert alb[row(bottom) - 3, col, 2] > 200 and alb[row(bottom) - 3, col, 0] < 50  # blue below
    assert (nrm[row(top) + 2, col] == [128, 128, 255]).all()                         # flat normal kept


def test_bake_vertex_colour_zero_selects_weathered_layer():
    alb, *_ , uvs = _wall_bake(np.zeros(4, np.float32))
    centre = ((uvs[0][0] + uvs[0][2]) / 2 * 128).astype(int)
    assert alb[centre[1], centre[0], 1] > 200 and alb[centre[1], centre[0], 0] < 50


def test_pack_regions_do_not_overlap():
    import bake_materials as bm
    exts = [(0.8, 0.4), (0.2, 0.9), (0.5, 0.5), (0.05, 0.05)]
    s, origins = bm.pack_regions(exts, 1024)
    boxes = [(x, y, x + w * s, y + h * s) for (x, y), (w, h) in zip(origins, exts)]
    assert all(0 <= b[0] and b[2] <= 1024 and 0 <= b[1] and b[3] <= 1024 for b in boxes)
    for i, a in enumerate(boxes):
        for b in boxes[i + 1:]:
            assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1]


def _bt_install():
    import os
    import pytest
    root = Path(os.environ.get("BT2018_INSTALL", "A:/Games/BATTLETECH/BattleTech_Data"))
    if not (root / "sharedassets21.assets").is_file():
        pytest.skip("BATTLETECH install not present (owner machine only)")
    pytest.importorskip("UnityPy")
    return root


def test_real_prop_window_rows_run_horizontally_on_walls():
    """civilianLargeBldgA (window rows, panel lines): on wall triangles the texture's v axis must follow world up
    after the extractor's frame conversion, i.e. the mapping is not sideways."""
    root = _bt_install()
    import extract_bt2018_props as ex
    inst = ex.Install(root)
    pos, nrm, uv, _t, subs, _c = ex.mesh_prims(ex.find_mesh(inst, "sharedassets21.assets", "civilianLargeBldgA_LOD0"))
    up_v = total = 0
    for t in subs:
        for a, b, c in t.reshape(-1, 3):
            n = np.cross(pos[b] - pos[a], pos[c] - pos[a])
            if np.linalg.norm(n) < 1e-6 or abs(n[1]) / np.linalg.norm(n) > 0.3:
                continue                                    # walls only
            duv = np.stack([uv[b] - uv[a], uv[c] - uv[a]], 1)
            if abs(np.linalg.det(duv)) < 1e-9:
                continue
            j = np.stack([pos[b] - pos[a], pos[c] - pos[a]], 1) @ np.linalg.inv(duv)   # d(pos)/d(u, v)
            total += 1
            up_v += abs(j[1, 1]) / (np.linalg.norm(j[:, 1]) + 1e-9) > abs(j[1, 0]) / (np.linalg.norm(j[:, 0]) + 1e-9)
    assert total > 50
    assert up_v / total > 0.8, f"v follows world up on only {up_v}/{total} wall triangles"
