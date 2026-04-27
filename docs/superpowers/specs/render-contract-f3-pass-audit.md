# Render Contract F3 — Pass Audit (Q1–Q3)

**Status:** Audit complete (per §6 of [F3 design spec v2](2026-04-26-render-contract-f3-mrt-completeness-design.md))
**Date:** 2026-04-26
**Branch:** `claude/nifty-mendeleev`

This document answers Q1–Q3 of §1 of the F3 design:

- **Q1.** Which passes can depth-overdraw a previous `GBuffer1` writer?
- **Q2.** Which of those overdrawing passes produce pixels `shadow_screen.frag` consumes?
- **Q3.** For each such pass, what `GBuffer1` value correctly describes its surface?

The classification (Group I / Group II-Opaque / Group II-Blend / Excluded) below is the input both Option A and Option Hybrid will use during implementation.

---

## 1. Frame draw order (verified at HEAD `5256659`)

Source: [`gameosmain.cpp:395-484`](../../../GameOS/gameos/gameosmain.cpp).

```
pp->beginScene()                         // bind sceneFBO, glDrawBuffers(2)
glClear(COLOR | DEPTH)                   // clears both attachments to glClearColor (alpha=1)
glClearColor(<sky color>, 1.0f)          // alpha=1
glClear(COLOR | DEPTH | STENCIL)         // re-clears both attachments
[skybox.frag is currently DISABLED — line 459 commented out]
Environment.UpdateRenderers()            // ALL world-space draws into sceneFBO
pp->endScene()                           // post-process: shadow_screen, bloom, ssao, godrays, FXAA, tonemap → composites to FB 0
projectz_overlay_render(...)             // drawn to FB 0 AFTER composite
gos_RendererFlushHUDBatch()              // HUD/text drawn to FB 0 AFTER composite
```

**Critical implication.** Anything drawn *after* `pp->endScene()` is on the default framebuffer (FB 0), not the scene FBO, and never interacts with `GBuffer1`. That puts HUD/text, projectZ debug overlay, and any other late post-composite draw outside F3's scope entirely.

The scene-FBO draw window is therefore exactly: **`pp->beginScene()` → `Environment.UpdateRenderers()` body → `pp->endScene()` boundary**. Every shader F3 must classify lives inside `Environment.UpdateRenderers()`.

---

## 2. Shader inventory (full)

All `.frag` files in `shaders/`:

| Shader | Declares `layout(location=1)` | Bound on scene FBO? | Phase classification |
|---|---|---|---|
| `gos_terrain.frag` | yes | yes | **Group I** |
| `terrain_overlay.frag` | yes | yes | **Group I** |
| `decal.frag` | yes | yes | **Group I** |
| `gos_grass.frag` | yes | yes | **Group I** |
| `static_prop.frag` | yes | yes | **Group I** |
| `gos_vertex.frag` | no | yes | **Group II-Opaque (debug)** — see §3.6 |
| `gos_vertex_lighted.frag` | no | yes | **Group II-Opaque (rare)** — see §3.5 |
| `gos_tex_vertex.frag` (non-overlay variant) | no | yes | **Group II-Opaque** — see §3.4 |
| `gos_tex_vertex.frag` (IS_OVERLAY variant) | no | yes | **Group II-Blend** — see §3.4 |
| `gos_tex_vertex.frag` (gosFX particle path) | no | yes | **Group II-Blend / Excluded** — see §3.7 |
| `gos_tex_vertex_lighted.frag` | no | yes | **Group II-Opaque (PRIMARY)** — see §3.3 |
| `object_tex.frag` | no | **NOT REFERENCED IN PRODUCTION** | **Excluded (vestigial)** — see §3.8 |
| `gos_text.frag` | no | **drawn AFTER endScene to FB 0** | **Excluded** — see §3.9 |
| `skybox.frag` | no | **disabled (commented out)** | **Excluded** — see §3.10 |
| `shoreline.frag` | no | post-process pass | **Excluded** — runs inside `endScene` on a different FBO |
| `overlay_alpha_clear.frag` | no | post-process pass | **Excluded** — fullscreen quad with stencil; writes flat-up sentinel `(0.5,0.5,1.0,0.0)`; not a scene-draw shader |
| `bloom_threshold.frag`, `bloom_blur.frag` | no | post-process | Excluded |
| `ssao.frag`, `ssao_blur.frag`, `ssao_apply.frag` | no | post-process | Excluded |
| `godray.frag` | no | post-process | Excluded |
| `postprocess.frag` | no | post-process composite to FB 0 | Excluded |
| `shadow_depth.frag`, `shadow_terrain.frag`, `shadow_object.frag`, `shadow_debug.frag`, `shadow_screen.frag` | n/a | shadow FBOs / fullscreen pass | Excluded |

