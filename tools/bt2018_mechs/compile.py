"""BT2018 recipe -> normalised MC2 mech profile -> compatibility files.

One authoritative profile per chassis (raw BT values beside every MC2 value), from
which the pack's runtime files are generated:

  data/objects/<stem>.csv                   chassis + variants, legacy 23+97n layout
  data/defs/chassis/<ns>/<family>/chassisdef.fit
                                            object type (ObjectClass/ObjectType/
                                            MechProfile) + package chassis registry
                                            blocks (Chassis/Region/Hardpoint/MountTags)
  data/tgl/<stem>.ini                       appearance: imported LOD0, host gesture
                                            timing, package weapon-node sockets
  data/defs/text/en_us/<ns>_mechs.fit       chassis name/description strings
  data/defs/equipment/<ns>/<item>/equipmentdef.fit
                                            pack-owned equipment actually referenced

The engine registry (code/package_chassis.h) and these emitters share one layout
contract; tests/unit/test_package_chassis.cpp and scripts/tests/test_bt2018_mechs.py
lock both ends.
"""
from __future__ import annotations

import copy
import fnmatch
import json
import math
from pathlib import Path

from . import btjson

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
RECIPES = HERE / "recipes"

STRING_ID_BASE = 61800          # Pack builder checks every allocated ID against installed text.
STRING_ID_LIMIT = 62000

# Legacy chassis CSV item rows (0-based item index) per BT location. Items 0..11 are the
# fixed internals every MC2 chassis carries; 12 is the sensor; 16..21 the leg/jump slots.
ITEM_RANGES = {
    "Head": (13, 15),
    "LeftArm": (22, 29),
    "RightArm": (30, 37),
    "LeftTorso": (38, 49),
    "RightTorso": (50, 61),
    "CenterTorso": (62, 71),
}
FIXED_ITEMS = [  # (master id, name) for items 0..12, as every stock chassis lists them
    (1, "Fusion Engine"), (22, "Gyroscope II"), (4, "Arm Actuator"), (4, "Arm Actuator"),
    (5, "Leg Actuator"), (5, "Leg Actuator"), (6, "Cockpit"), (35, "LifeSupport"),
    (33, "Shoulder"), (33, "Shoulder"), (34, "Hip"), (34, "Hip"), (13, "Standard Sensors"),
]
# ComponentPlacement grid (item index per Slot1..12, columns Head..RightLeg) shared by every
# stock chassis CSV. BattleMech::init reads it to give each item a body location; the sensor
# system is only created when item 12 is placed, so an empty grid leaves the mech blind.
STOCK_PLACEMENT = [
    [6, 7, 12, 13, 14, 15],
    [8, 2, 2, 2] + list(range(22, 30)),
    [9, 3, 3, 3] + list(range(30, 38)),
    list(range(38, 50)),
    list(range(50, 62)),
    [0, 1] + list(range(62, 72)),
    [10, 4, 4, 16, 18, 20],
    [11, 5, 5, 17, 19, 21],
]
EMPTY_MASTER = 255
JUMP_ITEM_FIRST = 16
CSV_COLS = 26
HARDPOINT_CSV_COLUMN = 41       # out-of-band token column read by code/package_chassis.h
AP_MOUNT = "AntiPersonnel"

# CSV columns for the eight body regions (BaseInternalStructure / BaseArmorPoints rows):
# Head, LeftArm, RightArm, LeftTorso, RightTorso, CenterTorso, LeftLeg, RightLeg, then rear
# LeftTorso, RightTorso, CenterTorso.
REAR_ORDER = ["LeftTorso", "RightTorso", "CenterTorso"]

FORM_TO_TAG = {"EnergyWeapon": "Energy", "BallisticWeapon": "Ballistic", "MissileWeapon": "Missile"}


class ProfileError(ValueError):
    pass


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def load_tables(weapon_map=None, balance=None):
    wm = load_json(weapon_map or DATA / "weapon_map_v1.json")
    bp = load_json(balance or DATA / "balance_profile_v1.json")
    return wm, bp


