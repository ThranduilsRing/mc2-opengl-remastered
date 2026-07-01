# tools/unit_converter/extract.py
"""Facet extraction: stock source -> UnitDefinition.

Two entry points (extract_mech / extract_vehicle) knowing the two stock unit
shapes recon'd in .claude/UNIT-DATA-MODEL-RECON-1.md +
PRESENTATION-LOADOUT-STRANGLER-RECON-1.md:

  MECH     = chassis CSV (identity/chassis/armor/loadout) + tgl INI
             (model LODs + [Nodes]/[WeaponNodeN] sockets). 8-part damage,
             MechWalker locomotion. Jump = an INSTALLED jumpjet component
             (compbas Type==JumpJet), NOT the mechlab AllowComponents flag.
  VEHICLE  = .fit type file ([Header] FileType=GroundVehicleType,
             [InternalStructure] facings, [InventoryInfo]/[Item:N] loadout,
             IconIndex/DescIndex) + tgl INI (model LODs, no weapon nodes).
             Vehicle-facings damage, wheeled/tracked locomotion.

Deterministic; no RNG, no engine, no writes.
"""
from __future__ import annotations

import os
from typing import List, Optional

from . import model as M
from .fitparse import ChassisCsv, CompBas, FitIni


def _namespaced(stem: str, namespace: str = "core") -> str:
    return f"{namespace}:{stem.lower()}"


def _weapon_tag_from_comptype(ctype: str) -> str:
    c = ctype.lower()
    if "energy" in c:
        return M.WT_ENERGY
    if "ballistic" in c:
        return M.WT_BALLISTIC
    if "missile" in c:
        return M.WT_MISSILE
    return M.WT_ANY


# --------------------------------------------------------------------------
# tgl INI -> presentation (shared by both kinds)
# --------------------------------------------------------------------------

def _presentation_from_tgl(tgl: FitIni) -> M.Presentation:
    pres = M.Presentation()
    pres.model_name = str(tgl.get("TGLData", "FileName0", "") or "")
    # ShadowName has inconsistent casing across stock files (ShadowName /
    # Shadowname); FitIni keys are case-sensitive so probe both.
    pres.shadow_name = str(
        tgl.get("TGLData", "ShadowName", tgl.get("TGLData", "Shadowname", "")) or ""
    )
    # Weapon sockets: [Nodes] NumWeapon + [WeaponNodeN] SocketName/WeaponType.
    num_weapon = int(tgl.get("Nodes", "NumWeapon", 0) or 0)
    for i in range(num_weapon):
        blk = f"WeaponNode{i}"
        if not tgl.has_block(blk):
            continue
        name = str(tgl.get(blk, "WeaponNodeName", "") or "")
        if not name:
            continue
        wt = int(tgl.get(blk, "WeaponType", 0) or 0)
        pres.sockets.append(
            M.Socket(name=name, weapon_type_tag=M.WEAPON_NODE_TYPE_TAG.get(wt, M.WT_ANY))
        )
    return pres


def _jumpnode_present(tgl: FitIni) -> bool:
    return int(tgl.get("Nodes", "NumJumpJet", 0) or 0) > 0 and tgl.has_block("JumpJetNode0")


# --------------------------------------------------------------------------
# MECH
# --------------------------------------------------------------------------

