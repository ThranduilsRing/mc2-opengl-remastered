<!-- doc-status: current | reviewed:2026-10-11 -->
# Baseline A — post-8z frozen render/perf reference

**Stamp:** post-8z, pre-GlStateGuard, pre-visual-lanes
**Branch:** `claude/nifty-mendeleev`
**nifty HEAD:** `034c3176` (`docs(terrain): record 8z production closeout milestone`)
**Build captured:** `A:/Games/mc2-opengl/mc2-win64-0.4c` — verified terrain-closeout build (game `mc2.exe` mtime 2026-06-10 21:06, `Mission Editor.exe` 21:14). State dump reports `build.config=Release`, `build.commit=unknown` (commit string not stamped in this build).
**Captured:** 2026-06-10, headless, `mc2.exe --profile stock --mission <stem> --duration 30` with `MC2_SMOKE_MODE=1 MC2_FASTPATH_DROP_LOG=1 MC2_DEBUG_STATE_DUMP=1 MC2_DEBUG_STATE_DUMP_HISTORY=1`. Logs + per-mission state JSON archived at `.claude/baseline-A-logs/`.

This is the frozen reference that gates opening the modernization backlog. The game terrain renderer is **chunk/GPU-only** (8z closeout): `setupTextures` is editor-gated, `slimReduce` + `MC2_TERRAIN_ACTIVE_AB` are deleted. Re-run any future engine change against these numbers before merging.

---

## 1. Summary table (tier1, 30s headless each)

| Mission | Result | Frames | Avg FPS | FATAL | FASTPATH_DROP | Terrain path | Parity | slimVerts |
|---------|--------|--------|---------|-------|---------------|--------------|--------|-----------|
| mc2_01  | pass   | 3284   | 109.4   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_03  | pass   | 3825   | 127.4   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_10  | pass   | 3924   | 130.7   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_17  | pass   | 3948   | 131.5   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_24  | pass   | 3944   | 131.4   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |

FPS = `frames / 30.02s`. These are **idle fly-through** smoke runs (passive mode, no AI/weapon-fire load) — they are a renderer-floor reference, not an interactive-gameplay benchmark.

---

## 2. Golden frames (fixed-frame state captures)

State dumps every 300 frames; the frame-3000/3600 (`latest_render_state.json`) snapshot is the golden frame per mission, archived as `.claude/baseline-A-logs/<mission>.state.json`. Invariant fields, identical across all 5 missions:

- `schema=MC2_DEBUG_STATE_V1`, `renderSnapshot.ok=true`, `arenaOverflow=false`.
- **engineView:** `MainScene` Visual, viewport `[0,0,1920,1080]`, viewUniformsBinding 3.
- **registeredViews:** MainScene 1920×1080 · ShadowDirectional0-Static 4096×4096 · ShadowDynamic 4096×4096 (all valid).
- **renderPasses:** shadow=true, screenShadow=true, bloom=false, fxaa=false, tonemap=false.
- **feature gates:** `MC2_VIEW_UNIFORMS`, `MC2_SNAPSHOT_STATIC_PROP_BUILD`, `MC2_MATERIAL_GPU(+SAMPLE)`, `MC2_STATIC_PROP_IBL_SH`, `MC2_QUADSETUP_ARMED_SKIP`, `MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE` = ON; `MC2_STATIC_PROP_PBR_V1` = OFF.
- **staticPropOpaque:** `snapshotDispatchDefault=true`, `legacyDispatch=false`, shaderVariant `snapshot+materialGpu+sample+iblSh`, materialGpu table=34 / inventory=34, iblSh strength 0.5, pbr OFF (roughness override 0.95).
- **renderResources:** MainDepth Depth24 1920×1080; ShadowStaticMap + ShadowDynamicMap Depth24 4096×4096 (64 MB each); TerrainHeightTexture R32F 100×100; WaterReflection 480×270.

mc2_01 golden frame is reproduced in full at `.claude/baseline-A-logs/mc2_01.state.json` (frame=3000).

---

## 3. FASTPATH_DROP = 0 (terrain stays armed, no legacy fallback)

`[FASTPATH_DROP]` count = **0** in every mission's stdout+stderr. Confirmed armed-state signals per mission:

