# CPU → GPU Offload — Orchestrator (pinnable)

> **Role for a fresh session reading this:** You are the orchestrator for MC2's
> CPU → GPU rendering offload. Don't dive into individual milestones until you
> understand the end-to-end. Read this doc → skim the linked memory/specs →
> propose what to do next OR confirm what the user wants. Update this doc as
> milestones land.
>
> **Maintainer rules:** keep the Status Board accurate after each shipped
> milestone. Promote "Queued" → "In progress" → "Shipped" inline. Don't let
> this doc grow past ~250 lines — extract narratives to memory files and link.

---

## North Star

**Abandon the world-tile per-frame CPU work entirely.** MC2 is a 2002 D3D7
engine with ~40K terrain quads iterated 2-3× per frame on the CPU (one pass
per: setupTextures, draw, drawMine, renderWater, plus shadow). On modern
hardware this is the dominant frame cost. Each milestone shifts one slice of
that work to the GPU until the final state is a few SSBOs + a handful of
`glDrawArrays`/`glDrawElements` calls per frame.

The work is incremental, stays parity-validated against the legacy renderer
at every step, and gates each path behind env vars until smoke-clean. **Stock
install must remain playable** at every step (CLAUDE.md critical rule).

### Scope — stock missions only

Validation gates for **all** CPU→GPU offload slices (M-family, Shape-family,
renderWater, vertexProjectLoop, indirect-draw endpoint, future water-to-GPU)
target **stock content only**. The canonical regression set is tier1's five
hand-picked missions: `mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`.

**Out of scope for this workstream:** Carver5O, Magic, MCO Omnitech, Wolfman,
MC2X, and any other mod content. They modify rendering inputs in ways the
stock engine doesn't expect (oversized assets, custom ABL extensions,
non-stock component IDs); validating offload slices against them is signal-
conflated. If a slice ships clean on stock and a mod regresses, that is the
mod's problem to fix — not a blocker for the slice.

Adjacent mod-content workstreams (mc2x-import, omnitech-abl, Carver5O
stability) remain SEPARATE and continue validating against their own content.
Don't cross-contaminate. Full rationale + how-to-apply: `memory/feedback_offload_scope_stock_only.md`.

---

## Status Board

> **Update protocol:** when a milestone ships, move its row from "In progress"
> to "Shipped" and add the perf/correctness result. When a new spec is
> approved, add it to "Queued" with a one-line scope.

### Shipped

