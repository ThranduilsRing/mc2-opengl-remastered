<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2-OpenGL Modernization Roadmap

> ## ⭐ MILESTONE — 2026-06-10: TERRAIN 8z PRODUCTION CLOSEOUT — FUNCTIONALLY COMPLETE
>
> **The production game terrain renderer is now chunk/GPU-only.** State transition (future sessions: read this first):
> - **production game path:** chunk/GPU only (legacy retired from the production link)
> - **`setupTextures`:** editor-gated (`#ifdef MC2_IS_EDITOR`, commit `006800e5`) — game build has NO legacy fallback
> - **`slimReduce` / `MC2_TERRAIN_ACTIVE_AB`:** DELETED (~430 lines; commits `126a299a` / `11bba3f6`)
> - **tier1:** 5/5 PASS on the 8z build (FATAL=0, FASTPATH_DROP=0, terrain renders via chunk path)
> - **game + editor builds:** both GREEN
> - **A2 (delete makeLists) / A4 (delete `TerrainQuad::draw()`):** DEFERRED by explicit kill-switch (`MC2_TERRAIN_LOD_CHUNK=0` opt-out) / overlay-policy (`MC2_TERRAIN_INDIRECT_OVERLAY=0` regression guard) decisions — **NOT terrain-closeout blockers**
> - **Supporting commits:** `4f520eae` T16/T19 loud-fail · `6f5d243a` A5 mine-handle init · `9dd853dc` createWeaponBolt guard · `98af2c80` static-building skip (default-off) · `ad6cff3c` R2b trees default-on · `202a04fb` R2b rename
> - **Verified build:** the full game+editor 8z build is deployed + tier1-verified at **`A:/Games/mc2-opengl/mc2-win64-0.4c`** (the verified terrain-closeout build). v0.4 deploy left pending (convenience, not a gate).
>
> **NEXT GATE → Baseline A** (off 0.4c): golden frames · per-pass timings · FASTPATH_DROP=0 · terrain/path snapshot · oracle counters — captured **post-8z, pre-GlStateGuard / pre-visual-lanes**. That baseline gates opening the modernization backlog.
> **Do NOT yet:** merge Tube to mainline · start GlStateGuard · widen static-building/service-lane. Baseline A first.
>
> ### ✅ STATUS FOLD — 2026-06-10 (backlog now legitimately OPEN)
> - **Terrain closeout:** COMPLETE / verified (above).
> - **Baseline A:** CAPTURED + committed as **`82add3ca`** (`docs/baseline-A-post-8z.md`). 5/5 tier1, FASTPATH_DROP=0, slimVerts=0, terrain gpu-armed, render oracles clean.
> - **Oracle + perf-budget harness:** SHIPPED (`scripts/smoke_lib/oracleparse.py`, oracle_report.py [retired 2026-10-09], `docs/perf-budget.{md,json}`) — the shared parser/budget vocabulary that makes future runs comparable to Baseline A. OBJBATCHER late-register classified (benign WARN), not panicked.
> - **S2b:** CLOSED — mech bodies already GPU.
> - **gosFX Tube:** live **residual legacy lane** (A/B validation pending; merge decision deferred — not before harness).
> - **PertCloud:** DEAD.
> - **Per-pass timings:** **CPU attribution FILLED** via smoke cost-split (Baseline A §6a — MISSION_SPLIT / GEOM_PHASE_SPLIT / MIF_SPLIT; slim=0µs re-confirms slimReduce dead). **GPU per-pass still OPEN** (§6b) — scoped to a small per-pass GL `TIME_ELAPSED` instrumentation lane (only gpu_cull+water timed today), NOT a headless dead-end.
>
> - **GlStateGuard slice 1:** ✅ **ACCEPTED** — branch `claude/glstateguard-slice1`, commits `36a51bcf` (+ `f698e806` log-caveat). Target = GPU_CULL SSBO binding slots 7/15 via RAII `mc2gl::GlScopedSsboBinding` (compute pass, no depth/blend/cull → split-brain-safe). build green + `oracle_report --strict` EXIT 0 + perf at/above Baseline A. **Merge to nifty PENDING** branch-integration window (cherry-pick → tier1 + oracle_report --strict on nifty). Risk low. The structural loop (Baseline A → oracle_report --strict → perf budget → commit alone) is proven.
>
> **Disciplined next order:** (1) harness ✅ → (2) per-pass timing: CPU ✅, GPU = §6b GL-timer lane → (3) GlStateGuard slice 1 ✅ → (4) **merge slice 1 to nifty** (+ tier1/oracle re-check) → (5) **slice 2 recon = gosFX Tube ribbon bridge** (real render state: blend/depth/program/VAO — fresh recon + eyes-on validation; NOT "more guard because slice 1 worked") → (6) TerrainLOD indirect/submit (the scary pass, after Tube) → (7) HZB or asset-cook lane.

**Date:** 2026-06-09 (milestone updated 2026-06-10)
**Worktree:** `.claude/worktrees/nifty-mendeleev`
**Status:** Planning + terrain-closeout execution record. Terrain 8z closeout COMPLETE (see milestone above).
**Author role:** Senior engine-architecture planner.

Primary asset to protect: **the game runs well today.** Every recommendation below is
default-off, measurable, observable, single-lane, and reversible. No rewrite. No mega-branch.

---

## 1. Executive Synthesis

### Where the engine is now

MC2 is a late-90s DX6/7-era RTS engine ported to OpenGL, mid-transition from immediate-mode
CPU-baked rendering toward GPU-direct indirect rendering. The transition is **furthest along on
terrain** and **barely started on dynamic actors**.

