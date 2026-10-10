<!-- doc-status: stale | reviewed:2026-10-11 -->
# TERRAIN-RUNTIME-API-RECON-1 — Four Terrain Landmines

Read-only recon (2026-06-25). Worktree `nifty-mendeleev`. All file:line verified by reading code.
Classifies each landmine as: colormap-baked (CPU load) / shader-runtime / geometry.

---

## 1. Additive detail/normal layer — `sampler2DArray` (marble cliff mat5)

**CLASS: shader-runtime (texture-array sample, additive into detailN).**

- GLSL uniform decl: `uniform sampler2DArray matNormalArray;` — `shaders/gos_terrain.frag:56`.
- Layer index defines: `shaders/include/terrain_mat_layers.hglsl:5-14`.
  `MAT_LAYER_MARBLE_CLIFF = 5  // mat5_normal.tga (marble_cliff_01)` (line 10).
  Layer order MUST match `buildTerrainNormalArray()` in C++ (comment lines 1-4).
- How indexed: `texture(matNormalArray, vec3(uv, float(MAT_LAYER_*)))` — e.g. cement uses
  `MAT_LAYER_PAINTED_CONC` at `gos_terrain.frag:720`. mat5/marble cliff slot is reserved in the
  enum but the LIVE additive splat in `applyDetailNormals` (frag:700-725) sums per-material
  weights through `matNormal0..4` individual samplers under the non-array `#else` path
  (lines 700-711); the array path (`TERRAIN_NORMAL_ARRAY`) is the sampler2DArray consumer.
- ADDITIVE: yes. `detailN += weight * normalBoost * fw * (sample.rgb*2.0-1.0)` (frag:702,706,710,724).
  detailN is a tangent-space perturbation accumulator; it is NOT added to colormap COLOR — it
  perturbs the surface normal `N.xy = detailN.xy * detailNormalStrength.x` (frag:728). The
  separate triplanar CLIFF *color* darken (frag:803-815) is a runtime color mix, not the array.
- C++ binding: `buildTerrainNormalArray()` `GameOS/gameos/gameos_graphics.cpp:1962`; built lazily,
  bound via `glUniform1i(sl.matNormalArray, kTerrainTexUnitNormalArray)` at gameos_graphics.cpp
  :6249-6250, 6734-6735, 6869-6870, 7023-7024. Uniform locs fetched at :2486/2549/2608.
  NOTE: enum comment (hglsl:3) warns C++ layer order in buildTerrainNormalArray must stay lockstep.

---

## 2. Cement burned into colormap (always flat) + cement↔terrain TRANSITION

**CLASS: SPLIT — solid cement = colormap-baked (CPU load); transition = shader-runtime mask, currently shader PASS-THROUGH.**

- CPU bake (cement burned into composite colormap at load):
  `TerrainTextures::combineOverlayTxm` — `mclib/terrtxm.cpp:1131`. `isCementType()` gate at
  terrtxm.cpp:1116-1120; cement composites regenerated from source .tga (comment :110), per-corner
  cement routing at :1275/1305/1322/1337. This is where solid cement gets its flat authored tone
  baked into the colormap (the shader later "preserves authored colormap tone for runway/cement",
  frag:795-801 concreteColorBlend).
- Runtime transition (the "hard to get on same layer as terrain" part):
  `uniform sampler2DArray u_transitionMaskArray;` — `gos_terrain.frag:95` (unit 4, 14 layers R8).
  cementWord bit layout: bit31=VALID, bit30=IS_TRANSITION, bits29:24=maskId→u_transitionMaskArray,
  bits15:0=cement atlas layerIdx (frag:431-435).
  Override branch: `gos_terrain.frag:436-453`. **KEY LANDMINE (frag:449-451):** when isTransition,
  the shader is a NO-OP pass-through — `// Transition: legacy overlay draw handles cement blend.
  Shader pass-through.` Only SOLID cement (`else`, line 452) does `texColor = cementColor`.
  → The cement↔terrain transition is NOT composited in the terrain material layer; it is deferred
  to a separate legacy overlay decal draw. This is the "can't get transition on the same layer as
  terrain" problem (cf. MC2_TERRAIN_CEMENT_BAKE / CEMENT-TRANSITION-COMPOSITE-1 Path A which tried
  to move it in-material via mix(texColor,cementColor,u_transitionMaskArray[maskId])).