def resolve_item(bt_id: str, wm: dict) -> dict:
    """Mapping record for a BT ComponentDefID: exact key first, then wildcard keys."""
    entries = wm["entries"]
    if bt_id in entries:
        return dict(entries[bt_id], bt_id=bt_id)
    for key, rec in entries.items():
        if "*" in key and fnmatch.fnmatchcase(bt_id, key):
            return dict(rec, bt_id=bt_id)
    if "_" in bt_id and bt_id.rsplit("_", 1)[-1][:1].isdigit() and not bt_id.endswith("_0-STOCK"):
        return {"kind": "unsupported", "bt_id": bt_id,
                "reason": "manufacturer variant: the distinction from the STOCK item is not implemented"}
    return {"kind": "unsupported", "bt_id": bt_id, "reason": "no mapping entry"}


def component_stats(rec: dict, wm: dict) -> dict:
    """MC2 weight/heat/footprint/form for a mapped stock or pack item."""
    if rec["kind"] in ("stock", "jumpjet"):
        s = wm["stock_components"].get(str(rec["master_id"]))
        if not s:
            raise ProfileError(f"{rec['bt_id']}: master id {rec['master_id']} has no stock_components entry")
        return {"weight": s["weight"], "heat": s["heat"], "w": s["w"], "h": s["h"], "form": s["form"],
                "master_id": rec["master_id"]}
    if rec["kind"] == "pack":
        p = pack_definition(rec["equipment"], wm)
        return {"weight": p["tonnage"], "heat": p["heat"], "w": p["w"], "h": p["h"],
                "form": p["category"] + "Weapon", "master_id": p["ordinal"]}
    raise ProfileError(f"{rec['bt_id']}: kind {rec['kind']} has no MC2 component")


def pack_definition(equip_id: str, wm: dict) -> dict:
    p = wm["pack_equipment"].get(equip_id)
    if not p:
        raise ProfileError(f"pack equipment {equip_id} is not defined in the weapon map")
    base = wm["derive_rows"][str(p["derive_from"])]
    return {
        "id": equip_id, "ordinal": p["ordinal"], "display": p["display"], "kind": p["kind"],
        "description": p["description"],
        "category": p["category"], "w": p["w"], "h": p["h"], "crit": p["crit"], "cost": p["cost"],
        "range": p["range"], "ammo_master_id": p["ammo_master_id"],
        "missile_type": p.get("missile_type", ""),
        "damage": round(base["damage"] * p["damage_x"], 2),
        "heat": round(base["heat"] * p["heat_x"], 2),
        "tonnage": round(base["weight"] * p["weight_x"], 2),
        "recycle": base["recycle"],
        "cv": round(base["cv"] * p["damage_x"], 2),
        "special_effect": wm["derive_rows"][str(p["special_effect_from"])]["special_effect"],
        "icon": base["icon"],
        "derive_from": p["derive_from"],
        "multipliers": {k: p[k] for k in ("damage_x", "heat_x", "weight_x")},
    }


def speed_class(movedef: dict, bp: dict) -> dict:
    walk_mp = round(float(movedef["MaxWalkDistance"]) / bp["units"]["bt_metres_per_movement_point"])
    run_mp = math.ceil(1.5 * walk_mp)
    kmh = run_mp * bp["units"]["kmh_per_run_mp"]
    cls, disp = min(bp["speed"]["classes"], key=lambda c: (abs(c[1] - kmh), c[0]))
    return {"walk_mp": walk_mp, "run_mp": run_mp, "kmh": round(kmh, 1), "mc2_class": cls, "mc2_display_kmh": disp}


def tabletop(v, per):
    return -1 if v < 0 else v / per