- **Terrain** is the success story: the chunk renderer is default-on (`mc2TerrainLodChunkEnabled()`,
  opt-out `MC2_TERRAIN_LOD_CHUNK=0`), at legacy parity (colormap atlas, relief lighting + GBuffer,
  shadows, skirts/apron, explicit GL state ownership, splat/detail, reverse-Z). The legacy terrain
  path (`makeLists`/`geometry`/`slimReduce`/`TerrainQuad::draw`/`setupTextures`) still exists as a
  fast-path fallback gated by a **6-condition `fullyArmed` conjunction** + `MC2_QUADSETUP_ARMED_SKIP`.
  Deletion (8z) is blocked on fallback-semantics proof, not on the renderer.
- **Static objects** got a real win: terrain-object update cardinality collapsed ~4977→~145 via the
  R2b static-natural skip. Service-lane classification exists on paper (docs/archive/SERVICE-LANE-DECOMPOSITION.md).
- **Dynamic actors (mechs/vehicles/turrets) are entirely legacy:** `TG_Shape::MultiTransformShape`
  CPU-bakes every vertex to clip space every frame, then `addTriangle` → `renderLists()` flush. A
  dormant GPU-VB path (`bShadersDrawPathEnabled=false`) exists but is buggy. S2 was just re-scoped:
  measurement showed `MultiTransformShape` ≤1.2ms peak — the prime suspects are now present + the
  renderLists submit path, not the CPU transform. **Frame-time attribution must precede S2 surgery.**
- **Cross-cutting hazards:** projection is split-brain (D3D pixel-homog `terrainMVP` vs `worldToClipGL`),
  the cause of repeated cull/shadow/prop bugs. GL state is implicitly inherited across passes (the
  terrain transparency saga root cause). These are the two highest-risk domains.

### What "UE5/Unity-style modernization" should mean *here*

NOT a RenderWorld rewrite. For this codebase it means four incremental convergences:

1. **Explicit passes** — passes that own and restore their GL state (a lightweight FrameGraph, not a
   scheduler), replacing implicit state inheritance.
2. **GPU scene records** — actors/materials/lights/poses become data the GPU consumes via indirect
   draw, replacing per-frame CPU bake. Terrain already proves the pattern; extend it to actors.
3. **Cooked assets** — offline-baked terrain/props/materials/IBL, replacing runtime interpretation.
4. **Diagnostics as a first-class layer** — the env-gated logs, Tracy zones, tier smokes, oracle
   gates, and the `mc2-render-state` MCP become a standing oracle + perf-budget system, not ad-hoc.

### What to do next vs. not touch yet

- **DO next:** finish terrain closeout (R2b cleanup → FASTPATH_DROP steady-state → T3 map audit →
  editor chunk verify → 8z-A/8z-B → **Baseline A**). Then: GlStateGuard, then S2a attribution,
  then the diagnostics/oracle hardening that everything else depends on.
- **DO NOT touch yet:** F1 projection unification, the first HZB draw-consumer, shadow cherry-pick,
  water reflection, next-gen tactical LOD. All are high-value but high-risk and depend on terrain
  closeout + GL-state sanity + a frozen baseline existing first.

---

## 2. Survey by Focus Area

### A. RenderWorld / FrameGraph / RenderPass ownership

**Current legacy shape.** Render order is fixed and documented (architecture.md): `land->render()`
queues terrain → craters → `ObjectManager->render()` **draws objects immediately via MLR** → water
queue → `renderLists()` flushes in 8 phases (shadow pre-pass, terrain solid, detail alpha, overlay
craters, non-terrain craters, non-terrain alpha, water, terrain shadows). Post-process is a separate
RGBA16F → bloom → composite chain. Passes mutate global GL state implicitly and **inherit** it from
whatever ran before (proven failure mode: chunk terrain inherited `glDepthMask(FALSE)` → see-through).

**Target modern shape.** Each pass is an explicit object that declares its inputs (FBO/textures/UBOs),
its outputs, and **owns the GL state it depends on** (depth/blend/cull/depthfunc), restoring on exit.
A minimal FrameGraph is just an ordered list of these passes with explicit resource handles — no
dynamic scheduling, no auto-barrier inference. The existing fixed order becomes the declared edge set.

**Why it matters.** The split-brain projection bugs and the transparency saga are both
state/ownership-inheritance failures. Explicit pass state ownership is the structural fix that
unblocks HZB, shadows, and F1 safely.

**First slices.**
1. **RenderPass inventory doc** (recon only): enumerate every pass, its FBO, the GL state it sets vs
   inherits, who owns inputs/outputs. (this doc names ~8 renderLists phases + shadow + post.)
2. **`GlStateGuard` RAII** (see G): wrap GPU-direct passes, snapshot/restore depth/blend/cull,
   invalidate the gos state cache. Default-off env gate, A/B counter of state deltas.
3. **Pass-boundary assertion mode** (`MC2_PASS_STATE_ASSERT`): at each pass entry, assert the GL
   state matches what the pass declares it needs; log violations loudly. Off by default.
4. Convert the 2–3 GPU-direct passes (chunk terrain, static props) to explicit RenderPass structs
   that consume GlStateGuard — no behavior change, just ownership made explicit.

**Risks.** Touching shared flush order can destabilize the whole frame; keep order byte-identical.
**Dependencies.** Terrain 8z reduces legacy draw paths first (fewer passes to wrap).
**Validation.** Tier1 5/5 parity + pixel-diff at fixed frames + state-delta counter == 0.
**Model/agent.** Inventory: Haiku/Explore. GlStateGuard + assert mode: Sonnet. Sequencing: Opus.

---

### B. GPU Scene / dynamic actor modernization

**Current legacy shape.** `TG_Shape::MultiTransformShape` (mclib/tgl_transform.cpp:128) CPU-bakes every
mech/vehicle/turret/building vertex to clip space per frame; lighting + highlight + backface cull
baked into `gos_VERTEX`; one logical draw per shape through `addTriangle`. Dormant GPU path
(`TG_RenderShape`, `bShadersDrawPathEnabled=false`) has per-shape `vb_`/`ib_`/`mvp_` but multi-texture
FIXME, no alpha, double-submit bug. Paint is a texture-instance key (correct), highlight is per-vertex
(GPU path stale).

