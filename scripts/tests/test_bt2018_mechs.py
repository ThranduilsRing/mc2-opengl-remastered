"""BT2018 first-class mechs: data-mapping tests (BT JSON -> MC2 profile, weapon mapping,
hardpoint legality, compatibility-file layout) and the cook-reproducibility fix.

Offline only. Tests that read the owner's BattleTech dump or stock source data skip when
those folders are absent; everything else runs on the repo's recipes and tables.
"""
from __future__ import annotations

import copy
import csv
import io
import json
import os
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "scripts"))

from tools.bt2018_mechs import btjson, compile as C  # noqa: E402
from tools.mc2data.stock_corpus import resolve, StockDataError  # noqa: E402


def test_showcase_default_and_recipe_override():
    recipe = {"chassis_id": "test", "mc2": {"sockets": {}}}
    assert C.emit_package_json({}, recipe)["clips"] == {"showcase": "atlas_moveCoreWalkFwd"}
    recipe["mc2"]["showcase_clip"] = "custom"
    assert C.emit_package_json({}, recipe)["clips"] == {"showcase": "custom"}


@pytest.mark.parametrize("clips,valid", [(None, True), ({"showcase": "walk"}, True),
                                        ({"showcase": ""}, False), ({"showcase": 12}, False)])
def test_package_checker_showcase(tmp_path, monkeypatch, clips, valid):
    from tools.bt2018_import import check_mech_packages as checker
    nested = tmp_path / "bt2018"
    nested.mkdir()
    (tmp_path / "albedo.tga").write_bytes(b"fixture")
    pkg = {"materials": {"albedo": {"tga": "albedo.tga"}}}
    if clips is not None:
        pkg["clips"] = clips
    (nested / "test.package.json").write_text(json.dumps(pkg))
    monkeypatch.setattr(sys, "argv", ["check", "--deploy", str(tmp_path)])
    with pytest.raises(SystemExit) as result:
        checker.main()
    assert result.value.code == (0 if valid else 1)

DUMP = Path(os.environ.get("BT2018_DUMP", REPO / "scripts/tests/fixtures/bt2018-dump"))
CONVERSIONS = Path(os.environ.get("BT2018_CONVERSIONS", REPO / "scripts/tests/fixtures/bt2018-conversions"))


@pytest.fixture(scope="module")
def tables():
    return C.load_tables()


@pytest.fixture(scope="module")
def marauder_recipe():
    return C.load_json(C.RECIPES / "marauder.json")


@pytest.fixture(scope="module")
def marauder(tables, marauder_recipe):
    wm, bp = tables
    return C.build_profile(marauder_recipe, wm, bp)


# ---------------------------------------------------------------- tolerant JSON


def test_tolerant_json_repairs_are_explicit():
    clean = btjson.loads_tolerant('{"a": [1, 2]}')
    assert clean.repairs == []
    trailing = btjson.loads_tolerant('{"a": [1, 2,], "b": {"c": 1,},}', "t.json")
    assert trailing.data == {"a": [1, 2], "b": {"c": 1}}
    assert any("trailing comma" in r for r in trailing.repairs)
    missing = btjson.loads_tolerant('{"a": [\n {"x": 1}\n {"y": 2}\n]}', "m.json")
    assert missing.data["a"] == [{"x": 1}, {"y": 2}]
    assert any("missing delimiter" in r for r in missing.repairs)
    with pytest.raises(ValueError):
        btjson.loads_tolerant('{"a": ', "bad.json")


# ---------------------------------------------------------------- weapon mapping


def test_weapon_mapping_never_substitutes_silently(tables):
    wm, _ = tables
    assert C.resolve_item("Weapon_PPC_PPC_0-STOCK", wm)["master_id"] == 145
    assert C.resolve_item("Weapon_Laser_MediumLaser_0-STOCK", wm)["master_id"] == 156
    assert C.resolve_item("Weapon_LRM_LRM20_0-STOCK", wm)["kind"] == "pack"
    assert C.resolve_item("Ammo_AmmunitionBox_Generic_LRM", wm)["kind"] == "ammo"
    # A manufacturer variant is NOT folded into its STOCK item.
    assert C.resolve_item("Weapon_PPC_PPC_2-Magna", wm)["kind"] == "unsupported"
    assert C.resolve_item("Weapon_Autocannon_LB10X_0-STOCK", wm)["kind"] == "unsupported"
    assert C.resolve_item("Weapon_Totally_New_0-STOCK", wm)["kind"] == "unsupported"