| Milestone | What it did | Result |
|---|---|---|
| **M0b** | Terrain solid persistent-VBO seam | Visually correct, ±1 FPS vs legacy |
| **M0d** | Flush efficiency (bucket reuse) | Reduced bucket setup overhead |
| **M0e** | Direct texture bind (skip mcTextureManager dispatch) | Smaller per-bucket cost |
| **M0f** | appendQuad batching | Fewer per-quad function calls |
| **M1** | Compact `TerrainQuadRecord` (fat records, 192B) | SSBO-based path established |
| **M1d** | Thin records (48B) split per-frame data from per-quad recipe | Triple-buffered ring + persistent recipe cache |
| **M1e** | Skip expanded vertex staging when thin records active | Removed redundant VBO writes |
| **M1f** | Skip legacy solid staging when fast-path active | Cut `addVertices(DRAWSOLID)` calls |
| **M1g** | Thin-record draw via dedicated VS + `GL_TRIANGLES` (was `GL_PATCHES` + TCS + TES) | ~21ms GPU → ~3-5ms GPU; eliminated CPU fence stall |
| **M2 base** | Compact thin record 48 → 32B; pack TerrainType into recipe | 33% smaller per-frame upload |
| **M2b** | Loop-level pure-water hoist + in-function early-exit | drawPass 25 → ~12ms (skip 28K wasted iterations) |
| **M2c** | Water-interest detail quads enter fast path | drawPass ~12 → ~6ms (5,800 quads off legacy) |
| **M2c-ext** | terrainHandle==0 + detail subpath | Marginal (~89μs) — most candidates were overlay-bearing |
| **GL_FALSE → GL_TRUE for thin VS `projection_`** | Fixed Gate 1 visual bug | Unblocked thin-VS-only validation |
| **M2d-overlay** | Absorb overlay quads into fast path; inline `gos_PushTerrainOverlay` after thin record + detail emits | drawPass 5-6ms→1.46ms, fast=14000 legacy=0, tier1 5/5 PASS (`258e584`) |
| **Shape-C flip** | `MC2_MODERN_TERRAIN_PATCHES` default-on; cache-read for terrain texture-handle resolution | quadSetupTextures 3.47→3.17ms (-8.6%), 19.7M parity checks, 0 mismatches (`aee39cc`) |
| **quadSetupTextures slice 2a** | `addTriangleBulk` lift in `addTerrainTriangles` — single slot-walk per (handle, flags) tuple instead of paired `addTriangle` calls | 3.17→3.06ms (-0.11ms) |
| **quadSetupTextures slice 2b** | Mine-state cache — cache mine/blown classification per quad on the recipe entry | 3.06→3.01ms (-0.05ms), σ 384→291 µs (`53f09ca`) |
| **quadSetupTextures arc — asymptotic** | Recon: water-vertex projection block measured at 11% of self-time (8K calls × 42 ns = 341 µs). Below 30% threshold; further slices below σ noise floor. Arc concludes at cumulative -13% mean / -39% σ. | **Pivot to renderWater.** |
| **renderWater architectural slice — Stage 1+2+3 shipped, slice closed (2026-04-30)** | Map-stable WaterRecipe (built from `MapData::blocks`) + per-frame WaterThinRecord SSBO + GPU-direct draw via `Terrain::renderWaterFastPath()` post-`renderLists()`. Stage 3 added `MC2_RENDER_WATER_PARITY_CHECK` byte-comparison instrumentation. **All four gates green:** A visual canary clean, B Tracy delta 78–85% reduction (legacy 449–894 µs → fastpath 88–132 µs across mc2_01/03/10/17/24, exceeds ≥50% target), C parity-check silent-on-pass with ~3.2M quads byte-checked / zero mismatches, D tier1 5/5 PASS triple (unset / FASTPATH=1 / FASTPATH=1+PARITY_CHECK=1) with +0 destroys delta. Three real bugs surfaced and fixed during Stage 3 bring-up (recipe coverage, blank-vertex skip, fogRGB material patch) — would have shipped silently without parity. Reusable template lifted to `memory/water_ssbo_pattern.md`. 9 GPU-direct gotchas codified in `memory/gpu_direct_renderer_bringup_checklist.md`. |
| **vertexProjectLoop slice — D1 hoist asymptotic (2026-04-30)** | D1 CPU loop hoist (locals into registers, branch prediction, scratch globals → L1) shipped behind `MC2_VERTEX_PROJECT_FAST=1` (default off). **Compiler-ceiling outcome:** mean Δ +0.04% (475→475 µs) — the optimizer had already captured everything the hoist could move. σ tightened −10% (67→61 µs); P99/P99.9 came in slightly. Parity scaffolding shipped: `MC2_VERTEX_PROJECT_PARITY=1` with 96M verts byte-checked, zero mismatches across tier1. **Slice closed asymptotic at the trivial-hoist level.** Cost-decomposition surfaced: real floor is the math itself — `trans_to_frame` 3×3 mat-vec, `1/objectCenter.y` reciprocal-divide latency (~72 µs floor for 14400 verts), 2× `GetApproximateLength`, `projectZ` 4×4 matmul on survivors. Future SIMD or GPU-compute attempt has scaffolding ready (env-gate + parity infra), but is not queued — see indirect-terrain decision below. Lessons in `memory/vertexproject_loop_asymptotic.md`. |
| **Indirect terrain draw plan v2 — SOLID-only PR1 SHIPPED (Stages 0-3)** | The architectural endpoint of the CPU→GPU offload arc landed across four staged commits: `9bfcddc` (Stage 0 scaffolding — env gates, parity printer, counters), `bdb1628` (Stage 1 SOLID/detail-overlay cost split via per-frame timers), `094fa56` (Stage 2 dense recipe SSBO + per-mission Reset/Build), `f221570` (Stage 3 indirect SOLID draw + legacy SOLID gate-off — PR1 close). Plan v2 + supporting artifacts at `db3f947`. SOLID main-emit CPU iteration is now retired. Detail/overlay/mine remain on legacy paths (per A=(i) scope narrowing). Plan v1 stop-the-line at adversarial review (3 CRITICAL findings) was the inflection point that produced this clean shipping arc — the verify-then-write discipline encoded in worktree CLAUDE.md ensured plan v2 shipped without re-discovering stale prose. |

