# examples/mods — worked example checkbox mods (EXAMPLE-MODS-1)

Five small **real** mods that exercise the shipped mod system (`mod.json` /
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
| `sample_equipment` | a genuinely NEW weapon (`sample:hot_medium_laser`) that goes LIVE via EQUIP-ADD-FILL-1: explicit `LegacyMasterId=40` fills a FREE compbas ordinal at load (both consumers: in-sim MasterComponent + mechlab row), `Enabled=TRUE` rides the S1 enable wire; gates `MC2_UNIT_PROFILE_DATA=1 MC2_EQUIPMENT_ADD=1`; modlint-clean |
| `sample_variant`   | a new namespaced unit variant (`sample:atlas_laserboat`) on a stock chassis, 3-layer loadout join, modlint-clean |
| `sample_new_variant` | the LEGACY zero-code variant ADD (UNIT-VARIANT-ADD-1 / S4): whole-file-shadows `data/objects/atlas.csv` with an appended 4th variant block at row 314 = 23+97×3 (`Atlas AS7-X Sampler`, distinct loadout); no gate, no engine change; `mc2_modlint --mod` now lints shadowed chassis CSVs (VARC_ rules: stride, unreachable blocks, duplicate names, loadout refs, truncation) |
| `sample_bad_mod`   | INTENTIONAL failures the validator catches with modder-first messages (exit 1) |
| `core_unlocks`     | re-surface TWO dormant stock weapons (Clan Gauss Rifle 113, ER PPC 146) via the CLEAN enabled->purchase wire (EQUIP-ENABLE-WIRE-1: tiny `[Equipment] b Enabled=TRUE` def overrides, gate `MC2_UNIT_PROFILE_DATA=1`; enable-only, purchase.fit stays authoritative) |

## Required env gates per mod (what makes content go live)

All gates default OFF → with them unset the game is byte-identical stock.

| mod | gates to set | notes |
|---|---|---|
| `sample_equipment` | `MC2_UNIT_PROFILE_DATA=1` `MC2_EQUIPMENT_ADD=1` | ADD-fill reads the `equipmentdef.fit` sidecar directly; **`MC2_EQUIPMENT_DEFINITION_SOURCE` is NOT needed** (that gate is only for the overlay/re-stat path) |
| `core_unlocks` | `MC2_UNIT_PROFILE_DATA=1` | enable-only wire; no `MC2_EQUIPMENT_ADD` (rows already populated) |
| `sample_new_variant` | *(none)* | pure data CSV-shadow ADD — zero engine gate |
| `sample_variant` | `MC2_UNIT_PROFILE_DATA=1` (`MC2_UNIT_DEFINITION_SOURCE=generated` for the facet overlay) | overlay-only, NOT live-spawned — see the variant-path note below |
| `sample_bad_mod` | *(offline validator only)* | never loaded live; it is a modlint fixture |

Activate any mod with EITHER the launcher / `MC2_ACTIVE_MOD=<id>` OR
`MC2_MOD_DEPS=<abs path>` (alternative paths — do not set both).

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

TWO rungs of the clean wire have now landed on top of the overlay:

* **S1 EQUIP-ENABLE-WIRE-1** (`core_unlocks`): a POPULATED stock row becomes
  purchasable via `[Equipment] b Enabled=TRUE` in its def — enable-only, gate
  `MC2_UNIT_PROFILE_DATA=1`.
* **S2 EQUIP-ADD-FILL-1** (`sample_equipment`): a NEW equipment def with an
  EXPLICIT `LegacyMasterId` naming an UNDEFINED compbas ordinal is materialized
  at load as a live `MasterComponent` row AND a mechlab `LogisticsComponent`
  (ONE synthesized compbas line feeds BOTH consumers; `[Presentation] Icon` is
  mandatory or the mechlab consumer drops the row). Conflicts are first-wins +
  `[EQUIP-ADD]` findings, never silent; populated stock ordinals are never
  filled; NO auto-assign of ordinals. Gates `MC2_UNIT_PROFILE_DATA=1` +
  `MC2_EQUIPMENT_ADD=1`, both default OFF — gates off is byte-identical.

New unit VARIANT ADD is now covered by `sample_new_variant` (S4, legacy zero-code path + `mc2_modlint --mod` chassis-CSV linting); new CHASSIS ADD remains future work (S5); the in-mission info window's
`IDS_COMP_ABBR0+masterID` label for new ids is the S3 UI-name rung.

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

## Load-order conflict report (MOD-LOAD-ORDER-CONFLICT-REPORT-1)

Conflicts are NEVER silently resolved. At game load (only when mods are
active — zero mods produce zero new output), the engine prints one
`[mod] conflict: [MODCONFLICT_*] ...` line per finding (capped, plus a summary
line):

* `MODCONFLICT_DUPLICATE_DEF_ID` — two mods ship the same `data/defs/...` key
  (same qualified id); the higher-priority mod wins and both are named.
* `MODCONFLICT_DUPLICATE_FILE` — two mods ship the same non-def data file.
* `MODCONFLICT_RESERVED_CORE_NAMESPACE` — a mod ships a def under the reserved
  `core:` namespace (`data/defs/{units,equipment}/core/...`). That replaces
  STOCK content — legitimate only as a deliberate override.
* `MODCONFLICT_STOCK_OVERRIDE` — a mod def shadows a stock def (one visibility
  line per override).

The same checks run offline (same pure classifier, `mclib/mod_conflict.h`):

```
mc2_modlint --mod mods/<active> --mod mods/<dep0> ... [--stock-root <install>]
```

Pass mod dirs highest-priority-first (active mod, then deps in `MC2_MOD_DEPS`
order) — the same precedence the engine loader uses. `--stock-root` (the game
install root containing the loose `data/` tree) enables the stock-override
check; without it only duplicate + reserved-namespace checks run. Exit 1 when
findings are present.