def test_pack_racks_are_explicit_definitions_with_free_ordinals(tables):
    wm, _ = tables
    defs = [C.pack_definition(e, wm) for e in wm["pack_equipment"] if not e.startswith("_")]
    ordinals = [d["ordinal"] for d in defs]
    assert len(set(ordinals)) == len(ordinals)
    assert all(0 < o < 255 for o in ordinals)
    lrm10 = C.pack_definition("bt2018:lrm10", wm)
    assert lrm10["damage"] == pytest.approx(6.0)          # 2 x LRM5 (3.0)
    assert lrm10["missile_type"] == "LRM"
    assert lrm10["ammo_master_id"] == 220
    text = C.emit_equipment_def(lrm10)
    assert "l LegacyMasterId=60" in text and 'st MissileType="LRM"' in text and "b Enabled=TRUE" in text


def test_stock_snapshots_match_compbas(tables):
    try:
        compbas = resolve("objects", names=["compbas.csv"]).dir / "compbas.csv"
    except StockDataError as exc:
        pytest.skip(str(exc))
    wm, _ = tables
    rows = {}
    for row in csv.reader(io.StringIO(compbas.read_text(encoding="latin-1"))):
        try:
            rows[int(row[0])] = row
        except (ValueError, IndexError):
            pass
    for mid, s in wm["stock_components"].items():
        if mid.startswith("_"):
            continue
        row = rows[int(mid)]
        assert row[1] == s["form"], mid
        assert float(row[6]) == pytest.approx(s["weight"]), mid
        assert (int(row[27]), int(row[28])) == (s["w"], s["h"]), mid
    for mid, d in wm["derive_rows"].items():
        if mid.startswith("_"):
            continue
        row = rows[int(mid)]
        assert float(row[4]) == pytest.approx(d["recycle"]), mid
        assert float(row[7]) == pytest.approx(d["damage"]), mid
        assert int(row[21]) == d["special_effect"], mid
    for pack in wm["pack_equipment"].values():
        if isinstance(pack, dict):
            assert rows.get(pack["ordinal"], ["", "undefined"])[1] == "undefined", pack["ordinal"]


@pytest.mark.skipif(not DUMP.is_dir(), reason="BattleTech dump not present")
def test_every_dump_inventory_item_has_an_explicit_mapping(tables):
    wm, _ = tables
    kinds = {}
    for p in sorted(DUMP.glob("mechdef_*.json")):
        md = btjson.load_tolerant(p).data
        for inv in md.get("inventory", []):
            rec = C.resolve_item(inv["ComponentDefID"], wm)
            kinds[rec["kind"]] = kinds.get(rec["kind"], 0) + 1
            if rec["kind"] == "unsupported":
                assert rec["reason"]
    assert kinds.get("stock", 0) > 0 and kinds.get("pack", 0) > 0


@pytest.mark.skipif(not DUMP.is_dir(), reason="BattleTech dump not present")
def test_json_backed_chassis_parses_to_locations_and_mounts():
    cd = btjson.load_tolerant(DUMP / "chassisdef_archer_ARC-2R.json").data
    locs = btjson.chassis_locations(cd)
    assert locs["LeftTorso"]["mounts"] == ["Missile"]
    assert locs["CenterTorso"]["mounts"] == ["Energy", "Energy"]
    assert locs["LeftTorso"]["max_rear"] == 75 and locs["LeftArm"]["max_rear"] == -1
    md = btjson.load_tolerant(DUMP / "mechdef_archer_ARC-2R.json").data
    armor = btjson.mechdef_armor(md)
    assert armor["CenterTorso"] == (165, 50)


# ---------------------------------------------------------------- Marauder profile


