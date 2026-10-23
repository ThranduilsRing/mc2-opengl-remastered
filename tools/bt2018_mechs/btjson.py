"""Tolerant reader for BattleTech 2018 TextAsset JSON.

The dump is hand-edited game data: some files carry trailing commas or a missing
delimiter between objects (plan section 2: 19 strict-parse failures). The reader
normalises only those two forms, records every repair, and never edits the source
file. Anything else that fails to parse is an error the caller reports.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

_TRAILING_COMMA = re.compile(r",(\s*[}\]])")
_MISSING_DELIM = re.compile(r"([}\]\"0-9el])(\s*\n\s*)([{\"])")


@dataclass
class Loaded:
    data: object
    repairs: list = field(default_factory=list)


def loads_tolerant(text: str, source: str = "<text>") -> Loaded:
    try:
        return Loaded(json.loads(text))
    except json.JSONDecodeError:
        pass
    repairs = []
    fixed, n = _TRAILING_COMMA.subn(r"\1", text)
    if n:
        repairs.append(f"{source}: removed {n} trailing comma(s)")
    try:
        return Loaded(json.loads(fixed), repairs)
    except json.JSONDecodeError:
        pass
    fixed2, n2 = _MISSING_DELIM.subn(r"\1,\2\3", fixed)
    if n2:
        repairs.append(f"{source}: inserted {n2} missing delimiter(s)")
    try:
        return Loaded(json.loads(fixed2), repairs)
    except json.JSONDecodeError as e:
        raise ValueError(f"{source}: unparseable after normalisation: {e}") from e


def load_tolerant(path) -> Loaded:
    p = Path(path)
    return loads_tolerant(p.read_text(encoding="utf-8-sig"), str(p.name))


# BT location names, in the order MC2's eight body regions are listed in a chassis CSV.
LOCATIONS = ["Head", "LeftArm", "RightArm", "LeftTorso", "RightTorso", "CenterTorso", "LeftLeg", "RightLeg"]
REAR_LOCATIONS = {"LeftTorso": "RearLeftTorso", "RightTorso": "RearRightTorso", "CenterTorso": "RearCenterTorso"}
MOUNT_TYPES = ("Energy", "Ballistic", "Missile", "AntiPersonnel")


def canonical_location(name: str) -> str:
    for loc in LOCATIONS:
        if loc.lower() == str(name).replace(" ", "").lower():
            return loc
    raise ValueError(f"unknown BT location {name!r}")


def chassis_locations(chassisdef: dict) -> dict:
    """Location -> {mounts[], slots, max_armor, max_rear, structure} from a ChassisDef."""
    out = {}
    for rec in chassisdef.get("Locations", []):
        loc = canonical_location(rec["Location"])
        mounts = []
        for hp in rec.get("Hardpoints", []):
            m = hp.get("WeaponMount")
            if m not in MOUNT_TYPES:
                raise ValueError(f"{loc}: unknown WeaponMount {m!r}")
            mounts.append(m)
        out[loc] = {
            "mounts": mounts,
            "slots": int(rec.get("InventorySlots", 0)),
            "max_armor": int(rec.get("MaxArmor", 0)),
            "max_rear": int(rec.get("MaxRearArmor", -1)),
            "structure": int(rec.get("InternalStructure", 0)),
        }
    missing = [l for l in LOCATIONS if l not in out]
    if missing:
        raise ValueError(f"chassisdef missing locations {missing}")
    return out


def mechdef_armor(mechdef: dict) -> dict:
    """Location -> (front, rear) assigned armour; rear is -1 where inapplicable."""
    out = {}
    for rec in mechdef.get("Locations", []):
        loc = canonical_location(rec["Location"])
        out[loc] = (int(rec.get("AssignedArmor", rec.get("CurrentArmor", 0))),
                    int(rec.get("AssignedRearArmor", rec.get("CurrentRearArmor", -1))))
    return out