### In progress

| Milestone | Stage | Status |
|---|---|---|
| **Indirect terrain SOLID — merged colormap upload (sampler2D)** | Bug-fix + reusable infrastructure | **Stage 4 of PR1 (default-on promotion) BLOCKED on this.** Bug discovered post-Stage-3: indirect path binds sampler at unit 0 but never binds any texture; every quad samples whatever texture was last bound (typically overlay rock). Root cause: `TerrainColorMap` divides the map into `numTexturesAcross × numTexturesAcross` separate textures for legacy per-bucket binding. **Recon find (2026-04-30):** `Terrain::terrainTextures2->cpuColorMap` (`terrtxm2.cpp:1414-1418`) is the **full merged RGBA atlas already in CPU memory**, retained at mission load — the legacy pipeline splits it BACK into tiles for sampler binding, but the modern path skips the split. Fix shape collapses to: upload `cpuColorMap` as ONE sampler2D, add 3 uniforms (`mapTopLeft`, `oneOverWorldUnitsMapSide`, `numTexturesAcross`), VS derives tile coords from `recipe.worldPos0.xy` and computes `atlasUV = (tileXY + recipeLocalUV) / numTexturesAcross`. **Recipe + thin-record schemas UNCHANGED** — Stage 2's 231M parity checks continue passing. M2 path stays on sampler2D via separate `gos_terrain_indirect.frag`. Estimated ~80 LoC. The "sampler2DArray + layerIdx in _pad0" design considered earlier is superseded — single sampler2D suffices because the merged buffer already exists. |

**Stage 4 promotion of plan v2 PR1 is paused** until this slice lands. Default config remains unaffected (`MC2_TERRAIN_INDIRECT=1` is the only env state where the bug manifests).

### Queued (next)

