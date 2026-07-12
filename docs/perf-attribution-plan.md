# Perf Attribution Plan (R0–R3) — supersedes the S2-only framing

**Date:** 2026-06-09 · Branch `claude/nifty-mendeleev`

## Why this exists (the correction that triggered the revamp)

Stage 0 measured `TG_Shape::MultiTransformShape` (CPU clip-space bake) at ≤~1.2ms peak / ~0.5ms
typical on the densest object map, ~0 elsewhere. The first instinct ("S2 is dead") was **wrong** for
a subtle reason:

> The old S2 plan (`docs/archive/superseded/S2_IMPLEMENTATION_PLAN.md`) activates the existing GPU-VB `TG_RenderShape`
> color-submit path but **explicitly keeps `MultiTransformShape` running** (it's still needed for
> backface cull, visible-face lists, shadow verts, CPU fallback, and the highlight/lighting bake).
> So if the measured 1.2ms lives *inside* `MultiTransformShape`, **old-S2 does not remove it** — it
> only touches the later submit path.

Correct conclusions:
1. **1.2ms peak is worth caring about** — but only via a *local, low-risk* patch. It is NOT worth a
   renderer migration carrying highlight-loss + CPU↔GPU lighting + multi-texture + fog/water + double-
   submit risk.
2. **Old-S2 attacks the wrong part.** Don't implement it. Re-scope into S2a (measure the submit path)
   and S2b (true `MultiTransformShape` elimination — much bigger than old-S2, only if justified).
3. **We don't yet know where the 55ms/frame (1K map, 18fps) goes.** Terrain is now <100µs (LOD-chunk
   arc). MultiTransformShape is 0.018ms there. So the 55ms is somewhere we haven't attributed. Find it
   before writing any renderer code.

The next work is a **frame-time attribution plan**, not a terrain plan and not a dynamic-object plan.

## Huge leverage: most of this is already instrumented (drive, don't build)

| Need | Existing tool | Headless? | Gap |
|------|---------------|-----------|-----|
| Frame phase split (logic/render/present/cap) | `MC2_HITCH_TRACE=1` → `[HITCH_PHASE]` (mc2_hitch_trace.cpp:146) | ✅ stderr | none |
| Water / terrain-tex / static-flush sub-costs | `[HITCH_WATER]`, `[HITCH_TERRAIN_TEX]`, `[HITCH_STATIC_FLUSH]` (same flag) | ✅ | none |
| Object update split (bldg/tree/generic/mShape/shape) | `MC2_OBJECT_RECON_TRACY=1` → `[OBJECT_RECON v1] frame=` | ✅ (now whitelisted in run_smoke) | none |
| MultiTransformShape cost | `OBJECT_RECON` `shape={ns,calls,alloc,vlight,flight}` | ✅ | none |
| Per-pass GPU/CPU zones | Tracy zones: `Render.3DObjects`, `Render.TerrainSolid`, `Render.PostProcess`, `Render.SSAO`, `textureManagerRenderLists`, `RenderLists.*`, `Shadow.*`, `SwapWindow.*`, `Render.GpuStaticProps` | ⚠️ needs Tracy UI connected | headless wall-ms dump for the few that matter |
| Present GPU-backpressure vs vsync | `MC2_PRESWAP_FINISH=1` moves the stall into `SwapWindow.PreFinish` (glFinish) vs staying in `SwapWindow.SDL` | ✅ (Tracy) / partial stderr | confirm a stderr readout |
| TG renderLists flush wall-ms + draw-call/texture-switch count | — | — | **new thin counter (S2a)** |
| Gesture FSM / anim cost | — | — | **new thin timer (R2/S6)** |

Genuine new code is small: a draw-call/texture-switch + renderLists wall-ms dump (S2a), and a gesture
FSM timer (S6). Everything else is run-and-read.

---

## R0 — Frame-time split (DO FIRST; locate the 55ms)

**Goal:** attribute the 1K-map 55ms/frame (and a normal map for contrast) to top-5 costs.

