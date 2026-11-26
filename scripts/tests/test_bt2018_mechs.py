"""BT2018 first-class mechs: data-mapping tests (BT JSON -> MC2 profile, weapon mapping,
hardpoint legality, compatibility-file layout) and the cook-reproducibility fix.

Offline only. BT schema and socket tests use tiny synthetic checked-in fixtures;
stock corpus checks use the repository's stock-data resolver.
"""
from __future__ import annotations

import copy
import csv
import io
import json
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
    assert C.emit_package_json({}, recipe)["clips"] == {"showcase": "atlas_moveCoreIdle"}
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

FIXTURES = REPO / "scripts/tests/fixtures/bt2018"


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
    for d in defs:
        description = d["description"]
        assert description.endswith(" (attr: sarna.net)")
        assert 2 <= description.removesuffix(" (attr: sarna.net)").count(".") <= 4
        assert 'st Description="' + description + '"' in C.emit_equipment_def(d)
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


def test_fixture_inventory_items_have_explicit_mappings(tables):
    wm, _ = tables
    kinds = {}
    for p in sorted(FIXTURES.glob("mechdef_*.json")):
        md = btjson.load_tolerant(p).data
        for inv in md.get("inventory", []):
            rec = C.resolve_item(inv["ComponentDefID"], wm)
            kinds[rec["kind"]] = kinds.get(rec["kind"], 0) + 1
            if rec["kind"] == "unsupported":
                assert rec["reason"]
    assert kinds.get("stock", 0) > 0 and kinds.get("pack", 0) > 0


def test_json_backed_chassis_parses_to_locations_and_mounts():
    cd = btjson.load_tolerant(FIXTURES / "chassisdef_fixture.json").data
    locs = btjson.chassis_locations(cd)
    assert locs["LeftTorso"]["mounts"] == ["Missile"]
    assert locs["CenterTorso"]["mounts"] == ["Energy", "Energy"]
    assert locs["LeftTorso"]["max_rear"] == 75 and locs["LeftArm"]["max_rear"] == -1
    md = btjson.load_tolerant(FIXTURES / "mechdef_fixture.json").data
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

    retarget_file(str(out), str(atlas), str(out))           # re-running on a shipped GLB is a no-op
    assert out.read_bytes() == first


def test_retarget_turns_bones_in_model_space_whatever_the_bind_frame(tmp_path):
    """Trigger: scripts/bt2018_retarget_clips.py. Owner 11-16: arms wrong except on the Atlas."""
    pytest.importorskip("pygltflib")
    from bt2018_retarget_clips import retarget_file, quat_mul, quat_conj
    from pygltflib import GLTF2
    import numpy as np
    s = 0.7071067811865476
    atlas, mech = tmp_path / "atlas.glb", tmp_path / "mech.glb"
    _tiny_glb(atlas, (0, 0, 0, 1), [(0, 0, 0, 1), (0, 0, s, s)])   # Atlas: +90 deg about Z
    _tiny_glb(mech, (s, 0, 0, s), [(0, 0, 0, 1), (0, 0, s, s)])    # bind frame turned 90 about X
    retarget_file(str(mech), str(atlas), str(mech))
    g = GLTF2().load(str(mech))
    key = np.frombuffer(g.binary_blob(), "<f4", 4, 4 * 2 + 16)
    delta = quat_mul(key, quat_conj(np.array([s, 0, 0, s])))      # world turn away from bind
    assert np.degrees(2 * np.arccos(min(1.0, abs(float(delta @ [0, 0, s, s]))))) < 0.5


def test_native_clip_baker_decodes_streamed_dense_and_constant_curves():
    """Trigger: tools/bt2018_import/bake_clips.py (native BT clips replace Atlas retargets)."""
    from tools.bt2018_import import bake_clips as B
    # one streamed curve: key at t=0 value 1 slope 2, key at t=1 constant 5; one constant curve 7
    words = struct.unpack("<14I", struct.pack("<fi i4f fi i4f", 0.0, 1, 0, 0, 0, 2.0, 1.0, 1.0, 1, 0, 0, 0, 0, 5.0))
    clip = {"m_StreamedClip": {"data": list(words), "curveCount": 1},
            "m_DenseClip": {"m_CurveCount": 1, "m_SampleArray": [0.0, 10.0], "m_BeginTime": 0.0, "m_SampleRate": 1.0},
            "m_ConstantClip": {"data": [7.0]}}
    vals = B.sample_clip({"m_MuscleClip": {"m_Clip": {"data": clip}}}, [0.0, 0.5, 1.0])
    assert vals.tolist() == [[1.0, 0.0, 7.0], [2.0, 5.0, 7.0], [5.0, 10.0, 7.0]]