---

## 3. Group II analysis (per shader)

### 3.1 Terminology

- **Owns the visible pixel:** the shader's draw passes the depth test, writes `COLOR0`, AND writes depth (`glDepthMask(GL_TRUE)`). `shadow_screen.frag` will sample this surface's color and depth.
- **Contributes color but does not own pixel:** depth test passes, color is blended into `COLOR0`, but `glDepthMask(GL_FALSE)` keeps the depth at whatever surface owned it. `shadow_screen.frag` reads the underlying surface's depth and `GBuffer1`. Coherence is unaffected by this shader's writes.
- **Anticipated `GBuffer1`** under Option A: `rc_gbuffer1_screenShadowEligible(<normal>)`. Where no real surface normal exists, falls back to `vec3(0.0, 0.0, 1.0)` (flat-up). All flat-up migrations must be listed in the closing report (per design spec §3.1 conservative-normal-fallback rule).

### 3.2 Group I (already correct — listed for completeness)

All five Group I shaders write `GBuffer1` via registry helpers:

| Shader | Helper | Pass identity |
|---|---|---|
| `gos_terrain.frag` | `rc_gbuffer1_shadowHandled(N)` (and `rc_gbuffer1_legacyTerrainMaterialAlpha` in water/shoreline paths) | TerrainBase |
| `terrain_overlay.frag` | `rc_gbuffer1_shadowHandled_flatUp()` | TerrainOverlay |
| `decal.frag` | `rc_gbuffer1_shadowHandled` (verify during impl) | TerrainDecal |
| `gos_grass.frag` | `rc_gbuffer1_shadowHandled` (verify during impl) | Grass |
| `static_prop.frag` | `rc_gbuffer1_screenShadowEligible(N)` | StaticProp |

These are **not** modified by F3 under either option. They form the existing baseline.

### 3.3 `gos_tex_vertex_lighted.frag` — TGL world objects (PRIMARY Group II-Opaque)

**Material:** `basic_tex_lighted_material_` (loaded via [`gameos_graphics.cpp:1704`](../../../GameOS/gameos/gameos_graphics.cpp)). Selected via `selectLightedRenderMaterial` ([line 2207](../../../GameOS/gameos/gameos_graphics.cpp)) when `gos_State_Lighting=1` AND `gos_State_Texture!=0`.

**Callsite:** `drawIndexedTris.Lighted` at [`gameos_graphics.cpp:3038-3049`](../../../GameOS/gameos/gameos_graphics.cpp). Invoked by TGL `TG_Shape::Render` for mechs, buildings, vehicles. The shader has access to vertex normals (`Normal`, `WorldPos`) — a real per-vertex normal is available.

**Render state at draw time:** `gos_State_Lighting=1`, `gos_State_Texture!=0`. Depth test on, depth mask on (typical TGL state). `gos_State_AlphaTest` may be set for alpha-cutout textures.

**Overdraw:** YES — mechs/buildings/vehicles routinely overdraw terrain that has already written `GBuffer1.alpha=1.0`.

**Owns visible pixel:** YES — depth-writing.

**Sampled by `shadow_screen`:** YES — depth < 1.0 after composition, post-shadow expected to apply.

**Anticipated `GBuffer1`** (Option A): `rc_gbuffer1_screenShadowEligible(normalize(Normal))`. Real surface normal in world space. Not flat-up.

**Anticipated MRT scope** (Option Hybrid): bracket `drawIndexedTris.Lighted` with `enableMRT()`/`disableMRT()`. Tag `[RENDER_CONTRACT:requiresMRT Pass=TGLWorldObject_Lighted site=gosRenderer_drawIndexedTris]`. Relies on AMD's `vec4(0,0,0,0)` undeclared-output behavior to set `GBuffer1.alpha=0.0` on these pixels.

