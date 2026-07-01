# tools/unit_converter/equipment_extract.py
"""LEGACY-COMPBAS-CONVERTER-1 (equipment arc, slice 4) — extraction: stock
compbas.csv rows -> EquipmentDefinition list + waivers.

STOCK-ONLY: reads base mc2srcdata/objects/compbas.csv rows ONLY. No engine, no
RNG, no writes. Deterministic (file order).

Column layout (0-indexed) is fixed by the stock compbas.csv header and the
MasterComponent::saveEXCEL writer (mclib/cmponent.cpp):

  0  Component Table (row ordinal = masterID = legacyMasterId ALIAS)
  1  Type            (component FORM string -> EquipmentKind)
  2  Name
  3  crit hits       (InventorySize -> WeaponFitFacet.crit_slots)
  4  Recycle         (weapon refire delay; "#shots" for ammo)
  5  Heat
  6  Weight          (tonnage)
  7  Damage
  8  BR              (combat value)
  9  RP              (resource points / cost)
  10 Range           ("short"/"medium"/"long"/"0"/"No"...)
  11-18 Location Head/CT/LT/RT/LA/RA/LL/RL  ("Yes"/"No"/count)
  19 Missile type    (0=Energy,1=Ballistic,LRM,ST,SRM)
  20 Fields          (1=streak,2=inferno,4=LBX,8=Artillery)
  21 Special FX ID
  22 Ammo Master ID
  23 Logistics Icon 1
  25 String Index    (localized name string id)

FORM classification uses the `Type` column matched against ComponentFormString
(cmponent.cpp:25); the CSV `Type` strings are the exact form tokens
("EnergyWeapon","BallisticWeapon","MissileWeapon","Ammo","Sensors","Actuator",
"Engine","Cockpit","HeatSink","JumpJet","Gyroscope","Bulk","LifeSupport","ECM",
"Probe"). Non-canonical stock tokens seen in the data ("Optics","Removed") and
the sentinel "undefined" are handled explicitly.
"""
from __future__ import annotations

import csv
from typing import List, Optional, Tuple

from . import equipment_model as EM
from . import qualified_id

# -- column indices --
COL_ID = 0
COL_TYPE = 1
COL_NAME = 2
COL_CRITS = 3
COL_RECYCLE = 4
COL_HEAT = 5
COL_WEIGHT = 6
COL_DAMAGE = 7
COL_BR = 8
COL_RP = 9
COL_RANGE = 10
COL_LOC_FIRST = 11          # Head..RL = 11..18 (8 body locations)
COL_LOC_LAST = 18
COL_MISSILE_TYPE = 19
COL_FIELDS = 20
COL_SPECIAL_FX = 21
COL_AMMO_MASTER = 22
COL_ICON1 = 23
COL_STRING_INDEX = 25

BODY_LOCATION_NAMES = [
    "Head", "CenterTorso", "LeftTorso", "RightTorso",
    "LeftArm", "RightArm", "LeftLeg", "RightLeg",
]

# Fields bitmask (col 20): 1=streak, 2=inferno, 4=LBX, 8=Artillery.
FIELD_STREAK = 1
FIELD_INFERNO = 2
FIELD_LBX = 4
FIELD_ARTILLERY = 8

# `Type` string (compbas col 1) -> EquipmentKind. Mirror of ComponentFormString
# grouping in cmponent.cpp. Weapon forms are further split by category later.
TYPE_TO_KIND = {
    "EnergyWeapon": EM.EK_WEAPON,
    "BallisticWeapon": EM.EK_WEAPON,
    "MissileWeapon": EM.EK_WEAPON,
    "Weapon": EM.EK_WEAPON,          # generic ANY weapon form
    "Ammo": EM.EK_AMMO,
    "Sensors": EM.EK_SENSOR,
    "Optics": EM.EK_SENSOR,          # non-canonical stock token; sensor family
    "Probe": EM.EK_SENSOR,           # active probe -> sensor family
    "ECM": EM.EK_ECM,
    "Jammer": EM.EK_ECM,
    "JumpJet": EM.EK_JUMPJET,
    "HeatSink": EM.EK_HEATSINK,
    "Bulk": EM.EK_STRUCTURE,         # armor plating / internal structure
    "Case": EM.EK_STRUCTURE,
    "Cockpit": EM.EK_COCKPIT,
    "LifeSupport": EM.EK_COCKPIT,    # life support -> cockpit family
    "Actuator": EM.EK_ACTUATOR,
    "Gyroscope": EM.EK_ACTUATOR,     # gyro is a chassis actuator-class part
    "Engine": EM.EK_ENGINE,
    "PowerAmplifier": EM.EK_OTHER,
    "Simple": EM.EK_OTHER,
}