def test_partial_own_clips_preserve_the_existing_retarget_per_missing_state(tmp_path):
    from pygltflib import GLTF2
    import numpy as np
    from bt2018_retarget_clips import retarget_file, accessor_view
    atlas, raw, baseline, out = [tmp_path / f"{n}.glb" for n in ("atlas", "raw", "baseline", "own")]
    s = 2 ** -0.5
    for p, bind in ((atlas, (0, 0, 0, 1)), (raw, (s, 0, 0, s))):
        _tiny_glb(p, bind, [(0, 0, 0, 1), (0, 0, s, s)])
        g = GLTF2().load(str(p))
        fire = copy.deepcopy(g.animations[0])
        fire.name = "atlas_attackFirePelvisBack"
        g.animations.append(fire)
        g.save(str(p))
    retarget_file(str(raw), str(atlas), str(baseline))
    own = np.array([[0, s, 0, s], [0, s, 0, s]])
    retarget_file(str(raw), str(atlas), str(out), {"atlas_moveCoreIdle": (1, {"j_Hip": own})})
    before, after = GLTF2().load(str(baseline)), GLTF2().load(str(out))
    def keys(g, name):
        a = next(a for a in g.animations if a.name == name)
        return accessor_view(g, g.binary_blob(), a.samplers[0].output)[0]
    assert {a.name for a in after.animations} == {a.name for a in before.animations}
    np.testing.assert_array_equal(keys(after, "atlas_attackFirePelvisBack"), keys(before, "atlas_attackFirePelvisBack"))
    np.testing.assert_allclose(keys(after, "atlas_moveCoreIdle"), own)


def test_baked_clip_buffer_views_stay_inside_the_binary_chunk(tmp_path):
    """Trigger: bake_clips.add_to_glb. f8 keys once sized views at 2x the f4 bytes written: Assimp rejected 50/51 GLBs (3 mechs drew placeholders)."""
    pygltflib = pytest.importorskip("pygltflib")
    import numpy as np
    from tools.bt2018_import import bake_clips as B
    g = pygltflib.GLTF2(nodes=[pygltflib.Node(name="j_Root")], buffers=[pygltflib.Buffer(byteLength=4)])
    g.set_binary_blob(b"\0\0\0\0")
    g.save(str(tmp_path / "in.glb"))
    rots = {"j_Root": np.tile(np.array([0.0, 0.0, 0.0, 1.0]), (3, 1))}   # float64, like the baker's output
    B.add_to_glb(str(tmp_path / "in.glb"), str(tmp_path / "out.glb"), {"atlas_idle": (0.1, rots)})
    out = pygltflib.GLTF2().load(str(tmp_path / "out.glb"))
    end = len(out.binary_blob())
    assert all(v.byteOffset + v.byteLength <= end for v in out.bufferViews)


def test_native_decoder_uses_avatar_paths_and_rotation_only(tmp_path):
    from tools.bt2018_import import bake_clips as B
    from pygltflib import GLTF2
    tt = {"m_Name": "crab_idle", "m_MuscleClip": {"m_StartTime": 0.0, "m_StopTime": 0.1,
          "m_Clip": {"m_StreamedClip": {"data": [], "curveCount": 0},
          "m_DenseClip": {"m_CurveCount": 0}, "m_ConstantClip": {"data": [100, 200, 300, 0.5, 0.5, 0.5, 0.5, 0, 0, 0, 1]}}},
          "m_ClipBindingConstant": {"genericBindings": [
              {"typeID": 4, "attribute": 1, "path": 42},
              {"typeID": 4, "attribute": 2, "path": 42},
              {"typeID": 4, "attribute": 2, "path": 99}]}}
    dur, rots = B.decode_clip(tt, {42: "j_Root"})
    assert list(rots) == ["j_Root"]  # helper 99 is outside the Avatar
    assert rots["j_Root"][0].tolist() == [0.5, -0.5, -0.5, 0.5]
    from pygltflib import Node, Buffer
    g = GLTF2(nodes=[Node(name="j_Root")], buffers=[Buffer(byteLength=4)])
    g.set_binary_blob(b"\0" * 4)
    g.save(str(tmp_path / "raw.glb"))
    from bt2018_retarget_clips import retarget_file
    retarget_file(str(tmp_path / "raw.glb"), "absent-atlas.glb", str(tmp_path / "own.glb"),
                  own_clips={"atlas_idle": (dur, rots)})
    own = GLTF2().load(str(tmp_path / "own.glb"))
    assert [a.name for a in own.animations] == ["atlas_idle"]
    assert {c.target.path for c in own.animations[0].channels} == {"rotation"}
    times = B.np.frombuffer(own.binary_blob(), '<f4', 4, own.bufferViews[0].byteOffset)
    assert times[-1] == pytest.approx(dur)