def build_profile(recipe: dict, wm: dict, bp: dict) -> dict:
    cd = recipe["chassisdef"]
    mc2 = recipe["mc2"]
    locs = btjson.chassis_locations(cd)
    tons = float(cd["Tonnage"])
    per_a = bp["units"]["bt_armor_per_tabletop_point"]
    per_s = bp["units"]["bt_structure_per_tabletop_point"]
    report = []

    structure = {}
    for loc in btjson.LOCATIONS:
        v = round(tabletop(locs[loc]["structure"], per_s) * bp["structure"]["scale"])
        if not 0 < v <= bp["structure"]["byte_limit"]:
            raise ProfileError(f"{loc}: internal structure {v} outside 1..{bp['structure']['byte_limit']}")
        structure[loc] = v

    # Hardpoints: one id per BT mount, sockets assigned per location in order.
    hardpoints = []
    for loc in btjson.LOCATIONS:
        mounts = locs[loc]["mounts"]
        if mounts and loc not in mc2["regions"]:
            raise ProfileError(f"{loc} has weapon mounts but no mech-lab grid region")
        sockets = mc2["sockets"].get(loc, [])
        if mounts and not sockets:
            raise ProfileError(f"{loc} has weapon mounts but no weapon-node socket")
        counts = {}
        for i, m in enumerate(mounts):
            n = counts.get(m, 0)
            counts[m] = n + 1
            hardpoints.append({"id": f"{loc}.{m}.{n}", "location": loc, "mount": m,
                               "socket": sockets[i % len(sockets)]})
    validate_regions(mc2, report)

    spd = speed_class(recipe["movedef"], bp)
    variants = [build_variant(recipe, md, locs, hardpoints, wm, bp, report) for md in recipe["mechdefs"]]
    if not variants:
        raise ProfileError("recipe has no mechdefs")

    ref = variants[0]
    payload = round(sum(i["mc2"]["weight"] for i in ref["items"]) + math.floor(bp["payload"]["slack_per_ton"] * tons), 2)
    empty_mass = round(tons - payload, 2)
    for v in variants:
        used = round(sum(i["mc2"]["weight"] for i in v["items"]), 2)
        v["payload_used"] = used
        if used > payload:
            raise ProfileError(f"{v['name']}: equipment mass {used} exceeds payload {payload}")

    heat_index = bp["heat"]["base"] + bp["heat"]["per_sink"] * bp["heat"]["engine_sinks"]
    cost = round(cd["Description"]["Cost"] / bp["economy"]["cbill_divisor"])
    br = round(tons * bp["economy"]["br_per_ton"])

    front_total = sum(round(tabletop(a[0], per_a) * bp["armor"]["scale"]) for a in ref["armor_raw"].values())
    rear_total = sum(round(tabletop(a[1], per_a) * bp["armor"]["scale"]) for a in ref["armor_raw"].values() if a[1] >= 0)
    max_armor = round((front_total + rear_total) * bp["armor"]["max_armor_over_total"])

    ap_ordinals = sorted({wm["pack_equipment"][e]["ordinal"] for e, t in pack_tags(wm).items() if t == AP_MOUNT})
    profile = {
        "schema": "bt2018-mech-profile/1",
        "chassis_id": recipe["chassis_id"],
        "family": recipe["family"],
        "stem": mc2["stem"],
        "name": cd["Description"]["UIName"],
        "details": cd["Description"].get("Details", ""),
        "balance_profile": bp["id"],
        "provenance": recipe.get("provenance", {}),
        "raw": {
            "Tonnage": cd["Tonnage"], "InitialTonnage": cd.get("InitialTonnage"),
            "weightClass": cd.get("weightClass"), "Heatsinks": cd.get("Heatsinks"),
            "MaxJumpjets": cd.get("MaxJumpjets"), "Cost": cd["Description"]["Cost"],
            "Locations": locs, "movedef": recipe["movedef"],
        },
        "mc2": {
            "tonnage": tons, "structure": structure, "heat_index": heat_index, "speed": spd,
            "cost": cost, "br": br, "max_armor": max_armor, "payload": payload, "empty_mass": empty_mass,
            "jump_capable": int(cd.get("MaxJumpjets", 0)) > 0,
            "grid": mc2["grid"], "regions": mc2["regions"], "layout_rev": int(mc2.get("layout_rev", 1)),
            "ap_ordinals": ap_ordinals,
        },
        "hardpoints": hardpoints,
        "variants": variants,
        "report": report,
    }
    return profile


