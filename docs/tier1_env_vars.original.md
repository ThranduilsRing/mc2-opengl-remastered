# Tier-1 instrumentation env vars

Extracted from CLAUDE.md 2026-05-24. Each env var here is a STABLE
instrumentation knob — opt-in (mostly default-off) probe that doesn't change
default behavior. Add new env-gated probes to this list when shipping.

Startup banner `[INSTR v1] enabled: ...` enumerates which probes fired at log
start. Grep schema versions with `\[SUBSYS v[0-9]+\]`.

## Feature gates (registered in RendererFeatureRegistry.h kFeatureTable)

- `MC2_COLORMAP_KTX2` — **COLORMAP-BC7-KTX2-1** (default **ON**). When a `.burnin.ktx2` BC7 sidecar is present alongside `.burnin.tga`, `BuildColormapAtlas` uploads via `glCompressedTexImage2D(GL_COMPRESSED_RGBA_BPTC_UNORM)` instead of `glTexImage2D RGBA8`. Saves ~81 MB VRAM/mission (108 MB -> ~27 MB). Falls back silently to RGBA8 when sidecar absent or BPTC cap missing. Kill-switch `=0` forces RGBA8 even with sidecar. Bake sidecars: `py -3 scripts/bake_colormap_ktx2.py`. Log: `[COLORMAP] COLORMAP-BC7-KTX2-1: uploaded ...`.
- `MC2_COLORMAP_CPU_RETIRE` — **COLORMAP-CPU-RETIRE-1** (default **ON**). Frees `cpuColorMap` + `cpuDispAlpha` after atlas upload so the CPU displacement block in `terrainElevation` (mapdata.cpp:2435) is dead. The indirect path has no geometry displacement; CPU grounding now matches visual. Kill-switch `=0` restores CPU copy. Probe: `MC2_COLORMAP_DISPLACE_PROBE=1` (requires `=0`).
- `MC2_VIEW_UNIFORMS` — ViewUniforms UBO upload + shader consumption (static_prop.vert). Default **ON**. `MC2_VIEW_UNIFORMS=0` — kill-switch, reverts to legacy `uniform mat4 u_worldToClipGL`. **Requires process restart** — shaders are compiled at startup. Upload log `[VIEW_UNIFORMS v1]` appears by default.
- `MC2_SHADOW_ENABLE=1` — opt-in: enable dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow). Default **OFF**. The old "terrain objects invisible with shadows on" regression is FIXED (registry flushed before flushShadow, commit 2764cb65/f04e3997); trees/fences/prop-buildings now render and cast shadows. v0.4 deploy needs a rebuild from current nifty to pick up the fix.
- `MC2_IMGUI=1` — enable ImGui overlay (GuiRuntime/GuiRuntime.cpp). Default OFF. Editor sets this automatically.
- `MC2_IMGUI_INSPECTOR=1` — enable ImGui inspector panel (GuiRuntime/EditorInspector.cpp). Default OFF. Requires `MC2_IMGUI`.
- `MC2_DEBUG_RENDERER=1` — enable debug overlay renderer (GuiRuntime/EditorInspector.cpp). Default OFF. Requires `MC2_IMGUI_INSPECTOR`.
- `MC2_STATIC_PROP_REGISTRY=1` — GpuStaticPropRegistry enable (default ON; editor sets `=0` via EditorMFC.cpp to bypass registry for edit-time mutations).

## Dynamic shadow tuning (SHADOW-BOUNDED-NEAR-FIT-1 / SHADOW-FRUSTUM-AUDIT-1)

`gosPostProcess::buildDynamicLightMatrix`. The dynamic sun-shadow ortho is already frustum-fit + pow-2 + texel-snapped, but the RTS camera saturates the full-map clamp every frame (~5.57 WU/texel over ~22808²). See `docs/shadow-frustum-audit.md`.

- `MC2_SHADOW_BOUNDED_NEAR_FIT=1` — opt-in: cap the frustum fit radius to a small camera-centered region for higher texel density. Default **OFF** (byte-identical full-frustum fit when off). Trades far-map shadow coverage for crisp near shadows. Applied before the pow-2/texel snap (snap preserved).
- `MC2_SHADOW_BOUNDED_NEAR_RADIUS=2500` — bounded near-fit radius in world units. Default 2500, clamped 512..mapClampR. Only consulted when `MC2_SHADOW_BOUNDED_NEAR_FIT=1`. Resolved once at process start. (radius 2500 → pow-2 snap to 4096 half-extent → ~2.0 WU/texel; radius ≤2048 → ~1.0 WU/texel.)
- `MC2_SHADOW_FRUSTUM_DIAG=1` — read-only per-frame coverage probe (sun dir, frustum XY, `fitRadius(orig=...)`, xyRadius, mapClampR, texel WU, ortho WxH, depth). Default OFF; no behavior change.
- `MC2_STATIC_PROP_BUILDING_SHADOW=1` — opt-in (SHADOW-STATIC-BUILDINGS-2): replay **all** registered rigid-building recipes into the world-fixed static shadow map, once per mission at the static-map build, for stable far-field building shadows. Source = the **full registry** (`GpuStaticPropRegistry::getBuildingShadowInstances`, visibility-independent — NOT per-frame buckets, which only hold the camera-visible subset; that was the failed Option B). **Trees excluded** (per-recipe Building population filter). Default **OFF**. Independent of `MC2_SHADOW_ENABLE` (static map builds regardless). `=2` adds a `[SHADOW_STATIC_BLDG v1] recipes_in/types/inst/draws` trace. Relies on C-pre min-combine (`7ea32b83`) so near buildings in both the static and dynamic maps don't double-darken terrain. Static map ~4.64 WU/texel (coarse — far field; near crispness = dynamic bounded-near map). **Debt:** destroyed buildings keep a stale static shadow until mission reload (destruction-invalidation deferred); sun frozen at build.
- `MC2_SHADOW_DYNAMIC_PROP_CASTERS` — **DEFAULT ON** (SHADOW-DYNAMIC-PROP-CASTERS-1): feed the **dynamic** sun-shadow caster pass from the **full registry** (`GpuStaticPropRegistry::getDynamicPropShadowInstances`, visibility-independent) instead of the camera-visible `s_typeRanges` set, which only admitted props near the camera (so distant trees never cast into the now-correctly-camera-fit dynamic map; the projection fix `a365e6ad` exposed this). `flushShadow` is skipped for static props (registry replay replaces it). **Building handling:** when the static building map is active (`MC2_STATIC_PROP_BUILDING_SHADOW`) buildings are EXCLUDED from this feed (they cast crisply via the static map — no fuzzy double-shadow); otherwise buildings are INCLUDED so they still cast a dynamic shadow (no regression under bare `MC2_SHADOW_ENABLE`). Only takes effect when `MC2_SHADOW_ENABLE` is set. **Kill-switch `MC2_SHADOW_DYNAMIC_PROP_CASTERS=0`** → legacy camera-visible `flushShadow` feed. `=2` adds a `[SHADOW_DYN_PROP v1] recipes_in/types/inst/draws` trace. **Debt:** no light-box cull yet (draws all map props every frame; HZB planned).

