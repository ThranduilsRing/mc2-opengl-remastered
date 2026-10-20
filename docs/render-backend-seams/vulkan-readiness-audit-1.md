<!-- doc-status: current | reviewed:2026-10-11 -->
# VULKAN-READINESS-AUDIT-1

> s68/P9: Retired GL sources are cited with Git object syntax (`8064c6930:path`) so the historical implementation remains inspectable.

Wholesale audit of how Vulkan-shaped the MC2 renderer is, and where it's GL-shaped /
lax-where-it-should-be-strict. 8 parallel dimension sweeps (2026-06-22). Read-only;
no port work implied. Capstone of the RENDER-BACKEND-SEAMS arc — sits alongside
`opengl-correctness-ledger-1.md`, `gpu-buffer-owner-recon-1.md`, `gpu-buffer-wrapper-design-1.md`.

> **Master index:** see `docs/render-backend-seams/render-contract-index-1.md` — the single map of
> every render-contract artifact and which Vulkan-readiness dimension each covers. Read it before
> scoping any VULKAN-CONTRACT-MANIFEST-ARC slice; the dominant risk is re-recon of things already on disk.
>
> **(reconciled 2026-06-22 RENDER-CONTRACT-INDEX-1)** — several findings below predated shipped
> slices and were inheriting wrong premises. Corrected claims carry an inline
> `(corrected 2026-06-22 RENDER-CONTRACT-INDEX-1)` marker. Net: binding registry SHIPPED (multiplexed, CI-checked);
> water thin-ring fenced; `gos_UpdateBuffer` near-dead (not the orphan liability); pass-DAG skeleton exists;
> the one TRUE absent contract dimension is sampler/texture-unit bindings.

Scoring: **READY** (already explicit/Vulkan-shaped) · **LAX** (GL-implicit; a seam to add, not a rewrite) · **BLOCKER** (no Vulkan equivalent or a design change).

---

## Headline

The renderer is **further along than a typical GL codebase** — the GPU-driven arc already produced Vulkan-shaped patterns: fenced N-frame coherent rings (mech/static-prop), immutable `glBufferStorage` VBO/IBO, explicit compute→draw `glMemoryBarrier`s on the indirect/cull paths, an async 3-slot readback ring (exemplary), separate samplers, dynamic-offset UBOs, and a worker-threads-never-touch-GL invariant (flag + assert). No bindless dependency, no `gl_FragDepth`, viewport/scissor already dynamic.

**The dominant theme is "strictness tooling built but not adopted."** Seams were *designed/built* during this arc and sit at varying adoption:
- `gl_state_guard.h` RAII guards (GlScopedCapability/DepthState/TextureUnit) — **defined, proven, deployed in ~1 site; the other ~8 GPU-direct passes still hand-roll save/restore.**
- Buffer binding-slot lockstep — **SHIPPED, not "not built" (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).** GPU-BINDING-SLOTS-REGISTRY-1 landed as a CI/check-time lockstep + occupancy checker, NOT a flat runtime enum. The flat-`GpuBindingSlots`-enum premise was **overturned**: buffer base-binding slots are intentionally **multiplexed per-pass** (slot 0 is bound by 7 different buffers across passes), so a slot number is semantic only *inside* a pass/pipeline — a flat enum would encode a false model. Artifacts: `scripts/check-binding-slots.py` (FAILs on named C++/GLSL pair mismatch or same-file slot collision; WARNs on bare GLSL literals / documented cross-pass reuse) + `docs/render-backend-seams/binding-slot-occupancy.{md,json}`.
- **Sampler / texture-unit bindings — the one TRUE absent contract dimension (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).** `check-binding-slots.py` is **buffer-only** (samplers explicitly out of scope per its docstring — that is the `GL_TEXTURE_*` namespace, not the buffer-binding-base namespace). Texture units are scattered C++ `glUniform1i` literals coupled to GLSL only by hand-comment. This is the next slice: **SHADER-SAMPLER-BINDING-MANIFEST-1**.
Adopting what already exists — and closing the sampler gap — is the cheapest Vulkan-prep available.

---

## Readiness scorecard by dimension