def pack_tags(wm):
    """Pack equipment id -> BT mount tag it was declared with (from the weapon map entries)."""
    out = {}
    for rec in wm["entries"].values():
        if rec.get("kind") == "pack":
            out[rec["equipment"]] = rec["tag"]
    return out


def validate_regions(mc2: dict, report: list):
    gw, gh = mc2["grid"]["width"], mc2["grid"]["height"]
    if not (1 <= gw <= 6 and 1 <= gh <= 6):
        raise ProfileError(f"grid {gw}x{gh} outside the mech lab's 6x6 limit")
    seen = set()
    for loc, (x, y, w, h) in mc2["regions"].items():
        btjson.canonical_location(loc)
        if x < 0 or y < 0 or x + w > gw or y + h > gh or w < 1 or h < 1:
            raise ProfileError(f"region {loc} {x},{y},{w},{h} outside the {gw}x{gh} grid")
        for i in range(x, x + w):
            for j in range(y, y + h):
                if (i, j) in seen:
                    raise ProfileError(f"region {loc} overlaps another region at {i},{j}")
                seen.add((i, j))


def build_variant(recipe, md, locs, hardpoints, wm, bp, report):
    # MC2 resolves salvage and recovery by variant name, so the pack name carries a suffix
    # wherever an installed chassis already uses the BT name (build_pack refuses collisions).
    name = md["Description"]["UIName"] + recipe["mc2"].get("variant_name_suffix", "")
    armor_raw = btjson.mechdef_armor(md)
    items, unsupported, ammo_raw = [], [], []
    used_mounts = set()
    ap_tags = pack_tags(wm)
    for inv in md.get("inventory", []):
        bt_id = inv["ComponentDefID"]
        loc = btjson.canonical_location(inv["MountedLocation"])
        rec = resolve_item(bt_id, wm)
        if rec["kind"] == "ammo":
            ammo_raw.append({"bt_id": bt_id, "location": loc})
            continue
        if rec["kind"] == "unsupported":
            unsupported.append({"bt_id": bt_id, "location": loc, "reason": rec["reason"]})
            continue
        stats = component_stats(rec, wm)
        item = {"bt_id": bt_id, "kind": rec["kind"], "equipment": rec.get("equipment"),
                "location": loc, "mc2": stats, "hardpoint": None}
        if rec["kind"] == "jumpjet":
            item["location"] = "Legs"
        elif stats["form"] in FORM_TO_TAG:
            tag = rec["tag"]
            hp = pick_mount(hardpoints, loc, tag, used_mounts, int(inv.get("HardpointSlot", 0)))
            if hp is None and tag == AP_MOUNT:
                # BT lets anti-personnel weapons use their own category's mount as well.
                tag = FORM_TO_TAG[stats["form"]]
                hp = pick_mount(hardpoints, loc, tag, used_mounts, 0)
            if hp is None:
                raise ProfileError(f"{name}: {bt_id} in {loc} has no free {tag} mount")
            used_mounts.add(hp["id"])
            item["hardpoint"] = hp["id"]
            item["tag"] = tag
        items.append(item)
    for u in unsupported:
        report.append(f"{name}: UNSUPPORTED {u['bt_id']} ({u['location']}): {u['reason']} -- not installed")
    variant = {"name": name, "id": md["Description"]["Id"], "raw_cost": md["Description"].get("Cost"),
               "armor_raw": armor_raw, "items": items, "unsupported": unsupported, "ammo_raw": ammo_raw}
    variant["armor"] = convert_armor(armor_raw, bp)
    place_items(recipe["mc2"], variant)
    return variant


def pick_mount(hardpoints, loc, tag, used, preferred):
    """Free mount in loc accepting tag. AntiPersonnel items may also take their category's mount."""
    cands = [h for h in hardpoints if h["location"] == loc and h["id"] not in used]
    exact = [h for h in cands if h["mount"] == tag]
    if exact:
        return exact[min(preferred, len(exact) - 1)] if preferred >= 0 else exact[0]
    return None