## Always-on background / safety

- `MC2_TGL_POOL_TRACE=1` — per-frame TGL pool NULL trace; monotonic summary every 600 frames always-on
- `MC2_DESTROY_TRACE=1` — per-destruction cull/lifecycle snapshot
- `MC2_GL_ERROR_DRAIN_SILENT=1` — suppress first-error prints (PRINT-ON by default; drain loop always runs)
- `MC2_ASSET_SCALE_TRACE=1` — per-key runtime lookup events; first `oob_blit` per `(path, callerTag)` always-on
- `MC2_ASSET_SCALE_SELFTEST=1` — synthetic 2x/4x/8x/1.5x golden tests at startup
- `MC2_HEARTBEAT=1` — stderr `[HEARTBEAT]` per second; detect renderer freezes during mod load
- `MC2_REVERSE_Z_TRACE=1` — `[REVERSE_Z v1]` lifecycle prints (projection-matrix build + inverseProjectZ fence-seam first use)
- `MC2_GL_DEBUG_FATAL=1` — `abort()` on `GL_DEBUG_SEVERITY_HIGH`; opt-in safety net for smoke (Tier 1.2)

## RenderWorld arc (M1.5..M2.6 substrate + pickup)

- `MC2_OBJECT_ID_BUFFER=1` — enable R32_UINT MRT attachment-2 substrate (M1.5; required by all object-ID consumers)
- `MC2_RENDER_WORLD_TRACE=1` — per-frame + per-event RenderWorld trace (always-on banner stays; this enables verbose events)
- `MC2_MECH_OBJECT_ID_SELFTEST=1` — M2.5 mech-substrate validator (emits `[MECH_OBJECT_ID_SELFTEST v1] result=PASS|FAIL`)
- `MC2_MECH_PICK=1` — enable mech-pick consumer (M2.6); requires `MC2_OBJECT_ID_BUFFER=1`
- `MC2_MECH_PICK_DEBUG=1` — verbose mech-pick logging (M2.6)
- `MC2_MECH_PICK_PIERCE_FOG=1` — debug-only fog bypass (M2.6); respect fog by default
- `MC2_MECH_PICK_SELFTEST=1` — M2.6 pickup validator (emits `[MECH_PICK_SELFTEST v1] result=PASS|FAIL`)
- `MC2_STATIC_PROP_PICK=1` — enable static-prop pick consumer (M1.6); requires `MC2_OBJECT_ID_BUFFER=1`
- `MC2_STATIC_PROP_PICK_DEBUG=1` — verbose static-prop pick logging (M1.6)
- `MC2_GAMEPLAY_PICK_SELFTEST=1` — M2-pre spine validator

## EditorBridge

- `MC2_EDITOR_MODE` — default `0`. Set to `1` to activate the `EditorBridge` API surface. All `EditorBridge::*` functions are no-ops when `0`. Must be combined with `MC2_OBJECT_ID_BUFFER=1` for full GPU pick support (terrain raycast still works without it). Process-lifetime cached at `EditorBridge::init()`, which the mission editor must call explicitly — mc2 game startup does NOT auto-call it.

## Render-contract assert (Phase 2)

- `MC2_RENDER_CONTRACT_ASSERT=1` — stderr `[RENDER_CONTRACT v2] assert mode ACTIVE` on startup; runtime queries live GL draw-buffer/depth state and compares to `render_contract::stateContractFor()` declared expectation. Inits once after `glewInit` in `gameosmain.cpp`. Phase 2 shipped 2026-05-24 (commit `137dc70`).

## Pre-commit invariant scripts

Run if you touched the area:

- Object lifecycle: `sh scripts/check-destroy-invariant.sh`
- UI icon atlas / `code/mechicon.cpp`: `sh scripts/check-asset-scale-callers.sh`
- Shader ABI (SSBO/UBO layout): `cmake --build build64 --target shader_schema`
  - Trigger: touched `RenderCore/MaterialGpu.h`, `gos_mech_batcher.h`, their GLSL
    mirrors, or `tools/shader_schema/manifest.json`
  - Pass: `[SHADER_SCHEMA v1] PASS interfaces=2`
  - Failure example (field offset drift): `[SHADER_SCHEMA v1] FAIL interface=GpuMechInstance field=materialIdx cppOffset=52 glslOffset=48`
  - Failure example (size mismatch): `[SHADER_SCHEMA v1] FAIL interface=MaterialGpu no SSBO with array_stride=32 in ...`
  - To add an interface: edit `tools/shader_schema/manifest.json`, run
    `py -3 tools/shader_reflect/reflect.py --update` if adding a new fixture,
    then verify `shader_schema` passes

