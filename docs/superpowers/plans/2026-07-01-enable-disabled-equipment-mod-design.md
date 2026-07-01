# ENABLE-DISABLED-EQUIPMENT-MOD-1 — design

**Branch:** `claude/unit-profile-seam` (worktree `.claude/worktrees/unit-profile-seam`, local-only). **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3`.

## Goal

Ship a playable, checkbox-selectable example mod that enables a curated set of stock-disabled equipment, authored in the new `EquipmentDefinition` format (proving the moddability arc's authoring loop), while the actual runtime unlock goes through the **proven legacy `compbas.csv` overlay mechanism** already shipped by `mods/magic-ballistic-weapons/`. No firing-code changes, no new masterIDs, no `EquipmentRef` widening.

## Background / why this shape

- `LogisticsData::getAvailableComponents` (`code/logisticsdata.cpp:553`, filters on `bAvailable`) and `MasterComponent::initEXCEL` (`mclib/cmponent.cpp:87-114`) both parse the same `compbas.csv` `Type` column. Rows with `Type="Removed"` fail the `ComponentFormString[]` match in both parsers — `LogisticsComponent::init()` returns `-1` (rejected from the buy list) and `initEXCEL` bails early leaving stats zero/garbage (return value silently discarded by the caller, `cmponent.cpp:672`). There is **no separate purchase-catalog choke point** — fixing `Type` + stats in the shared CSV format fixes both simultaneously.
- The new-defs generated-source runtime path (`EquipmentRegistry`, `mech.cpp:1555/1580` loadout-id-alias) is only proven end-to-end for one weapon so far (E7/E8 "one-weapon-presentation-proof"). Routing this mod's 12 items through it would mean extending fire-executor coverage to untested weapon kinds — collides with the standing "no firing rewrite" redline.
- Therefore: **author** in the new format (satisfies "do it with the new defs"), **export** to the old proven format (satisfies "playable now, zero firing-code risk").
- `code/equipment_registry.h`'s `enabled` field is not consumed by gameplay anywhere (only a diagnostic log + a unit test read it). This mod does not attempt to make that field runtime-authoritative — see "Enabled semantics" below.

## Scope

**In:** un-disable 12 existing `Removed` rows in stock `compbas.csv`:

| masterID | Name |
|---|---|
| 8 | Double Heat Sink |
| 9 | Clan Double Heat Sink |
| 105 | Machine Gun |
| 107 | Light LBX Autocannon |
| 116 | Clan Light LBX |
| 117 | Clan LBX Autocannon |
| 118 | Clan Heavy LBX Autocannon |
| 121 | LR Missile/15 |
| 122 | LR Missile/20 |
| 126 | Heavy Thunderbolt |
| 2 | XL Fusion Engine |
| 3 | Clan XL Fusion Engine |

(12 rows — "engines" + "LRM15/20" + the ballistic-weapons set named in the original ask, minus SRM6 variants.)

**Out (phase 2, later milestone):** SRM6 / Clan SRM6 / Clan Streak SRM6 — these don't exist as rows in stock `compbas.csv` at all (confirmed: only `SRM2 Pack` (124) and `Clan Streak SRM2 Pack` (135) are live). Adding them requires brand-new masterIDs, which risks the documented 254-row / `0xff`-sentinel `EquipmentRef` widen redline. Belongs to a follow-on (`NEW-EQUIPMENT-ROW-MOD-1`, after `INVENTORY-EQUIPMENTREF-WIDEN=E4` if ever needed).

**Base CSV:** `mc2srcdata/objects/compbas.csv` (canonical stock source), not the packed `misc.fst` inside `mc2-win64-v0.3` — the FST extractor (`tools/mc2x_import/fst.py:91`) currently throws `ValueError: LZW: invalid code` on that archive (pre-existing, unrelated bug). This is safe because mod-file-shadow precedence is loose-file-beats-`.fst` (`mclib/file.cpp:69`) — the mod's full CSV supersedes whatever's packed, so correctness depends on completeness relative to canonical source, not byte-matching the current archive. The FST-extractor bug is flagged as a separate follow-on, not blocking here.

## Architecture

```
mc2srcdata/objects/compbas.csv (canonical base, 256 rows)
        +
