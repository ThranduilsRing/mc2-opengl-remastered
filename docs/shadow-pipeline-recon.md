# SHADOW-PIPELINE-RECON — What casts/receives shadows today (2026-06-11)

Recon-only synthesis. Builds on (does not duplicate): `shadow-rv-arc-recon.md`
(end-to-end authority chain), `shadow-frustum-audit.md` (texel-density math +
B′ bounded-near fit), `shadow-static-buildings-recon.md` /
`shadow-static-buildings-registry-recon.md` (static-building caster slice),
`shadow-soak-1.md` (debug/tuning tooling), `hzb-depth-convention.md`
(reverse-Z contract), `terrain-decal-lighting-recon.md` (overlay receivers).
All line numbers re-verified against the current worktree.

---

## (a) Caster / receiver matrix per object family

| Family | CASTS into | How / where | RECEIVES via | Where |
|---|---|---|---|---|
| **Terrain heightfield** | STATIC map (once per mission) | `Terrain::mapData->renderStaticTerrainShadowFullMap` (`mclib/txmmgr.cpp:2185`, impl `mclib/mapdata.cpp:1294`), `shadow_terrain.*` tessellated depth | Inline PCF in frag — both maps, `min()` combine | legacy `gos_terrain.frag:792-801`; chunk `terrain_lod_chunk.frag:482-484` |
| **Chunk GPU terrain** (default-on production path) | Same static map as legacy — the static prepass is gated on `gos_IsTerrainTessellationActive()` (`txmmgr.cpp:2163`), independent of the chunk draw | n/a (chunk path adds no new caster) | `#include <include/shadow.hglsl>` (`terrain_lod_chunk.frag:11`); driver explicitly binds maps+matrices+`enableShadows` because the chunk draw is a state bolt-on (`gos_terrain_lod_chunk.cpp:568-603`, comment :570). Debug skip bit 8 in diag bitmask (:40) | same |
| **Mechs/vehicles (GPU batcher)** | DYNAMIC map, every frame | `GpuMechBatcher::flushShadow()` (`gos_mech_batcher.cpp:705`, callsite `txmmgr.cpp:2458`), `shadow_mech.vert` GPU-skinned, prev-frame ring slot | Deferred screen-shadow pass; GBuffer1 eligible flag | `mech.frag:192` → `shadow_screen.frag:147-192` (`min()` combine) |
| **Static props — trees/foliage** | DYNAMIC map (camera-visible subset only — `s_typeRanges` is the per-frame visible bucket) | `GpuStaticPropBatcher::flushShadow(s_skipBldgInDynamic)` (`txmmgr.cpp:2456`, impl ~`gos_static_prop_batcher.cpp:7150`), `shadow_static_prop.vert` instanced | Screen-shadow pass | `static_prop.frag:428` |
| **Static props — buildings** | DYNAMIC map by default; opt-in STATIC map all-buildings one-shot (`MC2_STATIC_PROP_BUILDING_SHADOW=1`, default OFF) — registry-driven, visibility-independent, drawn once inside the static-latch block | `txmmgr.cpp:2194-2214` → `drawStaticBuildingShadows()`; population tag from `BldgAppearance::registerStatic` (`bdactor.cpp:2538` area). When gate on, buildings are skipped in the dynamic pass (`txmmgr.cpp:2302-2325, 2456`) | Screen-shadow pass | `static_prop.frag` / `gos_tex_vertex_lighted.frag:68` |
| **Legacy blob shadow shapes** (mechShadowShape ASE) | Nothing (modern) — explicitly disabled: `mechShape->SetUseShadow(false)` (`mech3d.cpp:3748`) + "Slice D-shadow-skip" when GPU mech path + tessellation active (`mech3d.cpp:3758+`); bdactor gates at `bdactor.cpp:2738/5169` on legacy `useShadows` | n/a | n/a | legacy projected-texture path, effectively dormant on the modern renderer |
| **Terrain overlays (cement/road)** | Do not cast | — | Inline static+dynamic sampling — `setupOverlayShadowsForShp` uploads `lightSpaceMatrix`/`dynamicLightSpaceMatrix`/`enableShadows` (`gameos_graphics.cpp:~7679-7754`) | `terrain_overlay.frag`; writes `rc_gbuffer1_shadowHandled` so the screen pass skips it |
| **Decals (craters/footprints)** | Do not cast | — | NONE (intentional; marked shadowHandled) | `decal.frag` |
| **Grass** | Does not cast | — | shadowHandled (excluded from screen pass) | `gos_grass.frag` |
| **Water** | Does not cast | — | screen-shadow ELIGIBLE with flat-up normal | `gos_terrain_water_mdi.frag:251-313` |
| **VFX / gosFX particles** | Do not cast (never enter either shadow pass — only the two batchers + terrain feed the FBOs) | — | Excluded from screen pass: billboards don't write GBuffer1 eligible; `particle_billboard.frag` reconstructs depth but only for soft-fade, and sky/far guard depth==0 | `shadow-rv-arc-recon.md` §7.1 |
| **Editor** | Terrain static path only if tessellation active; the dynamic block additionally requires `g_useGpuObjects || g_useGpuMechs` (`txmmgr.cpp:2261`). Editor has a legacy `useShadows ^= true` toggle (`editor/EditorInterface.cpp:2012`) controlling the OLD blob path, not the modern maps | | | Editor runs the same default-on chunk terrain; modern shadows follow the same gates as game |