**Canary target** (§4 of design spec): this is the recommended canary shader. Highest pixel coverage, easiest to spot corruption.

### 3.4 `gos_tex_vertex.frag` — basic textured (Group II-Opaque, plus IS_OVERLAY and gosFX variants)

**Material:** `basic_tex_material_`. Selected via `selectBasicRenderMaterial` ([line 2188](../../../GameOS/gameos/gameos_graphics.cpp)) when `gos_State_Lighting=0` AND `gos_State_Texture!=0`.

The shader has three known callsite contexts that differ in render state:

#### 3.4.a Non-overlay textured world draws (Group II-Opaque)

**Callsite:** `drawIndexed` non-Lighted variant via `selectBasicRenderMaterial`. Some unlit textured 3D content (legacy paths). `IS_OVERLAY` flag NOT set.

**Overdraw:** likely yes if used for any 3D content. Audit confirmation required during Option A migration: identify which production draw paths route here without `IS_OVERLAY` AND without `gos_State_Lighting`.

**Owns visible pixel:** YES (depth-writing).

**Anticipated `GBuffer1`** (Option A): `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` (flat-up — no normal in this shader's varyings). **List in closing report's flat-up roster** for later normal-quality cleanup.

#### 3.4.b IS_OVERLAY variant — water and road overlays (Group II-Blend, conditional)

**Callsite:** `selectBasicRenderMaterial` with `rs[gos_State_Overlay]=1`, which selects the `IS_OVERLAY` shader variant. Used for water and road overlays per [`gameos_graphics.cpp:3015`](../../../GameOS/gameos/gameos_graphics.cpp) (water `isWater` uniform path) and the IS_OVERLAY bridge mentioned in [`docs/render-contract.md`](../../render-contract.md) Bucket D2.

**Render state:** typically `glDepthMask(GL_FALSE)` for overlays (decoration on top of terrain without owning depth) — **VERIFY during impl** by tracing `gos_State_ZWrite` for overlay submissions.

**Owns visible pixel:** likely NO (overlay convention is depth-test-yes-write-no).

**Anticipated treatment:** if depth-write is OFF, the underlying surface (terrain) owns the pixel; `GBuffer1` correctly describes the terrain. Coherence holds without modifying this shader. Classify as **Excluded with rationale** if verified.

If depth-write IS ON for any overlay path, that path migrates to Option A's `rc_gbuffer1_screenShadowEligible(flat-up)` (overlay surfaces are not "shadow-handled"; they should receive post-shadow like the underlying terrain does). **Flag for verification.**

#### 3.4.c gosFX particle path (Group II-Blend → Excluded under verified conditions)

**Callsite:** gosFX particles render via gos_DrawTriangles paths that route through `selectBasicRenderMaterial` (typically without lighting flag). Per worktree memory `MEMORY.md`: "*gosFX particle system: Data-driven via EffectLibrary. CardCloud/Tube/PertCloud/DebrisCloud. Renders through gos_tex_vertex.frag.*"

**Render state:** typical particle state — `glDepthMask(GL_FALSE)`, `glBlendFunc(...)` set to additive or alpha-blend. Particles do **not** own depth.

**Owns visible pixel:** NO. The underlying surface (terrain or world object) retains depth-buffer ownership.

**Anticipated treatment:** **Excluded.** `GBuffer1` correctly describes the underlying surface; particles' color is blended into `COLOR0` over that surface, which is the correct visual outcome. `shadow_screen.frag` darkens the underlying surface color (which includes the particle blend), producing the intended shadowed-particle look.

**Verification needed during impl:** confirm gosFX paths actually have `glDepthMask(GL_FALSE)` for all their submissions. If any particle submission writes depth, it joins Group II-Opaque and needs Option A's flat-up write or Option Hybrid's MRT scope.

### 3.5 `gos_vertex_lighted.frag` — lit untextured world objects (Group II-Opaque, rare)

**Material:** `basic_lighted_material_`. Selected via `selectLightedRenderMaterial` when `gos_State_Lighting=1` AND `gos_State_Texture=0`.

**Callsite:** same `drawIndexedTris.Lighted` path as 3.3. Production draws that hit this path are uncommon (most lit content is textured) but possible (e.g., debug-colored mech vertices in dev builds).

**Overdraw:** yes if any production path hits it.

**Owns visible pixel:** yes.

**Anticipated `GBuffer1`** (Option A): `rc_gbuffer1_screenShadowEligible(normalize(Normal))`. Real per-vertex normal available.

**Anticipated MRT scope** (Hybrid): same scope as 3.3 — `drawIndexedTris.Lighted` is shared.

### 3.6 `gos_vertex.frag` — basic untextured (Group II-Opaque, debug)

**Material:** `basic_material_`. Used directly by:

- `gos_DrawLines` ([gameos_graphics.cpp:2310](../../../GameOS/gameos/gameos_graphics.cpp))
- `gos_DrawPoints` ([gameos_graphics.cpp:2336](../../../GameOS/gameos/gameos_graphics.cpp))
- Selected via `selectBasicRenderMaterial` when `gos_State_Lighting=0` AND `gos_State_Texture=0`.

**Callsite distribution:** lines/points are typically used for debug visualization (projectZ-style overlays, navmesh debug, etc.) inside `Environment.UpdateRenderers()`. Some game content may use untextured colored verts (rare).

**Overdraw:** yes for any draw inside `Environment.UpdateRenderers()` that lands on top of terrain.

**Owns visible pixel:** depends on the render state of the caller. Lines/points typically draw with depth test on, depth mask on (default).

**Anticipated `GBuffer1`** (Option A): `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` (flat-up — no normal in vertex shader output). Flat-up is acceptable here because the visual content is debug-tier and post-shadow on debug lines is benign either way. **List in flat-up roster.**

**Note:** projectZ overlay (`projectz_overlay_render`) is drawn AFTER `endScene()` to FB 0 — it does NOT route through this shader on the scene FBO. (Different code path that binds FB 0 and uses its own state.) So `gos_vertex.frag` callsites are limited to in-scene-FBO debug draws, which are rare.

### 3.7 gosFX particle classification consolidation

See §3.4.c. Anticipated outcome: **Excluded** (particles don't own depth → underlying surface's `GBuffer1` is correct). Verify `glDepthMask(GL_FALSE)` for all gosFX submission paths during impl.

### 3.8 `object_tex.frag` — VESTIGIAL, not in production

**Reference search:** `grep -r "object_tex" GameOS/ mclib/ code/` returns **zero hits in source code**. The shader file exists at [`shaders/object_tex.frag`](../../../shaders/object_tex.frag) but is not loaded by any material registration in `gameos_graphics.cpp`. References to it in docs (`modding-guide.md:128`, `2026-04-13-gpu-projection-migration-design.md:149`) are either stale documentation or forward-looking plans that never landed.

**Classification:** **Excluded.** F3 does not modify it. A separate post-F3 cleanup follow-up may delete the file along with `object_tex.vert`.

**This resolves design spec OQ-3.** The body anomaly (`c + vec3(1,1,1)` saturating to white) is irrelevant to F3 because the shader is never bound.

### 3.9 `gos_text.frag` — drawn after composite, Excluded

**Material:** `text_material_`. Used by `gos_DrawTextString` → `text_->draw(text_material_)` ([gameos_graphics.cpp:3327, 3367](../../../GameOS/gameos/gameos_graphics.cpp)).

**Frame timing:** text submissions are buffered into `hudBatch_` during the scene-render phase but **not actually drawn until `flushHUDBatch()`**, which is invoked by `gos_RendererFlushHUDBatch()` at [`gameosmain.cpp:481`](../../../GameOS/gameos/gameosmain.cpp) — **AFTER `pp->endScene()`**. The flush replays buffered draws to the **default framebuffer (FB 0)**.

**Classification:** **Excluded.** Text never draws to the scene FBO and never interacts with `GBuffer1`. The registry callsite inventory's "likely MRT-bound" entry was wrong; it conflated submission timing with draw timing.

### 3.10 `skybox.frag` — currently disabled, Excluded

**Callsite:** `pp->renderSkybox(...)` at [`gameosmain.cpp:459`](../../../GameOS/gameos/gameosmain.cpp) — **commented out**. Per worktree memory `MEMORY.md`: "*Skybox disabled — terrain fog provides atmosphere*."

**Classification:** **Excluded** (not active).

If re-enabled in the future, skybox runs as a fullscreen quad before world draws. Coherence treatment depends on whether it writes depth (typically not for skyboxes — they fill the depth-far plane via depth-test-disable). Re-enabling skybox is out of F3 scope; the F3 implementation plan should record a follow-up to classify it if/when re-enabled.

---

## 4. Provisional pass classification (input to implementation)

### 4.1 Group I — registry-explicit writers (no F3 changes)

| Pass | Shader | Already correct |
|---|---|---|
| TerrainBase | `gos_terrain.frag` | yes |
| TerrainOverlay | `terrain_overlay.frag` | yes |
| TerrainDecal | `decal.frag` | yes |
| Grass | `gos_grass.frag` | yes |
| StaticProp | `static_prop.frag` | yes |

### 4.2 Group II-Opaque — depth-writing world objects (need F3 treatment)

| Pass | Shader | Callsite | Anticipated under Option A | Anticipated under Option Hybrid |
|---|---|---|---|---|
| TGLWorldObject_Lighted | `gos_tex_vertex_lighted.frag` | `drawIndexedTris.Lighted` (gameos_graphics.cpp:3038) | `rc_gbuffer1_screenShadowEligible(normalize(Normal))` | MRT scope around `drawIndexedTris.Lighted` |
| TGLWorldObject_LightedUntextured | `gos_vertex_lighted.frag` | same | `rc_gbuffer1_screenShadowEligible(normalize(Normal))` | (same scope as above; shared callsite) |
| BasicTextured_NonOverlay | `gos_tex_vertex.frag` non-overlay variant | `drawIndexed` via `selectBasicRenderMaterial` | `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` ⚠️ flat-up | MRT scope around the non-overlay path |
| BasicUntextured | `gos_vertex.frag` | `gos_DrawLines`, `gos_DrawPoints`, basic `drawIndexed` | `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` ⚠️ flat-up | MRT scope around lines/points draws |

⚠️ = flat-up fallback under Option A; must be listed in closing report's flat-up roster.

### 4.3 Group II-Blend — non-depth-writing (provisional Excluded, verify per-shader)

| Pass | Shader | Verification needed |
|---|---|---|
| Water/road overlay | `gos_tex_vertex.frag` IS_OVERLAY variant | confirm `glDepthMask(GL_FALSE)` for all overlay submissions |
| gosFX particles (CardCloud, Tube, PertCloud, DebrisCloud, etc.) | `gos_tex_vertex.frag` | confirm all gosFX paths have `glDepthMask(GL_FALSE)` |

If verification confirms depth-write is off, these become **Excluded** (no F3 changes — coherence holds via underlying surface). If any path writes depth, that path joins Group II-Opaque.

### 4.4 Excluded (no F3 changes, ever)

| Reason | Shaders/passes |
|---|---|
| Vestigial / unreferenced | `object_tex.frag`, `object_tex.vert` |
| Drawn to FB 0 after `endScene` | `gos_text.frag` (HUD/text), projectZ overlay (`projectz_overlay.cpp`), HUD draw replay |
| Currently disabled | `skybox.frag` |
| Post-process pass (different FBO or composite) | `shoreline.frag`, `bloom_*`, `ssao*`, `godray.frag`, `postprocess.frag`, `overlay_alpha_clear.frag` |
| Shadow pass (separate FBOs) | `shadow_depth.frag`, `shadow_terrain.frag`, `shadow_object.frag`, `shadow_debug.frag`, `shadow_screen.frag` |

---

## 5. Open verification items (resolve during implementation)

**V1.** Confirm `glDepthMask` state for IS_OVERLAY draws in `selectBasicRenderMaterial` callsites. If depth-write ON for any overlay path, promote that path from Group II-Blend to Group II-Opaque.

**V2.** Confirm `glDepthMask` state for all gosFX particle submission paths (CardCloud, Tube, PertCloud, DebrisCloud, ShapeCloud, Card, Shape, etc.). If any path writes depth, promote to Group II-Opaque.

**V3.** Confirm `gos_DrawLines` / `gos_DrawPoints` callsites inside `Environment.UpdateRenderers()` — are any of them so visually impactful that flat-up `GBuffer1` would noticeably change post-shadow application? (Expected: no; debug visualization tier.)

**V4.** Identify any `gos_DrawTriangles` callsite inside `Environment.UpdateRenderers()` whose render state is `(Lighting=0, Texture=1, Overlay=0)` — these route through `gos_tex_vertex.frag` non-overlay variant (Group II-Opaque). Confirm whether such paths exist in production or are vestigial.

**V5.** Confirm that `Environment.UpdateRenderers()` drains the per-frame buffer fully into scene-FBO draws by the time it returns. This is the assumption that anything not flushed before `endScene()` does not affect coherence. (Expected: yes; `gos_RendererEndFrame()` is called inside `UpdateRenderers` block.)

---

## 6. Inputs the audit provides to the canary

The §4 canary in the design spec selects `gos_tex_vertex_lighted.frag` (PRIMARY Group II-Opaque shader, §3.3) as the canary target. This audit confirms that choice:

- **Highest visual coverage** — every mech, building, and vehicle pixel passes through it.
- **Has a real per-vertex normal** — `rc_gbuffer1_screenShadowEligible(normalize(Normal))` is a meaningful test of Option A's full semantic, not the flat-up fallback.
- **Lifecycle is well-bounded** — single callsite (`drawIndexedTris.Lighted`), no early returns inside the draw path, easy to add and revert the shader change cleanly.

If the canary corrupts on this shader, Option Hybrid is the only safe path. If it does not corrupt, Option A is unblocked for this shader specifically; remaining Group II-Opaque shaders (§3.4.a, 3.5, 3.6) are migrated shader-by-shader with per-commit visual A/B per the design spec's commit sequence.

---

## 7. Inputs the audit provides to Option A implementation (if canary clean)

Order of shader migrations (highest pixel coverage first, easiest to audit visually):

1. `gos_tex_vertex_lighted.frag` — TGL world objects (canary already validated this).
2. `gos_tex_vertex.frag` non-overlay variant — basic textured 3D (V4 confirms which production paths exist).
3. `gos_vertex_lighted.frag` — lit untextured (rare; co-migrate with 1).
4. `gos_vertex.frag` — basic untextured / lines / points.

After commit 4, all Group II-Opaque shaders have explicit `GBuffer1` writes. Group II-Blend remains untouched (Excluded under verification V1-V2).

---

## 8. Inputs the audit provides to Option Hybrid implementation (if canary corrupts)

Per-pass MRT scopes:

- **Group I** (already): bracket each entry/exit point for `TerrainBase`, `TerrainOverlay`, `TerrainDecal`, `Grass`, `StaticProp`.
- **Group II-Opaque (incomplete shaders preserved):** bracket `drawIndexedTris.Lighted` (covers TGLWorldObject_Lighted and TGLWorldObject_LightedUntextured), bracket the non-overlay `drawIndexed` path, bracket `gos_DrawLines` / `gos_DrawPoints` interior draws if they overdraw terrain.
- **Group II-Blend / Excluded:** no MRT scope. They run in default `{COLOR0}` mode after F3.

---

## 9. References

- [F3 design spec v2](2026-04-26-render-contract-f3-mrt-completeness-design.md)
- [Render Contract Registry spec](2026-04-26-render-contract-registry-design.md)
- [Registry callsite inventory](render-contract-callsite-inventory.md) — predecessor; this audit deepens its §3.2 entries
- [`docs/architecture.md`](../../architecture.md) — frame draw order overview
- Frame entry: [`GameOS/gameos/gameosmain.cpp:395-484`](../../../GameOS/gameos/gameosmain.cpp)
- Material registration: [`GameOS/gameos/gameos_graphics.cpp:1704-1705`](../../../GameOS/gameos/gameos_graphics.cpp)
- Material selectors: `selectBasicRenderMaterial` ([line 2188](../../../GameOS/gameos/gameos_graphics.cpp)), `selectLightedRenderMaterial` ([line 2207](../../../GameOS/gameos/gameos_graphics.cpp))
- Lit object draw: `drawIndexedTris.Lighted` ([line 3038](../../../GameOS/gameos/gameos_graphics.cpp))
- HUD timing: `flushHUDBatch` ([gameos_graphics.cpp:3384](../../../GameOS/gameos/gameos_graphics.cpp)) called from `gos_RendererFlushHUDBatch` at gameosmain.cpp:481
- gosFX shader memory: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` "gosFX particle system" entry