**Method (zero new code):**
```bash
MC2_HITCH_TRACE=1 MC2_HITCH_MS=20 MC2_OBJECT_RECON_TRACY=1 \
  py -3 scripts/run_smoke.py --mission 1kbasicmap --duration 50 --keep-logs
# contrast:
MC2_HITCH_TRACE=1 MC2_HITCH_MS=10 MC2_OBJECT_RECON_TRACY=1 \
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 30 --keep-logs
```
Read from the mission log: `[HITCH_PHASE]` (gameLogic vs render vs present vs cap ms),
`[HITCH_WATER]`/`[HITCH_TERRAIN_TEX]`/`[HITCH_STATIC_FLUSH]`, and `[OBJECT_RECON v1] frame=` for the
object-update share. **Output:** the top-5 frame costs on the 55ms frame, as a table.

**Decision branches:**
- If **present** dominates → go to R3.
- If **render** dominates → go to R1 (which pass? terrain/renderLists/3DObjects).
- If **gameLogic** dominates → R2 (and broader AI, not just gesture).

---

## R1 — Render-pass + TG submit split (decides whether S2 matters at all)

**Goal:** within "render", separate `MultiTransformShape` (transform) from the submit/flush path, and
decide S2a vs S2b vs neither.

**Existing:** `OBJECT_RECON` gives MultiTransformShape. Tracy zones give `textureManagerRenderLists`,
`RenderLists.*`, `Render.3DObjects`, `Render.TerrainSolid`, `Render.PostProcess`.

**Gap → S2a (small, new):** a headless wall-ms + count dump for the TG submit/flush:
- `TG_Shape::Render` addVertices loop wall-ms (the per-tri `gos_VERTEX` assembly + `addVertices`)
- `mcTextureManager` bucket-insertion cost
- `renderLists()` TG-flush wall-ms (wrap the existing `textureManagerRenderLists` zone region)
- total draw-call count + texture-switch count per frame
Mirror the `OBJECT_RECON`/`FX_COUNT` pattern: env `MC2_TG_SUBMIT_STATS`, atexit/`puts` summary, default OFF.

**Decision:**
- If submit/renderLists is the big cost (not MultiTransformShape) → design a **submit/batching** fix
  (texture-sorted batching, fewer draw calls). This is lower-risk than transform migration and is the
  real lever if draw-call count is high.
- If only `MultiTransformShape` is 0.5–1.2ms and submit is cheap → **do NOT do the risky GPU path.**
  Consider only a *local* MultiTransformShape micro-opt (see S2b note) if R0 ranks it top-5.

### S2a — TG render-submit measurement (the new, safe first step of "S2")
Scope above. Read-only + thin counter. No renderer behavior change. This replaces old-S2 Stage 1/2 as
the next actionable S2 work.

### S2b — true MultiTransformShape elimination (only if R1 proves it's worth it)
NOT the old Stage 2. This means actually retiring the per-frame CPU transform for eligible opaque
shapes: GPU-side transform, GPU/fixed-function backface cull, **separate** shadow-vert handling,
highlight uniform, an explicit CPU↔GPU lighting-parity decision (tolerance or re-baseline), and a
fallback for alpha/window/spotlight/multi-texture. This is a large slice — justified only if R0 ranks
MultiTransformShape top-3 AND R1 shows the submit path is already cheap (so transform is the residual).

---

## R2 — S6 gesture FSM Stage 0 (cheap close-out of the last unmeasured dynamic claim)

**Goal:** the only dynamic-pipeline cost never measured. Original claim: ~100µs/mech (× ~30 mechs ≈ 3ms
— would be significant if true). Note: `OBJECT_RECON mShape` (appearance update) is only ~3–5µs/mech,
which makes the 100µs claim suspect — but the gameplay **gesture FSM lives in `BattleMech::update`
(mech3d.cpp ~3893 region), a different scope** not covered by OBJECT_RECON. So genuinely unmeasured.

**Gap → new thin timer (env `MC2_GESTURE_STATS`):** wall-ms accumulators around the gesture state
machine, frame interpolation, and node recalc in `BattleMech::update` / `Mech3DAppearance::update`;
per-mech count. atexit summary. Default OFF.

**Decision:** if gesture/anim is genuinely >1ms total in mc2_17 combat → optimize (pre-baked transition
table). If it shrinks (likely, per the pattern) → formally close the dynamic-pipeline arc.

---

## R3 — Present / swap stall investigation

**Goal:** explain the present cost (a ~45ms present stall is noted at gameosmain.cpp:1544).

