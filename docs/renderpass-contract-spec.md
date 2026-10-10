<!-- doc-status: current | reviewed:2026-10-11 -->
# Render Pass Contract Registry — Spec

**Slice:** RENDERPASS-CONTRACT-2.5
**Status:** IMPLEMENTING (recon cleared the file-touch guard)
**Branch tip:** `1d7b9ea6` (no commit yet at spec-write time)

## Intent

Provide a small, **descriptive** header-only registry that enumerates the major
render-pass lanes and tabulates their current closure state (PipelineDesc-registered,
ViewUniforms-bound, snapshot-row-authoritative, kill-switch, inspector section,
owning subsystem).

Mirrors the table-of-facts style of `RenderCore/RendererFeatureRegistry.h`. It is
NOT a scheduler. It owns no callbacks. It does not modify draw-order or routing.
Pass-owner code paths continue to be invoked imperatively from the frame loop
exactly as today. The registry exists for inspection, audit, and migration tracking.

## Struct

```cpp
enum class RenderPassId : uint32_t {
    StaticPropOpaque = 1,
    Terrain          = 2,
    MechOpaque       = 3,
    Shadow           = 4,
    VFX              = 5,
};

struct RenderPassContract {
    RenderPassId id;
    const char*  name;
    const char*  ownerSubsystem;
    bool         viewUniformsBound;
    bool         pipelineDescRegistered;
    bool         snapshotRowAuthoritative;
    const char*  inspectorSectionId;
    const char*  killSwitchEnv;    // nullptr if none
    const char*  notes;
};
```

## Entries (5)

| id | name | owner | viewUni | pipeDesc | snapAuth | inspector | kill-switch |
|----|------|-------|---------|----------|----------|-----------|-------------|
| 1 | StaticPropOpaque | GpuStaticPropBatcher | Y | Y | Y | StaticProp | MC2_SNAPSHOT_STATIC_PROP_BUILD |
| 2 | Terrain | TerrainPatchStream | N | N | N* | Terrain Pass##tp | (none) |
| 3 | MechOpaque | GpuMechBatcher | N | N | Y | Mech | MC2_SNAPSHOT_MECH_EXTRACT |
| 4 | Shadow | gosPostProcess + per-lane shadow programs | N | N | N | Shadow Pass##sp | (none) |
| 5 | VFX | mc2::particles::Batcher | N | N | N | VFX Pass##vfx | (none) |

*Terrain snapAuth=N: `TerrainPassFacts` row just landed in `1d7b9ea6` as a passive
recorder. Not yet authoritative — flip flag when v3-equivalent terrain extraction
ships.

## Files touched

| File | Action | Line budget |
|------|--------|-------------|
| `RenderCore/RenderPassContract.h` | NEW header-only | ~110 lines |
| `docs/renderpass-contract-spec.md` | NEW (this file) | n/a (doc) |
| `GuiRuntime/EditorInspector.cpp` | OPTIONAL: add "Render Pass Contracts" CollapsingHeader | ~25 lines |

**Total estimate: 3 files, ~135 lines added.** Comfortably under the 6-file /
250-line budget.

## What the pass-owner files do NOT do

Pass-owner files (`gos_terrain_*.cpp`, `gos_static_prop_batcher.cpp`,
`gos_mech_batcher.cpp`, `gosPostProcess.cpp`, `particles/batcher.cpp`) are
NOT modified. The contract is purely descriptive — no `kPassId` constant
includes, no callbacks, no registration sites.

## Ship strategy

Single slice. The registry is header-only constexpr data with optional
inspector consumer; both halves are small and trivially reviewable together.
No need to split into per-pass slices.

## Anti-goals (sanity)

- No execute() callbacks.
- No scheduler / render graph.
- No modification to feature/pipeline/material registries.
- No shader changes.
- No new env vars.
- No GL state changes.
- No new object-ID writers.
