# Indirect Terrain Draw — Scope Brainstorm

**Date:** 2026-04-30
**Status:** Brainstorm (not a spec). User signs off on scope before spec session opens.
**Workstream:** CPU → GPU offload (final architectural slice, 23-of-24).
**Prereqs:** orchestrator status board (`docs/superpowers/cpu-to-gpu-offload-orchestrator.md`),
`memory/water_ssbo_pattern.md`, `memory/gpu_direct_renderer_bringup_checklist.md`,
`memory/quadlist_is_camera_windowed.md`, `memory/m2_thin_record_cpu_reduction_results.md`,
`memory/patchstream_shape_c.md`, `memory/renderwater_fastpath_stage2.md`,
`memory/vertexproject_loop_asymptotic.md`, `memory/cull_gates_are_load_bearing.md`.

This brainstorm answers Q1–Q7 from the orchestrator's "Brainstorm pending" row. It
does NOT design implementation; that's the spec session's job. The intent here is
to nail down WHAT gets retired, with what gates, with what trade-offs — so the
spec session has unambiguous inputs and the user can redirect scope before code
gets written.

---

## TL;DR (the spec session inherits these decisions)

| Q | Decision | One-line why |
|---|---|---|
| **Q1** | Scope (b): retire `quadSetupTextures` per-frame iteration. (a) folded in. (c) deferred to follow-up. | (a) alone is sub-ms; (b) targets the real ~3 ms asymptotic remainder; (c) doubles bring-up surface in same slice. |
| **Q2** | CPU builds indirect command buffer per frame. | CPU is already iterating quads to pack thin records; GPU compute for command buffer is the (c) follow-up. |
| **Q3** | Extend M2 recipe SSBO into dense map-stable. Reuse M2 thin-record format. Small new indirect command buffer. Leave water SSBOs untouched. | Right data, wrong shape. Memory cost ~4-10 MB at largest stock map; bounded. |
| **Q4** | Per-thin-record byte-compare (mirror renderWater Stage 3). Visual canary as gate A. | Thin record IS the GPU input. Caught 3 silent bugs in renderWater. |
| **Q5** | Retire contract for terrain main-emit ONLY. Leave `drawMine` and debug overlays on legacy. Default-on at promotion (`MC2_TERRAIN_INDIRECT=0` killswitch). | Mirrors Shape C flip. drawMine is unzoned/low-cost; debug overlays are rare/toggled. |
| **Q6** | Single-point invalidation on `setTerrain` / `setMine` (already exists from slice 2b). `setOverlay` + bridge destruction are open follow-ups. Editor edits out of scope. | Stock missions may have zero in-game terrain mutations; if confirmed, recipe is post-load immutable. |
| **Q7** | Same 4-gate ladder as renderWater. Tracy delta ≥50% on `Terrain::geometry quadSetupTextures`. D triple = unset / INDIRECT=1 / INDIRECT=1+PARITY=1. | Pattern shipped 3 times; reuse > reinvent. |

---

## Q1 — Retirement scope

### Decision: **Scope (b)** — retire `quadSetupTextures` per-frame iteration. (a) folds in as a side-effect. (c) deferred to a follow-up slice.

### Why (a) alone is too small

The M2 fast path already collapses terrain main-emit into a single
`glDrawArrays(GL_TRIANGLES, 0, thinCount * 6)` per pass via the renderLists
flush (`m2_thin_record_cpu_reduction_results.md` line 41:
`Legacy quads/frame: 0`, `FP fast-path quads/frame: 14,000 (every quad)`). Going
to multi-draw-indirect with bucketed commands means collapsing roughly 3-4 draws
(solid + detail + overlay + maybe mine) into one indirect call. The driver-call
saving is sub-millisecond on modern drivers and changes nothing about the
per-frame CPU iteration cost. Not worth its own slice; not worth being the
endpoint of a 24-slice arc.

### Why (b) is where the real win lives

