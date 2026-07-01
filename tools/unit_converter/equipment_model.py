# tools/unit_converter/equipment_model.py
"""LEGACY-COMPBAS-CONVERTER-1 (equipment arc, slice 4) — facet dataclasses for
the converted EquipmentDefinition.

These mirror the offline C++ schema (code/equipment_definition.h +
code/weapon_definition.h): an EquipmentDefinition is a FACET AGGREGATOR over the
whole compbas component family (weapons, ammo, sensors, ECM, jump-jets,
heatsinks, structure, cockpit, actuators, engines, ...), NOT "weapons forever".
A weapon is simply an EquipmentDefinition carrying a WeaponFacet.

Token spellings (kind names, WeaponTag, fireExecutor names, event names) match
the C++ header field names so a FUTURE engine reader can consume the emitted
blocks. No RNG, no I/O.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional


# --- EquipmentKind tokens (mirror enum EquipmentKind, equipment_definition.h) ---
EK_WEAPON = "Weapon"
EK_AMMO = "Ammo"
EK_SENSOR = "Sensor"
EK_ECM = "ECM"
EK_JUMPJET = "JumpJet"
EK_HEATSINK = "Heatsink"
EK_STRUCTURE = "Structure"
EK_COCKPIT = "Cockpit"
EK_ACTUATOR = "Actuator"
EK_ENGINE = "Engine"
EK_OTHER = "Other"

# --- WeaponTag tokens (mirror hardpoint.h WeaponTag; converter neutral tags) ---
WT_ENERGY = "Energy"
WT_BALLISTIC = "Ballistic"
WT_MISSILE = "Missile"
WT_ANY = "Any"

# --- fireExecutor default names (mirror weapon_definition.h WeaponFireExecutorId
#     docstring: beam_direct / projectile_ballistic / missile_salvo / ...). These
#     are STARTING DEFAULTS derived from FORM + missile-type; a later slice /
#     author refines them. Kept as stable names (no fixed enum). ------------------
FX_BEAM_DIRECT = "beam_direct"
FX_PROJECTILE_BALLISTIC = "projectile_ballistic"
FX_MISSILE_SALVO = "missile_salvo"
FX_MISSILE_STREAK = "missile_streak"       # streak SRM (locked salvo)
FX_ARTILLERY_INDIRECT = "artillery_indirect"  # Fields bit 8 = Artillery
FX_LBX_SPREAD = "lbx_spread"               # Fields bit 4 = LBX cluster
FX_UNSET = ""                              # no executor bound


@dataclass
class WeaponCombatStats:
    """Mirror WeaponCombatStats (weapon_definition.h). Range here is the single
    stock range BUCKET token (short/medium/long) kept as a name; the stock CSV
    carries one range bucket per weapon, not three thresholds."""
    damage: float = 0.0
    heat: float = 0.0
    recycle_time: float = 0.0
    range_bucket: str = ""       # "short"|"medium"|"long"|"" (stock Range column)
    combat_value: float = 0.0    # BR column


@dataclass
class WeaponAmmoFacet:
    """Mirror WeaponAmmoFacet: ammo linkage by NAMED category, not raw offset.
    ammo_category empty == self-powered (energy). ammo_master_id kept as
    provenance of the stock WEAPON_AMMO_OFFSET arithmetic for the shadow slice."""
    ammo_category: str = ""          # named category id (empty = none)
    ammo_master_id: int = 0          # stock ammo master-id column (provenance)


@dataclass
class WeaponFitFacet:
    """Mirror WeaponFitFacet: crit slots + tonnage + allowed locations. Allowed
    locations come from the compbas per-location Yes/No columns."""
    crit_slots: int = 0
    tonnage: float = 0.0
    allowed_locations: List[str] = field(default_factory=list)  # body-location names


@dataclass
class WeaponFacet:
    """Mirror WeaponFacet (weapon_definition.h). Category is a WeaponTag; the
    fireExecutor is a best-effort default name; combat/ammo/fit are composed."""
    category: str = WT_ENERGY
    fire_executor: str = FX_UNSET
    combat: WeaponCombatStats = field(default_factory=WeaponCombatStats)
    ammo: WeaponAmmoFacet = field(default_factory=WeaponAmmoFacet)
    fit: WeaponFitFacet = field(default_factory=WeaponFitFacet)


@dataclass
class EquipmentPresentation:
    """Best-effort presentation/UI refs (event-based; see emitter). Derivable
    fields only — icon token + UI name-string id from the compbas columns.
    Undeductible cues are WAIVED, not faked."""
    icon: str = ""                       # Logistics Icon 1 (tga token), if present
    ui_name_string_id: Optional[int] = None   # String Index column (localized name)
    special_fx_id: Optional[int] = None  # stock Special FX ID (provenance)


@dataclass
class EquipmentDefinition:
    """Mirror EquipmentDefinition (equipment_definition.h): id / kind / tags /
    legacyMasterId + optional WeaponFacet + presentation. legacy_master_id is the
    compbas row ordinal — the ALIAS, never the identity."""
    equip_id: str                    # canonical core:<slug> (identity)
    display_name: str                # raw component Name
    kind: str = EK_OTHER
    tags: List[str] = field(default_factory=list)   # energy/ballistic/missile/...
    legacy_master_id: int = -1       # compbas row ordinal (alias)
    weapon: Optional[WeaponFacet] = None            # present iff kind == Weapon
    presentation: EquipmentPresentation = field(default_factory=EquipmentPresentation)
    heat: float = 0.0                # component metadata (EquipmentMetadata.heat)
    cost: float = 0.0               # RP / value metric (EquipmentMetadata.cost)

    def to_report_dict(self) -> dict:
        d = {
            "equip_id": self.equip_id,
            "name": self.display_name,
            "kind": self.kind,
            "tags": list(self.tags),
            "legacy_master_id": self.legacy_master_id,
        }
        if self.weapon is not None:
            w = self.weapon
            d["weapon"] = {
                "category": w.category,
                "fire_executor": w.fire_executor,
                "damage": w.combat.damage,
                "heat": w.combat.heat,
                "recycle": w.combat.recycle_time,
                "range_bucket": w.combat.range_bucket,
                "ammo_category": w.ammo.ammo_category,
                "crit_slots": w.fit.crit_slots,
            }
        return d
