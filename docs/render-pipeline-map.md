<!-- doc-status: current | reviewed:2026-10-11 -->
# Render Pipeline Map

> **Branch:** `claude/nifty-mendeleev` · **Verified:** 2026-06-04 (full re-trace, file:line grep-confirmed)
>
> How each object type travels from asset-on-disk to pixels on screen. This is the **living** map —
> refresh it in place (see Maintenance). The prior `render-pipeline-matrix.md` is now a frozen
> snapshot at [`observations/2026-05-14-render-pipeline-matrix.md`](observations/2026-05-14-render-pipeline-matrix.md)
> (its cross-cutting-concerns and pattern-family sections are still worth reading); the
> [`observations/2026-05-25-pipeline-master-index.md`](observations/2026-05-25-pipeline-master-index.md)
> series is likewise a dated snapshot. Both predate the GPU-driven default-on flips captured here.

Line numbers drift — symbols are stable. Grep the symbol when precision matters.

---

## The big picture: two render spines

Almost everything is a **TGL shape** (`.ase` authored → `.tgl` binary cache) loaded by one shared
loader (`TG_TypeMultiShape::LoadTGMultiShapeFromASE`, `mclib/msl_load.cpp:441`). What differs is *which of
two spines* carries it to the screen.

```
                          ┌─────────────────────────────────────────────┐
  .ase / .tgl  ──load──►  │  TG_MultiShape (CPU geometry + textures)     │
  (shared TGL loader)     └─────────────────────────────────────────────┘
                                   │                          │
              ╔════════════════════╪══════════╗   ╔═══════════╪═══════════════════════╗
              ║  SPINE A (legacy)  │          ║   ║  SPINE B (GPU-direct batchers)    ║
              ║  per-actor render()→ ENQUEUE  ║   ║  register once → build SSBOs →    ║
              ║  into mcTextureManager        ║   ║  GPU cull → indirect/instanced    ║
              ║  master-node arrays           ║   ║  draw                             ║
              ╚════════════════════╪══════════╝   ╚═══════════╪═══════════════════════╝
                                   ▼                          ▼
                    txmmgr.cpp renderLists() flush  ◄── both fire here (Spine-B batchers are
                    ShapeRenderer → glDrawElements       *called from* renderLists; terrain/water
                    (gos_tex_vertex_lighted)             hook right after it)
```

**Load-bearing convention** (`memory/render_functions_are_enqueuers_not_submitters.md`): an object's
`render()` method does **not** issue GL. Spine A enqueues into `masterHardwareVertexNodes[]`; Spine-B
batchers stage SSBOs. The actual `glDraw*` happens inside `MC_TextureManager::renderLists()`
(`mclib/txmmgr_lights.cpp:531`) — or, for terrain/water, in a GPU-direct fast-path hook *after* it
(`memory/render_order_post_renderlists_hook.md`). The MLR path (`mclib/mlr/`, used by some gosFX) is
the one immediate-draw exception.

## Frame render order (`GameCamera::render` → `renderLists`)

```
1.  HDRI skybox        → sceneFBO_                      (gamecam.cpp:279)
2.  Shadow pre-passes  → shadowFBO_ / dynShadowFBO_     (txmmgr.cpp:2153–2330)
3.  renderLists() flush:                                (mclib/txmmgr_lights.cpp:531)
       Terrain DrawIndirect
       GpuStaticPropRegistry.flush → GPU cull → GpuStaticPropBatcher.flush   (props/trees/buildings)
       GpuMechBatcher.flush                                                  (mechs)
       ShapeRenderer loop over masterHardwareVertexNodes  (vehicles, generic, CPU-fallback mechs, MLR FX)
4.  Water fast-path    → renderWaterFastPath()  (post-renderLists overlay)   (gamecam.cpp:354)
5.  GPU particles      → Batcher::Flush()       (gosFX billboards)           (gamecam.cpp:388)
6.  Post-process       → bloom / tonemap / SSAO / screen-shadow             (gos_postprocess.cpp)
7.  HUD batch replay   → gos_RendererFlushHUDBatch()                         (gameosmain.cpp:622)
8.  ImGui              → GuiRuntime::Render()                                (gameosmain.cpp:626)
```

---

## 1. Terrain — `colormap + heightmap → tessellation/compute → GPU indirect`