def _cell(row: List[str], idx: int, default: str = "") -> str:
    return row[idx].strip() if idx < len(row) else default


def _fnum(row: List[str], idx: int) -> float:
    raw = _cell(row, idx)
    try:
        return float(raw)
    except ValueError:
        return 0.0


def _inum(row: List[str], idx: int) -> int:
    raw = _cell(row, idx)
    try:
        return int(raw)
    except ValueError:
        try:
            return int(float(raw))
        except ValueError:
            return 0


def _slug(name: str, master_id: int) -> str:
    """Slugify a component Name into the `<slug>` portion of core:<slug>.

    Deterministic. Lowercase, non-alnum -> '_', collapse, strip. Names starting
    with a digit or empty get a stable `comp<masterID>` fallback so the id always
    parses under the qualified-id charset (must start [a-z])."""
    import re
    cleaned = (name or "").strip().lower().replace("'", "")
    s = re.sub(r"[^a-z0-9]+", "_", cleaned).strip("_")
    if not s or not s[0].isalpha():
        s = f"comp{master_id}" if not s else f"c_{s}"
    return s


def _weapon_category(type_str: str, missile_col: str) -> str:
    t = type_str.lower()
    if "energy" in t:
        return EM.WT_ENERGY
    if "ballistic" in t:
        return EM.WT_BALLISTIC
    if "missile" in t:
        return EM.WT_MISSILE
    # generic "Weapon": fall back on the missile-type column
    mc = (missile_col or "").strip().upper()
    if mc in ("LRM", "SRM", "ST"):
        return EM.WT_MISSILE
    if mc == "1":
        return EM.WT_BALLISTIC
    return EM.WT_ANY


def _range_bucket(row: List[str]) -> str:
    raw = _cell(row, COL_RANGE).lower()
    if raw in ("short", "medium", "long"):
        return raw
    return ""


def _fire_executor(category: str, missile_col: str, fields: int) -> str:
    """Best-effort default fireExecutor from FORM + missile-type + fields.

    MAPPING (documented; a starting default, refinable by a later slice/author):
      energy weapon                 -> beam_direct
      ballistic weapon              -> projectile_ballistic
      ballistic + Artillery field   -> artillery_indirect
      ballistic + LBX field         -> lbx_spread
      missile, streak (ST)          -> missile_streak
      missile (LRM/SRM/other)       -> missile_salvo
      any/undetermined weapon       -> projectile_ballistic (safe default)
    """
    mc = (missile_col or "").strip().upper()
    if category == EM.WT_ENERGY:
        return EM.FX_BEAM_DIRECT
    if category == EM.WT_MISSILE:
        if mc == "ST" or (fields & FIELD_STREAK):
            return EM.FX_MISSILE_STREAK
        return EM.FX_MISSILE_SALVO
    if category == EM.WT_BALLISTIC:
        if fields & FIELD_ARTILLERY:
            return EM.FX_ARTILLERY_INDIRECT
        if fields & FIELD_LBX:
            return EM.FX_LBX_SPREAD
        return EM.FX_PROJECTILE_BALLISTIC
    # WT_ANY / undetermined weapon
    return EM.FX_PROJECTILE_BALLISTIC


def _allowed_locations(row: List[str]) -> List[str]:
    """Body locations where the component may mount (compbas per-location cols).
    "No" => not allowed; "Yes" or a crit count => allowed."""
    out: List[str] = []
    for i, name in enumerate(BODY_LOCATION_NAMES):
        cell = _cell(row, COL_LOC_FIRST + i)
        if cell and cell.lower() != "no":
            out.append(name)
    return out


def _tags_for(kind: str, category: str, missile_col: str, fields: int) -> List[str]:
    tags: List[str] = []
    if kind == EM.EK_WEAPON:
        if category == EM.WT_ENERGY:
            tags.append("energy")
        elif category == EM.WT_BALLISTIC:
            tags.append("ballistic")
        elif category == EM.WT_MISSILE:
            tags.append("missile")
            mc = (missile_col or "").strip().upper()
            if mc == "LRM":
                tags.append("lrm")
            elif mc == "SRM":
                tags.append("srm")
            elif mc == "ST":
                tags.append("streak")
        if fields & FIELD_STREAK and "streak" not in tags:
            tags.append("streak")
        if fields & FIELD_INFERNO:
            tags.append("inferno")
        if fields & FIELD_LBX:
            tags.append("lbx")
        if fields & FIELD_ARTILLERY:
            tags.append("artillery")
    return tags