**Target modern shape.** Actors are GPU scene records: actor record (transform, material id, light
data index, pose/gesture stream pointer), material record (SSBO), light record (already partially
exists via `light_data_buffer_index_`). Pose/gesture becomes a streamed buffer. Draws become indirect.

**Why it matters.** This is the largest remaining CPU bake and the path to true GPU scene parity with
terrain. BUT: **S2 was just re-scoped — `MultiTransformShape` measured ≤1.2ms peak. The cost is now in
present + the renderLists submit path.** So the *first* dynamic work is attribution, not transform
surgery.

**Sequencing (revised, from perf-attribution-plan.md + docs/archive/superseded/S2_IMPLEMENTATION_PLAN.md superseded note).**
- **S2a — frame-time attribution (R0–R3):** instrument present + renderLists + submit; prove where the
  ms actually go now that terrain is <100µs. *This gates everything else dynamic.*
- **S2b — `MultiTransformShape` elimination, only if S2a justifies it:** activate `TG_RenderShape` for
  mechs-only behind `MC2_TG_GPU_XFORM=1`, fix multi-texture + highlight, keep addTriangle fallback.
- **S4 — material → SSBO:** once actors submit GPU-side, hoist material params to an SSBO.
- **S5 — HZB dynamic cull:** only after the static HZB consumer (E/H) is proven and a margin/discontinuity
  guard exists.
- **S6 — gesture/pose table:** stream poses as a buffer; last because it needs S2b's record layout.

**Stays CPU gameplay:** AI, pathing, weapon logic, target selection, damage. **Becomes render data:**
transforms, materials, lights, poses, draw args.

**Risks.** Highlight/paint/alpha/spotlight/window special-cases silently regress; double-submit. The
GPU path has known bugs. **Dynamic work must live in its own worktree/branch** — it must NOT
contaminate terrain-closeout baselines.
**Dependencies.** Baseline A frozen first; oracle gates (MECH_MATERIAL_GPU/MATERIAL_GPU/TEX_RESOLVE/
RENDER_SNAPSHOT) are the per-stage verifier.
**Validation.** Oracle mismatch == 0 + pixel-diff at mc2_17 f600.
**Model/agent.** S2a attribution: Sonnet (instrumentation). S2b: Sonnet recon + Opus risk-gate.

---

### C. Asset / material cook pipeline

**Current legacy shape.** Runtime interpretation: terrain colormap is `.burnin.jpg`/`.tga` decoded to
full RGBA8 RAM (no VRAM win); BC7/KTX2 path is unfinished (`ktx.exe create` rejects raw BC7).
Materials are partly dead data (PBR factors unused until IBL/PBR consumes them). Mech textures bake
paint at load. HDRI exists for planned IBL. Cook folders are appearing (`tools/asset_cook/`,
`tools/mc2texcook/`, asset-pipeline.md, asset-manifest-schema.md).

**Target modern shape.** Offline-cooked, GPU-ready data: BC7/KTX2 terrain colormap (texconv/nvtt →
`ktx create --raw`), cooked prop meshes + LOD chains, prebaked material records, prefiltered IBL
cubemap + SH from the HDRI. Runtime *loads*, never *interprets*. A single asset manifest binds them.

**What stays runtime-dynamic:** team paint instancing, damage/decal state, dynamic lights, anything
gameplay-mutable.

**First slices.**
1. **Asset cook inventory doc** (recon): what's interpreted at runtime today, what's cookable, current
   tools, gaps. (Anchor: asset-pipeline.md, asset-manifest-schema.md, data-provenance.md.)
2. **Terrain colormap BC7/KTX2 finish:** texconv/nvtt raw-BC7 → `ktx create --raw`; this is a
   self-contained VRAM/RAM win with an existing sampling path (UV-decoupled, downscale-safe).
3. **IBL prefilter cook** from the existing HDRI (cubemap + SH) — the enabling slice for the visual
   roadmap; no runtime consumer yet so it's safe to land cold.
4. **Material record schema** — define the cooked material struct that S4 SSBO and PBR will consume.

**Risks.** Cook output drift vs runtime expectation; manifest becoming a second source of truth.
**Dependencies.** None blocking for colormap/IBL cook; material schema should precede S4 + PBR.
**Validation.** Decode-parity oracle (cooked vs runtime pixels), extents oracle, screenshot gate.
**Model/agent.** Inventory: Haiku/Explore. Cook scripts: Mini/Codex. Schema: Sonnet.

---

### D. Visibility / LOD / tactical importance

**Current legacy shape.** Distance-only: terrain LOD by camera distance; props by distance; actors
draw if on-screen. No notion of tactical relevance.

**Target modern shape.** An **importance service** produces a per-entity score from cheap signals
(distance, on-screen, selected, in-combat, sensor-visible, threat). LOD/presentation consume the
score: model → silhouette → icon → formation marker as score drops. Fog/LOS/sensors gate visibility;
zoom gates the presentation ladder.

**Why it matters.** This is the next-gen RTS thesis. But it's long-horizon — treat as enabling slices
only, not a campaign yet.

**Smallest useful slice.**
1. **Importance score as pure read-only data** (`MC2_IMPORTANCE_SCORE`, off): compute + log a score
   per actor from existing signals (distance, selected, on-screen). No consumer. Proves the signal.
2. **Zoom-band telemetry:** log which presentation band each entity *would* be in at current zoom.
   Still no behavior change.
3. (Later) wire score → existing actor LOD selection as the first consumer, default-off.