- `[TERRAIN_INDIRECT v1] event=first_arm path=gpu` — terrain came up on the GPU indirect path (nodeIds 19–109, atlasTex 99–258 per mission).
- `[QUADSETUP_SKIP v1] fullyArmed=1 skip=1 quadsSkipped=0` — legacy quad setup skipped, fully armed.
- `[TerrainLOD prod] … slimVerts=0` every dump — legacy `slimReduce` is gone (chunk producer is sole path), confirming the 8z deletion at runtime.
- `[TerrainLOD parity] sample[0..3] … MATCH` (4/4, 0 mismatch) — chunk world-pos output matches the legacy reference at the four probe cells.

No legacy terrain fallback occurred in any run.

---

## 4. Terrain / path snapshot + frame counts

| Mission | first_arm nodeIds | atlasTex | objBlocks (last) | objVerts | solidWindow | TerrainLOD flush cmds |
|---------|-------------------|----------|------------------|----------|-------------|----------------------|
| mc2_01  | 65  | 129 | 20 | 8000  | 8000  | 12–20 |
| mc2_03  | 35  | 132 | 24 | 9600  | 9600  | ~18   |
| mc2_10  | 19  | 99  | 31 | 12400 | 12400 | ~18   |
| mc2_17  | 38  | 107 | 25 | 10000 | 10000 | ~18   |
| mc2_24  | 109 | 258 | 24 | 9600  | 9600  | ~18   |

`objVerts == solidWindow` every dump (chunk producer writes both from the same active set; no stale window). Frame counts in §1.

---

## 5. Oracle counters (all clean — but see the RETRACTION note below)

> **RETRACTED IN PART (2026-07-30, lane FALSE-CONCLUSION-RETRACT-1).** The
> `[OBJBATCHER v1]` row of this section listed `gpu_drawn_instances=0`,
> `submit_legacy=0` and `submit_trees=0` as *clean oracle readings*. **Three of
> those zeros are structural, not observations** — they would read 0 whether the
> renderer was healthy or catastrophically broken, so they are not oracles and
> this section must not be cited as if they were. Detail in the note at the end
> of the `OBJBATCHER v1` bullet. The rest of §5 (RENDER_SNAPSHOT, TEX_RESOLVE,
> MECH_MATERIAL_GPU, GPU_CULL) is unaffected and stands.
>
> **This matters beyond this file:** `docs/perf-budget.md:26` points future
> baseline captures at this document as the pattern to copy, so the framing
> propagates forward unless corrected here.

Per-mission, end-of-run / last-dump values:

| Mission | RENDER_SNAPSHOT fallback | TEX_RESOLVE mismatches / oob | MECH_MATERIAL_GPU mismatches | OBJBATCHER cpu_fallback (rate) | OBJBATCHER submit_legacy |
|---------|--------------------------|------------------------------|------------------------------|--------------------------------|--------------------------|
| mc2_01  | 0 (using_snapshot=1) | 0 / 0 (213433 resolves) | 0 | 1456 (0.0005) | 0 |
| mc2_03  | 0 | 0 / 0 (133911) | 0 | 0 (0.0000) | 0 |
| mc2_10  | 0 | 0 / 0 (83538)  | 0 | 0 (0.0000) | 0 |
| mc2_17  | 0 | 0 / 0 (150029) | 0 | 0 (0.0000) | 0 |
| mc2_24  | 0 | 0 / 0 (429910) | 0 | 3941 (0.0007) | 0 |