- C++ transition bind: `gos_terrain_indirect_isTransitionMaskReady()` →
  `glActiveTexture(GL_TEXTURE4)` + `glGetUniformLocation(prog,"u_transitionMaskArray")` /
  `u_useTransitionMask` at gameos_graphics.cpp:4133-4145, 4290-4291, 4460-4472, 4525-4526.

---

## 3. Prior heightmap RESAMPLE attempt (resample, NOT geometric displace)

**CLASS: CPU-load (bilinear resample of cached source elevations) feeding a shader-runtime R32F normal recompute. NO geometry displacement.**

- TERRAIN-RESAMPLE-1 lives in `GameOS/gameos/gos_terrain_height_tex.cpp`.
  - Env gate: `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR` (1/2/4) — `read_resample_factor_env()`
    gos_terrain_height_tex.cpp:45-51.
  - CPU bilinear resampler: `resample_bilinear()` :59-65 (corner-aligned). Applied at :141-152
    (`if factor>1` → resampled buffer becomes uploadPtr).
  - Source elevations cached for live re-resample at :131; globals g_side(post)/g_sourceSide(pre)/
    g_resampleFactor at :24-26; live re-resample without touching MapData at :255.
  - Exposed: `gos_terrainHeightResampleFactor()` :285.
- It resamples the HEIGHT TEXTURE only (R32F, sampler unit 11). The resampled height feeds the
  per-fragment normal recompute `computeTerrainNormalFromHeight()` in
  `shaders/include/terrain_height_normal.hglsl:59-90` (central-difference from 4 axial samples;
  span scales with worldUnitsPerVertex/factor — finer resample → tighter span → more sensitive
  normal). Consumed in gos_terrain.frag:736-745 under `useTerrainNormalsFromHeight`.
- It does NOT displace tessellated geometry — purely a normal-shading input. This is the
  "resample, not displace" prior art the new TerrainRuntime visual-height API must reconcile with.

---

## 4. Shoreline attempt — partially live, "non-functional" claim is overstated

**CLASS: geometry-selection (CPU quad predicate) + shader-runtime fade. Functional in water FS; CPU predicate is a heuristic fallback that can be dead when alphaDepth=0.**

- CPU shore-extension quad predicate: `mclib/terrain.cpp:3545-3566` (BOTH water-fast-path branch
  3539-3554 and legacy branch 3556-3566). `shoreExt = MapData::alphaDepth*0.5 (else 15.0f)`;
  includes submerged-sand tiles slightly ABOVE waterElevation so the water mesh extends onto shore.
  `submergedSand` test on the 4 vertex elevations < `we + shoreExt`. Appends narrow water candidate
  (3568-3569 `WaterStream::AppendNarrowCandidate`).
- Shader fade (this IS functional): `shaders/gos_terrain_water_mdi.frag:175-180`:
  `shoreBlend = max(alphaDepth,1.0); shore = smoothstep(-shoreBlend*0.5, shoreBlend, WaterThickness);
  if (shore<=0.0) discard;` — negative-WT (above-water) tiles fade out; debug mode 5 = shore mask
  (:276). Alpha output `shore * WATER_MAX_ALPHA` (:300).
- WHY it may look dead / non-functional: the whole shore extension is gated on `alphaDepth`. When a
  mission's `MapData::alphaDepth == 0` the CPU side falls back to a flat 15.0 world-unit shoreExt
  and the FS `shoreBlend` floors to 1.0 → the smoothstep range collapses to (-0.5,1.0) world units,
  far smaller than tile spacing (128 wu), so the ramp is effectively a hard 1-tile edge (no visible
  shoreline gradient). i.e. it RUNS but produces no perceptible shoreline on alphaDepth=0 maps,
  which reads as "non-functional." No dedicated shoreline texture/mask layer exists — it is purely
  a water-alpha ramp, not a sand/foam material.

