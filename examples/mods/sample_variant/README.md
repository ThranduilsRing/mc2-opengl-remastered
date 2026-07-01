# sample_variant — a new unit variant on a stock chassis

Adds `sample:atlas_laserboat`, an energy-heavy variant on the EXISTING stock
Atlas chassis (`core:atlas`) — no new art. Its loadout mounts
`sample:hot_medium_laser` (from the `sample_equipment` mod) plus stock lasers.

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
