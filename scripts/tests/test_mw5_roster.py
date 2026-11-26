"""The recipe-driven MW5 mech pipeline (tools/mw5_mechs/make_mech.py, recipes/*.json, roster.json)."""
import json
import sys
from pathlib import Path

import numpy as np
import pytest

TOOLS = Path(__file__).resolve().parents[2] / "tools" / "mw5_mechs"
sys.path.insert(0, str(TOOLS))
import make_mech  # noqa: E402

RECIPES = sorted(p for p in (TOOLS / "recipes").glob("*.json") if p.name != "vehicles.json")


@pytest.mark.parametrize("path", RECIPES, ids=lambda p: p.stem)
def test_recipe_is_self_consistent(path):
    r = json.loads(path.read_text())
    ex = r["extraction"]
    assert set(r["clips"]) <= set(ex["clips"]), "every mapped clip is extracted"
    if "loadout" not in r:
        pytest.skip("legacy Timberwolf recipe predates make_mech")
    nodes = {row["node"] for row in r["loadout"]}
    for row in r["loadout"]:
        if "copy_of" in row:
            assert row["copy_of"] in nodes
        else:
            assert row["mesh"] in ex["weapons"]
    assert {"mask", "normal"} <= set(ex["textures"])
    for hold in r["hold_clips"].values():
        assert hold[0] in ex["clips"]


def test_roster_maps_every_recipe_as_done():
    roster = json.loads((TOOLS / "roster.json").read_text())
    assert roster["schema"] == "mw5-roster/1"
    done = {c["mc2"] for c in roster["chassis"] if c["status"] == "done"}
    hosts = {json.loads(p.read_text())["mc2"]["model"]["host_ini"] for p in RECIPES}
    assert hosts <= done
    for c in roster["chassis"]:
        assert (c["mw5"] is None) == (c["status"] == "no-mw5-asset")


def test_material_params_later_entries_win():
    defaults = {"S": {k: 0.25 for k in make_mech.SCALAR_KEYS}, "V": {k: [0.1, 0.2, 0.3, 1.0] for k in tuple(make_mech.PAINT_KEYS) + ("DirtColor", "GrimeColor")}}
    for m in make_mech.METALS:
        defaults["S"][m + " Roughness"] = 0.5
        defaults["S"][m + " Metallic"] = 1.0
    key = make_mech.SCALAR_KEYS[0]
    chain = [{"scalars": [{"ParameterInfo": {"Name": key}, "ParameterValue": 0.5}]},
             {"scalars": [{"ParameterInfo": {"Name": key}, "ParameterValue": 0.75}]}]
    assert make_mech.material_params(chain, defaults)["scalars"][key] == 0.75


def test_urbanmech_includes_both_separate_arm_meshes():
    recipe = json.loads((TOOLS / "recipes/urbanmech.json").read_text())
    attachments = make_mech.weapon_attachments(recipe, Path("work"))
    for side in ("Left", "Right"):
        assert any(f"Urbanmech_{side}_Arm_SKM.gltf=Forearm_{side}" in a for a in attachments)


@pytest.mark.parametrize("family", ["shadowcat", "maddog"])
def test_clan_cockpit_glass_is_built(family):
    recipe = json.loads((TOOLS / f"recipes/{family}.json").read_text())
    assert "glass" not in recipe["extraction"]["cockpit"]  # first-person canopy has a different origin
    assert recipe["extraction"]["cockpit"]["hull"].endswith("_TP_Cockpit_DM0_STM")
    assert not recipe["cockpit"]["drop_glass"]


def test_free_tile_stays_inside_atlas_and_avoids_coverage(tmp_path, monkeypatch):
    uv = np.array([[0.0, 0.0], [1.0, 0.0], [0.0, 1.0], [1.0, 1.0]])
    tris = np.array([[0, 1, 2]])  # lower-left half covered
    monkeypatch.setattr(make_mech, "load_glb", lambda p: ({"meshes": []}, b""))
    x, y, size = make_mech.free_tile("unused.glb", extra_uvs=[(uv, tris)])
    assert 0 <= x and 0 <= y and x + size <= 2048 and y + size <= 2048
    assert (x + size / 2) / 2048 + (y + size / 2) / 2048 > 1.0, "tile centre lies in the uncovered half"


def test_additive_hold_uses_existing_base_pose(capsys):
    r = json.loads((TOOLS / "recipes/atlas.json").read_text())
    available = {r["clip_pattern"].format(source=s) for s in r["extraction"]["clips"] if s != "Crouch"}
    make_mech.select_base_clips(r, available)
    assert r["hold_clips"]["atlas_moveJumpUpStart"] == ["Stand", 0]
    assert r["hold_clips"]["atlas_moveJumpDownLand"] == ["Stand", 0]
    assert r["hold_clips"]["atlas_deathIdle"] == ["Falling", -1]
    assert "Crouch" not in str(make_mech.native_request(r)["clips"])
    assert "source=Crouch -> Stand" in capsys.readouterr().out
    assert r["hold_clips"]["atlas_moveJumpUpIdle"] == ["Stand", 0]
