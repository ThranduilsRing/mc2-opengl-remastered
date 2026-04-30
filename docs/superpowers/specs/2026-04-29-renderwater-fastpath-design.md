# renderWater Architectural Slice — Static SSBO + Single Draw

**Status:** design confirmed (recon shipped, design overseer-confirmed 2026-04-30, code pending)
**Owner:** session 2026-04-29 → 2026-04-30 (renderWater pivot)
**Predecessor:** quadSetupTextures arc concluded asymptotic; pivoted to renderWater per orchestrator (`docs/superpowers/cpu-to-gpu-offload-orchestrator.md`).

## Goal

Eliminate the unconditional 40K-quad CPU loop in `Terrain::renderWater` (`mclib/terrain.cpp:975-988` legacy form). Replace with a static SSBO of water-bearing quads built once at terrain load and a single instanced draw per frame.

## Recon (already captured)

mc2_01, default smoke camera, full thin-record env stack, Wolfman mode (`visibleVerticesPerSide = 200`):

| Frame | total quads | handle_valid (work) | detail_eligible | renderWater µs |
|---:|---:|---:|---:|---:|
| 0 | 0 (off-by-one)¹ | 0 | 0 | 570.5 |
| 1 | 39601 | 1314 | 1314 | 620.7 |
| 2 | 39601 | 1324 | 1324 | 602.5 |
| 3 | 39601 | 1322 | 1322 | 564.0 |
| 4 | 39601 | 1324 | 1324 | 577.3 |

¹ Cosmetic latch in `MC2_WATER_DEBUG` collect flag; the 570 µs reading is clean (no count overhead) and confirms the loop pays ~570 µs even when not collecting.

**Conclusions:**
1. **96.7% of quads pure-skip** inside `drawWater()` (`waterHandle == 0xffffffff`, set in `setupTextures` `quad.cpp:963-989` only when `clipped1||clipped2`).
2. **Detail eligibility = base eligibility** on `terrainTextures2`-bearing maps (every shipping map). Single SSBO record can drive both layers.
3. **Classification is mission-stable.** `Terrain::waterElevation` loaded once at `terrain.cpp:1453`; per-vertex elevation never mutates in normal play. Per-frame variance (±10 quads) is camera-driven frustum drift, not classification mutation.
4. **Per-frame globals drive animation:** `cloudScrollX/Y` (UV scroll), `sprayFrame` (detail texture handle slot index — already animates per-frame, must remain a uniform). Per-vertex `lightRGB`/`fogRGB` mutate (lighting). Per-vertex elevation does not.

## Architecture

```
Static (built once at primeMissionTerrainCache):
└── WaterRecipeSSBO[N]     where N = water-bearing quad count (typically ~30k on water maps)
    per-record (~48 B):
      vec2  v0_xy, v1_xy, v2_xy, v3_xy   // raw world (vx, vy) per quad-corner
      float v0_elev, v1_elev, v2_elev, v3_elev
      uint  uvMode                       // BOTTOMRIGHT vs BOTTOMLEFT
      uint  flags                        // packed: hasDetail, etc.

Per-frame uniform block:
└── WaterFrame {
      mat4  terrainMVP;       // shared with M2 path
      vec4  cloudOffset;      // (cloudOffsetX, cloudOffsetY, sprayOffsetX, sprayOffsetY)
      float waterElevation;
      float alphaDepth;
      float oneOverWaterTF, oneOverTF;
      uint  alphaEdge, alphaMiddle, alphaDeep;
      uint  waterTexSlot, waterDetailTexSlot;   // resolved at draw time per mc2_texture_handle_is_live.md
      // lightRGB/fogRGB sampling: TBD — see "Light data" below
    }

Per-frame thin record (small, ring-buffered):
└── WaterFrameSSBO[M]   M = currently-water-emitting quad indices
    per-record (~16 B):
      uint  recipeIdx
      uint  lightRGB0..3 (packed)        // per-vertex lighting for this frame
      uint  fogRGB0..3 (packed)

Draw: single glDrawElementsInstanced
  - VS reads recipe + frame record
  - VS computes UV, alpha-band per-vertex (deep/middle/edge from elevation+waterElevation+alphaDepth)
  - FS = existing gos_tex_vertex.frag (pixel-stable)
```

### Light data — confirmed parallel SSBO

