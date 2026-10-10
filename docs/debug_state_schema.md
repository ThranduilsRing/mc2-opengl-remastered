<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Debug State JSON Schema

Schema version: `MC2_DEBUG_STATE_V1`

Written by `GameOS/gameos/debug_state_dump.cpp` when `MC2_DEBUG_STATE_DUMP=1`.
Default output: `debug_state/latest_render_state.json` (override: `MC2_DEBUG_STATE_DUMP_DIR`).
Cadence: frame 1, then every 300 frames.
Optional rolling history: `history_0.json`..`history_7.json` when `MC2_DEBUG_STATE_DUMP_HISTORY=1`.

Validated by `scripts/check-debug-state-json.py`.

---

## Top-level fields

> V1 fields: `schema`, `frame`, `mission`, `build`, `features`, `engineView`, `renderSnapshot`, `staticPropOpaque`, `mech`.
> V2 additions (below `schema`): `schema_version`, `session_id`, `pid`, `build_id`, `dump_kind`, `written_at_epoch`.

| Field | Type | Description |
|---|---|---|
| `schema` | string | Always `"MC2_DEBUG_STATE_V1"`. Bump on breaking change. |
| `frame` | uint64 | Engine frame index at write time. |
| `mission` | object | Current mission identity. |
| `build` | object | Build configuration. |
| `features` | object | Feature gate states at write time. |
| `engineView` | object | Active EngineView registration. |
| `renderSnapshot` | object | RenderSnapshot ok gate + mismatch counters. |
| `staticPropOpaque` | object | StaticPropOpaque visual globals. |
| `mech` | object | Mech snapshot counters + per-instance packet array. Gate: `MC2_SNAPSHOT_MECH_EXTRACT=1`. |

---

## `mission`

| Field | Type | Description |
|---|---|---|
| `name` | string | Mission name string (e.g. `"mc2_24"`). Empty string when `known=false`. |
| `known` | bool | `true` when `missionName[]` is non-empty at dump time. |

---

## `build`

| Field | Type | Description |
|---|---|---|
| `commit` | string | Git commit hash. Currently always `"unknown"` (no compile-time injection). |
| `config` | string | `"Debug"`, `"Release"`, or `"RelWithDebInfo"`. Derived from `_DEBUG`/`NDEBUG` macros. |

---

## `features`

All fields are bool. `true` = gate active (feature enabled). `false` = gate inactive (kill-switch or default-OFF).

| Field | Default | Env var | Notes |
|---|---|---|---|
| `MC2_DEBUG_STATE_DUMP` | `true` | `MC2_DEBUG_STATE_DUMP` | Always `true` in the snapshot (file only exists when dump is on). |
| `MC2_VIEW_UNIFORMS` | `true` | `MC2_VIEW_UNIFORMS` | ViewUniforms UBO upload. =0 to disable. |
| `MC2_SNAPSHOT_STATIC_PROP_BUILD` | `true` | `MC2_SNAPSHOT_STATIC_PROP_BUILD` | Snapshot-built static-prop dispatch (v3). =0 reverts to live builder. |
| `MC2_MATERIAL_GPU` | `true` | `MC2_MATERIAL_GPU` | GPU material table. =0 disables. |
| `MC2_MATERIAL_GPU_SAMPLE` | `true` | `MC2_MATERIAL_GPU_SAMPLE` | GPU albedo sampling in static_prop.frag. Requires `MC2_MATERIAL_GPU`. =0 falls back to texArrayLayer. |
| `MC2_STATIC_PROP_IBL_SH` | `true` | `MC2_STATIC_PROP_IBL_SH` | SH-L2 image-based ambient on static props. =0 disables. |
| `MC2_STATIC_PROP_PBR_V1` | `false` | `MC2_STATIC_PROP_PBR_V1` | Schlick-Fresnel + power-lobe specular. =1 enables. Requires `MC2_VIEW_UNIFORMS`. |

---

## `engineView`

| Field | Type | Description |
|---|---|---|
| `known` | bool | `true` if `setCurrentView()` has been called this frame. |
| `viewId` | int | Active view ID. `0` = invalid/unset, `1` = MainScene. |
| `viewKind` | string | `"MainScene"` for viewId=1, `"unknown"` otherwise. |
| `viewMode` | string | `"Visual"`, `"ObjectIdDebug"`, `"TacticalOverlay"`, `"Thermal"`, `"Infrared"`, `"LowLight"`. |
| `viewUniformsBinding` | int | UBO binding point for ViewUniforms. Always `3`. |
| `viewport` | int[4] | `[x, y, width, height]` in pixels. All zeros if `known=false`. |

