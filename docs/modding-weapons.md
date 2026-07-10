# Modding weapons — annotated `equipmentdef.fit` reference

Field-by-field reference for a modded **EquipmentDefinition** (`equipmentdef.fit`),
the file a mod ships under `data/defs/equipment/<namespace>/<slug>/`. Adding a
genuinely new, live, purchasable weapon is the **EQUIP-ADD-FILL-1** path — see
the worked example `examples/mods/sample_equipment/`.

- **Format authority:** `tools/unit_converter/equipment_emit.py`
  (`emit_equipment_fit`) — the canonical block/token order.
- **Engine reader (ADD path):** `applyEquipmentAddFillIfEnabled`
  (`code/equipment_registry_engine.cpp`) — the exact fields the fill pass parses.
- **Offline validator:** `mc2_modlint --equipment <facts>.json` (schema below).

## Required env gates (to make a NEW weapon live)

```
set MC2_UNIT_PROFILE_DATA=1      # family gate for unit/equipment modding
set MC2_EQUIPMENT_ADD=1          # arms the ADD-fill pass
```

Both default OFF (gates-off = byte-identical stock). You do **NOT** need
`MC2_EQUIPMENT_DEFINITION_SOURCE=generated` — that gate is only for the *overlay*
path that re-stats an EXISTING stock weapon. The ADD-fill pass reads the sidecar
directly via the mod index.

## The blocks

```ini
FITini

[Equipment]
st EquipmentId="sample:hot_medium_laser"   ; namespaced qualified id — namespace
                                           ;   ADDS; reusing core:<slug> OVERRIDES stock
st DisplayName="Hot Medium Laser"          ; mechlab / help label
st Description="..."                        ; optional (S3) UI description; absent -> DisplayName
st Kind="Weapon"                           ; Weapon|Ammo|Sensor|ECM|JumpJet|...
st Tags="energy"                           ; free comma list (energy|ballistic|missile|...)
l  LegacyMasterId=40                        ; MANDATORY, EXPLICIT compbas ordinal 0..254.
                                           ;   Must name a FREE (undefined) row. No auto-assign.
                                           ;   255 = 0xff sentinel, never use it.
b  Enabled=TRUE                             ; rides the S1 enable wire -> purchasable with no
                                           ;   purchase.fit edit. Absent = not enabled.
f  Heat=4.5                                 ; equipment-level heat (see also WeaponCombatStats.Heat)
f  Cost=2100.0                              ; C-bill price

[WeaponFacet]                               ; weapon kinds only
st Category="Energy"                        ; Energy|Ballistic|Missile|AntiPersonnel|AMS
st FireExecutor="beam_direct"              ; how it fires (vocab below); "" -> EQV_WEAPON_NO_FIRE_EXECUTOR

[WeaponCombatStats]
f  Damage=3.2
f  Heat=4.5
f  Recycle=4.2                              ; cooldown seconds
st RangeBucket="short"                      ; short|medium|long (case-insensitive) -> ordinal 0|1|2
f  CombatValue=120.0

[WeaponAmmo]                                ; energy weapons: empty category + AmmoMasterId=0
st AmmoCategory=""                          ; non-empty with ammo_per_shot>0 required, else EQV_AMMO_CATEGORY_MISSING
l  AmmoMasterId=0

[WeaponFit]
l  CritSlots=1
f  Tonnage=2.0
st AllowedLocations="CenterTorso,LeftTorso,RightTorso,LeftArm,RightArm"

[Presentation]
st Icon="MCL_MC_Laser.tga"                  ; MANDATORY — mechlab consumer DROPS icon-less rows
st Picture="MCL_MC_Laser"                   ; component picture stem (no extension)
l  UINameStringId=32156                     ; optional IDS override for the UI name
l  SpecialEffect=12                         ; firing VFX cue id (reuse a stock cue)
l  IconX=1                                  ; mechlab icon atlas cell coords (see below)
l  IconY=1

FITend
```

### `FireExecutor` vocabulary (from `equipment_extract.py::_fire_executor`)

| token | used for |
|---|---|
| `beam_direct` | energy weapons |
| `projectile_ballistic` | ballistic weapons (also the safe default) |
| `artillery_indirect` | ballistic + Artillery |
| `lbx_spread` | ballistic + LBX |
| `missile_streak` | streak (ST) missiles |
| `missile_salvo` | LRM/SRM/other missiles |

### Icon atlas coords (`IconX` / `IconY`)

`IconX`/`IconY` index a cell in the mechlab component icon atlas. The engine reads
them as raw ordinals (`code/equipment_registry_engine.cpp`); an invalid cell means
the mechlab row is skipped. The simplest reliable recipe: **reuse an existing
stock icon `.tga` plus its known-good `IconX`/`IconY`** (the sample clones the
stock laser's `MCL_MC_Laser.tga` at `IconX=1 IconY=1`). Authoring a NEW atlas cell
(exact cell dimensions and free-slot map) is not yet documented — treat it as a
follow-up; clone a stock icon until then.

## Offline validation — the facts-JSON

`mc2_modlint --equipment` reads a small **facts-JSON** (NOT the `.fit`) and runs
the SAME coherence check the engine uses. Ship one per weapon at
`modlint/<slug>.facts.json`. Full schema lives in the header comment of
`tools/mc2_modlint/modlint_equipment_input.h`; the core fields:

```json
{
  "equip_id": "sample:hot_medium_laser",
  "kind": "Weapon",
  "has_weapon": true,
  "fire_executor": "beam_direct",
  "category": "Energy",
  "category_authored": true,
  "damage": 3.2,
  "range_short": 90.0, "range_med": 180.0, "range_long": 270.0,
  "ammo_per_shot": 0,
  "ammo_category": "",
  "claimed_events": [], "presented_events": [],
  "hardpoints": [ { "accepts": ["Energy"] } ]
}
```

### Validator finding vocabulary (`EQV_*`)

Stable, grep-able tokens (exit 1 when present). Seen failing in
`examples/mods/sample_bad_mod/`:

| code | meaning |
|---|---|
| `EQV_WEAPON_NO_FIRE_EXECUTOR` | weapon has empty `FireExecutor` |
| `EQV_WEAPON_NO_CATEGORY` | weapon category not authored |
| `EQV_WEAPON_RANGE_INVALID` | range buckets incoherent |
| `EQV_FIRE_EXECUTOR_UNBOUND` | executor token maps to no engine executor |
| `EQV_KIND_FACET_MISMATCH` | `Kind` vs present facet disagree |
| `EQV_AMMO_CATEGORY_MISSING` | `ammo_per_shot>0` but no `ammo_category` |
| `EQV_PRESENTATION_ASSET_MISSING` | a claimed event maps no asset |
| `EQV_WEAPON_HARDPOINT_REJECTED` | weapon category not accepted by any hardpoint |
| `EQV_ADD_ORDINAL_UNAVAILABLE` | `LegacyMasterId` names a populated/claimed ordinal |
| `EQV_ADD_FIELDS_INCOMPLETE` | ADD def missing a mandatory field (e.g. Icon) |
| `EQV_ADD_AMMO_UNRESOLVED` | ADD weapon's ammo reference does not resolve |
| `EQV_ENABLE_UNRESOLVED` | `Enabled=TRUE` on a row that cannot be enabled |

## Conflicts

Duplicate ordinal claims and stock-override conflicts are never silent —
`[EQUIP-ADD]` findings at load, and the `MODCONFLICT_*` load-order report. See the
"Load-order conflict report" and "Conflict consequences" sections of
`examples/mods/README.md`.