def convert_armor(armor_raw, bp):
    per = bp["units"]["bt_armor_per_tabletop_point"]
    scale = bp["armor"]["scale"]
    out = {}
    for loc in btjson.LOCATIONS:
        f, r = armor_raw.get(loc, (0, -1))
        out[loc] = round(tabletop(f, per) * scale)
        if loc in REAR_ORDER:
            out["Rear" + loc] = 0 if r < 0 else round(tabletop(r, per) * scale)
    for k, v in out.items():
        if not 0 <= v <= bp["armor"]["byte_limit"]:
            raise ProfileError(f"armour {k}={v} outside the byte range")
    return out


def place_items(mc2, variant):
    """Assign mech-lab grid cells: each grid item goes wholly inside its location's region."""
    occupied = {}
    order = sorted([i for i in variant["items"] if i["location"] in mc2["regions"]],
                   key=lambda i: (-i["mc2"]["h"], -i["mc2"]["w"]))
    for item in order:
        x0, y0, rw, rh = mc2["regions"][item["location"]]
        w, h = item["mc2"]["w"], item["mc2"]["h"]
        spot = None
        for y in range(y0, y0 + rh - h + 1):
            for x in range(x0, x0 + rw - w + 1):
                cells = [(x + i, y + j) for i in range(w) for j in range(h)]
                if all(c not in occupied for c in cells):
                    spot = (x, y, cells)
                    break
            if spot:
                break
        if not spot:
            raise ProfileError(f"{variant['name']}: {item['bt_id']} ({w}x{h}) does not fit the {item['location']} region")
        item["grid"] = [spot[0], spot[1]]
        for c in spot[2]:
            occupied[c] = item["bt_id"]
    for item in variant["items"]:
        if item["location"] not in mc2["regions"] and item["location"] != "Legs":
            raise ProfileError(f"{variant['name']}: {item['bt_id']} is in {item['location']}, which has no grid region")


# ----------------------------------------------------------------------------- emitters


def string_ids(profile, index):
    """Legacy string ids for the index-th chassis of the pack (name, description).
    Assigned by position in the sorted recipe list; saves never store these ids."""
    base = STRING_ID_BASE + 2 * index
    if base + 1 >= STRING_ID_LIMIT:
        raise ProfileError(f"pack string range {STRING_ID_BASE}..{STRING_ID_LIMIT - 1} exhausted")
    return {"name": base, "details": base + 1}


def _row(cells):
    cells = [str(c) for c in cells]
    return ",".join(cells + [""] * max(0, CSV_COLS - len(cells)))


def emit_csv(profile, recipe, index):
    m = profile["mc2"]
    ui = recipe["mc2"]["ui"]
    sid = string_ids(profile, index)
    s = m["structure"]
    rows = [
        _row(["BaseChassisInfo"]),
        _row(["Identification", "", "", "ChasisStats", "", "", "AllowComponents", "", "", "Generated by tools/bt2018_mechs (do not hand-edit)"]),
        _row(["MechName", profile["name"], "", "Tonnage", int(m["tonnage"]), "", "Advanced Sensor", "No"]),
        _row(["HouseID", recipe["mc2"].get("house_id", 0), "", "Heat Index", m["heat_index"], "", "Optics", "No"]),
        _row(["DescIndex", sid["name"], "", "Load Index", int(round(m["payload"])), "", "GlobalSensor", "No"]),
        _row(["AbbrIndex", sid["details"], "", "Total Armor", sum(profile["variants"][0]["armor"].values()), "", "ECM", "No"]),
        _row(["EncyclopediaID", -1, "", "MaxRunSpeed", m["speed"]["mc2_class"], "", "ActiveProbe", "No"]),
        _row(["HelpId", -1, "", "TorsoYawRate", 360, "", "Jump Jets", "Yes" if m["jump_capable"] else "No"]),
        _row(["LittleIcon", ui["host_little_icon"], "", "MaxTorsoYaw", 120, "", "Extended Sensor", "No"]),
        _row(["Animation", profile["stem"] + "walk", "", "ChassisBR", m["br"]]),
        _row(["BigIcon", ui["host_big_icon"], "", "ChassisC-Bills", m["cost"]]),
        _row(["MechParts", ui["host_icon_index"], "", "MAX Armor", m["max_armor"]]),
        _row(["LogisticsX", m["grid"]["width"]]),
        _row(["LogisticsY", m["grid"]["height"]]),
        _row([]),
        _row(["Base Armor Values", "Head", "LeftArm", "RightArm", "LeftTorso", "RightTorso", "CenterTorso",
              "LeftLeg", "RightLeg", "RearLeftTorso", "RearRightTorso", "RearCenterTorso"]),
        _row(["BaseInternalStructure"] + [s[l] for l in btjson.LOCATIONS]),
        _row(["BaseArmorPoints"] + armor_cells(profile["variants"][0]["armor"])),
        _row([]),
    ]
    for v in profile["variants"]:
        rows += variant_rows(profile, v)
    assert all(len(r.split(",")) >= CSV_COLS for r in rows)
    return "\n".join(rows) + "\n"


