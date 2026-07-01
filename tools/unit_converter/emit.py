# tools/unit_converter/emit.py
"""Emit sidecar UnitDefinition .fit files in the EXACT block/token format the
Phase-2 engine loaders parse, so a later slice can feed them back in.

Format authority (grep'd in the canonical worktree, not from memory):

  code/unitprofile_fit.cpp::populateProfileFromFit
    [UnitProfile]  b Jump=TRUE|FALSE            (readIdBoolean "Jump")
                   st Locomotion="MechWalker"    (readIdString  "Locomotion")
                   st DamageSchema="Mech8Part"   (readIdString  "DamageSchema")
    [UIProfile]    st Icon / Portrait / Minimap / Silhouette
                   l  NameID / EncyclopediaID    (readIdLong)

  mclib/mech3d.cpp (PRESENTATION-SOCKET-PROFILE-1 slice)
    [PresentationSockets]  l NumSockets = N
    [SocketN]              st SocketName = "<model node name>"
                           (sprintf "Socket%ld"; readIdString "SocketName")

Hardpoints + Loadout have no shipped engine reader yet (HardpointId schema is
Phase-2 slice 1, not yet a .fit block), so they are emitted to their own
sidecar files in a forward-looking, self-consistent block form clearly marked
as not-yet-consumed. The engine-consumable files (unit/presentation/ui) use the
exact tokens above.

FitIni type sigils used: st=string, l=long, b=bool. `FITini`/`FITend` bracket
each file (stock loader tolerates their presence; matches every stock .fit).
Deterministic byte output; LF newlines; keys emitted in a fixed order.
"""
from __future__ import annotations

import os
from typing import List

from . import model as M


def _bool_tok(v: bool) -> str:
    return "TRUE" if v else "FALSE"


def emit_chassisdef_fit(ch: "M.ChassisProfile") -> str:
    """LOADOUT-THREE-LAYER-1 (Phase 3): `chassisdef.fit` — the SHARED frame
    (BT2018 `chassisdef`), emitted ONCE per chassis and referenced by every
    variant's mechdef via ChassisID. Holds locomotion/damage/hardpoints (the
    fixed frame); NOT the per-variant loadout or jump capability. Hardpoints are
    REPORT-only mount-type descriptors (enforcement is a deferred future slice).
    The [UnitProfile] Locomotion/DamageSchema tokens match populateProfileFromFit;
    the chassis/hardpoint blocks are forward-looking (no shipped reader yet)."""
    lines: List[str] = ["FITini", ""]
    lines.append("// LEGACY-UNIT-CONVERTER-1 generated chassisdef (do not hand-edit).")
    lines.append(f"// chassis: {ch.chassis_id}  kind={ch.kind}")
    lines.append("")
    lines.append("[Chassis]")
    lines.append(f'st ChassisID="{ch.chassis_id}"')
    lines.append(f'st DisplayName="{ch.display_name}"')
    if ch.tonnage is not None:
        lines.append(f"f Tonnage={ch.tonnage}")
    lines.append("")
    lines.append("[UnitProfile]")
    lines.append(f'st Locomotion="{ch.locomotion.model}"')
    lines.append(f'st DamageSchema="{ch.damage.schema}"')
    lines.append("")
    lines.append("[Hardpoints]")
    lines.append(f"l NumHardpoints = {len(ch.hardpoints)}")
    lines.append("")
    for i, hp in enumerate(ch.hardpoints):
        lines.append(f"[Hardpoint{i}]")
        lines.append(f'st HardpointId = "{hp.hardpoint_id}"')
        lines.append(f'st SocketName = "{hp.socket_name}"')
        lines.append(f'st WeaponTag = "{hp.weapon_tag}"')
        lines.append("")
    lines.append("FITend")
    lines.append("")
    return "\n".join(lines)


