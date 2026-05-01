# Indirect Terrain Draw — Architectural Slice (CPU→GPU Offload Endpoint)

**Status:** design confirmed via brainstorm 2026-04-30 (`docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md`). Recon and code pending.
**Owner:** session 2026-04-30 (post-vertexProjectLoop pivot).
**Predecessor:** `vertexProjectLoop` D1 closed asymptotic, `quadSetupTextures` arc closed asymptotic. This is slice 23 of 24 in the CPU→GPU offload arc. Final slice (c — `drawPass` outer-loop elimination via GPU compute) deferred per brainstorm Q1.

## Goal

Eliminate the per-frame CPU iteration in `Terrain::geometry quadSetupTextures` (~3.01 ms after Shape C / slice 2a / 2b shipped). Convert the M2 recipe SSBO from a sparse `unordered_map`-keyed lazy cache into a dense, map-stable, mission-stable structure indexed by `vertexNum`. Per-frame CPU work for terrain admission collapses to: (i) `vertexProjectLoop` (already exists), (ii) walk live `quadList`, look up recipe by `vertexNum`, pack thin record. Single multi-draw indirect for terrain main-emit. Stock install must remain playable; new path behind `MC2_TERRAIN_INDIRECT` env gate, default off until 4-gate ladder green.

## Background — what's already shipped

Per orchestrator status board (`docs/superpowers/cpu-to-gpu-offload-orchestrator.md`):

- M0–M2d: terrain main-emit fast path. `Terrain::render drawPass` 25→1.46 ms. 0 legacy quads / 14000 fast-path quads. Contract retired here for solid + detail + overlay.
- Shape C (default-on, `aee39cc`): texture-handle resolution moves to a hash-cached recipe. `quadSetupTextures` 3.47→3.17 ms inclusive.
- Slice 2a (`addTriangleBulk`): 3.17→3.06 ms.
- Slice 2b (mine-state cache): 3.06→3.01 ms; σ tightened −24%.
- renderWater Stage 2+3 (closed 2026-04-30): water emit retired via static recipe + thin record + post-`renderLists()` GPU-direct hook. Tracy delta 78–85% on `Terrain::renderWater` zone.
- vertexProjectLoop D1 (closed 2026-04-30 asymptotic): per-vertex projection pre-pass at compiler ceiling. Parity infra and env-gate scaffolding shipped.

This slice picks up where `quadSetupTextures` arc closed: the asymptotic 3 ms remainder is dominated by per-frame iteration over ~14000 quads with hash-cached recipe lookups. Architecturally, the only path past asymptotic is to build a dense map-stable recipe at terrain load, mirror the water pattern at terrain scale, and emit a single multi-draw indirect for all main-emit terrain.

## Architecture

```
Static (built once at primeMissionTerrainCache):
└── TerrainRecipeSSBO[mapSide² entries]
    indexed by vertexNum = mapX + mapY * realVerticesMapSide
    (set at mapdata.cpp:1104 as `topLeftX + (topLeftY * Terrain::realVerticesMapSide)`;
     -1 sentinel for off-map blankVertex)
    per-record (~64 B target — confirm during recon):
      vec4  worldPosCornerN[4]    // (vx, vy, elev, packed)
      uint  terrainHandle         // mc2_texture_handle_is_live slot index, NOT gosHandle
      uint  overlayHandle         // sentinel 0xffffffff if absent
      uint  detailHandle          // water-interest blend texture
      uint  uvData                // packed: uvMode, hasOverlay, hasDetail, mineState bits
      uint  terrainTypePacked     // m0 | (m1<<8) | (m2<<16) | (m3<<24); per slice 2 layout
      uint  flags                 // packed classifier bits
    Build: walks MapData::blocks at primeMissionTerrainCache time. Mission-stable;
    invalidated only via single-point chokepoints (see Mutation Events).

Per-frame thin record (subsumes M2 thin-record SSBO):
└── TerrainThinRecordSSBO[N_emitting]   N ≤ ~14000 visible quads on Wolfman zoom
    triple-buffered ring; 32 B per record (existing M2 layout):
      uint  recipeIdx              // back-reference into recipe SSBO
      uint  terrainHandle          // resolved per mc2_texture_handle_is_live each frame
      uint  flags                  // bit0 uvMode, bit1 pzTri1Valid, bit2 pzTri2Valid
      uint  _pad0
      uint  lightRGB0..3           // per-vertex lighting from vertexProjectLoop output
    Population: walk live quadList, dereference each quad's top-left vertexNum,
    look up recipe via vertexNum→recipeIdx hash, pack thin record. Skip set per
    water_ssbo_pattern.md (pointer guards + blankVertex skip + per-pass admission).

Per-frame indirect command buffer:
└── TerrainIndirectCmdSSBO[1..4 commands]
    glMultiDrawElementsIndirect command structs
    one per draw bucket: (i) base solid, (ii) detail, (iii) overlay.
    drawMine and debug overlays remain on legacy contract (Q5).

Per-frame uniform block: shared with M2 thin-record path
  (terrainMVP, projection_, fog state, light state, animation phases).

Draw: ONE glMultiDrawElementsIndirect post-renderLists()
  - Driven by indirect command buffer (CPU-built per Q2).
  - VS = M2 thin-record VS (gos_terrain_thin.vert), reused as-is in the
    happy path. May need a `+ 0.001` screen-z fudge added (or a new VS
    variant) pending depth-state recon — see Constraint #4 gotcha #9.
  - FS = existing terrain FS (pixel-stable visuals), unchanged.
```