Per-vertex `lightRGB` is recomputed per-frame in `projectVertices`. **Decision (overseer-confirmed 2026-04-30):** stream a thin per-frame water record carrying `lightRGB0..3` and `fogRGB0..3` per emitting quad (~24 KB/frame on mc2_01).

Co-indexing into the M2 thin-record SSBO is wrong on two axes:
- **Schema** — different shader, different fields; one would padding-pad for the other's missing data.
- **Population** — water-bearing quads are not a subset of M2-emitting quads. Many M2 quads have terrainHandle==0 and skip via M2b loop hoist; many water quads similarly. Both directions of disjoint membership exist.

24 KB/frame is bandwidth nothing. Water lives in its own SSBO permanently — even at the indirect-draw architectural endpoint the right answer is "one SSBO per pass type," not "one big SSBO with a discriminator."

### Classification site

`Terrain::primeMissionTerrainCache` (terrain.cpp:563-578) is the build hook. After `mapData->buildTerrainFaceCache` and `warmTerrainFaceCacheResidency` complete, walk `quadList[0..numberQuads-1]` and compute, **independent of frustum**:

- For each quad, derive whether it would receive a non-zero `waterHandle` if the map data + elevation gates fired. The current logic in `quad.cpp:963-989` is gated on `clipped1||clipped2` (frustum) — but the *underlying* trigger is the per-vertex elevation comparison vs `waterElevation` and the texture-slot lookup `Terrain::terrainTextures->getTextureHandle(MapData::WaterTXMData & 0x0000ffff)` / `terrainTextures2->getWaterTextureHandle()`. These are mission-static.
- Concretely: a quad is water-bearing iff at least one vertex has `elevation < waterElevation - alphaDepth` OR (more conservative) `elevation < waterElevation` — pick the inclusive form to match the runtime alpha-band test. Verify against the runtime `alphaMode != 0` outer gate.

Build the recipe SSBO with N entries. Persist for mission lifetime; reset on mission load.

#### Verified mutation invariant (2026-04-30 grep)

Water classification is mission-immutable in normal play. Verified write-site survey:

| Symbol | Write sites | Gameplay-reachable? |
|---|---|---|
| `Terrain::waterElevation` | `terrain.cpp:1453` (FIT load), `mapdata.cpp:602` (`calcWater` body) | **Load-only.** `calcWater` is invoked from `Terrain::calcWater` (terrain.cpp:864) at terrain load. No live-game write path. |
| `MapData::WaterTXMData` | `mapdata.cpp:557` (`assignTerrainTextures`) | **Load-only.** Computed during initial texture assignment. |
| `MapData::recalcWater()` | Two callsites: [mission.cpp:2221](../../../code/mission.cpp:2221) (**commented out** — `//Should have already been done in the editor`), [missiongui.cpp:256/2737](../../../code/missiongui.cpp:256) (**CTRL+ALT+W editor shortcut only**) | **No.** Editor-only path; not reachable in shipped gameplay. |
| Per-quad `waterHandle` / `waterDetailHandle` | `quad.cpp:517/590/603/968/973/982/988` (all inside `setupTextures`) | Per-frame read/reset for frustum gate; underlying classification (would-emit-if-in-frustum) derives from immutable map data. |

**Implication:** the static SSBO needs zero invalidation hooks for gameplay. Editor-mode recalc is out of scope (parked TBD if/when editor returns to scope). Even `Terrain::recalcWater` (terrain.cpp:1678-1680) only re-flags per-vertex `water` bits — it does NOT re-set `Terrain::waterElevation`.

### Draw site

In the new `Terrain::renderWaterFastPath()`:
1. Update WaterFrame uniform block (cloudOffset, sprayFrame texture handle, etc.).
2. Walk emitting subset → write per-frame light records into ring SSBO. Initial implementation can walk all N recipe entries and let GPU clip — skip the CPU emit subset entirely; MS overhead from drawing extra GPU-clipped quads is negligible at N=~30k.
3. `glBindVertexArray(emptyVAO); glUseProgram(waterFastPathVS); glDrawArraysInstanced(GL_TRIANGLES, 0, 6, N);` (6 verts = 2 triangles unrolled in VS, instance = water quad).
4. Feed the emit through the existing `gos_tex_vertex.frag` pixel path (pixel-stable visuals).

#### Projection chain — load-bearing pre-implementation read