def extract_equipment(row: List[str]) -> Tuple[Optional[EM.EquipmentDefinition], Optional[str]]:
    """Convert ONE compbas row -> (EquipmentDefinition, None) or (None, waiver).

    A waiver string is returned (and the def dropped) for rows that are not
    convertible: malformed ordinal, the `undefined` sentinel, or the `Removed`
    placeholder (a dead compbas slot with no live component).
    """
    if not row:
        return None, "empty row"
    # row ordinal = masterID = legacyMasterId ALIAS
    try:
        master_id = int(_cell(row, COL_ID))
    except ValueError:
        return None, f"malformed masterID cell: {_cell(row, COL_ID)!r}"

    type_str = _cell(row, COL_TYPE)
    name = _cell(row, COL_NAME)

    if type_str == "" or type_str.lower() == "undefined":
        return None, f"masterID {master_id}: undefined sentinel slot (skipped)"
    if type_str == "Removed":
        return None, f"masterID {master_id}: 'Removed' dead slot {name!r} (skipped)"

    kind = TYPE_TO_KIND.get(type_str)
    if kind is None:
        # Unmapped but real Type token -> classify Other, needs-review waiver.
        kind = EM.EK_OTHER

    slug = _slug(name, master_id)
    raw_id = f"core:{slug}"
    canon = qualified_id.canonicalize_qualified_id(raw_id)
    if canon is None:
        return None, (f"masterID {master_id}: name {name!r} -> id {raw_id!r} "
                      f"failed qualified-id validation (skipped)")

    missile_col = _cell(row, COL_MISSILE_TYPE)
    fields = _inum(row, COL_FIELDS)

    ed = EM.EquipmentDefinition(
        equip_id=canon,
        display_name=name,
        kind=kind,
        legacy_master_id=master_id,
        heat=_fnum(row, COL_HEAT),
        cost=_fnum(row, COL_RP),
    )

    # Presentation / UI (best-effort; waive what isn't derivable).
    icon1 = _cell(row, COL_ICON1)
    ed.presentation.icon = icon1 if icon1 and icon1 != "0" else ""
    sidx = _inum(row, COL_STRING_INDEX)
    ed.presentation.ui_name_string_id = sidx if sidx else None
    sfx = _inum(row, COL_SPECIAL_FX)
    ed.presentation.special_fx_id = sfx if sfx else None

    if kind == EM.EK_WEAPON:
        category = _weapon_category(type_str, missile_col)
        wf = EM.WeaponFacet(category=category)
        wf.fire_executor = _fire_executor(category, missile_col, fields)
        wf.combat = EM.WeaponCombatStats(
            damage=_fnum(row, COL_DAMAGE),
            heat=_fnum(row, COL_HEAT),
            recycle_time=_fnum(row, COL_RECYCLE),
            range_bucket=_range_bucket(row),
            combat_value=_fnum(row, COL_BR),
        )
        ammo_master = _inum(row, COL_AMMO_MASTER)
        ammo_cat = ""
        if category != EM.WT_ENERGY and ammo_master:
            # Named ammo category derived from the ammo master-id (provenance kept).
            ammo_cat = f"core:ammo_{ammo_master}"
        wf.ammo = EM.WeaponAmmoFacet(ammo_category=ammo_cat, ammo_master_id=ammo_master)
        wf.fit = EM.WeaponFitFacet(
            crit_slots=_inum(row, COL_CRITS),
            tonnage=_fnum(row, COL_WEIGHT),
            allowed_locations=_allowed_locations(row),
        )
        ed.weapon = wf

    ed.tags = _tags_for(kind, ed.weapon.category if ed.weapon else "", missile_col, fields)

    return ed, None


def extract_all(rows: List[List[str]]) -> Tuple[List[EM.EquipmentDefinition], List[str]]:
    """Convert the full compbas row list. Header row (non-integer col0) is skipped
    automatically. Returns (definitions, waivers). Deterministic (file order)."""
    defs: List[EM.EquipmentDefinition] = []
    waivers: List[str] = []
    for row in rows:
        ed, waiver = extract_equipment(row)
        if ed is not None:
            defs.append(ed)
        elif waiver is not None:
            # Header row / empty rows are silent; content skips are reported.
            if waiver.startswith("empty row"):
                continue
            if waiver.startswith("malformed masterID"):
                # The single header row is expected & silent; any OTHER malformed
                # cell IS reported.
                if _cell(row, COL_ID).lower().startswith("component table"):
                    continue
            waivers.append(waiver)
    return defs, waivers


def load_compbas_rows(path: str) -> List[List[str]]:
    """Read compbas.csv into a raw row list (latin-1, no header stripping)."""
    with open(path, "r", encoding="latin-1", newline="") as fh:
        return list(csv.reader(fh))