---

## 5. Mine overlay / decal layer (user field note 2026-06-25)

The terrain overlay/decal layer carries **literal landmines** — a per-cell mine
state drawn as a terrain decal. It is a SEPARATE data axis from height/material.

- **STATE LIVES IN PACKET-4 MOVE GRID, not PostcompVertex.** Per-cell field on
  `MapCell` (`move.h:362-366` `getMine()/setMine()`); map-wide accessor
  `MissionMap::getMine(row,col)` (`move.h:669-678`). Loaded from `.pak` packet 4
  via `mapData[r*w+c].mine` (`move.cpp:1024`, `setMine` 1024/1065); object-placed
  mines stamp cells in `move.cpp:1248-1265` (`setInnerSphereMine`/`setClanMine`).
  Incremental `tileMineCount` maintained on `setMine` (`move.h:682`). So mines are
  in the SAME packet axis as passability — NOT derivable from the terrain vertex.
- **RENDER = terrain decal/overlay riding the VISUAL mesh.** `TerrainQuad` samples
  `GameMap->getMine(cellR,cellC)` (`quad.cpp:455/824/903`) and enqueues per-quad
  mine state (`enqueueTerrainMineState`, `mineResult.setMine` `quad.cpp:463/833/912`);
  textures via `TerrainQuad::initMineTextureHandles` (`mclib/quad.cpp:129`). Drawn onto
  the terrain surface like the cement/road decals — **shares the overlay/decal
  render layer with the cement transition (landmine #2)**.
- **Objects too:** `isLandMine(objTypeNum)` (`objmgr.cpp:1329/1697`) classifies
  mine GameObjects; `MC2_TERRAIN_MINE_AB` (`mclib/terrain_update.cpp:243-325`) is a read-only
  legacy-vs-grid placement diagnostic.

**Divergence break-risk:** HIGH z-fight — same class as craters / decal ring. If
`visualHeight` diverges from `gameplayHeight`, mine decals (placed on the visual
quad surface) float/sink relative to units (grounded on gameplay height).

**API consequence:** mines = a future **feature/state axis sourced from packet-4
MOVE** (alongside passability), NOT a `sampleFeatureMask` derived from terrain
geometry. A future `FM_Mine` would forward to `GameMap->getMine`, coupling
terrain_runtime to move.h — deferred; documented here, not coded in API-1. Any
fix that moves the cement transition off the colormap onto a proper decal/overlay
layer must keep hosting the mine decal on that same layer.

---

## TL;DR table

| # | Landmine | Primary file:line | Class |
|---|----------|-------------------|-------|
| 1 | matNormalArray (mat5 cliff) | gos_terrain.frag:56,720 + terrain_mat_layers.hglsl:10 + GameOS/gameos/gameos_graphics_renderer_internal.h:1094 | shader-runtime (additive to normal) |
| 2 | cement bake + transition | mclib/terrtxm.cpp:1178 (bake) + gos_terrain.frag:436-453 (transition pass-through) | bake=CPU-load; transition=shader-runtime but currently overlay-deferred |
| 3 | height resample | gos_terrain_height_tex.cpp:45-152 + terrain_height_normal.hglsl:59-90 | CPU-load resample → shader normal recompute; NO geometry |
| 4 | shoreline | terrain.cpp:3545-3566 + gos_terrain_water_mdi.frag:175-180 | CPU quad-select + shader fade; dead-ish when alphaDepth=0 |
| 5 | mine overlay/decal | move.h:362-678 (packet-4 state) + quad.cpp:455-921 (decal draw) + code/objmgr_io.cpp:97 | packet-4 MOVE state axis + terrain decal on visual mesh; HIGH z-fight on height divergence |