## Firewall / no-raw-GL / VFX-no-objectId

Three CI scripts that lock the RenderWorld arc invariants — see `docs/renderworld_arc_status.md` § "CI / enforcement layer" for the full inventory.

- `sh scripts/check-include-firewall.sh` — SCOPE_DIRS layering
- `sh scripts/check-no-raw-gl-from-game.sh` — game-side raw GL prohibition (M6)
- `sh scripts/check-vfx-no-objectid.sh` — VFX attachment-2 prohibition (M4)

## MaterialKtx sidecar (Phase 0)

- `MC2_MATERIAL_KTX=1` — KTX2 sidecar loader: try `.ktx2` alongside `.tga` when building static-prop texture array. Phase 0: RGBA8 (VK_FORMAT_R8G8B8A8_UNORM/SRGB) only; compressed/supercompressed formats return false silently. Default OFF. Requires `MC2_COALESCE=1`. Implementation: `RenderCore/KtxLoader.{h,cpp}`. Call site: `RenderCore::ktxLoadRgba8(path, out)` in `gos_static_prop_batcher.cpp`.

## MC2_MATERIAL_GPU

Default: **ON** (set `MC2_MATERIAL_GPU=0` to disable).

Controls static-prop MaterialGpu table upload, SSBO bind, and compare validation.
When enabled, every flush validates `materials[materialIdx].albedoTex == texArrayLayer` (load-bearing invariant).

## MC2_MATERIAL_GPU_SAMPLE

Default: **ON** (set `MC2_MATERIAL_GPU_SAMPLE=0` to disable shader sampling).

Routes static-prop albedo through the MaterialGpu table in `static_prop.frag`.
Set `=0` to fall back to `texArrayLayer` (legacy fallback / compare authority).
Requires `MC2_MATERIAL_GPU` also active (sampleOn gate checks both).

Log tag: `[MATERIAL_GPU v4]`

## StaticPropOpaque visual gates

- `MC2_STATIC_PROP_AMBIENT_V1=1` - enable the gated hemisphere ambient fill in `static_prop.vert`. Default **OFF**; unset or `=0` uploads `u_ambientV1Strength=0.0`, preserving the pre-ambient path. Window-flag nodes skip this term.
- `MC2_STATIC_PROP_IBL_SH` - gate SH-L2 image-based ambient in `static_prop.vert`. Default **ON**; set `=0` as the explicit kill switch. When disabled, `u_iblShStrength=0.0` and the shader short-circuits before evaluating SH. Coefficients come from `RenderCore/IblShCoeffs.h`; current set selection is shown in ImGui.
- `MC2_STATIC_PROP_IBL_SH_STRENGTH=<f>` - optional default-strength override for SH-L2 ambient (clamped 0..3). Sets the initial `g_iblShStrength` ImGui slider value. Only contributes when `MC2_STATIC_PROP_IBL_SH` is active. Unset/empty -> default 0.5.
- `MC2_STATIC_PROP_DEBUG_MATERIAL=N` — static-prop fragment debug view. Default **0 (off)**. Values 1-6 select a debug mode for the StaticPropOpaque lane. ImGui inspector (when active) can override at runtime. Shader branch numbers: 0=Final, 1=Albedo, 2=MaterialIdx, 3=Normal, 4=TexArrayLayer, 5=Roughness, 6=Metallic. Canonical labels from `RenderCore/RenderDebugView.h`. See DEBUG-VIEW-REGISTRY-1.

## V-MATERIAL-PBR-3 (per-fragment specular)

- `MC2_STATIC_PROP_PBR_V1=1` — gate the per-fragment Schlick-Fresnel + power-lobe specular on StaticPropOpaque lane (`static_prop.frag`, inside `#if defined(MC2_USE_VIEW_UNIFORMS)`). Default **OFF**. When OFF, `u_pbrV1Strength` uploads 0.0 -> shader `if (u_pbrV1Strength > 0.0)` short-circuits before any `u_cameraWorldPos` read. When MaterialGpu sampling is active, PBR reads `roughnessFactor` and `metallicFactor`; otherwise it falls back to `metallic=0.0`, `roughness=0.6`. F0 is albedo-tinted for metallic materials. Sun detection accepts both `TG_LIGHT_INFINITE` and `TG_LIGHT_INFINITEWITHFALLOFF`. Window/hot-pink nodes bypass PBR. **Gate-ON can still add broad or blown highlights on flat legacy roofs**; this is expected while PBR remains experimental and material masks are sparse.
- `MC2_STATIC_PROP_PBR_V1_STRENGTH=<f>` — optional default-strength override (clamped 0..3). Sets the initial `g_pbrV1Strength` slider value. ImGui slider may still override at runtime. Only meaningful when `MC2_STATIC_PROP_PBR_V1=1`. Unset/empty → default 1.0.
- `MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND=1` — diagnostic visualizer for the forwarded sun-found state. Only meaningful with `MC2_STATIC_PROP_PBR_V1=1`; paints cyan when a supported infinite sun light was found and magenta when not found.

## Debug state dump (DEBUG-STATE-DUMP-1)