def test_native_controller_keeps_aliases_instead_of_guessing_suffixes(monkeypatch):
    from types import SimpleNamespace as NS
    from tools.bt2018_import import bake_clips as B
    clip = NS(path_id=7, read_typetree=lambda: {"m_Name": "catapult_attackMeleePush"})
    entries = [NS(m_OriginalClip=NS(deref=lambda n=n: NS(peek_name=lambda: n)),
                  m_OverrideClip=NS(path_id=7, deref=lambda: clip))
               for n in ("atlas_attackMeleePush", "atlas_attackMeleePunch", "catapult_moveCoreIdle")]
    entries.append(NS(m_OverrideClip=NS(path_id=0)))  # No own clip: leave this state to Atlas.
    controller = NS(type=NS(name="AnimatorOverrideController"),
                    peek_name=lambda: "charoverctrl_catapultBase-001",
                    read=lambda: NS(m_Clips=entries))
    avatar = NS(type=NS(name="Avatar"), read_typetree=lambda: {"m_TOS": [(42, "j_Root/j_Arm")]})
    monkeypatch.setitem(sys.modules, "UnityPy", NS(__version__=B.UNITYPY_VERSION,
                                                  load=lambda _: NS(objects=[controller, avatar])))
    seen = []
    monkeypatch.setattr(B, "decode_clip", lambda tt, paths: seen.append((tt, paths)) or (1, {}))
    clips = B.native_clips("read-only-bundle", "catapult")
    assert set(clips) == {"atlas_attackMeleePush", "atlas_attackMeleePunch",
                          "catapult_moveCoreIdle", "atlas_moveCoreIdle"}
    assert clips["catapult_moveCoreIdle"] is clips["atlas_moveCoreIdle"]
    assert len(seen) == 1 and seen[0][1] == {42: "j_Arm"}


def test_native_search_uses_dlc_and_falls_back_only_when_absent(tmp_path, monkeypatch):
    from tools.bt2018_import import bake_clips as B
    for name in ("chrprfmech_ravenbase-001", "urbanwarfare"):
        (tmp_path / name).touch()
    monkeypatch.setattr(B, "native_clips", lambda path, fam: {"atlas_idle": (1, {})}
                        if Path(path).name == "urbanwarfare" and fam == "raven" else {})
    assert B.chassis_clips(tmp_path, "raven") == {"atlas_idle": (1, {})}
    assert B.chassis_clips(tmp_path, "absent") == {}


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


def test_muzzle_sockets_sit_on_geometry_not_joint_centres(marauder_recipe):
    from tools.bt2018_mechs import sockets as SK
    import numpy as np
    joints = {"weapon_leftarm": "j_LForearm", "weapon_leftarm2": "j_LForearm",
              "weapon_rightarm": "j_RForearm", "weapon_centertorso": "j_Spine2"}
    off = SK.socket_offsets(REPO / "scripts/tests/fixtures/synthetic_mech/sockets_fixture.glb", joints)
    assert set(off) == set(joints)
    for name, v in off.items():
        assert np.linalg.norm(v) > 1e-3, name               # not the joint centre
    assert off["weapon_leftarm"] != off["weapon_leftarm2"]   # two mounts, two muzzles
    pkg = C.emit_package_json({"nodes": {"weapon_leftarm": "j_LForearm", "weapon_rightarm": "j_RForearm",
                                         "weapon_lefttorso": "j_LClavicle", "weapon_righttorso": "j_RClavicle",
                                         "weapon_centertorso": "j_Spine2"}}, marauder_recipe, off)
    assert len(pkg["socketOffsets"]["weapon_leftarm"].split()) == 3


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
    assert len({p["stem"] for p in profiles}) == 51
    ids = [sid for i, p in enumerate(profiles) for sid in C.string_ids(p, i).values()]
    assert len(set(ids)) == 102
    for p in profiles:
        assert 0 < p["mc2"]["payload"] < p["mc2"]["tonnage"]
        if p["family"] != "marauder":
            assert "pack-authored MC2 adaptation" in p["provenance"]["gameplay"]
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


