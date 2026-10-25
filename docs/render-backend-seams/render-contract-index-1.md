<!-- doc-status: current | reviewed:2026-10-11 -->
# RENDER-CONTRACT-INDEX-1 — master index of render-contract artifacts

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC · **Slice:** RENDER-CONTRACT-INDEX-1 (recon/docs only, no code, no relink)
**Built against:** nifty-mendeleev HEAD `0390b805` (lane advanced to `b4357065` mid-session — re-grep line numbers) · **Date:** 2026-06-22

## Purpose

One map of every existing render-contract artifact, what Vulkan-readiness dimension it
covers, and where the real gaps are. Built because the dominant risk in this arc is **wasted
re-recon**: the original arc proposal and a fresh four-agent recon both independently
re-derived artifacts that already exist on disk. Read this before scoping any
VULKAN-CONTRACT-MANIFEST-ARC slice. If you find yourself "discovering" a contract doc,
check here first — it is probably already inventoried below.

The seven Vulkan-shaped contract dimensions tracked: **passes · resources · bindings (buffer)
· samplers (texture units) · shader-variants · xform/transform conventions · resource
lifetime/sync**.

## Coverage matrix (dimension → best existing artifact → status)

| Dimension | Authoritative artifact(s) | Status | Gap |
|---|---|---|---|
| **Buffer bindings (SSBO/UBO)** | `scripts/check-binding-slots.py` + `docs/render-backend-seams/binding-slot-occupancy.{md,json}` | **COVERED** (checked + regenerated; multiplexed-per-pass model) | Bare GLSL literals WARN-only (hand-lockstep) |
| **Sampler / texture units** | `sampler-unit-occupancy.md` | **RETIRED** (GL renderer removed) | Vulkan GLSL descriptors: `binding_slots` + `vulkan_bindings`; Slang has separate contracts |
| **Render passes (identity)** | `mclib/render_contract.h` `PassIdentity` (15) · `RenderCore/RenderPassContract.h` `RenderPassId` (5) | **PARTIAL/SPLIT** | Two un-reconciled taxonomies; 10 PassIdentity orphans have no RenderPassId. → RENDERPASS-ENUM-RECON-1 (largely done below) |
| **Pass resource flow / barriers** | `RenderCore/RenderPassContract.h` `reads[4]`/`writes[4]`/`barrierAfter` + `RenderResourceRegistry.h` | **SKELETON EXISTS** | No load/store ops, no layout transitions, no ordered pass list, only 5 of ~15 passes have rows. → RENDER-PASS-DAG-CONTRACT-1 |
| **Pass GL-state (blend/depth/MRT)** | `mclib/render_contract.h` `PassStateContract` | **COVERED** (separate file, different enum key) | Split from resource-flow half; not joined to DAG |
| **Shader variants / ABI** | `scripts/check-shader-schema.sh` + `tools/shader_schema/manifest.json` + `check-material-gpu-mirror.sh` | **COVERED** (SSBO struct/std430/offset CI) | Runtime `#define` prefix injection still un-inventoried (separate SHADER-PIPELINE-OFFLINE-ARC, deferred) |
| **Xform / transform conventions** | `docs/render-contract.md` + `check-unified-projection-retirement.sh` + ViewUniforms ABI (binding=3, 144B) | **PARTIAL** | Per-subsystem transform-ownership (mech bone-fold vs static model-matrix vs terrain global) not in one ledger. → XFORM-CONVENTION-LEDGER-1 |
| **Resource lifetime / sync** | docs/render-backend-seams/gpu-buffer-owner-recon-1.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz) + `gpu-buffer-wrapper-design-1.md` | **RECON DONE, no instrumentation** | No live per-frame orphan-vs-ring counter. → GPU-UPDATE-BUFFER-COUNTER-1 |

## Artifact registry (grouped)

### Checkers (`scripts/`)
- `check-contracts.sh` — aggregator (runs all cheap static-contract checks)
- `check-binding-slots.py` — buffer binding slots, C++↔GLSL lockstep, preprocessor-branch-aware (**samplers explicitly out of scope per its docstring**)
- `check-shader-schema.sh` — SSBO struct ABI vs `manifest.json` golden, std430/offsetof
- `check-material-gpu-mirror.sh` — `MaterialGpu.h` ↔ `material_gpu.hglsl` field-order lockstep
- `check-render-contract-gbuffer1.sh` — fragment shaders must use `rc_*` helpers for GBuffer1

#### Raw GL ownership (updated 2026-10-14)

The five per-axis Python checkers and their capstone were retired in
`633856094`. Their depth-func, depth-mask, color-mask, blend-func/equation and
FBO-bind checks now run in `36544d925:scripts/check-no-raw-gl-from-game.sh`, alongside
the game-side GL firewall. The aggregator row is `no_raw_gl_from_game`.