| # | Dimension | Verdict | Already Vulkan-shaped (READY) | Lax / seams (LAX) | Blockers |
|---|---|---|---|---|---|
| A | Resource lifetime & memory | **mostly READY** | mech/static-prop fenced coherent rings; immutable `glBufferStorage` VBO/IBO; terrain-chunk/postprocess static buffers | per-frame `glBufferData` orphaning. **Real churn = raw GL + the private 5-arg `updateBuffer()` overload (`GameOS/gameos/utils/gl_utils.cpp:435`, HUD/`gosMesh`, not hitch-accounted)** (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1). `gos_UpdateBuffer` is **near-dead** (1 runtime callsite, `mclib/txmmgr_render_phases.cpp:370` scene-data UBO) — NOT the big liability earlier text implied. Plus: mech-material SSBO, dynamic-prop shadow SSBO, mine/decal static VBO, light-data SSBO, HUD meshes | none hard (no DSA `glCreateBuffers` anywhere = stylistic) |
| B | Synchronization & barriers | **mostly READY** | compute→draw barriers present (terrain indirect/water `:1935`, gpu_cull `:1297`/`:1118`, lighting `:845/857`); fenced rings + readback fence | CPU-coherent-write→draw relies on `GL_MAP_COHERENT` w/o explicit barrier (mech instance/bone, static-prop instance, mech-material) — works in GL, Vulkan needs host-write→shader-read barrier | none |
| C | Pipeline state vs global state | **NOT READY** | viewport/scissor already dynamic; render state is C++-side (not in GLSL) | depth/cull/blend set via scattered mutable global GL (12–15 sites each); `gl_state_guard` RAII **defined but adopted in 0–1 sites** | `stateCacheValid_` single-bit cache model has no Vulkan analog; no pipeline-state descriptor exists |
| D | Descriptor / binding model | **partial** | separate VkSampler-style samplers; dynamic-offset UBO; no bindless deps; SSBO-only (no image bindings); **buffer base-binding slots now CI-checked for C++/GLSL lockstep (`check-binding-slots.py`), occupancy documented (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1)** | **buffer slots are intentionally multiplexed per-pass (no flat enum — that premise was overturned); the TRUE blind spot is SAMPLER / texture-unit bindings** (scattered C++ `glUniform1i` literals, GLSL-coupled by hand-comment only; out of `check-binding-slots.py` scope) → SHADER-SAMPLER-BINDING-MANIFEST-1; per-bucket texture rebinds in loops | cross-pipeline slot reuse (slot 0/2/9/11) needs descriptor-set-layout isolation under Vulkan |
| E | Render-pass / FBO model | **partial** | **pass-DAG skeleton EXISTS, not greenfield: `RenderCore/RenderPassContract.h` carries `reads[4]`/`writes[4]`/`barrierAfter` per pass (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1)**; CONTRACT-3 ordering audit + ENFORCEMENT-1 scope tracker | **missing for Vulkan: load/store ops, image-layout transitions, an ordered pass list, and rows for ~10 orphan passes**; FBO setup ad-hoc per pass; contract not wired into draw sites; reads[] incomplete (postprocess G-buffer reads undeclared) | serialized-pass-graph + explicit attachment load/store + layout transitions need the contract completed + enforced |
| F | Shaders → SPIR-V pipelines | **NOT READY (design change)** | no `#version` in files (injected); `gl_FragDepth` absent; derivatives correct (UB2-01/02/05 fixed); state decoupled from GLSL; geometry path dormant | hot-reload (fails silently; can't recompile SPIR-V at runtime); sampler decls lack `layout(binding=)` in some shaders | runtime `#define` prefix-injection (`shader_builder.cpp` makeShader) → must offline-compile SPIR-V or use specialization constants; permutation enumeration (94 `#ifdef` across 81 shaders) |
| G | Immediate-mode / legacy GL | **READY except gl_utils** | game render path is fully VBO/shader; fixed-function alpha/matrix in `gameosmain` are `#if 0`; HUD is VBO (per-frame upload) | HUD per-frame `glBufferData` → trivially `glBufferSubData`/ring | **TRUE legacy** in `8064c6930:GameOS/gameos/utils/gl_utils.cpp`: `glBegin/glEnd` + `glVertex` + fixed-function matrix stack + `GL_QUADS` in `draw_quad`/`drawCube`/`draw_in_2d` (confirm liveness — likely debug utils) |
| H | Readbacks / picking / command model | **85% READY** | picking = one-shot click readback (accepted); GPU-cull async 3-slot readback ring w/ non-blocking `glClientWaitSync(timeout=0)` (exemplary); workers CPU-only (guarded) | water-RT + diagnostic readbacks (periodic, gated) | persistent-mapped coherent staging relies on implicit GL coherence → Vulkan needs explicit barrier + semaphore (audit AMD BAR coherence); single-context implicit (recommend single-queue v1) |

---

## Prioritized seam/blocker list (what to do, cheapest-first)

**Tier 1 — adopt strictness tooling already built (low risk, high Vulkan-prep value):**
1. **Deploy `gl_state_guard` RAII across the ~8 GPU-direct passes** (water, shadows, particles, mech/static-prop/terrain batchers, post-process). The guards are defined + proven (Slice-1 model) but unadopted; each site still hand-rolls save/restore. Collapses the `gos_InvalidateRenderStateCache` manual list, makes state explicit per-pass = the precursor to a pipeline-state object. *No behavior change.* (Dimension C.)
2. ~~Build the `GpuBindingSlots` registry~~ **DONE — GPU-BINDING-SLOTS-REGISTRY-1 shipped (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).** Delivered as a CI-time lockstep + occupancy checker (`scripts/check-binding-slots.py` + `docs/render-backend-seams/binding-slot-occupancy.{md,json}`), NOT a flat enum — the flat-namespace premise was overturned (slots are multiplexed per-pass). **Replacement live slice → SHADER-SAMPLER-BINDING-MANIFEST-1:** build `sampler-unit-occupancy.{md,json}` + a checker mirroring `check-binding-slots.py` for the texture-unit namespace (parse C++ `glUniform1i`+`glActiveTexture`/`glBindTexture` per program, cross-ref GLSL `uniform sampler*`; FAIL on same-program unit collision / sampler-never-assigned / target mismatch). This is the one TRUE absent binding-contract dimension. (Dimension D.)

**Tier 2 — complete the explicit-contract scaffolding (medium):**
3. **Complete + wire `RenderPassContract`** — the skeleton already exists (`RenderCore/RenderPassContract.h` has `reads[4]`/`writes[4]`/`barrierAfter`); this is NOT greenfield (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1). Add what Vulkan needs: per-attachment load/store ops, image-layout transitions, an ordered pass list, and rows for the ~10 orphan passes; complete `reads[]` (postprocess G-buffer reads undeclared); then wire into draw sites. (Dimension E.)
4. **Add explicit barriers on CPU-coherent-write→draw** (mech/static-prop instance+bone, mech-material): ~3 `gpuSyncBarrier` calls; matches Vulkan host-write→shader-read. (Dimension B.)
5. **GpuBuffer wrapper adoption, Tier-0 first** (lod_chunk → HUD → postprocess) to kill per-frame `glBufferData` orphaning and unify ring/fence. (Dimension A; design already done.)

**Tier 3 — design changes (defer; biggest effort):**
6. **Shader SPIR-V seam** — convert the `#define` prefix-injection point to offline SPIR-V baking / specialization constants; enumerate permutations (classify each `#ifdef` static-vs-dynamic). The single seam is `shader_builder.cpp` makeShader. (Dimension F.)
7. **Pipeline-state objects** — replace `stateCacheValid_` with per-pass pipeline IDs once #1 makes state explicit. (Dimension C.)
8. **Rewrite `gl_utils.cpp` immediate-mode** (`draw_quad`/`drawCube`/`draw_in_2d`) to VBO + UBO matrices — but **confirm these are live first** (likely debug-only utilities; if dead, delete instead). (Dimension G.)
9. **Explicit staging barrier+semaphore** for persistent-mapped readback in the eventual Vulkan port; single-queue design for v1. (Dimension H.)

---

## Notes / corrections
- **MOOT / FIXED — water thin-ring fence (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).** Any "water thin-record ring is unfenced" finding is resolved: WATER-THINRING-FENCE-1 SHIPPED. The ring waits on its slot fence before reuse (`GameOS/gameos/gos_terrain_water_stream.cpp:205` `WaitAndClearThinFenceForCurrentSlot`) and posts a post-draw fence (`:2154` `EndThinRingFrameFence`). All thin-rings (solid + water) and instance/bone rings are now fenced.
- The cross-cutting takeaway for "lax where we should be strict": the renderer keeps **building** strictness mechanisms (state guards, binding registry, pass contract, scope tracker) but **wiring them everywhere lags**. The highest-leverage Vulkan-prep is *adoption*, not new mechanism.
- This is a recon artifact. The first concrete slice off it is Tier-1 #1 (deploy gl_state_guard) — small, no-behavior-change, and it's the precursor every later tier depends on. Queue as a new arc slice when ready; not started here.