- `MC2_DEBUG_STATE_DUMP=1` — write a JSON render-state snapshot every 300 frames (and at frame 1) to `debug_state/latest_render_state.json` relative to the working directory. Snapshot includes: feature gate states, `RenderSnapshot` ok/mismatch counters, `EngineView` state, and `StaticPropOpaque` visual globals. **Read-only; no renderer or gameplay changes.** Default **OFF**. See `docs/debug_state_dump.md`.
- `MC2_DEBUG_STATE_DUMP_DIR=<path>` — override the output directory for the JSON snapshot. No effect when `MC2_DEBUG_STATE_DUMP` is unset.
- `MC2_DEBUG_STATE_DUMP_HISTORY=1` — enable rolling 8-slot history ring. Requires `MC2_DEBUG_STATE_DUMP=1`. Each write also produces `history_0.json`..`history_7.json` in the output directory (oldest slot overwritten). Bounded to 8 files. Default **OFF**.

## Terrain debug views (TERRAIN-DEBUG-VIEWS-1)

- `MC2_TERRAIN_DEBUG_MODE=N` — terrain fragment-shader debug-mode selector for the tessellated terrain path (`gos_terrain.frag` `tessDebug.x`). When set, overrides the runtime `terrain_debug_mode_` member at all three GL upload sites in `gameos_graphics.cpp`. Default unset = mode 0 (off, byte-identical to legacy output). **Visual modes:** 1=DepthComparison, 2=RawColormap, 3=BlurredColormap, 4=MaterialWeights (R=rock,G=grass,B=dirt), 5=NormalLighting, 6=ShadowFactor, 7=CloudShadow. **Diagnostics:** 8=CementDiag, 9=ThinRecordDiag, 10=HeightDerivedNormal (TERRAIN-NORMALS-FROM-HEIGHT-1; black if height texture not yet uploaded), 11=HemiAdditive (TERRAIN-LIGHTING-2; visualizes the V1 hemisphere ambient contribution ×4 after V2 shadow-floor modulation; black when V1 OFF), -1=TessAliveProbe. Diagnostic-only; no gameplay, correctness, or default visual effect. Runtime equivalent: `gos_SetTerrainDebugMode()` C-API, Surface Debug Mode picker in GraphicsOptionsWindow, mini-control in the Terrain Pass inspector panel. Full mode table: `GuiRuntime/GraphicsOptionsWindow.cpp` `kTerrainModes`.

## Terrain visual improvements (TERRAIN-NORMALS-FROM-HEIGHT-1)

- `MC2_TERRAIN_NORMALS_FROM_HEIGHT=1` — derive a macroscopic terrain surface normal from the per-mission R32F height texture (uploaded once at mission load from `MapData` blocks by `GameOS/gameos/gos_terrain_height_tex.cpp`; sampler unit 11) and add it into the terrain detail-normal local frame in `gos_terrain.frag`. Default **OFF**; when unset/`=0`, `useTerrainNormalsFromHeight` uploads 0 and the shader branch is skipped — byte-identical legacy output. **Visual-only**: gameplay height (`Terrain::getTerrainElevation`) is unchanged; no geometry, vertex position, pathfinding, collision, or unit placement is moved. Inspector mini-control in the Terrain Pass panel shows current effective state. Visualize the height-derived normal directly via `MC2_TERRAIN_DEBUG_MODE=10` (independent of this gate so the upload path can be diagnosed separately).
- `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR=N` — CPU bilinear resample factor for the per-mission terrain height texture (TERRAIN-RESAMPLE-1). Accepted values **1, 2, 4** (anything else clamps to 1). Default **1** (byte-equivalent to pre-slice). Render texture side becomes `(sourceSide-1)*factor + 1`; source samples preserved EXACTLY at factor-multiple positions. Read at each `gos_uploadTerrainHeightTex()` call (i.e. per mission load); toggling mid-mission does not re-upload. **Affects only normal derivation** (and debug mode 10) — gameplay height, geometry, displacement: all unchanged. Memory: 4× factor on a 120² source ≈ 890 KB; bounded by source-grid × 16. Inspector shows source/render/factor.
- `MC2_TERRAIN_LIGHTING_V1=1` — enable hemisphere ambient fill on tessellated terrain (TERRAIN-LIGHTING-1). Default **OFF**; when unset/`=0`, `terrainLightingV1Strength` uploads 0.0 and the `gos_terrain.frag` additive branch is skipped — byte-identical legacy output. Fill is sky/ground tinted, added AFTER shadow multiplication so shadowed terrain still receives sky bounce. Strength tunable via Graphics Options terrain section (member-default 1.0; env gate authoritative on/off). Best paired with `MC2_TERRAIN_NORMALS_FROM_HEIGHT=1` so the sky term tracks the height-derived surface verticality. **Visual-only**; no gameplay, geometry, or collision change.
- `MC2_TERRAIN_LIGHTING_V2=1` — enable shadow-aware modulation of the TERRAIN-LIGHTING-1 hemisphere fill (TERRAIN-LIGHTING-2). Default **OFF**. When OFF, `terrainLightingV2ShadowFillFloor` uploads 1.0 → mix(floor,1.0,shadow) collapses to 1.0 → V1 unmodulated (byte-equivalent to V1 alone). When ON, the member floor (default 0.3, slider 0..1) scales the hemi additive down in shadowed terrain so shadows stay readable. `floor=0.3` = 30% hemi in fully shadowed, 100% in fully lit. Debug mode `MC2_TERRAIN_DEBUG_MODE=11` visualizes the hemi additive contribution as RGB (×4 for visibility). Effective only when `MC2_TERRAIN_LIGHTING_V1=1` is also set. **Visual-only**.