The water VS's clip-space output **must use the same projection chain as the M2 thin-record terrain VS** or clip-cull becomes the bug, not a feature. Read before writing the VS:

- `memory/terrain_mvp_gl_false.md` — `terrainMVP` direct upload uses **GL_FALSE** (row-major; the comment in `gamecam.cpp` saying GL_TRUE is wrong; GL_FALSE + row-major cancels to the right math).
- `memory/terrain_tes_projection.md` — D3D pixel-homogeneous chain, `abs(clip.w)` is load-bearing in TES; pz gate in quad.cpp required.
- `memory/clip_w_sign_trap.md` — never `sign(clip.w)`; use `pz ∈ [0,1)` from `projectZ`.

The shared uniform `projection_` (screen→NDC) is `GL_TRUE` in the thin VS path (M2/M2d shipping convention, see `gameos_graphics.cpp:3028`). Mirror that exactly. The water VS computes clip-space from **raw world position via `terrainMVP`** (matching how `gVertex[].x/y/z` are derived in the legacy path) — not from the post-`projectZ` window-space `(wx, wy, wz, ww)`. The two are equivalent under the canonical chain; the SSBO should carry the raw `(vx, vy, elevation)` form so the VS owns the full math.

### Env gates (mirror M2/M2d ladder)

- `MC2_RENDER_WATER_FASTPATH=1` — default off. Routes `renderWater()` to the new path; legacy 40K-loop stays intact when off.
- `MC2_RENDER_WATER_PARITY_CHECK=1` — captures legacy `mcTextureManager->addVertices` argument streams and byte-compares against the fast-path SSBO record contents. Print `[WATER_PARITY v1] event=mismatch frame=N quad=K layer=base|detail field=<name>` on divergence. Silent on success (per the M2 `[PATCH_STREAM v1] event=thin_record_parity` convention in `m2_thin_record_cpu_reduction_results.md`).
- Both already added to `scripts/run_smoke.py:232-247` propagation list this session — verify the `[INSTR v1] enabled: ...` startup banner picks them up after the implementation lands.

**Parity scope (overseer-confirmed 2026-04-30):** byte-compare on **inputs** (SSBO contents — what the legacy `addVertices` args would have been vs what the fast path puts in the SSBO). Do NOT byte-compare outputs (post-VS rendered vertices); water UVs are world-derived with per-quad integer wrapping (per `water_rendering_architecture.md`) and CPU vs GPU floating-point will drift below 1 ULP, producing fake mismatches.

#### Legacy `addVertices(waterHandle, gVertex, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER)` byte layout

For each emitting quad in `TerrainQuad::drawWater()` (one of the four uvMode/triangle blocks at `quad.cpp:2773..3343`), the legacy path writes **3 × `gos_VERTEX` per call × 2 calls (top-tri + bottom-tri) = 6 base-water vertices per quad**, plus **3 × 2 = 6 detail-spray vertices per quad** when `useWaterInterestTexture && waterDetailHandle != 0xffffffff`.

Each `gos_VERTEX` is 32 bytes:

| Field | Offset | Type | Source (legacy → fast-path equivalent) |
|---:|---:|---|---|
| `x` | 0 | float | `vertices[i]->wx` → projected from `(vx, vy, waterElevation + waveDisp(waterBits, frameCos))` via `terrainMVP` |
| `y` | 4 | float | `vertices[i]->wy` → same projection |
| `z` | 8 | float | `vertices[i]->wz + TERRAIN_DEPTH_FUDGE` → projected z + fudge constant |
| `rhw` | 12 | float | `vertices[i]->ww` → projected w |
| `u` | 16 | float | base layer: `(vertices[i]->vx - mapTopLeft3d.x) * oneOverTF + cloudOffsetX` (per quad.cpp:2803). detail layer: same formula with `oneOverWaterTF` and `sprayOffsetX`. Followed by the `MaxMinUV` floor-shift correction at quad.cpp:2863-2884. |
| `v` | 20 | float | base: `(mapTopLeft3d.y - vertices[i]->vy) * oneOverTF + cloudOffsetY`. detail: same with `oneOverWaterTF` + `sprayOffsetY`. Same `MaxMinUV` correction. |
| `argb` | 24 | DWORD | base: `(vertices[i]->lightRGB & 0x00ffffff) + alphaMode_i` where `alphaMode_i ∈ {alphaEdge, alphaMiddle, alphaDeep}` from per-vertex elevation-vs-`waterElevation±alphaDepth` test (quad.cpp:2829-2856). detail: `(sVertex[i].argb & 0xff000000) + 0xffffff` (preserves alpha, sets RGB to white). |
| `frgb` | 28 | DWORD | `(vertices[i]->fogRGB & 0xFFFFFF00) | terrainTypeToMaterial(vertices[i]->pVertex->terrainType)` |