def extract_mech(csv_path: str, tgl_path: str, compbas: CompBas,
                 namespace: str = "core") -> M.UnitDefinition:
    csvf = ChassisCsv.load(csv_path)
    tgl = FitIni.load(tgl_path)
    stem = os.path.splitext(os.path.basename(csv_path))[0]

    name = csvf.label("MechName", stem) or stem
    variant = csvf.first_variant_name()
    if variant:
        name = variant
    tonnage = _to_float(csvf.label("Tonnage"))
    house = _to_int(csvf.label("HouseID"))

    identity = M.Identity(
        unit_id=_namespaced(stem, namespace), display_name=name,
        kind="mech", tonnage=tonnage, house_id=house,
    )

    # Loadout: ItemNMasterId rows; col4 (0-based) is the CompBas# reference,
    # col5 is the human Name. CompBas#255 == EMPTY, skip. A JumpJet component
    # (compbas Type==JumpJet) installed anywhere == baseline jump capability.
    loadout: List[M.LoadoutEntry] = []
    jump = False
    jump_evidence = "no installed jumpjet"
    for _idx, row in csvf.item_rows():
        compbas_id = _to_int(row[4]) if len(row) > 4 else None
        item_name = row[5].strip() if len(row) > 5 else ""
        if compbas_id is None or compbas_id in (0, 255):
            continue
        ctype = compbas.type_of(compbas_id)
        if ctype.lower() == "jumpjet":
            jump = True
            jump_evidence = f"installed JumpJet component (CompBas#{compbas_id})"
            continue
        tag = _weapon_tag_from_comptype(ctype)
        if tag == M.WT_ANY and "weapon" not in ctype.lower():
            # non-weapon equipment (engine/gyro/sensor/actuator...) -> not a
            # weapon loadout entry; skip for the loadout facet.
            continue
        loadout.append(M.LoadoutEntry(
            weapon_name=item_name or compbas.name_of(compbas_id),
            weapon_tag=tag, master_id=compbas_id,
        ))

    presentation = _presentation_from_tgl(tgl)

    # Hardpoints: one per weapon socket declared by the model. HardpointId is a
    # stable name (never an index): <UnitStem>_<Tag>_<n>. This is the JOIN KEY
    # binding loadout mount <-> model socket <-> (future) damage location.
    hardpoints = _hardpoints_from_sockets(stem, presentation.sockets)
    _bind_loadout(loadout, hardpoints)

    damage = M.DamageSchema(schema=M.DMG_MECH_8PART, locations=[
        "Head", "CTorso", "LTorso", "RTorso", "LArm", "RArm", "LLeg", "RLeg",
    ])

    ui = M.UIProfile(
        name_string_id=_to_int(csvf.label("DescIndex")),
        encyclopedia_string_id=_to_int(csvf.label("EncyclopediaID")),
        icon=str(csvf.label("LittleIcon", "") or ""),
    )

    ud = M.UnitDefinition(
        identity=identity, capabilities=M.Capabilities(jump=jump, jump_evidence=jump_evidence),
        locomotion=M.Locomotion(model=M.LOCO_MECH_WALKER), damage=damage,
        presentation=presentation, hardpoints=hardpoints, loadout=loadout, ui=ui,
    )
    _coherence_notes(ud, tgl, jump_node=_jumpnode_present(tgl))
    return ud


# --------------------------------------------------------------------------
# VEHICLE
# --------------------------------------------------------------------------

def extract_vehicle(fit_path: str, tgl_path: str, compbas: CompBas,
                    namespace: str = "core") -> M.UnitDefinition:
    fit = FitIni.load(fit_path)
    tgl = FitIni.load(tgl_path)
    stem = os.path.splitext(os.path.basename(fit_path))[0]

    name = str(fit.get("ObjectType", "Name", stem) or stem)
    # ObjectType Name is often "<stem>.ini"/"<stem>"; prefer a clean display.
    display = os.path.splitext(name)[0] or stem

    tonnage = _to_float(fit.get("General", "CurTonnage", fit.get("General", "TonnageClass")))

    identity = M.Identity(
        unit_id=_namespaced(stem, namespace), display_name=display,
        kind="vehicle", tonnage=tonnage, house_id=None,
    )

    # Locomotion: stock GroundVehicleType has no explicit wheel/track token, so
    # v0 uses the engine's non-mech default (Wheeled). A future data pass can
    # refine tracked/hover per chassis.
    locomotion = M.Locomotion(model=M.LOCO_WHEELED)

    # Damage: facings from [InternalStructure].
    facings = [k for k in ("Front", "Left", "Right", "Rear", "Turret")
               if fit.get("InternalStructure", k) is not None]
    damage = M.DamageSchema(schema=M.DMG_VEHICLE_FACINGS, locations=facings)

    # Loadout: [InventoryInfo] NumWeapons + [Item:N] MasterID rows; keep only
    # weapon components (compbas Type endswith Weapon).
    loadout: List[M.LoadoutEntry] = []
    i = 0
    while fit.has_block(f"Item:{i}"):
        mid = _to_int(fit.get(f"Item:{i}", "MasterID"))
        i += 1
        if mid is None:
            continue
        ctype = compbas.type_of(mid)
        if not ctype.lower().endswith("weapon"):
            continue
        loadout.append(M.LoadoutEntry(
            weapon_name=compbas.name_of(mid) or f"MasterID{mid}",
            weapon_tag=_weapon_tag_from_comptype(ctype), master_id=mid,
        ))

    presentation = _presentation_from_tgl(tgl)

    # Vehicles declare no weapon nodes in stock tgl INIs -> synthesize a single
    # turret hardpoint so the loadout has a join target. Socket name left empty
    # (no model socket in stock data) -> surfaced as a coherence waiver.
    hardpoints: List[M.Hardpoint] = []
    if loadout:
        turret_tag = loadout[0].weapon_tag
        hardpoints.append(M.Hardpoint(
            hardpoint_id=f"{stem.lower()}_turret_1", socket_name="", weapon_tag=turret_tag,
        ))
    _bind_loadout(loadout, hardpoints)

    ui = M.UIProfile(
        name_string_id=_to_int(fit.get("General", "DescIndex")),
        encyclopedia_string_id=_to_int(fit.get("General", "EncyclopediaID")),
        icon_index=_to_int(fit.get("General", "IconIndex")),
    )

    ud = M.UnitDefinition(
        identity=identity, capabilities=M.Capabilities(jump=False, jump_evidence="vehicle (no jumpjet)"),
        locomotion=locomotion, damage=damage, presentation=presentation,
        hardpoints=hardpoints, loadout=loadout, ui=ui,
    )
    _coherence_notes(ud, tgl, jump_node=False)
    return ud