### Data structure decisions (from brainstorm Q3)

- **Recipe SSBO** — extends the existing M1d/M2 hash-cached recipe into a dense
  array indexed by `vertexNum`. Recipe contents stay the same (terrainHandle,
  overlayHandle, detailHandle, uvData, mine state cache from slice 2b). Just
  packed densely. Memory cost: `mapSide² × ~64 B`. At 256² stock max, ~4 MB.
  At 384² (largest stock dimension known), ~9.4 MB. Well under sensible budget.
- **Thin record SSBO** — reuses the M2 32 B layout. No format change.
- **Indirect command buffer SSBO** — small (1-4 `DrawElementsIndirectCommand`
  structs). CPU-built per frame. Reused frame-to-frame (no reallocation).

### Why extend rather than merge with renderWater SSBOs

Per brainstorm Q3: water just shipped (Stage 3 zero mismatches). Touching
that schema means re-validating water parity for reasons unrelated to terrain.
Risk for no perf gain — water and terrain run different shaders, different
per-frame logic. Keep parallel.

### ARGB packing convention (gotcha — `memory/mc2_argb_packing.md`)

MC2 packs ARGB as BGRA-in-memory. The thin record's `lightRGB0..3` /
`fogRGB0..3` DWORDs follow this convention. Vertex-attribute uploads use a
`.bgra` swizzle; SSBO `uint` reads decode bits manually
(`(c >> 16) & 0xFFu` for R, etc.). The M2 thin VS already follows this; the
new bridge/VS must match exactly. Any field-level parity check
(`MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`) compares the packed DWORD
post-swizzle so legacy-vs-fast bit equality is preserved.

### Predecessor patterns — what this slice mirrors

This slice deliberately reuses architectural shapes that have shipped
across the M-family arc. New session readers should skim these to ground
the design in lived precedent rather than re-deriving:

- **`memory/patchstream_m0b.md`** — Persistent-VBO seam (M0b). The pattern
  of building a static GPU-resident structure once at terrain load and
  binding it across frames originated here.
- **`memory/water_rendering_architecture.md`** — Water as a separate
  overlay (gos_tex_vertex), NOT part of terrain splatting. Same separation
  applies to indirect-terrain: terrain main-emit uses its own thin VS;
  water keeps its own.
- **`memory/m2_thin_record_cpu_reduction_results.md`** — The M1d/M2 thin-
  record path established the (recipe, thin record, single draw) shape
  for terrain. This slice converts the recipe from sparse to dense.
- **`memory/patchstream_shape_c.md`** — Texture-handle cache architecture.
  Cache-read default-on; Shape C's `invalidateTerrainFaceCache()` chokepoint
  becomes the recipe-mutation chokepoint pattern.
- **`memory/water_ssbo_pattern.md`** — The reusable template (renderWater
  Stage 1+2+3). The recipe-inclusion criterion ("MIGHT emit on some frame,"
  not "WILL emit"), the canonical skip set, and the parity-check infra
  pattern all transfer.
- **`memory/renderwater_fastpath_stage2.md`** — Stage 3 surfaced 3 bugs
  silently caught by parity. The same class of bug is expected in
  indirect-terrain (recipe coverage gap, blank-vertex skip drift, derived
  byte patches). Per-thin-record byte-compare is the mitigation.
- **`memory/vertexproject_loop_asymptotic.md`** — Parity-check infra
  validated on a third pattern shape (pure-CPU, no SSBO). The pattern is
  robust; reuse > reinvent.

### Render-order hook