def armor_cells(a):
    return [a[l] for l in btjson.LOCATIONS] + [a["Rear" + l] for l in REAR_ORDER]


def variant_rows(profile, v):
    items = [(mid, name, -1, -1, None) for mid, name in FIXED_ITEMS] + [(EMPTY_MASTER, "EMPTY!", -1, -1, None)] * (72 - len(FIXED_ITEMS))
    items = [list(i) for i in items]
    cursor = {loc: lo for loc, (lo, hi) in ITEM_RANGES.items()}
    jump = JUMP_ITEM_FIRST
    for it in sorted(v["items"], key=lambda i: (i["location"], i.get("hardpoint") or "~", i["bt_id"])):
        mid = it["mc2"]["master_id"]
        label = it["equipment"] or it["bt_id"]
        if it["location"] == "Legs":
            items[jump] = [mid, label, -2, -2, None]
            jump += 1
            continue
        lo, hi = ITEM_RANGES[it["location"]]
        k = cursor[it["location"]]
        if k > hi:
            raise ProfileError(f"{v['name']}: too many items in {it['location']} for the legacy CSV rows")
        x, y = it.get("grid", (-1, -1))
        items[k] = [mid, label, x, y, it.get("hardpoint")]
        cursor[it["location"]] = k + 1
    rows = [
        _row(["VariantInfo"]),
        _row(["Identification"]),
        _row(["VariantID", f"{profile['variants'].index(v):04d}"]),
        _row(["VariantName", v["name"]]),
        _row([]),
        _row(["MasterComponentID", "Load#", "MechBay X", "MechBay Y", "CompBas#", "Name"]),
    ]
    for i, (mid, name, x, y, hp) in enumerate(items):
        cells = [f"Item{i}MasterId", 0, x, y, mid, name]
        if hp:
            cells += [""] * (HARDPOINT_CSV_COLUMN - 1 - len(cells)) + [hp]   # CSVFile columns are 1-based
        rows.append(_row(cells))
    rows += [
        _row([]),
        _row(["Totals"]),
        _row([]),
        _row(["ComponentPlacement", "Head", "LeftArm", "RightArm", "LeftTorso", "RightTorso", "CenterTorso", "LeftLeg", "RightLeg"]),
    ]
    rows += [_row([f"Slot{n + 1}"] + [col[n] if n < len(col) else "" for col in STOCK_PLACEMENT])
             for n in range(12)]
    rows.append(_row(["CurrentInternalStructure"] + [profile["mc2"]["structure"][l] for l in btjson.LOCATIONS]))
    rows.append(_row(["CurrentArmorPoints"] + armor_cells(v["armor"])))
    rows.append(_row([]))
    assert len(rows) == 97, len(rows)
    return rows