# --------------------------------------------------------------------------
# shared join / coherence helpers
# --------------------------------------------------------------------------

def _hardpoints_from_sockets(stem: str, sockets: List[M.Socket]) -> List[M.Hardpoint]:
    hardpoints: List[M.Hardpoint] = []
    counters: dict = {}
    for sk in sockets:
        tag = sk.weapon_type_tag
        counters[tag] = counters.get(tag, 0) + 1
        hardpoints.append(M.Hardpoint(
            hardpoint_id=f"{stem.lower()}_{tag.lower()}_{counters[tag]}",
            socket_name=sk.name, weapon_tag=tag,
        ))
    return hardpoints


def _bind_loadout(loadout: List[M.LoadoutEntry], hardpoints: List[M.Hardpoint]) -> None:
    """Deterministic first-fit bind: each weapon claims the first free hardpoint
    whose weapon_tag is compatible (exact tag, or an Any hardpoint). Preview of
    the Phase-2 HardpointId join; the C++ validator refines legality later."""
    used = [False] * len(hardpoints)
    for entry in loadout:
        for hi, hp in enumerate(hardpoints):
            if used[hi]:
                continue
            if hp.weapon_tag == entry.weapon_tag or hp.weapon_tag == M.WT_ANY:
                entry.hardpoint_id = hp.hardpoint_id
                used[hi] = True
                break


def _coherence_notes(ud: M.UnitDefinition, tgl: FitIni, jump_node: bool) -> None:
    """Python-side sanity preview of what the C++ validator will confirm."""
    socket_names = {s.name for s in ud.presentation.sockets}
    for hp in ud.hardpoints:
        if hp.socket_name and hp.socket_name not in socket_names:
            ud.waivers.append(
                f"hardpoint {hp.hardpoint_id} references socket '{hp.socket_name}' "
                f"not present in model nodes"
            )
        if not hp.socket_name:
            ud.waivers.append(
                f"hardpoint {hp.hardpoint_id} has no model socket "
                f"(stock data supplies no weapon node for this unit)"
            )
    for entry in ud.loadout:
        if entry.hardpoint_id is None:
            ud.waivers.append(
                f"weapon '{entry.weapon_name}' ({entry.weapon_tag}) has no "
                f"compatible hardpoint to bind to"
            )
    if not ud.presentation.model_name:
        ud.waivers.append("presentation has no model (tgl FileName0 empty)")
    # jump vs jumpjet-node coherence: capability says jump but the model has no
    # jumpjet node (or vice-versa) is worth a note, not an error.
    if ud.capabilities.jump and not jump_node:
        ud.waivers.append("jump-capable but model declares no jumpjet node")


def _to_int(v) -> Optional[int]:
    if v is None:
        return None
    try:
        return int(str(v).strip())
    except (ValueError, TypeError):
        return None


def _to_float(v) -> Optional[float]:
    if v is None:
        return None
    try:
        return float(str(v).strip())
    except (ValueError, TypeError):
        return None
