# core_unlocks — re-surface ONE dormant stock component (honest v0 STUB)

## Goal

Enable one piece of dormant stock content — a component that exists in the data
but is not offered for purchase — so it shows up in the logistics/purchase UI.

## Why this is a STUB (the honest part)

There is **no clean `enabled`-flag → purchase wire** in the engine yet. The E2
equipment registry carries an `enabled` bit
(`code/equipment_registry_engine.cpp`), but the purchase/availability layer
(`code/logisticscomponent.cpp`) does not consume it — it reads id/stats/icon
straight off `MasterComponent` rows. So "dormant" is not a flag you flip; it is a
row that the purchase/availability catalog does not surface.

Re-surfacing therefore rides the **LEGACY whole-file overlay path** (the same
mechanism `mods/magic-ballistic-weapons` uses): a mod ships its own copy of a
stock data file at `data/objects/compbas.csv`, and the `g_modIndex` file-shadow
serves the mod's copy at mod precedence (see `mclib/file.cpp`,
`.claude/MOD-SYSTEM-RECON-1.md`). Because the overlay is **whole-file, not a
row-merge**, the override must be a COMPLETE compbas.csv with the one dormant row
edited to be purchasable — you cannot ship just the delta row.

We do **not** vendor a full stock `compbas.csv` into this example (it is a large
stock asset, and the task scope is to author mods as source without editing
mc2srcdata). Hence this mod is shipped as a documented stub.

## How to produce the real one-row override (for a working install)

1. Copy the stock `data/objects/compbas.csv` out of your install.
2. Find the dormant component's row (e.g. a magic-ballistic component that ships
   present-but-not-purchasable). Edit only that row's availability/cost fields so
   the purchase catalog offers it — matching how `mods/magic-ballistic-weapons`
   re-adds rows.
3. Place the edited file at `mods/core_unlocks/data/objects/compbas.csv`.
4. Drop `mods/core_unlocks/` into your staging install's `mods/` folder and tick
   it in the launcher (or add `core_unlocks` to `MC2_MOD_DEPS`). The file-shadow
   overlay serves your compbas.csv at mod precedence; the dormant component is now
   purchasable.

## The clean follow-on (not built)

The MODDABILITY-phase target is a real `enabled` → purchase wire: a mod overrides
an equipment def's `enabled` flag under `data/defs/equipment/…`, the purchase
catalog consults the E2 registry's `enabled` bit, and re-surfacing needs no
whole-file CSV override. That path replaces this legacy stub and is tracked in
`docs/superpowers/plans/2026-07-01-unit-profile-arc-roadmap.md` (MODDABILITY
phase). Until then, this stub documents the only honest v0 available.
