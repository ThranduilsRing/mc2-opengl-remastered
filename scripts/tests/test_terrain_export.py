"""TERRAIN-REGEN-1 tooling: export_mission / classify_terrain / prototype_cliff / render_matched (no game, no Blender)."""
import json
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

TOOLS = Path(__file__).resolve().parents[2] / "tools" / "terrain_export"
sys.path.insert(0, str(TOOLS))
import classify_terrain as C  # noqa: E402
import export_mission as E  # noqa: E402
import prototype_cliff as P  # noqa: E402
import render_matched as R  # noqa: E402


def test_render_diagonal_is_tl_br():
    elev = np.zeros((3, 3))
    _, _, _, idx = E.terrain_arrays(elev, 0.1)
    tri = idx.reshape(-1, 3)[:2]  # quad (0,0): tl=0 tr=1 bl=3 br=4
    assert [tuple(t) for t in tri] == [(0, 3, 4), (0, 4, 1)]  # {tl,bl,br},{tl,br,tr}


def test_prototype_tri_height_follows_tl_br_diagonal():
    # tl=0 tr=10 bl=20 br=30; plane tl,tr,br: 10u+20v ; plane tl,bl,br: 20v+10u -> check off-diagonal sample
    elev = np.array([[0.0, 10.0], [20.0, 40.0]])  # br = 40 breaks planarity, so the diagonal matters
    ur = float(P.tri_height(elev, np.array([0.25]), np.array([0.75]))[0])  # u>v: tl,tr,br
    ll = float(P.tri_height(elev, np.array([0.75]), np.array([0.25]))[0])  # u<=v: tl,bl,br
    assert ur == pytest.approx(0 + 0.75 * 10 + 0.25 * 30)
    assert ll == pytest.approx(0 + 0.75 * 20 + 0.25 * 20)


def test_install_dir_requires_explicit_install(monkeypatch):
    monkeypatch.delenv("MC2_INSTALL", raising=False)
    with pytest.raises(SystemExit):
        E.install_dir(None)
    assert E.install_dir("X:/inst") == Path("X:/inst")
    monkeypatch.setenv("MC2_INSTALL", "Y:/env")
    assert E.install_dir(None) == Path("Y:/env")


def test_find_colormap_and_visual_height_use_install(tmp_path):
    tex = tmp_path / "data" / "textures"
    tex.mkdir(parents=True)
    assert E.find_colormap("m", tmp_path) is None
    Image.new("RGB", (4, 4)).save(tex / "m.burnin.jpg")
    assert E.find_colormap("m", tmp_path) == tex / "m.burnin.jpg"
    elev = np.full((5, 5), 100.0)
    h = E.visual_height("m", elev, 8, tmp_path)  # no sidecar -> upsampled sim heights
    assert h.shape == (8, 8) and np.allclose(h, 100.0)
    bd = tmp_path / "data" / "missions" / "m.beauty"
    bd.mkdir(parents=True)
    v = (5 - 1) * 4 + 1
    np.full(v * v, 7.0, "<f4").tofile(bd / "visual_height_4x.r32")
    assert np.allclose(E.visual_height("m", elev, 8, tmp_path), 7.0)


def test_as_rendered_darkens_cliffs_and_keeps_flat():
    side = 8
    elev = np.full((side, side), 400.0)
    elev[:, side // 2:] += 900.0  # sheer step
    cm = Image.new("RGB", (64, 64), (180, 150, 100))
    out = np.asarray(E.as_rendered_colormap(cm, elev, E.material_tints(None))).astype(float)
    flat, cliff = out[32, 4].mean(), out[32, 32].mean()
    assert cliff < flat * 0.9


def _mission(objects):
    side = 32
    elev = np.full((side, side), 200.0)
    elev[:, side // 2:] = 700.0  # water | sheer step at world x = 0 | plateau
    return {"side": side, "elev": elev, "texdata": np.zeros((side, side), np.uint32),
            "water_elev": 350.0, "objects": objects}


def test_classify_footprint_does_not_overwrite_cliff():
    p = {"cliff": 38.0, "steep": 24.0, "edge_wu": 256.0, "beach_h": 30.0, "green": 0.02,
         "jungle_lum": 0.28, "foot_wu": 96.0}
    cm = Image.new("RGB", (64, 64), (120, 120, 120))
    cls_a, _, _ = C.classify(_mission([]), cm, 128, p)
    cls_b, _, _ = C.classify(_mission([(1, 0.0, 0.0, 700.0, 0.0), (1, 1000.0, 0.0, 700.0, 0.0)]), cm, 128, p)
    assert (cls_a == 2).any()
    assert ((cls_a == 2) == (cls_b == 2)).all()  # a prop box on the cliff leaves cliff cells alone
    assert (cls_b == 5).any()  # but still stamps footprint on ordinary ground


def test_classify_cli_reports_percent_of_land(tmp_path, monkeypatch):
    # stats basis: pct_land excludes water; water reports share of whole map
    p = {"cliff": 38.0, "steep": 24.0, "edge_wu": 256.0, "beach_h": 30.0, "green": 0.02,
         "jungle_lum": 0.28, "foot_wu": 96.0}
    cls, _, _ = C.classify(_mission([]), Image.new("RGB", (64, 64), (120, 120, 120)), 128, p)
    land = int((cls != 0).sum())
    assert 0 < land < cls.size
    assert sum(100.0 * int((cls == i).sum()) / land for i in range(1, 10)) == pytest.approx(100.0)


def test_letterbox_band_and_cam_args():
    im = Image.new("RGB", (100, 100), (0, 0, 0))
    im.paste(Image.new("RGB", (100, 40), (90, 90, 90)), (0, 30))
    assert R.letterbox_band(im) == (30, 70)
    side = {"cap_pos": [1, 2, 3], "cap_rot": -4, "cap_proj": 5, "cap_alt": 6}
    assert R.cam_args(side) == ["1", "2", "3", "-4", "5", "6"]


def test_no_hardcoded_install_paths_left():
    for f in TOOLS.glob("*.py"):
        assert "mc2-win64-v0.4" not in f.read_text(encoding="utf-8"), f.name