| Milestone | Scope | Spec |
|---|---|---|
| **Indirect terrain draw — SOLID-only PR1** | Retire CPU SOLID main-emit setup loop in `quadSetupTextures` for terrain solid quads. Indirect SOLID packer + dense recipe SSBO + preflight-armed legacy bypass. Detail/overlay/mine remain legacy. Brainstorm Q1=(b) narrowed to SOLID-only at plan-revision time per adversarial-review findings. | Brainstorm: [`brainstorms/2026-04-30-indirect-terrain-draw-scope.md`](brainstorms/2026-04-30-indirect-terrain-draw-scope.md). Design: [`specs/2026-04-30-indirect-terrain-draw-design.md`](specs/2026-04-30-indirect-terrain-draw-design.md). Recon: [`specs/2026-04-30-indirect-terrain-recon-handoff.md`](specs/2026-04-30-indirect-terrain-recon-handoff.md). Revision brief: [`specs/2026-04-30-indirect-terrain-plan-v2-revision-brief.md`](specs/2026-04-30-indirect-terrain-plan-v2-revision-brief.md). Plan v1 superseded by v2 (in progress). |
| **Indirect terrain draw — detail/overlay/mine consolidation (follow-up)** | After SOLID-only PR1 ships and soaks: address detail (`addVertices(MC2_DRAWALPHA)`), overlay (`gos_PushTerrainOverlay`), and mine population state-cascade. Multi-bucket draw mechanism is a separate design question from SOLID retirement; needs its own brainstorm to settle whether `gl_DrawIDARB` + texture array, separate indirect calls, or single-command-with-per-quad-texture is the right shape. | None yet — brainstorm follow-up after SOLID PR1 soaks |
| **Indirect terrain draw — legacy retirement (post-soak follow-up)** | After both SOLID and detail/overlay/mine consolidation slices ship and soak: physically delete `TerrainPatchStream::flush()`, M2 thin-record-direct emit, M2b/M2c/M2d branches, and the opt-out env flag. Mechanical (rm + verify), no new design. | None yet — auto-queued post-soak |
| **Water vertex projection skip — fast-path stranded-upstream cleanup** | renderWater retired the water DRAW (post-renderLists fast path) but the legacy water CPU projection at `quad.cpp:773-1100` and per-vertex wAlpha displacement scattered across 8 sites still run every frame. Output (`addTriangleBulk(waterHandle, …DRAWALPHA\|ISWATER, 2)` at quad.cpp:1087-1088) is wasted post-renderWater-default-on. Gates the upstream CPU work behind the existing inline armed predicate at `terrain.cpp:1048-1051`. **Pattern: "fast-path with stranded upstream"** — same shape may exist in other slices that retired draws but not their CPU feeders. **Recon ready-for-brainstorm:** `wAlpha` is dead-data with zero readers; modern-`terrainTextures2` precondition makes legacy single-bitmap path consumers unreachable; the "second projection site at terrain.cpp:~1615" turned out to be `vertexProjectLoop`'s terrain (not water) accumulator — NOT a target for this slice. Scope closer to M2d-overlay than renderWater Stage 1+2+3. One residual risk: `leastZ/mostZ/leastW/mostW` accumulator contributions feeding `Camera::inverseProjectZ()` for mouse-pick depth — brainstorm picks between Option C (rely on renderWater Stage 3 parity + visual canary) or Option A (dev-only side-channel diagnostic). Findings: [`explorations/2026-04-30-water-projection-skip-recon.md`](explorations/2026-04-30-water-projection-skip-recon.md). | Recon complete; brainstorm pending. |
| **Shape D — pure dense recipe SSBO (decoupled from indirect-draw)** | **CLOSED — SUBSUMED 2026-04-30.** The hypothesis "ship Shape D as a precursor to indirect-terrain" became moot when indirect-terrain plan v2 Stages 0-3 shipped (`9bfcddc`/`bdb1628`/`094fa56`/`f221570`). Stage 2 dense recipe SSBO is live via `gos_terrain_indirect::g_denseRecipes`. Recon also surfaced that the "527 µs recoverable" framing was a category error — the cachedVisibleSubmission + resolveFallback Tracy zones wrap CPU work (`addTriangleBulk` + `pz_emit_terrain_tris`) that the legacy DRAW machinery still consumes; pure SSBO migration would have left those callers intact, with realistic recoverable ~100-200 µs not 527 µs. Findings: [`explorations/2026-04-30-shape-d-recon.md`](explorations/2026-04-30-shape-d-recon.md). | N/A — closed. |
| **quadSetupTextures admission / early guards** | **Recon CLOSED 2026-04-30** — recommendation: bundle as "Tracy hygiene + code clarity" micro-commit, NOT a perf slice. The 0.68 ms is overwhelmingly Tracy zone-emission overhead at 39,601 pairs (consistent with ~17 ns/pair); actual recoverable CPU is ~40-160 µs (two integer-comparisons × 39,601 calls on a per-mission-stable static). The "40K vs 14K = 50× amplification" framing was a misread: 39,601 = `(GameVisibleVertices-1)²` per frame from the hard-locked `GameVisibleVertices=200` at `mechcmd2.cpp:1481` (visible window > stock 120² map size; off-map quads receive `blankVertex` and still iterate). Hoist is mechanical (4 lines, move two `if` blocks to a pre-loop site in `Terrain::geometry()` at `terrain.cpp:1703`, remove the Tracy zone). Findings: [`explorations/2026-04-30-admission-early-guards-recon.md`](explorations/2026-04-30-admission-early-guards-recon.md). | N/A — bundle into next Tracy/code-hygiene commit. |

### Brainstorm pending (no spec yet)

_(none — SOLID-only PR1 has all three docs; detail/overlay consolidation brainstorm is a future-slice precondition.)_

### Blocked / parked

| What | Why | When to revisit |
|---|---|---|
| **GPU static props** (mechs/vehicles/buildings) | Cull-bypass infrastructure cascades into pool exhaustion + stale matrices. CPU mode (RAlt+0 OFF) is the supported path. | After M2d/quadSetupTextures land — separate system, similar lessons. See `cull_gates_are_load_bearing.md`. |
| **Flat grid array recipe cache** (M2 spec called for) | `unordered_map::find` proved cheap enough (~16ns per call) to not justify the refactor. | Only if a future profile shows hash lookup as dominant. |