**Per-quad triangle layout depends on `uvMode`:**

| uvMode | Top tri verts | Bottom tri verts |
|---|---|---|
| `BOTTOMRIGHT` (0) | `vertices[0,1,2]` | `vertices[0,2,3]` |
| `BOTTOMLEFT` (1) | `vertices[0,1,3]` | `vertices[1,2,3]` |

UV ranges per corner (matching `gVertex.u`/`v` initial values before the world-derived overwrite):

| uvMode | tri | corner 0 | corner 1 | corner 2 |
|---|---|---|---|---|
| BOTTOMRIGHT top | (minU,minV) | (maxU,minV) | (maxU,maxV) | |
| BOTTOMRIGHT bot | (minU,minV) | (maxU,maxV) | (minU,maxV) | |
| BOTTOMLEFT top | (minU,minV) | (maxU,minV) | (minU,maxV) | |
| BOTTOMLEFT bot | (maxU,minV) | (maxU,maxV) | (minU,maxV) | |

(But note: those initial UV values are **immediately overwritten** at quad.cpp:2803-2810 with the world-derived form; the only place `minU/maxU/minV/maxV` survive is in the very first uv assignment block before the world-derived overwrite. For parity purposes, only the world-derived UVs matter.)

**Wave displacement (`waveDisp` in the table above):** the legacy path applies this in `setupTextures` water-projection block at quad.cpp:689-700:
- `vertices[i]->water & 0x80` set → `wAlpha = -frameCosAlpha`, `ourCos = -frameCos`
- `vertices[i]->water & 0x40` set → `wAlpha = +frameCosAlpha`, `ourCos = +frameCos`
- neither → `ourCos = +frameCos` (default), `wAlpha` not modulated
- Then `vertex3D.z = ourCos + waterElevation` is what gets projected.

The fast-path VS reads `waterBits` from the recipe, picks the per-vertex modulator, and applies the same `z = ourCos + waterElevation` before transforming via `terrainMVP`.

**Per-frame uniforms the parity check needs the fast path to write into the SSBO record (or apply via uniform):**
- `cloudOffsetX/Y` (= `cos/sin(360 * π/180 * 32 * cloudScrollX/Y) * 0.1`) — quad.cpp:2726-2727
- `sprayOffsetX/Y` (= `cloudScrollX/Y * 10.0f`) — quad.cpp:2729-2730
- `oneOverTF`, `oneOverWaterTF` (functions of `terrainTextures2->getWaterTextureTilingFactor() / worldUnitsMapSide` and water-detail equivalent) — quad.cpp:2738-2739
- `frameCos`, `frameCosAlpha` — `Terrain` statics, refreshed per-frame
- `waterElevation`, `alphaDepth`, `alphaEdge`, `alphaMiddle`, `alphaDeep` — mission-stable but global uniforms
- `mapTopLeft3d.x`, `.y` — mission-stable
- `terrainMVP`, `projection_` — same as M2 thin-record path

Stage 3 parity implementation: capture the legacy `addVertices` args under `MC2_RENDER_WATER_PARITY_CHECK=1`, project each fast-path recipe through the equivalent CPU arithmetic on the same per-frame state, byte-compare. Any field-level mismatch prints a `[WATER_PARITY v1] event=mismatch quad=K layer=base|detail vert=V field=<x|y|...>` line.

## Constraints (load-bearing)

1. **Pixel-stable water visuals.** UV seam rule from `water_rendering_architecture.md`: integer wrapping boundaries. The `MaxMinUV` floor-shift in `drawWater()` (`quad.cpp:2863-2884` and three siblings) is a per-triangle correction; replicate exactly in the VS or pre-bake into UV seeds at recipe build time.
2. **Mutable per-quad state still flows.** Per-vertex `lightRGB`/`fogRGB` are still per-frame; carried in the small per-frame water SSBO. Animation (cloudOffset, sprayFrame) is uniform.
3. **Stock install must remain playable** — env gate default-off until smoke-clean.
4. **`waterDetailHandle` = `terrainTextures2->getWaterDetailHandle(sprayFrame)`** — texture *handle slot index* mutates per-frame. Per `mc2_texture_handle_is_live.md`: store *slot index* in the recipe (or resolve fresh each frame), never cache the handle DWORD itself.
5. **Frustum admission stays naive.** Draw all N water quads per frame; let GPU clip-cull do the work. CPU admission pass = next slice (`vertexProjectLoop` queued in orchestrator).

