# sample_equipment — a new EquipmentDefinition

Adds `sample:hot_medium_laser`, a stock Medium Laser (`core:laser`, legacy
masterId 156) cloned with a hotter/harder profile (damage 2.5→3.2, heat 3.0→4.5).

## Files

* `data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit` — the generated
  EquipmentDefinition (same block/token format the source-flip overlay reads;
  format authority: `tools/unit_converter/equipment_emit.py`).
* `modlint/hot_medium_laser.facts.json` — the facts the offline validator checks.

## Proves

* a namespaced **qualified id** (`sample:…`) so the def ADDS rather than clobbers
  the stock laser (whole-file overlay is per-path; a namespaced dir is a new key);
* `LegacyMasterId` kept in 0..254 (200) — an addressable alias that does NOT
  trigger the E4 id-widen and, being namespaced, never aliases a real stock row;
* **modlint-clean**: `mc2_modlint --equipment modlint/hot_medium_laser.facts.json`
  → exit 0.

## Verify

```
mc2_modlint --equipment examples/mods/sample_equipment/modlint/hot_medium_laser.facts.json
```

See `../README.md` for the honest note on the overlay-not-add source-flip: a NEW
namespaced weapon is validated offline here; a live in-mission stat overlay is
supported only for an EXISTING stock `core:<slug>` row.
