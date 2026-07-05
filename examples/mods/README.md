# examples/mods — worked example checkbox mods (EXAMPLE-MODS-1)

Four small **real** mods that exercise the shipped mod system (`mod.json` /
`MC2_MOD_DEPS` loader) + the generated-definition source-flip
(`data/defs/{units,equipment}/…`, gates `MC2_UNIT_DEFINITION_SOURCE` /
`MC2_EQUIPMENT_DEFINITION_SOURCE=generated`) + the offline validator
(`tools/mc2_modlint`).

These live in `examples/mods/` as **source in the repo**. They are NOT deployed
into `mc2srcdata`. To try one, copy `examples/mods/<id>/` into a staging
install's `mods/` folder and tick it in the launcher (or point `MC2_MOD_DEPS` at
the folder). STOCK content is the base; every mod here ADDs or overrides on top —
with the mods absent, the game is byte-identical.

| mod | proves |
|---|---|
| `sample_equipment` | a new `EquipmentDefinition` (qualified id `sample:hot_medium_laser`), generated-source `.fit`, modlint-clean |
| `sample_variant`   | a new namespaced unit variant (`sample:atlas_laserboat`) on a stock chassis, 3-layer loadout join, modlint-clean |
| `sample_bad_mod`   | INTENTIONAL failures the validator catches with modder-first messages (exit 1) |
| `core_unlocks`     | re-surface ONE dormant stock component via the LEGACY compbas overlay path (honest v0; see its README) |

## What "modlint-clean" means here (honest scope)

`mc2_modlint` validates a **facts-JSON** (the pure coherence check the engine
runs), not the whole `.fit` byte stream. Each good mod therefore ships BOTH:

* the real generated def (`data/defs/equipment/…/equipmentdef.fit` or
  `data/defs/units/…/unit.fit`) — what the engine overlay reads, and
* a `modlint/*.facts.json` companion — what the offline validator reads.

The facts-JSON is the def's facts restated in the modlint schema (see the header
comment in `tools/mc2_modlint/modlint_input.h` / `modlint_equipment_input.h`).

## Honest limitation of the source-flip roundtrip (READ THIS)

The shipped source-flip is an **OVERLAY, not an ADD**:

* `overlayGeneratedEquipmentStatsIfEnabled()` (code/equipment_registry_engine.cpp)
  walks the already-loaded `masterList`, derives each row's `core:<slug>` path,
  and overwrites only that row's green weapon stats — keyed by `LegacyMasterId`.
* `sourceGeneratedChassisFacets()` / `sourceGeneratedVariantJump()`
  (code/unitprofile.cpp, code/mech.cpp) overlay facets onto units the mission
  ALREADY spawns, keyed by chassis stem + variant name.

Consequence: a **new `sample:`-namespaced** def is not auto-instantiated by the
current engine — nothing walks it, because there is no stock `masterList` row or
spawned unit whose derived `core:<slug>` path points at it. So the load proof for
a genuinely-new mod def is the **offline modlint roundtrip** (the def's facts
resolve and validate), NOT a live in-mission spawn. The clean
"enabled-flag → purchase → spawn" wire that would make a new namespaced def
purchasable/spawnable is a documented follow-on (see the roadmap MODDABILITY
phase and `core_unlocks/README.md`).

A mod that RE-STATS an EXISTING stock weapon (reusing the stock `core:<slug>`
path + matching `LegacyMasterId`) IS picked up live by the overlay — that is the
supported live path today, and `core_unlocks` documents the legacy variant of it.

## mod.json — the mod package manifest (MOD-PACKAGE-MANIFEST-1)

Every mod ships one `mod.json` at its root (`mods/<id>/mod.json`). This is the
ONE manifest — the engine loader (`mclib/file.cpp` `ReadModJson()`), the
launcher (`tools/mc2_launcher/mc2_launcher.cpp`), and the packer
(`tools/mc2mod/mc2mod.py`) all read the same file. Validate it offline:

```
mc2_modlint --manifest mods/<id>/mod.json
```

| field | required | consumer | rule |
|---|---|---|---|
| `schema` | yes | packer + tooling | must be `"mc2-mod/1"` |
| `id` | yes | engine loader (namespace + dep resolution), launcher | lowercase token `^[a-z0-9][a-z0-9._-]*$`; matches the install folder name |
| `name` | yes | launcher (display name) | non-empty string |
| `version` | yes | launcher/packer | dotted digits, e.g. `"1.0.0"` |
| `author` | no | launcher tooltip / conflict-report attribution (near-term) | free string |
| `type` | yes | launcher bucketing | `"assets"` (stackable checkbox add-on), `"campaign"`, or `"dependency"` |
| `dependencies` | yes (may be `[]`) | engine loader (load order: active mod > dep[0] > … > base) | array of mod id tokens; no self-dep, no duplicates |
| `description` | yes | launcher description pane | non-empty string |
| `radioGroup` | no | launcher (mutually-exclusive radio bank) | token |
| `env` | no | launcher (child env vars when mod active) | flat `{ "KEY": "VALUE" }` object |

Missing/malformed fields produce `MODMAN_*` findings (stable grep-able tokens,
exit 1) so CI and modders catch manifest problems without launching the game.
The `package.json` inside a packed `.mc2mod` zip is packer-GENERATED
(`mc2-package/1`, with hashes) — never hand-author that one.