def test_marauder_profile_keeps_bt_identity_and_raw_values(marauder):
    assert marauder["chassis_id"] == "bt2018:marauder"
    assert marauder["stem"] == "bt2018_marauder"
    assert marauder["raw"]["Tonnage"] == 75
    m = marauder["mc2"]
    assert m["structure"] == {"Head": 3, "LeftArm": 12, "RightArm": 12, "LeftTorso": 16, "RightTorso": 16,
                              "CenterTorso": 23, "LeftLeg": 16, "RightLeg": 16}
    assert m["speed"]["walk_mp"] == 4 and m["speed"]["run_mp"] == 6 and m["speed"]["mc2_class"] == 19
    assert m["payload"] == 34 and m["empty_mass"] == 41
    assert len(marauder["hardpoints"]) == 10   # install MAD-3R mounts plus the pack Longbow left torso missile mount
    assert marauder["report"] == []


def test_marauder_stock_variant_mounts_and_fits(marauder):
    mad = marauder["variants"][0]
    assert mad["name"] == "Marauder MAD-3R BT"   # the dev install already has an MC2X "Marauder MAD-3R"
    mounted = {(i["bt_id"], i["hardpoint"]) for i in mad["items"] if i["hardpoint"]}
    assert ("Weapon_PPC_PPC_0-STOCK", "LeftArm.Energy.0") in mounted
    assert ("Weapon_Autocannon_AC5_0-STOCK", "RightTorso.Ballistic.0") in mounted
    assert mad["payload_used"] == 30
    assert mad["ammo_raw"] == [{"bt_id": "Ammo_AmmunitionBox_Generic_AC5", "location": "LeftTorso"}]
    assert len(marauder["variants"]) == 2


def test_longbow_preserves_original_pack_loadout(marauder):
    longbow = marauder["variants"][1]
    assert longbow["name"] == "Marauder MAD-3R-L Longbow BT"
    assert [(i["bt_id"], i["hardpoint"]) for i in longbow["items"] if i["hardpoint"]] == [
        ("Weapon_PPC_PPC_0-STOCK", "LeftArm.Energy.0"),
        ("Weapon_Laser_MediumLaser_0-STOCK", "LeftArm.Energy.1"),
        ("Weapon_PPC_PPC_0-STOCK", "RightArm.Energy.0"),
        ("Weapon_Laser_MediumLaser_0-STOCK", "RightArm.Energy.1"),
        ("Weapon_LRM_LRM10_0-STOCK", "LeftTorso.Missile.0")]
    assert longbow["ammo_raw"] == [{"bt_id": "Ammo_AmmunitionBox_Generic_LRM", "location": "LeftTorso"}]
    assert sum(i["bt_id"] == "Gear_HeatSink_Generic_Standard" for i in longbow["items"]) == 2


def _bad(recipe, mutate):
    r = copy.deepcopy(recipe)
    mutate(r)
    return r


def test_illegal_loadouts_are_refused_consistently(tables, marauder_recipe):
    wm, bp = tables

    def energy_in_torso(r):
        r["mechdefs"][0]["inventory"].append({"MountedLocation": "LeftTorso", "ComponentDefID": "Weapon_Laser_MediumLaser_0-STOCK", "HardpointSlot": 0})

    def third_arm_energy(r):
        r["mechdefs"][0]["inventory"].append({"MountedLocation": "LeftArm", "ComponentDefID": "Weapon_Laser_MediumLaser_0-STOCK", "HardpointSlot": 2})

    def overweight(r):   # payload comes from the reference (first) variant, so the heavy one is a second mechdef
        r["mechdefs"].append(copy.deepcopy(r["mechdefs"][0]))
        for _ in range(5):
            r["mechdefs"][-1]["inventory"].append({"MountedLocation": "CenterTorso", "ComponentDefID": "Gear_HeatSink_Generic_Standard", "HardpointSlot": -1})

    def no_region(r):
        del r["mc2"]["regions"]["RightTorso"]

    for mutate, msg in ((energy_in_torso, "no free Energy mount"), (third_arm_energy, "no free Energy mount"),
                        (overweight, "exceeds payload"), (no_region, "no mech-lab grid region")):
        with pytest.raises(C.ProfileError, match=msg):
            C.build_profile(_bad(marauder_recipe, mutate), wm, bp)


