# sample_bad_mod — intentional failures modlint must catch

This mod is deliberately broken so `mc2_modlint` demonstrates its **modder-first
error surface**: each finding names the id and the file
(`unit <id> [<file>]: …`), so a modder knows WHICH mod / WHICH file / WHICH id /
WHICH field. Both facts files exit 1.

## `modlint/bad_unit.facts.json` (`sample:broken_striker`)

| authored fault | finding token |
|---|---|
| Energy weapon into a Ballistic-only hardpoint | `UPV_HARDPOINT_WEAPON_CLASS_REJECTED` |
| declared hardpoint socket absent on model | `UPV_HARDPOINT_SOCKET_MISSING` |
| declared presentation socket absent on model | `UPV_HARDPOINT_SOCKET_MISSING` |
| no gesture in the anim fallback chain | `UPV_ANIMATION_FALLBACK_MISSING` |
| missing icon / UI asset does not resolve | `UPV_UI_ASSET_MISSING` |

## `modlint/bad_equipment.facts.json` (`sample:broken_laser`)

| authored fault | finding token |
|---|---|
| empty / unbound fire executor | `EQV_WEAPON_NO_FIRE_EXECUTOR` |
| no authored mount-class category | `EQV_WEAPON_NO_CATEGORY` |
| consumes ammo but names no ammo category | `EQV_AMMO_CATEGORY_MISSING` |
| ranges violate short≤med≤long | `EQV_WEAPON_RANGE_INVALID` |
| claims a presentation event with no asset | `EQV_PRESENTATION_ASSET_MISSING` |

## Bad namespace

A reserved (`core:`) or malformed namespace is rejected by the qualified-id
layer, not the fact validator — see `Q.is_mod_authorable` in
`tools/unit_converter/qualified_id.py` and its assertion in
`test_example_mods_roundtrip.py::test_authored_ids_are_mod_authorable`.

## Verify

```
mc2_modlint --unit      examples/mods/sample_bad_mod/modlint/bad_unit.facts.json      # exit 1
mc2_modlint --equipment examples/mods/sample_bad_mod/modlint/bad_equipment.facts.json # exit 1
```