def emit_unit_fit(ud: M.UnitDefinition) -> str:
    """`unit.fit` (per-variant mechdef): [UnitProfile] — the engine-consumable
    capability/locomotion/damage facet block. Tokens match populateProfileFromFit
    exactly. References its shared chassis via ChassisID (LOADOUT-THREE-LAYER-1)."""
    lines: List[str] = ["FITini", ""]
    lines.append("// LEGACY-UNIT-CONVERTER-1 generated mechdef (do not hand-edit).")
    lines.append(f"// variant: {ud.identity.unit_id}  kind={ud.identity.kind}")
    if ud.identity.chassis_id:
        lines.append(f"// chassis: {ud.identity.chassis_id}"
                     f"  variant_index={ud.identity.variant_index}"
                     f'  variant_name="{ud.identity.variant_name}"')
    lines.append("")
    lines.append("[Variant]")
    if ud.identity.chassis_id:
        lines.append(f'st ChassisID="{ud.identity.chassis_id}"')
    lines.append(f'st VariantName="{ud.identity.variant_name}"')
    lines.append(f"l VariantIndex={ud.identity.variant_index}")
    lines.append("")
    lines.append("[UnitProfile]")
    lines.append(f"b Jump={_bool_tok(ud.capabilities.jump)}")
    lines.append(f'st Locomotion="{ud.locomotion.model}"')
    lines.append(f'st DamageSchema="{ud.damage.schema}"')
    lines.append("")
    lines.append("FITend")
    lines.append("")
    return "\n".join(lines)


def emit_presentation_fit(ud: M.UnitDefinition) -> str:
    """`presentation.fit`: [PresentationSockets] — exact mech3d.cpp format."""
    sockets = ud.presentation.sockets
    lines: List[str] = ["FITini", ""]
    lines.append("// LEGACY-UNIT-CONVERTER-1 generated sidecar.")
    lines.append(f'// model: {ud.presentation.model_name}  shadow: {ud.presentation.shadow_name}')
    lines.append("")
    lines.append("[PresentationSockets]")
    lines.append(f"l NumSockets = {len(sockets)}")
    lines.append("")
    for i, sk in enumerate(sockets):
        lines.append(f"[Socket{i}]")
        lines.append(f'st SocketName = "{sk.name}"')
        lines.append("")
    lines.append("FITend")
    lines.append("")
    return "\n".join(lines)


def emit_ui_fit(ud: M.UnitDefinition) -> str:
    """`ui.fit`: [UIProfile] — exact populateProfileFromFit token names."""
    ui = ud.ui
    lines: List[str] = ["FITini", ""]
    lines.append("// LEGACY-UNIT-CONVERTER-1 generated sidecar.")
    lines.append("")
    lines.append("[UIProfile]")
    if ui.icon:
        lines.append(f'st Icon="{ui.icon}"')
    if ui.name_string_id is not None:
        lines.append(f"l NameID={ui.name_string_id}")
    if ui.encyclopedia_string_id is not None:
        lines.append(f"l EncyclopediaID={ui.encyclopedia_string_id}")
    # icon_index (vehicle atlas column) has no shipped [UIProfile] token; keep
    # it as provenance so the shadow-compare slice can reconcile it.
    if ui.icon_index is not None:
        lines.append(f"// provenance: legacy IconIndex={ui.icon_index}")
    lines.append("")
    lines.append("FITend")
    lines.append("")
    return "\n".join(lines)


def emit_hardpoints_fit(ud: M.UnitDefinition) -> str:
    """`hardpoints.fit`: forward-looking HardpointId table (Phase-2 slice 1
    schema; NOT yet read by the engine). Self-consistent block form."""
    lines: List[str] = ["FITini", ""]
    lines.append("// LEGACY-UNIT-CONVERTER-1 generated sidecar.")
    lines.append("// NOTE: HardpointId .fit block has no shipped engine reader")
    lines.append("//       yet (Phase-2 HARDPOINT-ID-SCHEMA-1). Emitted for the")
    lines.append("//       shadow-compare slice and future consumption.")
    lines.append("")
    lines.append("[Hardpoints]")
    lines.append(f"l NumHardpoints = {len(ud.hardpoints)}")
    lines.append("")
    for i, hp in enumerate(ud.hardpoints):
        lines.append(f"[Hardpoint{i}]")
        lines.append(f'st HardpointId = "{hp.hardpoint_id}"')
        lines.append(f'st SocketName = "{hp.socket_name}"')
        lines.append(f'st WeaponTag = "{hp.weapon_tag}"')
        lines.append("")
    lines.append("FITend")
    lines.append("")
    return "\n".join(lines)