The `quadSetupTextures` arc closed asymptotic at 3.01 ms (`patchstream_shape_c.md`
slice 2a/2b history; orchestrator status board "quadSetupTextures arc —
asymptotic"). That arc closed not because the work was small but because
trivial CPU optimizations had hit the compiler ceiling — same shape as
`vertexProjectLoop` D1 (`vertexproject_loop_asymptotic.md`: "Everything D1 was
supposed to harvest had already been captured by the compiler"). The only path
past asymptotic is an architectural shift: move the per-frame iteration off CPU
entirely, exactly the way renderWater Stage 2 did
(`water_ssbo_pattern.md` lines 13-38).

(b) takes the existing M2 recipe SSBO (sparse hash-cached) and converts it to
a dense, map-stable structure indexed by `vertexNum`. Per-frame CPU work
collapses to: (i) `vertexProjectLoop` (already exists, asymptotic), (ii) walk
quadList, look up recipe by `vertexNum`, pack thin record. No more
`unordered_map::find` per quad per frame. No more per-frame texture-handle
resolution. The 3 ms zone evaporates.

### Why (c) is the right next slice but not this one

The `drawPass` outer loop is now 1.46 ms (`m2_thin_record_cpu_reduction_results.md`).
Eliminating it requires either:
- GPU compute that walks the recipe + camera state and writes the indirect
  buffer (a new pipeline stage), OR
- Collapsing the thin-record build into a single SSBO update with no per-quad
  branching (which means moving every per-quad classifier — pz check, recipe
  lookup, mine state — to GPU).

Both are achievable, but neither is incremental on top of (b). They are
qualitatively different work from the recipe migration. Bundling (b)+(c)
doubles the bring-up surface in a slice that already has 9 GPU-direct gotchas
to satisfy (`gpu_direct_renderer_bringup_checklist.md`). Once (b) ships and
the drawPass loop is just "walk quads, pack thin record, look up recipe," the
path to (c) is well-defined and isolated.

The orchestrator's "and possibly one follow-up" language anticipates exactly
this split.

### Dependency chain confirmed

- (c) requires (b). If recipe is still per-frame computed in
  `quadSetupTextures`, the drawPass iteration depends on its output and
  cannot be made vacuous.
- (b) does NOT require GPU-side admission. Per
  `cull_gates_are_load_bearing.md`, terrain quads are NOT lifecycle-managed —
  that memory is about *object* cull (mechs/buildings/vehicles) and the
  TGL pool / `update()` cascade. Terrain has no equivalent. CPU pre-cull via
  projectZ-derived `pz` per-vertex (`gpu_direct_renderer_bringup_checklist.md`
  trap #7) is sufficient and is already shipped infrastructure.
- (a) folds in as a side-effect of (b): once recipe is dense and thin record
  covers all visible quads, a single indirect draw is the natural draw shape.

### Trade-off (vs (c))

We leave ~1.5 ms of `drawPass` outer-loop cost on the table for the
follow-up. We accept that vs taking on simultaneous risk on recipe migration
AND drawPass elimination. RenderWater spent measurable debug cycles on
bring-up traps even with single-purpose scope; doubling it in a single slice
is not worth the risk for an arc that's been disciplined about incremental
shape across 22 prior slices.

### Open follow-up for spec session

- The render-order trap (`gpu_direct_renderer_bringup_checklist.md` #6) gets
  more nuanced when the new fast path is *terrain itself* (the thing other
  passes' depth tests depend on). RenderWater hooked
  AFTER `mcTextureManager->renderLists()`. Indirect terrain may need to hook
  BEFORE other passes' depth-dependent draws (objects, water, shadow caster)
  but AFTER the `renderLists` setup. Spec must define the exact ordering.

---

## Q2 — Indirect command buffer source

### Decision: **(a) CPU builds the indirect command buffer per frame.**

### Why

With Q1 = (b), CPU is already iterating quads to pack thin records (the natural
extension of the M2 fast-path loop in `TerrainQuad::draw`). The indirect
command buffer in this shape is small: 1 to roughly 4 command structs (one per
draw bucket — solid base, detail, overlay base, possibly mine). Building it on
GPU compute would mean spinning up a compute shader to write 4 structs, which
costs more in dispatch overhead than the savings.

GPU-frustum cull (the (b)-option in the question) is exactly the (c)
follow-up: replace CPU iteration with a compute shader that walks the recipe
and emits indirect commands. That's a real architectural endpoint, but it's
the *next* endpoint, not this one.

### Why GPU-frustum cull is moot for terrain in this slice

The orchestrator's revision note (2026-04-30): terrain quads aren't
lifecycle-managed the way object props are. There's no equivalent of
"object update gated by inView" or "TGL pool exhaustion silent drop"
(`cull_gates_are_load_bearing.md`). CPU pre-cull on per-vertex
`pz ∈ [0,1)` plus per-tri valid bits (`water_ssbo_pattern.md` line 52,
`m2_thin_record_cpu_reduction_results.md`) is the load-bearing cull. The VS
already emits degenerate `gl_Position` for invalid tris. No GPU compute
needed; the cull is already on the GPU side via degenerate emit.

### Trade-off (vs (b) GPU compute)

CPU still iterates `quadList` per frame. That's ~14000 iterations × small
constant work. With (b) Q1 retiring `quadSetupTextures`, this is the residual
per-frame CPU cost the (c) slice will target. Acceptable for now.

### Open follow-up for spec session

- When (c) lands, decide whether GPU compute writes (i) the indirect command
  buffer directly, or (ii) the thin record + a one-line CPU-built indirect.
  Option (ii) is simpler; option (i) requires barrier discipline between
  compute and draw. Both work.
- If the spec session finds the per-quad CPU iteration is dominated by recipe
  hash-lookup (now eliminated by dense indexing), the residual cost may
  already be small enough to skip (c) entirely. Profile after (b) ships.

---

## Q3 — SSBO topology

### Decision: **Extend the M2 recipe SSBO into a dense map-stable structure. Reuse the M2 thin-record format. Add a small new indirect command buffer. Leave water SSBOs untouched.**

### Existing structures (per `m2_thin_record_cpu_reduction_results.md` and `water_ssbo_pattern.md`)

- M1d/M2 recipe SSBO — terrain per-quad world data, sparse `unordered_map<key, recipeIdx>` keyed by `(wx0, wy0)`. Lazily filled. Per Shape C flip, cache-read default-on.
- M1d/M2 thin-record SSBO — per-frame `(recipeIdx, terrainHandle, flags, lightRGBs[4])` 32B records. Triple-buffered ring.
- renderWater recipe + thin-record SSBOs — water-only, parallel to terrain. Dense map-stable. 64B recipe, 48B thin record.

### Why extend rather than merge

The M2 recipe is the *right data* in the *wrong shape*. The sparse hash was
always a workaround for not knowing the map shape in advance — but we DO know
the map shape; `realVerticesMapSide` is set at mission load. Converting to a
dense array indexed by `vertexNum` is a refactor of the recipe-build path,
not a new system. The recipe contents stay the same: terrainHandle, overlayHandle,
detailHandle, uvData, mine state cache (slice 2b), etc. Just packed densely.

The thin-record format (32B) is already minimal and matches GPU shader
expectations. No reason to change it.

A small new SSBO holds the indirect command buffer. Tiny (a handful of structs),
reused frame-to-frame.

### Why NOT merge with water SSBOs

Water Stage 3 just shipped, gates green, parity validated against ~3.2M quads.
Touching that schema means re-validating water and absorbing the whole renderWater
test surface into this slice. Risk: a terrain-driven schema change breaks water
parity for reasons unrelated to terrain. No perf gain — water and terrain run
through different shaders and different per-frame logic.

### Memory cost

Dense recipe at largest stock map: `mapSide² × sizeof(Recipe)`. Stock max is
~256² for the largest mission with ~10K-14K live recipes per the renderWater
Stage 3 reasoning. Recipe size estimate: ~64B (mirror water). Total
~256² × 64B = ~4 MB. Well under any sensible budget. Same memory order as the
renderWater recipe; not duplicative because terrain recipes carry different
content.

### Trade-off (vs keeping all parallel)

We DO change the M2 recipe shape. That's a refactor of the recipe-build
function (where Shape C currently lives) plus the per-frame lookup path.
Bounded scope; unit-testable in isolation. The Shape C cache-read code path
becomes the recipe-population path during the migration; same chokepoints
(`patchstream_shape_c.md` line 35: "`invalidateTerrainFaceCache()` in
`setTerrain()` invalidates the **whole** cache array").

### Open follow-up for spec session

- Confirm the M2 thin-record format covers ALL per-frame state currently
  computed in `setupTextures`. If anything per-frame is in `setupTextures` but
  not in the thin record (e.g., overlay UV state when overlay handle is
  mutable per-frame), it must either flow through the thin record OR the
  recipe is invalidated on those changes (Q6).
- Verify that the dense recipe array can grow if `realVerticesMapSide` ever
  changes mid-mission. (Probably never does on stock; confirm.)

---

## Q4 — Parity gate shape

### Decision: **Per-thin-record byte-compare, mirroring renderWater Stage 3 (`MC2_RENDER_WATER_PARITY_CHECK`). Visual canary as gate A.**

### Why this beats the alternatives

**Indirect-command-buffer byte-compare** is too low-level. The buffer is
just count + offset, derivable from the thin record. If thin records match,
the indirect buffer matches by construction. No new information surfaced.

**Screenshot diff** is too brittle for a primary gate. Animation phase,
particle state, post-process noise drift between runs even at fixed seed.
Past sessions have seen screenshot-diff false-positive at 1-2 ULP. Useful
as a coarse canary for visual regression (gate A); not useful as the byte-
level correctness check.

**Per-vertex output byte-compare** (the vertexProjectLoop pattern) doesn't
apply directly because terrain doesn't have an equivalent CPU-output stream.
The per-vertex emit IS what we're moving to GPU. Comparing CPU-derived
vertices to GPU-derived vertices runs into the "post-projection x/y/z drift
below 1 ULP" problem the renderWater Stage 3 spec explicitly avoided
(`water_ssbo_pattern.md` line 106).

### What per-thin-record byte-compare covers

The renderWater Stage 3 outcome (`renderwater_fastpath_stage2.md` lines
19-25): 3 real bugs caught silently — recipe coverage gap, blank-vertex
skip drift, fogRGB low-byte material patch. Each was a CPU-side state
divergence that would have shipped without it because none affected pixels
on the screen at the smoke-camera angles. The byte-compare on inputs
(recipe contents + thin-record contents + derived per-corner per-tri
u/v/argb/frgb-high) catches every state-level divergence at frame N, not
"wait till someone notices a graphical glitch in mission Y."

For terrain, the analogous bugs to expect:
- Recipe coverage gap on map-edge or LOD-transition quads.
- `vertexNum < 0` blankVertex handling drift.
- Per-quad classifier bits (mine state, water-interest, overlay alpha)
  drifting from `setupTextures` legacy behavior.
- Texture-handle slot vs gosHandle resolution (mc2_texture_handle_is_live).

### Implementation shape (per `water_ssbo_pattern.md` parity-check section)

- Env gate: `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`. Default off.
- Silent on success (matches `[PATCH_STREAM v1] event=thin_record_parity` and
  `[WATER_PARITY v1]` conventions).
- Field-level mismatch printer: `[TERRAIN_INDIRECT_PARITY v1] event=mismatch
  frame=N quad=Q layer=<base|detail|overlay> tri=T vert=V field=<name>
  legacy=0xHEX fast=0xHEX`. Throttle to 16/frame (mirror water).
- 600-frame summary always-on counter:
  `event=summary frames=N quads_checked=Q total_mismatches=K`.
- Compare on **inputs** (recipe contents + thin-record contents + derived
  per-corner per-tri u/v/argb/frgb-high). Don't compare post-projection
  x/y/z/rhw; those drift sub-1-ULP and produce false positives.

### Trade-off (vs cheaper visual diff)

The parity check requires running both legacy `quadSetupTextures` AND the new
indirect-recipe path simultaneously when `PARITY_CHECK=1`. Doubles per-frame
CPU during parity runs. Acceptable per renderWater pattern: parity is a
diagnostic mode, not the production hot path. tier1 5/5 PASS triple already
budgets for this (D gate's third leg).

### Open follow-up for spec session

- Define the exact "synthesize legacy emit args from CPU state" code path. The
  renderWater Stage 3 builder ran legacy emit with arg-stream capture; spec
  needs to lift that to terrain's emit shape (more layers — solid + detail +
  overlay + mine vs water's solid + detail).
- Decide whether `PARITY_CHECK=1` requires the full `MC2_TERRAIN_INDIRECT=1`
  stack, or whether it can run with INDIRECT=0 to validate the recipe is being
  built correctly even before the GPU path takes over.

---

## Q5 — `addTriangle` / `addVertices` contract retirement

### Decision: **Retire the contract for terrain MAIN emit only. Leave `drawMine` and debug overlays on legacy. Default-on at promotion via Shape-C-style killswitch (`MC2_TERRAIN_INDIRECT=0` opts out). Soak one cycle before flipping default.**

### Today's contract surface (per `m2_thin_record_cpu_reduction_results.md`)

- `Terrain::render drawPass` — 0 legacy quads / 14000 fast-path. Contract
  already retired here for solid + detail + overlay (M2 + M2c + M2d).
- `Terrain::geometry quadSetupTextures` — iterates per-frame. Doesn't directly
  emit triangles; sets up per-quad texture state. Indirect-terrain retires
  the per-frame iteration here.
- `Terrain::renderWater` — renderWater Stage 2+3 retired the contract for
  water emit.
- `TerrainQuad::drawMine` — still uses legacy contract. "minePass: loop
  ×~40K → TerrainQuad::drawMine() [unzoned, low cost]."
- Debug overlays — still legacy contract, behind toggles (grid/cells/LOS).

### Why retire main emit only

Bundling `drawMine` retirement adds scope without measurable perf benefit.
The mine path is unzoned (no Tracy data → unknown actual cost), runs only
on missions with mines (~2-3 missions across the entire stock campaign per
slice 2b's "campaign-wide ~97% mine-free" finding), and has its own state
machine (mineTextureHandle / blownTextureHandle). Porting it to indirect
draw is doable but distinct work — small and isolated, perfect for a
follow-up if profiles ever surface it as a bottleneck.

Debug overlays are diagnostic, behind toggles, and intentionally simple —
keeping them on legacy contract is a feature.

### Default-on at promotion

Mirrors the precedent set by:
- PatchStream Shape C — `MC2_MODERN_TERRAIN_PATCHES=0` killswitch
  (`patchstream_shape_c.md` line 7: "literal `0` explicitly opts out, any
  other value (including unset) opts in").
- M2 thin-record fast path — flipped default-on once tier1 5/5 PASS triple
  cleared.
- renderWater — pending similar flip; gates green per
  `renderwater_fastpath_stage2.md`.

Killswitch convention for this slice: `MC2_TERRAIN_INDIRECT=0` opts out;
anything else (including unset) opts in. Smoke runner already propagates
env vars via the allowlist (`scripts/run_smoke.py:233-234`); spec must
add `MC2_TERRAIN_INDIRECT` and `MC2_TERRAIN_INDIRECT_PARITY_CHECK` to it.

### Soak before flip

The renderWater Stage 3 close (2026-04-30) didn't immediately flip default —
ship slice opt-in, run tier1 5/5 PASS triple clean, soak a few days for
incidental mod-content noise (out-of-scope for validation but a useful real-
world canary), then flip default in a follow-up commit. Spec should explicitly
encode this as a two-PR sequence:
- PR 1: ship slice, gates green, default off.
- PR 2: flip default on.

### Trade-off (vs full retirement)

`drawMine` and debug overlays remain on legacy contract — divergent from
the new architectural endpoint. Acceptable because:
- They're rare (mine missions) or behind toggles (debug).
- Their cost is low / unmeasured.
- Retiring later is mechanical (mirror the same recipe + thin-record pattern
  for mine state, which is already cached on the recipe per slice 2b).

### Open follow-up for spec session

- Confirm `drawMine` runs on stock missions in the smoke set. mc2_24 has
  mines in vanilla MC2; verify the legacy path still works clean when
  indirect-terrain is on.
- Define exact env-gate semantics: does `MC2_TERRAIN_INDIRECT=0` also turn
  off the recipe SSBO build, or just the indirect draw call? If just the
  draw call, the recipe is still built but unused — wasted work. If both,
  the killswitch is a true legacy fallback. Recommend: full opt-out
  (no recipe build, no thin record, full legacy) for the killswitch.

---

## Q6 — Mutation events

### Decision: **Mirror the Shape-C / slice-2b chokepoint pattern. `setTerrain → invalidateRecipeFor(vertexNum)`. `setMine → invalidateRecipeFor(vertexNum)` (slice 2b infrastructure already exists). `setOverlay` and bridge destruction are open follow-ups for the spec session. Editor edits out of scope.**

### The point (per the user's brief)

The recipe SSBO must NEVER drift silently from CPU state. Either every mutation
has a single-point invalidation hook, or rare events trigger a wholesale
rebuild, or `PARITY_CHECK=1` catches drift before promotion.

### Known events

| Event | Already a chokepoint? | Recommendation |
|---|---|---|
| Terrain edit (editor) | N/A | Out of scope per orchestrator. |
| `setTerrain(vertexNum, terrainType)` | Yes — `invalidateTerrainFaceCache()` in Shape C | Hook a granular `invalidateRecipeFor(vertexNum)` here. Shape C currently invalidates the whole array; a dense recipe should invalidate just the affected entry. |
| `setMine(...)` / blown / unburied | Yes — slice 2b memory: "all 19 callsites route through `GameMap::setMine`" | Already cached on recipe per slice 2b. Confirm chokepoint is still the single mutation site after the dense-recipe migration. |
| `setOverlay(vertexNum, overlayHandle)` | Unknown — needs spec session check | **Open follow-up.** If overlay can change mid-mission (e.g., bridge destruction → overlay swap), need a hook here. |
| Bridge destruction | Unknown — may route through `setTerrain` or `setOverlay`, or have its own path | **Open follow-up.** Spec session needs to grep callsites and confirm. |
| Crater / damage to terrain type | Unknown if MC2 has this on stock | **Open follow-up.** If terrain type itself changes (e.g., explosion turns grass → dirt), `setTerrain` covers it. |
| Mission load (initial recipe build) | N/A — `primeMissionTerrainCache` chokepoint | Wholesale build at this site. Same as renderWater Stage 2 (`water_ssbo_pattern.md` line 18). |

### A potentially load-bearing observation

The smoke set is mc2_01, mc2_03, mc2_10, mc2_17, mc2_24. **Stock missions
in this set may have ZERO in-game terrain mutations.** Mines are present on
some (mc2_24 has them); bridges may exist (mc2_10 / mc2_17?); overlay
mutations are rare on stock content.

If the spec session confirms "no in-game mutations on stock smoke missions,"
then for the 4-gate ladder's purposes the recipe is effectively immutable
post-load. Q6 collapses to:
- Build at `primeMissionTerrainCache`. Never invalidate.
- Hook `setTerrain` / `setMine` / `setOverlay` defensively (because
  out-of-smoke missions or future mission additions might trigger them).
- `PARITY_CHECK=1` validates that the assumption holds.

This dramatically simplifies the spec.

### Trade-off (vs always-running parity check)

Defensive hooks plus `PARITY_CHECK=1` (off by default) is the cheapest gate.
We pay invalidation cost only when the event actually fires. We pay parity-
check cost only when the user sets the env var. We don't pay either in
production after promotion.

### Open follow-up for spec session

1. Grep callsites for `setOverlay`, bridge destruction, and any other terrain
   mutation. Classify each: chokepoint exists / needs new hook / rare enough
   to rebuild whole SSBO.
2. Check stock smoke missions for in-game mutations. If zero, document that
   assumption explicitly so future mod work knows the constraint.
3. Decide invalidation granularity: full-recipe rebuild (cheap to write, may
   drop frames if frequent), per-entry invalidate (more code, no per-frame
   cost), or batched (queue invalidations, flush before next draw).

---

## Q7 — Validation gate shape

### Decision: **Same 4-gate ladder as renderWater. Tracy delta target ≥50% on `Terrain::geometry quadSetupTextures` zone. tier1 5/5 PASS triple = unset / INDIRECT=1 / INDIRECT=1+PARITY=1.**

### The 4-gate ladder (per `water_ssbo_pattern.md` lines 110-116)

- **A.** Visual canary at fixed seed/camera, side-by-side legacy/fast.
- **A'.** (optional) Screenshot diff (automated, fixed seed).
- **B.** Tracy delta on the original CPU zone, target ≥50% reduction.
  Water achieved 78–85%.
- **C.** `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1` zero mismatches across stock-
  content tier1 only.
- **D.** tier1 5/5 PASS triple: unset / INDIRECT=1 / INDIRECT=1+PARITY=1.
  +0 destroys delta on every mission, every state.

### Tracy delta target

Q1 = (b) targets the 3.01 ms `quadSetupTextures` asymptotic remainder. Moving
the per-frame iteration to GPU (or eliminating it via dense recipe) should
achieve well above the 50% target — water Stage 2 pulled 78-85% on a 449-894
µs zone, and `quadSetupTextures` is a much fatter zone with more dispatch
overhead to harvest.

Specifically:
- Pre-(b) `quadSetupTextures`: 3.01 ms (mean, mc2_01 max-zoom-out).
- Post-(b) target: ≤1.50 ms (≥50% reduction).
- Aspirational: ≤0.50 ms (the residual would be just the per-frame thin-record
  build, mirroring renderWater's 88-132 µs fastpath cost).

### Why a per-frame "terrain admission CPU total" aggregator may be useful (stretch)

Q1 = (b) potentially shifts work between zones:
- `quadSetupTextures` ↓ (the targeted zone)
- new `Terrain::IndirectRecipeBuild` zone ↑ (one-time at primeMissionTerrainCache; not per-frame).
- new `Terrain::ThinRecordPack` zone ↑ (per-frame; subsumes some quadSetupTextures iteration).
- `Terrain::render drawPass` ↓ slightly (recipe lookup gets cheaper).

Single-zone delta may understate or overstate the true win. Stretch
recommendation: add a `Terrain::TotalCPU` aggregator that wraps the per-frame
chain (`vertexProjectLoop` + `ThinRecordPack` + drawPass minus the now-skipped
quadSetupTextures iteration). Compare aggregator-to-aggregator across the
env-gate flip. This catches "we moved work" vs "we eliminated work."

### Visual canary specifics

Beyond the standard fixed-camera visual diff, exercise:
- **Shoreline overlay** (mc2_17 water band): same canary water Stage 2 used.
  Catches overlay/depth/blend regressions.
- **Mine sites** (mc2_24): drawMine remains legacy per Q5; verify it still
  draws clean over the new indirect terrain.
- **Wolfman zoom** (high altitude, large frustum): catches LOD edge cases
  where the recipe coverage at map edges matters most. Maps to the
  renderWater Stage 3 "blank vertex skip" bug class (`renderwater_fastpath_stage2.md`
  bug 2).
- **Bridge / structure boundary** (mc2_10 if it has bridges): catches the
  setOverlay/bridge-destruction Q6 follow-up if we get unlucky with mid-
  mission mutation.

### Trade-off (vs a tighter Tracy target)

≥50% is conservative. Setting ≥75% to match water's actual achievement is
tempting but the work shapes differ — water was a single pass of 14K-quad
vertex emit, while quadSetupTextures involves texture-handle resolution
across multiple layers. The conservative target gives the slice room to
finish without chasing micro-optimization in late bring-up. If actual
delta turns out to be 75%+ during smoke, that's a happy result, not a
gate.

### Open follow-up for spec session

- Decide whether to add the `Terrain::TotalCPU` aggregator (recommended)
  or stick to per-zone deltas.
- Whether to require gate A' (screenshot diff) for promotion. Past sessions
  have been brittle; it's been useful as a debugging aid (`water_visual_diff.py`)
  but not as a gate. Recommend optional.
- Stock smoke-set scope confirmation. Per orchestrator scope, validation is
  stock missions only. Mod content is not a gate.

---

## Cross-cutting constraints (load-bearing, must respect)

- **Stock install must remain playable.** New path goes behind
  `MC2_TERRAIN_INDIRECT` env gate. Don't promote default-on until 4-gate
  ladder is green across tier1 stock. Per CLAUDE.md critical rule.
- **Validation is stock missions only.** Per orchestrator scope section.
  mc2_01, mc2_03, mc2_10, mc2_17, mc2_24. Mod content (Carver5O, Magic, MCO,
  Wolfman, MC2X) is not a gate; if a mod regresses, that's the mod's problem.
- **Tessellated terrain displacement, PBR splatting, shadow sampling, and
  water rendering all stay where they are.** This slice is the CPU-side
  admission/recipe path, not the visual pipeline.
- **Cull cascade for terrain is not in scope.** Per
  `cull_gates_are_load_bearing.md`, that doc applies to *object* cull (mechs/
  buildings/vehicles), where bypassing cascades into pool exhaustion + stale
  matrices + destroyed objects. Terrain quads have no such lifecycle. CPU
  pre-cull via per-vertex pz is the load-bearing gate, and it's already
  shipped.
- **The 9 GPU-direct gotchas all apply.** Per
  `gpu_direct_renderer_bringup_checklist.md`. The render-order trap (#6) gets
  more nuanced when terrain itself is the new fast path; the spec must define
  exact ordering.

---

## Out of scope for this slice (explicit)

- **Object cull / GPU static props.** Parked workstream
  (`cull_gates_are_load_bearing.md`).
- **Dynamic unit GPU transforms.** Touches gameplay; out of CPU→GPU offload arc.
- **Shadow / SSAO / post-process pipeline.** Already GPU-resident.
- **Mod content.** Per orchestrator scope.
- **Rewriting any other slice that's already shipped.** M0-M2, Shape C,
  renderWater, vertexProjectLoop slices stay as-is.
- **drawPass outer-loop elimination.** Q1 (c) — explicit follow-up.
- **GPU compute for indirect command buffer.** Q2 (b) — explicit follow-up.
- **drawMine / debug-overlay legacy contract retirement.** Q5 — explicit follow-up.
- **SIMD on `vertexProjectLoop` per-vertex math.** Per
  `vertexproject_loop_asymptotic.md`, scaffolding ready, slice not queued.

---

## Summary of open follow-ups (the spec session resolves these)

1. **Render-order ordering** when terrain itself is the new fast path
   (Q1 / Q2). Where exactly to hook in `gamecam.cpp` between `renderLists()`
   and other passes that read the depth buffer.
2. **Bridge destruction path** — does it route through `setTerrain` /
   `setOverlay`, or has its own logic? (Q6).
3. **Stock smoke missions in-game terrain mutations** — confirm zero, or
   list any that exist. Determines whether recipe is post-load immutable
   in practice (Q6).
4. **Thin-record format coverage** — does the existing 32B M2 thin record
   hold ALL per-frame state currently computed in `setupTextures`? If gaps
   exist, either thin record grows or recipe invalidates on those changes
   (Q3).
5. **`drawMine` on stock smoke missions** — does mc2_24 (or others)
   exercise it cleanly when `MC2_TERRAIN_INDIRECT=1`? (Q5).
6. **Killswitch semantics** — `MC2_TERRAIN_INDIRECT=0` should be a full
   legacy fallback (no recipe build, no thin record, no indirect draw). Spec
   formalizes this. (Q5).
7. **Tracy aggregator zone** — add `Terrain::TotalCPU` per-frame aggregator,
   or stick to per-zone deltas? (Q7).
8. **Two-PR promotion sequence** — explicitly encode "ship default-off, soak,
   flip" in the spec rollout plan. (Q5).

---

## Closing note

The arc has shipped 22 slices with disciplined small-scope incrementalism.
Indirect terrain draw is the architectural endpoint, but the discipline that
got us here matters more than getting all the way to the endpoint in one
slice. (b)+(c) bundled would risk the kind of "Stage 3 caught 3 silent bugs"
debugging cost (`renderwater_fastpath_stage2.md`) being doubled. (b) alone,
done systematically with the 4-gate ladder and per-thin-record byte-compare,
is on the established pattern.

The user signs off on this brainstorm before a spec session opens. Spec
session: `docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md`.
Implementation downstream of spec.