Eligibility contract: `rc_gbuffer1_screenShadowEligible` (normal.a=0.0) vs
`rc_gbuffer1_shadowHandled` (a=1.0) in `shaders/include/render_contract.hglsl:27-46`;
known violation = water/shoreline continuous materialAlpha escape hatch (:48-60).

## (b) Shadow map inventory

| | STATIC map | DYNAMIC map |
|---|---|---|
| Texture | `shadowDepthTex_` + `shadowFBO_` (`gos_postprocess.cpp` initShadows) | `dynShadowDepthTex_` + `dynShadowFBO_` |
| Size/format | 4096² `GL_DEPTH_COMPONENT24`, PCF sampler (`COMPARE_REF_TO_TEXTURE`/`LEQUAL`), border 1.0=lit, AMD dummy R8 color | same |
| Registry | `RenderResourceId::ShadowStaticMap` | `ShadowDynamicMap` (both registered, SHADOW-RESOURCE-1) |
| Projection | World-fixed ortho, center (0,0,0), r=mapHalfExtent·√2·1.05, near 1 far 2r, sun frozen at build; `buildStaticLightMatrix` `gos_postprocess.cpp:2328` | Per-frame camera-frustum-fit, pow-2 + texel snapped, depthDist 5000; `buildDynamicLightMatrix` `gos_postprocess.cpp:2522`. B′ bounded-near cap `MC2_SHADOW_BOUNDED_NEAR_FIT` (default OFF, `gos_postprocess.cpp:2680`) → 2.0 WU/texel vs degenerate 5.57 full-map |
| Renders into | Terrain heightfield (`shadow_terrain.*`); optional one-shot building append (gate above) | static-prop + mech batchers (`shadow_static_prop.vert`, `shadow_mech.vert`); writers all `gl_FragDepth = gl_FragCoord.z`, forward-Z |
| Sampled by | terrain inline (legacy unit 9 / chunk `kChunkTexUnitStaticShadow`), screen pass unit 2 | terrain inline (unit 10 / chunk unit), screen pass unit 3 |
| Rebuild | once per mission; latch re-armed only by `Terrain::destroy` → `gos_ResetStaticLightMatrix` (`mclib/terrain.cpp:781`) | every frame |

Light basis: shared `mc2ComputeLightBasis` (`gos_postprocess.cpp:2288-2327`),
SHADOW-ROBUST-BASIS-1 default-ON — legacy up-hint, singularity re-pick guard
(the prior "light-basis singularity" fix). Sampling library:
`shaders/include/shadow.hglsl` — gradient-adaptive Poisson PCF, back-face
guard NdotL<0.05→lit, OOB→lit, floor `mix(0.4,1.0,ratio)`.

## (c) Reverse-Z / bias convention audit

- Main scene: reverse-Z, `glClearDepth(0)`, `GL_GREATER/GEQUAL`, clip
  `ZERO_TO_ONE` (locked contract, `hzb-depth-convention.md`).
- Shadow passes: **forward-Z**, `glClearDepth(1)`, `GL_LESS` — boundary is a
  manual `glClearDepth` swap in begin/end pass (fragile; missing call =
  silent corruption). Both light matrices use clip-Z [0,1].
- Chunk terrain draw sets its own depth/blend state explicitly after the
  transparency-saga lesson (10.3, `f375e0ba`): bolt-on draws inherit nothing.
- Bias: dynamic-path `glPolygonOffset(2.0, 4.0)` (ImGui-tunable
  `shadowBiasFactor_/Units_`); **static terrain prepass has NO polygon
  offset** (`gos_BeginShadowPrePass`, gameos_graphics.cpp:4966 area); the
  static-building append adds its own offset. Per-pixel slope bias
  0.002–0.005 in `shadow.hglsl`; screen pass fixed 0.003 (terrain-calibrated,
  may under-bias oblique building faces → self-shadow acne risk).
- Combine is `min(static, dynamic)` everywhere now (C-pre `7ea32b83`):
  `gos_terrain.frag:801`, `terrain_lod_chunk.frag:484`,
  `shadow_screen.frag:147-192` — idempotent, double-caster safe. (A stale
  comment at `gos_terrain.frag:831` still says "staticShadow * dynShadow".)

## (d) Gaps & inconsistencies