**Status: GPU indirect is DEFAULT** (`MC2_TERRAIN_INDIRECT` ON). Tessellated patch path is the fallback.

```
mission .pak (PostcompVertex: elevation, terrainType, water bits)
 + <mission>.burnin.ktx2   (5120² BC7 colormap atlas)  [or .jpg/.tga fallback]
        │ load
        ▼
Terrain::init + MapData::newInit            (mclib/terrain.cpp:387, mclib/terrtxm2_load.cpp:108)
   → R32F height texture (unit 11)  +  BC7 colormap atlas (unit 0)
        │ per-mission bake
        ▼
BuildDenseRecipe() → TerrainQuadRecipe[] SSBO (binding 0, 144B/quad, world-indexed)
                                            (GameOS/gameos/gos_terrain_indirect_cement.cpp:422)
        │ per-frame
        ▼
makeLists() camera window  +  gos_terrain_lighting.comp → per-vertex light SSBO (binding 1)
        ▼
gpu_driven_terrain_solid.comp  → culls every recipe, atomicAdds visible quads,
                                 packs TerrainQuadThinRecord[] (binding 2/3) + indirect cmd
        │ draw — inside renderLists flush
        ▼
DrawIndirect() → glMultiDrawArraysIndirect(GL_TRIANGLES)   (GameOS/gameos/gameos_graphics_water.cpp:629)
        ▼
RENDERED BY: gos_terrain_thin.vert  +  gos_terrain.frag
   (PBR splat: colormap HSV-classified into rock/grass/dirt/concrete, POM, normals-from-height,
    calcShadow PCF, distance LOD)
```

- **Fallback (gate off):** tessellated `GL_PATCHES` via `gos_terrain.vert/.tesc/.tese/.frag`.
- **Caveat:** `Terrain::quadList` is a **camera sliding window** rebuilt every frame — index static
  SSBOs by stable world `vertexNum`, never by quadList slot
  (`memory/terrain_quadlist_is_camera_sliding_window.md`).
- **Gates:** `MC2_TERRAIN_INDIRECT` (ON), `MC2_COLORMAP_KTX2` (ON),
  `MC2_TERRAIN_SOLID_NARROW` (ON).

## 2. Water — `terrain water-flags → GPU recipe cull → overlay draw`

**Status: GPU full-recipe cull is DEFAULT** (`MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE` ON). Water is a
transparent overlay drawn *after* terrain depth, not terrain splatting.

```
MapData::calcWater(): PostcompVertex.water bit + Terrain::waterElevation   (mapdata.cpp:574)
        │ per-mission
        ▼
WaterStream::Build() → WaterRecipe[] SSBO (binding 5, 64B/quad, world-indexed, static)
                                            (gos_terrain_water_stream.cpp:200)
        │ per-frame
        ▼
gpu_driven_water.comp  → culls full world recipe set, reprojects at wave-bobbed waterElevation,
                         reads terrain-lighting SSBO (binding 1), atomicAdds → WaterThinRecord[] (binding 6)
        │ draw — fast-path hook AFTER renderLists (gamecam.cpp:354)
        ▼
gosRenderer::renderWaterFastPath()  (gameos_graphics.cpp:2402)
   self-manages: GEQUAL reverse-Z, depthMask OFF, SRC_ALPHA blend, CULL_FACE OFF
   → glMultiDrawArraysIndirect (base + detail layers)
        ▼
RENDERED BY: gos_terrain_water_fast_mdi.vert  +  gos_terrain_water_mdi.frag
   (Beer-Lambert depth color, shore smoothstep+discard, dual-fBm ripples+glint,
    SH-L2 sky reflection [gated], optional terrain reflection RT)
```

- **Note:** the FS is a dedicated `gos_terrain_water_mdi.frag` — *not* the old `gos_tex_vertex.frag`.
- **Gates:** `MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE` (ON), `MC2_RENDER_WATER_FASTPATH`,
  `MC2_GPU_DRIVEN_WATER`, `MC2_WATER_REFLECTION_RT` (OFF), `MC2_MISSION_INTRO_LEGACY_RENDER` (OFF).

## 3. Static props (buildings, trees, fences, static turrets, rocks) — `.ase → registry → GPU instanced indirect`

