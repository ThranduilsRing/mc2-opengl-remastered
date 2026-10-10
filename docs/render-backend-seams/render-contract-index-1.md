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
| **Sampler / texture units** | `scripts/check-sampler-bindings.py` + `docs/render-backend-seams/sampler-unit-occupancy.{md,json}` (registered in `check-contracts.sh` as `sampler_bindings`) | **COVERED** (SHADER-SAMPLER-BINDING-MANIFEST-1 shipped 2026-06-22; multiplexed-per-pass, derive-from-code, FAIL on intra-program contradiction / non-multiplexed comment-vs-code drift) | Literal loc-cache binders are `UNKNOWN` rows (WARN) until migrated to named binds; `building_pbr` dormant; 2D_ARRAY-target leak residual recorded |
| **Render passes (identity)** | `mclib/render_contract.h` `PassIdentity` (15) · `RenderCore/RenderPassContract.h` `RenderPassId` (5) | **PARTIAL/SPLIT** | Two un-reconciled taxonomies; 10 PassIdentity orphans have no RenderPassId. → RENDERPASS-ENUM-RECON-1 (largely done below) |
| **Pass resource flow / barriers** | `RenderCore/RenderPassContract.h` `reads[4]`/`writes[4]`/`barrierAfter` + `RenderResourceRegistry.h` | **SKELETON EXISTS** | No load/store ops, no layout transitions, no ordered pass list, only 5 of ~15 passes have rows. → RENDER-PASS-DAG-CONTRACT-1 |
| **Pass GL-state (blend/depth/MRT)** | `mclib/render_contract.h` `PassStateContract` | **COVERED** (separate file, different enum key) | Split from resource-flow half; not joined to DAG |
| **Shader variants / ABI** | `scripts/check-shader-schema.sh` + `tools/shader_schema/manifest.json` + `check-material-gpu-mirror.sh` | **COVERED** (SSBO struct/std430/offset CI) | Runtime `#define` prefix injection still un-inventoried (separate SHADER-PIPELINE-OFFLINE-ARC, deferred) |
| **Xform / transform conventions** | `docs/render-contract.md` + `check-unified-projection-retirement.sh` + ViewUniforms ABI (binding=3, 144B) | **PARTIAL** | Per-subsystem transform-ownership (mech bone-fold vs static model-matrix vs terrain global) not in one ledger. → XFORM-CONVENTION-LEDGER-1 |
| **Resource lifetime / sync** | `docs/render-backend-seams/gpu-buffer-owner-recon-1.md` + `gpu-buffer-wrapper-design-1.md` | **RECON DONE, no instrumentation** | No live per-frame orphan-vs-ring counter. → GPU-UPDATE-BUFFER-COUNTER-1 |

## Artifact registry (grouped)

### Checkers (`scripts/`)
- `check-contracts.sh` — aggregator (runs all cheap static-contract checks)
- `check-binding-slots.py` — buffer binding slots, C++↔GLSL lockstep, preprocessor-branch-aware (**samplers explicitly out of scope per its docstring**)
- `check-shader-schema.sh` — SSBO struct ABI vs `manifest.json` golden, std430/offsetof
- `check-material-gpu-mirror.sh` — `MaterialGpu.h` ↔ `material_gpu.hglsl` field-order lockstep
- `check-render-contract-gbuffer1.sh` — fragment shaders must use `rc_*` helpers for GBuffer1
- `check-raw-gl-depthfunc.py` — **RAW-GL-DEPTHFUNC-DIFF-GATE-1** (Phase 8 first enforcement gate). See note below.

#### RAW-GL-DEPTHFUNC-DIFF-GATE-1 (`scripts/check-raw-gl-depthfunc.py`)

A diff-based static check that bans **new** raw `glDepthFunc()` call sites on ADDED lines,
outside a sanctioned-file allowlist. It mirrors check-new-gates.py (retired in G8) Mode A: diff vs
a base ref (default `merge-base HEAD origin/main`, or `--base`), inspect only ADDED lines,
exit 1 naming each offender when a non-allowlisted file gains a raw `glDepthFunc(`. This is a
**different, non-overlapping** mechanism from `check-colormask-ownership.py` /
`check-drawbuffer-ownership.py`, which validate the descriptive PipelineDesc state table —
this gate bans the raw *call site*, steering depth-func state through the sanctioned emitter
(`pipeline_binder.cpp` applyPipeline) or the `ScopedDepthFunc` RAII wrapper.