**Risks.** Scope explosion; coupling render to gameplay state badly. Keep it a read-only service first.
**Dependencies.** Actor GPU records (B) make consumption clean; until then it's pure telemetry.
**Validation.** Score-stability logs across frames; no behavior change in slices 1–2.
**Model/agent.** Recon + telemetry: Sonnet. Design: Opus (thesis framing).

---

### E. Editor-as-engine-client

**Current legacy shape.** Editor shares the engine binary but `drawTerrainGrid` editor mode **depends
on legacy `setupTextures`/draw** (H2 confirmed) — so editor forces a fast-path drop. Editor overlays
(passability, grid) are bespoke draws, not passes. Editor runs default-on modern chain but self-skips
legacy terrain draw in places. Editor chunk-path verification is unresolved.

**Target modern shape.** Editor is a *client* of the same RenderPasses + GPU scene. Overlays become
explicit editor-only RenderPasses layered on the shared frame. Editor validation feeds the asset cook
+ map-compatibility audit. Shared: terrain renderer, scene records, asset load. Editor-only: overlays,
gizmos, selection, authoring tools.

**Why it matters.** Editor/game divergence is a first-class risk (the deploy-target trap already cost a
full cycle). Quarantine is a stopgap; parity is the goal.

**First slices.**
1. **Editor chunk-path verify** (the current closeout item): does the chunk renderer work under editor
   mode? Document parity gaps.
2. **8z-B compile-gated editor quarantine** — prefer an editor-build/compile gate over a fragile shared
   runtime global bool, so 8z-A game deletion can proceed without breaking editor.
3. **Editor chunk-path parity doc** — what `drawTerrainGrid` needs from legacy, and how to provide it
   on the chunk path (e.g. an editor overlay pass that draws the grid independent of `setupTextures`).
4. (Later) overlays → explicit editor RenderPasses; validation → cook/audit hooks.

**Risks.** Quarantine drifts into permanent fork; editor silently keeps legacy alive forever.
**Dependencies.** Terrain 8z-A; RenderPass ownership (A) for overlay-as-pass.
**Validation.** Editor smoke (audit-lanes-editor/) + visual parity on a known map.
**Model/agent.** Verify + parity doc: Sonnet. Compile-gate: Mini/Codex. Quarantine decision: Opus.

---

### F. Diagnostics / oracle / perf-budget infrastructure

**Current legacy shape.** Rich but ad-hoc: env-gated logs (`[FASTPATH_DROP]`, `[TerrainLOD prod]`,
`MC2_FX_COUNT_LOG`, A/B FN counters), 18 Tracy zones, tier1/3 smokes, oracle gates
(MECH_MATERIAL_GPU/MATERIAL_GPU/TEX_RESOLVE/RENDER_SNAPSHOT), the `mc2-render-state` MCP (state dump
every 300 frames). No standing golden baselines, no perf budgets, no log parsers.

**Target modern shape.** A first-class oracle layer: named golden traces + baselines per fixture,
perf budgets per pass (terrain <100µs is already implicit — make it explicit), runtime invariants
that fail loudly, and log parsers that turn the env-gated streams into pass/fail in CI smoke.

**Why it matters.** Every safe slice in this roadmap is "default-off + measurable." That requires the
measurement layer to be standing, not reconstructed per task. This is the cheapest highest-leverage
investment.

**First slices.**
1. **Golden baseline / perf-budget doc:** define per-fixture golden frames, per-pass budgets
   (terrain solid, shadow, 3D objects, post, present), and the parse rule for each env log.
2. **`[FASTPATH_DROP]` parser** → classify steady-state drops as the 8z-A gate (already needed).
3. **Oracle gate harness:** wrap the 4 mech oracle gates + terrain FN counters into one
   pass/fail script consumed by tier smokes.
4. **mc2-render-state baseline capture** as the canonical "is the frame healthy" probe.
**Risks.** Oracle rot (baselines drift, nobody updates). Make updates a committed artifact.
**Dependencies.** None — this should run *alongside* closeout and is a prerequisite for B/HZB/F1.
**Validation.** Self-validating (the layer *is* validation). 
**Model/agent.** Parsers/harness: Mini/Codex. Budget design: Sonnet. 

---

### G. Resource lifetime, GL state, pass isolation

**Current legacy shape.** GL state inherited across passes; gos state cache can desync; the chunk
terrain draw had to learn to set ALL its state explicitly (depth/blend/cull/depthfunc) after the
transparency saga. No RAII wrapper; cleanup is manual and inconsistent.

**Target modern shape.** `GlStateGuard` RAII wraps every GPU-direct pass: snapshot on enter, restore
on exit, invalidate gos state cache so the cache can't lie about post-pass state.

**Why it matters.** This is the **structural prerequisite** for HZB, shadows, water reflection, and F1
— all of which add passes that will inherit/leave state. Doing them *before* state isolation repeats
the transparency-saga class of bug.

**State hazards to eliminate first:** `glDepthMask` left FALSE by transparent passes; `GL_BLEND`
left on; cull winding flipped by X-mirror MVPs; depthfunc (reverse-Z GEQUAL) assumptions; gos cache
desync after raw GL calls.

**Order:** terrain 8z (fewer legacy draw paths) → **GlStateGuard** → then HZB/shadow/water/F1.

**First slices.**
1. **GlStateGuard RAII struct** + `MC2_GLSTATEGUARD` env gate + state-delta counter.
2. Wrap chunk terrain + static-prop passes; assert zero net state delta across the frame.
3. **gos-cache invalidation hook** on guard exit.
**Risks.** Over-restoring costs perf; mis-snapshotting masks a real bug. Counter-driven.
**Dependencies.** Terrain 8z reduces surface area; should land before HZB/shadow expansion.
**Validation.** State-delta counter == 0; tier1 parity; pixel-diff.
**Model/agent.** Sonnet (implementation), Opus (ordering vs feature work).