1. **Texel density is THE quality limiter** — 5.57 WU/texel full-map dynamic
   default; trees ≈1 texel (frustum-audit). B′ fix exists but **default OFF**;
   static-building far-field fix exists but **default OFF**.
2. **Dynamic caster feed = camera-visible buckets only** — off-screen casters
   at low sun never shadow into view (`s_typeRanges`, recon §2).
3. **Static map staleness** — destroyed buildings (under the gate) and any
   sun change keep stale shadows until mission reload; one-shot latch, no
   invalidation path.
4. **Forward-Z/reverse-Z manual `glClearDepth` boundary** — no assertion or
   RAII guard; any new pass inserted between begin/end can silently corrupt.
5. **No dynamic-pass polygon offset audit closed** — soak Row 6 follow-up
   (mech contact acne) never resolved; soak observation log still blank.
6. **Static prepass no polygon offset** for terrain depth.
7. **Chunk terrain has no tessellation** but samples the tessellated-terrain
   static shadow — caster/receiver geometry mismatch at extreme zoom
   (displacement desync risk class, rv-recon §9.4).
8. **Water/shoreline GBuffer1 mask contract violation** (continuous alpha,
   render_contract.hglsl:48-60) — shadow behavior flips with water depth.
9. **Decals/overlays receive inconsistently** (overlay inline-samples, decal
   none) — seam visibility rises with terrain lighting strength
   (terrain-decal-lighting-recon).
10. Stale comment `gos_terrain.frag:831`; uniforms still raw `glUniform`
    (not ViewUniforms UBO); shadow EngineViews registered but observer-only.

## (e) Ranked improvement candidates

1. **Flip B′ bounded-near fit + static-buildings default-ON** (after a soak
   pass) — biggest visual win for zero new code; both already shipped and
   gated (`MC2_SHADOW_BOUNDED_NEAR_FIT`, `MC2_STATIC_PROP_BUILDING_SHADOW`).
   Risk: far-field tree shadows vanish outside the near box (accepted trade);
   building destruction stale shadows (file follow-up re-arm).
2. **Stable 2-cascade CSM** (E in frustum-audit) — near crisp + far full-map;
   B′ is cascade-0 groundwork, EngineView ShadowDynamic + cascade naming
   already reserved. Risk: MEDIUM-HIGH — touches shadow.hglsl ABI, all
   receiver shaders, screen pass; must keep min-combine and forward-Z
   convention; do behind a gate with shadow_debug visualizer per cascade.
3. **Shadow debug overlay completion** (SHADOW-ENV-1 + capture presets) —
   `MC2_SHADOW_DEBUG_MODE` env var so soak/captures are automatable; the
   ImGui/hotkey tooling already exists (soak doc). Risk: trivial, debug-only.
   Do this BEFORE 1/2 so the flips are validatable.
4. **Self-shadow / bias consistency** — add polygon offset to the static
   terrain prepass, audit dynamic-pass offset (soak Row 6), expose screen-pass
   fixed 0.003 bias. Risk: LOW but unvalidatable without 3 first.
5. **Contact shadows / SSAO-lite** — `ssao.frag` exists and already agrees on
   reverse-Z reconstruction; a short screen-space ray from depth would mask
   the texel-density floor for small casters (trees). Risk: MEDIUM — new
   full-screen cost, must respect shadowHandled mask and water escape hatch.
6. **Caster-feed widening** (off-screen casters into dynamic map via
   light-frustum admit instead of camera buckets) — only if low-sun missions
   visibly miss shadows after 1; secondary per frustum-audit.

## (f) What NOT to touch

- **`glClearDepth` swap in begin/end shadow pass** and the forward-Z shadow /
  reverse-Z scene split — load-bearing, fragile; don't "unify".
- **`mc2ComputeLightBasis` legacy up-hint primary path** — byte-identical to
  legacy by design; only the degenerate branch is new. Don't re-orient the
  texel grid.
- **`min()` combine** — reverting to multiply re-introduces camera-dependent
  double-darkening.
- **Chunk-driver explicit state/uniform binding block**
  (`gos_terrain_lod_chunk.cpp:568-603`) — the bolt-on must keep setting ALL
  state it depends on (10.3 lesson).
- **Pow-2 + texel snap in `buildDynamicLightMatrix`** — anti-shimmer; any CSM
  work must preserve per-cascade snapping.
- **GBuffer1 shadowHandled/eligible sentinel encoding** and the water escape
  hatch (`render_contract.hglsl`) — grep-census-enforced; resolve via the
  planned F1 split, not ad-hoc edits.
- **Mech shadow prev-frame ring-slot read** — fence-safe one-frame lag by
  design; current-frame reads race the GPU.
- Static-map one-shot latch ordering inside `txmmgr.cpp:2163-2224` —
  registry-flush timing was the cause of the failed Option B; the registry
  one-shot deliberately avoids per-frame ordering dependencies.