def test_every_roster_chassis_places_its_sensor_and_items(tables, tmp_path):
    """mc2_18 invisible attackers: an empty ComponentPlacement grid left BT2018 mechs with
    no sensor system. Trigger: tools/bt2018_mechs/{roster.py,data/roster.json,compile.py}."""
    from tools.bt2018_mechs import roster
    rows = C.load_json(C.DATA / "roster.json")
    manifest = [{"btname": f, "glb": f + ".glb", "tga": f + ".tga"} for f in rows if not f.startswith("_")]
    (tmp_path / "conversions_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    recipes = roster.recipes(tmp_path)
    assert len(recipes) == 51
    for index, recipe in enumerate(recipes):
        p = C.build_profile(recipe, *tables)
        csv_rows = list(csv.reader(io.StringIO(C.emit_csv(p, recipe, index))))
        for v in range(len(p["variants"])):
            base = 97 * v
            items = {i: int(csv_rows[25 + base + i][4]) for i in range(72)}
            placed = {int(csv_rows[101 + base + s][c]) for s in range(12) for c in range(1, 9)
                      if csv_rows[101 + base + s][c]}
            assert items[12] == 13 and int(csv_rows[103 + base][1]) == 12, p["family"]  # Head Slot3
            assert {i for i, mid in items.items() if mid != C.EMPTY_MASTER} <= placed, p["family"]
            if p["mc2"]["jump_capable"]:
                assert items[C.JUMP_ITEM_FIRST] != C.EMPTY_MASTER, p["family"]


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


def test_hud_icons_have_stock_tone_mix_and_no_outline_ring():
    """Trigger: tools/bt2018_mechs/render_icons.py (owner 11-16: icons read too bright and busy)."""
    from tools.bt2018_mechs import render_icons as R
    import numpy as np
    rng = np.random.default_rng(1)
    parts = []
    for region, (cx, cy) in {6: (0, 3), 2: (2, 3), 3: (-2, 3), 0: (0.6, 1), 1: (-0.6, 1)}.items():
        v = rng.normal(size=(300, 3)) * 0.35 + [cx, cy, 0]       # jittered "high-poly" blob: noisy normals
        parts.append((region, v, np.array([[i, i + 1, i + 2] for i in range(0, 297, 3)])))
    img = R.hud_strip(parts, 25, 30)[:30]
    solid = img[:, :, :3].any(2)                 # the cell is opaque black: the mech is its non-black pixels
    assert R.palette_ok(img) == [] and not (img[solid][:, :3] == R.OUTLINE).all(1).any()
    green = img[solid][:, 1]
    assert (green == 229).mean() < 0.35 and (green == 83).mean() > 0.4     # dark-dominated like stock


# ---------------------------------------------------------------- pack GLB guard


def _glb(tmp_path, names, view_len=4):
    import pygltflib
    g = pygltflib.GLTF2(nodes=[pygltflib.Node(name=n) for n in names],
                        buffers=[pygltflib.Buffer(byteLength=4)],
                        bufferViews=[pygltflib.BufferView(buffer=0, byteOffset=0, byteLength=view_len)])
    g.set_binary_blob(b"\0\0\0\0")
    g.save(str(tmp_path / "m.glb"))
    return tmp_path / "m.glb"


def test_duplicate_node_names_are_renamed_deterministically_and_first_keeps_its_name(tmp_path):
    """Trigger: tools/bt2018_mechs/glb_guard.py (raven.glb: duplicate node name 'VFXCollider_vert' broke Assimp import)."""
    pytest.importorskip("pygltflib")
    from pygltflib import GLTF2
    from tools.bt2018_mechs import glb_guard as G
    p = _glb(tmp_path, ["j_Root", "VFXCollider_vert", "VFXCollider_vert", "VFXCollider_vert"])
    assert any("duplicate node name" in b for b in G.validate_glb(p))
    assert G.dedupe_node_names(p) == {"VFXCollider_vert": 2}
    assert [n.name for n in GLTF2().load(str(p)).nodes] == ["j_Root", "VFXCollider_vert", "VFXCollider_vert__dup2", "VFXCollider_vert__dup3"]
    assert G.validate_glb(p) == [] and G.dedupe_node_names(p) == {}


def test_pack_guard_flags_out_of_range_buffer_view(tmp_path):
    """Trigger: glb_guard.validate_pack_glbs (the e7e0ff83b bufferView overrun would have failed the build)."""
    pytest.importorskip("pygltflib")
    from tools.bt2018_mechs import glb_guard as G
    _glb(tmp_path, ["j_Root"], view_len=64)
    bad = G.validate_pack_glbs(tmp_path)
    assert "out of range" in " ".join(bad["m.glb"])


def test_pack_guard_catches_the_prefix_baker_bug(tmp_path):
    """Trigger: bake_clips.add_to_glb f8 sizing; the validator alone flags that output."""
    pygltflib = pytest.importorskip("pygltflib")
    import numpy as np
    from tools.bt2018_mechs import glb_guard as G
    src = _glb(tmp_path, ["j_Root"])
    g = pygltflib.GLTF2().load(str(src))
    arr = np.zeros((3, 4))                                   # float64 nbytes written as the view length, f4 bytes stored
    g.bufferViews.append(pygltflib.BufferView(buffer=0, byteOffset=4, byteLength=arr.nbytes))
    g.buffers[0].byteLength = 4 + arr.astype("<f4").nbytes
    g.set_binary_blob(b"\0" * g.buffers[0].byteLength)
    g.save(str(src))
    assert G.validate_glb(src)


def _bin_glb(prims, mats, sparse_pos=False):
    """Binary-backed skinned GLB (in memory). prims: [dict(mesh=name, mat=material index, pos=[(x, y, z)], idx=None | [ints],
    attrs={attribute: rows of floats})]; mats: [(material name, alphaMode, image name)]. One skinned node per mesh."""
    pg = pytest.importorskip("pygltflib")
    blob, views, accs = bytearray(), [], []

    def put(data):
        while len(blob) % 4:
            blob.append(0)
        views.append(pg.BufferView(buffer=0, byteOffset=len(blob), byteLength=len(data)))
        blob.extend(data)
        return len(views) - 1

    def floats(rows, typ):
        accs.append(pg.Accessor(bufferView=put(b"".join(struct.pack(f"<{len(r)}f", *r) for r in rows)), componentType=5126, count=len(rows), type=typ))
        return len(accs) - 1

    meshes = []
    for d in prims:
        a = pg.Attributes()
        if sparse_pos:   # no base data; every vertex comes from the sparse override
            n = len(d["pos"])
            accs.append(pg.Accessor(componentType=5126, count=n, type="VEC3", sparse=pg.Sparse(
                count=n, indices=pg.AccessorSparseIndices(bufferView=put(struct.pack(f"<{n}H", *range(n))), componentType=5123),
                values=pg.AccessorSparseValues(bufferView=put(b"".join(struct.pack("<3f", *r) for r in d["pos"]))))))
            a.POSITION = len(accs) - 1
        else:
            a.POSITION = floats(d["pos"], "VEC3")
        for k, rows in d.get("attrs", {}).items():
            setattr(a, k, floats(rows, {2: "VEC2", 3: "VEC3", 4: "VEC4"}[len(rows[0])]))
        idx = None
        if d.get("idx") is not None:
            accs.append(pg.Accessor(bufferView=put(struct.pack(f"<{len(d['idx'])}H", *d["idx"])), componentType=5123, count=len(d["idx"]), type="SCALAR"))
            idx = len(accs) - 1
        meshes.append(pg.Mesh(name=d["mesh"], primitives=[pg.Primitive(attributes=a, indices=idx, material=d["mat"])]))
    imgs = sorted({m[2] for m in mats})
    g = pg.GLTF2(nodes=[pg.Node(name="j")] + [pg.Node(name=f"n{i}", mesh=i, skin=0) for i in range(len(prims))],
                 skins=[pg.Skin(joints=[0])], meshes=meshes, accessors=accs, bufferViews=views,
                 buffers=[pg.Buffer(byteLength=len(blob))], textures=[pg.Texture(source=i) for i in range(len(imgs))],
                 images=[pg.Image(name=n) for n in imgs],
                 materials=[pg.Material(name=m[0], alphaMode=m[1], pbrMetallicRoughness=pg.PbrMetallicRoughness(
                     baseColorTexture=pg.TextureInfo(index=imgs.index(m[2])))) for m in mats])
    g.set_binary_blob(bytes(blob))
    return g


def _verts(n, shift=0.0):
    return [(float(i) + shift, 0.0, 0.0) for i in range(n)]


def _save(g, tmp_path):
    p = tmp_path / "x.glb"
    g.save(str(p))
    return p


def test_pack_guard_picks_the_dominant_skin_texture_and_flags_untextured_parts(tmp_path):
    """Trigger: glb_guard.skin_texture_problems (raven.glb: first skinned part used the weapons atlas -> untextured white mech)."""
    pg = pytest.importorskip("pygltflib")
    from tools.bt2018_mechs import glb_guard as G
    g = _bin_glb([dict(mesh="forearm", mat=1, pos=_verts(3)), dict(mesh="torso", mat=0, pos=_verts(30)),
                  dict(mesh="torso_dmg", mat=2, pos=_verts(3))],
                 [("base", None, "chrTxrMech_x-base-alb.png"), ("weapons", None, "chrTxrMech_x-weapons-alb.png"), ("none", None, "chrTxrMech_x-weapons-alb.png")])
    g.materials[2].pbrMetallicRoughness.baseColorTexture = None
    p = _save(g, tmp_path)
    tex = tmp_path / "tgl"
    tex.mkdir()
    assert any("not in the pack" in b and "base-alb" in b for b in G.skin_texture_problems(p, [tex]))   # weapons-first pick would have passed
    (tex / "chrtxrmech_x-base-alb.tga").write_bytes(b"")
    assert G.skin_texture_problems(p, [tex]) == []                                                       # _dmg part without texture is dropped, not flagged
    (tex / "chrtxrmech_x-base-alb.tga").unlink()
    (tex / "chrtxrmech_x-base-alb.ktx2").write_bytes(b"")
    assert G.skin_texture_problems(p, [tex]) == []  # MW5 ships cooked KTX2 without a loose TGA.
    g.materials[0] = pg.Material(name="base")
    p = _save(g, tmp_path)
    assert any("without a base colour texture" in b for b in G.skin_texture_problems(p))
    g.nodes[0].name = "j_Root__dup2"
    p = _save(g, tmp_path)
    assert any("dedupe-renamed" in b for b in G.skin_texture_problems(p))


def test_blend_skin_material_is_made_opaque_so_the_importer_keeps_the_real_texture_name(tmp_path):
    """Trigger: glb_guard.opaque_mech_materials (raven_base was BLEND: importer asked for 'a_...tga', mech rendered white)."""
    from tools.bt2018_mechs import glb_guard as G
    p = _save(_bin_glb([dict(mesh="torso", mat=0, pos=_verts(3))], [("base", "BLEND", "x-base-alb.png")]), tmp_path)
    assert any("BLEND" in b for b in G.skin_texture_problems(p))
    assert G.opaque_mech_materials(p) == ["base"] and G.opaque_mech_materials(p) == []
    assert not any("BLEND" in b for b in G.skin_texture_problems(p))


def _pick(g):
    from tools.bt2018_mechs import glb_guard as G
    return G.skin_pick(g)[0]


def test_skin_pick_variant_priority_lexical_ties_and_alpha_scope(tmp_path):
    """Trigger: glb_guard.skin_pick mirrors assimp_importer.cpp slot-0 pick; opaque_mech_materials touches only the skin material."""
    from tools.bt2018_mechs import glb_guard as G
    pg = pytest.importorskip("pygltflib")
    # canopy glass stays BLEND, body BLEND becomes OPAQUE
    g = _bin_glb([dict(mesh="body", mat=0, pos=_verts(9)), dict(mesh="canopy", mat=1, pos=_verts(4))],
                 [("base", "BLEND", "b-base-alb.png"), ("glass", "BLEND", "c-glass-alb.png")])
    p = _save(g, tmp_path)
    assert G.opaque_mech_materials(p) == ["base"]
    assert [m.alphaMode for m in pg.GLTF2().load(str(p)).materials] == ["OPAQUE", "BLEND"]
    assert G.skin_texture_problems(p) == []                                  # the canopy BLEND is not a skin problem
    # variant material wins over the larger plain atlas
    g = _bin_glb([dict(mesh="body", mat=0, pos=_verts(9)), dict(mesh="arm", mat=1, pos=_verts(2))],
                 [("x_base", "OPAQUE", "b-base-alb.png"), ("x_widow", "OPAQUE", "a-widow-alb.png")])
    assert _pick(g) == "a-widow-alb.tga"
    # equal counts: lexically first texture, not file order
    g = _bin_glb([dict(mesh="a", mat=0, pos=_verts(5)), dict(mesh="b", mat=1, pos=_verts(5))],
                 [("m1", "OPAQUE", "z-alb.png"), ("m2", "OPAQUE", "c-alb.png")])
    assert _pick(g) == "c-alb.tga"
    # a MASK skin material is refused, not silently converted
    p = _save(_bin_glb([dict(mesh="body", mat=0, pos=_verts(9))], [("base", "MASK", "b-base-alb.png")]), tmp_path)
    with pytest.raises(ValueError, match="MASK"):
        G.opaque_mech_materials(p)


def test_skin_pick_alpha_prefix_decides_ties_like_the_engine_and_the_guard_fails_the_missing_texture(tmp_path):
    """Trigger: glb_guard.skin_pick/_tga_name; equal-count OPAQUE b vs BLEND z is a_z.tga in the engine (DeriveMC2TextureName)."""
    from tools.bt2018_mechs import glb_guard as G
    g = _bin_glb([dict(mesh="p", mat=0, pos=_verts(5)), dict(mesh="q", mat=1, pos=_verts(5))],
                 [("m1", "OPAQUE", "b-alb.png"), ("m2", "BLEND", "z-alb.png")])
    assert _pick(g) == "a_z-alb.tga"                                          # 'a_z' sorts before 'b': a plain-name pick would say b
    p = _save(g, tmp_path)
    tex = tmp_path / "tgl"
    tex.mkdir()
    (tex / "b-alb.tga").write_bytes(b"")
    bad = G.skin_texture_problems(p, [tex])                                   # validation is engine-equivalent and runs first
    assert any("BLEND" in b for b in bad) and any("a_z-alb.tga" in b and "not in the pack" in b for b in bad)
    assert G.opaque_mech_materials(p) == ["m2"]                               # repair re-picks after the change: b wins the tie
    assert _pick(__import__("pygltflib").GLTF2().load(str(p))) == "b-alb.tga" and G.skin_texture_problems(p, [tex]) == []
    # an OPAQUE body and a BLEND decal on one atlas are two engine keys; the decal is not clobbered
    g = _bin_glb([dict(mesh="body", mat=0, pos=_verts(9)), dict(mesh="decal", mat=1, pos=_verts(2))],
                 [("base", "OPAQUE", "x-alb.png"), ("decal", "BLEND", "x-alb.png")])
    p = _save(g, tmp_path)
    assert _pick(g) == "x-alb.tga" and G.opaque_mech_materials(p) == []


def test_skin_pick_counts_post_join_referenced_vertices_of_every_compared_attribute(tmp_path):
    """Trigger: glb_guard._joined_vertex_count mirrors aiProcess_JoinIdenticalVertices (indices, tangent, uv1, colour, sparse)."""
    mats = [("m1", "OPAQUE", "a-alb.png"), ("m2", "OPAQUE", "b-alb.png")]
    # deduplication: 12 raw vertices that are 2 distinct positions lose to 3 distinct ones
    dup = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0)] * 6
    assert _pick(_bin_glb([dict(mesh="a", mat=0, pos=dup), dict(mesh="b", mat=1, pos=_verts(3))], mats)) == "b-alb.tga"
    # indices: 100 buffer vertices of which only 2 are referenced lose to 3 distinct; 2 distinct referenced tie-breaks lexically
    assert _pick(_bin_glb([dict(mesh="a", mat=0, pos=_verts(100), idx=[0, 1, 1]), dict(mesh="b", mat=1, pos=_verts(3))], mats)) == "b-alb.tga"
    assert _pick(_bin_glb([dict(mesh="a", mat=0, pos=_verts(100), idx=[0, 1, 1]), dict(mesh="b", mat=1, pos=_verts(2))], mats)) == "a-alb.tga"
    # a face with an out-of-range index is dropped by the importer
    assert _pick(_bin_glb([dict(mesh="a", mat=0, pos=_verts(5), idx=[0, 1, 2, 3, 4, 99]), dict(mesh="b", mat=1, pos=_verts(4))], mats)) == "b-alb.tga"
    # one position with differing TEXCOORD_1 / TANGENT / COLOR_0 stays separate (needs a NORMAL for the tangent to count)
    same = [(0.0, 0.0, 0.0)] * 4
    nrm = [(0.0, 0.0, 1.0)] * 4
    for attr, rows in (("TEXCOORD_1", [(0.0, 0.0), (0.5, 0.0), (0.0, 0.5), (0.5, 0.5)]),
                       ("COLOR_0", [(1.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0), (1.0, 1.0, 1.0, 1.0)]),
                       ("TANGENT", [(1.0, 0.0, 0.0, 1.0), (1.0, 0.0, 0.0, -1.0), (0.0, 1.0, 0.0, 1.0), (0.0, 1.0, 0.0, -1.0)])):
        a = dict(mesh="a", mat=0, pos=same, attrs={"NORMAL": nrm, attr: rows})
        assert _pick(_bin_glb([a, dict(mesh="b", mat=1, pos=_verts(2))], mats)) == "a-alb.tga", attr    # 4 distinct beat 2
        a = dict(mesh="a", mat=0, pos=same, attrs={"NORMAL": nrm})
        assert _pick(_bin_glb([a, dict(mesh="b", mat=1, pos=_verts(2))], mats)) == "b-alb.tga", attr    # without it they join to 1
    # TEXCOORD_8 / COLOR_8 are past Assimp's eight-channel limit: ignored, so the four vertices still join to 1
    for attr, rows in (("TEXCOORD_8", [(0.0, 0.0), (0.5, 0.0), (0.0, 0.5), (0.5, 0.5)]),
                       ("COLOR_8", [(1.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0), (1.0, 1.0, 1.0, 1.0)])):
        a = dict(mesh="a", mat=0, pos=same, attrs={"NORMAL": nrm, attr: rows})
        assert _pick(_bin_glb([a, dict(mesh="b", mat=1, pos=_verts(2))], mats)) == "b-alb.tga", attr
    # sparse accessor: the override rows are the vertices (a base-only read would see 1 zero vertex)
    assert _pick(_bin_glb([dict(mesh="a", mat=0, pos=_verts(5)), dict(mesh="b", mat=1, pos=_verts(3))], mats, sparse_pos=True)) == "a-alb.tga"