## Water visual: gated sky tint (WATER-VISUAL-FIRST-SLICE)

- `MC2_WATER_SKYTINT=1` — enable the **gated, camera-INDEPENDENT sky/horizon tint** on the MDI water FS (`gos_terrain_water_mdi.frag`). Default **OFF**: when unset/`=0`, `u_waterSkyTintStrength` resolves to **0.0** → `mix(col, tint, 0)` is a no-op → byte-identical to pre-slice water. When set, the default strength becomes **0.15** for a quick A/B; the Graphics Options > Water "Sky Tint" slider/color are authoritative once touched (mirrors the terrain NfH gate+slider pattern). The tint is a constant additive pull of the water color toward `u_waterSkyTintColor` (default soft sky-blue `(0.55,0.70,0.85)`), applied before fog — **f(uniform color, strength) only**, no view angle. **NOT fresnel/reflection** (those stay shelved per the 2026-05-17 camera-independence ruling). Requires the MDI path (`MC2_GPU_DRIVEN_WATER=1`); the legacy FS ignores it. **Visual-only**; no gameplay. Tunable live in Graphics Options > Water.

## Water visual: gated sky reflection (WATER-SKY-REFLECTION-1)

- `MC2_WATER_REFLECTION=1` — enable the **gated, camera-DEPENDENT SH-L2 sky reflection** on the MDI water FS (`gos_terrain_water_mdi.frag`, `u_waterReflStrength`). Default **OFF**: when unset/`=0`, `gos_GetWaterReflStrength()` resolves to **0.0**; the reflection block is skipped (mode 0) → byte-identical. When set, default strength **0.15** (suggested 0.10–0.25). The reflection source is the **SH-L2 sky** (9 coeffs inlined from `RenderCore/IblShCoeffs.h`, projected from `data/hdr/DaySkyHDRI063B_4K.exr`) evaluated along the reflected view ray — a smooth, **orbit-stable** broad sky shape. This **replaces the shelved terrain-colormap reflection** (whose ground-color source caused a compass hue-swing). The reflected ray is built from `cameraPos`/`WorldPos` + an fBm-gradient wave normal; **MC2 Z-up → SH Y-up axis swap** applied before SH eval; Schlick-Fresnel + waveLOD weighted, capped (`REFL_MAX=0.30`). **Camera-dependence is intentional and permitted** — the 2026-05-17 "camera-independent" rule was superseded 2026-05-18 (rejection was quality, not principle). Requires the MDI path (`MC2_GPU_DRIVEN_WATER=1`); the legacy FS ignores it. Graphics Options > Water "Sky Reflection" slider is the live control (disabled when the env gate is OFF, so it cannot bypass the gate). Visualize the term via `MC2_WATER_DEBUG_MODE=7`. **Visual-only**; no gameplay. No render target / no terrain reflection (Phase A is shader-only).

## Water visual: terrain reflection RT fill (WATER-TERRAIN-REFLECTION-1)

- `MC2_WATER_REFLECTION_RT=1` — enable the **mirrored terrain-only fill** of the quarter-res water reflection render target (Phase C1). Default **OFF**: when unset/`=0`, `gos_terrain_indirect::RenderWaterReflectionPass()` is a no-op → byte-identical; the RT stays cleared/black. When set, after the main terrain `renderLists()` and before water, the pass installs a mirror MVP (`clip = G · reflect(z→2·waterElevation−z)`), re-dispatches the terrain SOLID compute into a fresh ring slot, draws terrain into `WaterReflectionColor/Depth`, then **restores the production terrain MVP** (load-bearing — water/decals/overlays/cull read it later the same frame). **Terrain only** — no water, props, mechs, or clip plane (deferred to WATER-REFLECTION-CLIP-1; expect below-waterline terrain + shoreline seam in the RT). **C1 does NOT sample the RT in the water shader** (that is C2, WATER-REFLECTION-SAMPLE-1) — the only visible effect is the Graphics Options > GBuffer Preview "Water Reflection" thumbnail going non-black. Requires the GPU-driven terrain + water paths armed (`MC2_GPU_DRIVEN_WATER=1`). Proof/diagnostics: `[WATER_REFL_RT v1]` log (throttled) reports `drew`, whole-RT `coverage%`, `max_luma`, `gl_err`. **C2 (WATER-REFLECTION-SAMPLE-1):** with this gate ON **and** `MC2_WATER_REFLECTION=1`, the MDI water FS samples this RT at the fragment's screen UV (wave-distorted) and blends valid terrain (`rtSample.a>0`) OVER the SH sky via `u_waterRtStrength` (default 0.85, Graphics Options > Water "Terrain Reflection RT"); where the RT is empty (alpha 0 — e.g. the steep gameplay camera's ~0% coverage) it **falls back to the SH sky** so water never goes empty. **Coverage finding:** terrain RT fills 67–85% at wide/intro cameras, ~0% at the steep gameplay camera (reflected terrain off-frustum) — the RT contribution is mainly for future lower/across-water cameras; the SH sky remains the primary at top-down. Debug modes 8 (RT sample) / 9 (reflection blend). **Visual-only**; no gameplay.

## Water debug views (WATER-DEBUG-VIEWS-1)

