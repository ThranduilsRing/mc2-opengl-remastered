# core_unlocks — re-surface dormant stock gear via the clean enable wire

## Goal

Enable dormant stock content — components that exist fully populated in the
component table but are not offered for purchase by (early) missions'
`purchase.fit` — so they show up in the logistics/mechlab purchase UI.

## What it ships (EQUIP-ENABLE-WIRE-1, ADD-ladder S1)

Two enable-only def overrides, each a tiny `.fit` carrying just the wire fields:

| component | compbas row | file |
|---|---|---|
| Clan Gauss Rifle | 113 | `data/defs/equipment/core/clan_gauss_rifle/equipmentdef.fit` |
| ER PPC | 146 | `data/defs/equipment/core/er_ppc/equipmentdef.fit` |

Both rows are populated stock rows with valid logistics icons, but the early
campaign purchase files never list them (e.g. `purchase01.fit` offers the plain
PPC 145 and no Gauss at all) — so the effect is directly observable in the
mission-1 mechlab.

## How the wire works

`applyEquipmentEnableWireIfEnabled()` (`code/equipment_registry_engine.cpp`)
runs at the end of the purchase-file half of
`LogisticsData::updateAvailability`. For each POPULATED masterList row (the E2
registry's `enabled` flag) it probes
`data/defs/equipment/core/<slug>/equipmentdef.fit` (mod file-shadow serves this
mod's copy); a def declaring `[Equipment] b Enabled=TRUE` whose
`l LegacyMasterId` key-checks against the row ordinal forces
`available[ordinal]=true`.

Properties:

* **Gate**: `MC2_UNIT_PROFILE_DATA=1` (the unit/equipment data gate). Gate-OFF
  → zero file I/O, byte-identical.
* **Enable-only**: the wire ADDS to the mission `purchase.fit` verdicts, never
  removes. Stock availability is reproduced exactly with the mod absent (stock
  generated defs carry no `Enabled` key).
* **Populated rows only**: enabling an undefined compbas ordinal is a no-op
  (that is S2 EQUIP-ADD-FILL-1's job); modlint flags it
  (`EQV_ENABLE_UNRESOLVED`).

## Trying it

1. Copy `examples/mods/core_unlocks/` into your install's `mods/` folder and
   tick it in the launcher (or add `core_unlocks` to `MC2_MOD_DEPS`).
2. Run with `MC2_UNIT_PROFILE_DATA=1`.
3. Start the campaign, enter the mission-1 mechlab: Clan Gauss Rifle and ER PPC
   appear for purchase. Console/log shows
   `[EQUIP-ENABLE-WIRE-1] enabled core:clan_gauss_rifle (masterID=113) ...`.

## Notes / honest caveats

* The overrides live under the reserved `core:` namespace, so the load-order
  conflict report prints `MODCONFLICT_RESERVED_CORE_NAMESPACE` for them — that
  is the report doing its job; the override is deliberate.
* The defs carry no `[WeaponCombatStats]`; if you also run
  `MC2_EQUIPMENT_DEFINITION_SOURCE=generated`, the stat overlay reports "no
  overlayable weapon stat fields" for these two rows and keeps legacy stats —
  intended (this mod changes availability, not stats).
* Validate offline: `mc2_modlint --equipment mods/core_unlocks/modlint/*.facts.json`
  (facts include the wire fields `enabled` / `legacy_master_id`).