def test_skin_pick_rejects_layouts_it_cannot_decode_instead_of_guessing(tmp_path):
    """Trigger: glb_guard._buffer/_read; no silent fallback to raw counts for external buffers or overruns."""
    from tools.bt2018_mechs import glb_guard as G
    g = _bin_glb([dict(mesh="a", mat=0, pos=_verts(3))], [("m1", "OPAQUE", "a-alb.png")])
    g.buffers[0].uri = "mech.bin"
    with pytest.raises(G.UnsupportedGlb, match="external file"):
        G.skin_pick(g)
    g = _bin_glb([dict(mesh="a", mat=0, pos=_verts(3))], [("m1", "OPAQUE", "a-alb.png")])
    g.accessors[0].count = 400
    with pytest.raises(G.UnsupportedGlb, match="past its data"):
        G.skin_pick(g)
    import base64
    g = _bin_glb([dict(mesh="a", mat=0, pos=_verts(3))], [("m1", "OPAQUE", "a-alb.png")])
    g.buffers[0].uri = "data:application/octet-stream;base64," + base64.b64encode(g.binary_blob()).decode()
    assert _pick(g) == "a-alb.tga"                                            # data: buffers decode


def test_arm_lift_fix_lowers_a_raised_idle_hand_to_the_cap(tmp_path):
    """Trigger: scripts/bt2018_retarget_clips.py. Owner 11-23: crab/rifleman/stalker arms up at idle."""
    pygltflib = pytest.importorskip("pygltflib")
    import numpy as np
    from bt2018_retarget_clips import arm_lift_fix, quat_mul, quat_mat, elevation
    N = pygltflib.Node
    g = pygltflib.GLTF2(nodes=[N(name="j_LUpperArm", children=[1]), N(name="j_LHand", translation=[1.0, 0, 0.8])])
    ident = np.array([0.0, 0.0, 0.0, 1.0])
    h = np.radians(45) / 2
    raised = np.array([0.0, 0.0, np.sin(h), np.cos(h)])                  # hand 45 deg above the shoulder
    idle = [raised, raised]
    ua, fix = arm_lift_fix(g, {1: 0}, [ident, ident], idle, "L")
    assert ua == 0
    hand = quat_mat(quat_mul(fix, raised)) @ [1.0, 0, 0.8]
    assert abs(elevation(hand) - (-10.0)) < 0.01                          # cap = bind 0 + 10, clamped to -10
    assert arm_lift_fix(g, {1: 0}, [ident, ident], [quat_mul(fix, raised)] * 2, "L") is None   # already low: untouched


