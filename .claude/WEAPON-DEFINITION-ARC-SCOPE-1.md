# WEAPON-DEFINITION arc — scoping (read-only recon)

**Status:** RECON + SCOPING only. No code, no `mc2srcdata` changes. Mirrors the shape of the UNIT-PROFILE / `UnitDefinition` arc (`docs/superpowers/plans/2026-07-01-unit-profile-arc-roadmap.md`). Goal: move MC2 weapons/components from **positional/index representation** to a data-driven, BT2018-familiar `WeaponDefinition` so a modder adds a weapon as easily as a unit.

**★ STOCK ASSETS ONLY** in every claim about MC2 data — base `mc2srcdata` only (never MCO/MC2X/BT2018/mods). BT2018 is cited purely as the modder-familiar *format reference*.

**★ The join key already exists.** The unit arc built `WeaponTag {WTAG_ENERGY, WTAG_BALLISTIC, WTAG_MISSILE, WTAG_ANTIPERSONNEL, WTAG_AMS}` (`code/hardpoint.h:36`) + `HardpointProfile.acceptedTags` + `hardpointAcceptsWeapon()` (`hardpoint.h:158`). Today those describe what a hardpoint *accepts* but **nothing describes the weapon's tag** — the weapon side of that join is missing. `WeaponDefinition.category → WeaponTag` is the missing half. This arc supplies it; HARDPOINT-ENFORCEMENT-1 then closes the loop.

---

## 1. Current MC2 weapon/component data model (STOCK) — the pain

Weapons are not a distinct type. They are rows in the single **component master table** `mc2srcdata/objects/compbas.csv`, loaded into a flat array of `MasterComponent`. The disease is identical to the unit side: **row-index = identity, positional CSV columns, no string ids, per-location legality baked as a Yes/No grid.**

### Representation
- **Master table:** `MasterComponent::masterList` (flat array) + `numComponents` (`mclib/cmponent.h:176-177`). Loaded by `MasterComponent::loadMasterList()` (`mclib/cmponent.cpp:630`), which reads `compbas.csv` line-by-line and calls `initEXCEL()` per row (`cmponent.cpp:665-672`). Called with a **hardcoded `255`** component count from exactly two sites: `code/mission.cpp:2451`, `code/saveload.cpp:846`.
- **Row index = masterID.** `masterID = atoi(field)` is literally the first CSV column (`cmponent.cpp:98`), i.e. the row's ordinal. There is no string identity anywhere. `WEAPON_AMMO_OFFSET = 65` (`cmponent.h:50`) — a weapon's ammo component is found by *arithmetic on the index* in some paths; ammo linkage otherwise via the `Ammo Master ID` column → `stats.weapon.ammoMasterId` (`cmponent.cpp:238`), again a raw index.
- **Component FORM enum** discriminates kind (`cmponent.h:52-74`): `COMPONENT_FORM_WEAPON_ENERGY / _BALLISTIC / _MISSILE` are the three weapon forms (plus a legacy `COMPONENT_FORM_WEAPON`). The FORM is parsed by **string-matching the CSV `Type` column** against `ComponentFormString[]` (`cmponent.cpp:25-47`, matched at `cmponent.cpp:105-107`): `"EnergyWeapon"`, `"BallisticWeapon"`, `"MissileWeapon"`. So the *category vocabulary already exists as strings in the CSV* — it is just never surfaced as a stable weapon tag.
- **Stats are a `union ComponentStats`** (`cmponent.h:108-147`) — the `weapon` arm holds: `heat`, `damage`, `recycleTime`, `ammoAmount`, `ammoType` (`WeaponAmmoType` enum: NONE/SRM/LRM/ST/LIMITED, `cmponent.h:76-83`), `ammoMasterId`, `range` (a **bucket** `WeaponRangeType` short/med/long, `cmponent.h:92-99`, parsed from the `Range` text column `cmponent.cpp:142-153`), `flags` (`WEAPON_FLAG_STREAK/INFERNO/LBX/ARTILLERY`, `cmponent.h:103-106`), `specialEffect` (the FX/muzzle cue — a raw `char` id, `cmponent.cpp:237`), `type`.
- **Per-location legality:** `criticalSpacesReq[NUM_BODY_LOCATIONS]` (8 locations, `cmponent.h:166`, `NUM_BODY_LOCATIONS=8` `cmponent.h:40`). Parsed from **8 positional Yes/No/number columns** `Location Head/CT/LT/RT/LA/RA/LL/RL` (`cmponent.cpp:155-169`): `"No"→-1` (illegal), `"Yes"→0` (legal, 0 extra crits), number → crit-spaces required. This is a *body-location* grid — NOT a hardpoint-type filter. It does not restrict *which weapon class* mounts where.
- **Loadout reference:** a unit's mounted equipment is `InventoryItem inventory[MAX_MOVER_INVENTORY_ITEMS]` (`code/mover.h:412-440,655`, cap 72 `mover.h:616`). Each `InventoryItem.masterID` is an `unsigned char` index into `masterList` (`mover.h:415`) — **the loadout points at a weapon purely by row number.** `facing` doubles as the weapon Node ID (`mover.h:421`). `bodyLocation` says which of the 8 body parts. (The unit arc already added an optional `hardpointId` FNV hash here, `mover.h:439`, default 0 = legacy positional fallback — the seam is pre-drilled.)
- **Localized name/icon:** `name`/`abbreviation` are NOT in the CSV — they come from string tables via `cLoadString(COMPONENT_NAME_START+masterID, …)` (`cmponent.cpp:100-101`), i.e. **IDS-number keyed on the row index again**. Icons are the `Logistics Icon 1/2` TGA columns + `Icon X/Y` atlas coords.