- `MC2_WATER_DEBUG_MODE=N` — fragment/material-space debug-mode selector for the **MDI water path** fragment shader (`gos_terrain_water_mdi.frag` `u_waterDebugMode`). Resolved once from the env into `g_waterFsDebugMode` (`gameos_graphics.cpp`); `gos_GetWaterFsDebugMode()` is the accessor, uploaded in the MDI bind block. Default unset = mode 0 (Final, byte-identical to legacy output). **Modes (each backed by a real water-v1 term):** 1=Tint (DEEP↔SHALLOW Beer-Lambert mix, pre-ripple), 2=Alpha (final `shore × WATER_MAX_ALPHA` as grayscale), 3=Normal (flat-up `(0,0,1)` only — water has no real surface normal; honest constant), 4=Depth (Beer-Lambert transmittance `exp(-WaterThickness·density)`; 1=shallow, 0=deep), 5=Shore (shoreline ramp mask), 6=Lighting (fBm ripple brighten + crest glint), 7=Reflection SH sky (SH-L2 sky term, pre-Fresnel/strength; WATER-SKY-REFLECTION-1), 8=Reflection RT sample (terrain reflection RT, black where invalid; WATER-REFLECTION-SAMPLE-1), 9=Reflection blend (final reflected color = SH sky ← terrain RT, pre-Fresnel). Unknown N → magenta sentinel. **Requires the GPU-driven/MDI water path to be armed** (`gpu_driven::IsWaterEnabled()` / `MC2_GPU_DRIVEN_WATER`); the legacy fast path (`gos_tex_vertex.frag`) ignores this uniform. Distinct from `MC2_RENDER_WATER_FASTPATH_DEBUG` (VS geometry-space). Read-only readout in the Terrain Pass inspector panel; live ImGui control deferred to WATER-TUNING-UI-1. Diagnostic-only; no gameplay or default visual effect.

## VFX debug views (VFX-DEBUG-VIEWS-1)

- `MC2_VFX_DEBUG_MODE=N` — GPU particle billboard fragment-shader debug-mode selector (`particle_billboard.frag` `u_debugMode`, uploaded per flush by `gos_particle_bridge.cpp`). Default unset = mode **0 (Final, byte-identical** to default output). **Modes:** 1=Albedo (raw atlas texel rgb, no vertex-color tint), 2=Alpha (final alpha as grayscale), 3=ParticleKind (distinct hashed color per `kind_flags` kind), 4=Overdraw (constant additive proxy to visualize blend buildup). Seeded once at process start (clamped 0..4). The **VFX Pass** Object-Inspector panel shows the active mode read-only (`gos_vfx_getDebugMode()`); the Graphics Options **VFX Tuning** combo overrides it live (`gos_vfx_setDebugMode()`). Diagnostic-only; **no gameplay, emission, lifetime, sorting, or default visual effect**; VFX object-IDs remain prohibited. RenderDebugView canonical mapping (`kDebugViewMask_Vfx`): Final→0, Albedo→1; modes 2-4 are VFX-local (no canonical enum slot). Requires `MC2_GPU_PARTICLES` enabled (default ON) so particles actually draw.
- `MC2_VFX_AGE_SAMPLE=1` — VFX-AGE-SAMPLE-1 (first visual). Sample routed GPU-particle spec curves (color/alpha/size/UV) at the effect's **real CPU-advanced normalized age** (`gosFX::Effect::m_age`, threaded into `mc2::particles::Spawn` from each producer `Draw`) instead of the fixed `0.5` midpoint. Default **OFF** → `resolveSampleAge()` returns `0.5` → **byte-identical** to pre-slice. When ON, particles regain fade-in/out + grow/shrink (each per-frame re-emit samples at the effect's current age). **Read-only** consumption of `m_age` (already gameplay-advanced) — no emission/lifetime/spawn-rate/timing change; **no shader or `GpuParticle` ABI change** (curve eval stays CPU-side in `spawn_*.cpp`). Invalid/sentinel (`m_age=-1`) or out-of-`[0,1]` → fallback `0.5`. Affects only the 5 routed classes (Card/CardCloud/PointCloud/ShardCloud/Tube); unrouted CPU-only classes untouched; object-ID invariant preserved. Read once at process start (env authoritative). Min/max age summary logged under `MC2_GPU_PARTICLES_LOG=1` (`[VFX_AGE_SAMPLE v1]`).
- `MC2_VFX_ORACLE_RENDER=1` — VFX-ORIGINAL-RECORD-ABI-1 (Phase 1 of the originals-restoration arc; see `docs/vfx-originals-restoration-design.md`). Default **OFF**. **Target classes = CardCloud + ShardCloud** (PointCloud was the original pick but probes showed it births 0 particles in stock tier1; CardCloud ~30 live and ShardCloud ~25 live are the populated classes). ShardCloud is harvested the same way (center=`m_localTranslation`, color=`m_P_color[i*3]` per-vertex, size=`m_scale*m_radius`; triangle shape `m_angle` deferred). When OFF, `CardCloud::Draw`/`ShardCloud::Draw` run the existing placeholder `Spawn()` path **byte-identically**. When ON, they harvest the CPU gosFX sim's live per-particle data (per-particle `m_localTranslation` + `m_scale` + `m_halfX/Y` and `m_P_color[i]`, advanced this frame by `ParticleCloud::Execute`/`CardCloud::AnimateParticle`) and emits one GPU billboard per **live** particle (`m_age<1`): position = `m_localToParent*parentToWorld * m_localTranslation`; color = `m_P_color[i]`; size = `scale*sqrt(halfX²+halfY²)`. Reuses the placeholder's texture/blend group. CPU sim stays **authoritative** (no sim change, no bypass); legacy MLR draw stays skipped (**no dual-draw**). CardCloud + ShardCloud only — PointCloud/Card/Tube/trails untouched. **No** GPU-sim, emission/lifetime/timing change, object-ID write, or shader/ABI change. **Deferred** (ABI-extension follow-up): per-particle rotation (`m_localRotation`), aspect, per-particle UV frame. Parity diagnostic `[VFX_ORACLE v1]` FIRST_HARVEST + 240-call summary under `MC2_GPU_PARTICLES_LOG=1`. Parity-bridge/oracle stage, **not** the final architecture (end-state = GPU sim + CPU-sim deletion per class).
- `MC2_VFX_GPU_SIM_CARDCLOUD=1` — VFX-GPU-SIM-CARDCLOUD-BUFFER-1 (Stage 2; see `docs/vfx-gpu-sim-spec.md`). Default **OFF**. When ON, `CardCloud::Draw` gathers the live CPU-sim per-particle state (position, world velocity, age, ageRate, color, size, alive flag; dead `m_age>=1` filtered) into a CPU-compacted list and submits it to `gos_cardcloud_sim_submit`, which uploads to a **persistent GPU sim SSBO** (`CardCloudSimParticle`, 64B std430). **OBSERVE-ONLY substrate**: no compute integration, no GPU→CPU readback, no rendering, no CPU-sim bypass. CPU sim stays authoritative; the frame is unchanged (independent of `MC2_VFX_ORACLE_RENDER`). **CardCloud only.** COMPUTE-1 adds the compute integration + readback + parity on this layout.
- `MC2_VFX_GPU_SIM_COMPARE=1` — emit `[VFX_GPU_SIM v1]` integrity/compare logs for the CardCloud GPU sim buffer (cpuActive vs submitted-live count, age/alpha ranges, world bounds, SSBO capacity). Requires `MC2_VFX_GPU_SIM_CARDCLOUD=1`. Default **OFF**. BUFFER-1 is a CPU-side plumbing check (no GPU readback → no stalls); real CPU-vs-GPU parity (maxPosError, age divergence) arrives in COMPUTE-1.
- `MC2_TUNE_VFX_BRIGHTNESS=f` / `MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS=f` / `MC2_TUNE_VFX_ALPHA_SCALE=f` — VFX-TUNING-UI-1 startup defaults for the GPU-particle intensity scales (`particle_billboard.frag` `u_vfxBrightness` / `u_vfxAdditiveBrightness` / `u_vfxAlphaScale`). Each clamped **0..8**; unset/empty → **1.0 (byte-identical no-op)**. Brightness = global RGB scale (all particles); additive-brightness = extra RGB scale on additive groups only (per-group `u_vfxIsAdditive`; highest-value lever for pre-bloom additive overdraw — see `docs/vfx-overdraw-audit.md`); alpha-scale = opacity (all particles). Seeded once at process start; the Graphics Options **VFX Tuning** sliders override at runtime (`gos_vfx_setBrightness/AdditiveBrightness/AlphaScale`) with Reset buttons. **Look-only** — no emission/lifetime/sorting/timing change. No effect when `MC2_GPU_PARTICLES=0`.