---

## Architecture map

```
Terrain::render(GameCamera)
├── drawPass:   loop ×~40K → TerrainQuad::draw()    [our M2 family lives here]
│   ├── M2b loop-hoist (skip pure-water before call)
│   └── TerrainQuad::draw():
│       ├── pure-water early-exit (in-function fallback)
│       ├── M2 fast path (terrainHandle != 0 || has detail)
│       │   ├── thin record emit → SSBO → GPU draws via gos_terrain_thin.vert
│       │   └── inline detail emit (M2c) — addVertices(MC2_DRAWALPHA)
│       └── legacy path (overlay quads, edge cases)  [M2d target]
│           ├── full gVertex[6] build
│           ├── overlay emit (gos_PushTerrainOverlay)
│           ├── detail emit (legacy)
│           └── appendThinRecord (still feeds same SSBO)
├── minePass:   loop ×~40K → TerrainQuad::drawMine()  [unzoned, low cost]
└── debugOverlays: only when grid/cells/LOS toggles on

Terrain::geometry()
└── quadSetupTextures: loop ×~40K → setupTextures()  [next major target]

Terrain::renderWater()
└── loop ×~40K → drawWater()  [water-to-GPU target]
```

### Key data structures

- **`TerrainPatchStream`** (gos_terrain_patch_stream.{h,cpp}): the CPU/GPU bridge.
  - **Recipe SSBO** (single-buffered, persistent): per-quad world data, hash-cached by `(wx0, wy0)` key. Built once, reused across frames.
  - **Thin record SSBO** (triple-buffered): per-frame `(recipeIdx, terrainHandle, flags, lightRGBs[4])` — 32B each.
  - **Fat record SSBO** (M1): older path, ~192B records. Replaced by thin records when `MC2_PATCHSTREAM_THIN_RECORDS=1`.
- **`TerrainQuad`** (mclib/quad.h): per-quad CPU object iterated in the render loop. Owns `terrainHandle` (base), `overlayHandle` (splat overlay), `terrainDetailHandle` (water-interest), and `vertices[4]` pointers.

### Critical concepts to NOT confuse

| If you hear... | It means... | NOT to be confused with |
|---|---|---|
| "detail" / "detail overlay" (user-speak) | The GPU shader's `matNormal0..3` high-frequency surface detail (universal, fragment shader) | MC2's `terrainDetailHandle` (per-quad data field, water-interest blend texture) |
| "overlay" (in MC2 code) | `overlayHandle != 0xffffffff` — a SECOND terrain texture for splat-blending (cement/transition tiles, rendered via `gos_PushTerrainOverlay`) | Decals, footprints, craters (those are separate systems) |
| "fast path" / "M2 fast path" | The `if (fastPathEligible)` branch in `TerrainQuad::draw()` that emits a thin record + optional detail without building `gVertex[6]` | The "patch stream" itself — patch stream is the SSBO infrastructure, fast path is the CPU code path that feeds it |
| "thin records" vs "fat records" | Thin = M1d/M2 (32B, recipe-indirected), Fat = M1 (192B inline). They go to different SSBOs. | Both are types of "patch stream records" |

### Env vars to activate

```
MC2_PATCHSTREAM_THIN_RECORDS=1        # populate thin record SSBO
MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1   # use thin VS to draw (instead of legacy material)
MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1 # take the M2 fast path in quad.cpp
MC2_THIN_DEBUG=1                      # silent-by-default diagnostic counter (5 frames after warmup)
```

All default-off. Flip them on together to activate the full M2 pipeline.

---

## Required reading by topic

When a fresh session needs deeper context for a specific area:

