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
| Loadout | first-variant `ItemNMasterId` weapon rows → weapon tag via compbas | `[Item:N] MasterID` weapon rows |
| UI | `LittleIcon`, `DescIndex`, `EncyclopediaID` | `IconIndex`, `DescIndex`, `EncyclopediaID` |

## Emitted files (per unit, under `<out>/core/<unit>/`)

- **`unit.fit`** → `[UnitProfile]` `b Jump=`, `st Locomotion=`, `st DamageSchema=`
  — matches `code/unitprofile_fit.cpp::populateProfileFromFit`.
- **`presentation.fit`** → `[PresentationSockets]` `l NumSockets=N` + `[SocketN] st SocketName=`
  — matches the `mclib/mech3d.cpp` PresentationSockets loader.
- **`ui.fit`** → `[UIProfile]` `st Icon=`, `l NameID=`, `l EncyclopediaID=`
  — matches `populateProfileFromFit`'s `[UIProfile]` read.
- **`hardpoints.fit`, `loadout.fit`** → forward-looking HardpointId / Loadout
  blocks (no shipped engine reader yet; Phase-2 `HARDPOINT-ID-SCHEMA-1`).

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