## Static-prop dispatch hierarchy (v7)

As of v7 the static-prop draw path is:

| Path | Trigger | Log tag |
|---|---|---|
| **Primary** — `DrawPacket[] + StaticPropDispatchMeta[]` | Default ON; `event=armed ... default=1` at process start | `[DRAW_PACKET_V6]` |
| **Fallback** — legacy `glMultiDrawElementsIndirect` coalesce | `MC2_STATIC_PROP_LEGACY_DISPATCH=1` | none |
| **Diagnostic** — v5 per-draw-call loop | `MC2_DRAW_PACKET_COALESCE_V5=1` (deprecated opt-in) | `[DRAW_PACKET_V5]` |
| **Historical** — v4A/v4B/v4C coverage probes | Removed in v7.1 | n/a |

`[DRAW_PACKET_V6]` is the live log tag for the primary dispatch path as of v7. It fires by
default — it is not an opt-in tag. The old `MC2_DRAW_PACKET_STATIC_PROP_V6` opt-in env var
is fully inert; gate plumbing removed in v7.1. Future path: v8 will normalize the tag to
`[STATIC_PROP_PACKET_DISPATCH v1]` if/when shadow-pass dispatch is added.

## DrawPacket v2 compare

- `MC2_DRAW_PACKET_COMPARE=1` — per-frame candidate vs batcher field check (summary log). Emits one `[DRAW_PACKET_COMPARE v1]` line per frame to stderr: `frame=%u packets=%u pipeline_invalid=%u pipeline_oob=%u geom_mismatch=%u type_mismatch=%u alpha_mismatch=%u`. Default OFF. Log tag `v1`; increment if fields are added/removed in future slices. Cached at process start (static initializer); must be set before launching mc2.exe.
- `MC2_DRAW_PACKET_COMPARE_VERBOSE=1` — also emit per-mismatch detail lines (`[DRAW_PACKET_COMPARE detail]`) for firstIndex, indexCount, owningType, and alpha disagreements. Requires `MC2_DRAW_PACKET_COMPARE=1` to have any effect (compare must be enabled for the per-candidate loop to run). Valid values: `1` to enable; omit or set to any other value to disable. Cached at process start (static initializer); must be set before launching mc2.exe.
- `MC2_DRAW_PACKET_V3=1` — enable DrawPacket v3 conversion + build log — diagnostic only; no GL state mutation, no pixel change. Emits one `[DRAW_PACKET v3]` line per frame to stderr with 9 build counters (input, emitted, invalid_pipeline, pipeline_oor, invalid_index, invalid_instance, overflow, object_sentinel, light_sentinel) plus sorted_packet_cap.
- `MC2_DRAW_PACKET_COALESCE_V5=1` — master gate for DrawPacket v5 substitutive per-draw-call dispatch. Replaces the two `glMultiDrawElementsIndirect` calls in `flush()` coalesce branch with a per-slot `glDrawElementsInstancedBaseVertexBaseInstance` loop. Requires `ARB_base_instance`; falls through to legacy multidraw if absent (logs `event=unsupported`). Emits `[DRAW_PACKET_V5]` to stderr: `event=armed` once at gate-on, `event=dispatch_summary` every 600 frames (slots_considered, draws_issued, zero_instance_skips, sorted_oob, packet_oob, type_oob, base_instance_missing, gl_errors, ok). `ok=1` iff all error counters are 0. Default OFF. Cached at process start. Implementation: `gos_static_prop_batcher.cpp` only — no gameosmain changes.
- `MC2_DRAW_PACKET_COALESCE_V5_TRACE=1` — per-slot verbose trace for v5. Emits one line per issued draw and one line per skip with reason. Requires `MC2_DRAW_PACKET_COALESCE_V5=1`. Default OFF. Cached at process start.
- `MC2_DRAW_PACKET_STATIC_PROP_V6=1` — **REMOVED in v7.1.** Gate plumbing deleted; setting this var has no effect whatsoever. Kill-switch for the primary path: `MC2_STATIC_PROP_LEGACY_DISPATCH=1`.
- `MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE=1` — per-slot verbose trace for v6. Emits one `[DRAW_PACKET_V6] slot=S pkt=P type=T group=G inst=I base=B drawID=D first=F count=C baseV=V` line per issued draw. Requires v6 path active (default in v7; no explicit env var needed). Default OFF. Cached at process start.
- `MC2_STATIC_PROP_LEGACY_DISPATCH=1` — v7 kill-switch. Reverts the v6 packet+meta dispatch path to legacy `glMultiDrawElementsIndirect` for this process. Use to isolate v6-specific rendering regressions. When set, `s_v6Enabled` returns false at process start; no `[DRAW_PACKET_V6]` lines appear in logs. Default OFF. Cached at process start.

