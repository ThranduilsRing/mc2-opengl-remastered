# unit_converter — LEGACY-UNIT-CONVERTER-1

Offline, deterministic Python tool (Phase-3 opener of the unit-profile arc).
Reads **read-only** stock MC2 unit source data and **emits sidecar
`UnitDefinition` `.fit` files** in the exact block/token format the Phase-2
engine loaders already parse. It does **not** touch the engine runtime or the
stock source-of-truth; generated files land under a new `--out` dir.

It is a *converter* (emits files), distinct from the in-memory
`LEGACY-UNIT-DEFINITION-SYNTHESIS-1` runtime facade.

## Run

```bat
py -3 tools/unit_converter/unit_converter.py ^
    --srcdata A:/Games/mc2-opengl-src/mc2srcdata ^
    --mech atlas --vehicle armoredcar --out generated/units [--json]
```

`--srcdata` defaults to the bundled fixtures (`tests/fixtures/mc2srcdata/`,
atlas + armoredcar + compbas) so it runs self-contained in a worktree without
the `mc2srcdata` sibling checkout. `--out` omitted → report only, no files.

## What it extracts (facets)

| Facet | Mech source | Vehicle source |
|---|---|---|
| Identity | chassis CSV `MechName` / first `VariantName` / `Tonnage` | `.fit [ObjectType] Name`, `CurTonnage` |
| Capabilities (jump) | installed JumpJet component (`compbas Type==JumpJet`) — NOT the mechlab AllowComponents flag | none |
| Locomotion | `MechWalker` | `Wheeled` (v0 default) |
| DamageSchema | `Mech8Part` | `VehicleFacings` from `[InternalStructure]` |
| Presentation | tgl INI `[TGLData]` model + `[WeaponNodeN]` sockets | tgl INI model (stock vehicles declare no weapon nodes) |
| Hardpoints | one per weapon socket; `HardpointId = <stem>_<tag>_<n>` (named, never indexed) | synthetic turret hardpoint |
| Loadout | **per-variant** `ItemNMasterId` weapon rows → weapon tag via compbas | `[Item:N] MasterID` weapon rows |
| UI | `LittleIcon`, `DescIndex`, `EncyclopediaID` | `IconIndex`, `DescIndex`, `EncyclopediaID` |

## LOADOUT-THREE-LAYER-1 (Phase 3): chassis / variant / instance

BT2018-shaped separation, mirrored from the roadmap:

- **ChassisProfile** = the shared frame (locations, damage schema, hardpoint
  mount-types, model/sockets, locomotion). Emitted **once** per chassis as
  `chassisdef.fit`. Hardpoint mount-types are **REPORT-only** — mounting is not
  restricted (enforcement is a deferred future slice `HARDPOINT-ENFORCEMENT-1`).
- **VariantLoadout** = the per-variant loadout + jump capability. The chassis CSV
  holds one `VariantInfo` block per variant at a 97-row stride; **every** variant
  now converts (was Prime-only) to one `mechdef` keyed
  `core:<chassis>_<variant-slug>` referencing its `ChassisID`. This is what fixes
  the Atlas-jump finding: Atlas Prime installs no jumpjet (jump FALSE), Renard's
  Atlas does (jump TRUE) — previously only Prime was read.
- **InstanceLoadout** = the runtime customized state (the engine's live
  `Mover::inventory[]`). Left as-is; named here, not reimplemented.

## Emitted files (under `<out>/core/<chassis>/`)

- **`chassisdef.fit`** (mech, one per chassis) → `[Chassis]` `ChassisID` +
  `[UnitProfile]` `Locomotion`/`DamageSchema` + shared `[Hardpoints]`.
- Per **mech variant** in `<chassis>/<variant-slug>/`:
  - **`unit.fit`** (mechdef) → `[Variant] ChassisID/VariantName/VariantIndex` +
    `[UnitProfile]` `b Jump=`, `st Locomotion=`, `st DamageSchema=`
    — matches `code/unitprofile_fit.cpp::populateProfileFromFit`.
  - **`presentation.fit`, `ui.fit`, `hardpoints.fit`, `loadout.fit`** as below.
- **Vehicles** (single loadout) keep the flat `<chassis>/` dir (no chassisdef).
- **`presentation.fit`** → `[PresentationSockets]` `l NumSockets=N` + `[SocketN] st SocketName=`.
- **`ui.fit`** → `[UIProfile]` `st Icon=`, `l NameID=`, `l EncyclopediaID=`.
- **`hardpoints.fit`, `loadout.fit`** → forward-looking HardpointId / Loadout
  blocks (no shipped engine reader yet; Phase-2 `HARDPOINT-ID-SCHEMA-1`).

## Variant-aware shadow-compare

The engine witness dump row (`code/unitprofile_fit.cpp`) now emits `variant` +
`variant_id` per mover. `shadow_compare.py` joins by **(chassis, variant)** so a
spawned variant diffs against the SAME converted variant instead of the chassis'
first (Prime) variant — resolving the Atlas false-mismatch at the root.

## Conversion report

Per-unit facet summary + Python-side coherence waivers (a preview of what the
C++ validator will confirm): hardpoint socket-name presence, weapon-tag vs
hardpoint binding, missing model. The real legacy-vs-generated semantic diff is
the **next** slice `UNIT-DEFINITION-SHADOW-COMPARE-1` — not done here.

## Test

```bat
py -3 -m pytest tools/unit_converter -q
```

Covers parser, mech/vehicle extraction, emit format (asserted against the exact
engine loader tokens), a parse round-trip, and determinism (byte-stable emit +
PYTHONHASHSEED-subprocess guard, mirroring `tools/campaign_gen`).