Per brainstorm Q1 open follow-up: the render-order trap
(`memory/render_order_post_renderlists_hook.md`,
`memory/gpu_direct_renderer_bringup_checklist.md` #6) gets more nuanced when
terrain itself is the new fast path.

Confirmed call site shape (from current code):
```
code/gamecam.cpp:243   mcTextureManager->renderLists();    // legacy buckets flush here
code/gamecam.cpp:254   land->renderWaterFastPath();        // post-renderLists, water alpha-blends on top
```

RenderWater hooked AFTER renderLists because renderLists drains terrain into
the depth buffer first, and water then alpha-blends on top. Indirect terrain
draw IS what writes that depth buffer — so it must run AT or replace the
point where renderLists currently drained the legacy terrain main-emit buckets.

**Decision (resolved 2026-04-30 recon — see
`docs/superpowers/plans/progress/2026-04-30-indirect-terrain-recon-handoff.md`
Item 1):** the new indirect-terrain draw hooks AT the existing
`Render.TerrainSolid` Tracy zone inside `renderLists()` (txmmgr.cpp:1297-1406),
specifically replacing or paralleling the `TerrainPatchStream::flush()` call
at txmmgr.cpp:1330. The legacy DRAWSOLID skip at txmmgr.cpp:1340-1343
(`if (modernHandled && MC2_ISTERRAIN) continue;`) handles the legacy-bucket
fall-through unchanged.

This is **distinct from renderWater's post-renderLists pattern.** Water
hooks AFTER `renderLists()` because water reads depth and alpha-blends on
top of already-written terrain. Terrain WRITES depth, and everything later
in `renderLists()` (Render.GpuStaticProps at :1414, Render.TerrainOverlays
at :1430, Render.Decals at :1437, Render.Overlays at :1442, Render.NoUnderlayer
at :1526, shadow legacy buckets at :1610) depth-tests against terrain depth.
Moving terrain main-emit out of `renderLists()` would orphan all those
later passes against an empty depth buffer.

## Recon items (must complete before code lands)

These are the open follow-ups from the brainstorm doc Q1-Q7 sections. Each
needs a specific resolution before implementation begins. Track in a
`progress/2026-MM-DD-indirect-terrain-recon-handoff.md` artifact.

**Recon completed 2026-04-30** — handoff at
`docs/superpowers/plans/progress/2026-04-30-indirect-terrain-recon-handoff.md`.
All 9 items resolved (Item 4 partial — FST archive blocks static analysis;
runtime trace from Stage 1 closes that gap). Resolutions in-line below where
load-bearing; full findings + spec amendments + token budget log in handoff.
Implementation-plan session inherits this spec + handoff together.

1. **Render-order placement.** Where exactly does the new indirect draw hook
   in `gamecam.cpp`? Specifically: does it replace the existing
   `mcTextureManager->renderLists()` call at `code/gamecam.cpp:243`, sit
   between two phases of it, or run before/after as a separate pass?
   Inventory: read `mclib/txmmgr.cpp renderLists()` to enumerate what each
   bucket flush does (terrain main-emit vs decals vs objects vs other),
   then confirm which buckets indirect-terrain replaces. The
   `renderWaterFastPath()` hook at `code/gamecam.cpp:254` already
   demonstrates the post-renderLists pattern. Reference:
   `memory/render_order_post_renderlists_hook.md`.

2. **Thin-record format coverage.** Confirm the existing 32 B M2 thin record
   carries ALL per-frame state currently computed in `setupTextures`
   (`mclib/quad.cpp:429-` body, with the first Tracy zone
   `quadSetupTextures admission / early guards` at `:432`). Audit candidates:
   overlay UV state if overlay handle is mutable per-frame; alpha modulation
   factors; per-tri pzValid bits. The outer Tracy zone
   `Terrain::geometry quadSetupTextures` at `mclib/terrain.cpp:1681` wraps
   the full per-frame loop; everything inside it is candidate work to
   retire. If gaps exist (per-frame state not in the thin record), either
   thin record grows or recipe must invalidate on those changes (Q6).

3. **Bridge destruction path.** Grep callsites in stock-content code paths
   for: `setOverlay`, `setTerrain`, bridge destruction (search terms:
   `bridgeDestroy`, `Bridge::destroy`, `setOverlay`, "blow up bridge",
   `pBridge`). Reach via `Terrain::setTerrain` at `mclib/terrain.cpp:875`
   which routes to `MapData::setTerrain` at `mclib/mapdata.cpp:1293`. The
   existing `MapData::invalidateTerrainFaceCache()` at
   `mclib/mapdata.cpp:213` is called from `mclib/mapdata.cpp:149`, `:191`,
   `:1359` — those are the existing chokepoints. Classify each
   bridge-related callsite: chokepoint exists / needs new hook / rare
   enough for full SSBO rebuild.

4. **Stock smoke missions in-game terrain mutations.** mc2_01, mc2_03,
   mc2_10, mc2_17, mc2_24. Document any in-game mutation that would
   invalidate a recipe entry. If zero, recipe is post-load immutable on
   the smoke set; document this assumption explicitly so future mod work
   knows the constraint. Per `memory/feedback_offload_scope_stock_only.md`,
   mod content is not a gate, so even if mod missions DO mutate, that's
   not a blocker — it just means default-on flip (Q5) carries a residual
   "mod that mutates terrain mid-mission may regress unless the mod's PR
   adds invalidation hooks."

   **Recon resolution (2026-04-30, partial):** mission scripts are inside
   the FST archive (`mc2.fst`); on-disk static analysis is not feasible
   without a FST extractor / mission reader. Defer enumeration to Stage 1's
   `[TERRAIN_INDIRECT v1] event=invalidate` lifecycle prints captured
   during a tier1 smoke run. The defensive-hooks-plus-parity-check posture
   in this spec already handles unknown mutations: the single chokepoint
   (`invalidateTerrainFaceCache`, mapdata.cpp:1359) covers all known event
   classes (Recon Item #3 finding); `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`
   validates the assumption holds.

5. **`drawMine` on stock smoke missions.** mc2_24 has mines (per slice 2b
   memory). The legacy `TerrainQuad::drawMine` lives at `mclib/quad.cpp:4136`,
   called from the pass loop at `mclib/terrain.cpp:963`. Verify it still
   draws clean over the new indirect terrain when `MC2_TERRAIN_INDIRECT=1`.
   Visual canary at a mine site is the smallest test. Per orchestrator
   architecture map: `minePass: loop ×~40K → TerrainQuad::drawMine() [unzoned, low cost]`.

6. **Killswitch semantics.** `MC2_TERRAIN_INDIRECT=0` is a full legacy
   fallback — no recipe build, no thin record, no indirect draw. Confirm
   the gate flips cleanly without leaking partial state. Spec for the
   env-gate code path: at `Terrain::primeMissionTerrainCache`
   (`mclib/terrain.cpp:575`, where `WaterStream::Build()` already lives at
   `:599`), branch on env; never touch the new code if `=0`.

7. **Tracy aggregator zone.** Decide whether to add a `Terrain::TotalCPU`
   per-frame aggregator (vertexProjectLoop + ThinRecordPack + drawPass
   minus the now-skipped quadSetupTextures iteration) or stick to per-zone
   deltas. Recommended: add the aggregator. The work shape may shift
   between zones in a way single-zone delta would mis-attribute. Reference:
   `memory/tracy_profiler.md`.

8. **Two-PR promotion sequence.** PR 1 ships slice opt-in; tier1 5/5
   PASS triple gates default-on flip; PR 2 flips default. Encode in
   commit-message conventions and orchestrator status-board update protocol.

## Validation gates (4-gate ladder, mirrors renderWater)

A. **Visual canary** at fixed seed/camera, side-by-side legacy/fast.
   Beyond standard fixed-camera diff, exercise:
   - Shoreline overlay (mc2_17 water band) — same canary water Stage 2 used.
   - Mine sites (mc2_24) — drawMine remains legacy per Q5; verify clean overlay on new terrain.
   - Wolfman zoom (high altitude, large frustum) — catches map-edge LOD coverage gaps.
   - Bridge / structure boundary (whichever stock mission has them) — catches setOverlay regressions.

A'. (optional) Screenshot diff (automated, fixed seed, animation pinned).
   Brittle in past sessions; useful as debugging aid (`scripts/water_visual_diff.py`
   pattern) but NOT a promotion gate.

B. **Tracy delta** on `Terrain::geometry quadSetupTextures` zone.
   Target ≥50% reduction. Pre-(b) baseline: 3.01 ms mean, mc2_01 max-zoom-out.
   Post-(b) target: ≤1.50 ms (≥50%). Aspirational: ≤0.50 ms (residual being
   just the per-frame thin-record build, mirroring renderWater's 88-132 µs
   fastpath cost). Stretch: also report `Terrain::TotalCPU` aggregator delta.

C. **`MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`** zero mismatches across stock-
   content tier1 only. Silent on success per
   `[PATCH_STREAM v1] event=thin_record_parity` and `[WATER_PARITY v1]`
   conventions. Field-level mismatch printer:
   `[TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=N quad=Q layer=<base|detail|overlay> tri=T vert=V field=<name> legacy=0xHEX fast=0xHEX`.
   Throttle to 16/frame. 600-frame summary always-on counter:
   `event=summary frames=N quads_checked=Q total_mismatches=K`.
   **Compare on inputs:** recipe contents + thin-record contents + derived
   per-corner per-tri u/v/argb/frgb-high. Don't compare post-projection
   x/y/z/rhw (drift sub-1-ULP, false positives).

D. **tier1 5/5 PASS triple:** unset / `MC2_TERRAIN_INDIRECT=1` /
   `MC2_TERRAIN_INDIRECT=1+MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`.
   +0 destroys delta on every mission, every state. Both env vars added
   to `scripts/run_smoke.py` env-allowlist (mirror Shape C / renderWater
   pattern).

## Constraints (load-bearing — must respect)

1. **Stock install must remain playable.** New path behind
   `MC2_TERRAIN_INDIRECT` env gate, default off. Don't promote until
   4-gate ladder is green across tier1 stock. Per CLAUDE.md critical rule.

2. **Validation is stock missions only.** mc2_01, mc2_03, mc2_10, mc2_17,
   mc2_24. Mod content (Carver5O, Magic, MCO, Wolfman, MC2X) is not a
   gate; if a mod regresses, that's the mod's problem.

3. **Cull cascade is moot for terrain.** Per `cull_gates_are_load_bearing.md`,
   that doc applies to *object* cull (mechs/buildings/vehicles), where
   bypassing cascades into pool exhaustion / stale matrices / destroyed
   objects. Terrain quads have no such lifecycle. CPU pre-cull via per-
   vertex `pz` is the load-bearing gate, already shipped infrastructure.

4. **The 9 GPU-direct gotchas all apply.** Per
   `memory/gpu_direct_renderer_bringup_checklist.md`. Each gotcha must be
   addressed in the bridge/VS code; the spec must show how. Per-gotcha
   memory references:

   - **#1 `uniform uint` crashes shader builder** — `memory/uniform_uint_crash.md`.
     Fix: `uniform int` + cast to `uint` inside shader for bitwise ops.
     Same convention M2 thin VS already uses.
   - **#2 Two-tier texture handle indirection** —
     `memory/mc2_texture_handle_is_live.md`. Recipe stores
     mcTextureManager *textureIndex* / *slot index*, NOT engine
     *gosTextureHandle*. Bridge resolves via `tex_resolve(idx)` per frame
     (mirrors quad.cpp:2084 M2d-overlay pattern). Sentinel `0xffffffff`
     skips the call.
   - **#3 `terrainMVP` GL_FALSE; `projection_` / `mvp` GL_TRUE** —
     `memory/terrain_mvp_gl_false.md`. M2 thin VS already follows this
     convention; reuse the same `setMat4Direct` / `setMat4Std` helpers.
   - **#4 VAO 0 silently drops draws on AMD** — bridge calls
     `gos_RendererRebindVAO()` at start; saves/restores prior VAO binding.
   - **#5 Sampler inheritance** —
     `memory/sampler_state_inheritance_in_fast_paths.md`. Terrain main-emit
     atlas textures use the existing patch_stream sampler (CLAMP_TO_EDGE /
     LINEAR) — mirror legacy state. Don't blindly copy water's REPEAT
     sampler (water is world-tiled; terrain is atlas-tiled).
   - **#6 Render order — must run AFTER (or AT) renderLists()** —
     `memory/render_order_post_renderlists_hook.md`. See Recon Item #1.
     This is the single most expensive lesson because the legacy code
     makes it look like terrain main-emit happens at one place when it
     actually drains later.
   - **#7 CPU pre-cull is THE load-bearing frustum gate** —
     `memory/clip_w_sign_trap.md`, `memory/terrain_tes_projection.md`. The
     MC2 worldToClip matrix produces well-formed finite clip values for
     both visible AND behind-camera vertices. NO GPU-side test can
     distinguish them. Per-tri `pzTri1Valid` / `pzTri2Valid` bits in the
     thin record drive the VS's degenerate `gl_Position` emit. **Do not
     add a `clip.w < 0` guard in the VS** — that rejects legitimately-
     visible vertices the abs(clip.w) chain handles correctly.
   - **#8 Map-stable indexing** — `memory/quadlist_is_camera_windowed.md`.
     `Terrain::quadList[i]` is a camera-window slot; reshuffles every
     frame via `MapData::makeLists` (`mclib/mapdata.cpp:1072` per
     orchestrator). Recipe MUST be indexed by `vertexNum`
     (`mapX + mapY * realVerticesMapSide`, written at
     `mclib/mapdata.cpp:1104`). Off-map vertices have `vertexNum = -1`;
     skip uniformly.
   - **#9 Depth-state inheritance + `TERRAIN_DEPTH_FUDGE`** —
     `memory/gpu_direct_depth_state_inheritance.md`. Terrain WRITES depth
     (unlike water which reads but doesn't write). Bridge sets explicit
     `glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LEQUAL)`,
     `glDepthMask(GL_TRUE)` — and saves/restores around the draw.

     **Fudge recon resolution (2026-04-30):** real parity gap exists.
     Legacy CPU emit applies `+ TERRAIN_DEPTH_FUDGE` (`0.001f`, defined at
     `mclib/quad.cpp:1618`) to every terrain triangle z-coord at all 16
     emit sites in `mclib/quad.cpp` (lines 1762, 1869, 2004/2014/2024,
     2171, 2363/2373/2383, 2527, 2775/2785/2795, 2934, 3063/3073/3083,
     3222 — `pz + FUDGE` for terrain DRAWSOLID, `wz + FUDGE` for water).
     The current M2 thin VS at `shaders/gos_terrain_thin.vert:137` reads
     `screen.z = clip.z * rhw;` — **no fudge.** Water fast VS DID add it
     in commit `bc8c4f1` at `shaders/gos_terrain_water_fast.vert:332`
     ("2026-04-30 shoreline polish: bias water slightly farther in screen
     z to match legacy's TERRAIN_DEPTH_FUDGE=0.001 (quad.cpp:2775)").

     **Why hasn't the thin path's missing fudge surfaced?** Hypothesis:
     `Render.3DObjects` runs at txmmgr.cpp:1102 BEFORE `Render.TerrainSolid`
     at :1297. 3D mech/vehicle objects write to depth before terrain — they
     don't depth-test against terrain that hasn't been drawn yet. The
     legacy use case for the fudge (objects-vs-terrain z-tie at zoom levels
     where they coincide) doesn't apply in the M2 thin path's current draw
     order. Decals, GpuStaticProps, and DRAWALPHA water-on-terrain DO run
     after terrain in renderLists; for those, missing fudge could in theory
     cause z-fighting at exactly-coincident depth.

     **Observed manifestation (2026-04-30):** a power generator
     animation/decal that was previously visible (though already in the
     wrong place) is now hidden beneath the terrain. Consistent with
     missing fudge — the decal sits at exactly-coincident depth and
     terrain wins the LE tie without the bias. Captured in
     `memory/power_generator_decal_below_terrain.md`. Pre-existing visual
     correctness was already wrong (decal in wrong place), so this is not
     a smoke-detectable regression — it's a known quirk now connected to
     a load-bearing rendering decision.

     **Decision:** **DEFER fudge addition to the thin VS.** Adding
     `+ 0.001` would invalidate the existing M2 path's tier1 5/5 PASS
     baseline and surface new test surface for overlay/decal interaction
     (Render.TerrainOverlays, Render.Decals, GpuStaticProps placements
     all become subject to potential subpixel shifts). The fudge addition
     is a separate slice with its own gate — not folded into
     indirect-terrain's Stage 1+2. Indirect-terrain reuses the thin VS
     as-is, inheriting the same depth behavior the M2 path has had since
     M2d shipped. The parity-check (`MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`)
     compares on **inputs** (recipe + thin record bytes) per spec section
     "Validation gates" → C; post-projection z drift below 1 ULP is
     already excluded from the comparison, so the fudge gap does not
     surface as a parity mismatch. Document the deferred fudge addition
     as a follow-up slice candidate, with the power generator observation
     as one canary if/when it ships.

   **Gotcha NOT applicable to terrain** (read for the distinction so the
   rationale is durable in future sessions):
   - **TGL pool exhaustion** — `memory/tgl_pool_exhaustion_is_silent.md`.
     Applies to `TG_VertexPool` for object props (mechs/buildings/vehicles
     via `TG_Shape::Render`). Terrain quads do not allocate from the TGL
     pool; they go through the patch_stream / addVertices path. Don't
     confuse the two systems.
   - **Object cull cascade** — `memory/cull_gates_are_load_bearing.md`.
     Applies to `inView` / `canBeSeen` / `objBlockInfo.active` for objects.
     Terrain has no equivalent lifecycle; CPU pre-cull via `pz` is the
     load-bearing frustum gate, and it doesn't cascade into pool
     exhaustion or destroyed objects.
   - **Shadow caster eligibility** —
     `memory/shadow_caster_eligibility_gate.md`. Applies to
     `TG_Shape::Render` shadow gate. Terrain's shadow contribution is
     handled separately via the static terrain shadow map; not in scope
     for this slice.

5. **Pixel-stable visuals.** Tessellated terrain displacement, PBR splatting,
   shadow sampling, water rendering all stay where they are. This slice is
   the CPU-side admission/recipe path, not the visual pipeline. The thin VS
   is reused; the only potential VS change is the depth-fudge addition per
   gotcha #9 if recon shows a parity gap. The fragment shader does not change.

6. **Recipe must NEVER drift silently from CPU state.** Either every
   mutation event has a single-point invalidation hook, or rare events
   trigger wholesale rebuild, or `PARITY_CHECK=1` catches drift before
   promotion. See Mutation Events.

## Mutation events (Q6 — recon-pending)

| Event | Chokepoint | Strategy |
|---|---|---|
| Mission load (build) | `primeMissionTerrainCache` | Wholesale build. Mirrors renderWater Stage 2. |
| `setTerrain(vertexNum, terrainType)` | Existing — Shape C invalidates whole array | Convert to granular `invalidateRecipeFor(vertexNum)`. Single hook. |
| `setMine(...)` / blown / unburied | Existing — slice 2b: all 19 callsites route through `GameMap::setMine` | Already per-recipe cached. Confirm chokepoint after dense migration. |
| `setOverlay(vertexNum, overlayHandle)` | Existing — `MapData::setOverlay` (mapdata.cpp:1259) calls `setTerrain(indexY, indexX, -1)` which calls `invalidateTerrainFaceCache()` at :1359 | Subsumed by setTerrain chokepoint. No new hook needed. |
| Bridge destruction | No `Bridge` class in source (recon 2026-04-30) — bridges are encoded as map overlays via `setOverlay`. | Subsumed by setOverlay chokepoint (which itself routes through setTerrain). No new hook needed. |
| Crater / damage to terrain type | Routes through `setTerrain` (mapdata.cpp:1255 `setOverlayTile` call to `setTerrain`, plus general `setTerrain` callsites) | Subsumed by setTerrain chokepoint. No new hook needed. |
| Editor edits | N/A — out of scope | Skip. |

If Recon Item #4 confirms zero in-game mutations on stock smoke missions,
the recipe is effectively post-load immutable in practice. Defensive hooks
plus `PARITY_CHECK=1` (off by default) is the cheapest gate. We pay
invalidation cost only when the event fires; we pay parity cost only when
the user sets the env var.

## Env gates (mirror Shape C / M2 / renderWater ladder)

- `MC2_TERRAIN_INDIRECT=1` — default off. When on: builds dense recipe SSBO at
  primeMissionTerrainCache; per-frame walks live quadList, packs thin record,
  builds indirect command buffer, emits single `glMultiDrawElementsIndirect`.
  When off (default): legacy code path entirely; no recipe build, no thin
  record changes, no indirect draw call. Full legacy fallback per Recon Item #6.
  At promotion, flip to default-on with `MC2_TERRAIN_INDIRECT=0` killswitch
  semantics (mirror Shape C).
- `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1` — default off. Captures legacy
  `quadSetupTextures` per-frame state (or arg-stream synthesis) and byte-
  compares against recipe + thin-record contents. Doubles per-frame CPU during
  parity runs. Acceptable as diagnostic mode. **Decoupled from `INDIRECT=1`:**
  parity-check can run with `INDIRECT=0` (legacy still draws; recipe is built
  in parallel and validated). This is the Stage 1 gate. Once `INDIRECT=1` is
  enabled, `PARITY=1` continues to validate the per-frame thin record. Resolves
  brainstorm Q4 follow-up.
- Both env vars added to `scripts/run_smoke.py` env-allowlist propagation
  (currently lines 232-247-ish per renderWater convention; verify exact line
  during implementation).
- `[INSTR v1] enabled: ...` startup banner extended with `terrain_indirect`
  and `terrain_indirect_parity` fields.

## NOT in scope

- **drawPass outer-loop elimination.** Q1 (c) — explicit follow-up slice.
  Once (b) ships and the per-quad iteration is "walk + lookup + pack thin
  record," moving that to GPU compute becomes a clean isolated next slice.
- **GPU compute writes indirect command buffer.** Q2 (b) — explicit follow-up.
  CPU-built indirect is sufficient for this slice; GPU compute is the (c)
  follow-up's natural shape.
- **drawMine and debug-overlay legacy contract retirement.** Q5 — explicit
  follow-up. drawMine is unzoned/low-cost; debug overlays are rare/toggled.
  Bundling adds scope without measurable benefit.
- **SIMD on `vertexProjectLoop` per-vertex math.** Per
  `vertexproject_loop_asymptotic.md`, scaffolding ready, slice not queued.
- **Object cull / GPU static props.** Parked workstream
  (`cull_gates_are_load_bearing.md`).
- **Shadow / SSAO / post-process pipeline.** Already GPU-resident.
- **Mod content.** Per orchestrator scope.
- **Rewriting any other slice that's already shipped.** M0-M2, Shape C,
  renderWater, vertexProjectLoop slices stay as-is.
- **Frustum-aware admission via GPU compute.** First cut walks quadList on
  CPU; GPU clip-cull does the work. CPU per-quad iteration is the (c) target.

## Build sequence

> Each numbered stage ships behind the env gate, validates against the 4-gate
> ladder for that stage's deliverable, and updates the orchestrator status
> board on land.

### Stage 0 — Recon (no code yet)

Resolve the 8 recon items above. Output to
`docs/superpowers/plans/progress/2026-MM-DD-indirect-terrain-recon-handoff.md`.
Specifically:
- Map the render-order placement (Recon #1).
- Audit thin-record format coverage (Recon #2).
- Inventory mutation events and chokepoints (Recon #3).
- Confirm stock smoke missions' mutation profile (Recon #4).
- Identify drawMine sites in smoke set (Recon #5).
- Confirm killswitch semantics (Recon #6).
- Decide aggregator zone (Recon #7).
- Document promotion sequence (Recon #8).

Stage 0 deliverable is a recon doc and any pre-implementation memory updates
(e.g., a new memory file if mutation chokepoints surface a load-bearing pattern).

### Stage 1 — Dense recipe build

Convert M2 recipe SSBO from sparse `unordered_map` to dense array indexed by
`vertexNum`. Build at `Terrain::primeMissionTerrainCache`
(`mclib/terrain.cpp:575`); the WaterStream::Build() call at `:599` is the
sibling pattern. Wire `setTerrain` (via `Terrain::setTerrain` at
`mclib/terrain.cpp:875` → `MapData::setTerrain` at `mclib/mapdata.cpp:1293`)
and `setMine` invalidation hooks to a granular
`invalidateRecipeFor(vertexNum)`. Add defensive hooks for `setOverlay` /
bridge-destruction per Recon #3. Wire `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`
to compare new recipe contents to legacy `quadSetupTextures`-derived
contents per frame.

**Gotchas active at Stage 1:** #2 (texture-handle indirection — recipe stores
slot indices), #5 (sampler — recipe doesn't bind samplers but spec test
needs them at draw), #8 (map-stable indexing — primary concern of Stage 1).

Stage 1 gate: parity-check zero mismatches across tier1 with `INDIRECT=0` and
`PARITY=1` (recipe is built; legacy still drives draw; parity validates the
recipe).

### Stage 2 — Indirect draw

Per-frame: walk live quadList, pack thin record, build indirect command
buffer, emit `glMultiDrawElementsIndirect`. Hook in `gamecam.cpp` per
Recon #1. Bridge function (similar to `gos_terrain_bridge_renderWaterFast`)
for state save/restore, sampler, depth pipeline.

**Gotchas active at Stage 2:** #1 (uniform uint), #3 (matrix transpose
flags), #4 (VAO 0 rebind), #6 (render order — primary concern of Stage 2),
#7 (CPU pre-cull is the frustum gate), #9 (depth-state — terrain WRITES
depth so different from water's GL_FALSE depth mask).

Stage 2 gate: tier1 5/5 PASS triple. Visual canary clean. Tracy delta ≥50%
on `quadSetupTextures`. Parity-check still zero mismatches with full
`INDIRECT=1+PARITY=1`.

**Debug instrumentation per `memory/debug_instrumentation_rule.md`:** Stage
1 + Stage 2 each ship lifecycle-only env-gated `[TERRAIN_INDIRECT v1]`
prints (init, recipe-build, mutation-event invalidate, first-draw,
teardown). Format mirrors the existing
`[PATCH_STREAM v1] event=thin_record_parity` and `[WATER_PARITY v1]`
lines. Demote to silent (gated off by default) post-fix; do NOT delete.
This is project convention for any rework touching object lifecycle, cull
gates, render path, resource lifetime, or cross-system control flow.

### Stage 3 — Promotion (separate PR)

After soak (a few days, real-world canary including incidental mod-content
noise), flip `MC2_TERRAIN_INDIRECT` default-on with `=0` killswitch. Per Q5
two-PR promotion sequence.

Stage 3 deliverable: orchestrator status-board update, memory file
`indirect_terrain_endpoint.md` (or similar) capturing what shipped + the
4-gate ladder outcome + any new gotchas surfaced. Lessons feed the (c)
follow-up's brainstorm.

## Cleanup deferred to ship

- Demote any debug instrumentation per `debug_instrumentation_rule.md` (gated
  off by default, not deleted).
- Update `MEMORY.md` index when new memory files land.
- Mark Status Board's "Queued (next)" row as Shipped; archive the brainstorm
  doc under `docs/superpowers/brainstorms/` as the auditable scope record.

## Cross-references

- Brainstorm: `docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md`
- Orchestrator: `docs/superpowers/cpu-to-gpu-offload-orchestrator.md`
- Pattern template: `memory/water_ssbo_pattern.md`
- Bring-up checklist: `memory/gpu_direct_renderer_bringup_checklist.md`
- Predecessor slices:
  - M2: `docs/superpowers/specs/2026-04-29-m2-thin-record-cpu-reduction.md`
  - Shape C: `docs/superpowers/specs/2026-04-28-patchstream-shape-c-design.md`
  - renderWater: `docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md`
- Memory (gotcha + pattern files, by topic):
  - **Pattern templates and predecessors:**
    - `memory/water_ssbo_pattern.md` (the reusable template)
    - `memory/m2_thin_record_cpu_reduction_results.md` (M2 thin-record pattern)
    - `memory/patchstream_m0b.md` (persistent-VBO seam)
    - `memory/patchstream_shape_c.md` (recipe cache + invalidation chokepoints)
    - `memory/water_rendering_architecture.md` (water as separate overlay; pattern parallel to terrain)
    - `memory/renderwater_fastpath_stage2.md` (Stage 3 parity surfaced 3 silent bugs)
    - `memory/vertexproject_loop_asymptotic.md` (parity infra validated on 3rd pattern shape)
  - **GPU-direct rendering gotchas:**
    - `memory/gpu_direct_renderer_bringup_checklist.md` (consolidated 9-trap list)
    - `memory/uniform_uint_crash.md` (#1)
    - `memory/mc2_texture_handle_is_live.md` (#2)
    - `memory/terrain_mvp_gl_false.md` (#3)
    - `memory/sampler_state_inheritance_in_fast_paths.md` (#5)
    - `memory/render_order_post_renderlists_hook.md` (#6)
    - `memory/clip_w_sign_trap.md` (#7)
    - `memory/terrain_tes_projection.md` (#7 supporting; projection chain)
    - `memory/quadlist_is_camera_windowed.md` (#8 — map-stable indexing trap)
    - `memory/gpu_direct_depth_state_inheritance.md` (#9)
    - `memory/mc2_argb_packing.md` (BGRA-in-memory swizzle for ARGB DWORDs)
  - **NOT applicable to terrain (read for the distinction):**
    - `memory/cull_gates_are_load_bearing.md` (object cull cascade; terrain quads not lifecycle-managed)
    - `memory/tgl_pool_exhaustion_is_silent.md` (TG_VertexPool for object props; terrain uses patch_stream)
    - `memory/shadow_caster_eligibility_gate.md` (TG_Shape::Render shadow gate; terrain shadow is separate)
  - **Architectural rules / scope:**
    - `memory/feedback_offload_scope_stock_only.md` (validation scope — stock missions only)
    - `memory/stock_install_must_remain_playable.md` (architectural rule)
    - `memory/debug_instrumentation_rule.md` (env-gated `[SUBSYS]` prints, lifecycle only)
    - `memory/tracy_profiler.md` (Tracy zone conventions)
    - `memory/feedback_smoke_duration.md` (smoke-runner default duration)