---

## `renderSnapshot`

Counters reset each frame by `ExtractRenderSnapshot()`. Values reflect the most recent completed frame.

| Field | Type | Description |
|---|---|---|
| `ok` | bool | `true` = all ok-gate counters are zero. `false` = at least one failure counter is nonzero. |
| `staticPropValidationFail` | uint | Static-prop type validation failures (type out of range, etc.). |
| `staticPropPacketRangesFail` | uint | Packet range validation failures. |
| `staticPropPacketInvalid` | uint | Individual packet validity check failures. |
| `arenaOverflow` | bool | Frame arena overflowed capacity. |
| `spBuildAttempted` | uint | `1` if snapshot build gate ran this flush, `0` if not. Informational; not in ok gate. |
| `spBuildFallback` | uint | `1` if snapshot build fell back to live builder. Informational; not in ok gate. |
| `spBuildCountMismatch` | uint | Type-count mismatch between snapshot and live builder. In ok gate. |
| `spBuildPacketMismatch` | uint | Packet-content mismatch. In ok gate. |
| `spBuildMetaMismatch` | uint | Dispatch-meta mismatch. In ok gate. |

---

## `staticPropOpaque`

Runtime visual globals for the StaticPropOpaque render lane. Values reflect the state at dump time; some are set at process start (env-var gates), some are mutable at runtime via ImGui sliders.

| Field | Type | Description |
|---|---|---|
| `snapshotDispatchDefault` | bool | `true` = v6 snapshot-built dispatch is active (default). `false` = legacy multidraw. Complement of `legacyDispatch`. |
| `legacyDispatch` | bool | `true` = `MC2_STATIC_PROP_LEGACY_DISPATCH=1` kill-switch is active. |
| `materialGpuEnabled` | bool | GPU material table is active. |
| `materialGpuSample` | bool | GPU albedo sampling is active (requires `materialGpuEnabled`). |
| `iblShEnabled` | bool | SH-L2 IBL ambient is active. |
| `iblShStrength` | float | IBL ambient strength (ImGui slider, range 0..3). Effective value = `iblShEnabled ? iblShStrength : 0.0`. |
| `iblShSet` | string | Active IBL SH coefficient set name (e.g. `"default"`, `"mc2_24"`). |
| `pbrEnabled` | bool | PBR specular gate is active. |
| `pbrStrength` | float | PBR specular strength (ImGui slider, range 0..3). Effective value = `pbrEnabled ? pbrStrength : 0.0`. |
| `pbrRoughnessOverrideEnabled` | bool | Per-material roughness override is active. |
| `pbrRoughnessOverride` | float | Override roughness value (range 0..1). Only meaningful when `pbrRoughnessOverrideEnabled=true`. |
| `debugMaterialMode` | int | Material debug view mode. `0` = off (normal rendering). Nonzero = debug visualization active. |

---

## `mech`

Gate: `MC2_SNAPSHOT_MECH_EXTRACT=1` (default OFF). When the gate is OFF or no submits have been recorded, `extractEnabled` is `false` (or `true` but counters are all zero) and `packets` is an empty array. No data is synthesized — this is a read-only projection of already-extracted data.

| Field | Type | Description |
|---|---|---|
| `extractEnabled` | bool | `true` if `MC2_SNAPSHOT_MECH_EXTRACT=1` at write time. When `false`, all counters are 0 and `packets` is empty. |
| `rows` | uint | `mechSnapshotCount` — number of mech submit entries captured this frame. |
| `mat_valid` | uint | Rows where `materialIdx != 0xFFFFFFFF` (material wired). |
| `mat_sentinel` | uint | Rows where `materialIdx == 0xFFFFFFFF` (not yet wired — expected in v0). |
| `countMismatch` | uint | `mechCountMismatch` — `1` if snapshot count diverged from live pending count. |
| `handleMismatch` | uint | `mechHandleMismatch` — rows where `typeLodIdx` differed from live. |
| `objectIdMismatch` | uint | `mechObjectIdMismatch` — rows where `objectIdRaw` differed from live. |
| `texHandleMismatch` | uint | `mechTexHandleMismatch` — rows where `texHandle` differed from live. |
| `materialIdxMismatch` | uint | `mechMaterialIdxMismatch` — rows where `materialIdx` differed from live. |
| `truncated` | bool | `true` if `rows > 32`; only the first 32 entries appear in `packets`. |
| `packets` | array | Up to 32 per-mech packet entries (see below). Empty when `extractEnabled=false` or no submits. |