---

### H. Runtime / gameplay service separation

**Current legacy shape.** `GameObjectManager::update` is monolithic. docs/archive/SERVICE-LANE-DECOMPOSITION.md
classifies objects: gates/turrets/spotlights/special-buildings (alarm/lookout/sensor) = **gameplay-
service every frame, non-negotiable**; trees/ordinary buildings/gate-control/turret-control/power-gen
= **pure render-static or event-gated**. R2b already collapsed ~4977→~145 updates by skipping
static-natural objects.

**Target modern shape.** Explicit service lanes: a `render-static` lane (cached, event-driven on
state change like destruction) vs a `gameplay-service` lane (every frame). Static objects move to
cached data + events; only the classified gameplay-service set ticks per frame.

**Why it matters.** R2b proved the win is huge. Formalizing the lanes makes it durable and extends to
the next dense-map blowups.

**First slices.**
1. **R2b diagnostics cleanup** (current closeout item): rename tree/Pine terminology → pure-static/
   static-natural; keep compact health counters; **do not rename env vars tooling depends on.**
2. **Event-driven move-map contributors:** bridges/forests/walls affect move map only on destruction —
   convert per-creation one-time + on-destroy event, drop any per-frame check.
3. **Formal lane split** of `GameObjectManager::update` into render-static vs gameplay-service,
   default-off behind a flag, with a per-lane object count counter.
**Risks.** Mis-classifying a gameplay-load-bearing object as static (gates/turrets MUST tick). The
classification table is the contract — honor it exactly.
**Dependencies.** None blocking; independent of render lanes.
**Validation.** Gameplay smoke (gates open, turrets track, alarms fire) + update-count counter.
**Model/agent.** Cleanup: Mini/Codex. Lane split: Sonnet. Classification review: Opus.

---

## 3. Ranked Top-15 Modernization Backlog

| # | Title | Goal | Prereq | Payoff | Risk | First prompt type | Why ranked here |
|---|-------|------|--------|--------|------|-------------------|-----------------|
| 1 | R2b diagnostics cleanup | Rename tree→static-natural, compact counters | none | Closeout unblock | Low | Mini/Codex mechanical patch | Active closeout, trivial, in-flight |
| 2 | FASTPATH_DROP steady-state classify | Prove zero unclassified drops (8z-A gate) | run + parser | Unblocks 8z-A | Low | Sonnet recon + Mini parser | Hard gate for terrain deletion |
| 3 | T3 map compatibility audit | No prod/mod non-colormap map without atlas/fallback | FASTPATH parser | Unblocks 8z-A | Med | Sonnet audit doc | Hard gate for terrain deletion |
| 4 | Editor chunk-path verify + parity doc | Does chunk render under editor? | none | Unblocks 8z-B | Med | Sonnet recon | Editor divergence is first-class risk |
| 5 | 8z-A/8z-B execution | Delete legacy terrain (game) + compile-gate editor | #2,#3,#4 | Retire legacy path | Med-High | Sonnet deletion plan + Opus gate | The terrain closeout payoff |
| 6 | Baseline A capture | Freeze golden frames + perf budgets post-closeout | #5 | Every later slice's reference | Low | Mini capture + Sonnet budget | Nothing downstream is safe without it |
| 7 | Diagnostics/oracle harness + budget doc | Standing oracle + parsers + budgets | #6 | Enables all default-off slices | Low | Mini/Codex + Sonnet | Cheapest high-leverage infra |
| 8 | GlStateGuard RAII | Pass state ownership, gos-cache invalidate | #5,#6 | Unblocks HZB/shadow/water/F1 safely | Med | Sonnet | Structural prereq for risky domains |
| 9 | S2a frame-time attribution | Find real ms now terrain <100µs | #6 | Targets dynamic work correctly | Low | Sonnet instrumentation | Must precede any S2 surgery |
| 10 | Terrain colormap BC7/KTX2 cook | texconv/nvtt → ktx --raw | none | RAM/VRAM win, cook-pipeline proof | Med | Mini/Codex script | Self-contained asset-pipeline first slice |
| 11 | IBL prefilter cook | Cubemap+SH from HDRI, no consumer | none | Enables PBR/visual roadmap | Low | Mini/Codex + Sonnet | Safe to land cold; unblocks visuals |
| 12 | GPU particle real-age | Sample curves at real age not 0.5 | #6 | High-value visual win | Low-Med | Sonnet (own worktree) | Cheap visible win, isolated |
| 13 | Service-lane formal split | render-static vs gameplay-service lanes | #1 | Durable update-cardinality win | Med | Sonnet | Extends R2b; honor classification |
| 14 | S2b TG GPU-xform (mechs) | Activate TG_RenderShape behind flag | #9 justifies | Largest CPU-bake retirement | High | Sonnet recon + Opus gate | Only if S2a justifies; own branch |
| 15 | RenderPass inventory + first passes | Make 2–3 GPU passes explicit | #8 | FrameGraph foundation | Med | Haiku inventory + Sonnet | Foundation for A; after StateGuard |

---

## 4. Dependency Graph

```
terrain closeout chain:
  R2b cleanup ─► FASTPATH_DROP steady-state ─► T3 map audit ─┐
                                          editor chunk verify ─┤
                                                              ▼
                                              8z-A delete (game) + 8z-B quarantine (editor)
                                                              ▼
                                                        BASELINE A (golden + budgets)
                                                              ▼
                          ┌───────────────┬──────────────────┼───────────────────┐
                          ▼               ▼                  ▼                   ▼
                 Diagnostics/oracle   GlStateGuard      S2a attribution    asset-cook inventory
                    harness               │                  │                   │
                          │               ▼                  ▼                   ├─► colormap BC7/KTX2
                          │      HZB consumer / shadow   S2b TG GPU-xform        ├─► IBL prefilter ─► PBR specular
                          │      / water reflection / F1   (own branch)         └─► material schema ─► S4 SSBO
                          ▼               (each its OWN branch)                          ▲
                  perf budgets ◄──────────────────────────────────────────────── S4 ◄──┘
                                                                                  ▼
                                                                          S5 HZB dyn cull ─► S6 gesture table

independent lane (no render dep):
  R2b cleanup ─► event-driven move-map contributors ─► service-lane formal split

long-horizon (gated on B GPU records):
  importance score (telemetry) ─► zoom-band telemetry ─► score→LOD consumer ─► tactical field rendering
```

