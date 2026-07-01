# tools/unit_converter/model.py
"""Facet dataclasses for the converted UnitDefinition.

These mirror the north-star facet split from the arc roadmap (Identity /
UnitProfile / Locomotion / DamageSchema / Hardpoint / Loadout / Presentation /
UI). They are the neutral in-memory shape the extractor produces and the
emitter serializes into the engine-consumable .fit blocks. No RNG, no I/O.
"""
from __future__ import annotations

import dataclasses
from dataclasses import dataclass, field
from typing import List, Optional


# --- vocabulary that mirrors the engine facet headers (token spellings must
#     match the C++ parsers exactly, see docstrings on the emitter) -----------

# LocomotionModel tokens parsed by code/locomotion_facet.h::parseLocomotionModel
LOCO_MECH_WALKER = "MechWalker"
LOCO_TRACKED = "Tracked"
LOCO_WHEELED = "Wheeled"
LOCO_HOVER = "Hover"
LOCO_INFANTRY = "Infantry"

# DamageSchemaId tokens parsed by code/damage_schema.h::parseDamageSchema
DMG_MECH_8PART = "Mech8Part"
DMG_VEHICLE_FACINGS = "VehicleFacings"
DMG_SQUAD = "Squad"
DMG_SINGLE_ACTOR = "SingleActor"

# Weapon-class tag vocabulary. Derived from the compbas.csv `Type` column
# (EnergyWeapon / BallisticWeapon / MissileWeapon...). Kept as neutral tags so
# the Phase-2 HardpointId validator can compare weapon tag vs hardpoint class.
WT_ENERGY = "Energy"
WT_BALLISTIC = "Ballistic"
WT_MISSILE = "Missile"
WT_ANY = "Any"

# tgl INI [WeaponNodeN] WeaponType integer -> hardpoint weapon-class tag.
# (Legend lives verbatim in every stock mech tgl INI.)
WEAPON_NODE_TYPE_TAG = {
    0: WT_ANY,        # ANY Weapon
    1: WT_MISSILE,    # Missile weapon
    2: WT_BALLISTIC,  # Ballistic Weapon
    3: WT_ANY,        # Non-Energy Weapon (Ballistic|Missile) -> Any for v0
    4: WT_ENERGY,     # Energy Weapon
    5: WT_ANY,        # Direct-Fire Weapon -> Any for v0
}


@dataclass
class Identity:
    unit_id: str                 # namespaced stable id, e.g. "core:atlas"
    display_name: str            # "Atlas"
    kind: str                    # "mech" | "vehicle"
    tonnage: Optional[float] = None
    house_id: Optional[int] = None


@dataclass
class Capabilities:
    jump: bool = False           # baseline CAP_JUMP -> [UnitProfile] Jump=TRUE
    jump_evidence: str = ""      # why (installed jumpjet / node / none)


@dataclass
class Locomotion:
    model: str = LOCO_WHEELED    # one of the LOCO_* tokens


@dataclass
class DamageSchema:
    schema: str = DMG_VEHICLE_FACINGS   # one of the DMG_* tokens
    locations: List[str] = field(default_factory=list)


@dataclass
class Socket:
    name: str                    # model node name (must exist in the tgl INI)
    weapon_type_tag: str = WT_ANY  # hardpoint weapon-class for weapon sockets


@dataclass
class Hardpoint:
    hardpoint_id: str            # stable HardpointId name (never an index)
    socket_name: str             # -> presentation socket / model node
    weapon_tag: str              # allowed weapon class (Energy/Ballistic/...)


@dataclass
class LoadoutEntry:
    weapon_name: str             # "ER Laser"
    weapon_tag: str              # Energy / Ballistic / Missile / Any
    master_id: int               # stock MasterID (provenance)
    hardpoint_id: Optional[str] = None  # bound HardpointId when resolvable


@dataclass
class Presentation:
    model_name: str = ""         # base LOD model file stem (tgl FileName0)
    shadow_name: str = ""
    sockets: List[Socket] = field(default_factory=list)  # weapon + fx nodes


@dataclass
class UIProfile:
    name_string_id: Optional[int] = None       # DescIndex / DescIndex-like
    encyclopedia_string_id: Optional[int] = None
    icon: str = ""                             # little/big icon token or index
    icon_index: Optional[int] = None           # vehicle IconIndex atlas col


@dataclass
class UnitDefinition:
    identity: Identity
    capabilities: Capabilities
    locomotion: Locomotion
    damage: DamageSchema
    presentation: Presentation
    hardpoints: List[Hardpoint] = field(default_factory=list)
    loadout: List[LoadoutEntry] = field(default_factory=list)
    ui: UIProfile = field(default_factory=UIProfile)
    # non-fatal notes surfaced in the conversion report (missing socket, etc.)
    waivers: List[str] = field(default_factory=list)

    def to_report_dict(self) -> dict:
        return {
            "unit_id": self.identity.unit_id,
            "name": self.identity.display_name,
            "kind": self.identity.kind,
            "tonnage": self.identity.tonnage,
            "jump": self.capabilities.jump,
            "jump_evidence": self.capabilities.jump_evidence,
            "locomotion": self.locomotion.model,
            "damage_schema": self.damage.schema,
            "num_sockets": len(self.presentation.sockets),
            "num_hardpoints": len(self.hardpoints),
            "num_loadout": len(self.loadout),
            "num_loadout_bound": sum(1 for e in self.loadout if e.hardpoint_id),
            "model": self.presentation.model_name,
            "waivers": list(self.waivers),
        }