def emit_chassis_fit(profile, recipe):
    m = profile["mc2"]
    stem = profile["stem"]
    lines = ["FITini", "",
             f"// {profile['chassis_id']} -- generated by tools/bt2018_mechs/compile.py (do not hand-edit).",
             f"// balance profile {profile['balance_profile']}; raw BT values in content/{profile['family']}.profile.json",
             "",
             "[ObjectClass]", "l ObjectTypeNum = 2", "",
             "[ObjectType]", f'st Name = "{stem}"', "l ExplosionObject = 41", "l DestroyedObject = 101",
             "f ExtentRadius = 12", f'st AppearanceName = "{stem}"', "",
             "[MechProfile]", f'st ProfileName = "{stem}"', "f destructdamage = 55",
             "f explosiondamage = 25", "f explosionradius = 80", "",
             "[Chassis]", f'st ChassisId = "{profile["chassis_id"]}"', f'st Stem = "{stem}"',
             f'st BalanceProfile = "{profile["balance_profile"]}"', "f Scale = 1.0",
             f"f Tonnage = {m['tonnage']}", f"f EmptyMass = {m['empty_mass']}", f"f Payload = {m['payload']}",
             "b Purchasable = TRUE", f"l LayoutRev = {m['layout_rev']}",   # saves/exports of a revised layout are fenced from older builds
             f'st HudIcon = "{stem}_hud"',
             f"l RegionCount = {len(m['regions'])}", f"l HardpointCount = {len(profile['hardpoints'])}", ""]
    for i, (loc, (x, y, w, h)) in enumerate(m["regions"].items()):
        lines += [f"[Region{i}]", f'st Location = "{loc}"', f"l X = {x}", f"l Y = {y}", f"l W = {w}", f"l H = {h}", ""]
    for i, hp in enumerate(profile["hardpoints"]):
        lines += [f"[Hardpoint{i}]", f'st Id = "{hp["id"]}"', f'st Location = "{hp["location"]}"',
                  f'st Mount = "{hp["mount"]}"', f'st Socket = "{hp["socket"]}"', ""]
    ap = ",".join(str(o) for o in m["ap_ordinals"])
    lines += ["[MountTags]", f'st AntiPersonnel = "{ap}"', "", "FITend", ""]
    return "\n".join(lines)


def emit_strings(profiles, ns="bt2018"):
    lines = ["// generated by tools/bt2018_mechs/compile.py (do not hand-edit)", ""]
    for index, p in enumerate(profiles):
        sid = string_ids(p, index)
        for key, text, lid in ((f"{ns}.{p['family']}.name", p["name"], sid["name"]),
                               (f"{ns}.{p['family']}.details", p["details"], sid["details"])):
            safe = text.replace("\\", "\\\\").replace('"', "'")
            lines += ["String {", f'    key = "{key}"', f'    text = "{safe}"', f"    legacyId = {lid}",
                      f'    source = "{ns}_mechs"', '    category = "units"', "}", ""]
    return "\n".join(lines)


def emit_ini(profile, recipe, host_ini_text):
    """Appearance INI: imported LOD0 from the pack, host gesture timing + node blocks."""
    blocks = parse_fit_blocks(host_ini_text)
    keep = [b for b in blocks if b[0].startswith("Gestures") or b[0] in
            ("Bounds", "SmokeNode0", "JumpJetNode0", "FootNode0", "FootNode1", "FootPrint")]
    sockets = []
    for loc in btjson.LOCATIONS:
        for s in recipe["mc2"]["sockets"].get(loc, []):
            if s not in sockets:
                sockets.append(s)
    host = recipe["mc2"]["model"]["host_ini"]
    speed_scale = profile["mc2"]["speed"]["mc2_class"] / 23.0
    out = ["FITini", "",
           f"// {profile['chassis_id']} appearance -- generated by tools/bt2018_mechs/compile.py (do not hand-edit).",
           f"// Gesture timing and effect nodes from the host '{host}' INI; velocities scaled by {speed_scale:.3f}",
           "// to this chassis' speed class. No host LOD, shadow or arm meshes are referenced.", "",
           "[Import]", f'st Source = "{recipe["mc2"]["model"]["pack_path"]}"', f'st AnimHost = "{host}"', "",
           "[TGLData]", f'st FileName0 = "{profile["stem"]}"', "f Distance0 = 0.0", "",
           "[Nodes]", "l NumSmoke = 1", "l NumJumpJet = 1", f"l NumWeapon = {len(sockets)}", "l NumFeet = 2", ""]
    for i, s in enumerate(sockets):
        out += [f"[WeaponNode{i}]", f'st WeaponNodeName = "{s}"', "l WeaponType = 0", ""]
    for name, body in keep:
        if name.startswith("Gestures"):
            body = [scale_velocity(l, speed_scale) for l in body]
        out += [f"[{name}]"] + body + [""]
    out += ["FITend", ""]
    return "\n".join(out)