## Validation gates

A. **Visual canary (manual side-by-side)** at Wolfman zoom on mc2_01: water animation, deep/middle/edge alpha bands, sky-reflection detail spray, shoreline transitions. `MC2_RENDER_WATER_FASTPATH=0` vs `=1` at fixed mission seed and a representative camera position. Visual canary catches shader-level drift the byte-compare physically cannot see (ARGB swizzle, UV precision, blend-state mistakes).

A'. **Screenshot-diff (automated)** at the same fixed seed/camera. Capture one frame each side, byte-compare PNGs. Expect bit-identical or near-identical (alpha-blended water + per-frame UV scroll means seed/frame-counter pinning matters; if necessary, compare the still-frame *before* spray animation kicks off, or pin `sprayFrame` to a fixed value during diff). Treat differences above a small per-channel-pixel threshold as failure. *Per user 2026-04-30: keep this as a separate gate from gate A, not as automation of it.*

B. **Tracy delta** on `Terrain::renderWater` zone, Wolfman mc2_01: target ≥ 50% reduction. Recon baseline 570–620 µs; expectation is sub-100 µs (the loop is gone; new path is one small uniform/SSBO update + one `glDrawArraysInstanced` call).

C. **`MC2_RENDER_WATER_PARITY_CHECK=1`** zero mismatches across **stock-content tier1 only** (5 missions). Per `m2_thin_record_cpu_reduction_results.md` the parity counter is silent-on-pass; any `[WATER_PARITY v1] event=mismatch` line = fail. *Per user 2026-04-30: non-stock content (Carver5O, Magic, Wolfman, Omnitech) is explicitly out of scope for validation at this point.*

D. **tier1 smoke 5/5 PASS** with `+0` destroys delta, both with `MC2_RENDER_WATER_FASTPATH=1` and `=0` (legacy regression coverage).

## NOT in scope

- M2 thin-record path. Different shader, different data, different SSBO. Water is a parallel structure (per orchestrator).
- GPU static-prop work (parked — see `cull_gates_are_load_bearing.md`).
- `vertexProjectLoop` offload (queued after this; ~500 µs/frame).
- Frustum-aware admission. The first cut draws all N water quads; GPU clips.
- Indirect terrain draw (the architectural endpoint; viable only after this slice + vertexProjectLoop).

## Build sequence

1. **[SHIPPED Stage 1, 2026-04-30]** WaterRecipe data structure + load-time population pass. *Initially* indexed by `quadList` slot (Stage 1 first cut), but that proved **broken** — see Stage 2 note below.

2. **[SHIPPED Stage 2, 2026-04-30]** Full SSBO/VS/draw pipeline visible-rendering on tier1.

### Stage 2 implementation notes (load-bearing for future work)

Implementing Stage 2 surfaced **eight project-specific gotchas** that are now codified in the code+memory. Future GPU-renderer work in this codebase MUST account for all of them:

1. **`quadList` is camera-windowed and reshuffles every frame** (mapdata.cpp:1072 `MapData::makeLists`). The original Stage 1 design indexed the static recipe by `quadList` slot, which silently went stale by frame 2. **Fix:** index by **map-stable `vertexNum`** (set at mapdata.cpp:1104 as `mapY * realVerticesMapSide + mapX`), build recipe directly from `MapData::blocks` (mission-static, never reshuffles). Per-frame thin record walks the live `quadList` window and looks up recipes via a `vertexNum → recipeIdx` hash.

2. **Render order matters.** `gamecam.cpp` calls `land->renderWater()` BEFORE `mcTextureManager->renderLists()`. Legacy water queues into mcTextureManager and drains during `renderLists()` AFTER terrain — that's why water alpha-blends correctly on top of terrain. A naive "do my fast path inside `renderWater()`" runs BEFORE terrain has been drawn, and terrain then overwrites the water. **Fix:** add a separate `Terrain::renderWaterFastPath()` hook called from gamecam.cpp AFTER `renderLists()`. The original `renderWater()` early-returns when fast-path is enabled.