### Sample stock weapon rows (verbatim, `compbas.csv`)
```
102,BallisticWeapon,Heavy Autocannon (AC20),12,5.88,7.00,16.00,12.00,545.11,7100,short,No,Yes,Yes,Yes,Yes,Yes,No,No,1,0,19,202,MCL_MC_HeavyAutoCannon.tga,...
120,MissileWeapon,LRM5 Rack,2,9.50,2.00,4.00,3.00,174.46,1700,long,No,Yes,Yes,Yes,Yes,Yes,No,No,LRM,0,9,220,MCL_MC_LRMRack.tga,...
```
Columns: `masterID, Type, Name, crit hits, Recycle, Heat, Weight, Damage, BR(CV), RP, Range, [8×Location Yes/No], MissileType, Fields(flags), SpecialFX, AmmoMasterID, Icon1, Icon2, StringIdx, MechpediaStr, IconX, IconY` (header `compbas.csv:1`).

### Index/positional diseases (same as the unit side)
1. **Row-index = ID.** `masterID` is the CSV ordinal (`cmponent.cpp:98`). Insert/reorder a row → every loadout, ammo-master-id, IDS string offset, and the `WEAPON_AMMO_OFFSET=65` arithmetic shifts. Modders cannot add a weapon without renumbering.
2. **Positional columns.** 28 unlabeled comma fields via `strtok` (`cmponent.cpp:92-197`); an extra/missing comma silently misaligns everything after it (e.g. the ECM branch `strtok`s an *extra* field mid-parse, `cmponent.cpp:211`).
3. **Loadout by number.** `InventoryItem.masterID` is a bare `unsigned char` index (`mover.h:415`); a `.fit` loadout says "mount component 102", not "mount `core:ac20`".
4. **Ammo by number/offset.** `ammoMasterId` raw index (`cmponent.cpp:238`) + `WEAPON_AMMO_OFFSET` arithmetic — no named ammo category.
5. **Name/icon by parallel index space.** `cLoadString(NAME_START+masterID)` (`cmponent.cpp:100`) + atlas X/Y — a third index language keyed on the same fragile row number.
6. **Category is a throwaway string.** `"BallisticWeapon"` is matched to a FORM enum then discarded; there is no surviving weapon-category value a validator can compare against a hardpoint's `acceptedTags`.
7. **Hardpoints don't restrict weapons.** `criticalSpacesReq` is body-location crit-space accounting, not mount-type legality — you can put an AC20 anywhere its location grid allows, regardless of hardpoint class. (Same "hardpoints are firing-location, not a filter" finding as the unit roadmap §100.)

---

## 2. BT2018 weapon format (reference — the modder-familiar target)

`A:\Games\mc2-opengl\BattleTech_2018_Dump\TextAsset\Weapon_*.json` / `Ammunition_*.json`. Everything is a **Component** with a **string-id reference graph** — no positional index anywhere.

