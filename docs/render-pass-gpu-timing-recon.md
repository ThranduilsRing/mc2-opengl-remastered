# Render-pass GPU timing recon — `[RENDER_PASS_TIME v1]` spec

**Date:** 2026-06-11 · RECON/SPEC ONLY — no code shipped. Branch `claude/nifty-mendeleev`.
**Question:** where do `GL_TIME_ELAPSED`/`GL_TIMESTAMP` scopes go around every RenderWorld
pass, what exists already, and what telemetry format do we emit headlessly?

---

## (a) Existing timer infrastructure inventory

| Infra | Where | Mechanism | Headless? | Reusable? |
|---|---|---|---|---|
| **EditorGpuTimer** | `editor/EditorGpuTimer.{h,cpp}` (217 lines) | `glQueryCounter(GL_TIMESTAMP)` mark-chain, ping-pong double buffer, read previous frame's buffer in `Begin()` (never stalls), `kMaxMarks=16`. Env `MC2_EDITOR_GPU_TIMERS=1`. Emits `[EDGPU f=N] sky=.. terrain=.. TOTAL=..` + parallel CPU line `[EDCPU]` + pre-render `[EDPRE]` + whole-frame `[EDFRM]` to editor-startup.log | yes (stdout/log file) | **YES — this is the template.** Port pattern, not the file (editor-only link target, writes to editor-startup.log, single hardcoded log path, 2-deep ping-pong assumes ≤1 frame latency) |
| **Tracy GPU zones** | `TracyGpuZone(...)` in `mclib/txmmgr.cpp`, `GameOS/gameos/gos_postprocess.cpp` (full list in section b — the zone set IS the pass enumeration) | Tracy OpenGL timestamp calibration | no (needs Tracy UI connected) | zone *placement* is the authoritative pass-boundary map; reuse the same scopes |
| **gpu_cull_compute spike timer** | `GameOS/gameos/gpu_cull_compute.cpp:987` `glBeginQuery(GL_TIME_ELAPSED, s_timerQuery)` … `:1298`/`:1342` `glEndQuery`, deferred availability-checked readback (`s_timerPending`, polled next dispatch ~line 980) | single GL_TIME_ELAPSED query, poll-don't-stall | yes | pattern proof that GL_TIME_ELAPSED + deferred poll works in this engine |
| **Water spike timer** | `GameOS/gameos/gos_terrain_water_stream.cpp:1710/1754` (`s_spikeOn`-gated GL_TIME_ELAPSED around compute dispatches) | same | yes | same pattern |
| **MC2_HITCH_TRACE** | `GameOS/gameos/mc2_hitch_trace.cpp:146` `[HITCH_PHASE]`; scopes at `gameosmain.cpp:1522` (PhaseRender) / `:1582` (PhasePresent) | CPU wall clock | yes | the CPU-side frame split this spec complements (GPU side was the R0 gap, perf-attribution-plan.md:37 "headless wall-ms dump for the few that matter") |
| **debug_state_dump / mc2-render-state MCP** | `GameOS/gameos/debug_state_dump.cpp`, schema `docs/debug_state_schema.md` (V1), MCP `docs/mcp-render-state.md` | JSON every 300 frames, additive fields allowed without schema bump | yes | integration target for section (e) |

Constraint from CLAUDE.md profiling rules: **coarse per-pass zones only, 100ns floor** —
this spec adds ~14 query pairs/frame, well within that.

## (b) Pass-boundary insertion-point table

Two host functions own essentially the whole GPU frame:
`MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp:1953`, called from
`code/gamecam.cpp:394` inside `GameCamera::render`), and the present block in
`RunGameOSLogic` (`GameOS/gameos/gameosmain.cpp:1581`). Postprocess passes live in
`gos_postprocess.cpp` and are invoked from within/after the renderLists flow. Each row =
one `GL_TIME_ELAPSED` begin/end scope, co-located with the existing Tracy zone braces
(grep the zone name to re-confirm line drift before coding).