3. **VAO 0 silently drops draws on AMD** (memory:projectz_overlay_findings.md). The fast path runs in a render-stage where the VAO may not be bound. **Fix:** call `gos_RendererRebindVAO()` at the start of the fast-path bridge.

4. **Custom REPEAT/LINEAR sampler required.** Bypassing `gosRenderMaterial::apply()` means inheriting whatever sampler was last bound — typically the patch_stream's CLAMP_TO_EDGE/LINEAR for terrain. Water UVs are world-derived (range 0..MaxMinUV ≈ 0..8), so CLAMP collapses every fragment to a texture-edge sample. **Fix:** the bridge function lazily creates and binds a REPEAT/LINEAR sampler object on unit 0; saves/restores the prior sampler.

5. **`mvp` uses GL_TRUE, `terrainMVP` uses GL_FALSE** (memory:terrain_mvp_gl_false.md, memory:terrain_tes_projection.md). Sending both with GL_FALSE silently transposes the screen-pixel→NDC matrix incorrectly, mapping all geometry off-screen.

6. **`uniform uint` crashes the shader_builder** (memory:uniform_uint_crash.md). The alpha-band byte uniforms are declared `uniform int` and cast to `uint` inside the shader for bitwise ops.

7. **`tex_resolve()` two-tier handle indirection** (memory:mc2_texture_handle_is_live.md). `terrainTextures2->getWaterTextureHandle()` returns mcTextureManager's `textureIndex`, NOT the engine's `gosTextureHandle`. Mirrors the M2d-overlay pattern at quad.cpp:2084.

8. **CPU pre-cull is THE LOAD-BEARING gate, not GPU clip** (memory:terrain_tes_projection.md, memory:clip_w_sign_trap.md). The Stuff matrix produces well-formed finite clip values for both visible AND behind-camera vertices with positive packaged w and NDC inside [-1,1]. **No GPU-side test** can distinguish them. **Fix:** per-triangle `wz ∈ [0,1)` bits set by CPU (sourced from the live `vertices[i]->wz` written by `setupTextures`' water projection at quad.cpp:715-722) and packed into the per-frame thin record. VS reads `pzTri1Valid`/`pzTri2Valid` from the thin record's flags and emits degenerate position when 0.

### Stage 2 validation

- **Visual parity:** mc2_17 (84.7% water, water band visible at smoke camera) — fastpath water band matches legacy near-identically (`tests/smoke/artifacts/water-diff-1777563391/`).
- **tier1 stability:** 5/5 PASS, +0 destroys delta, both `MC2_RENDER_WATER_FASTPATH=1` and `=0`. FPS marginally improved on water-heavy maps (mc2_17 132 vs 127, mc2_10 135 vs 132).
- **Per-frame counts (mc2_17 smoke):** legacy `handle_valid=72-76`, fastpath `pz_valid=43-45` thin records (CPU-pre-cull culls more aggressively than legacy's outer-gate alone, matching legacy's per-tri gz-check behavior).
2. **Add `gos_terrain_water_fast.vert`** that consumes the recipe + frame SSBO, computes UV (cloud + spray offsets), elevation→alpha band, emits 6 verts/instance.
3. **Add `Terrain::renderWaterFastPath()`** branch. Toggle inside `Terrain::renderWater()` on `MC2_RENDER_WATER_FASTPATH=1`.
4. **Parity check infrastructure.** Capture legacy `mcTextureManager->addVertices` calls under `MC2_RENDER_WATER_PARITY_CHECK=1` and diff against fast-path SSBO contents.
5. **Visual A/B validation** on mc2_01 at multiple camera positions/altitudes; compare with smoke runner screenshots.
6. **Smoke + perf gate.** Tier1 5/5 PASS both env states. Tracy delta verification.
7. **Promote** when validation lands: orchestrator status board, m2 results memory, new memory file `water_ssbo_pattern.md` (the static-SSBO + single-draw template the indirect-terrain endpoint will reuse).

## Cleanup deferred to ship

- Fix `MC2_WATER_DEBUG` frame-0 off-by-one (cosmetic).
- Demote `MC2_WATER_DEBUG` printer to silent post-fix per debug-instrumentation rule (`memory/debug_instrumentation_rule.md`).