**`Weapon_Autocannon_UAC5_0-STOCK.json`** (verbatim, trimmed):
```json
{
    "Category" : "Ballistic",            // ← must match a hardpoint WeaponMount
    "Type" : "Autocannon",
    "WeaponSubType" : "UAC5",
    "MinRange" : 90, "MaxRange" : 540,
    "RangeSplit" : [360, 360, 540],      // short/med/long thresholds (real distances)
    "AmmoCategory" : "AC5",              // ← named ammo linkage (string, not index)
    "HeatGenerated" : 16,
    "Damage" : 45,
    "RefireModifier" : 4,
    "ShotsWhenFired" : 2, "ProjectilesPerShot" : 2,
    "WeaponEffectID" : "WeaponEffect-Weapon_AC5_Ultra",   // ← FX by string id
    "Description" : {
        "Cost" : 220000, "Manufacturer" : "Mydron",
        "UIName" : "UAC/5",
        "Id" : "Weapon_Autocannon_UAC5_0-STOCK",          // ← the stable string id
        "Name" : "UAC/5",
        "Icon" : "uixSvgIcon_weapon_Ballistic"            // ← UI icon by string
    },
    "ComponentType" : "Weapon",
    "PrefabIdentifier" : "uac5",         // ← model/prefab by string
    "InventorySize" : 2,                 // ← crit slots
    "Tonnage" : 9,
    "AllowedLocations" : "All", "DisallowedLocations" : "All",
    "statusEffects" : [ ],
    "ComponentTags" : { "items" : ["component_type_stock","range_very-long", ...] }
}
```

**`Ammunition_LB10X.json`** (verbatim):
```json
{
   "Description" : { "Id" : "Ammunition_LB10X", "Name" : "LB 10-X Ammo", ... },
   "ammunitionTypeID" : "Normal",
   "ammoCategoryID" : "LB10X",          // ← weapon.AmmoCategory joins to THIS
   "HeatGenerated" : 0,
   "ArmorDamageModifier" : 1, "ISDamageModifier" : 1, "CriticalDamageModifier" : 1
}
```

**Steal (the same 3 lessons the unit arc adopted):**
1. **String-id reference graph, never index.** Every join is a greppable string: `Id`, `AmmoCategory`↔`ammoCategoryID`, `WeaponEffectID`, `PrefabIdentifier`, `Icon`. Kills every MC2 index disease at the root.
2. **`Category` is a first-class mount-class field** (`Ballistic|Energy|Missile|AntiPersonnel`) that MUST match a chassis location's `Hardpoint.WeaponMount` — validatable at author time. This is exactly `WeaponTag` ↔ `acceptedTags`.
3. **Weapon vs Ammunition split.** Ammo is its own Component with `ammoCategoryID`; the weapon names a category, not a row. Replaces `ammoMasterId` + `WEAPON_AMMO_OFFSET` arithmetic.

---

## 3. Proposed `WeaponDefinition` facet model (MC2-shaped, BT2018-familiar)

A modder-facing `WeaponDefinition` that *references* facets/vocabularies (not one blob), keyed by a namespaced string id (`core:ac20`, `myMod:heavy_ppc`). Existing `MasterComponent` becomes an **adapter** synthesized from (or shadowing) the definition.

| Facet | Fields | MC2 legacy source | BT2018 analog |
|---|---|---|---|
| **Identity** | `weaponDefId` (`namespace:name`), display `name`, `abbreviation` | `masterID` (index!) + `cLoadString(NAME_START+id)` | `Description.Id` / `Name` / `UIName` |
| **Category / mount tag** | `category: WeaponTag` (`WTAG_ENERGY/BALLISTIC/MISSILE/ANTIPERSONNEL/AMS`) | `Type` string → FORM enum (discarded) | `Category` |
| **Combat stats** | `damage`, `heat`, `recycleTime`, `range` (bucket now → real `RangeSplit` later), `flags` (streak/inferno/LBX/artillery), `CV/BR`, `resourcePoints` | `stats.weapon.*` union (`cmponent.h:128-139`) | `Damage`,`HeatGenerated`,`RefireModifier`,`RangeSplit`,`Min/MaxRange` |
| **Ammo linkage** | `ammoCategory` (string id, e.g. `core:ammo_ac5`), `ammoType` (SRM/LRM/ST/LIMITED/NONE), `ammoPerShot` | `ammoMasterId` (index) + `WEAPON_AMMO_OFFSET=65` + `WeaponAmmoType` | `AmmoCategory` ↔ `ammoCategoryID` |
| **Fit / size** | `critSlots` (`size`), `tonnage`, `health`, `disableLevel`, allowed damage-locations (reuse `DamageLocationId`) | `size`,`tonnage`,`health`,`criticalSpacesReq[8]` grid | `InventorySize`,`Tonnage`,`AllowedLocations` |
| **FX / muzzle hooks** | `weaponEffectId` (string), `muzzleSocket` (reuse `HardpointProfile.muzzle`) | `specialEffect` (raw char id) | `WeaponEffectID`, `PrefabIdentifier` |
| **UI** | `iconId` (string), `uiName`, mechpedia id | `Logistics Icon`/`Icon X,Y` atlas + IDS number | `Description.Icon`/`UIName` |
| **Usage / tech** | `usableBy` (mech/vehicle), `techBase` (IS/Clan), `tags[]` | `uses`,`techBase` bitfields | `ComponentTags`, `AllowedLocations` |