## Snapshot-assisted dispatch (Extraction v2.3)

- `MC2_SNAP_CULL=1` — opt-in snapshot-assisted static-prop snap-cull. When enabled, the v6 dispatch loop uses the previous frame's RenderSnapshot to skip draw slots whose instanceCount was zero. Requires `snap->ok==1` and count-match validation. Warmup guard prevents frame-1 blank artifact. `spSnapCullSlotMismatch` is in the `ok` gate. Smoke tier1 passes with this set; skipped counts vary by mission density. Default OFF. Cached at process start. Implementation: `gos_static_prop_batcher.cpp` only.
- `MC2_SNAPSHOT_MECH_EXTRACT=1` — MECH-EXTRACTION-1/2/3 observe-only mech snapshot (default OFF). Wires `ExtractedMechPacket[]` from the persist buffer populated in `flush()` before `s_pendingSubmits.clear()`. materialIdx wired via `s_mechHandleToMaterialIdx` (independent compare — same map as SSBO upload). In `ok` gate from MECH-EXTRACTION-4. Implementation: `gos_mech_batcher.cpp` + `render_snapshot.cpp`. Inspector: "Mech Snapshot" panel in Mech section (MECH-EXTRACTION-2).
  Tier1 forced-ON canary (MECH-EXTRACTION-3) — all 5 missions clean, mat_sentinel=0, all mismatches=0:
  `mc2_01` snapshot=3 mat_valid=3 | `mc2_03` snapshot=1 mat_valid=1 | `mc2_10` snapshot=1 mat_valid=1 | `mc2_17` snapshot=12 mat_valid=12 | `mc2_24` snapshot=6 mat_valid=6
  Run: `py -3 scripts/run_smoke.py --tier tier1 --duration 30` with this set; verify each `[mech-extract]` shows matching snapshot + mat_valid + mat_sentinel=0 + all mismatches=0.
- `MC2_SNAPSHOT_STATIC_PROP_BUILD` — snapshot-owned slot identity dispatch (Extraction v3). Default ON as of STATIC-PROP-V3-FLIP (2026-05-27); set `=0` to disable (kill-switch → live builder authority). When ON, builds a second set of dispatch arrays (`s_snapV6Packets`/`s_snapV6Meta`) from the previous frame's `RenderSnapshot` rows, compares against live-built arrays field-by-field, and dispatches snapshot-built arrays if compare passes (zero mismatch). Falls back to live arrays on any mismatch. Incompatible with `MC2_SNAP_CULL=1` (collision → live dispatch + log line). Counters: `spBuildAttempted`/`spBuildFallback` informational; `spBuildCountMismatch`/`spBuildPacketMismatch`/`spBuildMetaMismatch` in `ok` gate. Cached at process start. Implementation: `gos_static_prop_batcher.cpp`; accessor `batcher_getSnapshotBuildStats()`.

## SSBO binding registry

Note: bindings are **per-pass** (each GL program declares its own layout bindings and each pass re-binds before drawing). The static_prop pass and terrain pass are separate programs.

### static_prop pass

| Binding | Owner | Status |
|---|---|---|
| 0 | Instances | active |
| 1 | Colors (legacy) | active |
| 2 | PerType | active |
| 3 | Parity (debug) | active (`MC2_OBJECT_PARITY_CHECK=1`) |
| 4 | PerDraw | active (coalesce path) |
| 5 | MaterialGpu | ACTIVE (v3+) — always declared in static_prop.frag coalesce variant; SSBO bound when MC2_MATERIAL_GPU=1, unbound otherwise. Shader accesses only when u_materialGpuSample=1. |

### terrain pass (separate GL program — independent binding namespace)

| Binding | Owner | Status |
|---|---|---|
| 5 | WaterRecipeBuf | active (gos_terrain_mask_water.vert, gos_terrain_water_fast.vert, gos_terrain_water_fast_mdi.vert) |