def emit_loadout_fit(ud: M.UnitDefinition) -> str:
    """`loadout.fit`: weapons bound to HardpointIds (Weapon@HardpointId, never
    Weapon@modelBone). Forward-looking; not yet engine-read."""
    lines: List[str] = ["FITini", ""]
    lines.append("// LEGACY-UNIT-CONVERTER-1 generated sidecar.")
    lines.append("")
    lines.append("[Loadout]")
    lines.append(f"l NumEntries = {len(ud.loadout)}")
    lines.append("")
    for i, e in enumerate(ud.loadout):
        lines.append(f"[Entry{i}]")
        lines.append(f'st Weapon = "{e.weapon_name}"')
        lines.append(f'st WeaponTag = "{e.weapon_tag}"')
        lines.append(f'st HardpointId = "{e.hardpoint_id or ""}"')
        lines.append(f"l MasterID = {e.master_id}")
        lines.append("")
    lines.append("FITend")
    lines.append("")
    return "\n".join(lines)


# emitted file set: (filename, emitter). unit/presentation/ui are the
# engine-consumable ones; hardpoints/loadout are forward-looking.
EMITTERS = [
    ("unit.fit", emit_unit_fit),
    ("presentation.fit", emit_presentation_fit),
    ("ui.fit", emit_ui_fit),
    ("hardpoints.fit", emit_hardpoints_fit),
    ("loadout.fit", emit_loadout_fit),
]


def emit_all(ud: M.UnitDefinition) -> dict:
    """Return {filename: text} for the full sidecar set (no I/O)."""
    return {fname: fn(ud) for fname, fn in EMITTERS}


def write_chassis(ch: "M.ChassisProfile", out_root: str) -> str:
    """LOADOUT-THREE-LAYER-1: write the shared chassisdef ONCE per chassis to
    <out_root>/core/<chassis-stem>/chassisdef.fit. Returns the file path."""
    stem = ch.chassis_id.split(":", 1)[-1]
    unit_dir = os.path.join(out_root, "core", stem)
    os.makedirs(unit_dir, exist_ok=True)
    path = os.path.join(unit_dir, "chassisdef.fit")
    with open(path, "w", encoding="latin-1", newline="\n") as fh:
        fh.write(emit_chassisdef_fit(ch))
    return path


def write_unit(ud: M.UnitDefinition, out_root: str) -> str:
    """Write the per-variant sidecar set. Mech variants share a chassis dir and
    get a per-variant subdir so the mechdef sits next to its chassisdef:
      <out_root>/core/<chassis-stem>/<variant-slug>/{unit,presentation,...}.fit
    Vehicles (no variant) keep the flat <chassis-stem>/ dir. Returns the dir.
    LF newlines, deterministic."""
    stem = ud.identity.unit_id.split(":", 1)[-1]
    chassis_stem = (ud.identity.chassis_id.split(":", 1)[-1]
                    if ud.identity.chassis_id else stem)
    if ud.identity.kind == "mech" and stem != chassis_stem:
        # variant suffix -> per-variant subdir under the chassis dir
        variant_slug = stem[len(chassis_stem) + 1:] if stem.startswith(chassis_stem + "_") else stem
        unit_dir = os.path.join(out_root, "core", chassis_stem, variant_slug)
    else:
        unit_dir = os.path.join(out_root, "core", stem)
    os.makedirs(unit_dir, exist_ok=True)
    for fname, text in emit_all(ud).items():
        with open(os.path.join(unit_dir, fname), "w", encoding="latin-1", newline="\n") as fh:
            fh.write(text)
    return unit_dir