def scale_velocity(line, k):
    s = line.strip()
    for key in ("StartVel", "EndVel"):
        if s.replace(" ", "").startswith(f"f{key}="):
            v = float(s.split("=", 1)[1])
            return f"f {key}={v * k:.6f}"
    return line


def parse_fit_blocks(text):
    blocks, cur, body = [], None, []
    for raw in text.splitlines():
        line = raw.rstrip()
        st = line.strip()
        if st.startswith("[") and st.endswith("]"):
            if cur is not None:
                blocks.append((cur, body))
            cur, body = st[1:-1], []
        elif cur is not None and st and not st.startswith("//") and st not in ("FITend", "FITEnd"):
            body.append(st)
    if cur is not None:
        blocks.append((cur, body))
    return blocks


def emit_package_json(base_package: dict, recipe, socket_offsets: dict | None = None) -> dict:
    """Model package sidecar: the converted model's node manifest plus every socket this chassis names."""
    pkg = copy.deepcopy(base_package)
    nodes = pkg.setdefault("nodes", {})
    for loc, sockets in recipe["mc2"]["sockets"].items():
        for s in sockets:
            if s not in nodes:
                base = s.rstrip("0123456789")
                if base in nodes:
                    nodes[s] = nodes[base]
                else:
                    raise ProfileError(f"socket {s} has no joint in the model package node manifest")
    pkg["chassisId"] = recipe["chassis_id"]
    pkg["clips"] = {"showcase": recipe["mc2"].get("showcase_clip", "atlas_moveCoreIdle")}
    if socket_offsets:
        # P10: muzzle points in each socket joint's local frame (sockets.py); "x y z" strings keep the
        # engine's flat-map reader.
        pkg["socketOffsets"] = {k: " ".join(f"{c:.4f}" for c in v) for k, v in sorted(socket_offsets.items())}
    return pkg


def emit_equipment_def(d: dict) -> str:
    rng = {"short": "short", "medium": "medium", "long": "long"}[d["range"]]
    lines = ["FITini", "",
             f"// {d['id']} -- pack-owned definition generated by tools/bt2018_mechs/compile.py.",
             f"// Derived from stock compbas row {d['derive_from']} x {d['multipliers']}; ordinal is explicit.",
             "", "[Equipment]", f'st EquipmentId="{d["id"]}"', f'st DisplayName="{d["display"]}"',
             f'st Description="{d["description"]}"',
             f'st Kind="{d["kind"]}"', f'st Tags="{d["category"].lower()}"', f"l LegacyMasterId={d['ordinal']}",
             "b Enabled=TRUE", f"f Heat={d['heat']}", f"f Cost={float(d['cost'])}", "",
             "[WeaponFacet]", f'st Category="{d["category"]}"', "",
             "[WeaponCombatStats]", f"f Damage={d['damage']}", f"f Heat={d['heat']}", f"f Recycle={d['recycle']}",
             f'st RangeBucket="{rng}"', f"f CombatValue={d['cv']}", "",
             "[WeaponAmmo]", f"l AmmoMasterId={d['ammo_master_id']}"]
    if d["missile_type"]:
        lines.append(f'st MissileType="{d["missile_type"]}"')
    lines += ["", "[WeaponFit]", f"l CritSlots={d['crit']}", f"f Tonnage={d['tonnage']}",
              'st AllowedLocations="CenterTorso,LeftTorso,RightTorso,LeftArm,RightArm"', "",
              "[Presentation]", f'st Icon="{d["icon"]}"', f"l SpecialEffect={d['special_effect']}",
              f"l IconX={d['w']}", f"l IconY={d['h']}", "", "FITend", ""]
    return "\n".join(lines)


def used_pack_equipment(profiles, wm):
    ids = sorted({i["equipment"] for p in profiles for v in p["variants"] for i in v["items"] if i["kind"] == "pack"})
    return [pack_definition(e, wm) for e in ids]