**Status: DrawPacket-v8 / `glMultiDrawElementsIndirect` (MDI) is DEFAULT.** All world-fixed
non-animated geometry shares Spine-B's `GpuStaticPropBatcher` / `GpuStaticPropRegistry`. Geometry is
uploaded once to an immutable VBO; instances are GPU-culled and drawn via indirect MDI.

```
<name>.ase/.tgl  +  <type>.ini (FitIni LOD/filenames)  +  mission placement
        │ load
        ▼
LoadTGMultiShapeFromASE()                              (mclib/msl_load.cpp:365)
        │ register ONCE at mission load
        ▼
GpuStaticPropBatcher::registerType → finalizeGeometry  (GameOS/gameos/gos_static_prop_batcher_state.cpp:1035 / 2009)
   → immutable VBO/IBO (glBufferStorage), texture arrays (alpha-OFF + alpha-ON groups),
     MaterialGpu table SSBO (binding 5)
   → GpuStaticPropRegistry::registerRecipe   (permanent per-instance recipe + baked light slot)
        │ per-frame
        ▼
ExtractRenderSnapshot (dirty-only memcpy fast path) → registry.flush()  (mclib/txmmgr_lights.cpp:822)
   → fills instance SSBO (binding 0) + substrate_appendStaticPropRecord
        ▼
gpu_cull::compute_dispatch()  → frustum-culls instances, writes GPU-authoritative
                                instanceCount into indirect cmd buffer   (mclib/txmmgr_lightrow.cpp:148)
        │ draw — inside renderLists ("Render.GpuStaticProps")
        ▼
GpuStaticPropBatcher::flush()  → glMultiDrawElementsIndirect ×2 (alpha-OFF, then alpha-ON groups)
                                            (GameOS/gameos/gos_static_prop_batcher_submit.cpp:817 / 6191)
        ▼
RENDERED BY: static_prop.vert  +  static_prop.frag
   (instance modelMatrix from SSBO, Stuff→GL axis swap, calc_light via per-instance lightDataIndex,
    texture-array sample, alpha-discard for foliage cards)
```

**Sub-type divergences:**

| Sub-type | Class | Divergence |
|---|---|---|
| **Building** | `BldgAppearanceType` (`bdactor.cpp`) | LOD swap + damage shape; **animated** buildings lose static registration and fall back to dynamic submit (`MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE` allows static when not animating); optional static building shadow map |
| **Tree** | `TreeAppearanceType` (`bdactor.cpp:3396`) | loaded with `SetAlphaTest(true)` → alpha-ON group + `frag` alpha-discard; lit-color floor `max(rgb,0.5)` avoids black cards; LOD multi-shapes (×5 distance push-out). **No impostor on nifty** — the 2-card billboard far-LOD is a `model-override` branch feature |
| **Fence / rock / prop** | `GenericAppearanceType` (`genactor.cpp`) | `Generic` population; enters Spine B when `g_useGpuObjects` |
| **Static turret** | `BldgAppearance` (via `code/turret.cpp`) | renders through the **building/static-prop path**, not the vehicle path |
| **Window nodes** | (within buildings) | node-name `LitWin_*` → `isWindow` flag → VS skips `calc_light`, keeps hot-color magic |

- **Gates:** `MC2_GPU_OBJECTS`, `MC2_STATIC_PROP_REGISTRY`, `MC2_GPU_CULL_SUBSTRATE`, `MC2_MATERIAL_GPU(_SAMPLE)`,
  `MC2_STATIC_PER_INSTANCE_LIGHT`, `MC2_STATIC_LIGHT_UPLOAD_SPLIT`, `MC2_STATIC_PROP_PERSISTENT_BUCKETS`
  (all DEFAULT ON); `MC2_STATIC_PROP_LEGACY_DISPATCH=1` reverts to the per-slot loop.
- **Not on nifty:** `MC2_STATIC_PROP_DEPTH_PREPASS` (foliage depth-prepass) — lives on the
  `model-override` branch, not here.

## 4. Mechs — `.ase → Mech3DAppearance → GPU mech batcher`

**Status: GPU mech batcher is DEFAULT** (`MC2_GPU_MECHS` ON). CPU `mechShape->Render(true)` is the
fallback. Animated, articulated units get per-node bone matrices.