`36544d925:scripts/raw-gl-state.allowlist.json` records the sanctioned backend owners
for all five axes; the checker rejects an incomplete axis set. New raw calls
on added diff lines must retain those owners. FBO saved-target restores and
explicit `FBO-OWNER` tags retain their exemptions. The game-side scan still
rejects raw GL outside `scripts/check-no-raw-gl-from-game.allowlist`.

Run `sh scripts/check-no-raw-gl-from-game.sh [--base REF]`. The default base is
`merge-base HEAD origin/main`, falling back to HEAD when unavailable. The old
per-axis `--all` audit modes and standalone capstone command no longer exist.

- `check-include-firewall.sh`, `check-no-raw-gl-from-game.sh`, `check-vfx-no-objectid.sh`, `check-visibility-log-schema.sh`, `check-unified-projection-retirement.sh`, `check-mlr-leaves-gated.sh`, `check-particles-no-cpu-projection.sh` — RenderWorld-boundary / lane firewalls (PARTIAL contract coverage)
- `validate_shaders.py` — glslang SPIR-V compile gate; uses `--auto-map-bindings` (compile-only; **does NOT record sampler occupancy** — symptom of the blind spot, not coverage)

### Docs
- `docs/render-contract.md` — coordinate-space / submission-bucket contract (PARTIAL, half-stale per 2026-05-14 audit)
- `docs/renderpass-contract-spec.md` — 5-lane render-pass registry spec (FULL)
- `docs/render-binding-registry.md` — hand doc; **superseded** by `binding-slot-occupancy.md`; sampler table (`:104-127`) self-admittedly incomplete
- `docs/render-backend-seams/binding-slot-occupancy.{md,json}` — current buffer-slot occupancy (authoritative)
- docs/render-backend-seams/gpu-buffer-owner-recon-1.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz) + `gpu-buffer-wrapper-design-1.md` — buffer-lifetime master inventory + `GpuRingBuffer<N>` design (extend, don't duplicate)
- `docs/render-backend-seams/vulkan-readiness-audit-1.md` — prior readiness audit (the % baseline this arc moves)
- `docs/render-backend-seams/opengl-correctness-ledger-1.md` — GLSTATE/texunit correctness ledger
- `docs/engine-standalone-seams.md` — EngineView/ViewUniforms/PipelineRegistry/RenderResourceRegistry seams
- `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md` — SSBO slots 8-20, M1-M6 (FULL)
- `docs/superpowers/specs/2026-04-26-render-contract-{registry,f3-mrt}-design.md` — GBuffer1 contract design
- `docs/superpowers/specs/2026-06-04-trackv-render-contract-checklist.md` — all-dimension compliance checklist

### C++ headers
- `RenderCore/RenderPassContract.h` — `RenderPassId` (5), `kRenderPassContracts[5]`, per-pass `reads`/`writes`/`barrierAfter`
- `RenderCore/RenderResourceRegistry.h` — `RenderResourceId` (MainColor/Depth, Shadow{Static,Dynamic}Map, TerrainHeight, MaterialGpuBuffer, WaterReflection{Color,Depth})
- `RenderCore/PipelineDesc.h` — BlendMode/CullMode/DepthFunc/ColorAttachmentMask/PipelineDesc; SSBO slot comments (8/9/14/16/20)
- `RenderCore/MaterialGpu.h` — `MaterialFlags` (6 bits)
- `RenderCore/ViewUniforms.h` — UBO binding=3, std140, 144B
- `mclib/render_contract.h` — `PassIdentity` (15), `PassStateContract` (blend/depth/MRT)
- `RenderWorld/RenderWorld.h`, `RenderWorld/VisibilityRequest.h` — M1 lifecycle API, visibility masks/results

### GLSL includes (`shaders/include/`)
- `render_contract.hglsl` — GBuffer1 `rc_*` helpers
- The GL `view_uniforms.hglsl`, `scene.hglsl`, `lighting.hglsl` and
  `material_gpu.hglsl` binding declarations were retired.
- Live Vulkan buffer, sampler and image descriptors use explicit set/binding
  declarations in `shaders/vulkan/`; see `binding-slot-occupancy.md`.

## RENDERPASS-ENUM-RECON-1 result (folded in — this recon is effectively complete)

Two genuinely different taxonomies, not just different sizes:
- `render_contract::PassIdentity` (`mclib/render_contract.h:40-56`, 15 vals) = fine-grained **callsite tag** (terrain split base/overlay/decal/grass; objects opaque/alpha; UI/Debug/Water/Vegetation first-class).
- `RenderCore::RenderPassId` (`RenderCore/RenderPassContract.h:47-55`, 5 vals) = coarse **owner-lane** registry (one row per GPU batcher).

**Map (PassIdentity → RenderPassId):** `TerrainBase→Terrain`, `StaticProp→StaticPropOpaque`, `ShadowCaster→Shadow`, `ParticleEffect→VFX`, `OpaqueObject→MechOpaque` (lossy — also covers vehicles + legacy buildings).
**10 PassIdentity orphans** (no RenderPassId): Unknown, TerrainOverlay, TerrainDecal, Grass, Water, AlphaObject, VegetationCards, UI, DebugOverlay, PostProcess. **0 RenderPassId orphans.**

A DAG keyed off either enum today is wrong: `RenderPassId` omits the entire post-process chain, water, overlays/decals, vegetation, UI — exactly what a barrier graph most needs.

**Frame execution order** (enqueue `code/gamecam.cpp:437-603`; actual GL flush in `mcTextureManager::renderLists()` `mclib/txmmgr.cpp:2524-3081`; post-process after `gos_RendererEndFrame` in `gameosmain.cpp:585-598`): sky → terrain enqueue → craters → object enqueue (mixed immediate/queued) → water enqueue → shadow enqueue → **renderLists flush (shadows resolve FIRST, then static-prop/mech/terrain/water draw)** → water reflection RT → water fastpath → vegetation → particles → clipper FX → weather → UI/HUD → post-process. **Shadow-before-geometry must be an explicit DAG edge** (shadows enqueue after geometry but resolve before it inside the flush; `frameBegin()` pre-seeds `ShadowDynamicMap` to paper over this).

**What RenderPassContract already has for a DAG:** logical `reads[4]`/`writes[4]` + coarse `barrierAfter` (GL `glMemoryBarrier` mask). **What it lacks for Vulkan:** load/store ops, image layout transitions (src/dst layout + access/stage masks, not a GL barrier mask), ordered pass list / resolved edges, per-attachment blend/depth (lives in the *other* file `PassStateContract`), and rows for the 10 orphan passes.

## Buffer-lifetime recon result (folded in)

`gos_UpdateBuffer` is **near-dead** (1 runtime callsite: `txmmgr.cpp:2368`, tiny scene UBO; orphan-on-write `glBufferData`, **ignores its `offset` arg**). Real per-frame orphan churn is **raw GL** + the private `updateBuffer()` 5-arg overload (`gl_utils.cpp:431`, HUD/gosMesh, **not** hitch-accounted). Already-fenced 3-frame rings: mech batcher, static-prop batcher, terrain solid+water thin-record (WATER-THINRING-FENCE-1 shipped — water smell closed). Ring template to extract: `gos_mech_batcher.cpp:701-717`. Counter slice should reuse `g_mc2HitchAccum` (`mc2_hitch_trace.h:135-157`), gate `MC2_GPUBUF_COUNTER`, **HUD as Tier-0 pilot** (postprocess quad is static → poor counter target).

## Corrected arc sequence (supersedes the original proposal ordering)

The original "fastest sane sequence" assumed several already-done slices. Corrected:

1. **RENDER-CONTRACT-INDEX-1** ← *this doc* (DONE 2026-06-22). Also folded: RENDERPASS-ENUM-RECON-1 (below) and the RenderWorld/API-spine modern-vs-legacy routing (now in `docs/engine-standalone-seams.md` + `docs/render-contract.md`).
2. **SHADER-SAMPLER-BINDING-MANIFEST-1** -- RETIRED with the GL renderer; see `sampler-unit-occupancy.md`.
3. **RENDER-PASS-DAG-CONTRACT-1** — widen `RenderPassId` to cover the 10 orphans (append-only before `_SentinelLast`, update table + static_assert same commit); add `PassIdentity→RenderPassId` mapping fn (keep PassIdentity — 10 live callsites); add load/store + layout + `dependsOn[]` fields to `RenderPassContract`; emit ordered `kFramePassOrder[]` with the shadow-before-geometry edge; cross-ref (don't merge yet) `PassStateContract` blend/depth/MRT.
4. **GPU-UPDATE-BUFFER-COUNTER-1** — measurement-only, reuse `g_mc2HitchAccum`, gate `MC2_GPUBUF_COUNTER`, HUD pilot. Byte-identical output; tier1 5/5 is the gate.
5. **XFORM-CONVENTION-LEDGER-1** — single ledger of per-subsystem transform ownership (mech bone-fold / static model-matrix / terrain global / ViewUniforms). Docs-only.
6. *(deferred, higher risk, needs 2-5 landed)* GPU-RESOURCE-MANAGER-GL-ONLY-1 (first true backend seam) · SHADER-PIPELINE-OFFLINE-ARC (runtime `#define` injection inventory).

**Do NOT** build a flat global binding enum (multiplexed model is correct), a big `IRenderBackend`/`RenderDevice`/`CommandContext` (premature per seam recon), or resurrect `gpu-buffer-wrapper-design-1.md` §3 flat slot-enum (superseded).

## Per-slice preflight discipline

Shared-recon-queue rule: every slice runs `repo_query.py slice-preflight` (or `mcp__mc2-repo-intel__slice_preflight`) against nifty HEAD before coding; `verdict=STOP` → re-recon. All file:line citations here are current as of `0390b805` — cheap to drift; re-grep before relying on a line number.