- **Allowlist rationale:** the chokepoint emitter and RAII wrapper are sanctioned by design;
  `debug_renderer.cpp` and `editor/EditorGameOS.cpp` are out of the frame loop; `tools/asset_viewer/`
  and `tests/` are out-of-engine / offline (prefix-matched). The **frozen backlog** (9 existing
  render TUs, e.g. `gameos_graphics.cpp`, `gos_postprocess.cpp`, `gos_particle_bridge.cpp`) is
  allowlisted by exact file so it doesn't retro-fail; it's documented as the v1 freeze.
- **v1 = file-level freeze** (backlog files may add raw calls; new files may not) →
  **v2 tightening = per-file count ceiling** (record per-file baseline counts from Mode B so
  even backlog files can't ADD raw calls). v1 is intentionally simple.
- **Run:** Mode A (enforcement, default) `py -3 scripts/check-raw-gl-depthfunc.py [--base REF] [--quiet]`;
  Mode B (audit, advisory exit 0) `py -3 scripts/check-raw-gl-depthfunc.py --all` lists every raw
  site with file:line + per-file counts. Wired into `scripts/check-contracts.sh` as `raw_gl_depthfunc`
  (same way as the other seam checkers). Regression-proof: freezes the backlog, blocks new bypasses.
- `check-raw-gl-depthmask.py` — **RAW-GL-DEPTHMASK-DIFF-GATE-1** (Phase 8 second enforcement gate). See note below.

#### RAW-GL-DEPTHMASK-DIFF-GATE-1 (`scripts/check-raw-gl-depthmask.py`)

Exact clone of the depth-func gate above, swapped to the `glDepthMask()` axis (the depth-WRITE
mask). Diff-based static check that bans **new** raw `glDepthMask()` call sites on ADDED lines
outside a sanctioned-file allowlist; Mode A (default) diffs vs a base ref (default
`merge-base HEAD origin/main`, or `--base`), inspects only ADDED lines, exit 1 naming each
offender when a non-allowlisted file gains a raw `glDepthMask(`. Non-overlapping with the
PipelineDesc state-table checks — it bans the raw *call site*, steering depth-mask state through
the sanctioned emitter (`pipeline_binder.cpp` applyPipeline) or the `GlScopedDepthState` RAII
wrapper (`gl_state_guard.h`, ctor takes `(mask, func)`).

- **Allowlist rationale:** same shape as the depth-func gate — chokepoint emitter + RAII wrapper
  sanctioned by design; `debug_renderer.cpp` and `editor/EditorGameOS.cpp` out of the frame loop;
  `tools/asset_viewer/` and `tests/` out-of-engine / offline (prefix-matched). The **frozen
  backlog** (existing render TUs: `gameos_graphics.cpp`, `gameosmain.cpp`, `gos_postprocess.cpp`,
  `gos_particle_bridge.cpp`, `gos_vfx_mesh_bridge.cpp`, `gos_terrain_lod_chunk.cpp`,
  `gos_vegetation.cpp`, `gos_mech_batcher.cpp`) is allowlisted by exact file so it doesn't
  retro-fail; documented as the v1 freeze. Mode B currently reports 72 raw sites across 12 files.
- **v1 = file-level freeze** (backlog files may add raw calls; new files may not) →
  **v2 tightening = per-file count ceiling** (record per-file baseline counts from Mode B so
  even backlog files can't ADD raw calls). v1 is intentionally simple.
- **Run:** Mode A (enforcement, default) `py -3 scripts/check-raw-gl-depthmask.py [--base REF] [--quiet]`;
  Mode B (audit, advisory exit 0) `py -3 scripts/check-raw-gl-depthmask.py --all` lists every raw
  site with file:line + per-file counts. Wired into `scripts/check-contracts.sh` as `raw_gl_depthmask`
  (right after `raw_gl_depthfunc`). Regression-proof: freezes the backlog, blocks new bypasses.
- `check-raw-gl-colormask.py` — **RAW-GL-COLORMASK-DIFF-GATE-1** (Phase 8 third enforcement gate). See note below.

#### RAW-GL-COLORMASK-DIFF-GATE-1 (`scripts/check-raw-gl-colormask.py`)

Exact clone of the depth-mask gate above, swapped to the `glColorMask()` axis (the color-WRITE
mask). Diff-based static check that bans **new** raw `glColorMask()` / `glColorMaski()` call sites
on ADDED lines outside a sanctioned-file allowlist; the regex `\bglColorMaski?\s*\(` catches BOTH
the plain four-channel form AND the indexed per-draw-buffer (MRT) form. Mode A (default) diffs vs a
base ref (default `merge-base HEAD origin/main`, or `--base`), inspects only ADDED lines, exit 1
naming each offender when a non-allowlisted file gains a raw `glColorMask(i)`. Non-overlapping with
the PipelineDesc state-table checks (`check-colormask-ownership.py`) — it bans the raw *call site*,
steering color-mask state through the sanctioned emitter (`pipeline_binder.cpp` applyPipeline, which
emits `glColorMaski` from `PipelineDesc`) or the `GlScopedColorMask` RAII wrapper (`gl_state_guard.h`).

- **Allowlist rationale:** same shape as the depth-mask gate — chokepoint emitter + RAII wrapper
  sanctioned by design; `debug_renderer.cpp` and `editor/EditorGameOS.cpp` out of the frame loop;
  `tools/asset_viewer/` and `tests/` out-of-engine / offline (prefix-matched). The **frozen
  backlog** (existing render TUs: `gameos_graphics.cpp`, `gos_postprocess.cpp`,
  `gos_static_prop_batcher.cpp`, `RenderCore/ambient_contract.h`) is allowlisted by exact file so it
  doesn't retro-fail; documented as the v1 freeze. **These include the LOAD-BEARING save/restore
  sites the default-ON ambient guard depends on** — the shadow-pass `glColorMask(GL_TRUE,...)`
  re-assert in `gameos_graphics.cpp` and the `glColorMaski` MRT masks in `gos_postprocess.cpp` —
  so they MUST stay allowlisted, not banned. Mode B currently reports 33 raw sites across 5 files
  (gos_postprocess.cpp: 4 plain + 12 indexed; gameos_graphics.cpp: 11 plain; pipeline_binder.cpp:
  3 indexed; gos_static_prop_batcher.cpp: 2 plain; ambient_contract.h: 1 plain).
- **v1 = file-level freeze** (backlog files may add raw calls; new files may not) →
  **v2 tightening = per-file count ceiling** (record per-file baseline counts from Mode B so
  even backlog files can't ADD raw calls). v1 is intentionally simple.
- **Run:** Mode A (enforcement, default) `py -3 scripts/check-raw-gl-colormask.py [--base REF] [--quiet]`;
  Mode B (audit, advisory exit 0) `py -3 scripts/check-raw-gl-colormask.py --all` lists every raw
  site with file:line + per-file counts (plain vs indexed). Wired into `scripts/check-contracts.sh`
  as `raw_gl_colormask` (right after `raw_gl_depthmask`). Regression-proof: freezes the backlog,
  blocks new bypasses.
- `check-raw-gl-blendfunc.py` — **RAW-GL-BLENDFUNC-DIFF-GATE-1** (Phase 8 fourth enforcement gate). See note below.

#### RAW-GL-BLENDFUNC-DIFF-GATE-1 (`scripts/check-raw-gl-blendfunc.py`)

Exact clone of the color-mask gate above, swapped to the blend FUNC/EQUATION axis. Diff-based static
check that bans **new** raw blend-func/equation call sites on ADDED lines outside a sanctioned-file
allowlist; the regex `\bglBlend(Func|FuncSeparate|Equation|EquationSeparate)i?\s*\(` catches the whole
family: `glBlendFunc`, `glBlendFunci`, `glBlendFuncSeparate`, `glBlendFuncSeparatei`,
`glBlendEquation`, `glBlendEquationSeparate` (+ `i`). It deliberately does **not** gate
`glEnable(GL_BLEND)` / `glDisable(GL_BLEND)` — blend on/off is globally managed per the ambient-ledger
lesson; only the blend FUNC/EQUATION is the meaningful contracted state. Mode A (default) diffs vs a
base ref (default `merge-base HEAD origin/main`, or `--base`), inspects only ADDED lines, exit 1 naming
each offender when a non-allowlisted file gains a raw blend-func/equation call. Non-overlapping with the
descriptive PipelineDesc checks — it bans the raw *call site*, steering blend state through the
sanctioned emitter (`pipeline_binder.cpp` applyPipeline, which emits `glBlendFunc` from `PipelineDesc`)
or a scoped blend wrapper (`gl_state_guard.h`).

- **Allowlist rationale:** same shape as the color-mask gate — chokepoint emitter + RAII wrapper
  sanctioned by design; `debug_renderer.cpp` and `editor/EditorGameOS.cpp` out of the frame loop;
  `tools/asset_viewer/` and `tests/` out-of-engine / offline (prefix-matched). The **frozen backlog**
  (existing render TUs: `gameos_graphics.cpp`, `gos_postprocess.cpp`, `gos_particle_bridge.cpp`,
  `gos_vfx_mesh_bridge.cpp`) is allowlisted by exact file so it doesn't retro-fail; documented as the
  v1 freeze. Mode B currently reports 35 raw sites across 6 files (gameos_graphics.cpp: 16 Func;
  pipeline_binder.cpp: 6 Func; gos_particle_bridge.cpp: 6 Func; debug_renderer.cpp: 2 FuncSeparate +
  2 EquationSeparate; gos_vfx_mesh_bridge.cpp: 2 Func; gos_postprocess.cpp: 1 Func).
- **v1 = file-level freeze** (backlog files may add raw calls; new files may not) →
  **v2 tightening = per-file count ceiling** (record per-file baseline counts from Mode B so even
  backlog files can't ADD raw calls). v1 is intentionally simple.
- **Run:** Mode A (enforcement, default) `py -3 scripts/check-raw-gl-blendfunc.py [--base REF] [--quiet]`;
  Mode B (audit, advisory exit 0) `py -3 scripts/check-raw-gl-blendfunc.py --all` lists every raw
  site with file:line + variant + per-file counts. Wired into `scripts/check-contracts.sh` as
  `raw_gl_blendfunc` (right after `raw_gl_colormask`). Regression-proof: freezes the backlog, blocks
  new bypasses.
- `check-raw-gl-fbobind.py` — **RAW-GL-FBO-BIND-GATE-1** (Phase 8 fifth raw-GL axis gate). See note below.

#### RAW-GL-FBO-BIND-GATE-1 (`scripts/check-raw-gl-fbobind.py`)

The fifth — and highest-context — raw-GL axis gate. Bans **new** unsanctioned `glBindFramebuffer`
call sites on ADDED diff lines outside a sanctioned-file allowlist, so the FBO-bind backlog cannot
grow new bypasses of the sanctioned FBO-redirect chokepoint (executor `applyTopLevelGenericAxes` /
the FBO-owner TUs). Regex `\bglBindFramebuffer\s*\(`.

**Not a flat clone of the four scalar axis gates.** An FBO bind is higher-context than a scalar state
setter: FBO identity is a *logical resource*, not a flag, and many binds are load-bearing save/restore
brackets that RETURN to a previously-captured FBO (cannot redirect to a new target). So this gate uses
a **hybrid taxonomy** with three escape paths:
1. **file allowlist** — sanctioned FBO-owner / executor / editor TUs (exact: `gos_postprocess.cpp`
   create/init/resize/PP-apply + registerFbo, `gameos_graphics.cpp` executor + shadow brackets,
   `gameosmain.cpp` screenshot/video READ-blit, `gos_terrain_indirect.cpp` water-reflection
   prev-restore, `RenderWorld.cpp` readback, `editor/EditorGameOS.cpp`, `editor/EditorInterface.cpp`;
   prefix: `tools/`, `tests/`).
2. **`// FBO-OWNER: <reason>` tag** — a deliberate new owner in a non-allowlisted file escapes by
   tagging the added line OR the immediately-previous added line.
3. **restore-pattern exempt** — a bind whose added line targets a `prev`/`saved`/`csmSaved*` variable
   (regex `\b(prev|saved|csmSaved)\w*`, case-insensitive) is a save/restore RETURN, not a new
   redirect → auto-exempt.

Mode A (default, enforcement) diffs vs a base ref (default `merge-base HEAD origin/main`, or `--base`),
inspects only ADDED lines, exit 1 naming each offender when a non-allowlisted file gains a raw bind
that is neither a restore nor FBO-OWNER-tagged. Mode B (`--all`, advisory exit 0) lists every site with
file:line + class tag (ALLOWLISTED / RESTORE / OWNER-TAGGED / RAW) + per-file/by-class counts —
currently **82 sites across 11 files, all ALLOWLISTED, 0 RAW** (freeze-forward baseline).

- **v1 = file-allowlist + `// FBO-OWNER:` tag + prev/saved-restore exempt** (static only, no registry
  dependency). **v2 (after RESOURCE-REGISTRY-COMPLETE-1) = require the FBO-OWNER tag to name a
  ledger-registered logical resource** + optional runtime resolve != Unknown.
- **Run:** Mode A `py -3 scripts/check-raw-gl-fbobind.py [--base REF] [--quiet]`; Mode B
  `py -3 scripts/check-raw-gl-fbobind.py --all`. Wired into `scripts/check-contracts.sh` as
  `raw_gl_fbobind` (right after `raw_gl_blendfunc`). Regression-proof: freezes the backlog, blocks
  new bypasses.
- **Fifth and final raw-GL axis gate.** The five shipped axes — depth-func, depth-mask, color-mask,
  blend-func/equation, and FBO-bind — cover the meaningful pipeline-state setters plus the logical
  render-target redirect. The **raw-GL-bypass CAPSTONE** (a unifying meta-gate / no-raw-GL-from-game
  consolidation) comes after this gate.
- `check-raw-gl-capstone.py` — **RAW-GL-BYPASS-CAPSTONE-1** (Phase 8 capstone meta-gate). See note below.

#### RAW-GL-BYPASS-CAPSTONE-1 (`scripts/check-raw-gl-capstone.py`)

The Phase-8 capstone. **Not another per-axis gate — it is the enforcement-COMPLETENESS meta-gate**
that validates the raw-GL enforcement SET ITSELF, so the set cannot silently regress (a gate file
deleted, an un-wired `run_check`, or a gate quietly gutted to an empty stub). It also records the
KNOWN-DEFERRED raw-GL axes as an explicit ledger so "no un-gated escape" is a documented decision,
not a silent gap.

**What it asserts (Mode A, default; exit 1 on any failure):** for each of the 5 enforced axes —
(a) the gate SCRIPT EXISTS in `scripts/`; (b) it is WIRED into `check-contracts.sh` (a `run_check`
line invokes it); (c) the gate script CONTAINS its expected axis token (sanity that it gates the
right axis and is not an empty stub). Mode B (`--all`) additionally prints the full enforced +
deferred matrix (advisory) but still validates and returns the same exit code.

**The 5 ENFORCED axes** (canonical `ENFORCED_AXES` list in the script): `depthFunc`
(check-raw-gl-depthfunc.py), `depthMask` (check-raw-gl-depthmask.py), `colorMask`
(check-raw-gl-colormask.py), `blendFunc` (check-raw-gl-blendfunc.py), `fboBind`
(check-raw-gl-fbobind.py).

**KNOWN-DEFERRED axes** (intentionally NOT gated — explicit, not forgotten; `KNOWN_DEFERRED` list):
- `glViewport` — render-target-mode state owned by the executor applyTopLevel* path; too many
  load-bearing save/restore brackets, no stable per-callsite contract yet.
- `glActiveTexture / glBindTexture` — texture-unit latch is far too numerous (every sampler bind)
  and is covered descriptively by the sampler-occupancy manifest (`check-sampler-bindings.py`).
- `glScissor / glEnable(GL_SCISSOR_TEST)` — narrow UI/clip brackets only; low blast radius, no
  migration chokepoint defined.
- `glStencil* (Func/Op/Mask)` — effectively unused in the current frame; no backlog to freeze, no
  contracted owner — a gate would be empty.
- `glCullFace / glFrontFace` — cull state carried in `PipelineDesc` (CullMode), emitted by the
  sanctioned applyPipeline; a raw-callsite gate would duplicate existing descriptive coverage.

**Maintenance rule** (keeps the set self-describing): adding a **new** raw-GL axis gate → add a row
to `ENFORCED_AXES` in the capstone AND wire its `run_check` into `check-contracts.sh`; **promoting a
deferred axis** to gated → MOVE it from `KNOWN_DEFERRED` into `ENFORCED_AXES` and ship its gate +
wiring. The capstone then enforces the new gate exists + is wired + is non-stub.

- **Run:** Mode A `py -3 scripts/check-raw-gl-capstone.py [--quiet]`; Mode B
  `py -3 scripts/check-raw-gl-capstone.py --all` prints the matrix. Wired into
  `scripts/check-contracts.sh` as `raw_gl_capstone`, AFTER the 5 axis gates (it validates them, so it
  runs last). With Phase 8 the raw-GL bypass surface is now **regression-proof**: the 5 axis gates
  freeze the per-axis backlogs and block new bypasses, and this capstone freezes the gate SET so it
  cannot silently lose a gate.

- `check-include-firewall.sh`, `check-no-raw-gl-from-game.sh`, `check-vfx-no-objectid.sh`, `check-visibility-log-schema.sh`, `check-unified-projection-retirement.sh`, `check-mlr-leaves-gated.sh`, `check-particles-no-cpu-projection.sh` — RenderWorld-boundary / lane firewalls (PARTIAL contract coverage)
- `validate_shaders.py` — glslang SPIR-V compile gate; uses `--auto-map-bindings` (compile-only; **does NOT record sampler occupancy** — symptom of the blind spot, not coverage)

### Docs
- `docs/render-contract.md` — coordinate-space / submission-bucket contract (PARTIAL, half-stale per 2026-05-14 audit)
- `docs/renderpass-contract-spec.md` — 5-lane render-pass registry spec (FULL)
- `docs/render-binding-registry.md` — hand doc; **superseded** by `binding-slot-occupancy.md`; sampler table (`:104-127`) self-admittedly incomplete
- `docs/render-backend-seams/binding-slot-occupancy.{md,json}` — current buffer-slot occupancy (authoritative)
- `docs/render-backend-seams/gpu-buffer-owner-recon-1.md` + `gpu-buffer-wrapper-design-1.md` — buffer-lifetime master inventory + `GpuRingBuffer<N>` design (extend, don't duplicate)
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
- `view_uniforms.hglsl` — `layout(binding=3, std140) ViewUniformsBlock` (mirrors ViewUniforms.h)
- `scene.hglsl` — `SCENE_DATA_ATTACHMENT_SLOT 1` (UBO)
- `lighting.hglsl` — `LIGHT_DATA_SSBO_BINDING 20`, legacy `LIGHT_DATA_ATTACHMENT_SLOT 0`
- `material_gpu.hglsl` — `MaterialTable` SSBO binding=5 (mirrors MaterialGpu.h)
- **No** `binding_slots.hglsl` exists yet, and **no** GLSL sampler carries `layout(binding=)` — units live only in trailing comments

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
2. **SHADER-SAMPLER-BINDING-MANIFEST-1** ✅ **SHIPPED 2026-06-22** — `scripts/check-sampler-bindings.py` + `sampler-unit-occupancy.{md,json}`, registered as `sampler_bindings`. Derive-from-code (not a hand golden); FAILs on intra-program contradiction + non-multiplexed comment-vs-code drift; WARNs on the multiplex/UNKNOWN-binder surface. Verified: clean tree PASS exit 0; planted `sceneTex` drift → FAIL exit 1; reverted clean. Records target per sampler → subsumes the 2D_ARRAY-leak residual. **Next within this dimension (future):** migrate literal loc-cache binders (terrain/mech/static-prop) to named binds + a generated `sampler_units` constant pair, to drop the `UNKNOWN`/WARN rows.
3. **RENDER-PASS-DAG-CONTRACT-1** — widen `RenderPassId` to cover the 10 orphans (append-only before `_SentinelLast`, update table + static_assert same commit); add `PassIdentity→RenderPassId` mapping fn (keep PassIdentity — 10 live callsites); add load/store + layout + `dependsOn[]` fields to `RenderPassContract`; emit ordered `kFramePassOrder[]` with the shadow-before-geometry edge; cross-ref (don't merge yet) `PassStateContract` blend/depth/MRT.
4. **GPU-UPDATE-BUFFER-COUNTER-1** — measurement-only, reuse `g_mc2HitchAccum`, gate `MC2_GPUBUF_COUNTER`, HUD pilot. Byte-identical output; tier1 5/5 is the gate.
5. **XFORM-CONVENTION-LEDGER-1** — single ledger of per-subsystem transform ownership (mech bone-fold / static model-matrix / terrain global / ViewUniforms). Docs-only.
6. *(deferred, higher risk, needs 2-5 landed)* GPU-RESOURCE-MANAGER-GL-ONLY-1 (first true backend seam) · SHADER-PIPELINE-OFFLINE-ARC (runtime `#define` injection inventory).

**Do NOT** build a flat global binding enum (multiplexed model is correct), a big `IRenderBackend`/`RenderDevice`/`CommandContext` (premature per seam recon), or resurrect `gpu-buffer-wrapper-design-1.md` §3 flat slot-enum (superseded).

## Per-slice preflight discipline

Shared-recon-queue rule: every slice runs `repo_query.py slice-preflight` (or `mcp__mc2-repo-intel__slice_preflight`) against nifty HEAD before coding; `verdict=STOP` → re-recon. All file:line citations here are current as of `0390b805` — cheap to drift; re-grep before relying on a line number.
