<!-- doc-status: current | reviewed:2026-10-11 -->
# Asset manifest schema (scaffold)

`ASSET-MANIFEST-SCHEMA-SCAFFOLD-1` (VALIDATION-SCAFFOLD-PREFLIGHT-1).

A minimal manifest schema + validator FOUNDATION for the future asset-pipeline
probe lane (`TRACKG-ASSET-PIPELINE-PROBE-OPUS-1`). This is a guardrail, not the
pipeline: it validates manifest SHAPE only — no asset import, no KTX2 bake, no
file-existence checks (asset binaries are not committed).

## Validator
`tools/validate_asset_manifest.py <manifest.json>` — exit 0 valid / 1 invalid.
(`tools/` is gitignored at repo root; the file is force-added like the other
committed `tools/` scripts.)

## Fixture
`tests/fixtures/assets/minimal_asset_manifest.json` — the canonical minimal
valid manifest; round-trips through the validator (exit 0).

## Fields
| field | type | notes |
|---|---|---|
| `assetId` | string | stable identity, non-empty |
| `source` | string | source asset path (relative) |
| `kind` or `type` | string | e.g. `mesh` / `prop` / `mech` |
| `materials` | array | may be empty |
| `capabilities` | object | the six booleans below (all required) |
| `lods` | array | empty array = explicit "no LODs" |
| `textureRefs` | array | empty array = explicit "no textures" |

### Capabilities (vocabulary reconciled with the renderer)
Names match `RenderCore/RenderObjectDesc.h ArchetypeFlags` where they overlap so
the manifest vocabulary stays consistent with the engine:

| capability | engine mapping |
|---|---|
| `hasNormals` | vertex attribute — validator-only, no engine flag |
| `hasTangents` | vertex attribute — validator-only, no engine flag |
| `hasLods` | `ArchetypeFlags.hasClusterLod` |
| `hasImpostor` | `ArchetypeFlags.usesImpostor` |
| `castsShadow` | `ArchetypeFlags.castsShadow` |
| `supportsObjectId` | `RenderObjectDesc.gameObjectId` / object-ID buffer |

## Optional sections (`ASSET-MANIFEST-0-EXTEND`)
These are validated for SHAPE only when present; a manifest without them stays
valid (the minimal fixture above still round-trips). Worked example:
`tests/fixtures/assets/extended_asset_manifest.json`.

| field | type | notes |
|---|---|---|
| `geometry` | object | ASSIMP-IMPORTER-PHASE-0 output contract (see below) |
| `lods[]` items | object | may be `{level, vertexCount, triangleCount, error}` (MESHOPT LOD-stat contract); plain/opaque entries still allowed |
| `textureRefs[]` items | object | `slot` + `path` required; `slot` ∈ {`albedo`,`normal`,`orm`,`emissive`,`mask`}; optional cooked fields `format`/`vkFormat`/`mips`/`dims` |
| `provenance` | object | `{tool, toolVersion, generatedAt, sourceHash}` (all string) |
| `generatedOutputs` | array | `[{path, kind}]` — pointers to regeneratable artifacts (live under ignored `out/`) |

### `geometry` (ASSIMP-IMPORTER-PHASE-0 output contract)
The shape a future offline importer probe writes per imported source mesh:

| field | type |
|---|---|
| `meshCount` / `vertexCount` / `indexCount` / `materialSlotCount` | integer |
| `hasNormals` / `hasTangents` | boolean |
| `bounds` | `{min:[x,y,z], max:[x,y,z], radius}` |

## Material authoring (`MATERIAL-AUTHORING-VALIDATION-1`)
When present, material/texture authoring metadata is validated against the
engine conventions (`RenderCore/MaterialGpu.h`, `tools/mc2texcook/mc2texcook.py`):

| field | rule |
|---|---|
| `materials[].name` / `.shader` | required non-empty strings |
| `materials[].alphaMode` | ∈ {`opaque`,`alphaTest`,`blend`} |
| `materials[].alphaTestThreshold` | number in [0,1]; only with `alphaMode='alphaTest'` |
| `materials[].doubleSided` | boolean |
| `materials[].pbr.baseColorFactor` | [0,1] number, or 3–4 element [0,1] array |
| `materials[].pbr.metallicFactor` / `.roughnessFactor` | number in [0,1] (cook defaults metallic=0.0, roughness=1.0) |
| `textureRefs[].colorSpace` | must match the slot convention below |

### Slot → colorSpace convention (mc2texcook presets)
| slot | colorSpace | vkFormat |
|---|---|---|
| `albedo` / `emissive` | `srgb` | 43 (`R8G8B8A8_SRGB`) |
| `normal` / `orm` / `mask` | `linear` | 37 (`R8G8B8A8_UNORM`) |

`orm` packs R=AO, G=roughness, B=metalness. **Cross-check:** any `normal` slot
requires `capabilities.hasTangents = true`.

Fixtures: `material_validation_pass.json` (valid); `invalid/material_fail_*.json`
(rejected — tangent mismatch, colorSpace mismatch, bad alphaMode). These
`invalid/material_fail_*.json` fixtures are NOT yet written — they will land when
`MATERIAL-AUTHORING-VALIDATION-1` is implemented in `validate_asset_manifest.py`.

## Running the gate
`py -3 scripts/check-asset-manifests.py` validates every
`tests/fixtures/assets/*.json` (must pass) and every `…/invalid/*.json` (must be
rejected). Deterministic, offline, no asset binaries.

Also available: `py -3 scripts/check-asset-manifest-fixtures.py` — an earlier
single-runner (`ASSET-MANIFEST-NEGATIVE-FIXTURES-1`) that drives
`validate_asset_manifest.py` over the base-capability fixture corpus only (9
invalid fixtures for the core required-field rules). When `check-asset-manifests.py`
is fully wired it supersedes this runner; run either or both.

## For the future asset-pipeline lane
Extend HERE rather than inventing a parallel format: add fields to the schema +
validator + fixture, and wire `validate_asset_manifest.py` into CI alongside
`check-toolchain-bom.py`. Keep file-existence / bake checks OUT of this
validator — it is the shape gate. Cooked-texture provenance is exercised by
`KTX2-BAKE-PROBE-1`.