```
<mech>.ini + <name>.ase (mc2srcdata/tgl)   [optional GLB via Assimp, opt-in [Import]]
        │ load
        ▼
Mech3DAppearanceType::init → LoadTGMultiShapeFromASE + registerTypeLod   (mclib/mech3d_render.cpp:103 / 385)
BattleMech::init → new Mech3DAppearance::init/initFX                     (mech.cpp:1328)
        │ per-frame
        ▼
Mech3DAppearance::updateGeometry()         (mclib/mech3d_render.cpp:723)
   anim/node transforms, LOD select, paint-scheme (team color) bake, terrain light sample,
   heat/damage MechVisualState, CacheGpuLightData (dedup light slot)
        │ enqueue
        ▼
render() → submitActor()  → per-node bone matrices (shapeToWorld rows) + texture handles
                            into PendingSubmits           (mech3d.cpp:2675 → GameOS/gameos/gos_mech_batcher_geometry.cpp:161)
        │ draw — inside renderLists ("Render.GpuMechs", mclib/txmmgr_lightrow.cpp:205)
        ▼
GpuMechBatcher::flush()
   instance SSBO (binding 0), bone SSBO (binding 1), material SSBO (binding 2),
   LightsData SSBO (binding 0), ViewUniforms UBO (binding 3)
   → applyPipeline(MechOpaque) → glDrawElementsInstancedBaseVertex per bucket   (gos_mech_batcher.cpp:1730 / 1960)
        ▼
RENDERED BY: mech.vert  +  mech.frag
   (per-node rigid/skinned bone transform, Stuff→GL axis swap, calc_light, sun-search specular,
    hemisphere ambient, glass/cockpit mask, fog, GBuffer1 normal)
```

- **Gates:** `MC2_GPU_MECHS`, `MC2_MECH_VIEWUNIFORMS`, `MC2_MECH_SPECULAR_V1`
  (all DEFAULT ON); `MC2_MECH_NORMALS_MODE` (default 2 = angle-threshold smoothed); plus the
  `MC2_GPU_MECH_*` skip-slice cohort (leaf/sensor/shadow skips, all ON).

## 5. Vehicles & generic animated units — `.ase → GVAppearance → legacy ShapeRenderer`

**Status: DEFAULT path is legacy Spine A.** Vehicles look like mechs on disk but render through the
generic `ShapeRenderer` — **no** mech batcher, **no** bone SSBO, **no** skinning.

```
<name>.ase + <name>.ini  (referenced from FIT AppearanceName)
        │ load
        ▼
GVAppearanceType::init → LoadTGMultiShapeFromASE   (gvactor.cpp:151)
GroundVehicle::init → new GVAppearance::init       (gvehicl.cpp:940)
        │ per-frame (when inView)
        ▼
GVAppearance::updateGeometry → TransformMultiShape   (mclib/gvactor_render.cpp:195 / 2542)
   CPU vertex projection + per-vertex lighting + frustum clip; turret-node yaw;
   body pitch/roll from terrain; team color
        │ enqueue
        ▼
TG_Shape::Render → mcTextureManager->addRenderShape → masterHardwareVertexNodes[]   (txmmgr.cpp:764)
        │ draw — inside renderLists ("Render.3DObjects")
        ▼
ShapeRenderer::render → gos_RenderIndexedArray (glDrawElements)   (mclib/txmmgr_lights.cpp:692 / 1750)
        ▼
RENDERED BY: gos_tex_vertex_lighted.vert  +  gos_tex_vertex_lighted.frag
   (per-vertex SSBO lighting, diffuse × VertexLight, fog, GBuffer1 normal)
```

- **⚠️ Shader correction:** `ShapeRenderer` binds **`gos_tex_vertex_lighted`** (`txmmgr.cpp:1721`),
  NOT `mech.vert`. `mech.vert/.frag` is used *only* by the GPU mech batcher. This applies to the
  whole legacy ShapeRenderer family — vehicles, CPU-baseline mechs, animated buildings, legacy props.
  (The old matrix labeled these `mech.vert` — wrong.)
- **Shared with mech CPU baseline:** loader, `TransformMultiShape`, masterNode queue, renderLists /
  ShapeRenderer, light + scene SSBO/UBO, shadow-map bind, `inView` cull gate, GV-shadow-skip.
- **Diverges:** `GVAppearance` not `Mech3DAppearance`; node-rotation only (no skeleton); no GPU mech
  batcher / Track-D path.