**The join to the unit arc (already built):** `WeaponDefinition.category (WeaponTag)` is exactly the value `hardpointAcceptsWeapon(HardpointProfile, WeaponTag)` (`hardpoint.h:158`) has been waiting for. Today `acceptedTags` is authored on the hardpoint but no weapon carries a tag to test — this arc supplies the weapon side. That enables the future **HARDPOINT-ENFORCEMENT-1**: mechlab rejects `myMod:ac20` (WTAG_BALLISTIC) on a `RightTorso_Energy_1` hardpoint (`acceptedTags = {WTAG_ENERGY}`), author-time and validatable — the report-first→enforce-later order from the unit roadmap §100-101.

---

## 4. Proposed slice sequence (mirrors the unit arc; each = gated + legacy-fallback + stock-parity)

Gate: everything new behind **`MC2_WEAPON_DEFINITION_DATA`** (default OFF; gate-OFF = byte-parity). Offline-first: pure logic into `tests/unit/` doctest before engine glue. Standard SLICE-DISPATCH CONTRACT (unit roadmap §114-122) applies verbatim.

**Phase 0 — WEAPONDEF-OFFLINE-HARNESS-1.** Extend `tests/unit/` (standalone `build64-tests`, GL/IO-free) with a `WeaponDefinition` POD + `weaponDefValidate()` red/green cases *before* any engine touch. Reuse `hardpoint.h` `WeaponTag`/`hardpointAcceptsWeapon` directly.

1. **WEAPONDEF-SCHEMA-1** — define `WeaponDefinition` POD facet (fields §3) + `WeaponCategory ≡ WeaponTag` + `WeaponDefId` (namespaced string, same `HardpointId` hash pattern, `hardpoint.h:83`). Pure schema + doctest. No behavior change.
2. **WEAPONDEF-REGISTRY-1** — an additive-merge registry `WeaponDefId → WeaponDefinition` (mirror the loadout/anim override registries the unit arc used). Populated optionally from `.fit` blocks / sidecar files; empty = legacy.
3. **WEAPONDEF-TYPELOAD-READ-1** — optional read at the ONE type-load seam (`MasterComponent::loadMasterList`, `cmponent.cpp:630`): when a `WeaponDefinition` exists for a row's identity, populate the `MasterComponent` weapon fields from it; else legacy `initEXCEL`. Gate-OFF = current `initEXCEL` unchanged. **This is the strangler intercept.**
4. **WEAPON-CATEGORY-TAG-1** — surface `MasterComponent → WeaponTag` (from FORM string / definition `category`), so `hardpointAcceptsWeapon` finally has a weapon side. Report-only: validator gathers `(loadout mount, hardpoint acceptedTags, weapon category)` triples and reports mismatches (`UPV_HARDPOINT_WEAPON_CLASS_REJECTED` already exists in `code/unit_profile_validator.h`).
5. **WEAPONDEF-VALIDATOR-1** — load-time modder-term rejection: unknown ammo category, category not in any accepting hardpoint, missing icon/FX id, crit-slots exceed location, dangling `ammoCategory`. Reuse the unit `unit_profile_validator.h` framework + finding-code style.
6. **AMMO-CATEGORY-LINK-1** — named `ammoCategory` string replaces `ammoMasterId`/`WEAPON_AMMO_OFFSET` arithmetic; legacy index fallback when unset.
7. **LEGACY-WEAPON-CONVERTER-1** — offline tool (extend `tools/unit_converter/`) reading stock `compbas.csv` → emit sidecar `generated/weapons/core/<weapon>.fit` `WeaponDefinition` files. Do NOT change runtime source of truth. Run validator on output.
8. **WEAPONDEF-SHADOW-COMPARE-1** — for every stock weapon row, diff legacy-`MasterComponent` runtime data vs generated `WeaponDefinition` across all facets (damage/heat/recycle/range/ammo/flags/category/crit-grid/icon/FX). Acceptance: stock component count matches · all generated defs validate · semantic diff zero (or waived) · mission + logistics smoke parity clean. Extend `tools/unit_converter/shadow_compare.py`.
9. **Source gate `MC2_WEAPON_DEFINITION_SOURCE=legacy|generated`** — default **legacy**; flip only when shadow-compare is boring. Middle state (allowed earlier): **new mod weapons use native `WeaponDefinition` while stock stays legacy/synthesized.**
10. **WEAPON-MODLINT-1** — add weapon validation to `tools/mc2_modlint/` (`mc2_modlint --weapon mods/x/weapons/y.fit` → modder-term errors) — reuse the CLI/report framework wholesale.