| # | Pass (task name) | Insertion scope | File:line (zone anchor) |
|---|---|---|---|
| 1 | Shadow static full-map build | `Shadow.StaticFullMapBuild` | `mclib/txmmgr.cpp:2165` (+ inner `Shadow.StaticPrePass` `gameos_graphics.cpp:5434`, `Shadow.TessBatch` `:5480`, `Shadow.DynObjectDirect` `:5585` — keep as one outer scope) |
| 2 | Shadows: dynamic prop/caster pass | `RenderLists.DynamicShadowPass` | `mclib/txmmgr.cpp:2258` (inner draw `RenderLists.DynShadowDraw` `:2451`) |
| 3 | Mech batch (GPU mech draw) | `Render.GpuMechs` | `mclib/txmmgr.cpp:2719` |
| 4 | Legacy 3D objects (TG renderLists color) | `Render.3DObjects` | `mclib/txmmgr.cpp:2071` |
| 5 | Terrain solid — **chunk path (default-on)** | wrap `gos_TerrainLodChunk_SubmitDrawCommands` call | `mclib/terrain.cpp:2447` (impl `GameOS/gameos/gos_terrain_lod_chunk.cpp:443`); driver runs inside `Terrain::render` drawPass `mclib/terrain.cpp:2198` |
| 6 | Terrain solid — legacy indirect dispatch+draw | `Render.TerrainSolid` | `mclib/txmmgr.cpp:2467` |
| 7 | Terrain mask/overlays/mines/decals | one scope spanning `Render.TerrainMask.Solid`→`Render.Overlays` | `mclib/txmmgr.cpp:2733–2786` (sub-zones 2733/2746/2759/2771/2778/2785 stay Tracy-only) |
| 8 | Static prop depth prepass | `GpuSP.DepthPrepass` (`flushDepthPrepassV6`, env `MC2_STATIC_PROP_DEPTH_PREPASS`, default OFF) | `GameOS/gameos/gos_static_prop_batcher.cpp:4996` (call site `:6288`) |
| 9 | Static prop color (cull dispatch + batcher flush) | `Render.GpuStaticProps` | `mclib/txmmgr.cpp:2615` (inner `GpuSP.CullDispatch` `:2658`, `GpuSP.BatcherFlush` `:2708`, flush impl `gos_static_prop_batcher.cpp:5125`) |
| 10 | Water + terrain alpha | `RenderLists.TerrainAlphaWaterLoops` | `mclib/txmmgr.cpp:2800` (water fast-path compute already has its own spike timer, `gos_terrain_water_stream.cpp:1710`) |
| 11 | Shadow blobs | `RenderLists.ShadowBlobs` | `mclib/txmmgr.cpp:2936` |
| 12 | VFX billboard / Tube mesh / non-terrain alpha | `RenderLists.NonTerrainAlphaLoops` | `mclib/txmmgr.cpp:3006` (GPU particle bridge draws here; Tube/VFX mesh shares this bucket — split later only if it shows up) |
| 13 | VFX/HUD submit + UI | `RenderLists.VfxHudSubmit` | `mclib/txmmgr.cpp:3069` |
| 14 | Postprocess chain | one scope around HZB/SSAO/ScreenShadow/GodRays/Shoreline/PostProcess | `GameOS/gameos/gos_postprocess.cpp:1023/1486/1578/1656/1726/1769` (single outer scope at the chain entry; per-stage = phase 2) |
| 15 | Present / swap | `glQueryCounter` timestamps NOT TIME_ELAPSED (swap itself isn't GL work); record CPU present from existing `[HITCH_PHASE]` | `GameOS/gameos/gameosmain.cpp:1581` (`SwapWindow`), `:1589` PreFinish, `:1591` SDL swap |

Nesting rule: `GL_TIME_ELAPSED` queries **cannot nest** (one active query per target).
Rows above are chosen disjoint; inner Tracy zones (GpuSP.CullDispatch etc.) stay
Tracy-only. The existing gpu_cull_compute/water spike timers must be mutually exclusive
with the new gate OR converted to the shared pool (they currently begin/end their own
TIME_ELAPSED inside rows 9/10 — gate check: skip pool query if a spike query is active,
or simply make the new gate supersede them).

## (c) Telemetry format + env gate

Env gate: **`MC2_RENDER_PASS_TIME=1`** (default OFF, zero-cost when unset — getenv once,
cached bool, same as EditorGpuTimer `enabled()`). Optional `MC2_RENDER_PASS_TIME_EVERY=N`
(default 60): emit one aggregated line every N frames to keep logs small.

```
[RENDER_PASS_TIME v1] frame=12345 n=60 gpu_total=3.41 shadowStatic=0.02 shadowDyn=0.11 mechs=0.31 obj3d=0.27 terrainChunk=0.85 terrainSolid=0.00 overlays=0.04 spPrepass=0.00 spColor=0.62 water=0.18 blobs=0.01 alphaVfx=0.55 hud=0.12 post=0.33
```

- Values = **mean ms over the window** (matches `[SPFLUSH_COST_SPLIT]`/`[OBJECT_RECON]`
  house style: versioned tag, key=val, one line, stdout + fflush at emit only).
- A second optional line `... stat=max` for the window max (hitch attribution).
- Passes that did not run emit `0.00` (fixed key set → trivially diffable / awk-able).
- Keys are stable identifiers, never reordered (smoke scripts can regex them).

## (d) Query pooling / latency design (no stalls)

Pattern = EditorGpuTimer generalized from 2-deep ping-pong to an N-deep ring:

- **Ring of 4 frame-slots** (`kFramesInFlight=4`; 2 suffices for the editor's
  immediate-present loop, 4 is safe under driver triple-buffering + the smoke
  minimized-window throttle). Each slot = `GLuint q[kMaxPasses]` GL_TIME_ELAPSED query
  objects (kMaxPasses=16) + a pass-ran bitmask, `glGenQueries` lazily once.
- **Write side:** `RenderPassTimer_Begin(passId)` / `_End(passId)` around each row in (b);
  `RenderPassTimer_FrameEnd()` right before swap advances the ring cursor.
- **Read side:** at FrameEnd, check the **oldest** slot's *last-issued* query with
  `glGetQueryObjectiv(GL_QUERY_RESULT_AVAILABLE)` (exact pattern:
  `gpu_cull_compute.cpp:980-987` and `EditorGpuTimer.cpp:64-67`). Available → harvest all
  results into CPU accumulators, mark slot reusable. Not available → **skip, never block**
  (slot retried next frame; with 4 slots a result has ≥3 full frames to land).
- If the cursor catches up to an unharvested slot (pathological GPU backlog): drop that
  frame's measurements (count `dropped=` in the emit line) rather than stall.
- Aggregation: per-pass running sum + max + sample count; reset at each emit.
- Cost: ~14 query begin/ends + 1 availability poll per frame ≈ µs-scale; zero when gated off.
- GL_TIME_ELAPSED chosen over timestamp-chain (editor style) because passes here are NOT
  a strict linear chain (rows 5 vs 6 are alternates; row 8 conditional) — disjoint
  elapsed scopes tolerate reordering and absent passes; a mark-chain does not.
- Placement: new `GameOS/gameos/gos_render_pass_timer.{h,cpp}` (GameOS layer — txmmgr,
  gos_postprocess, gos_static_prop_batcher, gameosmain all link it; respects the
  no-raw-GL-from-game firewall since all call sites are mclib/GameOS).

## (e) Telemetry cockpit / mc2-render-state MCP integration

- **Phase 1 (stdout only):** `[RENDER_PASS_TIME v1]` lines land in the smoke mission log;
  `scripts/run_smoke.py --keep-logs` artifacts make them agent-greppable. No MCP change.
- **Phase 2 (debug-state dump):** add optional top-level object `renderPassTime` to
  `debug_state/latest_render_state.json` (`GameOS/gameos/debug_state_dump.cpp`):
  `{ "enabled": bool, "windowFrames": N, "meanMs": {pass: float...}, "maxMs": {...},
  "gpuTotalMs": float, "dropped": N }`. Additive ⇒ schema stays `MC2_DEBUG_STATE_V1`
  (docs/debug_state_schema.md:155 — additive fields don't bump). Validator
  `scripts/check-debug-state-json.py` treats it optional.
- **Phase 3 (MCP tool):** `mc2-render-state` server gains `get_render_pass_times` reading
  that object — agents can poll live per-pass GPU budgets every ~5s while the user plays,
  same workflow as `get_render_health`. Budget-regression check becomes a one-call oracle.
- Cockpit sketch: a tiny `scripts/render_budget_report.py` that tails the latest smoke
  log, parses `[RENDER_PASS_TIME v1]`, prints a budget table vs thresholds (e.g. terrain
  ≤1.0ms, spColor ≤1.0ms, post ≤0.8ms) — exit nonzero on breach → usable as a smoke
  side-gate and as the Baseline-A per-pass-timings capture mentioned in MEMORY.md.

## (f) Acceptance oracle

1. **Gate-off zero-cost:** unset env → zero GL query calls (assert via `MC2_GL_ERROR` drain
   clean + no `[RENDER_PASS_TIME]` lines), tier1 5/5 FPS within noise of baseline.
2. **Gate-on no-stall:** tier1 5/5 with `MC2_RENDER_PASS_TIME=1`: FPS delta < 2% vs OFF;
   `dropped=0` (or ≤ a handful at mission load) in all emit lines.
3. **Sum sanity:** per-window `gpu_total` ≥ Σ(pass means) is NOT required (gaps exist:
   un-scoped GL work), but Σ(pass means) ≤ gpu_total ± 10%, where `gpu_total` = a whole-
   frame bracketing TIME_ELAPSED disjoint trick is impossible (no nesting) → instead
   gpu_total = timestamp pair (`glQueryCounter` at frame start/end). Oracle: Σ(passes)
   ∈ [0.5×, 1.05×] of the timestamp total on every emitted window (catches scope leaks
   and double-began queries).
4. **Cross-check vs Tracy:** one interactive Tracy capture; per-pass means must agree with
   the corresponding `TracyGpuZone` means within ~15% (same scopes, different transport).
5. **GL cleanliness:** `GL_INVALID_OPERATION=0` over tier1 (catches nested TIME_ELAPSED —
   the most likely implementation bug, esp. interaction with the existing spike timers in
   `gpu_cull_compute.cpp:987` / `gos_terrain_water_stream.cpp:1710`).
6. **Smoke fixture:** add `[RENDER_PASS_TIME v1]` to the run_smoke whitelist (the
   `[OBJECT_RECON]` precedent) so gate-on runs don't trip log-noise checks.

## Known traps (carried from prior arcs)

- GPU timestamp only brackets *submitted* GPU commands; CPU-heavy passes look cheap
  (EditorGpuTimer.cpp header comment) — `[HITCH_PHASE]` stays the CPU complement.
- Smoke runs minimized → SDL throttle distorts frame totals but NOT per-pass GPU ms;
  per-pass numbers remain valid headlessly (R0 lesson, perf-attribution-plan.md:179).
- Verify deployed exe mtime before reading any telemetry (v0.4 vs 0.4c deploy-target trap).
- Re-grep all line numbers above before coding; txmmgr.cpp is high-churn.
