# tools/unit_converter/equipment_emit.py
"""LEGACY-COMPBAS-CONVERTER-1 (equipment arc, slice 4) — emit sidecar
EquipmentDefinition .fit files.

Forward-looking: NO shipped engine reader consumes these yet (equipment schema
is header-only, code/equipment_definition.h). The blocks/tokens use the SCHEMA
FIELD NAMES so a FUTURE engine reader can consume them:

  [Equipment]     st EquipmentId / st DisplayName / st Kind / st Tags
                  l  LegacyMasterId   (== compbas row ordinal ALIAS)
                  f  Heat / Cost
  [WeaponFacet]   st Category / st FireExecutor            (weapon kinds only)
  [WeaponCombatStats] f Damage / Heat / Recycle / st RangeBucket / f CombatValue
  [WeaponAmmo]    st AmmoCategory / l AmmoMasterId
  [WeaponFit]     l CritSlots / f Tonnage / st AllowedLocations
  [Presentation]  st Icon / l UINameStringId / (// provenance SpecialFXId)
                  event-based cue slots are WAIVED (not derivable from compbas)

FitIni type sigils: st=string, l=long, f=float, b=bool. `FITini`/`FITend`
bracket each file. Deterministic byte output; LF newlines; fixed key order.
"""
from __future__ import annotations

import os
from typing import List

from . import equipment_model as EM


def emit_equipment_fit(ed: "EM.EquipmentDefinition") -> str:
    lines: List[str] = ["FITini", ""]
    lines.append("// LEGACY-COMPBAS-CONVERTER-1 generated EquipmentDefinition"
                 " (do not hand-edit).")
    lines.append(f"// legacyMasterId (compbas row ordinal, ALIAS not identity):"
                 f" {ed.legacy_master_id}")
    lines.append("// NOTE: no shipped engine reader yet (equipment schema is"
                 " header-only).")
    lines.append("")
    lines.append("[Equipment]")
    lines.append(f'st EquipmentId="{ed.equip_id}"')
    lines.append(f'st DisplayName="{ed.display_name}"')
    lines.append(f'st Kind="{ed.kind}"')
    lines.append(f'st Tags="{",".join(ed.tags)}"')
    lines.append(f"l LegacyMasterId={ed.legacy_master_id}")
    lines.append(f"f Heat={ed.heat}")
    lines.append(f"f Cost={ed.cost}")
    lines.append("")

    if ed.weapon is not None:
        w = ed.weapon
        lines.append("[WeaponFacet]")
        lines.append(f'st Category="{w.category}"')
        lines.append(f'st FireExecutor="{w.fire_executor}"')
        lines.append("")
        lines.append("[WeaponCombatStats]")
        lines.append(f"f Damage={w.combat.damage}")
        lines.append(f"f Heat={w.combat.heat}")
        lines.append(f"f Recycle={w.combat.recycle_time}")
        lines.append(f'st RangeBucket="{w.combat.range_bucket}"')
        lines.append(f"f CombatValue={w.combat.combat_value}")
        lines.append("")
        lines.append("[WeaponAmmo]")
        lines.append(f'st AmmoCategory="{w.ammo.ammo_category}"')
        lines.append(f"l AmmoMasterId={w.ammo.ammo_master_id}")
        lines.append("")
        lines.append("[WeaponFit]")
        lines.append(f"l CritSlots={w.fit.crit_slots}")
        lines.append(f"f Tonnage={w.fit.tonnage}")
        lines.append(f'st AllowedLocations="{",".join(w.fit.allowed_locations)}"')
        lines.append("")

    p = ed.presentation
    lines.append("[Presentation]")
    if p.icon:
        lines.append(f'st Icon="{p.icon}"')
    if p.ui_name_string_id is not None:
        lines.append(f"l UINameStringId={p.ui_name_string_id}")
    if p.special_fx_id is not None:
        lines.append(f"// provenance: legacy SpecialFXId={p.special_fx_id}")
    # Event-based presentation grid (OnFire/OnImpact/... x sfx/vfx/decal/light):
    # NOT derivable from compbas (stock has only a single raw SpecialFX id) --
    # WAIVED here, authored by a later presentation slice.
    lines.append("// event-based presentation cues WAIVED (not derivable from"
                 " compbas)")
    lines.append("")
    lines.append("FITend")
    lines.append("")
    return "\n".join(lines)


def write_equipment(ed: "EM.EquipmentDefinition", out_root: str) -> str:
    """Write one EquipmentDefinition sidecar to
    <out_root>/core/<slug>/equipmentdef.fit. Returns the file path. LF newlines,
    deterministic, latin-1."""
    slug = ed.equip_id.split(":", 1)[-1]
    equip_dir = os.path.join(out_root, "core", slug)
    os.makedirs(equip_dir, exist_ok=True)
    path = os.path.join(equip_dir, "equipmentdef.fit")
    with open(path, "w", encoding="latin-1", newline="\n") as fh:
        fh.write(emit_equipment_fit(ed))
    return path
