# renderWater Stage 3 — Parity Check Infrastructure (Fresh-Session Handoff)

> **Role for fresh session:** Stage 1 + Stage 2 of the renderWater architectural slice
> are SHIPPED. Visual parity with the legacy water flush has been confirmed on tier1
> stock missions (smooth shoreline matching legacy, mc2_01 interactive). Your job is
> Stage 3: implement `MC2_RENDER_WATER_PARITY_CHECK=1` byte-compare instrumentation,
> run it on tier1, and close out the slice.

## TL;DR for the new session

Read 5 things in order, then start:

1. `memory/renderwater_fastpath_stage2.md` — what shipped, including the depth-state
   fix that closed out the shoreline staircase 2026-04-30
2. `memory/gpu_direct_renderer_bringup_checklist.md` — 9 traps every GPU-direct path
   hits (item #9 added 2026-04-30)
3. `docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md` — full spec.
   Stage 3 details are at lines 117-122 (env gate spec), 125-177 (legacy
   `addVertices` byte layout), 187-197 (validation gates A/A'/B/C/D)
4. `memory/feedback_offload_scope_stock_only.md` — parity gates validate against
   tier1 stock only; Carver5O / Magic / MCO / Wolfman / MC2X are out of scope
5. `memory/feedback_smoke_duration.md` — pass `--duration 20` to run_smoke.py

Then run a quick sanity check:
```
py -3 scripts/run_smoke.py --tier tier1 --duration 20 --kill-existing
```
This should already PASS 5/5 with the Stage 2 fix landed. If it doesn't, fix
that before starting Stage 3.

## What "Stage 3" actually means

Add an env-gated parity-check pass that:

1. **Captures** what the legacy `mcTextureManager->addVertices(waterHandle, gVertex,
   ...)` and `addVertices(waterDetailHandle, sVertex, ...)` arguments *would have
   been* if the legacy emit ran for each in-window water-bearing quad this frame.
2. **Computes** what the fast path's recipe + per-frame thin record + per-vertex
   shader inputs *would resolve to* if you projected them through the equivalent
   CPU arithmetic.
3. **Byte-compares** the two on field granularity.
4. **Prints** `[WATER_PARITY v1] event=mismatch frame=N quad=K layer=base|detail
   vert=V field=<name> legacy=<bytes> fast=<bytes>` on any divergence.
5. **Stays silent** on success (matches `m2_thin_record_cpu_reduction_results.md`
   convention).

The check runs both code paths on the same frame, with the same seed/state, on the
same CPU side. It does NOT rasterize anything for the parity check — it stays in
the world of input bytes (the `gos_VERTEX` struct contents), not pixels.

**Critical scope:** parity is on the **inputs** (SSBO / `addVertices` args), not
on rasterized outputs. Per the spec line 122-123: "Do NOT byte-compare outputs
(post-VS rendered vertices); water UVs are world-derived with per-quad integer
wrapping (per `water_rendering_architecture.md`) and CPU vs GPU floating-point
will drift below 1 ULP, producing fake mismatches."

## The legacy byte layout you're matching

From the spec (lines 125-177), each `gos_VERTEX` is 32 bytes:

| Field | Offset | Type | Source (legacy → fast-path equivalent) |
|---:|---:|---|---|
| `x` | 0 | float | projected from `(vx, vy, waterElevation + waveDisp)` via `terrainMVP` |
| `y` | 4 | float | same projection |
| `z` | 8 | float | projected z + `TERRAIN_DEPTH_FUDGE` (0.001) |
| `rhw` | 12 | float | projected w |
| `u` | 16 | float | base: `(vx - mapTopLeft3d.x) * oneOverTF + cloudOffsetX` (post-`MaxMinUV` wrap) |
| `v` | 20 | float | base: `(mapTopLeft3d.y - vy) * oneOverTF + cloudOffsetY` (post-wrap) |
| `argb` | 24 | DWORD | base: `(lightRGB & 0x00ffffff) + alphaMode_i` from per-vertex elev band; detail: `(argb & 0xff000000) + 0xffffff` |
| `frgb` | 28 | DWORD | `(fogRGB & 0xFFFFFF00) | terrainTypeToMaterial(terrainType)` |

Per-quad triangle layout depends on uvMode (spec lines 142-156).

Wave displacement (spec lines 160-166): `water & 0x80` → `ourCos = -frameCos`,
`water & 0x40` → `ourCos = +frameCos`, neither → `+frameCos` default.
`vertex3D.z = ourCos + waterElevation` is what gets projected.

## Implementation shape (suggested — feel free to adjust)

### Where to put the capture

Two ways to source the legacy bytes:

**A. CPU-side recompute (preferred — no engine modification needed):**
For each thin record the fast path emits this frame, walk the same per-vertex
math the legacy `drawWater()` did and synthesize the `gos_VERTEX` bytes the
legacy WOULD have produced for that quad. This is purely arithmetic on
`MapData::blocks[]` + per-frame uniforms (`cloudOffsetX/Y`, `frameCos`,
`mapTopLeft3d`, `oneOverTF`, etc.). The legacy formulas are all in
`mclib/quad.cpp:2773..3343` — copy them verbatim into a parity-check helper.

The reason this works: the legacy `gVertex` contents are pure functions of
`MapData::blocks[i]` + per-frame globals + `vertices[i]->lightRGB/fogRGB`. Both
the CPU recompute and the legacy hand-rolled emit produce identical bytes (the
fast path's job is to produce identical *shader inputs* — the SSBO contents and
per-vertex VS computation must reach the same byte values).

**B. Engine instrumentation (only if A is insufficient):**
Add a hook in `mcTextureManager::addVertices` that, when `MC2_RENDER_WATER_PARITY_CHECK=1`
is set AND the flags include `MC2_ISWATER`, captures the `gos_VERTEX[3]` bytes
into a per-frame ring buffer. Diff against the fast path's equivalent recipe-driven
synthesis at end of frame. Has the advantage of being a true ground-truth capture.
Disadvantage: requires running both paths simultaneously, which means temporarily
NOT early-returning out of `Terrain::renderWater()` when `_FASTPATH=1` is set.

I'd start with A and only fall back to B if you find an unmatched field.

### Where to invoke the check

Option 1: at end of `Terrain::renderWaterFastPath()` (after the GPU draw is
queued). Walk the thin records, synthesize legacy bytes for each, compare
against the equivalent fast-path recipe-derived bytes.

Option 2: at end of frame in a separate pass driven by env-gate.

Option 1 keeps the parity check colocated with the fast path and runs at the
right time (after thin records are populated for the frame). Recommended.

### What to emit

Silent on pass. On mismatch:
```
[WATER_PARITY v1] event=mismatch frame=12345 quad=7 layer=base vert=0 field=u legacy_bytes=0x40c00000 fast_bytes=0x40c00001
```

Counter at end of frame (always-on summary every 600 frames or on shutdown,
matching the `[TGL_POOL v1]` convention from
`memory/debug_instrumentation_rule.md`):
```
[WATER_PARITY v1] event=summary frames=N total_mismatches=K
```

### Capacity / cost

mc2_01 has ~1300 in-window water quads per frame, each emitting 2 base tris (6
verts) + 2 detail tris (6 verts) = 12 `gos_VERTEX` per quad. So ~15K
gos_VERTEX × 32 bytes = ~500 KB of comparison work per frame. Per-frame is
fine — the env-gated path runs in dev only. No need to ring-buffer beyond the
current frame.

## Validation gates (from spec)

You're closing out **gate C** (zero mismatches across stock-content tier1).
Gates A/A'/B/D you can skim:

- **A. Visual canary** — already passed at Stage 2 (mc2_01 shore matches
  legacy). Re-run as sanity check.
- **A'. Screenshot diff** — `scripts/water_visual_diff.py mc2_01` produces
  side-by-sides; expect bit-near-identical except for per-frame UV scroll.
- **B. Tracy delta** — target ≥ 50% reduction on `Terrain::renderWater` zone.
  Recon baseline 570-620 µs; expectation sub-100 µs.
- **C. `MC2_RENDER_WATER_PARITY_CHECK=1`** — zero mismatches across **stock
  tier1 only** (5 missions: mc2_01, mc2_03, mc2_10, mc2_17, mc2_24).
- **D. tier1 5/5 PASS** with both `_FASTPATH=1` and unset, +0 destroys delta.

## Definition of done

- `MC2_RENDER_WATER_PARITY_CHECK=1` env gate added to engine + plumbed in
  `scripts/run_smoke.py:232-247` propagation list (verify the
  `[INSTR v1] enabled: ...` startup banner picks it up after the implementation
  lands)
- Tier1 5/5 PASS with both `_FASTPATH=1` and `_FASTPATH=0`
- Tier1 5/5 PASS with `_FASTPATH=1` AND `_PARITY_CHECK=1` together — zero
  `[WATER_PARITY v1] event=mismatch` lines emitted
- Tracy delta gate B verified (≥ 50% reduction on `Terrain::renderWater` zone)
- New memory file: `water_ssbo_pattern.md` capturing the static-SSBO + single-
  draw + thin-record template the indirect-terrain endpoint will reuse
- Update `renderwater_fastpath_stage2.md` "Validation" section with parity
  results
- Update `gpu_direct_renderer_bringup_checklist.md` if you discover a 10th trap
- Update orchestrator `docs/superpowers/cpu-to-gpu-offload-orchestrator.md`:
  promote Stage 2 from "Shipped" to "Stage 3 closed; renderWater slice
  complete"

## Deferred cleanup (from spec line 245-248)

- Fix `MC2_WATER_DEBUG` frame-0 off-by-one (cosmetic)
- Demote `MC2_WATER_DEBUG` printer to silent post-fix per debug-instrumentation
  rule (`memory/debug_instrumentation_rule.md`)

These are nice-to-have during Stage 3; not required.

## What NOT to do

- **Don't byte-compare rasterized output.** CPU vs GPU floating-point drift
  will produce fake mismatches. The spec is explicit at line 122-123.
- **Don't validate against non-stock content.** Carver5O, Magic, MCO, Wolfman,
  MC2X are out of scope per `feedback_offload_scope_stock_only.md`. If parity
  fails on those, that's the mod's problem, not ours.
- **Don't try to parity-check the post-shadow pass.** GBuffer1.a writes are
  identical between legacy and fastpath (water FS is shared); not a meaningful
  signal.
- **Don't re-architect.** Stages 1-2 are shipped and visually correct. Stage 3
  is verification, not redesign. If you discover a parity bug, fix the bug
  surgically in the existing fast-path code; don't rebuild the architecture.
- **Don't forget the depth-state fix.** Item #9 in the bring-up checklist
  (`gpu_direct_depth_state_inheritance.md`). Skipping `glEnable(GL_DEPTH_TEST)`
  + `glDepthFunc(GL_LEQUAL)` reproduces the shoreline staircase. The current
  bridge has it; don't accidentally remove it during cleanup.

## Files involved (Stage 2 — already shipped, study but don't break)

- `GameOS/gameos/gos_terrain_water_stream.{h,cpp}` — recipe + thin SSBO
  management
- `shaders/gos_terrain_water_fast.vert` — paired with `gos_tex_vertex.frag`
- `mclib/terrain.{h,cpp}` — `renderWater()` early-return + `renderWaterFastPath()`
- `code/gamecam.cpp` — calls `land->renderWaterFastPath()` after `renderLists()`
- `mclib/mapdata.h` — `getBlocks()` accessor (Stage 2 added)
- `GameOS/gameos/gameos_graphics.cpp` — bridge function
  (`renderWaterFastPath` C++ method; with depth-state save/restore as of
  2026-04-30 fix)

## Files you'll likely add (Stage 3)

- `GameOS/gameos/gos_water_parity_check.{h,cpp}` (suggested, or fold into
  `gos_terrain_water_stream.cpp`) — capture + diff helpers, env-gated
- Whatever instrumentation site option you pick (A or B above)

## Memory files to read

⭐ Load-bearing:
- `gpu_direct_renderer_bringup_checklist.md` (9 traps incl. depth-state)
- `gpu_direct_depth_state_inheritance.md` (NEW 2026-04-30)
- `renderwater_fastpath_stage2.md` (what shipped + how the shoreline bug was
  closed)
- `feedback_offload_scope_stock_only.md` (parity scope = stock tier1 only)

Also relevant:
- `m2_thin_record_cpu_reduction_results.md` — parallel slice with parity-check
  pattern already shipped; mirror its `[PATCH_STREAM v1] event=thin_record_parity`
  conventions
- `debug_instrumentation_rule.md` — 600-frame summary cadence, env-gating
  conventions
- `feedback_smoke_duration.md` — `--duration 20` for smoke
- `quadlist_is_camera_windowed.md` — your parity check walks live `quadList`
- `clip_w_sign_trap.md` + `terrain_tes_projection.md` — projection chain math
  (the parity check needs to reproduce the legacy projection arithmetic
  exactly)

## Reproduce the fast path (sanity check before starting)

```
set MC2_RENDER_WATER_FASTPATH=1& set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1& set MC2_MODERN_TERRAIN_PATCHES=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
```

Start mc2_01. The shoreline should look smooth and effortlessly match the
underlying terrain — no tile-aligned blocks. If you see staircase, something
regressed in the depth-state setup; fix that before starting Stage 3.

## Build / deploy

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo --target mc2
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
```

(Or use `/mc2-build-deploy` skill once the worktree is loaded.)

## Smoke

```
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --duration 20 --kill-existing
```

For the parity-check run, add `MC2_RENDER_WATER_PARITY_CHECK=1` to the env-var
propagation list in `run_smoke.py:232-247` then re-run.

## What success looks like

- One commit: "feat(water-stage3): add MC2_RENDER_WATER_PARITY_CHECK byte-diff
  instrumentation"
- One commit: "docs(water): close out renderWater architectural slice — Stage
  3 shipped" (memory updates, orchestrator promotion, water_ssbo_pattern.md)
- Tier1 5/5 PASS triple: unset / `_FASTPATH=1` / `_FASTPATH=1 + _PARITY_CHECK=1`
- Tracy zone `Terrain::renderWater` showing < 100 µs (recon baseline 570-620 µs)
- Zero `[WATER_PARITY v1] event=mismatch` lines in tier1 logs

When you hit all of those, the renderWater architectural slice is fully shipped
and the indirect-terrain follow-up can build on the now-proven
`water_ssbo_pattern.md`.