- **Same path also serves:** `GenericAppearance` (gun emplacements, spotlights — though these *can*
  enter the static-prop-Generic batcher) and CPU-fallback mechs.
- **Gates:** `MC2_GPU_GV_SHADOW_SKIP` (ON), `MC2_GPU_CULL_LIFECYCLE`.
- Vehicle assets are **not deployed in v0.4** (source exists; `docs/asset-pipeline.md`).

## 6. VFX / particles (gosFX) — `mc2.fx → CardCloud oracle → GPU billboards`

**Status: GPU particle pipeline is OPT-IN** (`MC2_GPU_PARTICLES=1`; default OFF, legacy CPU/MLR; reverted 2026-06-15 4ad31a9e). Covers weapon FX, explosions,
smoke, trails, and "lights" (which are CardCloud billboards).

```
data/effects/mc2.fx  (binary gosFX stream: Fireball, Missile_Flare, PPC_Trail, ...)
        │ load
        ▼
EffectLibrary::Load → SpecLibrary   (mclib/txmmgr_texture.cpp:40, spec_library.cpp:53)
        │ spawn (weapon fire / explosion) + per-frame Effect::Execute (age, curves, physics)
        ▼
CardCloud/ShardCloud/PointCloud/Card::Draw()  ← oracle: harvest live particle state
   Batcher::Emit(GpuParticle 64B)             (cardcloud.cpp:491, batcher.cpp:202)
        │ after renderLists (gamecam.cpp:388)
        ▼
Batcher::Flush → gos_particle_bridge_flush()
   particle SSBO binding 14; per-group blend (additive GL_ONE / alpha)   (gos_particle_bridge.cpp)
   → glDrawArrays(GL_TRIANGLES, count*6)   (6 verts/particle, gl_VertexID-driven)
        ▼
RENDERED BY: particle_billboard.vert  +  particle_billboard.frag
   (camera-aligned billboard expansion + Stuff→GL swap, flipbook atlas, magenta colorkey discard,
    optional soft-particle depth fade + lit particles)
```

- **Still on legacy MLRClipper (CPU):** `Tube` (missile smoke / PPC ribbons — oracle `#if 0`),
  `Shape`, `DebrisCloud`, `ShapeCloud`, `PertCloud`.
- **Blend-state hazard:** the bridge bypasses the gos state cache, so it restores blend and calls
  `gos_InvalidateRenderStateCache()` (`memory/blend_state_inheritance_in_post_process.md`).
- **Gates:** `MC2_GPU_PARTICLES` (OFF), `MC2_VFX_ORACLE_RENDER` (OFF), `MC2_DISABLE_GOSFX` (kill all MLR FX),
  `MC2_VFX_SOFT_PARTICLES` / `MC2_VFX_LIT_PARTICLES` (OFF).

## 7. Sky, HUD/UI, shadows (cross-cutting)

### Skybox — HDRI EXR → fullscreen quad
**Status: HDRI is DEFAULT.** The old `theSky` GenericAppearance actor is instantiated but never drawn
(vestigial, per spec).
```
data/hdr/DaySkyHDRI063B_4K.exr → GameCamera::render → GameAdapters::Sky::renderHdri
 → pp->renderHdriSkybox() fullscreen quad into sceneFBO_   (gamecam.cpp:279, gos_postprocess.cpp:2008)
 RENDERED BY: hdri_skybox.vert/.frag (equirect world-ray sample). Gate: MC2_HDRI_SKY.
```
The gradient `skybox.vert/.frag` via `renderSkybox` exists but is never called in the main frame.

### HUD / UI / ImGui — 2D overlay at the near plane, drawn last
```
HUD sprites/fonts → gos_DrawQuads with gos_State_IsHUD=1, z=HUD_DEPTH=0.9999f (reverse-Z near)
 → deferred into hudBatch_ → replayed post-composite by gos_RendererFlushHUDBatch() (gameosmain.cpp:622)
ImGui (#ifdef MC2_IMGUI) → GuiRuntime::Render → ImGui_ImplOpenGL3_RenderDrawData()  fires LAST (:626)
```
- ImGui frame: `GuiRuntime::NewFrame()` (`gameosmain.cpp:1173`, before scene) → widgets in
  `GuiRuntime::Render()` → renders after the HUD batch. Gate: `MC2_IMGUI` (compile + runtime),
  `MC2_IMGUI_INSPECTOR`.