| Topic | Read |
|---|---|
| **What just shipped (M2b/c)** | `memory/m2_thin_record_cpu_reduction_results.md` |
| **Why GL_FALSE for terrainMVP but GL_TRUE for projection_** | `memory/terrain_mvp_gl_false.md`, `memory/terrain_tes_projection.md`, `memory/clip_w_sign_trap.md` |
| **TES projection chain** | `memory/terrain_tes_projection.md`, `memory/static_prop_projection.md` |
| **Patch stream architecture** | `memory/patchstream_m0b.md`, `memory/patchstream_shape_c.md`, plans `2026-04-28-patchstream-m1*` |
| **Cull infrastructure (don't bypass)** | `memory/cull_gates_are_load_bearing.md`, `memory/tgl_pool_exhaustion_is_silent.md` |
| **Texture handle lifecycle** | `memory/mc2_texture_handle_is_live.md`, `memory/texture_handle_cap.md` |
| **Water rendering** | `memory/water_rendering_architecture.md` |
| **Why ARGB swizzle and SSBO bit decode** | `memory/mc2_argb_packing.md` |
| **Tracy profiling setup** | `memory/tracy_profiler.md`, `CLAUDE.md` Profiling section |
| **Stock-install constraint** | `memory/stock_install_must_remain_playable.md` |

---

## How a fresh session uses this

1. **Read this doc** to understand the end-to-end strategy and current state.
2. **Skim the relevant memory files** for the topic at hand (table above).
3. **If continuing the next queued milestone:** open its spec from the Status
   Board "Queued" row, follow its handoff prompt (most specs have one at the bottom).
4. **If starting something new:** brainstorm with the user using the
   `superpowers:brainstorming` skill, write a spec, queue it on this board.
5. **After landing work:** update the Status Board, write a short memory file,
   index it in `MEMORY.md`, commit.

## Working principles (learned through M0–M2)

- **Measure before you fix.** Use `superpowers:systematic-debugging`. Tracy
  zones are the first instrument; `MC2_THIN_DEBUG`-style env-gated counters
  are the second.
- **Tracy zone overhead matters at sub-μs scale.** ~30-100ns per zone-pair
  plus cache pressure from the queue. Don't add per-quad zones for measuring
  per-quad work below ~200ns; switch to rdtsc accumulators or just remove
  zones to measure delta.
- **"Self time" attribution can lie** when child zones are followed by
  significant unzoned work in the same scope (legacy body cost leaked into
  preBranch self in M2c diagnosis).
- **Diminishing returns are real.** M2c moved 5,800 quads (big win), M2c-ext
  moved 89 (lost in noise). Stop when the next slice is small; spec it for
  later if interesting; move to a fresh population.
- **Hoist the per-quad check upstream when possible.** Loop-level skip
  (terrain.cpp) beats in-function early-exit (quad.cpp) by saving the
  function call + zone enter overhead for the skipped quads.
- **Parity validate every fast path.** `[PATCH_STREAM v1] event=thin_record_parity match=1`
  must hold. Visual canaries (cement/concrete tiles, water-interest borders)
  catch ARGB/UV drift the parity counter doesn't.
- **Two paths can coexist.** Legacy + fast path active simultaneously means
  every quad gets evaluated by both gates. Useful while migrating populations.
  Eventually retire legacy when fast path covers ≥99% of common cases.
- **Validate against stock only.** Mod-content parity is not a gate for this
  workstream — see Scope section above.
- **Grep at write-time, not after.** Every cited symbol (struct field,
  function signature, file:line, env flag) gets grep-verified at the moment
  it enters a brainstorm answer, recon claim, design assertion, or plan
  step — not in an end-of-document appendix pass. Verify-then-write costs
  minutes; verify-after-write costs days at execution time when fictional
  content surfaces. Indirect-terrain plan v1 stop-the-line (2026-04-30) is
  the case study. Worktree `CLAUDE.md` "Documentation Discipline" + skill
  `.claude/skills/adversarial-plan-review.md` formalize this.

## Adjacent systems (don't confuse with this work)

These are SEPARATE workstreams that share some infrastructure but have their
own milestones, design docs, and constraints:

- **GPU static props** (mechs/vehicles/buildings via `gameos_graphics.cpp`
  static-prop batcher, RAlt+0 killswitch). Currently CPU-mode-only — GPU mode
  is a static-prop-OFF toggle, not a working alternate. See
  `docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md`.
- **Shadow pipeline** (static terrain shadow + dynamic mech shadow + post-process
  shadow pass). Mostly working; `dynamic_shadow_status.md`, `shadow_quality_upgrade.md`.
- **Render contract registry / F3 MRT completeness** (frame state validation,
  not perf). `docs/superpowers/specs/2026-04-26-render-contract-*-design.md`.
- **PBR splatting / detail normals / triplanar** (fragment shader visual quality,
  not CPU offload). `memory/terrain_texture_tuning.md`.
- **AssetScale subsystem** (icon atlas + chrome scaling). Independent.
- **Mod content / ABL stubs** (Magic, Wolfman, Carver5O, Omnitech). Independent.

---

## Update log

> Append a one-liner when something material changes. Most-recent at top.

- **2026-04-30** — Indirect terrain SOLID PR1 bug discovered post-Stage-3: indirect path binds sampler unit 0 but no texture; quads sample whichever texture was last bound (typically overlay rock). Parity passed because parity verifies recipe DATA, not pipeline STATE. Fix decided: sampler2DArray atlas (option b), not per-bucket binding (option a) — aligns with North Star direction + unblocks Target 2 multi-bucket consolidation. Stage 4 default-on promotion BLOCKED until atlas slice ships. Default config unaffected (regression only manifests with `MC2_TERRAIN_INDIRECT=1`). Process learning: "data parity ≠ pipeline state correctness" — visual canary in Gate A degraded to FPS/destroys/recipe-equivalence and missed pixel-level regression. Worth adding to `gpu_direct_renderer_bringup_checklist.md`.
- **2026-04-30** — All three future-target recons closed. **Target 1 (water-projection-skip):** ready-for-brainstorm. Stub citations were approximate; verified gate sites at `quad.cpp:773-1100` + `quad.cpp:1087-1088`. `wAlpha` is dead-data; "second projection site" turned out to be `vertexProjectLoop` terrain accumulator, NOT a target. Scope closer to M2d-overlay. One residual risk: mouse-pick depth via `inverseProjectZ()` accumulators. **Target 2 (Shape D):** CLOSED — SUBSUMED. Indirect-terrain plan v2 Stages 0-3 shipped (`9bfcddc`/`bdb1628`/`094fa56`/`f221570`); the decoupling hypothesis was moot. The "527 µs recoverable" framing was a category error — the Tracy zones wrap CPU work the legacy DRAW machinery still consumes regardless of SSBO migration. **Target 3 (admission/early-guards):** CLOSED — bundle as code-hygiene commit, not a perf slice. The 0.68 ms is Tracy zone overhead at 39,601 zone-pairs; actual recoverable is ~40-160 µs.
- **2026-04-30** — Indirect terrain draw plan v2 SOLID-only PR1 SHIPPED across Stages 0-3. SOLID main-emit CPU iteration is now retired; detail/overlay/mine remain on legacy paths per A=(i) scope narrowing. The plan v1 stop-the-line at adversarial review was the inflection point that produced this clean shipping arc — verify-then-write discipline encoded in worktree CLAUDE.md ensured plan v2 shipped without re-discovering stale prose.
- **2026-04-30** — Three future targets queued for recon. Initial dispatch with `code-explorer` agent type (read-only) caused two of three to stall at watchdog while attempting long-inline-output workaround for missing Write tool; re-spawn with `general-purpose` agent type completed both in ~20 min each. Lesson: verify subagent_type has the tools needed for the task before dispatch.
- **2026-04-30** — Indirect terrain draw plan v1 stop-the-line at adversarial review. 3 CRITICAL findings (fictional `TerrainQuadRecipe` fields, wrong `invalidateTerrainFaceCache` signature, missing `quadSetupTextures` gate-off) + multiple major issues (multi-bucket trap, AMD attrib 0, no per-mission teardown, etc.). Scope narrowed to SOLID-only PR1; detail/overlay/mine deferred to a follow-up consolidation slice with its own brainstorm. Three architectural decisions confirmed: A=(i) SOLID-only, B=(i)-narrowed-to-SOLID gate-off, C=(iv) preflight-armed bypass. Plan revision-pass brief sent back to planner. Process learning: brainstorm Q-by-Q format succeeded structurally but Q3 (SSBO topology) inherited stale memory because no one grep'd actual structs — adversarial review's code-grounded verification is the gap normal review misses. Memory: `brainstorm_code_grounding_lesson.md`.
- **2026-04-30** — vertexProjectLoop D1 hoist closed asymptotic. Mean Δ +0.04% (475→475 µs), σ −10%, P99/P99.9 in slightly. Parity scaffolding shipped (96M verts byte-checked / 0 mismatches) — useful as durable infra even though perf gate B failed. **Compiler-ceiling outcome:** the optimizer had already register-allocated the trivial-hoist targets; real cost is per-vertex math (mat-vec, reciprocal-divide, length, projectZ matmul) — only SIMD or GPU compute moves it further, neither queued. Pivoting to indirect terrain draw, which now needs a brainstorm phase (D3-prerequisite framing no longer fits; scope questions open). Memory: `vertexproject_loop_asymptotic.md`.
- **2026-04-30** — renderWater architectural slice **CLOSED.** Stage 3 (`MC2_RENDER_WATER_PARITY_CHECK`) ships silent-on-pass across tier1 stock with ~3.2M quads byte-checked / zero mismatches. Tracy delta gate B verified: legacy 449–894 µs → fastpath 88–132 µs across mc2_01/03/10/17/24 (78–85% reduction, exceeds ≥50% target). tier1 5/5 PASS triple (unset / FASTPATH=1 / FASTPATH=1+PARITY_CHECK=1). Three real bugs found and fixed during bring-up (recipe coverage broadened to all map quads, blank-vertex skip applied to both upload + parity, fogRGB material low byte patched at upload time). Reusable template captured in `memory/water_ssbo_pattern.md` for the indirect-terrain endpoint. `[INSTR v1]` banner extended with `water_fp` + `water_parity` fields.
- **2026-04-30** — renderWater Stage 2 ships visually + tier1 5/5 PASS both env states. Architecture: map-stable WaterRecipe (from MapData::blocks at primeMissionTerrainCache) + per-frame WaterThinRecord SSBO + GPU-direct draw via new `Terrain::renderWaterFastPath()` hooked AFTER `mcTextureManager->renderLists()`. mc2_17 visual diff matches legacy near-identically. Shoreline alpha-fade fixed via depth-state setup in bridge (gpu_direct_depth_state_inheritance.md). 9 GPU-direct gotchas codified in `memory/gpu_direct_renderer_bringup_checklist.md` + 3 NEW memory files (render_order_post_renderlists_hook, sampler_state_inheritance_in_fast_paths, quadlist_is_camera_windowed) for the previously-undocumented traps.
- **2026-04-30** — Scope clarified: validation is stock missions only. Mod content (Carver5O, Magic, MCO, Wolfman, MC2X) is out of scope for offload slices. Earlier handoff prompts mentioning "tier1 + Carver5O + Magic" are obsolete; future prompts use tier1 only. See `memory/feedback_offload_scope_stock_only.md`.
- **2026-04-29** — renderWater architectural slice promoted to In progress. Three design decisions confirmed (parallel SSBO, skip CPU admission, byte-compare-on-inputs / visual-canary-on-outputs). Spec at `specs/2026-04-29-renderwater-fastpath-design.md`.
- **2026-04-29** — quadSetupTextures arc concluded asymptotic. Cumulative slices 1+2a+2b: 3.47→3.01 ms (-13% mean, σ 476→291 µs / -39%). Recon showed water-vertex projection at 11% / 42 ns per call — below pivot threshold. Next: renderWater architectural slice.
- **2026-04-29** — Shape-C flipped default-on (`aee39cc`); slice 2a `addTriangleBulk` and slice 2b mine cache (`53f09ca`) both shipped.
- **2026-04-29** — M2d-overlay shipped (`258e584`). drawPass 5-6ms → 1.46ms, fast=14000 legacy=0, tier1 5/5 PASS.
- **2026-04-29** — M2b/c/c-ext shipped + cleanup committed (`8da7007`). drawPass 25→6ms, FPS 50-60→80-90 on mc2_01.
- **2026-04-29** — M1g shipped. GL_PATCHES+TCS+TES → thin VS GL_TRIANGLES. ~21ms GPU → ~3-5ms GPU.
- **2026-04-28** — M1d/e/f shipped. Thin records + skip expanded staging.
- **2026-04-27** — M0b shipped. Persistent VBO seam.