Cross-cutting later: **HARDPOINT-ENFORCEMENT-1** (make typed hardpoints actually *restrict* mounting; gated, after def format authoritative — never enforce on legacy stock loadouts), and folding weapon defs into the top-level `UnitDefinition` package (loadout items reference `WeaponDefId`, not `masterID`).

---

## 5. Strangler seams (where a `WeaponDefinition` intercepts, with legacy fallback)

| Seam | File:line | Intercept | Legacy fallback |
|---|---|---|---|
| **Component master load** | `MasterComponent::loadMasterList` `cmponent.cpp:630`; callers `mission.cpp:2451`, `saveload.cpp:846` | before/after per-row `initEXCEL`, overlay registry `WeaponDefinition` onto the `MasterComponent` | gate-OFF → untouched `initEXCEL` |
| **Row parse** | `MasterComponent::initEXCEL` `cmponent.cpp:87-264` (weapon branch `228-241`) | if a def exists for the row identity, populate weapon stats from it instead of `strtok` columns | no def → current positional parse |
| **Category surfacing** | FORM match `cmponent.cpp:105-107` + `ComponentFormString[]` `cmponent.cpp:25-47` | map FORM/`Type` string → `WeaponTag`, store on the component | derive tag from FORM only (no restriction) |
| **Loadout resolution** | `InventoryItem.masterID` `mover.h:415`; the pre-drilled `hardpointId` `mover.h:439` | let a loadout name a `WeaponDefId`; resolve to a `masterID`/def | `masterID` raw index (stock CSV loadouts) |
| **Ammo linkage** | `stats.weapon.ammoMasterId` `cmponent.cpp:238`; `WEAPON_AMMO_OFFSET` `cmponent.h:50` | resolve named `ammoCategory` → component | raw `ammoMasterId` / offset arithmetic |
| **Name/UI** | `cLoadString(NAME_START+masterID)` `cmponent.cpp:100-101`; `Logistics Icon`/`Icon X,Y` cols | named `uiName`/`iconId` from def | IDS-number + atlas index |

Same discipline as the unit arc: add gated registry → read optional facet → validate → **fall back to legacy CSV/index** → replace ONE index seam at a time. Redline: do NOT rewrite `initEXCEL`, mechlab, or the combat/ammo runtime in one slice.

---

## TOP 3 REUSE OPPORTUNITIES from the unit arc

1. **`WeaponTag` vocabulary + `hardpointAcceptsWeapon()` are already built** (`code/hardpoint.h:36-42, 158`) — `WTAG_ENERGY/BALLISTIC/MISSILE/ANTIPERSONNEL/AMS` map 1:1 to the CSV `EnergyWeapon/BallisticWeapon/MissileWeapon` FORM strings AND to BT2018 `Category`. This arc only has to attach a tag to the *weapon* side; the join, the mask, and the accept-test already exist and are doctested. This is the single biggest head-start — the weapon arc is literally "supply the other half of a join the unit arc left half-built."

2. **Converter + shadow-compare + modlint tooling is generic and already parameterized** (`tools/unit_converter/` incl. `extract.py`/`emit.py`/`shadow_compare.py`, `tools/mc2_modlint/` with `modlint_input.h`/`modlint_report.h`/`modlint_registry.h`). LEGACY-WEAPON-CONVERTER-1 / SHADOW-COMPARE-1 / WEAPON-MODLINT-1 extend these rather than greenfielding — the "generate → validate → shadow-compare → gate source-of-truth → flip when boring" staging is proven and its acceptance bar is copyable.

3. **Validator framework + finding-code + gate/parity discipline** (`code/unit_profile_validator.h`, incl. `UPV_HARDPOINT_WEAPON_CLASS_REJECTED`) plus the whole STANDARD SLICE-DISPATCH CONTRACT (gate `MC2_*_DATA` default-OFF, offline-doctest-first in `tests/unit/`, additive-merge registries, per-slice single-mission parity, banked tier1 at arc end). The weapon arc is a re-application of an established playbook, not a new methodology.
