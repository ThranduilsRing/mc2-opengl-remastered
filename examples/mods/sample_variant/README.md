# sample_variant — generated unit.fit (OVERLAY, not yet live-spawnable)

> ## ⚠ STATUS: overlay-only — does NOT spawn a new variant in-game today
>
> This mod ships a **generated `unit.fit`** under `data/defs/units/`. The shipped
> engine path for generated unit defs is an **OVERLAY**
> (`sourceGeneratedVariantJump` / `sourceGeneratedChassisFacets`,
> `code/unitprofile_fit.cpp` / `code/mech.cpp`): it patches facets onto units a
> mission **already spawns**, keyed by chassis stem + variant name. It does
> **NOT** add a new selectable/spawnable variant. Load this mod and you will see
> no new mech in the bay list.
>
> **Want a variant that actually appears and is purchasable?** Use
> **`sample_new_variant`** (the S4 legacy CSV-shadow ADD path) instead — that one
> spawns. Registry-side variant ADD (synthesizing a LogisticsVariant from a
> generated unit def, no whole-CSV shadow) is the documented CLEAN follow-on and
> is not implemented yet.
>
> What this example DOES prove: the generated unit-def format, the namespaced
> unit id, the 3-layer loadout join shape, and modlint-clean validation — a
> useful authoring/round-trip reference, just not a live spawn.

Adds `sample:atlas_laserboat`, an energy-heavy variant on the EXISTING stock
Atlas chassis (`core:atlas`) — no new art. Its loadout mounts
`sample:hot_medium_laser` (from the `sample_equipment` mod) plus stock lasers.

## Dependency on sample_equipment

The `[Loadout*]` blocks mount `sample:hot_medium_laser`, which is defined by the
`sample_equipment` mod — so `mod.json` declares `"dependencies":
["sample_equipment"]`. The engine loader honors that (load order: active mod >
dep[0] > … > base), so `sample_equipment` loads first and the id resolves. Note
this mount is **forward-looking**: because this variant is overlay-only (banner
above), the loadout is not consumed by a live spawn today — the declared
dependency states intent and keeps the example coherent for when the registry
variant-ADD path lands.

## Files

* `data/defs/units/sample/atlas/laserboat/unit.fit` — the generated mechdef.
  `[Variant]` + `[UnitProfile]` tokens match `populateProfileFromFit`; the
  `[Loadout*]` blocks are the forward-looking 3-layer loadout→hardpoint→socket
  join (format authority: `tools/unit_converter/emit.py`).
* `modlint/atlas_laserboat.facts.json` — the facts the offline validator checks.

## Proves

* a namespaced unit id on a stock chassis (references `core:atlas`);
* a **3-layer loadout join** (loadout item → hardpoint mount-class → model socket)
  where each Energy weapon lands in an Energy-accepting hardpoint at a valid mech
  damage location with a present socket;
* **modlint-clean**: `mc2_modlint --unit modlint/atlas_laserboat.facts.json`
  → exit 0.

## Verify

```
mc2_modlint --unit examples/mods/sample_variant/modlint/atlas_laserboat.facts.json
```