curated EquipmentDefinition sidecars (new-defs authoring format, hand-authored,
  values ported from mods/magic-ballistic-weapons's already-shipped rebalance)
        ↓
  equipment_export_legacy.py (new, offline, one-way)
        ↓
mods/enable-disabled-equipment/data/objects/compbas.csv
  (FULL merged CSV — all 256 rows, only the 12 curated rows patched)
        ↓
player checks "enable-disabled-equipment" in launcher → MC2_MOD_DEPS includes folder
        ↓
mclib/file.cpp mod-shadow resolves data/objects/compbas.csv to the mod's full file
        ↓
MasterComponent::initEXCEL (masterList stats) + LogisticsComponent::init /
  getAvailableComponents (mechlab buy-list) both parse the real Type + stats
        ↓
item is purchasable AND usable, through the existing/proven legacy path
```

## Components

1. **12 `equipmentdef.fit` sidecars** — `mods/enable-disabled-equipment/data/defs/equipment/core/<name>/equipmentdef.fit`, new-defs schema (same shape as `examples/mods/sample_equipment/data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit`). These are the **authoring source of truth** and a validation record — not runtime-read in phase 1 (no source-flip env forcing; see "Launcher" below).
2. **`tools/unit_converter/equipment_export_legacy.py`** (new) — reads base `compbas.csv`, for each curated sidecar looks up its `LegacyMasterId` row, validates preconditions (see Validation), patches `Type` + stat columns from the sidecar's values, writes the **full merged CSV** (not a partial/12-row file) to the mod's `data/objects/compbas.csv`. Also emits an export report (base file hash, patched row list, untouched row count) alongside.
3. **`mods/enable-disabled-equipment/mod.json`** — `schema:"mc2-mod/1"`, `id:"enable-disabled-equipment"`, `type:"assets"` (same shape as `sample_equipment`/`magic-ballistic-weapons`).
4. **No bundled art** in phase 1 — matches `magic-ballistic-weapons` precedent (its README tells users to separately source TGAs); noted as a follow-on in this mod's README, not blocking.
5. **No launcher change in phase 1** — dropped the earlier idea of auto-forcing `MC2_UNIT_PROFILE_DATA`/`*_DEFINITION_SOURCE=generated` when a mod ships `data/defs/`. Too magical (some mods may ship defs as authoring/docs/partial-override content with no runtime intent). This mod's actual unlock is 100% legacy-overlay-driven and needs no source-flip envs to work. Handoff item "generated-source toggle in launcher" stays open/deferred.

## Enabled semantics (be honest about scope)

The engine does not currently consult `EquipmentRegistry.enabled` for anything gameplay-facing. This mod does **not** change that. For this mod:

- **Export-time `enabled`:** whether `equipment_export_legacy.py` includes/patches a given curated row (a build-time concept, controlled by which sidecars exist / are marked for export).
- **Runtime "enabled":** unchanged mechanism — a `compbas.csv` row with a real `Type` and real stats is usable; one with `Type="Removed"` isn't. This was always true; the mod doesn't invent new runtime behavior, it just produces a correct row the same way a hand-edited overlay would.

## Validation (exporter fails loud on)

- `LegacyMasterId` from a sidecar not found in base `compbas.csv`.
- Base row's current `Type` is not `"Removed"` (unless an explicit `--allow-existing` override is passed) — guards phase-2 SRM6 work from silently misfiring on rows that aren't actually dormant.
- Patched `Type` value not a valid `ComponentFormString` entry.
- Required stat field missing for the target kind (e.g. weapon needs range/damage/heat/recycle).
- Output row count differs from base row count (must stay 256 — proves this is a patch, not a reshape).
- Any masterID outside the curated 12 changed from base (proves untouched rows really are untouched).

## Testing

**Offline:**
- Exporter run produces a full 256-row merged CSV; diff against base shows only the 12 curated rows changed.
- Curated rows no longer `Type=Removed`.
- Numeric sanity check: curated row stats compared against `mods/magic-ballistic-weapons/data/objects/compbas.csv`'s equivalent rows (not byte-identical — sanity, since that mod also rebalances) for the 9 overlapping ballistic/missile items (the mechanism doesn't cover engines, which magic-ballistic-weapons doesn't touch).
- modlint passes on the mod package.
- Generated-defs roundtrip: sidecar → exporter → CSV → (optionally) re-run stock `equipment_converter.py`-style extraction on the patched row and confirm it no longer waives as `Removed`.

**In-engine:**
- Deploy to `mc2-win64-v0.3` via `scripts/deploy_payload.py`.
- Check "enable-disabled-equipment" in launcher, launch.
- Stretch: a `LOGISTICS-COMPONENT-CATALOG-DUMP-1` diagnostic dump of `getAvailableComponents()` output, to confirm the 12 items appear, rather than relying only on manual mechlab visual inspection.
- Manual: mechlab shows the 12 items as purchasable/mountable; mount one, confirm non-zero/non-garbage stats.
- tier1 smoke (`mc2_01`, `mc2_24`, 30s, exit 0) with the mod active.
- Regression: same tier1 smoke with the mod unchecked — confirms zero behavior change when off (mod system is inherently additive/opt-in, but worth one explicit check).

## Explicit non-goals (redline compliance)

- No firing-code changes.
- No new masterIDs / no new `compbas.csv` rows (12 patches, 256-row count preserved).
- No `EquipmentRef` widening.
- No hardpoint enforcement.
- No SRM6/new-content rows (phase 2).
- No forced generated-source env flips from the launcher.