- **RENDER_SNAPSHOT v3:** `fallback=0`, `count/pkt/meta_mismatch=0`, `using_snapshot=1` every 600-frame checkpoint. Static-prop snapshot path authoritative, no fallback.
- **TEX_RESOLVE v1:** `mismatches=0 oob=0` at shutdown, all missions.
- **MECH_MATERIAL_GPU v1:** `mismatches=0` every compare (mechs 12–84 per mission).
- **OBJBATCHER v1:** `gpu_drawn_instances=0`, `submit_legacy=0`, `submit_trees=0` everywhere. **`cpu_fallback` is non-zero on mc2_01 (1456) and mc2_24 (3941)** — but in both, `cpu_fallback == late_register_recovery_skips` and `fallback_rate ≤ 0.0007`. These are **late-register recovery skips** (e.g. the `compass` HUD node registering after the batch window), **not** GPU-cull CPU fallback of real geometry. Recorded honestly as a non-zero baseline value to watch, not a regression.

  > **RETRACTED (2026-07-30) — the three zeros above are not evidence of health.**
  > What this bullet claimed: that `gpu_drawn_instances=0`, `submit_legacy=0` and
  > `submit_trees=0` "everywhere" were clean oracle readings corroborating a
  > healthy renderer (reinforced by the section heading "Oracle counters (all
  > clean)" and by the §1/§5 table columns). **Why they cannot support that:**
  >
  > - **`submit_trees` / `submit_buildings` / `submit_generics` / `submit_legacy`**
  >   are `submitted_instances_by_pop[]`, incremented ONLY at
  >   `GameOS/gameos/gos_static_prop_batcher_submit.cpp:264`, on the **dynamic**
  >   `submitMultiShape` path. Once an actor registers a persistent recipe it
  >   replays every frame via `submitCachedInstance()` (`:9516`), which
  >   **deliberately does not tick them** (comment at `:9526`). In a steady-state
  >   scene essentially all static props are registry-replayed, so these read 0
  >   while the geometry draws every frame. `submit_trees=0` is the *expected*
  >   value and says nothing about whether trees drew. The batcher's own comment
  >   at `:9262` records the contradiction: "733 trees eligible, all via the
  >   store, dyn-submit=0". Reading `submit_legacy=0` as "the legacy path never
  >   activated" is the same error — 0 is what you get either way.
  > - **`gpu_drawn_instances`** is incremented only on the legacy/per-type path
  >   (`:8153`, from the CPU-side `TypeRangeSsbo.instanceCount`). The live
  >   GPU-authority coalesce-v6 path authors instance counts on the GPU and never
  >   reaches that site, so it reads 0 there. This is now declared in the source
  >   (STALE-COUNTER-RETIRE-1) and the summary line carries
  >   `gpu_drawn_note=legacy_path_only_v6_uses_submitted`.
  >
  > **Also wrong as a unit:** these counters increment **once per ACTOR**, not
  > once per instance, despite the name `submitted_instances` — child packets go
  > to the separate `submitted_children` counter. Any reasoning about instance
  > counts or geometry volume from them (here or downstream) is invalid.
  >
  > **What is still true:** the raw values recorded above were correctly
  > transcribed from the logs — they are accurate readings of the counters. Only
  > the inference "therefore clean" is withdrawn. The `cpu_fallback` /
  > `late_register_recovery_skips` analysis in this bullet is independent of the
  > three zeros and **stands unchanged**.
  >
  > **Replacement signal:** do not substitute `submitted_instances` (a prior
  > recon recommended this; it is per-actor and dynamic-path-only, so it has the
  > same blind spot). Use the population census
  > (`gos_static_prop_registry.h::getPopulationCensus()` /
  > `getPopulationsEverTagged()`), or pixel/RenderDoc draw attribution.
- **GPU_CULL v1:** substrate ready every frame (records 2.6k–2.9k / capacity 12295), `indirect_draw overflow=0`, `submit flush=600`. Clean shutdown (substrate/compute/readback).

---

## 6. Per-pass timings

Two layers, captured two ways. **Measurement source is marked on every number** — do not mix CPU cost-split with GPU pass time.

### 6a. CPU per-pass attribution — FILLED (smoke + RDTSC cost-split)

Source: **smoke cost-split**, env-gated RDTSC accumulators (`MC2_*_COST_SPLIT` / `MC2_*_SPLIT`), captured headless off 0.4c on mc2_01 (lightest) + mc2_24 (heaviest), 30s each, ~2000–2500 frames. Values are **per-frame average µs** (the `avg_us` field). `max_us` is discarded — it is load/first-frame hitch (100–160 ms), same artifact as `peak_ms`. Archive: `.claude/baseline-A-costsplit/`.

**CPU update split** (`MISSION_SPLIT v1`, the per-frame sim+build work, not GPU submit):