Key orderings (as requested):
- **terrain closeout → Baseline A → S2a attribution**
- **8z/H2 fallback proof → GlStateGuard → HZB/shadows/water/F1** (each separately)
- **asset cook audit → IBL prefilter → PBR specular**
- **R2b cleanup → service-lane split** (independent of render)

---

## 5. Do-Next Plan (after current closeout)

Immediate stack assumed done: R2b cleanup → FASTPATH_DROP steady-state → T3 map audit →
editor chunk verify → 8z-A/8z-B → **Baseline A**.

Next 5 actions, in order:

1. **Diagnostics/oracle harness + perf-budget doc** (F). Turn the env logs, oracle gates, FN
   counters, and mc2-render-state into one standing pass/fail + budget layer. *Everything default-off
   after this leans on it.* — Mini/Codex parsers + Sonnet budget design.
2. **GlStateGuard RAII** (G). Wrap chunk-terrain + static-prop passes; state-delta counter == 0;
   gos-cache invalidate. *Structural prerequisite before any new pass.* — Sonnet.
3. **S2a frame-time attribution** (B), in its **own worktree**. Instrument present + renderLists +
   submit; prove where ms go now terrain is <100µs. Do NOT start S2b until this report exists. — Sonnet.
4. **Terrain colormap BC7/KTX2 cook** (C). Self-contained asset-pipeline first slice; texconv/nvtt →
   `ktx create --raw`; decode-parity oracle. — Mini/Codex.
5. **IBL prefilter cook** (C). Cubemap + SH from the existing HDRI, no runtime consumer yet — lands
   cold, unblocks PBR + the visual roadmap. — Mini/Codex + Sonnet.

(GPU particle real-age is a strong alternative #4/#5 if a visible win is wanted sooner — it's isolated
and cheap; run it in its own worktree so it never touches a baseline capture.)

---

## 6. Defer List (and exactly why)

- **F1 projection unification** — Highest-risk domain (split-brain is the source of the cull/shadow/
  prop bug class). It collapses dual clip-space ownership across *all* passes at once. Defer until
  GlStateGuard exists, Baseline A is frozen, and it can be a dedicated campaign on its own branch.
- **First HZB draw-consumer** — Cull/projection is high-risk; historical clip-space bugs. Needs
  GlStateGuard + a default-off spec with margin, discontinuity guard, and A/B counters first.
- **Shadow cherry-pick to nifty** — v0.4 crashes at `batcher.cpp:4778` shadow-on; needs GL-state
  sanity (G) and terrain closeout first so it isn't debugged against a moving target.
- **Water reflection (WATER-REFLECTION-CLIP-1)** — Needs reflected projection + oblique near-plane;
  that's projection-domain work — wait for F1 direction or at least GlStateGuard.
- **Full RenderWorld rewrite** — Explicitly out of scope. Incremental passes only.
- **Next-gen tactical LOD / importance / tactical field rendering** — Long-horizon thesis; depends on
  GPU actor records (B). Only the read-only telemetry slices are safe now.
- **S2b TG GPU-xform** — Defer until S2a attribution *justifies* it (measured ≤1.2ms; the cost moved).

---

## 7. Dangerous Mega-Branches to Avoid

- **8z terrain deletion + S2 dynamic actor rendering** — Two different subsystems, two baselines.
  Mixing means a dynamic-actor regression masquerades as a terrain-deletion bug. Separate worktrees.
- **F1 projection + HZB cull consumer + water reflection** — All three are clip-space/projection
  work. Combined, any cull/shadow/transparency artifact is unbisectable. One projection change per
  branch, validated alone.
- **GlStateGuard + shadow cherry-pick + dynamic cull** — All mutate GL state ownership. Combined, a
  state-restore bug and a shadow-crash and a cull-margin bug are indistinguishable. StateGuard lands
  and proves zero-delta *alone* first.
- **GPU particle real-age + Baseline A capture** — Particle age changes pixels; capturing a baseline
  while it's in flight poisons the golden frames. Particle work goes in its own worktree, never the
  baseline branch.
- **Editor quarantine + 8z-A game deletion in one commit** — If editor breaks you can't tell whether
  it's the deletion or the quarantine. Land the compile-gate, prove editor, *then* delete.
- **Asset cook + any renderer change** — Cook drift vs runtime expectation must be isolated so a
  decode-parity failure is attributable to the cook, not a shader edit.

---

## 8. Missing Docs / Recons (write before code)

- **RenderPass inventory** — every pass, FBO, GL state set vs inherited, input/output owners. (A)
- **S2a frame-time attribution report** — where ms go now terrain <100µs. (B) *Gates all S2 work.*
- **Asset cook inventory** — runtime-interpreted vs cookable, current tools, gaps. (C)
- **Editor chunk-path parity doc** — what `drawTerrainGrid` needs from legacy; chunk-path provision. (E)
- **T3 map compatibility audit** — prod/mod maps without colormap atlas + fallback definition. (closeout)
- **Golden baseline / perf-budget doc** — per-fixture golden frames, per-pass budgets, log parse rules. (F)
- **GlStateGuard design + state-hazard catalog** — enumerate every state left dirty across passes. (G)
- **Material record schema** — cooked material struct for S4 SSBO + PBR. (C)
- **`[FASTPATH_DROP]` classification spec** — what counts as an unclassified steady-state drop. (8z-A)