def test_capacity_overflow_is_refused(tables, marauder_recipe):
    wm, bp = tables

    def five_sinks_in_arm(r):
        for _ in range(4):
            r["mechdefs"][0]["inventory"].append({"MountedLocation": "RightArm", "ComponentDefID": "Gear_HeatSink_Generic_Standard", "HardpointSlot": -1})

    with pytest.raises(C.ProfileError):
        C.build_profile(_bad(marauder_recipe, five_sinks_in_arm), wm, bp)


# ---------------------------------------------------------------- compatibility files


def test_chassis_csv_layout_matches_the_engine_readers(marauder, marauder_recipe):
    text = C.emit_csv(marauder, marauder_recipe, 0)
    rows = list(csv.reader(io.StringIO(text)))
    cell = lambda r, c: rows[r - 1][c - 1]   # CSVFile is 1-based
    assert cell(3, 2) == "Marauder" and cell(3, 5) == "75"
    assert cell(5, 2) == str(C.STRING_ID_BASE)            # chassis name string id
    assert cell(7, 5) == "19"                             # MaxRunSpeed class
    # Owner 11-17 08:23: restore Longbow; keep the revised layout that fits both loadouts.
    assert (cell(13, 2), cell(14, 2)) == ("6", "6")       # mech-lab grid
    assert len(rows) == 19 + 97 * len(marauder["variants"])
    for v, variant in enumerate(marauder["variants"]):
        base = 97 * v
        assert cell(23 + base, 2) == variant["name"]
        items = {int(cell(26 + base + i, 5)): rows[26 + base + i - 1] for i in range(72)}
        assert 145 in items and 156 in items
        for i in range(72):
            r = rows[26 + base + i - 1]
            if len(r) > 40 and r[40]:
                assert C.ITEM_RANGES[r[40].split(".")[0]][0] <= i <= C.ITEM_RANGES[r[40].split(".")[0]][1]
        for c in range(2, 13):
            assert 0 <= int(cell(115 + base, c)) <= 255
    assert cell(115, 2) == str(marauder["variants"][0]["armor"]["Head"])


def test_chassis_fit_lists_regions_and_mounts(marauder, marauder_recipe):
    text = C.emit_chassis_fit(marauder, marauder_recipe)
    assert "l ObjectTypeNum = 2" in text and 'st ProfileName = "bt2018_marauder"' in text
    assert 'st ChassisId = "bt2018:marauder"' in text
    assert text.count("[Hardpoint") == 10 and text.count("[Region") == 5
    assert 'st AntiPersonnel = "67,68"' in text
    assert "l LayoutRev = 3" in text


# layout digest -> the mc2.layout_rev it was pinned with. A layout change without a bump would restore old saves' coordinates blindly.
PINNED_LAYOUTS = {"bt2018:marauder": (3, "67f2b25ce41489b1")}