**Method (mostly existing):**
```bash
MC2_HITCH_TRACE=1 MC2_PRESWAP_FINISH=1 \
  py -3 scripts/run_smoke.py --mission 1kbasicmap --duration 50 --keep-logs
```
- `[HITCH_PHASE]` present ms = the present cost.
- `MC2_PRESWAP_FINISH=1`: if the cost moves into `SwapWindow.PreFinish` (glFinish) → it's **GPU
  completion backpressure** (GPU draining terrain/shadows) → optimize GPU work, not CPU. If it stays in
  `SwapWindow.SDL` → it's **vsync/windowing** → not a CPU-code problem; do not chase it.
- Compare minimized vs visible (the smoke runs minimized → SDL throttles to ~100fps; the 18fps on 1K is
  NOT the throttle, it's real GPU cost). Optionally vsync on/off if a toggle exists.

**Decision:** GPU backpressure → the GPU hotspot is the real target (likely terrain/shadow GPU passes,
visible in `Render.TerrainSolid`/`Shadow.*` Tracy zones). Vsync/windowing → leave it.

---

## Sequencing & ranking (revised)

1. **R0** — frame split (zero code). Locates the 55ms. **DO FIRST.**
2. **R3** — present probe (zero code, can run alongside R0). Likely explains a big chunk of the 1K 55ms.
3. **R1 / S2a** — TG submit split (thin counter). Decides if any S2 work is justified, and whether it's
   the safe submit/batching lever or the risky transform migration (S2b).
4. **R2 / S6** — gesture Stage 0 (thin timer). Cheap close-out of the last unmeasured claim.
5. **S2b** — true transform elimination. Only if R0+R1 prove it's a top-3 cost with a cheap submit path.

### What changed vs the old S1–S6 arc
- **S2 is not dead — it was mis-scoped.** Split into S2a (measure submit) + S2b (real transform kill).
- The headline target is no longer "dynamic objects" — it's **frame-time attribution** (R0) with
  **present** (R3) and **renderLists/submit** (R1) as the prime suspects now that terrain is <100µs.
- S1 (done/low), S3 (done), S4 (unmotivated until R1), S5 (deferred, scales with object count) unchanged.

**Bottom line:** measure submit/renderLists and present *before* writing renderer code. Keep the 1.2ms
on the board as a candidate for a *small* fix, not a migration.

---

## R0 RESULTS — Measured 2026-06-09 (`MC2_HITCH_TRACE`, zero new code)

`[HITCH_PHASE]` per-frame wall-ms split (steady-state samples):

| Mission | dt (ms) | logic | render | present | unknown | object-update (OBJECT_RECON) |
|---------|--------:|------:|-------:|--------:|--------:|------------------------------|
| mc2_10 (normal) | 14.0 | 1.98 | 1.28 | 0.29 | 9.73 | bldg 0.026 / shape 0.067 ms |
| **1kbasicmap (1K)** | **61.0** | **50.72** | **0.45** | 0.04 | 9.72 | bldg 0.056 / shape 0.004 ms |

### Findings (decisive redirect)
1. **The 1K-map 18fps/61ms is GAME-LOGIC-bound: logic=50.7ms = 83% of the frame.** Render is 0.45ms,
   present 0.04ms. Rendering is NOT the bottleneck on large maps.
2. **The 50ms logic is NOT object/appearance update** — OBJECT_RECON accounts for only ~0.072ms of it.
   The remaining ~50.6ms is uninstrumented game logic: **AI / pathfinding / MOVE-map / GlobalMap**
   over the oversized 1K grid (`ObjectManager::update`, `MoverGroup::calcMoveGoals`,
   `GoalManager::calcRegions`, movement/region calcs that scale with map size).
3. **`unknown≈9.7ms` is constant across both maps** → a fixed per-frame overhead, almost certainly the
   **minimized-window SDL throttle** (~100fps floor in smoke), i.e. a measurement artifact, not a cost.
4. **Normal maps (mc2_10) have no real bottleneck:** logic 2ms + render 1.3ms + present 0.3ms = ~3.6ms
   real work; the rest is throttle/cap. 64fps is the minimized-throttle artifact, not a perf problem.

### Consequence for the S-arc and R-plan
- **The entire render-side arc (S1, S2/S2a/S2b, S4, S5, S6) is irrelevant to the only real
  bottleneck.** All of it attacks the ≤1.3ms render phase. R1 (TG submit split) and S2b (transform
  elimination) would optimize a phase that is 0.45–1.3ms.
- **R3 (present) is also moot** here — present is 0.04–0.29ms; the `unknown` is throttle, not a present
  stall (the earlier "~45ms present stall" note was about a different/visible-window scenario or is
  superseded by this measurement; PhasePresent is tiny in smoke).
- **The real target — IF large-map perf matters — is the game-logic / AI / pathfinding / MOVE
  subsystem**, a completely different area from this whole arc. Next step there would be an R0-style
  split *inside* `Environment.DoGameLogic()` / `ObjectManager::update()` (sub-phase timers around
  pathfinding, MOVE/region calcs, AI) to find which calc is the 50ms on the 1K grid.

### Open product question (for the user)
The 50ms-logic problem is **specific to the oversized 1K fixture map**; normal mission maps are fine.
Decide: does large-/oversized-map gameplay perf matter for the project? 
- **Yes** → pivot to a game-logic/pathfinding attribution + optimization arc (new; not S1–S6, not
  rendering). The MOVE-grid pathfinding scaling is the prime suspect.
- **No** → there is no current perf bottleneck worth chasing; the render arc closes, and the oracle
  infrastructure (counters + screenshot) remains for future *correctness* work.

---

## P1 STAGE 0 RESULTS — Measured 2026-06-09 (MC2_MOVE_RECON + SLIMSPLIT + mc2_24 control)

Hypothesis under test: the 1K-map 50ms logic is mover-AI / pathfinding / MOVE-map.
**Verdict: REJECTED. The 50ms is NOT pathfinding.** Four independent measurements:

| Subsystem | cost | tool |
|-----------|------|------|
| Mover AI (`control.update`) on 1K (2 movers) | ~8µs/frame | MOVE_RECON |
| Pathlock range updates on 1K | ~1.6µs/frame | MOVE_RECON |
| Path search / A* on 1K | 0 (0 replans; 5s-throttled) | MOVE_RECON |
| Object appearance update | 0.07ms | OBJECT_RECON |
| Terrain slimReduce/makeLists | **0 cycles** (gated off by default chunk path) | SLIMSPLIT |
| Render draw | 0.45ms | HITCH_PHASE |
| **Terrain-logic (mission.cpp:544/567/569)** | **~50ms (by elimination)** | wall-timer TODO |

### The mc2_24 control (the decisive test)
mc2_24 stresses AI movement hard: ~41 mechs pathfinding after a gate/wall event, 1287 replans.
Measured: ctrl 59µs + pathlock 16µs + astar 31µs ≈ **~106µs/frame average**, peak ~1–2ms; total
HITCH logic stays **2.3ms** at 67fps. **AI/pathfinding is cheap even under maximum load.** If it were
the 1K 50ms, mc2_24 (far more movement) would have shown it. It did not. Pathfinding is conclusively
exonerated as a perf bottleneck.

### Standing facts (independent of perf)
- **Crash fixed** (commit 77a0c12a): all MissionMap cell accessors inBounds-guarded. The pathlock
  OOB-AV class is closed.
- **MOVE-grid / terrain-grid mismatch is real**: `offmap_noops` ≈ 4/frame on the 1K map (movers
  querying cells beyond the 720² MOVE grid, absorbed by the guard). An architecture smell worth
  documenting, but NOT a perf cost.
- astar_global (area-graph search) is the larger A* component (~22µs/frame, 0.58ms peak on mc2_24) and
  scales with area count — a future watch-item on very large multi-area maps, not a current problem.

### Conclusion & next step
- **Close the pathfinding-perf idea.** Measured cheap under max load. No new pathfinding arc.
- **The 1K 50ms is terrain-logic** (`land->update` / `terrainTextures->update` / `land->geometry`
  minus slimReduce). One wall-timer on mission.cpp:544/567/569 pinpoints which call. This is the
  **existing terrain-LOD-chunk arc's** territory (Phase 8z), not a new arc.
- MC2_MOVE_RECON instrumentation left uncommitted (scaffolding; code/objmgr.cpp is mixed with
  pre-existing R2b work — split the hunk before persisting if desired).