---

## 8b. Campaign Ownership Table

The contract that prevents "which session owns which modified files?" recurring. **Before any
mutating work, an agent confirms its worktree+branch match the owning row, and never touches files
owned by another campaign.** Update the `current status` / `next gate` cells as work lands.

| Campaign | Owning branch / worktree | Current status | Next gate | Do-NOT-mix-with |
|----------|--------------------------|----------------|-----------|-----------------|
| Terrain closeout | `claude/terrain-gen-pcg` (nifty-mendeleev worktree) | H2; R2b `202a04fb`; FASTPATH auto GREEN; T3 CLEARS; editor recon+8z plan DONE; quarantine mechanism = `MC2_IS_EDITOR` (exists) | **2 user-driven empirical confirms** (interactive game FASTPATH + visible editor arming) → A5 mine/overlay migration verify → 8z-B then 8z-A incremental | S2 / F1 / HZB / shadows / water |

**Closeout gate status (terrain campaign):**
- **R2b diag rename:** ✅ committed `202a04fb` (build green; tier1 verified, mc2_17 heartbeat flake passed on isolated rerun).
- **FASTPATH_DROP automated proof:** ✅ FIRST-PASS GREEN. tier1 + `MC2_FASTPATH_DROP_LOG=1`: 0 transitions (0 steady-state, 0 warmup) across 5 missions. Interpretation: fully armed *before* first per-frame geometry() call (textures load before render). Log path confirmed live by adjacent telemetry (`ComputeDispatch`/`[TerrainLOD flush]` run); independent systems agree (`[RENDER_SNAPSHOT] fallback=0`, `[OBJBATCHER] cpu_fallback=0`). Not "log was dead" — "nothing transitioned." **Caveat: idle fly-through only.**
- **FASTPATH_DROP interactive proof:** ⏳ PENDING (hand-driven session; exercises pan/destruction/mine-overlay states the fly-through misses). Not worth a `MC2_TERRAIN_LOD_CHUNK=0` forced-fallback gamble — that flag is semantically "disable the thing we're deleting."
- **T3 map audit:** ✅ CLEARS — docs/terrain-t3-map-audit.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz). 0 T3 blockers / 0 unknowns across 140 maps (57 base + DarkRain/PicturesOfARebeliion/TangoMaster/cveg mods). Predicate confirmed OR (`hasTileNodes || hasAtlas`); atlas-presence sufficient. Residuals (disclosed, non-blocking): mod dirs outside the 0.4c deploy not swept; runtime `setColorMapName` override not callsite-exhaustive.
- **Editor chunk verify / quarantine:** ✅ RECON DONE + partial empirical. `docs/editor-chunk-path-parity.md`. **Editor smoke (11/12; foliage_present pre-existing fail) + direct editor launch with `MC2_FASTPATH_DROP_LOG=1` confirmed: chunk path IS enabled in editor (`[TerrainLOD v1] ENABLED drawPath=chunk`).** BUT the **headless/minimized editor renders NO terrain** — frustum planes all `(0,0,0,0)` (degenerate viewport) → 0 draw commands after frame 4 → zero-cmd ERROR streak → `terrain_mid_px=RGBA(0,0,0,255)` → `[TERRAIN_INDIRECT_PARITY] *_packed_quads=0`. So **the headless editor smoke is structurally BLIND to terrain arming/rendering**; "0 FASTPATH_DROP" there is uninformative (nothing rendered). Confirms recon open-Q2 (zero-cmd guard fires in editor). **Editor-arming confirm STILL needs a VISIBLE interactive editor session (user-driven) — cannot be done headless.** Editor renders terrain surface via the **GPU-indirect** path (legacy per-quad `draw()` already suppressed under chunk=ON, terrain.cpp:2221). The chunk LOD mesh (`flushDrawCommands`, code/gamecam.cpp:582) is **absent** from the editor chain — latent, not a visual gap. **Only legacy dependency = `setupTextures()`→`clipInfo`→`drawLine()` for the passability grid when `drawTerrainGrid==true` (T13).** Quarantine = **Option A: compile-gate `setupTextures`/`draw`/`slimReduce` behind an editor-build flag** (unblocks 8z-A with no editor regression); later Option C (add `flushDrawCommands` to editor chain + decouple `drawLine` from setupTextures clipInfo) retires the `drawTerrainGrid` global entirely. **Empirical TODO (editor session):** run `MC2_FASTPATH_DROP_LOG=1` in the editor to confirm the GPU-indirect path arms (no continuous fallback) and characterize the `drawTerrainGrid` fallback.
- **8z deletion plan:** ✅ DONE — `docs/terrain-8z-deletion-plan.md`. **8z-B (editor quarantine) lands BEFORE 8z-A**, using the **already-existing `MC2_IS_EDITOR` compile gate** (editor/CMakeLists.txt:130, on EditRel; already used in missiongui.cpp) — gates `setupTextures`/`draw()`/`slimReduce`/`drawTerrainGrid` to the editor build. 8z-A ordered + risk-tagged: A1 retire `MC2_TERRAIN_ACTIVE_AB` (LOW) → A2 makeLists (LOW) → A3 slimReduce (LOW/MED) → A4 `TerrainQuad::draw()` (MED, blocked on overlay/T8 decision) → A5 `setupTextures` (**HIGH, BLOCKED — verified `docs/terrain-8z-A5-setuptextures-deletion-safety.md`**) → A6 dead-env removal (LOW). **A5 blocker is REAL + has a fix:** `mineTextureHandle`/`blownTextureHandle` (static, quad.h:82-83) are assigned ONLY in setupTextures (quad.cpp:691/698), loaded during the 1-3 warmup frames before arming. Deleting setupTextures → handles stay `0xffffffff` → `BuildMineTextureArray` bails → **mine tiles silently black forever.** Overlay handle (R7-b) is SAFE (mission-load face cache, mapdata.cpp:299). **Required pre-A5 fix (small):** add `InitMineTextureHandles()` to the GPU-indirect init near `primeMissionTerrainCache` (terrain.cpp:~971), replicating the `loadTexture("mine_00.tga"/"minescorch_00.tga")` calls. **Test:** `MC2_SETUPSKIP_WARMUP=1` (force skipSetup from frame 0) on a mine mission → mines must still render. **Loud-fail (T16/T19) required before A5** (ForceDisableArmingForProcess silent→PANIC_LOG). **Biggest remaining blocker = A5 mine/overlay migration verification.**
- **mc2_17 smoke-reporter false crash_silent:** filed as separate infra task (does not block closeout unless it starts hiding real crashes).
- **Editor headless blindness (methodological finding):** the editor smoke/headless launch cannot validate terrain rendering (degenerate frustum → black). Editor terrain validation requires a VISIBLE session OR a future fix to give the headless editor a valid viewport projection. Worth a separate infra note if editor terrain regressions need automated coverage.
| Baseline A capture | terrain-closeout branch (after 8z lands) | not started | golden frames + per-pass budgets frozen | particle age, any in-flight pixel change |
| Diagnostics/oracle harness | own branch (post-Baseline A) | not started | parser + budget doc + oracle harness green | — (infra, consumed by all) |
| GlStateGuard | own branch | not started | state-delta counter == 0 on wrapped passes | shadow cherry-pick, dynamic cull |
| S2 dynamic actors | **separate worktree (non-negotiable)** | attribution only | S2a frame-time report exists | terrain baseline, F1, HZB |
| Visual particles | **separate worktree** | not started | real-age visual gate | Baseline A capture |
| Asset cook | own branch | inventory pending | cook inventory doc → colormap BC7/KTX2 decode-parity | any renderer/shader change |
| F1 projection | dedicated future campaign | not started | GlStateGuard landed + Baseline A frozen | HZB, water reflection, shadows |