- `HUD_DEPTH=0.9999f` is load-bearing for reverse-Z (`memory/hud_depth_reverse_z_fix.md`).

### Shadows — feeds all lit color shaders
```
Caster gather (txmmgr.cpp renderLists):
  • terrain patches  • static buildings (getBuildingShadowInstances, opt-in)
  • dynamic props (getDynamicPropShadowInstances, dirty-only cached)  • mechs/vehicles (flushShadow)
        ▼
Shadow-map draw (forward-Z while scene is reverse-Z):
  static one-shot 4096² shadowFBO_  → shadow_terrain.* + shadow_static_prop.vert + shadow_instanced.frag
  per-frame dynShadowFBO_           → shadow_static_prop.vert / shadow_mech.vert + shadow_instanced.frag
        ▼
Sampled by color shaders via calcShadow()/calcDynamicShadow() (Poisson PCF, shaders/include/shadow.hglsl)
  in: gos_terrain.frag, gos_tex_vertex_lighted.frag (mechs/vehicles), gos_grass.frag, decal.frag,
      shadow_screen.frag
```
- Shadow projection uses `worldToClipGL()`, not the D3D matrix
  (`memory/shadow_dynamic_projection_and_caster_feed_fixed.md`).
- **Gates:** `MC2_SHADOW_ENABLE`, `MC2_SHADOW_DYNAMIC_PROP_CASTERS` (ON),
  `MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY` (ON), `MC2_STATIC_PROP_BUILDING_SHADOW` (OFF).

---

## Cross-cutting infrastructure

- **Shared TGL loader:** `TG_TypeMultiShape::LoadTGMultiShapeFromASE` (`mclib/msl_load.cpp:441`) compiles `.ase`
  → `.tgl` binary cache; `LoadFromFile` probes `.glb` (Assimp) first.
- **Lights SSBO:** `TG_HWLightsData[]` uploaded once/frame in `mclib/txmmgr_lights.cpp:587`; static-prop prefix is
  split (dirty-only) from the per-frame suffix (`MC2_STATIC_LIGHT_UPLOAD_SPLIT`). Indexed by
  `lightDataIndex`. Lighting is a mission-load constant — no dynamic emitters
  (`memory/lighting_is_mission_load_static_no_dynamic_emitters.md`).
- **Render-state cache:** any path mutating GL state outside `applyRenderStates` must call
  `gos_InvalidateRenderStateCache()` at the end (terrain bridge, batcher flushes, particle bridge,
  overlays/decals). See `memory/render_state_change_cost_hierarchy.md`.
- **Cull gates are load-bearing:** `inView`/`canBeSeen`/`objBlockInfo` also gate object update and
  pool lifecycle — bypassing cascades into stale state / pool exhaustion
  (`memory/cull_gates_are_load_bearing.md`).
- **GBuffer1:** color shaders emit `rc_gbuffer1_screenShadowEligible(normal)` at attachment 1 for the
  screen-space shadow / SSAO passes.

## What's NOT on nifty (lives on other branches)

- Tree 2-card **impostor far-LOD** and the **glTF model-override** system → `model-override` branch.
- **`MC2_STATIC_PROP_DEPTH_PREPASS`** (foliage depth-prepass) → `model-override` branch.
- **Track-D GLB mech pipeline at scale** → the `[Import]` Assimp hook exists, but stock mechs use `.ase`.

## Maintenance

Refresh this doc in place when a path's default flips, a fast path ships/retires, or a shader file is
added/renamed. Re-grep the most-cited symbols and bump the `Verified:` date in the header. Frozen
snapshots (the `observations/2026-*` series) are never edited — supersede them with a banner instead.

Cross-references: `docs/render-contract.md` (submission-space contracts) ·
`docs/render-perf-snapshot.md` (bucket map + slice state) ·
`observations/2026-05-14-render-pipeline-matrix.md` (cross-cutting concerns + pattern families) ·
`.planning/codebase/ARCHITECTURE.md` (higher-level frame flow) ·
`memory/INDEX-RENDERING.md` / `INDEX-TERRAIN.md` / `INDEX-MECH.md`.