def _layout_digest(profile):
    import hashlib
    m = profile["mc2"]
    blob = json.dumps({"grid": m["grid"], "regions": m["regions"],
                       "hardpoints": [[h["id"], h["location"], h["mount"]] for h in profile["hardpoints"]]}, sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()[:16]


def test_layout_change_requires_a_layout_rev_bump(marauder):
    rev, digest = PINNED_LAYOUTS[marauder["chassis_id"]]
    assert marauder["mc2"]["layout_rev"] == rev
    assert _layout_digest(marauder) == digest, "chassis grid/regions/hardpoints changed: bump mc2.layout_rev in the recipe, then re-pin PINNED_LAYOUTS"


def test_appearance_ini_uses_pack_model_and_no_host_meshes(marauder, marauder_recipe):
    host = "\n".join(["FITini", "[Import]", 'st Source = "Madcat"', "[TGLData]", 'st FileName0 = "Madcat"',
                      "f Distance0 = 0.0", 'st FileName1 = "MadcatL1"', 'st ShadowName="MadcatX"',
                      "[Bounds]", "l UpperLeftX=-40", "[Gestures4]", "f StartVel=12.000000", "f EndVel=23.000000", "FITend"])
    ini = C.emit_ini(marauder, marauder_recipe, host)
    assert 'st Source = "bt2018/marauder"' in ini and 'st AnimHost = "madcat"' in ini
    assert "MadcatL1" not in ini and "MadcatX" not in ini and "Madcat\"" not in ini
    assert "f EndVel=19.000000" in ini              # run gesture scaled to the speed class
    assert ini.count("[WeaponNode") == 8


def test_package_json_names_every_socket(marauder_recipe):
    base = {"schema": "bt2018-mech-package/2", "nodes": {"weapon_leftarm": "j_LForearm", "weapon_rightarm": "j_RForearm",
                                                         "weapon_lefttorso": "j_LClavicle", "weapon_righttorso": "j_RClavicle",
                                                         "weapon_centertorso": "j_Spine2"}}
    pkg = C.emit_package_json(base, marauder_recipe)
    assert pkg["nodes"]["weapon_leftarm2"] == "j_LForearm"
    assert pkg["chassisId"] == "bt2018:marauder"
    with pytest.raises(C.ProfileError):
        C.emit_package_json({"nodes": {}}, marauder_recipe)


def test_emitters_are_deterministic(tables, marauder_recipe):
    wm, bp = tables
    a = C.build_profile(copy.deepcopy(marauder_recipe), wm, bp)
    b = C.build_profile(copy.deepcopy(marauder_recipe), wm, bp)
    assert C.emit_csv(a, marauder_recipe, 0) == C.emit_csv(b, marauder_recipe, 0)
    assert C.emit_chassis_fit(a, marauder_recipe) == C.emit_chassis_fit(b, marauder_recipe)
    assert C.emit_strings([a]) == C.emit_strings([b])


# ---------------------------------------------------------------- cook reproducibility


def _tiny_glb(path: Path, bind_quat, key_quats):
    from pygltflib import GLTF2, Scene, Node, Animation, AnimationSampler, AnimationChannel, \
        AnimationChannelTarget, Accessor, BufferView, Buffer
    times = struct.pack("<%df" % len(key_quats), *[float(i) for i in range(len(key_quats))])
    rots = b"".join(struct.pack("<4f", *q) for q in key_quats)
    blob = times + rots
    g = GLTF2(scenes=[Scene(nodes=[0])], scene=0, nodes=[Node(name="j_Hip", rotation=list(bind_quat))],
              buffers=[Buffer(byteLength=len(blob))],
              bufferViews=[BufferView(buffer=0, byteOffset=0, byteLength=len(times)),
                           BufferView(buffer=0, byteOffset=len(times), byteLength=len(rots))],
              accessors=[Accessor(bufferView=0, componentType=5126, count=len(key_quats), type="SCALAR",
                                  min=[0.0], max=[float(len(key_quats) - 1)]),
                         Accessor(bufferView=1, componentType=5126, count=len(key_quats), type="VEC4")],
              animations=[Animation(name="atlas_moveCoreIdle", samplers=[AnimationSampler(input=0, output=1)],
                                    channels=[AnimationChannel(sampler=0, target=AnimationChannelTarget(node=0, path="rotation"))])])
    g.set_binary_blob(blob)
    g.save_binary(str(path))


def test_cook_retargets_from_the_raw_cache_so_repeat_cooks_match(tmp_path):
    pytest.importorskip("pygltflib")
    from bt2018_retarget_clips import retarget_file
    sys.path.insert(0, str(REPO / "tools" / "bt2018_import"))
    import importlib
    conv = importlib.import_module("convert_all_bt2018")

    s = 0.7071067811865476
    atlas = tmp_path / "atlas.glb"
    _tiny_glb(atlas, (0, 0, 0, 1), [(0, 0, 0, 1), (0, 0, s, s)])
    raw = Path(conv.raw_glb_path(str(tmp_path), "mech"))
    raw.parent.mkdir(parents=True)
    _tiny_glb(raw, (s, 0, 0, s), [(0, 0, 0, 1), (0, 0, s, s)])
    out = tmp_path / "mech.glb"

    retarget_file(str(raw), str(atlas), str(out))
    first = out.read_bytes()
    retarget_file(str(raw), str(atlas), str(out))          # the fixed converter's repeat run
    assert out.read_bytes() == first

    legacy = tmp_path / "legacy.glb"                         # the old in-place behaviour, for contrast
    legacy.write_bytes(raw.read_bytes())
    retarget_file(str(legacy), str(atlas), str(legacy))
    once = legacy.read_bytes()
    retarget_file(str(legacy), str(atlas), str(legacy))
    assert legacy.read_bytes() != once


# ---------------------------------------------------------------- P6 icons


def test_generated_logistics_icon_uses_the_registry_stem(tmp_path, monkeypatch):
    """Trigger: tools/bt2018_mechs/render_icons.py and build_pack.py."""
    from tools.bt2018_mechs import render_icons as R
    from tools.bt2018_mechs import build_pack as B
    import numpy as np
    from PIL import Image
    quad = np.array([[-1, 0, 0], [1, 0, 0], [1, 2, 0], [-1, 2, 0]], dtype=float)
    monkeypatch.setattr(R, "load_parts", lambda _: [(6, quad, np.array([[0, 1, 2], [0, 2, 3]]))])
    products = R.build_icons(Path("model.glb"), "bt2018_marauder", tmp_path)
    relative = "mechicons/bt2018_marauder.png"
    icon = Image.open(tmp_path / relative).convert("RGBA")
    assert icon.size == (25, 30)
    assert np.array_equal(np.array(icon), products[relative])
    assert {p[:3] for p in icon.getdata() if p[3]} <= {(32, 32, 32), (96, 96, 96), (160, 160, 160), (255, 255, 255)}
    assert len(B.sha256(tmp_path / relative)) == 64  # builder hashes every returned relative path
    assert {f"bt2018_marauder_hud_{w}x{h}.tga" for w, h in R.HUD_SIZES} <= products.keys()


def test_hud_icons_use_only_the_exact_region_palette():
    from tools.bt2018_mechs import render_icons as R
    import numpy as np
    quad = np.array([[-1, 0, 0], [1, 0, 0], [1, 2, 0], [-1, 2, 0]], dtype=float)
    tris = np.array([[0, 1, 2], [0, 2, 3]])
    parts = [(R.region_of("mad_left_arm_forearm"), quad + [2, 0, 0], tris),
             (R.region_of("mad_centre_torso"), quad, tris),
             (R.region_of("mad_right_leg_calf"), quad - [2, 0, 0], tris)]
    assert [p[0] for p in parts] == [2, 6, 1]
    assert R.region_of("mad_left_arm_forearm_dmg") is None and R.region_of("uixMdlIndc_mechBlip") is None
    for w, h in R.HUD_SIZES:
        strip = R.hud_strip(parts, w, h)
        assert strip.shape == (2 * h, w, 4)
        assert R.palette_ok(strip, rear_row_from=h) == []
        reds = {int(v) for v in strip[:h, :, 0][strip[:h, :, 3] > 0]}
        assert {194 + 2, 194 + 6, 194 + 1} & reds          # region offset is in the red channel
        rear = strip[h:]
        assert not ({194 + 2, 194 + 1} & {int(v) for v in rear[:, :, 0][rear[:, :, 3] > 0]})   # no limbs on the rear row


MARAUDER_GLB = CONVERSIONS / "marauder.glb"


@pytest.mark.skipif(not MARAUDER_GLB.is_file(), reason="converted Marauder not present")
def test_muzzle_sockets_sit_on_geometry_not_joint_centres(marauder_recipe):
    from tools.bt2018_mechs import sockets as SK
    import numpy as np
    joints = {"weapon_leftarm": "j_LForearm", "weapon_leftarm2": "j_LForearm",
              "weapon_rightarm": "j_RForearm", "weapon_centertorso": "j_Spine2"}
    off = SK.socket_offsets(MARAUDER_GLB, joints)
    assert set(off) == set(joints)
    for name, v in off.items():
        assert np.linalg.norm(v) > 1e-3, name               # not the joint centre
    assert off["weapon_leftarm"] != off["weapon_leftarm2"]   # two mounts, two muzzles
    pkg = C.emit_package_json({"nodes": {"weapon_leftarm": "j_LForearm", "weapon_rightarm": "j_RForearm",
                                         "weapon_lefttorso": "j_LClavicle", "weapon_righttorso": "j_RClavicle",
                                         "weapon_centertorso": "j_Spine2"}}, marauder_recipe, off)
    assert len(pkg["socketOffsets"]["weapon_leftarm"].split()) == 3


def test_entire_converted_roster_compiles_with_unique_ids(tables):
    """Trigger: tools/bt2018_mechs/{roster.py,data/roster.json,build_pack.py,compile.py}."""
    from tools.bt2018_mechs import roster
    conversions = CONVERSIONS
    if not conversions.is_dir():
        pytest.skip("external conversions absent; set BT2018_CONVERSIONS to the converted dump")
    recipes = roster.recipes(conversions)
    profiles = [C.build_profile(r, *tables) for r in recipes]
    assert len(profiles) == 51
    assert len({p["stem"] for p in profiles}) == 51
    ids = [sid for i, p in enumerate(profiles) for sid in C.string_ids(p, i).values()]
    assert len(set(ids)) == 102
    for p, recipe in zip(profiles, recipes):
        assert 0 < p["mc2"]["payload"] < p["mc2"]["tonnage"]
        assert (conversions / recipe["mc2"]["model"]["source_glb"]).is_file()
        package = C.load_json(conversions / recipe["mc2"]["model"]["package_json"])
        C.emit_package_json(package, recipe)  # Every authored socket must resolve.
        C.emit_csv(p, recipe, profiles.index(p))
        if p["family"] != "marauder":
            assert "pack-authored MC2 adaptation" in p["provenance"]["gameplay"]


def test_roster_equipment_resolves_without_external_dump(tables, tmp_path):
    """Trigger: tools/bt2018_mechs/{roster.py,data/roster.json,compile.py}."""
    from tools.bt2018_mechs import roster
    from collections import Counter
    rows = C.load_json(C.DATA / "roster.json")
    assert rows["_source"]
    manifest = [{"btname": family, "glb": family + ".glb", "tga": family + ".tga"}
                for family in rows if not family.startswith("_")]
    (tmp_path / "conversions_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    wm, _ = tables
    stock = {int(k) for k in wm["stock_components"] if not k.startswith("_")}
    added = {v["ordinal"] for v in wm["pack_equipment"].values() if isinstance(v, dict)}
    assert stock.isdisjoint(added)  # ADD never replaces a stock component.
    recipes = roster.recipes(tmp_path)
    profiles = [C.build_profile(r, *tables) for r in recipes]
    assert len(profiles) == 51
    for p in profiles:
        for v in p["variants"]:
            ids = [i["mc2"]["master_id"] for i in v["items"]]
            assert [i for i in ids if i in stock | added] == ids, p["family"]
    ann = next(p for p in profiles if p["family"] == "annihilator")["variants"][0]
    counts = Counter(i["mc2"]["master_id"] for i in ann["items"])
    assert counts[wm["pack_equipment"]["bt2018:ac2"]["ordinal"]] == 4
    assert counts[156] == 2
    mad = next(p for p in profiles if p["family"] == "marauder")["variants"][0]
    stock_ids = [i["mc2"]["master_id"] for i in mad["items"]]
    assert [i for i in stock_ids if i in stock] == stock_ids
    assert [i for i in stock_ids if i in stock | added] == stock_ids


def test_spawn_fixture_uses_a_shipped_pilot_with_combat_skills():
    """A missing pilot leaves zero skills and turns weapon status into 0/0."""
    import hashlib
    import re
    from tools.mc2data import stock_missions as sm

    source = (REPO / "code/missiongui_uicmd_fixture.cpp").read_text(encoding="utf-8")
    pilot = re.search(r'strcpy\(data\.pilotFileName,\s*"([^"]+)"\)', source)
    assert pilot, "spawn fixture must name its pilot profile"
    fst = sm.find_deployed_fst()
    if fst is None:
        pytest.skip("deployed stock mission.fst not present")
    data, members = sm.fst_members(fst)
    relative = f"warriors/{pilot.group(1)}.fit"
    member = next((m for m in members if m[0] == "data/missions/" + relative), None)
    assert member is not None, f"spawn pilot {relative} is absent from shipped mission.fst"
    _, off, size, real = member
    blob = sm.fst_blob(data, off, size, real)
    assert blob is not None
    assert hashlib.md5(blob).hexdigest() == sm.load_manifest()[relative]
    text = blob.decode("latin-1")
    for skill in ("Gunnery", "Piloting"):
        value = re.search(rf"(?im)^\s*c\s+{skill}\s*=\s*(\d+)", text)
        assert value and int(value.group(1)) > 0, f"spawn pilot needs usable {skill}"