Rule of thumb: **one campaign = one worktree = one branch.** Cross-campaign edits are a red flag —
stop and re-confirm ownership.

## 9. Agent Routing

| Task class | Model/agent | Examples |
|------------|-------------|----------|
| Read-only grep/recon, small summaries | **Haiku / Explore** | RenderPass inventory sweep, asset cook inventory, "where is X drawn" |
| Mechanical patches, env-gated logs, small fixes, scripts, parsers | **Mini / Codex** | R2b rename, FASTPATH parser, cook scripts, IBL prefilter, counter wiring |
| Cross-system code recon + deletion plans | **Sonnet** | 8z deletion plan, S2a instrumentation, GlStateGuard impl, service-lane split, audits |
| Orchestration / design-risk sequencing only | **Opus** | branch sequencing, mega-branch gating, 8z-A/8z-B go/no-go, F1 campaign framing |

**Worktree safety rules (mandatory for any mutating agent):**
- Mutating agents MUST be given the **absolute worktree path** and `cd` into it.
- First action: print `pwd`, `git rev-parse --show-toplevel`, `git rev-parse --abbrev-ref HEAD`.
- **Abort** if branch or worktree do not match the prompt's stated target.
- Never edit the stale **root** tree (root CLAUDE.md is a pointer; authoritative tree is the worktree).
- Dynamic / particle / S2 work goes in a **separate worktree** so it never touches a terrain baseline.
- Deploy-target check: game runs from `mc2-win64-v0.4/`, editor from `mc2-win64-0.4c/` — verify the
  deployed exe mtime ≥ fix commit before claiming a fix is live.

---

## 10. Final Synthesis — North Star

**RenderGraph/FrameGraph** — an ordered list of explicit RenderPasses, each owning and restoring the
GL state it depends on. Not a scheduler; the existing fixed render order becomes the declared edge set.

**GPU Scene** — actors/materials/lights/poses are GPU records consumed via indirect draw. Terrain
already proves it; mechs/vehicles/turrets follow once S2a says where the cost really is. Gameplay
(AI/pathing/weapons) stays CPU; transforms/materials/lights/poses/draw-args become render data.

**Cooked assets/materials** — offline-baked terrain (BC7/KTX2), props + LOD, material records, IBL
(cubemap+SH). Runtime loads, never interprets. One manifest binds them. Team paint / damage stay dynamic.

**Runtime service lanes** — render-static (cached, event-driven) vs gameplay-service (every frame,
the classified gates/turrets/special-buildings set). R2b is the proof; formalize and extend.

**Editor as engine client** — same passes + scene; overlays become editor-only passes; validation
feeds the cook + map audit. Quarantine is a stopgap; parity is the destination.

**Diagnostics/oracles as first-class systems** — golden baselines, per-pass budgets, oracle gates,
log parsers, mc2-render-state as the standing health probe. This is the layer that makes every other
slice default-off, measurable, and reversible.

### First five moves
1. Finish terrain closeout → **Baseline A** (the reference everything else needs).
2. **Diagnostics/oracle harness + perf-budget doc** (make measurement standing).
3. **GlStateGuard** (pass state ownership — unblocks the risky domains safely).
4. **S2a frame-time attribution** in its own worktree (target dynamic work correctly).
5. **Terrain colormap BC7/KTX2 cook** + **IBL prefilter** (first asset-pipeline slices; IBL unblocks visuals).

Constraints honored: no rewrite, no giant RenderWorld branch, terrain/S2/F1/HZB/water/shadow never
combined, game stays stable, every slice default-off + measurable, editor divergence and clip-space
treated as first-class risks.