### `mech.packets[]`

One entry per mech instance submitted to the GPU batcher this frame (capped at 32).

| Field | Type | Description |
|---|---|---|
| `objectIdRaw` | uint | `GpuMechSubmitDesc::objectIdRaw` — `RenderObjectHandle.raw()` for this actor. |
| `instanceIdx` | uint | Index `i` in the frame's `s_pendingSubmits` array. |
| `texHandle` | uint | `GpuMechSubmitDesc::slot0TexHandle` — mcTextureManager slot index for the paint-scheme texture. |
| `textureName` | string | Human-readable texture name from `gos_getMechTextureNameByNodeIdx(texHandle)`. Empty string if not available. |
| `materialIdx` | uint | Index into `s_mechMaterialTable`. `4294967295` (`0xFFFFFFFF`) = sentinel (not wired in v0). |
| `materialIdxSentinel` | bool | `true` when `materialIdx == 0xFFFFFFFF`. |
| `typeLodIdx` | uint | PendingSubmit type×LOD record index. |
| `renderFlags` | uint | Render flags bitmask: bit 0 = `ALPHA_TEST`, bit 1 = `lightsOut`, bit 2 = `isHighlighted`. |

---

## Versioning

- Breaking schema change (field removed, type changed, semantics changed) → bump `schema` to `MC2_DEBUG_STATE_V3`.
- `MC2_DEBUG_STATE_V2` added V2 top-level fields (2026-06-17).
- `MC2_DEBUG_STATE_V1` was the original schema. Both V1 and V2 are recognized by MCP.
- Additive change (new field added) → keep current `schema` string; consumers must handle unknown fields gracefully.
- `check-debug-state-json.py` validates required fields for V1. New fields are optional and ignored by the validator until V2.

## Out of scope for V1

The following are explicitly excluded to keep the dump focused:

- Full object/static-prop instance table
- Full material table
- Mission script or save state
- Network state
- Any per-object or per-material detail beyond lane-level globals

---

## Schema V2 additions (MC2_DEBUG_STATE_V2)

Added in 2026-06-17. New top-level fields emitted BEFORE `frame`:

| Field | Type | Description |
|---|---|---|
| `schema_version` | int | Integer `2`. Allows numeric comparison without string parsing. |
| `session_id` | string | Process-unique identifier: `"<pid>-<epoch_us>"`. Same value for all dumps in one engine run. |
| `pid` | int | OS process ID of the engine instance that wrote this file. |
| `build_id` | string | Build config string, e.g. `"RelWithDebInfo-unknown"`. |
| `dump_kind` | string | `"periodic"` (routine dump), `"shutdown"` (final dump on exit). |
| `written_at_epoch` | float | Unix epoch seconds with microsecond precision at write time. |

### Liveness classification (MCP)

MCP `get_render_health()` and `get_liveness()` use these fields to distinguish:
- **Live**: `dump_kind == "periodic"` AND `pid_alive == true` AND `seconds_since_update < 30`
- **Shutdown dump**: `dump_kind == "shutdown"` — engine exited, data is final but process dead
- **V1/unknown**: no V2 fields — fall back to age-only heuristic

### Atomic write

The engine writes V2 snapshots via tmp-file + `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` to the same directory (NTFS atomic rename). MCP retry on `JSONDecodeError` remains as defense-in-depth against edge cases (existing corrupt file, antivirus interference).

### Shutdown dump

`mc2_debug_state::writeShutdownState()` writes a `dump_kind="shutdown"` snapshot on clean exit. Gate: `MC2_DEBUG_STATE_DUMP=1`. Useful for post-mortem queries when the engine is not running.

### unitProfile (UNIT-PROFILE-SEAM-1)

Additive section, present ONLY when `MC2_UNIT_PROFILE_DATA=1`. Schema byte-for-byte unchanged when the gate is absent. One row per live mover.

| Field | Type | Meaning |
|---|---|---|
| `cap_jump_baseline` | bool | Static `CAP_JUMP` fact on the mover's `ObjectType` profile (from installed-equipment scan, or `.fit [UnitProfile] Jump`). |
| `cap_jump_current` | bool | Current `CAP_JUMP` fact in the mover's runtime state (seeded from baseline at type-bind, refreshed post-Load). |
| `can_perform_jump` | bool | Executability: `cap_jump_current` AND a working jump executor (`hasJumpExecutor()`, true only for `BattleMech` this slice). Fact != executability. |