def test_arm_lift_fix_leaves_sideways_shoulder_pods_alone():
    """Trigger: scripts/bt2018_retarget_clips.py. Owner 11-23: Catapult missile pods must stay level."""
    pygltflib = pytest.importorskip("pygltflib")
    import numpy as np
    from bt2018_retarget_clips import arm_lift_fix, pod_arm
    N = pygltflib.Node
    ident = np.array([0.0, 0.0, 0.0, 1.0])
    h = np.radians(45) / 2
    raised = np.array([0.0, 0.0, np.sin(h), np.cos(h)])
    for hand_t, fixed in (([1.0, 0, 0.1], False), ([0.1, 0, 1.0], True)):      # sideways pod vs forward arm
        g = pygltflib.GLTF2(nodes=[N(name="j_LUpperArm", children=[1]), N(name="j_LHand", translation=hand_t)])
        assert (arm_lift_fix(g, {1: 0}, [ident, ident], [raised, raised], "L") is not None) == fixed
        assert (pod_arm(g, {1: 0}, [ident, ident], "L") is None) == fixed          # pods are held at bind


def test_pose_error_is_zero_at_bind_and_flags_a_displaced_arm():
    """Trigger: scripts/bt2018_retarget_clips.py. Ready-pose match against the BT2018 bind."""
    pygltflib = pytest.importorskip("pygltflib")
    import numpy as np
    from bt2018_retarget_clips import pose_error, quat_mul
    N = pygltflib.Node
    g = pygltflib.GLTF2(nodes=[N(name="j_Spine2", children=[1]), N(name="j_LClavicle", children=[2], translation=[0.1, 0, 0]),
                               N(name="j_LUpperArm", children=[3], translation=[0.1, 0, 0]), N(name="j_LHand", translation=[0.1, 0, 0])])
    par = {1: 0, 2: 1, 3: 2}
    ident = np.array([0.0, 0.0, 0.0, 1.0])
    bind = [ident] * 4
    assert pose_error(g, par, bind, bind)[0] == 0.0
    h = np.radians(60) / 2
    up = np.array([0.0, 0.0, np.sin(h), np.cos(h)])                      # arm lifted 60 deg about the shoulder
    assert pose_error(g, par, bind, [ident, ident, up, up])[0] > 1.0


def test_pack_builder_names_every_cooked_texture_a_package_uses():
    """Trigger: tools/bt2018_mechs/build_pack.py (a pack built without its .ktx2 files drew untextured mechs)."""
    from tools.bt2018_mechs import build_pack as B
    pkg = {"materials": {"albedo": {"ktx2": "a.ktx2"}, "ao": {"ktx2": "b.ktx2"}, "normal": {"ktx2": "c.ktx2"},
                         "emissive": {"ktx2": "ignored.ktx2"}, "other": {}}}
    assert B.package_ktx2(pkg) == ["a.ktx2", "b.ktx2", "c.ktx2"]
    assert B.package_ktx2({"materials": {"albedo": {}}}) == [] and B.package_ktx2({}) == []
