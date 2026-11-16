"""Generate the authored MC2-fit roster through the existing recipe compiler."""
from __future__ import annotations

from . import btjson, compile as C

LOC = {"LA": "LeftArm", "RA": "RightArm", "LT": "LeftTorso", "RT": "RightTorso", "CT": "CenterTorso"}
WEAPONS = dict(zip(
    "ML SL LL PPC AC2 AC5 AC10 AC20 GAUSS LRM5 LRM10 LRM15 LRM20 SRM2 SRM4 SRM6 MG FL".split(),
    ("Laser_MediumLaser", "Laser_SmallLaser", "Laser_LargeLaser", "PPC_PPC", "Autocannon_AC2",
     "Autocannon_AC5", "Autocannon_AC10", "Autocannon_AC20", "Gauss_Gauss", "LRM_LRM5", "LRM_LRM10",
     "LRM_LRM15", "LRM_LRM20", "SRM_SRM2", "SRM_SRM4", "SRM_SRM6", "MachineGun_MachineGun", "Flamer_Flamer")))


def recipes(conversions):
    rows = C.load_json(C.DATA / "roster.json")
    manifest = C.load_json(conversions / "conversions_manifest.json")
    families = sorted({r["btname"].split("_")[0] for r in manifest})
    missing = set(families) - rows.keys()
    if missing:
        raise C.ProfileError(f"converted families need roster rows: {sorted(missing)}")
    result = []
    for family in families:
        reviewed = C.RECIPES / f"{family}.json"
        if reviewed.is_file():
            result.append(C.load_json(reviewed))
            continue
        tons, walk, loadout = rows[family]
        name = family.title()
        desc = f"{name}: pack-authored MC2 adaptation ({tons} tonnes). Not a canonical BT2018 loadout."
        chassis = {"Description": {"Cost": tons * 80000, "UIName": name, "Details": desc},
                   "Tonnage": tons, "MaxJumpjets": 0, "Locations": []}
        inventory, armor = [], []
        for loc in btjson.LOCATIONS:
            structure = 15 if loc == "Head" else tons * (0.8 if "Arm" in loc else 1.2)
            front = 45 if loc == "Head" else tons * (1.5 if "Arm" in loc else 2)
            rear = tons * 0.4 if "Torso" in loc else -1
            chassis["Locations"].append({"Location": loc, "Hardpoints": [], "InventorySlots": 12,
                                         "InternalStructure": structure, "MaxArmor": front, "MaxRearArmor": rear})
            armor.append({"Location": loc, "AssignedArmor": front, "AssignedRearArmor": rear})
        wm, _ = C.load_tables()
        for group in loadout.split():
            region, weapons = group.split(":")
            loc = LOC[region]
            for alias in weapons.split(","):
                weapon = "Weapon_" + WEAPONS[alias] + "_0-STOCK"
                mount = C.resolve_item(weapon, wm)["tag"]
                next(r for r in chassis["Locations"] if r["Location"] == loc)["Hardpoints"].append({"WeaponMount": mount})
                inventory.append({"MountedLocation": loc, "ComponentDefID": weapon})
        source = next(r for r in manifest if r["btname"] == family)
        result.append({"schema": "bt2018-mech-recipe/1", "family": family, "chassis_id": f"bt2018:{family}",
                       "provenance": {"gameplay": desc, "model": f"MC2Conversions/{source['glb']}"},
                       "chassisdef": chassis, "movedef": {"MaxWalkDistance": walk * 30},
                       "mechdefs": [{"Description": {"UIName": name + " MC2 Adaptation", "Id": f"bt2018_{family}"},
                                     "Locations": armor, "inventory": inventory}],
                       "mc2": {"stem": f"bt2018_{family}", "grid": {"width": 6, "height": 6},
                               "regions": {"LeftArm": [0, 0, 2, 4], "RightArm": [4, 0, 2, 4],
                                           "LeftTorso": [0, 4, 2, 2], "RightTorso": [4, 4, 2, 2], "CenterTorso": [2, 0, 2, 6]},
                               "sockets": {loc: ["weapon_" + loc.lower()] for loc in LOC.values()},
                               "model": {"source_glb": source["glb"], "package_json": f"{family}/bt2018_mech_package.json",
                                         "textures": [f"{family}/{source['tga']}"], "pack_path": f"bt2018/{family}", "host_ini": "madcat"},
                               "ui": {"host_icon_index": 13, "host_little_icon": f"bt2018_{family}",
                                      "host_big_icon": f"bt2018_{family}_bay"}}})
    return result