| Subsystem (avg µs/frame) | mc2_01 | mc2_24 |
|---|---|---|
| objmgr (terrain-object update) | 429 | 961 |
| land_update (terrain) | 511 | 357 |
| geometry (terrain geom phase) | 338 | 363 |
| interface (HUD/mission UI) | 259 | 369 |
| camera | 39 | 78 |
| pathmgr | 0.2 | 70 |
| **TOTAL update** | **1793** | **2610** |

**Terrain geom-phase breakdown** (`GEOM_PHASE_SPLIT v1`, avg µs/frame):

| Phase | mc2_01 | mc2_24 |
|---|---|---|
| chunkProd (chunk producer) | 67 | 72 |
| quadSetup | 151 | 152 |
| lightingPack | 24 | 28 |
| lightingCopy | 21 | 23 |
| indirectCompute | 20 | 24 |
| **slim** (legacy slimReduce) | **0.0** | **0.0** |
| total | 302 | 322 |

`slim=0.0 µs` is the **runtime confirmation** that `slimReduce` is retired (corroborates §4 `slimVerts=0`).

**HUD split** (`MIF_SPLIT v1`, avg µs/frame): mc2_01 TOTAL 485 (invProj **399**, updateTarget 53, controlGui 18); mc2_24 TOTAL 369 (invProj **205**, updateTarget 120). `invProj` (cursor-pick `inverseProject`) is the dominant HUD cost — the known pick hotspot, recorded here as baseline, not a new finding.

**Static-prop flush** (`SPFLUSH_COST_SPLIT v1`, cached path): mc2_01 total ≈9.5 µs, mc2_24 ≈3.5 µs over 10 frames — negligible (snapshot cache hit, no per-frame rebuild).

### 6b. GPU per-pass times — STILL OPEN (precisely scoped)

Source available headless: **GL `TIME_ELAPSED` query** — but the engine only wraps **two** passes in a GL timer: `gpu_cull_compute` and `gos_terrain_water_stream`. There is **no** GL timer around terrain-solid, shadow, 3D-objects, post-process, or present.

| GPU pass | source | mc2_01 | mc2_24 |
|---|---|---|---|
| GPU-cull indirect submit | `GPU_CULL indirect_draw elapsed_us` | 11–13 | 10–11 |
| Water stream | `TIME_ELAPSED` | (no water in mission) | (no water in mission) |
| Terrain solid / shadow / 3D / post / present | — | **not instrumented** | **not instrumented** |

**Why still open, and the fix:** this is NOT a headless dead-end (smokes already pull the two timers that exist). The gap is missing instrumentation — there is no per-pass GL `TIME_ELAPSED` wrapper around the main passes. Closing 6b = a small, bounded code lane: add a coarse per-pass GL timer (one `glBeginQuery(GL_TIME_ELAPSED)` per pass, summary-emit like `GPU_CULL`) and re-run the cost-split smoke. Until then, GPU per-pass cost is inferred only from whole-frame `p50_ms` (6.6 µs… 6.9 ms, §1) minus the CPU update TOTAL above. A live Tracy/RGP GUI session remains the alternative for an interactive snapshot, but the per-pass GL timer is the *repeatable, smoke-capturable* path and is preferred.

---

## Gate

Baseline A captured. Tier1 5/5 pass, FATAL=0, FASTPATH_DROP=0, all correctness oracles clean (terrain chunk-only + armed, parity MATCH), OBJBATCHER late-register skips recorded as known non-zero. **(2026-07-30: "all correctness oracles clean" is narrowed — the three OBJBATCHER zeros counted here are structural, not oracles; see the RETRACTED note in §5. The terrain/parity/snapshot/TEX_RESOLVE oracles are unaffected, so the gate verdict itself is unchanged.)** **CPU per-pass attribution FILLED** (§6a, cost-split). **GPU per-pass times remain OPEN** (§6b) — scoped to a small per-pass GL-timer instrumentation lane, not a headless limitation. With that caveat noted, this freezes the post-8z reference; the modernization backlog (GlStateGuard, visual lanes, Tube merge) may open against it. GlStateGuard slice 1 can use §6a CPU attribution + the whole-frame `p99_ms` budget as its before/after comparator today; 6b sharpens it.
