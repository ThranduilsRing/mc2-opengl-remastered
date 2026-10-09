// RenderCore/RendererFeatureRegistry.h
//
// Header-only inventory of renderer feature gates and device capabilities.
// INVENTORY ONLY -- no feature decisions here. Each system's own env-flag
// accessor (e.g. RenderWorld::IsObjectIdBufferEnabled, g_useGpuMechs) remains
// authoritative for runtime decisions. This header is the single place to
// look up what env vars exist, what their defaults are, and what GL caps
// the engine requires per feature.
//
// Enforcement: scripts/check-env-registry.sh greps this file for all
// "MC2_[A-Z_]+" string literals. Any MC2_* var used in source that does not
// appear here AND is not in the script's ALLOWLIST triggers a CI failure.
// Adding a new env var: register it here first, then add the getenv() call.
//
// Usage:
//   DeviceCaps caps{};
//   fillDeviceCaps(caps);             // caller provides; needs GL context
//   debugDumpFeatureRegistry(caps);   // stderr banner at startup
//
// Firewall: MUST NOT include any GL header or game-side header.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace RenderCore {

// ---------------------------------------------------------------------------
// Device capabilities
// ---------------------------------------------------------------------------
// POD struct; caller fills it with GL queries after context creation.
// Zero-initialised default = "unknown / not queried yet".
// Nothing in this header calls GL.

struct DeviceCaps {
    int  glMajor              = 0;   // GL_MAJOR_VERSION
    int  glMinor              = 0;   // GL_MINOR_VERSION

    // Extension / promotion points
    bool hasSSBO              = false;  // GL 4.3 / ARB_shader_storage_buffer_object
    bool hasTessellation      = false;  // GL 4.0 / ARB_tessellation_shader
    bool hasComputeShaders    = false;  // GL 4.3 / ARB_compute_shader
    bool hasIndirectDraw      = false;  // GL 4.0 / ARB_draw_indirect
    bool hasBindlessTextures  = false;  // ARB_bindless_texture (AMD: driver-version-gated)
    bool hasClipControl       = false;  // GL 4.5 / ARB_clip_control (load-bearing for reverse-Z)
    bool hasDSA               = false;  // GL 4.5 / ARB_direct_state_access

    // Limits
    int  maxColorAttachments  = 0;  // GL_MAX_COLOR_ATTACHMENTS
    int  maxSSBOBindings      = 0;  // GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
    int  maxTessGenLevel      = 0;  // GL_MAX_TESS_GEN_LEVEL
};

// ---------------------------------------------------------------------------
// Env-var kind
// ---------------------------------------------------------------------------
// Used by EnvVarDesc to categorise entries in both tables below.

enum class EnvVarKind : uint8_t {
    Feature  = 0,  // gates a render code path (runtime behavior change)
    Trace    = 1,  // diagnostic output only; no correctness effect
    Retired  = 2,  // still in code; NO new consumers; superseded by RenderWorld API
};

// ---------------------------------------------------------------------------
// Env-var descriptor
// ---------------------------------------------------------------------------

struct EnvVarDesc {
    const char* featureId;   // "MC2_FEATURE_*" canonical name (or "MC2_TRACE_*" etc.)
    const char* envVar;      // env var string; nullptr = always-on (no runtime gate)
    EnvVarKind  kind;
    bool        defaultOn;   // true if active when env var is absent / unset
    const char* doc;         // one-line rationale / owner
};

// ---------------------------------------------------------------------------
// Feature enum
// ---------------------------------------------------------------------------
// Only Feature-kind and Retired-kind entries appear here (not Trace).
// Values are stable -- never renumber, only append before COUNT.
// Retired entries stay in the enum so existing code that indexes kFeatureTable
// by RendererFeature::GpuMechs etc. continues to compile.

enum class RendererFeature : int {
    // [Retired] superseded by RenderWorld::registerMech; no new getenv consumers
    GpuMechs            = 0,   // MC2_GPU_MECHS
    // [Retired] superseded by RenderWorld::upsertStaticProp; no new getenv consumers
    StaticPropIndirect  = 1,   // MC2_GPU_OBJECTS
    // Feature gates
    ObjectIdBuffer      = 2,   // MC2_OBJECT_ID_BUFFER
    TerrainTessellation = 3,   // MC2_TERRAIN_TESS (default-ON kill-switch, ISSUE-34)
    ReverseZ            = 4,   // (no env var -- glClipControl unconditional)
    ShadowMaps          = 5,   // MC2_SHADOW_ENABLE
    ImGui               = 6,   // MC2_IMGUI
    ImGuiInspector      = 7,   // MC2_IMGUI_INSPECTOR
    DebugRenderer       = 8,   // MC2_DEBUG_RENDERER
    MaterialGpu         = 9,   // MC2_MATERIAL_GPU
    MaterialGpuSample   = 10,  // MC2_MATERIAL_GPU_SAMPLE
    StaticPropRegistry  = 11,  // MC2_STATIC_PROP_REGISTRY
    MaterialKtx         = 12,  // MC2_MATERIAL_KTX
    // v6 DrawPacket+Meta dispatch. MC2_STATIC_PROP_LEGACY_DISPATCH=1 forces legacy fallback (kill-switch).
    StaticPropPacketDispatch = 13,  // MC2_STATIC_PROP_LEGACY_DISPATCH
    // [Retired] v6 opt-in gate removed in v7.1; setting this var is inert.
    DrawPacketStaticPropV6   = 14,  // MC2_DRAW_PACKET_STATIC_PROP_V6
    // Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state.
    RenderSnapshotBuild      = 15,  // MC2_SNAPSHOT_STATIC_PROP_BUILD
    // Extraction v2.3: skip prev-frame zero-instance snapshot slots (snap-cull opt-in).
    SnapCull                 = 16,  // MC2_SNAP_CULL
    // F1-3A ViewUniforms UBO upload (upload-only; F1-3B adds shader consumption).
    ViewUniforms             = 17,  // MC2_VIEW_UNIFORMS
    // MECH-EXTRACTION-0: mech snapshot extraction (observe-only).
    SnapshotMechExtract      = 18,  // MC2_SNAPSHOT_MECH_EXTRACT
    // V-AMBIENT-STATIC-1: hemisphere ambient fill on StaticPropOpaque lane.
    // Default-OFF; strength uniform = 0.0 when env unset/0 -> byte-identical
    // to pre-slice output.
    StaticPropAmbientV1      = 19,  // MC2_STATIC_PROP_AMBIENT_V1
    // V-MATERIAL-DEBUG-1: per-fragment material debug view (StaticPropOpaque).
    // Default-OFF; 0 = byte-identical to legacy output (shader short-circuit).
    StaticPropDebugMaterial  = 20,  // MC2_STATIC_PROP_DEBUG_MATERIAL
    // V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane.
    // Default-ON; MC2_STATIC_PROP_IBL_SH=0 uploads strength 0.0 so
    // shader `if (u_iblShStrength > 0.0)` short-circuits to byte-identical.
    // ImGui slider (g_iblShStrength) modulates strength when env gate is on;
    // the env var is the authoritative gate.
    StaticPropIblSh          = 21,  // MC2_STATIC_PROP_IBL_SH
    // V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular
    // on StaticPropOpaque lane. Default-OFF; strength uniform = 0.0 when
    // env unset/=0 -> shader `if (u_pbrV1Strength > 0.0)` short-circuits.
    // Uses MaterialGpu roughness/metallic when sampling is active, else
    // fallback metallic=0.0 roughness=0.6; F0 is albedo-tinted for metals.
    StaticPropPbrV1          = 22,  // MC2_STATIC_PROP_PBR_V1
    // TERRAIN-NORMALS-FROM-HEIGHT-1: gated macroscopic surface normal derived
    // from per-mission R32F height texture in gos_terrain.frag. Default-OFF.
    TerrainNormalsFromHeight = 23,  // MC2_TERRAIN_NORMALS_FROM_HEIGHT
    // TERRAIN-LIGHTING-1: hemisphere ambient fill on tessellated terrain
    // using terrain surface normal. Default-OFF.
    TerrainLightingV1        = 24,  // MC2_TERRAIN_LIGHTING_V1
    // TERRAIN-LIGHTING-2: shadow-aware modulation of the V1 hemisphere
    // fill — prevents over-bright shadows. Default-OFF (V1 unmodulated).
    TerrainLightingV2        = 25,  // MC2_TERRAIN_LIGHTING_V2
    // VFX-AGE-SAMPLE-1: sample GPU-particle spec curves at the effect's real
    // CPU-advanced m_age instead of the fixed 0.5 snapshot. Default-OFF.
    VfxAgeSample             = 26,  // MC2_VFX_AGE_SAMPLE
    // VFX-ORIGINAL-RECORD-ABI-1 (Phase 1): CPU-oracle render — GPU renders the
    // CPU sim's live per-particle records (CardCloud first; PointCloud births 0
    // in tier1). Default-OFF parity bridge; placeholder byte-identical when off.
    VfxOracleRender          = 27,  // MC2_VFX_ORACLE_RENDER
    // VFX-GPU-SIM-CARDCLOUD-BUFFER-1: compare-only GPU sim for CardCloud.
    // _CARDCLOUD gathers live CPU state into a persistent GPU sim SSBO
    // (observe-only, no integration/readback/render); _COMPARE emits integrity
    // logs. Default-OFF. Substrate for the eventual GPU-owned sim.
    VfxGpuSimCardCloud       = 28,  // MC2_VFX_GPU_SIM_CARDCLOUD
    VfxGpuSimCompare         = 29,  // MC2_VFX_GPU_SIM_COMPARE
    // TRACKV-POST-GROUNDING-MVP: experimental visual post-process + grounding
    // gates. ALL default-OFF (enforced by the Track-V guardrail unit test).
    // Promotion to default-ON is a deliberate edit here AND in that test.
    Ssao                     = 30,  // MC2_SSAO
    // TRACKV-VFX-PAYOFF-OPUS-1: VFX visual-payoff gates. Both default-OFF
    // (enforced by the Track-V VFX payoff guardrail unit test). Promotion to
    // default-ON is a deliberate edit here AND in that test.
    VfxSoftParticles         = 31,  // MC2_VFX_SOFT_PARTICLES
    VfxLitParticles          = 32,  // MC2_VFX_LIT_PARTICLES
    // VIEWMODE-POSTPROCESS-PRESENTATION-1: view-mode presentation framework.
    ViewmodeFramework        = 33,  // MC2_VIEWMODE_FRAMEWORK
    // TRACKRV-HZB-VISIBILITY-OPUS-1: gated reverse-Z Hi-Z depth pyramid build
    // from MainDepth (custom MIN reduction, ceil mip ladder). Default-OFF;
    // diagnostic substrate only -- NO culling/draw suppression. See
    // docs/hzb-depth-convention.md + docs/hzb-visibility-mvp.md.
    HzbBuild                 = 34,  // MC2_HZB_BUILD
    // TRACKRV-HZB-VISIBILITY-OPUS-1: diagnostic-only HZB occlusion probe.
    // Default-OFF; requires MC2_HZB_BUILD. Computes would-cull/would-keep +
    // pyramid-integrity counters from HZB readback. NEVER suppresses a draw.
    HzbProbe                 = 35,  // MC2_HZB_PROBE
    // COMPRESSION-BC7-STATICPROP-1: BC7-compressed KTX2 static-prop texture
    // arrays through the existing static-prop KTX path. Default-OFF; requires
    // MC2_MATERIAL_KTX. All-or-nothing + uniform-dim per array (see batcher).
    StaticPropBc7            = 36,  // MC2_STATICPROP_BC7
    // TEXMGR-COMPRESSED-UPLOAD-1: load a BC7 .ktx2 sidecar for a data/textures
    // texture (mech/vehicle/building/prop/UI/decal — via MC_TextureManager) and
    // upload it with glCompressedTexImage2D, single-level. Default-OFF; requires
    // GLEW_ARB_texture_compression_bptc. NOT static-prop arrays / terrain splat.
    TexmgrCompressedUpload   = 37,  // MC2_TEXMGR_COMPRESSED_UPLOAD
    // COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha immediately after
    // BuildColormapAtlas uploads to GPU. The indirect path (default-ON) has no
    // geometry displacement (thin vert, no TES), so the CPU elevation's null-guard
    // at mapdata.cpp:2435 becomes the correct state. Default-ON; kill-switch =0.
    ColormapCpuRetire        = 38,  // MC2_COLORMAP_CPU_RETIRE
    // COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas.
    // Falls back to RGBA8 when sidecar absent or BPTC cap missing. Default-ON.
    ColormapKtx2             = 39,  // MC2_COLORMAP_KTX2
    // TRACKV-CI-GATE-RESTORE-1: mission intro render path kill-switch.
    // Default-OFF = new/optimized intro render path active; ON = legacy path forced.
    MissionIntroLegacyRender = 40,  // MC2_MISSION_INTRO_LEGACY_RENDER
    // TRACKV-CI-GATE-RESTORE-1: setupTextures legacy path kill-switch.
    // Default-OFF = new/optimized setupTextures path active; ON = legacy path forced.
    SetupTexturesLegacyForce = 41,  // MC2_SETUPTEXTURES_LEGACY_FORCE
    // TERRAIN-DECAL-SLICE-0A: terrain mesh-decal system. When ON, cliff-wall
    // decoration types (e.g. "MarbleCliff") are submitted to the static-prop
    // batcher with an explicit CLIFF_WALL face-frame mat4 (up=world vertical,
    // facing=outward cliff normal, tangent=contour) + small outward offset,
    // instead of the yaw-only building recipe. Default-OFF -> byte-identical.
    TerrainDecal             = 42,  // MC2_TERRAIN_DECAL
    // FAR-BACKDROP-LIT: night-shade substitution for far/impostor static-prop
    // LODs (static_prop.vert). Far LODs skip the per-vertex night bake so their
    // base_light is a frozen day/black value -> fullbright/black at night; when
    // ON the shader fades the stale seed toward the mission night object-light
    // ambient across the LOD impostor distance. Default-OFF -> byte-identical.
    FarBackdropLit           = 43,  // MC2_FAR_BACKDROP_LIT
    // NIGHT-FOG-DIM: the edge-fog "cloud bank" (edge_fog.frag) + OOB fog paint a
    // hardcoded bright-grey u_fogColor ({0.80,0.81,0.83}) at the map horizon,
    // never dimmed for the mission's night lighting. On a night map (mc2_10) the
    // horizon cloud bank composites near-white (0.80 * exposure * warm-grade). When
    // ON, runEdgeFog/executor scale the fog color toward the measured night sky
    // shade before upload so the horizon band matches the dark HDRI sky it sits
    // against. Default-OFF -> uploads the unscaled constant -> byte-identical.
    NightFogDim              = 44,  // MC2_NIGHT_FOG_DIM
    // VEHICLE-NIGHT-LIT-1: coordinate-frame fix for the legacy lit shader path
    // (gos_tex_vertex_lighted.vert) — the program that draws ground vehicles.
    // It fed calc_light Stuff-frame position + normal while the ObjectLights
    // rows (and the mech.vert / static_prop.vert consumers) are MC2-frame, so
    // every positional light type failed its falloff distance test and vehicles
    // received no pool / cone light at night. Default-OFF -> byte-identical.
    LegacyLitMc2Frame        = 45,  // MC2_LEGACY_LIT_MC2_FRAME
    // VEHICLE-NIGHT-LIT-2: restore the missing else-branch that clears a ground
    // vehicle's lightsOut flag when it is no longer DESTROYED/DISABLED, matching
    // the mech path. Default-OFF -> legacy one-way latch -> byte-identical.
    GvLightsOutClear         = 46,  // MC2_GV_LIGHTSOUT_CLEAR
    // SHADOW-DECISIONS-1: re-enable shadow CASTING for far/impostor static-prop
    // LODs (lod >= 1) in the dynamic-prop shadow replay. The exclusion's original
    // "shadow depth pass has no alpha discard" rationale has been false since
    // b8f764b6. Default-OFF -> caster set byte-identical.
    PropLodShadowCast        = 47,  // MC2_PROP_LOD_SHADOW_CAST
    PropBaseLightDir         = 48,  // MC2_PROP_BASE_LIGHT_DIR
    PropFoliageAlphaFlag     = 49,  // MC2_PROP_FOLIAGE_ALPHAFLAG
    LightPosColumnFix        = 50,  // MC2_LIGHT_POS_COLUMN_FIX
    // VEHICLE-LIGHTING-PATH-1: populate the ground-vehicle light row before the
    // legacy draw. GVAppearance called neither CacheGpuLightData nor
    // submitMultiShape, so its lightData_ stayed all-zero and calc_light
    // returned vec3(1,1,1) -> vehicles rendered at raw albedo, fully unlit.
    // Default-ON; =0 restores the shipped unlit behavior.
    VehicleLitPath           = 51,  // MC2_VEHICLE_LIT_PATH
    // OVERLAY-CEMENT-ADOPT-1 (CONCRETE-DELIVER-1): on a cement tile the
    // overlay/decal layer adopts terrain_lod_chunk.frag's concrete albedo +
    // lighting model, closing the two-shader seam that made concrete a
    // patchwork -- including the in-shader distance fog the overlay applies
    // and the chunk frag does not. Default-ON; =0 restores stock.
    OverlayCementAdopt       = 52,  // MC2_OVERLAY_CEMENT_ADOPT
    // VEHICLE-AMBIENT-FLOOR-1: finishes VEHICLE-LIGHTING-PATH-1 by giving the
    // legacy lit vehicle path mech.vert's kAmbientFloor = 0.35 seed and its
    // clamp(0,1). Without them a gathered vehicle computes base_light+A+S*ndl
    // against a mech's clamp(0.35+A+S*ndl,0,1), so the truck still reads darker
    // than the mech beside it. Default-ON; =0 restores no-floor/no-clamp.
    VehicleAmbientFloor      = 53,  // MC2_VEHICLE_AMBIENT_FLOOR
    // LIGHT-ROW-PURE-1 (ARCH-LIGHTING slice 1): object light-ROW PRODUCTION
    // delegated to the pure object_light_row::BuildObjectLightRow, driven by an
    // explicit query + declared per-class policy instead of the shared-light-
    // list mutate/gather/restore dance. Row production only -- no shading
    // change. Default-OFF (killswitch inverted): =1 opts in; in-engine
    // byte-identity is unproven while launches are blocked.
    // Its companion diagnostic MC2_LIGHT_ROW_PURE_VERIFY is a Trace var and
    // lives in kAuxEnvVars, which is deliberately NOT indexed by
    // RendererFeature.
    LightRowPure             = 54,  // MC2_LIGHT_ROW_PURE
    // SHORELINE-GLOW-1: opt in to letting the active V3 terrain shoreline band
    // actually SUPPRESS the legacy screen-space runShoreline() post pass. The
    // suppression has existed since TERRAIN-SHORELINE-MASK-1 but was
    // unreachable, because its only input read MC2_TERRAIN_SHORELINE with a
    // stale default-OFF idiom. Reachable now; opt-in because enabling it is a
    // visible change no capture has graded. Default-OFF.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    ShorelineLegacySuppress  = 55,  // MC2_SHORELINE_LEGACY_SUPPRESS
    // SUN-AXIS-FIX-1: ObjectLights.light_dir is a STUFF-frame vector, but the
    // DIFFUSE consumers of calc_light dot it against MC2-frame normals. The one
    // consumer that swaps first (mech.vert:279, specular) is the correct one.
    // Default-OFF -> shipped expression bit-for-bit.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    SunAxisFix               = 56,  // MC2_SUN_AXIS_FIX
    // SUN-ELEVATION-FIX-1: Camera::lightDirection normalizes a unit-horizontal
    // plus sin(pitch), so its elevation is atan(sin(pitch)) rather than the
    // authored pitch. Large blast radius (terrain, props, SHADOW direction, and
    // the sun colour byte via getLightRed). Default-OFF -> bit-for-bit legacy.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    SunElevationFix          = 57,  // MC2_SUN_ELEVATION_FIX
    // LIGHTROW-FRESHNESS-1: the DYNAMIC static-prop submit path checks the
    // per-frame stamp on TG_MultiShape::cachedFrame_ before trusting
    // cachedGpuLightIndex_, and re-gathers instead of reusing a prior-frame
    // slot. Default-ON (it is a correctness fix, not a look change); =0 restores
    // the pre-slice no-frame-check read bit-for-bit.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    StaticPropLightFreshness = 58,  // MC2_STATIC_PROP_LIGHT_FRESHNESS
    // MECH-LIGHTROW-FRESHNESS-1: the GPU-mech submit path checks the per-frame
    // stamp on TG_MultiShape::cachedFrame_ before trusting cachedGpuLightIndex_,
    // and DECLINES the GPU submit (routing the mech to the existing CPU MLR
    // draw) instead of substituting slot 0. Default-ON (correctness fix, not a
    // look change); =0 restores the pre-slice no-frame-check read bit-for-bit.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    MechLightFreshness       = 59,  // MC2_MECH_LIGHT_FRESHNESS
    // RUNWAY-CLOSE-1: A/B arm — MC2_OVERLAY_ART_FLATLIGHT=0 exempts the
    // terrain_overlay.frag authored-art else-branch from u_missionFlatLight.
    // Default-ON (=stock multiply); appended LAST to match kFeatureTable.
    OverlayArtFlatlight      = 60,  // MC2_OVERLAY_ART_FLATLIGHT
    COUNT                    = 61,
};

// ---------------------------------------------------------------------------
// Feature table (kFeatureTable)
// ---------------------------------------------------------------------------
// Indexed by static_cast<int>(RendererFeature::*).
// static constexpr: one copy per TU (small struct; fine).

static constexpr EnvVarDesc kFeatureTable[] = {
    // GpuMechs [Retired]
    {
        "MC2_FEATURE_GPU_MECHS",
        "MC2_GPU_MECHS",
        EnvVarKind::Retired,
        true,
        "[RETIRED] GPU mech batcher (gos_mech_batcher). Superseded by RenderWorld::registerMech. No new getenv consumers."
    },
    // StaticPropIndirect [Retired]
    {
        "MC2_FEATURE_STATIC_PROP_INDIRECT",
        "MC2_GPU_OBJECTS",
        EnvVarKind::Retired,
        false,
        "[RETIRED] Indirect-draw static-prop batcher (gos_static_prop_batcher). Superseded by RenderWorld::upsertStaticProp."
    },
    // ObjectIdBuffer
    {
        "MC2_FEATURE_OBJECT_ID_BUFFER",
        "MC2_OBJECT_ID_BUFFER",
        EnvVarKind::Feature,
        true,
        "R32_UINT MRT attachment-2 with per-pixel object handles (RenderWorld M1.5). Default-ON; =0 disables (reader: RenderWorld.cpp readObjectIdBufferEnv)."
    },
    // TerrainTessellation
    {
        "MC2_FEATURE_TERRAIN_TESSELLATION",
        "MC2_TERRAIN_TESS",
        EnvVarKind::Feature,
        true,
        "TCS/TES terrain tessellation (gameos_graphics.cpp). Default-ON kill-switch (ISSUE-34): MC2_TERRAIN_TESS=0 skips the gos_terrain material load (same engine state as a TCS/TES compile failure) for headless A/B of the no-tess config. Shadow builds + blob-shadow suppression key on gos_IsModernShadowPathActive(), not tess."
    },
    // ReverseZ
    {
        "MC2_FEATURE_REVERSE_Z",
        nullptr,
        EnvVarKind::Feature,
        true,
        "Reverse-Z depth via glClipControl GL_ZERO_TO_ONE (gameosmain.cpp:992). Always-on; MC2_REVERSE_Z_TRACE for trace."
    },
    // ShadowMaps
    {
        "MC2_FEATURE_SHADOW_MAPS",
        "MC2_SHADOW_ENABLE",
        EnvVarKind::Feature,
        true,
        "Dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow). **DEFAULT ON; kill-switch MC2_SHADOW_ENABLE=0.** GATE-DEFAULT-TRUTH-1 (2026-07-30) corrected this entry, which had declared default `false` / \"Opt-in =1\" since it landed while BOTH GL readers implemented default ON: gos_static_prop_batcher.cpp:8717-8719 and gos_mech_batcher.cpp:1112-1114, each `!(getenv(...) && getenv(...)[0]=='0')`. The registry, docs/tier1_env_vars.md and docs/engine-closure-audit.md all said OFF, so every A/B graded as \"=1 vs unset\" compared ON against ON. TREES-REOPEN-1 documented the disagreement and nobody corrected the entry; the reader is what ships, so the entry now matches the reader. **KNOWN BACKEND DIVERGENCE, NOT FIXED HERE:** the VK reader (vulkan_post_screenfx_native.cpp:760, `mc2vkgate::envTriState(\"MC2_SHADOW_ENABLE\") == 1`) is genuinely opt-in, so a default VK session takes NO dynamic caster pass while a default GL session does. That is a real cross-backend parity defect and flipping it blind would change VK output with no capture to check it against, so it is named here instead of silently changed. VAO restore order fixed (SHADOW-DYNAMIC-RESTORE-1); prop-invisibility-with-shadows-on regression RESOLVED (flush-order bug, commit 2764cb65 — registry flush must precede flushShadow)."
    },
    // ImGui
    {
        "MC2_FEATURE_IMGUI",
        "MC2_IMGUI",
        EnvVarKind::Feature,
        true,
        "ImGui overlay (GuiRuntime/GuiRuntime.cpp). Default-ON; =0 disables (reader: GuiRuntime/GuiRuntime.cpp isEnabled())."
    },
    // ImGuiInspector
    {
        "MC2_FEATURE_IMGUI_INSPECTOR",
        "MC2_IMGUI_INSPECTOR",
        EnvVarKind::Feature,
        true,
        "ImGui inspector panel (GuiRuntime/EditorInspector.cpp). Default-ON; =0 disables (reader: EditorInspector.cpp). Requires MC2_IMGUI."
    },
    // DebugRenderer
    {
        "MC2_FEATURE_DEBUG_RENDERER",
        "MC2_DEBUG_RENDERER",
        EnvVarKind::Feature,
        false,
        "Debug overlay renderer (GuiRuntime/EditorInspector.cpp). Default-off; =1 enables. Requires MC2_IMGUI_INSPECTOR."
    },
    // MaterialGpu
    {
        "MC2_FEATURE_MATERIAL_GPU",
        "MC2_MATERIAL_GPU",
        EnvVarKind::Feature,
        true,
        "GPU material table upload/bind/compare for static props (default-ON as of v5, 2026-05-26). Set =0 to disable."
    },
    // MaterialGpuSample
    {
        "MC2_FEATURE_MATERIAL_GPU_SAMPLE",
        "MC2_MATERIAL_GPU_SAMPLE",
        EnvVarKind::Feature,
        true,
        "GPU material albedo sampling in static_prop.frag (default-ON as of v7, 2026-05-26). Set =0 to fall back to texArrayLayer. Requires MC2_MATERIAL_GPU."
    },
    // StaticPropRegistry
    {
        "MC2_FEATURE_STATIC_PROP_REGISTRY",
        "MC2_STATIC_PROP_REGISTRY",
        EnvVarKind::Feature,
        true,
        "GpuStaticPropRegistry enable. Default-on; editor sets =0 (EditorMFC.cpp) to bypass registry for edit-time mutations."
    },
    // MaterialKtx
    {
        "MC2_FEATURE_MATERIAL_KTX",
        "MC2_MATERIAL_KTX",
        EnvVarKind::Feature,
        false,
        "KTX2 sidecar loader for static-prop textures (RenderCore/KtxLoader). Phase 0: RGBA8 only. Default-off; =1 enables. Requires MC2_COALESCE=1."
    },
    // StaticPropPacketDispatch
    {
        "MC2_FEATURE_STATIC_PROP_PACKET_DISPATCH",
        "MC2_STATIC_PROP_LEGACY_DISPATCH",
        EnvVarKind::Feature,
        true,
        "v6 DrawPacket+Meta dispatch (default-ON as of v7). MC2_STATIC_PROP_LEGACY_DISPATCH=1 forces legacy glMultiDrawElementsIndirect fallback. Kill-switch; env var absent = packet path active."
    },
    // DrawPacketStaticPropV6 [Retired]
    {
        "MC2_FEATURE_DRAW_PACKET_STATIC_PROP_V6",
        "MC2_DRAW_PACKET_STATIC_PROP_V6",
        EnvVarKind::Retired,
        false,
        "[RETIRED] v6 opt-in gate removed in v7.1. Gate plumbing deleted; setting this var has no effect. Kill-switch is MC2_STATIC_PROP_LEGACY_DISPATCH."
    },
    // RenderSnapshotBuild
    {
        "MC2_FEATURE_RENDER_SNAPSHOT_BUILD",
        "MC2_SNAPSHOT_STATIC_PROP_BUILD",
        EnvVarKind::Feature,
        true,
        "Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state (gos_static_prop_batcher.cpp). Default-ON as of STATIC-PROP-V3-FLIP (2026-05-27). Kill-switch: =0 forces OFF and falls back to live builder authority."
    },
    // SnapCull
    {
        "MC2_FEATURE_SNAP_CULL",
        "MC2_SNAP_CULL",
        EnvVarKind::Feature,
        false,
        "Extraction v2.3: skip prev-frame zero-instance slots during snapshot-based dispatch (snap-cull opt-in). Default-off; =1 enables. Requires snapshot path active."
    },
    // ViewUniforms
    {
        "MC2_FEATURE_VIEW_UNIFORMS",
        "MC2_VIEW_UNIFORMS",
        EnvVarKind::Feature,
        true,
        "ViewUniforms UBO at binding=3. Default ON. =0 reverts to legacy uniform. Shader consumption (static_prop.vert) requires restart."
    },
    // SnapshotMechExtract
    {
        "MC2_FEATURE_SNAPSHOT_MECH_EXTRACT",
        "MC2_SNAPSHOT_MECH_EXTRACT",
        EnvVarKind::Feature,
        false,
        "MECH-EXTRACTION-0: mech snapshot extraction (observe-only, no GL mutation). Extracts ExtractedMechPacket[] from RenderSnapshot. Default-off; =1 enables."
    },
    // StaticPropAmbientV1
    {
        "MC2_FEATURE_STATIC_PROP_AMBIENT_V1",
        "MC2_STATIC_PROP_AMBIENT_V1",
        EnvVarKind::Feature,
        false,
        "V-AMBIENT-STATIC-1: hemisphere ambient fill on StaticPropOpaque lane (static_prop.vert). Default-off; =1 enables (uniform u_ambientV1Strength=1.0). When OFF, strength=0.0 -> byte-identical to legacy output. Skips window-flag nodes."
    },
    // StaticPropDebugMaterial
    {
        "MC2_FEATURE_STATIC_PROP_DEBUG_MATERIAL",
        "MC2_STATIC_PROP_DEBUG_MATERIAL",
        EnvVarKind::Feature,
        false,
        "V-MATERIAL-DEBUG-1 / V-MATERIAL-PBR-1: per-fragment material debug view on StaticPropOpaque lane (static_prop.frag). Default 0 = OFF (byte-identical via `if (u_debugMaterialMode != 0) return;` short-circuit). Modes 1=albedo, 2=materialIdx-palette, 3=worldNormal, 4=texArrayLayer-palette, 5=roughnessFactor (grayscale), 6=metallicFactor (grayscale). Set MC2_STATIC_PROP_DEBUG_MATERIAL=N (1..6)."
    },
    // StaticPropIblSh
    {
        "MC2_FEATURE_STATIC_PROP_IBL_SH",
        "MC2_STATIC_PROP_IBL_SH",
        EnvVarKind::Feature,
        true,
        "V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane (static_prop.vert). Default-ON (flipped 2026-05-27 per V-STATICPROP-VISUAL-REVIEW-AUDIT); =0 is explicit kill-switch (byte-identical to pre-flip OFF). When OFF, u_iblShStrength uploads 0.0 -> shader short-circuits before evalShL2. Coefficients from RenderCore/IblShCoeffs.h (projected from data/hdr/DaySkyHDRI063B_4K.exr). ImGui slider g_iblShStrength modulates per-frame strength (default 0.5); env var is authoritative gate."
    },
    // StaticPropPbrV1
    {
        "MC2_FEATURE_STATIC_PROP_PBR_V1",
        "MC2_STATIC_PROP_PBR_V1",
        EnvVarKind::Feature,
        false,
        "V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular on StaticPropOpaque lane (static_prop.frag, inside `#if defined(MC2_USE_VIEW_UNIFORMS)`). Default-OFF; =1 enables. When OFF, u_pbrV1Strength uploads 0.0 -> shader `if (u_pbrV1Strength > 0.0)` short-circuits before any u_cameraWorldPos read. Uses MaterialGpu roughnessFactor/metallicFactor when MC2_MATERIAL_GPU_SAMPLE is active; otherwise falls back to metallic=0.0 and roughness=0.6. F0 is albedo-tinted for metallic materials. Window/hot-pink nodes bypass the PBR branch. Safety interlock: when MC2_VIEW_UNIFORMS=0, CPU force-zeroes strength and the shader compile-guard excludes the block. Sun direction accepts TG_LIGHT_INFINITE and TG_LIGHT_INFINITEWITHFALLOFF. ImGui slider g_pbrV1Strength modulates per-frame strength (default 1.0, range 0..3); env var is authoritative gate. Optional override MC2_STATIC_PROP_PBR_V1_STRENGTH (clamped 0..3) sets the default."
    },
    // TerrainNormalsFromHeight
    {
        "MC2_FEATURE_TERRAIN_NORMALS_FROM_HEIGHT",
        "MC2_TERRAIN_NORMALS_FROM_HEIGHT",
        EnvVarKind::Feature,
        false,
        "TERRAIN-NORMALS-FROM-HEIGHT-1: gated macroscopic surface normal derived from a per-mission R32F height texture (gos_terrain_height_tex.cpp; uploaded at mission load from MapData heightfield). Default-OFF; =1 enables. When OFF, useTerrainNormalsFromHeight uploads 0 and gos_terrain.frag skips the height-derived perturbation branch entirely → byte-identical legacy output. Visual-only: gameplay height (Terrain::getTerrainElevation) is unchanged; no geometry or vertex position is moved. CPU plumbing: env var read once-per-terrain-uniform-upload (no restart needed). Inspector mini-control in Terrain Pass panel displays current effective state. Debug visualization: MC2_TERRAIN_DEBUG_MODE=10 shows the height-derived normal as RGB (independent of this gate so the upload path can be diagnosed separately). Sampler unit 11. Texture not bound when no mission is loaded."
    },
    // TerrainLightingV1
    {
        "MC2_FEATURE_TERRAIN_LIGHTING_V1",
        "MC2_TERRAIN_LIGHTING_V1",
        EnvVarKind::Feature,
        false,
        "TERRAIN-LIGHTING-1: gated hemisphere ambient fill on the tessellated terrain. Default-OFF; =1 enables. When OFF, terrainLightingV1Strength uploads 0.0 and gos_terrain.frag skips the additive branch → byte-identical legacy output. Adds sky/ground tinted ambient that fills shadowed terrain (added AFTER shadow multiplication so direct sun stays shadowed but bounce light continues). Best paired with MC2_TERRAIN_NORMALS_FROM_HEIGHT=1 — ambient verticality is derived from the final per-fragment normal (sky term scales with N.z). Strength tunable in-engine via Terrain Pass inspector slider (default 1.0); env gate is authoritative on/off. Visual-only; no gameplay, geometry, or collision change."
    },
    // TerrainLightingV2
    {
        "MC2_FEATURE_TERRAIN_LIGHTING_V2",
        "MC2_TERRAIN_LIGHTING_V2",
        EnvVarKind::Feature,
        false,
        "TERRAIN-LIGHTING-2: gated shadow-aware modulation of the TERRAIN-LIGHTING-1 hemisphere fill. Default-OFF; =1 enables. When OFF, terrainLightingV2ShadowFillFloor uploads 1.0 → the shader expression mix(floor, 1.0, shadow) collapses to 1.0 → V1 behavior preserved (byte-equivalent to TERRAIN-LIGHTING-1 alone). When ON, the member floor (default 0.3, ImGui-tunable 0..1 via the Graphics Options Terrain section) scales the hemi additive in shadowed terrain so dark areas stay dark: floor=0.3 = 30% hemi in fully shadowed terrain, 100% in fully lit terrain. floor=0.0 makes hemi follow shadow exactly (lifeless shadows); floor=1.0 = V1 unmodulated. Debug mode MC2_TERRAIN_DEBUG_MODE=11 visualizes the hemi additive contribution as RGB (×4 for visibility). Visual-only; no gameplay, geometry, or collision change. Effective only when MC2_TERRAIN_LIGHTING_V1 is also ON (since the floor multiplies the V1 additive)."
    },
    // VfxAgeSample
    {
        "MC2_FEATURE_VFX_AGE_SAMPLE",
        "MC2_VFX_AGE_SAMPLE",
        EnvVarKind::Feature,
        false,
        "VFX-AGE-SAMPLE-1: sample GPU-particle spec curves (color/alpha/size/UV) at the routed effect's real CPU-advanced normalized age (gosFX Effect::m_age, threaded into mc2::particles::Spawn from each producer Draw) instead of the fixed 0.5 midpoint. Default-OFF; =1 enables. When OFF, resolveSampleAge() returns 0.5 → byte-identical to the pre-slice snapshot. When ON, particles regain fade-in/out + grow/shrink because each per-frame re-emit samples at the effect's current age. Read-only consumption of m_age (already advanced by gameplay) — NO emission/lifetime/spawn-rate/timing change; NO shader or GpuParticle-ABI change (curve eval stays CPU-side in spawn_*.cpp). Invalid/sentinel age (m_age=-1) or out-of-[0,1] falls back to 0.5. Affects only the 5 routed classes (Card/CardCloud/PointCloud/ShardCloud/Tube); unrouted CPU-only classes untouched. Object-ID invariant preserved. Min/max age summary logged under MC2_GPU_PARTICLES_LOG=1."
    },
    // VfxOracleRender
    {
        "MC2_FEATURE_VFX_ORACLE_RENDER",
        "MC2_VFX_ORACLE_RENDER",
        EnvVarKind::Feature,
        false,
        "VFX-ORIGINAL-RECORD-ABI-1 (Phase 1 of the originals-restoration arc; see docs/vfx-originals-restoration-design.md). Default-OFF; =1 enables. Target classes = CardCloud + ShardCloud (PointCloud was the original pick but births 0 particles in stock tier1; CardCloud ~30 live and ShardCloud ~25 live are the populated classes — confirmed by probe). When OFF, CardCloud::Draw runs the existing placeholder Spawn() path byte-identically. When ON, CardCloud::Draw HARVESTS the CPU gosFX sim's live per-particle data (per-particle m_localTranslation + m_scale + m_halfX/Y and m_P_color[i] — already advanced this frame by ParticleCloud::Execute/CardCloud::AnimateParticle) and emits one GPU billboard per LIVE particle (m_age<1): position = m_localToParent*parentToWorld * m_localTranslation; color = m_P_color[i]; size = scale*sqrt(halfX^2+halfY^2). Reuses the placeholder's texture/blend group. CPU sim stays AUTHORITATIVE (no sim change, no bypass); legacy MLR DrawEffect stays skipped (no dual-draw). ShardCloud (1316) is covered the same way (center=m_localTranslation, color=m_P_color[i*3] per-vertex, size=scale*m_radius). CardCloud + ShardCloud ONLY — PointCloud/Card/Tube/trails untouched. NO GPU-sim, NO emission/lifetime/timing change, NO object-ID write, NO shader/ABI change. Deferred to the ABI-extension follow-up: per-particle rotation (m_localRotation), CardCloud aspect, ShardCloud triangle shape (m_angle), per-particle UV frame. Parity diagnostic [VFX_ORACLE v1] FIRST_HARVEST + 240-call summary under MC2_GPU_PARTICLES_LOG=1. Parity-bridge/oracle stage, NOT the final architecture (end-state = GPU sim + CPU-sim deletion per class)."
    },
    // VfxGpuSimCardCloud
    {
        "MC2_FEATURE_VFX_GPU_SIM_CARDCLOUD",
        "MC2_VFX_GPU_SIM_CARDCLOUD",
        EnvVarKind::Feature,
        false,
        "VFX-GPU-SIM-CARDCLOUD-BUFFER-1 (Stage 2 of the originals-restoration arc; docs/vfx-gpu-sim-spec.md). Default-OFF; =1 enables. When ON, CardCloud::Draw gathers the live CPU-sim per-particle state (position, world velocity, age, ageRate, color, size, alive flag) into a CPU-compacted list (dead m_age>=1 filtered) and submits it to the GameOS bridge gos_cardcloud_sim_submit, which uploads it to a PERSISTENT GPU sim SSBO (CardCloudSimParticle, 64B std430). OBSERVE-ONLY substrate: NO compute integration, NO GPU->CPU readback, NO rendering, NO CPU-sim bypass. CPU sim stays authoritative; the frame is unchanged (independent of MC2_VFX_ORACLE_RENDER). CardCloud ONLY. COMPUTE-1 adds the cardcloud_sim.comp integration + readback + parity compare on this buffer layout."
    },
    // VfxGpuSimCompare
    {
        "MC2_FEATURE_VFX_GPU_SIM_COMPARE",
        "MC2_VFX_GPU_SIM_COMPARE",
        EnvVarKind::Feature,
        false,
        "VFX-GPU-SIM-CARDCLOUD-BUFFER-1: emit the [VFX_GPU_SIM v1] integrity/compare logs (cpuActive vs submitted-live count, age/alpha ranges, world bounds, SSBO capacity) for the CardCloud GPU sim buffer. Requires MC2_VFX_GPU_SIM_CARDCLOUD=1. Default-OFF. BUFFER-1 is a CPU-side plumbing check (no GPU readback, no stalls); real CPU-vs-GPU parity (maxPosError, age divergence) arrives in COMPUTE-1 once the buffer is integrated on GPU."
    },
    // Ssao
    {
        "MC2_FEATURE_SSAO",
        "MC2_SSAO",
        EnvVarKind::Feature,
        false,
        "SSAO-GTAO-LITE-MVP-1 (Track V): lightweight half-res screen-space ambient-occlusion grounding pass (gosPostProcess::runSSAO) reusing the scene depth + GBuffer1 world normals + inverseViewProj already produced for the screen-shadow pass. Default-OFF; =1 enables. Multiplicative composite into the scene color (like runScreenShadow) BEFORE the composite/tonemap, with a far-depth (sky) guard so background and UI are untouched. ImGui + per-mission tunables aoRadius / aoStrength / aoBias. When OFF, runSSAO is skipped entirely -> byte-identical. Visual-only; no geometry/objectId/UI change."
    },
    // VfxSoftParticles
    {
        "MC2_FEATURE_VFX_SOFT_PARTICLES",
        "MC2_VFX_SOFT_PARTICLES",
        EnvVarKind::Feature,
        false,
        "VFX-SOFT-PARTICLES-MVP-1 (Track V): depth-fade GPU particles at scene intersections. When =1, gosPostProcess copies the resolved scene depth into a dedicated sampleable texture (a blit, avoiding the read-from-bound-attachment feedback loop) before the in-scene particle flush, and particle_billboard.frag fades alpha by the reverse-Z linear depth difference between the fragment and the scene behind it. Applies to alpha-blended groups only (u_vfxIsAdditive==0); additive flashes/lasers are unaffected. Default-OFF; when OFF the depth copy is skipped entirely (zero added cost) and the FS fade is bypassed -> byte-identical. Reverse-Z convention near=1/far=0 (NOT SSAO's stale forward-Z comments). Visual-only; no geometry/objectId/UI/gameplay change. No effect when MC2_GPU_PARTICLES=0."
    },
    // VfxLitParticles
    {
        "MC2_FEATURE_VFX_LIT_PARTICLES",
        "MC2_VFX_LIT_PARTICLES",
        EnvVarKind::Feature,
        false,
        "VFX-LIT-PARTICLES-MVP-1 (Track V): simple directional+ambient lighting for alpha-blended smoke/dust particles. When =1, particle_billboard.frag applies a wrapped hemispheric sun+ambient fill sourced from the global camera lighting (eye->lightDirection / lightRGB / ambientRGB, the same source terrain uses) to non-additive groups (u_vfxIsAdditive==0) only; additive flashes/lasers/PPC stay self-emissive/unlit. Strength is the MC2_TUNE_VFX_LIT_STRENGTH startup default / vfxLitStrength per-mission profile key / live ImGui slider. Default-OFF; when OFF the lit term resolves to identity (tex*v_color) -> byte-identical. No new SSBO/struct/ABI; no per-fragment normal (billboard). Visual-only; no geometry/objectId/UI/gameplay change. No effect when MC2_GPU_PARTICLES=0."
    },
    // ViewmodeFramework
    {
        "MC2_FEATURE_VIEWMODE_FRAMEWORK",
        "MC2_VIEWMODE_FRAMEWORK",
        EnvVarKind::Feature,
        false,
        "VIEWMODE-POSTPROCESS-PRESENTATION-1: enables the view-mode presentation framework that wires ViewMode::ObjectIdDebug (and future sensor modes) through the postprocess composite pass. Default-OFF; when OFF endScene forces u_viewMode=0 (Visual) and the ImGui combo is not rendered -> byte-identical. When ON, u_viewMode and u_objectIdTex are set on compositeProg_ each frame; ObjectIdDebug also requires MC2_OBJECT_ID_BUFFER=1 (sceneObjectIdTex_ non-zero). No new SSBOs, UBOs, or render targets. No scene-shader changes. Visual-only presentation overlay."
    },
    // HzbBuild
    {
        "MC2_FEATURE_HZB_BUILD",
        "MC2_HZB_BUILD",
        EnvVarKind::Feature,
        false,
        "TRACKRV-HZB-VISIBILITY-OPUS-1: build a reverse-Z Hi-Z depth pyramid from MainDepth each frame via a custom fragment MIN-reduction pass (hzb_reduce.frag), GL_R32F, ceil mip ladder down to 1x1 (NOT glGenerateMipmap -- it averages+floors). Default-OFF; when OFF the HZB texture/FBO is never allocated and runHzbReduce() is skipped -> zero cost, byte-identical. Diagnostic substrate ONLY: no consumers, no culling, no draw suppression. Matches docs/hzb-depth-convention.md (MIN = farthest occluder = conservative)."
    },
    // HzbProbe
    {
        "MC2_FEATURE_HZB_PROBE",
        "MC2_HZB_PROBE",
        EnvVarKind::Feature,
        false,
        "TRACKRV-HZB-VISIBILITY-OPUS-1: diagnostic-only HZB occlusion probe. Default-OFF; requires MC2_HZB_BUILD. After the pyramid is built, reads back a parent HZB level and its child level and (a) verifies parent == MIN(children) (reduction integrity), (b) runs the conservative reverse-Z cull comparison childDepth < parentDepth -- which must be wouldKeep for every self-point (a texel inside a tile can never be culled by that tile's min). Logs tested/wouldKeep/wouldCull(==0)/integrityMismatch/invalidDepth counters. NEVER suppresses any draw (neverAppliedToDraws=true). Validates the cull math + pyramid before real object bounds are wired (HZB-STATICPROP-CULL-RECON-1)."
    },
    // StaticPropBc7
    {
        "MC2_FEATURE_STATICPROP_BC7",
        "MC2_STATICPROP_BC7",
        EnvVarKind::Feature,
        false,
        "COMPRESSION-BC7-STATICPROP-1: upload BC7-compressed KTX2 static-prop texture arrays through the existing static-prop KTX path (gos_static_prop_batcher.cpp), using immutable glTexStorage3D + glCompressedTexSubImage3D. Default-OFF; =1 enables. Requires MC2_MATERIAL_KTX=1. ALL-OR-NOTHING per GL array (one internalformat): a BC7 array is built only when EVERY unique's .ktx2 sidecar loads, is stored BC7 (vkFormat 145/146), matches its GL dims, AND the group is uniform-dim (all layers == maxW x maxH) -- otherwise the group falls back to the existing RGBA8 path unchanged. Uniform-dim is required because compressed sub-region uploads at small mips would violate 4x4 block alignment. All mip levels come from the cooked KTX (no glGenerateMipmap for compressed). Static-prop albedo is always sRGB, so the SRGB BPTC internalformat is used for both 145 and 146. When OFF (or any condition unmet), behavior is byte-identical to the RGBA8 KTX/legacy path. Static props ONLY; does not touch terrain/mech/vehicle/UI/burnin or MC_TextureManager."
    },
    // TexmgrCompressedUpload
    {
        "MC2_FEATURE_TEXMGR_COMPRESSED_UPLOAD",
        "MC2_TEXMGR_COMPRESSED_UPLOAD",
        EnvVarKind::Feature,
        true,
        "TEXMGR-COMPRESSED-UPLOAD-1: teach MC_TextureManager to load a BC7 .ktx2 sidecar (same path/stem as the source data/textures texture, extension -> .ktx2) and upload it via glCompressedTexImage2D in MC_TextureNode::get_gosTextureHandle (mclib/txmmgr.cpp), through the new gos_NewCompressedTexture2D GameOS creator. **DEFAULT ON; opt out with MC2_TEXMGR_COMPRESSED_UPLOAD=0.** GATE-DEFAULT-TRUTH-1 (2026-07-30) corrected this entry, which read \"Default-OFF; =1 enables\" while mclib/txmmgr.cpp:5356-5359 does `(!v || !v[0]) ? true : (v[0] != '0')` -- and the comment directly above that reader already said \"Default-ON (opt out with MC2_TEXMGR_COMPRESSED_UPLOAD=0)\". The registry was the only source still claiming OFF. Found by scripts/check-gate-default-agreement.py on its first run. Requires GLEW_ARB_texture_compression_bptc (falls through + logs once if absent). This is the path for mech/vehicle/building/prop/UI/decal albedo textures — NOT static-prop texture arrays (MC2_STATICPROP_BC7) and NOT terrain splatting. All such textures are albedo/color (key in {Solid,Alpha,Keyed}) so vkFormat 146->GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM and 145->GL_COMPRESSED_RGBA_BPTC_UNORM. These loads use gosHint_DisableMipmap (single-level, GL_LINEAR/no-mip), so the sidecar is uploaded MIP 0 ONLY (byteLen = mipByteOffsets[1] if >1 mip, else pixels.size()); no glGenerateMipmap. Disk-only probe (std::fopen, not File/FastFile). On any failure (no sidecar, not BC7, load/upload fail, no BPTC support) the hook falls through to gos_NewTextureFromMemory unchanged. When OFF, the hook is fully gated (the env check short-circuits before any sidecar probe) -> byte-identical to the legacy RGBA8 path. The new gos_NewCompressedTexture2D mirrors gos_NewTextureFromMemory's gosTexture+addTexture lifecycle (handle integrates with textureList_/bind/destroy)."
    },
    // ColormapCpuRetire
    {
        "MC2_FEATURE_COLORMAP_CPU_RETIRE",
        "MC2_COLORMAP_CPU_RETIRE",
        EnvVarKind::Feature,
        true,
        "COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha in BuildColormapAtlas immediately after GPU atlas upload. The default indirect terrain path (thin vert, no TES) does no geometry displacement, so terrainElevation's existing null-guard (mapdata.cpp:2435) skips the CPU displacement block, aligning grounding with the visual surface (both undisplaced). Kill-switch MC2_COLORMAP_CPU_RETIRE=0 keeps the CPU copy alive and restores legacy displacement (up to +-1 wu vs visual surface). MC2_COLORMAP_DISPLACE_PROBE=1 logs displacement magnitude when kill-switch is active."
    },
    // ColormapKtx2
    {
        "MC2_FEATURE_COLORMAP_KTX2",
        "MC2_COLORMAP_KTX2",
        EnvVarKind::Feature,
        true,
        "COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas (gos_terrain_indirect.cpp). When the sidecar is found on disk and GLEW_ARB_texture_compression_bptc is present, uploads via glCompressedTexImage2D(GL_COMPRESSED_RGBA_BPTC_UNORM) instead of glTexImage2D RGBA8. Saves ~81 MB VRAM per mission (108 MB -> ~27 MB). Falls back silently to RGBA8 when sidecar absent, BPTC cap missing, or load fails. KTX2 sidecar baked offline by scripts/bake_colormap_ktx2.py (Pillow BGRA->RGBA, ktx create uastc, ktx transcode bc7, --levels 1). Default-ON; kill-switch =0 forces RGBA8 path even when sidecar present."
    },
    // MissionIntroLegacyRender (TRACKV-CI-GATE-RESTORE-1)
    {
        "MC2_FEATURE_MISSION_INTRO_LEGACY_RENDER",
        "MC2_MISSION_INTRO_LEGACY_RENDER",
        EnvVarKind::Feature,
        false,
        "TRACKV-CI-GATE-RESTORE-1: mission intro render path kill-switch. Default-OFF = new/optimized intro render path active; ON (=1) forces legacy path. Diagnostic default-OFF; no behavioral change when unset."
    },
    // SetupTexturesLegacyForce (TRACKV-CI-GATE-RESTORE-1)
    {
        "MC2_FEATURE_SETUPTEXTURES_LEGACY_FORCE",
        "MC2_SETUPTEXTURES_LEGACY_FORCE",
        EnvVarKind::Feature,
        false,
        "TRACKV-CI-GATE-RESTORE-1: setupTextures legacy path kill-switch. Default-OFF = new/optimized setupTextures path active; ON (=1) forces legacy path. Diagnostic default-OFF; no behavioral change when unset."
    },
    // TerrainDecal (TERRAIN-DECAL-SLICE-0A)
    {
        "MC2_TERRAIN_DECAL",
        "MC2_TERRAIN_DECAL",
        EnvVarKind::Feature,
        false,
        "TERRAIN-DECAL-SLICE-0A: terrain mesh-decal system. When ON, cliff-wall decoration types (e.g. \"MarbleCliff\") are submitted to the GPU static-prop batcher with an explicit CLIFF_WALL face-frame mat4 (up=world vertical, facing=outward cliff normal, tangent=contour) plus a small outward offset (bdactor.cpp registerStatic), instead of the yaw-only building recipe. Default-OFF -> byte-identical."
    },
    // FarBackdropLit (FAR-BACKDROP-LIT-IMPL-1)
    {
        "MC2_FAR_BACKDROP_LIT",
        "MC2_FAR_BACKDROP_LIT",
        EnvVarKind::Feature,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: night-shade substitution for far/impostor static-prop LODs (static_prop.vert). Far/impostor LODs register via the positions-only hierarchy walk (bdactor.cpp TransformMultiShape_HierarchyOnly) and never get the per-vertex NIGHT light bake, so base_light (a_aRGBLight) stays a stale mission-load (day) value -> fullbright impostors at night (and pure-black for dark-baked far geometry). When ON, main() fades the stale per-vertex seed toward the mission night object-light ambient (mix over clip.w view-depth across the LOD impostor distance) so far geometry takes the night shade with no hard LOD-pop boundary. Default-OFF -> u_farBackdropLit=0 -> shader branch skipped -> byte-identical. Distance band + strength + debug tint tunable via MC2_FAR_BACKDROP_LIT_NEAR/FAR/STRENGTH/TINT."
    },
    // NightFogDim (BACKDROP-BLIT-ATTRIB-1)
    {
        "MC2_NIGHT_FOG_DIM",
        "MC2_NIGHT_FOG_DIM",
        EnvVarKind::Feature,
        false,
        "BACKDROP-BLIT-ATTRIB-1: RenderDoc pixel-history on the pre-composite HDR scene FBO (231) pinned the mc2_10 night 'white horizon band' to the EDGE-FOG cloud-bank pass (edge_fog.frag, eid 7158) painting a hardcoded bright-grey u_fogColor ({0.80,0.81,0.83}) that is never dimmed for the mission's night lighting; the HDRI sky it sits against measures dark (~0.27 in the same FBO). When ON, runEdgeFog and the frame-graph executor context scale edgeFogColor_ (and the sibling OOB fog color) by MC2_NIGHT_FOG_DIM_SCALE (default 0.35, tuned to the measured night-sky value) before upload so the horizon cloud bank matches the dark sky. Default-OFF -> unscaled constant uploaded -> byte-identical. The cinematic letterbox bars are drawn by a different path and are untouched."
    },
    // LegacyLitMc2Frame (VEHICLE-NIGHT-LIT-1)
    {
        "MC2_LEGACY_LIT_MC2_FRAME",
        "MC2_LEGACY_LIT_MC2_FRAME",
        EnvVarKind::Feature,
        true,
        "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LEGACY_LIT_MC2_FRAME=0 reverts. THIS GATE IS A CORRECTNESS REQUIREMENT OF THE gvactor.cpp CacheGpuLightData() CALL, NOT AN OPTIONAL ENHANCEMENT -- do not demote it as inert. It previously measured as a no-op for a structural reason: until VEHICLE-LIGHTING-PATH-1, vehicles never populated lightData_ at all, so calc_light took its numLights==0 early-return (lighting.hglsl:232-236) and returned vec3(1,1,1) regardless of frame -- correcting the FRAME of a row holding ZERO lights cannot do anything. It was unreachable, not wrong. Now that the row is populated the frame is load-bearing, and shipping the gather with this OFF would produce confidently-wrong lighting. The shared-program blast radius noted below is now self-limiting: shapes that do not gather still have numLights==0 and return before the frame is read, so only shapes carrying real lights can observe the flip.] VEHICLE-NIGHT-LIT-1: coordinate-frame fix for the legacy mcTextureManager/ShapeRenderer lit program (gos_tex_vertex_lighted.vert) — the program that draws GROUND VEHICLES (GVAppearance::render -> gvShape->Render -> TG_Shape::Render -> addRenderShape -> ShapeRenderer::render). ObjectLights rows from GatherLightsParameters carry MC2-frame light_to_world translations and light_dir, and the two other calc_light consumers feed MC2-frame inputs (mech.vert passes worldMC2 + MC2 normal; static_prop.vert's worldPos is MC2-frame and STATIC-PROP-NORMAL-FRAME-FIX already swapped its normal). gos_tex_vertex_lighted.vert was the last consumer still passing STUFF-frame WorldPos and Normal, so length(vertex_world_pos - light_pos) mixed frames: GetFalloff rejected every POINT/SPOT/INFINITEWITHFALLOFF light and the INFINITE sun NdotL was ~90deg off. Vehicles therefore kept only base_light (static per-vertex aRGBLight seed) + AMBIENT and rendered as near-black silhouettes at night while mechs, buildings, fences and terrain in the SAME light pool were correctly lit — including trucks parked inside a guard-tower cone. When ON, the shader converts position and normal to the MC2 frame (MC2WorldPos, normalize(vec3(-n.x,n.z,n.y))) before calc_light, matching mech.vert/static_prop.vert. Default-ON as of VEHICLE-LIGHTING-PATH-1 -> u_legacyLitMc2Frame=1 -> MC2-frame feed; =0 restores the legacy Stuff-frame feed -> byte-identical to the pre-slice shipped build. NOTE: this program is shared by other legacy lit shapes, so ON changes their lighting too (day included); that shared blast radius is why it is gated."
    },
    // GvLightsOutClear (VEHICLE-NIGHT-LIT-2)
    {
        "MC2_GV_LIGHTSOUT_CLEAR",
        "MC2_GV_LIGHTSOUT_CLEAR",
        EnvVarKind::Feature,
        false,
        "VEHICLE-NIGHT-LIT-2: GVAppearance::update (mclib/gvactor.cpp) calls gvShape->SetLightsOut(true) when a ground vehicle is OBJECT_STATUS_DESTROYED or OBJECT_STATUS_DISABLED but has NO else-branch to clear it, unlike the mech path (mech3d.cpp Mech3DAppearance::update sets both arms). The flag is therefore a one-way latch: once a vehicle is disabled its shape can never be lit again, including after the actor slot is recycled for a live vehicle. When ON, the missing else-branch clears lightsOut so the flag tracks status exactly as the mech path does. Default-OFF -> legacy latch preserved -> byte-identical. This is a latent-correctness fix found during VEHICLE-NIGHT-LIT-1 recon; it is NOT the cause of the reported all-vehicles-dark-at-night defect (the reported trucks are intact, never disabled)."
    },
    // PropLodShadowCast (SHADOW-DECISIONS-1)
    {
        "MC2_PROP_LOD_SHADOW_CAST",
        "MC2_PROP_LOD_SHADOW_CAST",
        EnvVarKind::Feature,
        false,
        "SHADOW-DECISIONS-1: re-enable shadow CASTING for far/impostor static-prop LODs (lod >= 1). TreeAppearance::registerStatic (mclib/bdactor.cpp) calls GpuStaticPropRegistry::setRecipeNoShadow(regIdx, true) for every lod != 0, justified by a comment claiming the shadow depth pass has no alpha discard -- so flat foliage cards would write their whole quad as depth and cast rectangular blob shadows. That justification has been FALSE since b8f764b6 (2026-06-16, alpha-test foliage shadow casters): resolvePropShadowProgram (gos_static_prop_batcher.cpp:8461) binds shadow_static_prop_alpha_atlas / shadow_static_prop_alpha, both default-ON, and both run the same tex_color.a < 0.5 discard the color pass does (shaders/shadow_static_prop.frag). lod>=1 recipes carry ALPHA_TEST identically to LOD0 (SetAlphaTest(true) on every imported LOD), and the per-packet atlas sidecars are indexed by global packet index in finalizeGeometry() so far-LOD packets bind valid layers; a missing layer fails SAFE (bindPropShadowAtlasPacket clears ALPHA_TEST -> opaque depth), never a wrong-layer sample. SCOPE: the suppression only ever affected getDynamicPropShadowInstances (gos_static_prop_registry.cpp:1895), the full-registry replay; flushShadow's camera-visible loop has no noShadow filter, so visible LOD1 types already cast. Turning this ON therefore grows the caster set AT DISTANCE -- the candidate cure for missing distant-tree shadows -- at a cost proportional to far-prop count. Default-OFF -> setRecipeNoShadow still called -> caster set byte-identical to the shipped build. Visual-only; no geometry/objectId/UI/gameplay change."
    },
    // PropBaseLightDir (PROP-BASE-LIGHT-DIR-1)
    {
        "MC2_PROP_BASE_LIGHT_DIR",
        "MC2_PROP_BASE_LIGHT_DIR",
        EnvVarKind::Feature,
        false,
        "PROP-BASE-LIGHT-DIR-1: give the static-prop baked per-vertex base_light seed a directional (hemispherical-wrap) orientation term, so faces pointing away from the mission key light are darker than faces pointing toward it. VALUE = float strength 0..1 (the strength IS the enable; 0.0 == off), clamped. Default 0.0 -> static_prop.vert's `u_propBaseLightDirStrength > 0.0` guard skips the block entirely -> seed == base_light -> BYTE-IDENTICAL to the shipped build. ROOT CAUSE this addresses: the user reported that a building face should be in shadow as the light is from behind. That is not a shadow bug. Prop lighting is base_light + sunColor*ndl + fill, and shadow_screen.frag modulates the SUN TERM ONLY -- but a face with the light behind it already has ndl == 0, so there is nothing for a shadow to attenuate. MC2_PROP_SHADOW_RECEIVE was measured at <=0.05% of pixels by day and 0.000% at night for exactly this reason (.claude/SHADOW-DECISIONS-1-STATUS.md section 2). The face is bright because of the ORIENTATION-FREE part of the sum. MEASURED (PROP-BASE-LIGHT-DIR-1): that part is NOT the baked base_light seed, which is effectively zero on these props (scaling it by 0.0 is byte-identical on every pose tried); it is ambientTerm, the per-actor object-light AMBIENT gathered from the ObjectLights row, which is orientation-free by construction. The factor is therefore applied to the whole orientation-free portion (seed + ambientTerm). MECHANISM: factor = mix(1 - strength, 1, 0.5 + 0.5*dot(N, u_terrainSunMC2)), multiplied into the seed. factor <= 1.0 always, with equality only for a face pointing AT the sun, so the term REDISTRIBUTES and can never brighten -- it composes safely with MC2_STATIC_PROP_IBL_NIGHT_SCALE and does not worsen the separate over-bright-at-night complaint. Multiplicative on the existing per-vertex seed, so authored variation is preserved scaled rather than tone-flattened. No double-count with MC2_STATIC_PROP_IBL_SH: that probe is an ADDITIVE ambient irradiance summand, this is a MULTIPLICATIVE modulation of a different summand, and the IBL contribution is never scaled. Applied AFTER the FAR-BACKDROP-LIT mix has produced seed (static_prop.vert computes the mix first, then multiplies at the lit assignment); an earlier version of this entry said BEFORE, which never matched the shipped code. Behaviourally moot while MC2_FAR_BACKDROP_LIT defaults to 0. NEVER-BRIGHTENS CLAIM, RE-VERIFIED (MC2-05-BRIGHTEN-1): a report that this gate brightens mc2_05 by up to +71 percent did NOT reproduce and was a confounded measurement, not a shader defect. The two mc2_05 liao_compound poses used for it carry a 15 percent gate-OFF/gate-OFF noise floor, not the 0.000 percent recorded, because night object light pools are dimmed near movers (MC2_LIGHT_POOL_MOVER_DIM) and movers do not resolve twice; with MC2_TERRAIN_LIGHT_POOLS=0 those poses drop to a 0.000 percent floor and the gate darkens strictly. Re-graded across 24 poses on mc2_01/03/05/10/24 at a 0.000 percent floor: brighter-pixel fraction is 0.00 percent everywhere. Alpha-test instances (foliage/fence cards) are EXCLUDED because they are two-sided -- the ndl branch already uses abs() for that reason -- and a signed wrap would darken an arbitrary side. Implemented in the shader rather than the CPU bake because far/impostor LODs register via TransformMultiShape_HierarchyOnly and never run the per-vertex light bake, so a bake change could not reach them. Visual-only; no geometry/objectId/UI/gameplay change."
    },
    // PropFoliageAlphaFlag (FOLIAGE-ALPHAFLAG-1)
    {
        "MC2_PROP_FOLIAGE_ALPHAFLAG",
        "MC2_PROP_FOLIAGE_ALPHAFLAG",
        EnvVarKind::Feature,
        false,
        "FOLIAGE-ALPHAFLAG-1: make the per-instance flags bit 2 emitted by the static-prop RECIPE paths (BldgAppearance/TreeAppearance::registerStatic, mclib/bdactor.cpp) carry its actual documented meaning, kFlagIsAlphaTest, sourced from TG_TypeShape::alphaTestOn via the new TG_Shape::GetIsAlphaTest() seam. THE BUG: both recipe sites still built the word with `if (child->GetIsSpotlight()) flags |= (1u << 2)`, the meaning bit 2 carried BEFORE T3.2 repurposed it; and because spotlight children are `continue`-skipped about ten lines above both sites, bit 2 was never set on the recipe path at all. Two shader features keyed off it were therefore dead on that path: (1) the alpha-test EXCLUSION inside the MC2_PROP_BASE_LIGHT_DIR wrap term (static_prop.vert:800), so foliage was modulated by a directional term explicitly designed to exclude it -- note the MC2_PROP_BASE_LIGHT_DIR registry entry already ASSERTS that exclusion, a claim this gate is what makes true on the recipe path; and (2) two-sided abs(NdotL) foliage lighting (static_prop.vert:731). WHY THE GATE LIVES AT THE EMISSION SITE AND NOT IN THE SHADER: GpuStaticPropBatcher::submitMultiShape already sets bit 2 correctly from the same alphaTestOn source (gos_static_prop_batcher.cpp:5575), so two-sided foliage lighting is ALREADY LIVE on that path in the shipped build; gating the shader-side abs() branch would have CHANGED shipped pixels rather than preserved them. Gating the recipe-path emission instead keeps default-OFF byte-identical BY CONSTRUCTION (the recipe path emits the same zero bit it emits today; submitMultiShape is untouched) and makes gate-ON CONVERGE the two paths rather than diverge them. NO DOUBLE-CLASSIFICATION: the alpha-test shadow class (GBuffer1.a = 0.1875, PROP-ALPHA-SHADOW-CLASS-1, chosen inside the shoreline shader's 0.15..0.35 water band) is driven by the per-PACKET materialFlags baked at registerType from the same alphaTestOn (gos_static_prop_batcher.cpp:2894) -- an independent channel from this per-INSTANCE flags word -- so this gate cannot double-classify and cannot move shipped shoreline pixels. Visual-only; no geometry/objectId/UI/gameplay change."
    },
    // LightPosColumnFix (LIGHT-POS-COLUMN-1)
    {
        "MC2_LIGHT_POS_COLUMN_FIX",
        "MC2_LIGHT_POS_COLUMN_FIX",
        EnvVarKind::Feature,
        true,
        "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LIGHT_POS_COLUMN_FIX=0 reverts. ITS PRIOR 1/255 'SUB-PERCEPTUAL' MEASUREMENT NO LONGER CHARACTERISES THIS GATE and must not be cited as grounds to demote it: that number was taken at sun-dominated fixture poses, where the only meaningful contributor is the INFINITE sun, which has no position term and is structurally immune to this bug -- it measured the one condition under which the fix cannot matter. It becomes load-bearing now that vehicles carry a populated light row: vehicles are ground-level MOVERS sitting close to exactly the classes this bug breaks (POINT explosions/bolts, SPOT turret lamps and guard-tower cones), all previously evaluated at the world origin. Needs re-measurement at vehicle-adjacent night poses, not at the old fixtures.] LIGHT-POS-COLUMN-1: deliver the object-path light's WORLD POSITION to the GPU. THE BUG: Stuff::AffineMatrix4D is 3 columns x 4 rows indexed entries[(column<<2)+row] (mclib/stuff/affinematrix.hpp:141), and BuildTranslation writes (W_Axis, {X,Y,Z}_Axis), so the world translation lives at entries[3]/[7]/[11]; BuildRotation only rewrites rows 0..2 (affinematrix.cpp:162-194) so the BuildTranslation-then-BuildRotation producers (bdactor.cpp:3435 building point lights and :3673 building spotlights, carnage.cpp:742 explosion lights, turret.cpp:917 turret SPOT, weaponbolt.cpp:1451 weapon-bolt lights) genuinely leave a correct position there. GatherLightsParameters (mclib/txmmgr.cpp) memcpys those 12 floats -- landing translation at GPU floats [3]/[7]/[11], i.e. m[0].w/m[1].w/m[2].w under column-major mat4 -- and then EXPLICITLY ZEROES floats [12][13][14]. Every consumer decodes the light origin as m[3].xyz == floats [12][13][14]: shaders/include/lighting.hglsl:246,256,274 (POINT / SPOT / INFINITEWITHFALLOFF), shaders/vulkan/mech_tier1.vert:110,117,124, and shaders/lightgrid_build.comp:112. NET EFFECT IN THE SHIPPED BUILD: every object-path POINT / SPOT / INFINITEWITHFALLOFF light is evaluated as if it sat at the world origin, so its falloff is a function of distance-from-origin rather than distance-from-lamp. WHY THE FIX IS AT THE UPLOAD AND NOT IN THE SHADERS: GatherLightsParameters is the single chokepoint feeding all seven read sites across three shader languages (GLSL, SPIR-V, compute), so one edit corrects them together and requires no SPIR-V rebake. It also cannot double-correct: the value currently uploaded is the constant (0,0,0), which carries no information, so no downstream consumer can be compensating for it. NOT AFFECTED: lightDir is gathered separately via GetLocalForwardInWorld and is already correct; AMBIENT lights upload Identity (translation already zero) and the GLSL AMBIENT case returns before reading position; INFINITE has no position term. The CPU mirror (mclib/msl.cpp:1792) sources POINT position from TG_Light::direction, a different field, and never reads this matrix -- so this gate moves the GPU path TOWARD the CPU path, not away from it. Default ON as of VEHICLE-LIGHTING-PATH-1 -> correct world position uploaded; MC2_LIGHT_POS_COLUMN_FIX=0 takes the zeroing branch -> upload byte-identical to the pre-slice shipped build. Visual-only; no geometry/objectId/UI/gameplay change."
    },
    // VehicleLitPath (VEHICLE-LIGHTING-PATH-1)
    {
        "MC2_VEHICLE_LIT_PATH",
        "MC2_VEHICLE_LIT_PATH",
        EnvVarKind::Feature,
        true,
        "VEHICLE-LIGHTING-PATH-1: put GROUND VEHICLES on the same lighting path as every other object class. THE BUG: every other class populates its per-object light row via CacheGpuLightData() before drawing -- mech3d.cpp:5614, bdactor.cpp:3144/:3181/:4821 (buildings), :6566 (trees), genactor.cpp:1256 (generic props). GVAppearance called NEITHER CacheGpuLightData() NOR GpuStaticPropBatcher::submitMultiShape(), yet renders unconditionally through the legacy TG_Shape::Render path, which consumes lightData_ at tgl.cpp:3508 with no gather and no eligibility guard. (Buildings reach that same legacy Render only as an `if (!submittedToGpu)` fallback at bdactor.cpp:2313, which they normally never take -- so vehicles were the only class actually exercising an ungathered row.) CONSEQUENCE CHAIN, each link verified from source: (1) TG_HWLightsData() zero-inits numLights_ (tgl.h:339); (2) addLightDataStructure is a pure dedup/append with NO back-fill (txmmgr.cpp:1929-1988), so the all-zero row is stored verbatim; (3) calc_light takes its numLights.x==0 early-return, and because MC2_STATIC_PROP_LIGHTING is NOT defined for this shader (only mech.vert:15 and static_prop.vert:20 define it) it returns vec3(1,1,1), discarding even the per-vertex base_light (lighting.hglsl:232-236); (4) ENABLE_VERTEX_LIGHTING is 1 globally (lighting.hglsl:8) so that white reaches the fragment stage; (5) gos_tex_vertex_lighted.frag computes c.xyz = c.xyz * lighting. NET EFFECT: vehicles rendered at RAW ALBEDO x 1.0, completely unlit, in day AND night. WHY THIS LOOKED LIKE AN ALBEDO PROBLEM AND IS NOT: mechs and buildings receive ambient + sun*ndl, which exceeds 1.0 in daylight, so a vehicle texture measured at the 49th percentile of the corpus (trucksprop.tga masked luminance 85.9 vs corpus median 86.2) still looks dark parked beside a mech. The albedo theory was measured and correctly retracted in fc43338a; this gate reconciles that measurement with the user-visible symptom. Do not reopen it. WHY NOT ROUTE VEHICLES THROUGH GpuStaticPropBatcher INSTEAD: static_prop.vert:713 deliberately omits point/spot lights (\"negligible for buildings\"), so that would match buildings but still not mechs; and vehicles are movers, so pushing them through a static-transform batcher would collide with in-flight static-prop work. Instead this routes them into the SAME full 6-type calc_light (ambient/infinite/falloff/point/spot) that mech.vert:219 uses -- a strict superset of the building path. COUPLED GATES, both promoted to default-ON in the same slice and both load-bearing only once this one is on: MC2_LEGACY_LIT_MC2_FRAME (frame correctness for the now-populated row) and MC2_LIGHT_POS_COLUMN_FIX (POINT/SPOT world positions, which vehicles as ground-level movers are far more exposed to than the sun-dominated fixture poses where it was originally measured). Also adds the night light-pool term to gos_tex_vertex_lighted.frag -- the only object shader that lacked it -- uploaded via the new gos_UploadObjectLightPools seam. Default-ON; MC2_VEHICLE_LIT_PATH=0 restores the all-zero row and the shipped unlit-vehicle output. Visual-only; no geometry/objectId/UI/gameplay change."
    },
    // OVERLAY-CEMENT-ADOPT-1 (CONCRETE-DELIVER-1): the DELIVERED cure for the
    // concrete patchwork. Default ON. Supersedes MC2_OVERLAY_CEMENT_SHADOW_MATCH.
    {
        "MC2_FEATURE_OVERLAY_CEMENT_ADOPT",
        "MC2_OVERLAY_CEMENT_ADOPT",
        EnvVarKind::Feature,
        true,
        "OVERLAY-CEMENT-ADOPT-1: closes the two-shader concrete seam CEMENT-PATCHY-CAUSE-1 attributed but explicitly left open as a design call ('either the overlay adopts the cement atlas + flat light on cement tiles, or the cement terrain adopts the overlay's shading there'). The user has since made that call, twice and explicitly: 'the transition tiles should just use the same concrete texture', and mc2_01's on-concrete runway tone is the reference. THE TERRAIN PATH WINS. FOUR disagreements existed on one physical surface, not the one that was gated. chunk: base = cementAtlas * u_missionFlatLight (:985), normalLight forced 1.0 on concrete (:1868-1889), shadow -> dynamicShadow only (:1937), and NO distance fog anywhere in the shader. overlay: a DIFFERENT texture times a hardcoded vec3(0.82,0.80,0.76) ~18-20%% darkening, shadow = staticShadow * dynamicShadow, and an IN-SHADER distance fog toward (0.58,0.65,0.75) clamped at 0.70. THE MISSED TERM IS THE FOG, and CEMENT-PATCHY-CAUSE-1's own data was already showing it: (a) its measured seam step GROWS with camera distance (runway_ground -4.95 L, runway_mid -16.93 L, runway_overview -14.81 L) and fog is the only distance-dependent term in the difference set; (b) its RenderDoc pixel values are blue-shifted on the overlay side ONLY -- overlay eid 3678 wrote [0.301,0.3083,0.3623] with B>R while terrain eid 408 wrote [0.1515,0.1836,0.1495] with B lowest -- and the overlay's own albedo tint is WARM (B lowest), so it cannot produce a blue-dominant pixel, while the fog colour is blue-dominant. The overlay layer is DOUBLE-FOGGED: its own in-shader fog plus the shared screen-space edge_fog/fog_oob both layers receive, so it drifts lighter and bluer than the terrain concrete with distance. That is the patchwork at normal gameplay altitude, and it is why every atlas-side lever failed. MECHANISM, keyed off the cement word already bound at binding 25 for PAD-AREA-GLOW-1 (no new binding, no new plumbing on the class channel): bit31 set + bit24 CLEAR = genuine concrete transition tile -> adopt the cement ATLAS albedo with the identical UV math and flat light the chunk uses, drop the warm tint, drop staticShadow, skip the in-shader fog; bit31 set + bit24 SET = authored road/runway art on a cement base -> KEEP its albedo, reconcile LIGHTING ONLY (repainting here would be the albedo swap the user rejected twice as 'WAY worse'); bit31 clear = not cement or class unknown -> LEAVE ENTIRELY ALONE, ambiguity always resolves to do-nothing. Alpha always comes from the decal texture so the authored marching-squares transition SHAPE survives untouched; only the colour inside it changes. cementWord bit 24 (kCementPavedArtBit) is new, is only ever set alongside bit 31, and no pre-existing reader masks or compares it. Uploaded from uploadOverlayUniforms_, the single chokepoint all binders route through, so BOTH overlay bind sites are covered and MC2_TERRAIN_INDIRECT_OVERLAY=0 cannot silently lose it. DEFAULT ON. MC2_OVERLAY_CEMENT_ADOPT=0 writes no bit 24 and uploads 0, short-circuiting before any SSBO read => byte-identical stock. Inert by construction on the legacy terrain path (u_padGlowMapSide == 0). NOTE: this WIDENS the separate mc2_03 'terrain outside the pads is near-black' contrast, because it makes concrete more uniformly flat-lit while surrounding terrain keeps mix(0.35,1.20,diffuse) with diffuse clamped at 0.02 -- that is a terrain normalLight/ambient-floor issue, tracked separately, deliberately NOT folded in here."
    },
    // VehicleAmbientFloor (VEHICLE-AMBIENT-FLOOR-1)
    {
        "MC2_VEHICLE_AMBIENT_FLOOR",
        "MC2_VEHICLE_AMBIENT_FLOOR",
        EnvVarKind::Feature,
        true,
        "VEHICLE-AMBIENT-FLOOR-1: DEFAULT-ON. Completes VEHICLE-LIGHTING-PATH-1 by giving the legacy lit vehicle path (shaders/gos_tex_vertex_lighted.vert) the TWO terms mech.vert has and it never did: the kAmbientFloor = 0.35 seed handed to calc_light (mech.vert:204,:218-222) and the clamp(...,0,1) on the result (mech.vert:223). WHY THE PREVIOUS SLICE WAS NECESSARY BUT NOT SUFFICIENT: VEHICLE-LIGHTING-PATH-1 correctly diagnosed and fixed 'vehicles carry no light row', which is why they rendered at raw albedo. It did NOT fix the sentence the user actually wrote -- 'the truck is DARK, while the mech next to it is lit' -- because the two classes still evaluate DIFFERENT expressions off the same row. Vehicle: M = base_light + A + S*ndl, unclamped. Mech: M = clamp(0.35 + A + S*ndl, 0, 1). WHAT base_light ACTUALLY IS -- MEASURED, and NOT the 'always zero' it looks like from mclib/tgl.cpp:1093 alone: that line only SEEDS aRGBLight to 0xff000000, and the ASE loader overwrites it from *MESH_VERTCOL whenever *MESH_NUMCVFACES is present (tgl.cpp:1319-1448), which every stock vehicle ASE has; the binary .tgl loader then reads the TG_TypeVertex array verbatim (tgl.cpp:641) so the baked value ships. Counted over all 8 stock ground-vehicle .tgl binaries: 0xff000000 (0.0000) on 56-110 verts/file, 0xff010101 (0.0039) on 3-6, and 0xff646257 (0.3922) -- the ONLY value above the 0.35 floor -- on at most 2. The bright 0xffece7cc (0.9255) entry exists in the ASE palettes but no CFACE references it, so it reaches zero vertices. Net: the floor raises 97%+ of vehicle vertices and is inert on at most two per vehicle. A = AmbientRed/255 (the TG_LIGHT_AMBIENT slot, mclib/camera.cpp:516 + :2418); S = getLightRed(land->getTerrainLight(pos))/255 = min(LightRed*I + AmbientRed, 255)/255 (mclib/camera.cpp:4392-4408), the INFINITE sun slot whose per-vehicle colour is written at mclib/gvactor.cpp:2611-2620; I = terrainNormal . lightDirection with lightDirection.z = sin(pitch)/sqrt(1+sin^2 pitch) (mclib/camera.cpp:2244-2248, mclib/mapdata.cpp:1892-1900). MEASURED CONSEQUENCE ON THE MAP THE USER NAMED: mc2_10 authors AmbientRed 192 and LightDirPitch 180, so A = S = 0.7529 and the vehicle sits at 0.753 on shadow faces against the mech's 1.0 -- the reported symptom SURVIVING the fix meant to cure it. On mc2_03 (AmbientRed 31, LightRed 242, pitch 46 -> A = 0.1216, S = 0.6745) the vehicle PEAK is 0.796, below 1.0 at every possible normal, so every vehicle pixel darkened -- which falsified VEHICLE-LIGHTING-PATH-1's own pre-registered prediction that vehicles would brighten toward the mechs on the mc2_03 airfield in daylight. WITH THIS ON the vehicle expression is arithmetically IDENTICAL to the mech expression for the same light row, on every map, by construction rather than by tuning. WHY max() AND NOT A BARE vec3(0.35): mech.vert can hand calc_light a bare vec3(kAmbientFloor) because it has no per-vertex seed to lose; this shader does -- get_base_light still implements the hot-colour lit-window / lamp conventions -- so the floor is applied as max(base_light, vec3(0.35)), which can only raise the seed and never discards it. WHY THE CLAMP SHIPS WITH THE FLOOR AND NOT SEPARATELY: without it a vehicle on a bright-ambient map overshoots the mech beside it (mc2_10 lit faces reach 1.506 against the mech's 1.0, and the object target is RGBA16F so the excess is NOT free), turning the reported mismatch into its mirror image. One gate, one revert. SELF-LIMITING BLAST RADIUS: this program is shared with other legacy lit shapes, but a shape that never calls CacheGpuLightData still has numLights == 0, and calc_light returns vec3(1,1,1) for it BEFORE reading the seed (shaders/include/lighting.hglsl:232-236; the MC2_STATIC_PROP_LIGHTING branch is not taken here), and clamp(vec3(1),0,1) == vec3(1) -- so non-gathering shapes are bit-identical with this ON. Only shapes carrying a real light row can observe the flip, which after VEHICLE-LIGHTING-PATH-1 means ground vehicles plus the rare building that takes the !submittedToGpu fallback at bdactor.cpp:2313 (which lands it on the same treatment static_prop.vert already applies). WHERE THE NIGHT LAMPS SURVIVE THE CLAMP: the light-pool term is added in gos_tex_vertex_lighted.frag AFTER the vertex stage and is NOT clamped, exactly as in mech.frag -- so a truck under a guard-tower lamp still brightens even where ambient alone saturates the vertex multiplier. FALSIFICATION ARM: on a high-authored-ambient map the floor pushes the vertex multiplier to the clamp and vehicle shading goes FLAT (mc2_10: 0.35+0.7529 = 1.1029 -> 1.0 at every normal). That is not a defect of this gate -- mech.vert computes the same 1.0 there and always has, so flat is PARITY, and it is also what vehicles showed before VEHICLE-LIGHTING-PATH-1. The arm that would make this gate WRONG is a vertex whose base_light already exceeds 0.35 AND whose lit multiplier already exceeded 1.0, since then the floor is inert and only the clamp fires (net darkening). That arm is LIVE on stock content, not hypothetical -- but it is bounded and was measured rather than waved away: at most 2 vertices per stock vehicle sit above the floor, all at the single value 0.392, so on the worst map (mc2_10, A = 0.7529) those vertices go 1.145 -> 1.0, a 1.15x cap, while the other 97%+ go 0.753 -> 1.0, a 1.33x LIFT. After the change every vertex lands at 1.0 there: uniform, equal to the mech, and equal to what vehicles showed before VEHICLE-LIGHTING-PATH-1. Imported/BT2018 shapes carrying richer authored VERTCOL data can exceed that bound and are the first population to check if anything looks dimmer. Visual-only; no geometry/objectId/UI/gameplay change. MC2_VEHICLE_AMBIENT_FLOOR=0 -> u_vehicleAmbientFloor=0 -> no floor, no clamp -> byte-identical to the pre-slice shipped build."
    },
    // LightRowPure (LIGHT-ROW-PURE-1, ARCH-LIGHTING slice 1)
    // kFeatureTable is indexed by RendererFeature, so this entry's position is
    // its enum value (57).
    {
        "MC2_LIGHT_ROW_PURE",
        "MC2_LIGHT_ROW_PURE",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF, KILLSWITCH INVERTED: MC2_LIGHT_ROW_PURE=1 opts IN to the pure producer; unset/0 is the legacy in-place packing loop in GatherLightsParameters, byte-for-byte, i.e. the shipped build is unchanged. WHY OFF: tools/object_light_row_harness proves the ALGORITHM byte-identical but cannot cover the engine-coupled adapter (flattenLight(): TG_Light + Stuff::LinearMatrix4D -> LightInput), because the harness deliberately links no engine; game launches are blocked, so IN-ENGINE byte-identity is UNPROVEN and an unproven default-ON is the documented-default-lie class this repo keeps finding. FLIP CRITERION: one run with MC2_LIGHT_ROW_PURE=1 MC2_LIGHT_ROW_PURE_VERIFY=1 reporting checked>0 mismatched=0 across tier1 -- that is the missing evidence and nothing else is.] LIGHT-ROW-PURE-1: object light-ROW PRODUCTION is delegated to one pure function, object_light_row::BuildObjectLightRow (mclib/object_light_row.h). The row that ships is built from an EXPLICIT query -- light list + DECLARED per-class policy (sun-slot recolour / ambient-slot recolour / sun-disabled) -- instead of being a by-product of the per-class mutate-the-shared-eye-light-list, gather, restore dance. SCOPE: row PRODUCTION only. It does NOT touch shading, and deliberately does not touch terrain, water, VFX or UI lighting: no lighting formula, ambient floor, clamp or sun-direction policy changes, so the Gouraud-over-1998-vertex-colours look the user has ratified is untouched by construction. BYTE-IDENTITY IS THE ACCEPTANCE BAR and is proven three ways: (1) offline by tools/object_light_row_harness, which memcmps the 3600-byte row built from an UNMUTATED light list plus the declared policy against the row built by MUTATING the list and gathering with no policy, across every class policy, light counts 0..42, every single-light hole position and every posColumnFix/clampFixture combination; (2) live by MC2_LIGHT_ROW_PURE_VERIFY, which runs both producers on the same frame and memcmps; (3) structurally, the pure packer is a transliteration of the loop it replaces and the legacy loop still runs ahead of it, so =0 is an exact revert. WHAT IT DOES NOT DO -- and must not be described as doing: it does NOT remove the per-class mutations of the shared light list, so it does NOT by itself close PROP-BAKE-ORDER-VS-TIME-1 (mclib/bdactor.cpp:3134-3156). A reader census over this tree found the mutations are load-bearing for a consumer a row memcmp is structurally blind to: TG_Shape::MultiTransformShape, the CPU per-vertex lighting kernel (mclib/tgl.cpp:2189-2192), reads the same TG_Light objects' active/GetaRGB and runs INSIDE TreeAppearance's sun-off window (mclib/bdactor.cpp:7139, and :7103 under dual-emit). That kernel is live in this build -- useVertexLighting is true (mclib/terrain.cpp:353) and Environment.Renderer is 0, not 3 (code/mechcmd2.cpp:2974) -- so deleting the mutations would change baked vertex colours (listOfColors / listOfTriangles[].aRGBLight) while every gathered row stayed byte-identical. The mutations therefore stay and the policy is published as a DECLARATION of them; because a recolour publishes the same aRGB the mutation wrote and a disable publishes the same active=false, applying the policy on top of the already-mutated list is idempotent. Visual-only seam; no geometry, objectId, UI or gameplay change.",
    },
    // ShorelineLegacySuppress (SHORELINE-GLOW-1)
    // kFeatureTable is indexed by RendererFeature, so this entry's position is
    // its enum value (58).
    {
        "MC2_SHORELINE_LEGACY_SUPPRESS",
        "MC2_SHORELINE_LEGACY_SUPPRESS",
        EnvVarKind::Feature,
        true,
        "[DEFAULT FLIPPED OFF -> ON by LEGACY-SHORELINE-MISAIM-1 (2026-07-31). SHORELINE-GLOW-1's 'WHY OPT-IN' argument below is preserved verbatim and is now ANSWERED, not deleted, so the reversal can be judged against the original reasoning. It said the change was predicted from source rather than observed and that it removes brightness from prop edges while a dark-props complaint is open. Both remain true. What changed is the weighing: the brightness being removed is an ANIMATED one -- shoreline.frag scrolls its fbm by `time` and pulses it with a sin(time*2.0) term -- so the shipped behaviour is a shimmer crawling along the silhouette of a STATIC building, and no grading pass would have kept that whatever it decided about edge brightness. Holding a mis-aimed pass on by default while waiting for a capture that cannot be taken is the same bargain that produced the incapable-fix list this flip comes from. THE PASS IS NOT DELETED and this is not a no-op gate: setShorelineSuppressedByTerrainMask() is still called ONLY from the LOD-chunk terrain draw, so under MC2_TERRAIN_LOD_CHUNK=0 the setter never runs and the pass runs unsuppressed. [CORRECTED by SHORELINE-DISPOSITION-1 (2026-07-31) -- this sentence used to end '...the legacy terrain shader is what draws, gos_terrain.frag's continuous materialAlpha DOES land in the 0.15-0.35 band, and the pass still runs and still foams water -- the one configuration it was written for.' THAT WAS FALSE, and it was the stated reason the pass was kept rather than deleted, so it is corrected rather than left standing. gos_terrain.frag's two GBuffer1 writes (:1103, :1156) are inside #ifdef MRT_ENABLED; '#define MRT_ENABLED' is injected from exactly ONE C++ site (gameos_graphics.cpp:495, getMaterialVariation -> gosRenderMaterial::load), so those lines are preprocessed OUT of the thin/surface/mask_solid programs; and the one program that does get the macro, terrain_material_, is created and destroyed but NEVER apply()'d -- its draw was removed by TERRAIN-DEADCODE-DELETE-1 (75b8ebefe). So there is NO configuration in which the 0.15-0.35 classifier matches a water pixel. Under MC2_TERRAIN_LOD_CHUNK=0 the pass does not foam water; it applies the prop-silhouette rim unsuppressed. '=0' restores that rim, not shoreline foam. Pinned by Census 4 of scripts/check-render-contract-gbuffer1.sh. DISPOSITION: delete the pass; see that script's Census 4 block and gos_TerrainShorelineLegacySuppressOn()'s CORRECTION block.] ALSO CHANGED: the suppression no longer ANDs with gos_TerrainLodChunk_IsShorelineMaskActive() (MC2_TERRAIN_SHORELINE). That coupling encoded 'do not brighten the seam twice', and since the pass cannot reach a water pixel here there is no second brightening; keeping the AND would have meant MC2_TERRAIN_SHORELINE=0 silently restored the prop rim. '=0' restores the legacy pass exactly as it shipped. FLIP-BACK CRITERION: any mission with buildings or fences near terrain -- mc2_01 and mc2_03 both qualify -- comparing unset against MC2_SHORELINE_LEGACY_SUPPRESS=0 on a MOVING camera, because the artifact is temporal and a single still frame understates it.] SHORELINE-GLOW-1: let the active V3 terrain shoreline band actually SUPPRESS the legacy screen-space runShoreline() post pass. THE BUG THIS SITS ON TOP OF, fixed unconditionally in the same commit: MC2_TERRAIN_SHORELINE had THREE independent readers and SHORELINE-SMOOTHING-REVISIT-1's default flip updated only two. gos_TerrainLodChunk_IsShorelineMaskActive() kept the pre-flip presence test, so it answered false on every stock launch -- and it is the ONLY input to setShorelineSuppressedByTerrainMask(), which is the ONLY suppression term in runShoreline() (gos_postprocess.cpp:3403) and executorShorelineWillRun() (:7079). shorelineEnabled_ is constructed true (gos_postprocess.cpp:769) and is otherwise moved only by a debug key and a GUI checkbox, and sceneHasTerrain_ is set by the chunk terrain draw itself, so the legacy pass has been running on every stock frame with terrain while the V3 band ran too. All three readers now resolve through one accessor, gos_TerrainShorelineGateOn(), so that drift cannot recur. WHAT THE LEGACY PASS ACTUALLY TOUCHES -- this CORRECTS the description carried by the gate-defaults allowlist entry and by the killswitch note in docs/tier1_env_vars.md, both of which say it glows the water edge. It cannot. shaders/shoreline.frag:22 selects pixels by GBuffer1.a in (0.15, 0.35), and on the production path nothing water-like lands in that band: the MDI water frag (gos_terrain_water_mdi.frag:427/439/496) and the non-MDI water fast-path frag (gos_tex_vertex.frag:77) both write rc_gbuffer1_screenShadowEligible(), alpha 0.0; terrain_lod_chunk.frag writes rc_gbuffer1_shadowHandled_flatUp(), alpha 1.0; the MRT1 clear is alpha 0.0 (gos_postprocess.cpp:3132). A census of every fragment shader that writes attachment 1 leaves exactly two producers inside the band -- static_prop.frag at 0.25 (solid prop) and 0.1875 (alpha-test lattice) -- plus gos_terrain.frag's continuous materialAlpha, which belongs to the LEGACY terrain shader and never reaches a framebuffer in ANY configuration (SHORELINE-DISPOSITION-1: this clause used to read 'is not drawing while MC2_TERRAIN_LOD_CHUNK is default-ON', which wrongly implied it draws when that gate is OFF -- it does not; the write is behind #ifdef MRT_ENABLED and the only program that receives that macro is never bound). So the pass has been applying an animated fbm foam brighten, up to 1.4x multiplicative, to STATIC PROP pixels that have terrain within about six screen texels: a shimmering rim where buildings and fences meet the ground. Turning this gate ON removes that rim; it does not change one water pixel. WHY OPT-IN AND NOT DEFAULT-ON: the change is predicted from source rather than observed, and it REMOVES BRIGHTNESS FROM PROP EDGES while a user-reported dark-props complaint is open, so it wants one graded capture before it ships on. [SUPERSEDED TAIL, corrected by LEGACY-SHORELINE-MISAIM-1: this row used to end 'Unset -> setShorelineSuppressedByTerrainMask(false) every frame -> byte-identical stock'. That is no longer true and keeping it would be exactly the documented-vs-actual lie this registry exists to prevent. Unset now suppresses; MC2_SHORELINE_LEGACY_SUPPRESS=0 is the byte-identical-to-the-shipped-build arm.] Visual-only; no geometry, objectId, UI or gameplay change.",
    },
    // SunAxisFix (SUN-AXIS-FIX-1)
    // kFeatureTable is indexed by RendererFeature, so this entry's position is
    // its enum value (59).
    {
        "MC2_SUN_AXIS_FIX",
        "MC2_SUN_AXIS_FIX",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF, OPT-IN: MC2_SUN_AXIS_FIX=1 enables; unset/0 is the shipped expression bit-for-bit. WHY OFF DESPITE BEING A CORRECTNESS FIX -- the argument, not a shrug: (a) engine launches are hard-blocked in this lane, so NO HUMAN HAS SEEN THIS RENDER, and an unproven default-ON visual change is the documented-default-lie class this repo keeps finding; (b) it is LARGE -- 22 of the 24 shipping missions move, up to 0.53 of full scale on the mech/vehicle multiplier; (c) it is NOT MONOTONE, so it cannot be justified as 'the user wanted brighter mechs': measured over 4096 sphere normals per mission, almost exactly as many normals get DARKER as brighter (e.g. mc2_13 47.7% brighter / 48.0% darker, mc2_03 44.2 / 44.1). A fix that redistributes light rather than adding it is exactly the kind whose merit only the user's eyes can settle. FLIP CRITERION: one side-by-side on any unsaturated mission (mc2_03 and mc2_13 carry the largest delta; mc2_19 moves on 97.0% of normals) with MC2_SUN_AXIS_FIX=1 versus unset. That is the missing evidence and nothing else is.] SUN-AXIS-FIX-1: ObjectLights.light_dir is a STUFF-FRAME vector and the DIFFUSE consumers of calc_light dot it against MC2-FRAME normals. THE DERIVATION, read from the code that runs and not from any comment: the row's light_dir comes from GetLocalForwardInWorld() on the sun's light-to-world (mclib/object_light_row.h:123,:265-267 <- mclib/txmmgr.cpp:2621), which is built as BuildRotation(EulerAngles(lightPitch, lightYaw+135, 0)) at mclib/camera.cpp:509/:1860/:2110/:4252. LinearMatrix4D::BuildRotation forwards to AffineMatrix4D::BuildRotation (mclib/stuff/linearmatrix.hpp:85-86 -> mclib/stuff/affinematrix.cpp:162-194) -- NOT Matrix4D::BuildRotation in mclib/stuff/matrix.cpp, which an inherited note mis-cited; the two bodies happen to be arithmetically identical for EulerAngles, so the result stands but the citation did not. With roll==0, matrix ROW 2 is (cosP*sinY, -sinP, cosP*cosY), and GetLocalForwardInWorld returns exactly that row because FORWARD_AXIS==Z_Axis==2 and APPLY_FORWARD_SIGN is the identity (mclib/stuff/vector3d.hpp:55,:73). THE CANCELLATION that hid this: applying the standard Stuff->MC2 swap (-x, z, y) and negating for the toward-sun convention yields -swap(row) == (cosP*cos a, cosP*sin a, sinP) with a = lightYaw+45 -- the TRUE spherical toward-sun direction. The +90deg between the object light's yaw+135 and the terrain sun's yaw+45 (camera.cpp:2244) is EXACTLY cancelled by the axis swap, verified identically on all 24 shipping missions by tools/class_light_band_harness Stage B/D, offline, no launch. CONSEQUENCE: the consumer that swaps is right and the ones that do not are wrong. mech.vert:279 swaps before using light_dir for SPECULAR and is CORRECT -- this gate does not touch it, and must not, because doing so would both break the one correct consumer and double-apply the correction. The DIFFUSE branches did not swap: with the mech clamp removed and a face pointed straight at the sun, ndl == 0.0 on 14 of 24 missions. That is not '90 degrees off', it is ANTI-CORRELATED on the majority, and the clamp at mech.vert:223 is what has been hiding it. SCOPE: the swap is applied ONLY in the two DIRECTIONAL branches of calc_light (TG_LIGHT_INFINITE and TG_LIGHT_INFINITEWITHFALLOFF -- stock missions author the sun as the latter). POINT and SPOT derive their direction from `to_light`, a position difference, and never read light_dir.xyz -- note the SPOT branch's own comment claims it uses light_dir.xyz as the cone axis and the code beneath it does not, so no cone axis can move. AMBIENT and TERRAIN read no direction. IMPLEMENTATION SHAPE: calc_light gained a 5th parameter and the legacy 4-arg arity survives as a forwarder hard-coding 0, so any shader that has not opted in is byte-identical BY CONSTRUCTION rather than by promise. Opted in: shaders/mech.vert (diffuse only) and shaders/gos_tex_vertex_lighted.vert, the latter ONLY on its u_legacyLitMc2Frame=1 branch, because the correction converts light_dir INTO the MC2 frame and is meaningless against the Stuff-frame normal the =0 branch still feeds; MC2_LEGACY_LIT_MC2_FRAME=0 therefore reverts that shader completely. WHERE THE DELTA IS AND IS NOT VISIBLE: on a mission whose authored ambient alone clears the clamp (0.35 + A >= 1) the multiplier is pinned at 1.0 for every normal and this gate changes NOTHING -- that is mc2_10 (AmbientRed 192, one of the user's regular check missions, ZERO delta measured) and mc2_22. Everywhere else the visible fraction runs 32%-97% of the normal sphere. NOT A NIGHT-DARKENING COMPOUND with MC2_VEHICLE_AMBIENT_FLOOR: on the high-ambient night maps where that gate pins vehicles at 1.0, this one is arithmetically inert for the same reason. KNOWN LATENT COPIES OF THIS SAME DEFECT, DELIBERATELY NOT FIXED HERE -- enumerated so they cannot be lost, because each will re-introduce this bug the day it goes live and it will look exactly like today's: (1) shaders/building_pbr.vert:64 calls calc_light with a world/Stuff-frame Normal and takes the 4-arg forwarder, so it keeps the uncorrected feed; its companion shaders/building_pbr.frag:81 hardcodes a specular light vector normalize(vec3(-0.35, 0.65, 0.55)) that is not the mission sun at all. NOT LIVE: MC2_BUILDING_PBR is default-OFF and requires exactly \"1\" (mclib/bdactor.cpp:240-244), so this is hygiene, not a user-visible defect. Left alone on purpose -- building_pbr is a whole lighting class the architecture lane recommends finishing or deleting rather than maintaining, and widening a user-visible change to patch a shader that may be deleted is the wrong trade. (2) shaders/vulkan/mech_tier1.vert:95-107 is a VERBATIM RE-PORT of calc_light, not an include of it, and its INFINITE branch carries the identical uncorrected dot(normal, -ld.light_dir[i].xyz) against an MC2-frame worldNormal -- so the Vulkan backend has the bug independently and this gate does not reach it. scripts/check-lighting-divergence.py already pins that two calc_light DEFINITIONS exist; that pin is the tripwire. (3) shaders/gos_tex_vertex_lighted.frag:60 and shaders/building_pbr.frag:74 also call calc_light, but both sit in the #else of ENABLE_VERTEX_LIGHTING, which lighting.hglsl:8 hard-defines to 1 -- unreachable today. Visual-only; no geometry, objectId, UI or gameplay change. NOT VISUALLY VERIFIED -- engine launches are blocked in this lane; the evidence is offline arithmetic (tools/class_light_band_harness, 42 tests)."
    },
    // SunElevationFix (SUN-ELEVATION-FIX-1)
    // kFeatureTable is indexed by RendererFeature, so this entry's position is
    // its enum value (60).
    {
        "MC2_SUN_ELEVATION_FIX",
        "MC2_SUN_ELEVATION_FIX",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF, OPT-IN: MC2_SUN_ELEVATION_FIX=1 enables; unset/0 is the legacy three lines bit-for-bit. WHY OFF: this is NOT a terrain-shading detail. Camera::lightDirection is the single source for gos_GetTerrainLightDir, and through it for the terrain NdotL, the static-prop sun (u_terrainSunMC2), the SHADOW projection matrix (GameOS/gameos/gos_postprocess.cpp:455 buildStaticLightMatrix, :3214, :5208, :5502, :5688), SSAO/screen-FX, AND -- via `intensity = terrainNormal . lightDirection` handed to Camera::getLightRed (mclib/mapdata.cpp:1898) -- the SUN COLOUR BYTE of every actor class. So it moves the sun direction and the world shadow direction together, on every mission at once. CORRECTION, verified at source after this entry's first draft claimed otherwise: it does NOT move the PRE-BURNED TERRAIN COLORMAP. burnInShadows (mclib/terrtxm2.cpp:609) reads eye->lightDirection only under an `if (eye)` guard at :816, and its call chain runs inside land->init() at code/mission.cpp:3710 -- BEFORE `eye = new GameCamera` at :5108, with eye set to NULL on teardown at :5541. So at burn time eye is NULL and the bake takes the (0,0,1) straight-up fallback; the mission sun never reaches it. It is additionally one-shot and disk-cached (`if (!burnedIn)` then saveTGAFile, :2141-2145). The colormap is therefore unaffected by this gate in either position. That the first draft of this very entry asserted the opposite is the documented-vs-actual class this repo keeps finding, caught here by reading the call order instead of trusting the call. That is far too large to enable on arithmetic alone while engine launches are hard-blocked and no human has seen the result. FLIP CRITERION: a graded day-mission capture pair (mc2_24 carries the biggest move, 75 deg authored vs 44.0 delivered; mc2_01 50->37.5; mc2_03 46->35.7) plus a look at shadow direction and at the burned colormap, since those change too.] SUN-ELEVATION-FIX-1: Camera::lightDirection (mclib/camera.cpp:2244-2248) builds a vector whose HORIZONTAL part is already unit length and whose z is sin(pitch), and only THEN normalizes the whole thing. The result is not a direction at elevation `pitch` -- its elevation is atan(sin(pitch)). Every mission with a non-zero authored pitch therefore receives a sun LOWER than its .fit asked for. Measured across all 24 shipping missions offline by tools/class_light_band_harness (test terrain_sun_elevation_is_atan_sin_pitch_not_pitch, Stage B5; per-mission table in --test print_stage_d_delta). THIS IS A SEPARATE DEFECT FROM MC2_SUN_AXIS_FIX and is deliberately a separate gate: that one is about the FRAME of the object-light row and touches only mech/vehicle diffuse; this one is about the ELEVATION of the terrain sun and touches nearly everything else. They compose but neither implies the other. DEGENERATE-PITCH GUARD, because three shipping missions author a pitch that is not a solar elevation: for cos(pitch) < 0 the spherical form flips the AZIMUTH by 180 deg against every shipped build, so those missions return the legacy vector VERBATIM. Among the 24 that is mc2_10 (pitch 180) ALONE, and mc2_10 is one of the user's regular check missions -- it is bit-identical with this gate on. cos(pitch) == 0 (mc2_05 and mc2_21, pitch 270) is NOT degenerate: the spherical form collapses to straight down (0,0,-1) and there is no azimuth to flip, though their delivered elevation does move from -45 to -90 (both are below-horizon night either way). THE EPSILON IS LOAD-BEARING AND WAS FOUND THE HARD WAY: cos(270 deg) evaluates to -1.8e-16, not 0, so a bare `< 0.0` guard silently drops mc2_05 and mc2_21 onto the legacy branch; the harness caught that on the first run and now pins it (sentinel_fix_constants). WHAT TURNING IT ON UNIFIES: the corrected terrain sun becomes EXACTLY -swap(ObjectLights.light_dir) -- the vector mech SPECULAR already uses and the one MC2_SUN_AXIS_FIX hands mech/vehicle diffuse -- so terrain, props, mech diffuse, mech specular and vehicles land on ONE sun vector by construction rather than by tuning, on all 23 non-degenerate missions. Neither gate alone achieves that (test fix_b_unifies_terrain_sun_with_the_object_row). Visual-only; no geometry, objectId, UI or gameplay change. NOT VISUALLY VERIFIED -- engine launches are blocked in this lane; the evidence is offline arithmetic."
    },
    // StaticPropLightFreshness (LIGHTROW-FRESHNESS-1)
    // kFeatureTable is indexed by RendererFeature, so this entry's position is
    // its enum value (61).
    {
        "MC2_STATIC_PROP_LIGHT_FRESHNESS",
        "MC2_STATIC_PROP_LIGHT_FRESHNESS",
        EnvVarKind::Feature,
        true,
        "LIGHTROW-FRESHNESS-1: DEFAULT-ON. The DYNAMIC static-prop submit path (GpuStaticPropBatcher::submitMultiShape, GameOS/gameos/gos_static_prop_batcher.cpp) now checks TG_MultiShape::cachedFrame_ against g_mc2FrameCounter before trusting cachedGpuLightIndex_, and RE-GATHERS a row for the current frame when the stamp is stale. THE DEFECT: cachedGpuLightIndex_ names a DYNAMIC light-table slot, and dynamic slot indices are rebased every frame -- MC_TextureManager::resetLightData (mclib/txmmgr.cpp:2137) resets lightDataStructuresCount to the static high-water and clears every dedup map, and its own comment states that stale hash->slot entries from the prior frame 'are invalid'. A slot number captured on frame N therefore names a different actor's row, or no written row at all, on frame N+1. THE ASYMMETRY THIS CLOSES, and the reason this is a correctness fix rather than a look change: the field has THREE consumers and the other two both already check the stamp. TG_MultiShape::ResubmitCachedGpuLightData (mclib/msl.cpp:2147-2149) requires cachedFrame_ == g_mc2FrameCounter and re-gathers at :2171 when it does not hold. GpuStaticPropRegistry's flush (GameOS/gameos/gos_static_prop_registry.cpp:1051) compares getCachedFrame() against the current frame and SUPPRESSES the draw when stale, its comment at :1045 naming the consequence outright -- a slot 'whose content this frame was filled by a different actor -- emitting a draw with that index produces wrong/black lighting'. The declaration of cachedFrame_ itself (mclib/msl.h:315-320) documents the same contract and names the registry as the checker. The dynamic submit path was the one consumer that tested only the 0xFFFFFFFF sentinel and never the frame, so it accepted a prior-frame index verbatim. WHO LANDS ON THAT PATH: animated buildings specifically. BldgAppearance::setGesture calls invalidateStaticRegistration() (mclib/bdactor.cpp:1874) the moment a real animation starts, which drops the actor out of the static registry (which HAS the check) and onto the dynamic submit path (which did not). shaders/static_prop.vert:714 indexes light[] with no bounds test, so a stale or never-written row shades the actor with zeroes: pure black, texture-independent, silhouette-shaped. WHAT HAPPENS WHEN THE CHECK FAILS -- re-gather, never substitute: GatherGpuObjectLightDataOnly() allocates/dedups a row for THIS actor in THIS frame, the same repair msl.cpp:2171 performs. The pre-slice fallback was `lightDataIndex = 0`, a REAL slot rather than a sentinel, which is precisely what made the failure silent; but pointing at some other actor's row is not a smaller error than pointing at a stale one, so no substitution is made in either direction. If no SHAPE_NODE leaf exists to gather through, the actor is DROPPED rather than drawn against an arbitrary slot -- matching the registry's choice at :1051. A range check on the resolved index (>= getLightStructCount()) also drops, because the shader read is unchecked; that arm is an invariant assertion and must read zero. FAIL-LOUD: [LIGHTROW_FRESHNESS v1] event=stale_row_regathered emits once per distinct typeID with the uncapped running total inline, plus event=unresolvable_row / event=row_out_of_range one-shots; the [OBJBATCHER v1] periodic summary carries stale_light_rows_regathered / _unresolvable / light_rows_out_of_range. WHY DEFAULT-ON: it removes a read of a value the engine's own comments call invalid, and its repair branch (GatherGpuObjectLightDataOnly) is not new code -- it is the SAME branch this site already took on the sentinel miss, i.e. the first-frame path every dynamic prop already exercises. HONEST LIMIT ON THE EVIDENCE: engine launches are hard-blocked in this lane, so the stale-read is proven by source and by the two sibling consumers that guard against it, NOT by an observed frame. The counters above are the instrument that will settle it on the first run -- a zero for stale_light_rows_regathered would mean the dynamic path never actually holds a stale stamp in steady state, and this gate is then inert rather than wrong. THE REFUTATION, STATED BECAUSE IT IS REAL AND THIS IS NOT A PURE WIN: the re-gather is not free. GatherGpuObjectLightDataOnly -> MC_TextureManager::addLightDataStructureWithPerActorColor (mclib/txmmgr.cpp:2062) does `*light_data = it->second.data` -- it OVERWRITES the shape's lightData_ from a scene template built by GatherLightsParameters, which reads the TG_Shape::s_listOfLights / s_numLights STATICS that the last-transformed shape set, then patches the colour from the current world light list. That is precisely the Stage 2.D.2 render-time timing bug the cached index was introduced to avoid -- the reason the comment at the call site says the cache holds the value gathered at update() time 'while worldLights[0]->aRGB was per-actor-correct'. So on the stale path this gate trades one wrong answer for a different one: BEFORE, a prior-frame slot index that may name another actor's row or an unwritten (zero) row -> possibly PURE BLACK; AFTER, a valid current-frame row whose per-actor tint may be the last-transformed actor's rather than this one's -> MISTINTED, never black. That is an improvement in the worst case and a REGRESSION in one specific case: an actor whose stale index happened to still point at its own correct row (plausible, since dedup allocation can be stable frame-to-frame across a stable actor set) previously drew correctly and now draws mistinted for that frame. It self-heals the next time the actor's update() runs. The population at risk is narrow -- cachedFrame_ goes stale when the cull skipped the actor's update() (mclib/msl.h:315-320), i.e. it went offscreen, so this is an actor being submitted in the same frame its update was skipped. stale_light_rows_regathered is the instrument that sizes this: if it reads zero in steady state the trade never occurs and the gate is inert; if it reads high, the RIGHT fix is to re-stamp the row at update() time rather than re-gather at render time, and this gate should be reconsidered rather than defended. NOT GRADED VISUALLY -- engine launches are blocked in this lane. MC2_STATIC_PROP_LIGHT_FRESHNESS=0 restores the pre-slice read (sentinel-only test, slot-0 fallback, no range check) bit-for-bit."
    },
    // MechLightFreshness (MECH-LIGHTROW-FRESHNESS-1)
    // MUST STAY LAST -- kFeatureTable is indexed by RendererFeature, so this
    // entry's position is its enum value (62).
    {
        "MC2_MECH_LIGHT_FRESHNESS",
        "MC2_MECH_LIGHT_FRESHNESS",
        EnvVarKind::Feature,
        true,
        "MECH-LIGHTROW-FRESHNESS-1, amended by MECH-DECLINE-FIX-1: DEFAULT-ON. The GPU-mech submit path (Mech3DAppearance::render, mclib/mech3d.cpp) checks TG_MultiShape::cachedFrame_ against g_mc2FrameCounter before trusting cachedGpuLightIndex_, RECORDS a stale or unset stamp, and SUBMITS ANYWAY with the index the pre-slice read resolved. A resolved index that is out of range against getLightStructCount() is CLAMPED to slot 0 and recorded. Under this gate nothing is declined, dropped or re-gathered -- it is a DETECTOR plus one bounds clamp. THE DEFECT IT WATCHES: cachedGpuLightIndex_ names a DYNAMIC light-table slot, and dynamic slots are rebased every frame -- MC_TextureManager::resetLightData (mclib/txmmgr.cpp:2137) resets lightDataStructuresCount to the static high-water and clears every dedup map, its own comment (txmmgr.cpp:2153-2157) stating that prior-frame hash->slot entries 'are invalid'. An index minted on frame N could name a DIFFERENT actor's row on frame N+1, and shaders/mech.vert:229/:292 index light[] with NO bounds check. The other three consumers of the field all check the stamp (msl.cpp:2147-2149 re-gathers; gos_static_prop_registry.cpp:1051 suppresses the draw; gos_static_prop_batcher.cpp:6011 re-gathers); this site was the last that did not. WHY IT ONLY DETECTS -- the correction of db221dcc2, which DECLINED the GPU submit here. That decline was justified on the ground that 'gpuMechSubmitted stays false and the CPU MLR path draws the mech at mechShape->Render(true)'. THAT IS FALSE IN THE ONLY CONFIGURATION WHERE THE DECLINE CAN FIRE, and the enclosing gate of the submit block is the proof: it requires g_useGpuMechs && !previewContext, which are exactly the conditions under which updateGeometry (mech3d.cpp:4567-4576) transforms mechShape with a FAST-TRANSFORM variant -- preview forces the full TransformMultiShape and !g_useGpuMechs falls to the full transform in the else arm. In the default configuration (MC2_GPU_MECHS and MC2_GPU_MECH_LEAF_SKIP are both envFlagDefaultOn, gos_mech_batcher.cpp:134/:214, modern shadow path active) that variant is _HierarchyOnly, which delegates to TransformMultiShape_BuildRecipe (msl.cpp:2003) and hits `if (s_buildRecipeOnly) continue;` at msl.cpp:1910 BEFORE the per-leaf dispatch. TG_Shape::MultiTransformShape therefore never runs, lastTurnTransformed is never advanced (its only writers are tgl.cpp:1911/:1972/:2984, all inside the skipped per-leaf dispatchers, plus Touch() at :4352 which is called on building and tree shapes only, never on mechShape), and TG_Shape::Render returns at the staleness guard tgl.cpp:3190 WITHOUT DRAWING. The engine documents this twice in-tree: mech3d.cpp:4560-4563 ('the GPU fast-transform variants leave listOfVertices stale, so TG_Shape::Render early-returns and the preview goes blank') and mech3d.cpp:3117-3124, the MECH-CULL-CPU-FIX comment, which records it as an OBSERVED runtime bug -- 'we skipped the GPU submit and fell back to the CPU mechShape->Render() path, which does not actually draw while GPU mech batching owns rendering -> the mech vanished for that frame -> center-screen flicker'. Declining reintroduced a bug this file had already fixed once. AND THE DECLINE WAS WORST IN ITS OWN TARGET CASE. It cannot fire in steady state: Mission::update() calls clearArrays()->resetLightData() (code/mission.cpp:1146), then CacheGpuLightData() stamps cachedFrame_ = g_mc2FrameCounter (mech3d.cpp:5793, ungated by cull), then draw_screen() runs the check -- and g_mc2FrameCounter is incremented only AFTER draw_screen (GameOS/gameos/gameosmain.cpp:2207). Reset, stamp and check share one counter epoch, so the stamp always matches. The stamp can only go stale when Mech3DAppearance::update() does not run at render cadence, i.e. the Alt+D debug-draw latch (gameosmain.cpp:1816) or the Ctrl+R doTransformMath toggle (code/mechcmd2.cpp:2295) -- but those skip Mission::update() itself, so resetLightData() does not run either, the table is NOT rebased, and the 'stale' index still names its own correct row. Pre-fix those frames rendered CORRECTLY; the decline made the mech vanish on them. The decline was therefore inert in steady state and strictly harmful whenever it fired -- no reachable case improved. WHY THE RANGE CHECK CLAMPS RATHER THAN DECLINES: an out-of-range light[] read is undefined, so the check must stay, but slot 0 is the pre-slice substitute and a defined in-range row (the static/baked prefix under LIGHTBAKE). An invisible mech is a worse outcome than a mistinted one for the actors the player aims at and is shot by. The check now also applies on the stale path, which is where an out-of-range index is most likely. FAIL-LOUD: [LIGHTROW_FRESHNESS v1] event=mech_stale_row_detected emits once per distinct mechType with the uncapped running total inline, plus an event=mech_row_out_of_range one-shot. The totals ride the ALWAYS-ON per-mission [MECHBATCHER v1] event=stale_light_row_summary line (GpuMechBatcher::onMapUnload), so they are visible on a supervised run without MC2_MECH_BATCHER_STATS; previously they were readable only from their own once-per-mech-type line, which made a count that started or grew later unobservable. GpuMechFallbackReason::StaleLightRow is retained for numbering stability but is NO LONGER RECORDED -- nothing falls back, so recording a fallback would be false. WHY DEFAULT-ON: what ships default-ON is the detector, the bounds clamp and the per-mission report -- none of which change what is drawn relative to the pre-slice read except by replacing an undefined out-of-range read with a defined one. There is no behavioural risk to gate off. HONEST LIMIT ON THE EVIDENCE: engine launches are hard-blocked in this lane. The draw-path chain above is established by source trace, not by an observed frame; the one link that is inferred rather than proven is that gos_IsModernShadowPathActive() (gameos_graphics.cpp:10133) is true in a normal session -- it is a runtime shadow_terrain-material-loaded predicate, not an env default. stale=0 on the per-mission line means the freshness hazard is INERT, which is the predicted steady-state result. Non-zero means some actor's update() is not running at render cadence -- the candidate is the `withdrawing` early return at code/mech.cpp:6117 -- and the repair is to re-stamp the row at update() time, NOT to decline at render time. oob MUST read zero. NOT GRADED VISUALLY -- engine launches are blocked in this lane. MC2_MECH_LIGHT_FRESHNESS=0 restores the pre-slice read (sentinel-only test, slot-0 substitution, no range check, no counters) bit-for-bit."
    },
    // RUNWAY-CLOSE-1: overlay-art flat-light exemption A/B arm (enum 63, LAST).
    {
        "MC2_FEATURE_OVERLAY_ART_FLATLIGHT",
        "MC2_OVERLAY_ART_FLATLIGHT",
        EnvVarKind::Feature,
        true,
        "RUNWAY-CLOSE-1: A/B arm for the REOPENED 'runway is the wrong color' item (user re-grade 2026-08-02: 'there is no difference here to me'). Default 1/unset = stock: the terrain_overlay.frag else-branch (authored road/runway art, the ARTGUARD + word==0 MISSING-RUNWAY-1 classes) keeps its CEMENT-NIGHT-LIGHT-1 u_missionFlatLight multiply. =0 uploads u_overlayArtFlatExempt=1.0 and the frag's mix() lifts the flat-pad light off authored art ONLY (adopt-branch and chunk cement untouched). WHY THIS IS AN ARM AND NOT A FIX, measured before landing: the brief that requested it modelled the apron as flat-lit at FULL brightness, but terrain_lod_chunk.frag:999 multiplies the cement atlas by the SAME u_missionFlatLight (CEMENT-NIGHT-LIGHT-1 covers both paths), and mc2_10's lighting block (LightDirPitch=180 -> lightDirection.z=sin(180)=0 -> Camera::getLightRed ambient branch, Ambient=192) gives flatLight=(0.753,0.753,0.753); a full exemption therefore OVERBRIGHTENS art by ~1.33x relative to the apron beside it rather than converging on it, and on mc2_05 night (flatLight 0.165) it is a x6.07 road glow. The tone-parity family for the mc2_10 runway class is the CEMENT-ALPHA-LAND-1 own-alpha word (bit31|30|23), which routes the tile into the identical atlas*flatLight + flat-lit + dynamic-shadow + no-in-shader-fog treatment the apron gets AND draws the dropped swaths. Unbound-uniform default is 0.0 = stock, so the exemption fails safe. Offline model: scripts/predict_cement_adopt.py --flatlight-report."
    },
};

static_assert(
    sizeof(kFeatureTable) / sizeof(kFeatureTable[0]) == static_cast<int>(RendererFeature::COUNT),
    "kFeatureTable length must match RendererFeature::COUNT");

// ---------------------------------------------------------------------------
// Auxiliary env-var table (kAuxEnvVars)
// ---------------------------------------------------------------------------
// Trace, Infra, and other registered vars not indexed by RendererFeature.
// The enforcement script greps BOTH tables for "MC2_" string literals.

static constexpr EnvVarDesc kAuxEnvVars[] = {
    // MC2_GPU_PARTICLES (master GPU-particle path; lives in kAuxEnvVars: kFeatureTable is indexed by RendererFeature; read in mclib/particles/batcher.cpp initialize_env)
    {
        "MC2_GPU_PARTICLES",
        "MC2_GPU_PARTICLES",
        EnvVarKind::Feature,
        false,
        "Master gate for GPU billboard particles (mclib/particles/batcher.cpp initialize_env). Default-OFF: unset routes FX through the legacy CPU/MLR path (reverted to CPU default after GPU FX looked broken); =1 opts in to GPU billboards, =0 is explicit OFF. Several VFX gates (soft/lit particles, oracle render, age sample) have no effect when this is OFF.",
    },
    // -- WATER-SHORE-MODE-3 tuning knobs (GL prototype) ----------------------
    {
        "MC2_TUNE_WATER_SHORE3_EXTRA_WU",
        "MC2_WATER_SHORE3_EXTRA_WU",
        EnvVarKind::Feature,
        false,
        "Shore mode 3 (MC2_WATER_SHORE_MODE=3) tuning: extra world units added to the water admission extent beyond the sidecar contour so geometry exists for the per-pixel discard to carve (gos_terrain_water_stream.cpp). Float, unset => 60. Inert unless shore mode 3 is active.",
    },
    {
        "MC2_TUNE_WATER_SHORE3_DEPTH_K",
        "MC2_WATER_SHORE3_DEPTH_K",
        EnvVarKind::Feature,
        false,
        "Shore mode 3 tuning: u_shoreDepthK, fraction of clip.z added to the flat water plane depth (depth-only lift so coarse terrain triangles do not carve notches; gos_terrain_water_fast_mdi.vert, uploaded in gameos_graphics.cpp). Float, unset => 0.03. Inert unless shore mode 3 is active.",
    },
    // ── LIGHT-ROW-PURE-1 ───────────────────────────────────────────────────
    // The in-engine half of the byte-identity proof for MC2_LIGHT_ROW_PURE.
    // Lives here rather than in kFeatureTable because it is a Trace var and
    // gates no render path -- it only observes.
    {
        "MC2_LIGHT_ROW_PURE_VERIFY",
        "MC2_LIGHT_ROW_PURE_VERIFY",
        EnvVarKind::Trace,
        false,
        "[DEFAULT-OFF. Observation only -- never changes which row is used.] LIGHT-ROW-PURE-1: runs BOTH object light-row producers on the SAME frame and memcmps the 3600-byte TG_HWLightsData rows -- the legacy in-place packing loop (which reads the globally MUTATED shared light list) against object_light_row::BuildObjectLightRow (which reads the same list plus the class's DECLARED policy). Equal rows mean the declaration still matches what the class actually did to the world; a mismatch means a published policy has drifted from its mutation, which is exactly the divergence this slice exists to make impossible to ship silently. Emits [LIGHT_ROW_PURE] MISMATCH lines (first 8) plus a checked/mismatched summary every 5000 gathers. Complements the offline tools/object_light_row_harness, which proves the same identity with no game launch at all.",
    },
    // ── SIM-IDENTITY-HASH-1 ────────────────────────────────────────────────
    // Per-sim-frame world-state hash (mover positions / orientations / gesture
    // + anim phase / target ids) feeding the refuse-to-grade gate in
    // scripts/sim_identity.py. Mover nondeterminism is a DESYNC, not a metrics
    // problem; this is the lockstep-game instrument for it.
    //
    // `defaultOn = true` is deliberate and load-bearing, NOT a copy-paste slip.
    // The consumer is a gate that REFUSES to compare two capture arms whose
    // worlds differ. A default-OFF producer would leave the block absent from
    // every manifest, and "absent" is the one input the gate cannot grade -- so
    // the refusal would fire on everything on day one, get called noise, and be
    // switched off. Classified Trace because it emits diagnostic output only:
    // the collector is read-only (getters only; the mutating zero-arg
    // getMovePath() is deliberately never called) and changes no render, no
    // gameplay and no sim state.
    //
    // The DEFAULT IS READ FROM code/sim_identity.cpp envOnOff(), which parses
    // "0"/"false"/"off"/"no" as OFF -- unlike the neighbouring
    // MC2_BRIDGE_MOVER_STATE, which is `getenv() != nullptr` and therefore
    // ENABLES on `=0`. Do not "harmonize" these two; the difference is the fix.
    {
        "MC2_SIM_IDENTITY",
        "MC2_SIM_IDENTITY",
        EnvVarKind::Trace,
        true,
        "SIM-IDENTITY-HASH-1: emit a world-state hash (sorted-by-partId digest over mover position/rotation/gesture/anim-frame/damage/status/team plus target/brain/move/path) into every visual-capture sidecar as a `sim_identity` block. DEFAULT ON. Consumed by scripts/sim_identity.py, which returns UNGRADED (exit 7) when two A/B arms' worlds differ in VISUAL fields -- i.e. when a pixel comparison between them measures nothing. Read-only: getters only, no allocation on the per-frame path, no sim mutation. Set MC2_SIM_IDENTITY=0 to omit the block entirely (which downgrades the gate to UNVERIFIED, not to a pass)."
    },
    {
        "MC2_SIM_IDENTITY_TRACE",
        "MC2_SIM_IDENTITY_TRACE",
        EnvVarKind::Trace,
        false,
        "SIM-IDENTITY-HASH-1: emit one `[SIMID v1] simframe=N t=T n=C graded=.. visual=.. strict=..` line per SIM frame to stdout. DEFAULT OFF. Diffing two runs' logs names the FIRST divergent sim frame directly (`sim_identity.py trace-diff a.log b.log`), where the pre-existing MC2_BRIDGE_MOVER_STATE needs ~58k per-mover lines plus a bespoke script to answer the same question."
    },
    {
        "MC2_SIM_IDENTITY_TRACE_DETAIL",
        "MC2_SIM_IDENTITY_TRACE_DETAIL",
        EnvVarKind::Trace,
        false,
        "SIM-IDENTITY-HASH-1: with MC2_SIM_IDENTITY_TRACE on, additionally emit one `[SIMID v1 mover]` line per mover per sim frame. DEFAULT OFF. Very verbose -- intended for a narrow frame window once trace-diff has already named the divergent frame."
    },
    // ── TERRAIN-AMBIENT-FLOOR-2 ────────────────────────────────────────────
    // DEFAULT-ON visual change. `defaultOn` below is `true` ON PURPOSE and must
    // stay true: the predecessor slice registered the identical gate as
    // defaultOn=false while its C++ default was an ACTIVE 0.35, so every gate
    // audit, every dead-gate scan and every release note would have reported a
    // shipped visual change as OFF. Classified Feature, not Trace, for the same
    // reason -- Trace means "diagnostic output only; no correctness effect",
    // which this is not.
    {
        "MC2_FEATURE_TERRAIN_AMBIENT_FLOOR",
        "MC2_TERRAIN_AMBIENT_FLOOR",
        EnvVarKind::Feature,
        true,
        "TERRAIN-AMBIENT-FLOOR-2: terrain-only, albedo-scaled ambient floor in terrain_lod_chunk.frag. DEFAULT 0.35 = ON; MC2_TERRAIN_AMBIENT_FLOOR=0 is the SINGLE, COMPLETE revert and is byte-identical. THE DEFECT (user screenshot, mc2_03, broad daylight, NOT a regression): terrain outside the concrete pads reads near-black while the pads are correctly lit. THE MECHANISM: terrain has NO ADDITIVE TERM AT THE RUNTIME LIGHTING STAGE. Its only one, the hemisphere fill, is default-OFF (MC2_TERRAIN_LIGHTING_V1), so terrain is exactly `baseColor * normalLight * shadow` -- a pure product with nothing bounding it from below -- while the static props standing on that same ground DO get an additive ambient through SH-IBL. PRECISION, because the loose form of this claim is wrong and was inherited: baseColor is NOT raw albedo, the colormap is pre-burned with the mission light (terrtxm2.cpp burnInShadows -> eye->getLightRed(lightFactor)), so terrain albedo already carries baked mission lighting -- but that is a MULTIPLICATIVE factor inside baseColor, not a floor, and cannot stop normalLight (floor mix(0.35,1.20,0.02) = 0.367) from crushing the product. An 'ambient debug view reads 0.0000 over terrain' measurement proves only that hemiContrib is zero, which is true by construction when V1 is off. Two roughly-1.7x factors (brighter concrete albedo, times the user-ratified concrete lighting exemption) then multiply that zero floor into the reported ~3x pad:ground ratio. THE TERM: `baseColor * ambCol * floor * mix(0.55,1.0,shadow) * dayGate * (1 - flatLitShare)`. It multiplies baseColor, which is the rendering equation's actual ambient form (irradiance x albedo) and is what keeps dark ground proportionally dark instead of washing the map toward flat grey; the N.z hemisphere shaping keeps terrain RELIEF (the flat-to-away lighting ratio compresses only ~2.3x -> ~2.0x at 0.35). WHY NOT MC2_TERRAIN_LIGHTING_V1 INSTEAD: its term is `hemiFill * strength * 0.25`, an ABSOLUTE add independent of albedo -- it lifts dark and bright materials by the same nits AND lifts the concrete pads (measured +21-26%% on pads). FLAT-LIT SURFACES ARE EXEMPT ON BOTH PATHS: the mask is `flatLitShare`, built at the lighting site from `cementLightBlend * (1-u_cementLit)` maxed with `pavedOverlayFlat * (1-u_cementLit)` -- i.e. literally the fraction of the fragment's lighting that was replaced by the flat 1.0, so the ambient's exemption edge IS the flat-lighting exemption edge and cannot drift from it. Masking on pureConcrete alone (the predecessor) LIFTED user-ratified flat-lit pavement, because PAVED-OVERLAY-CLASS-1 runway/road tiles are flat-lit through a separate branch and carry pureConcrete == 0 by construction. CORRECTION 2026-07-30 (LIGHTING-INCAPABLE-BATCH-3): this justification ORIGINALLY read 'that gate returns 1 when unset (PavedOverlayClassMode()), so it is live at stock.' THAT WAS FALSE WHEN WRITTEN and is false now -- gos_terrain_indirect.cpp PavedOverlayClassMode() opens `if (!v || !v[0]) return 0;` because CONCRETE-DELIVER-2 reverted the default to 0 at 11:15:09 on 2026-07-30, 88 minutes BEFORE TERRAIN-AMBIENT-FLOOR-2 landed. So bit 22 is never baked at stock, pavedOverlayFlat is false on every default-launch fragment, and the pavement half of the flatLitShare mask is reachable ONLY under MC2_PAVED_OVERLAY_CLASS=1. NO PIXEL IMPACT AT STOCK (both arms of the branch reduce to the same expression) -- the design is still right, the stated reason was not. DAYLIGHT ONLY, AND THE GATE IS PROVEN OFFLINE: dayGate comes from GameOS/gameos/utils/terrain_daylight.h, keyed on the AUTHORED sun pitch (Camera::dayLightPitch) min the engine runtime night state, and tools/terrain_daylight_harness executes it against every shipping mission's authored pitch -- 12 tests, no launch, no capture. It is EXACTLY 0.000 on mc2_10 (pitch 180), mc2_05 and mc2_21 (pitch 270), mc2_19 (pitch 0) and mc2_23 (pitch 9), so every night mission is byte-identical. WHY NOT THE OBVIOUS SIGNALS: mission flat light cannot reach zero (camera.cpp:2028-2036 pins the night path to fixed night values by design), which is what made the predecessor lift mc2_10's ground +59.1%%; and the AUTHORED pitch is the right key regardless of what the runtime flags say. CORRECTED 2026-07-30 (TREE-CONSOLIDATE-1): this used to read 'the RUNTIME sun altitude / isNight / nightFactor cannot be trusted either, because camera.cpp:2033 folds a below-horizon authored sun into a mirrored MOON pitch and Camera::update() then re-derives the flags from it -- on mc2_05 the runtime signals report a noon sun overhead on a mission the engine itself just classified as night.' THAT IS FALSE, and it is refuted by the same C++ the claim points at (the correction is also recorded at the transport in GameOS/gameos/gameos_graphics.cpp gos_SetTerrainDaylightLevel, DAYLIGHT-GATE-RECHECK-1). The moon fold is at mclib/camera.cpp:2079-2080, INSIDE Camera::updateDaylight(), and updateDaylight never runs during a mission: GameCamera::activate() overrides the virtual base without chaining and its own call site is commented out (code/gamecam.cpp:1060, `//updateDaylight(true);`). So on mc2_05 lightPitch stays at the authored 270, the derivation in Camera::update() gets altitude -90, and it correctly reports isNight = true, nightFactor = 1. There is no 'noon sun overhead' anywhere. THE DESIGN STILL STANDS, for a sounder reason than the one that was written down: dayLightPitch is written once at load and is never folded or animated, so it stays correct even if that fold is ever revived, whereas the runtime flags are one commented-out line away from being wrong. The gate takes the MIN of authored and runtime, which fails DARK on disagreement. Bounded 0..2; junk/NaN clamps to 0. Trace: MC2_TERRAIN_AMBIENT_FLOOR_TRACE. Visual-only; no gameplay, geometry, collision or UI change. NOT VISUALLY VERIFIED: engine launches are hard-blocked in this lane -- see the capture matrix in docs/tier1_env_vars.md."
    },
    // ── TERRAIN-AMBIENT-FLOOR-NIGHT-1 ──────────────────────────────────────
    // DEFAULT-OFF companion to the row above. Registered as a Feature (not a
    // Trace) because it changes pixels when armed. kAuxEnvVars is not indexed by
    // RendererFeature, so this row's position is free.
    {
        "MC2_FEATURE_TERRAIN_AMBIENT_FLOOR_NIGHT",
        "MC2_TERRAIN_AMBIENT_FLOOR_NIGHT",
        EnvVarKind::Feature,
        false,
        "TERRAIN-AMBIENT-FLOOR-NIGHT-1: 0..1 FLOOR under TERRAIN-AMBIENT-FLOOR-2's daylight gate; the uploaded gate becomes max(dayGate, floor). DEFAULT 0.0 = OFF = byte-identical on every mission (max(dayGate,0) == dayGate). WHY IT EXISTS: the row above ships DEFAULT-ON at 0.35 to cure 'terrain is too dark', and its daylight gate is EXACTLY 0.0000 on mc2_05, mc2_10, mc2_19, mc2_21 and mc2_23 -- which are precisely the five lowest-multiplier (0.3670-0.4814) and highest-floor-pin (14.3-100% of fragments at diffuse == 0.02) maps in the game. The shipped default-ON cure for dark terrain is switched off on exactly the five maps where terrain is darkest, and those frames are byte-identical to before it landed. Verified at source, not inherited: terrain_daylight.h kNightFullDeg = 10, and the authored LightDirPitch values are 270/180/0/270/9, folding to altitudes -1/0/0/-1/9, all <= 10. SHAPE: a floor, not a second gate and not a night-only branch, so it is monotone (no step at the dusk band edge) and composes with MC2_TERRAIN_AMBIENT_FLOOR instead of duplicating it -- strength stays one number, this is the fraction of it night maps are allowed. WHY IT IS OFF AND MUST STAY OFF UNTIL GRADED: the predecessor slice was rejected at merge review for lifting mc2_10's ground +59.1% through a fade that could not fade out, and the user has explicitly praised how mc2_05 currently looks. This is a taste call, not a regression fix. FLIP CRITERION (saturation checked first, so this points somewhere something can actually move): the ambient term is ADDITIVE, so the normalLight floor pin does not block it, and terrain's headroom to clip on these maps is 9.07x (mc2_10) to 20.83x (mc2_21) -- nothing saturates. Grade mc2_05 and mc2_10 from a ground-level pose carrying both bare terrain and a concrete pad, 0 vs 0.30; expect bare terrain up and pads 0.00% (flatLitShare exempts them), i.e. the pad:ground ratio closing from the terrain side only. Bounded 0..1; junk/NaN clamps to 0 (fail-dark). Visible in MC2_TERRAIN_AMBIENT_FLOOR_TRACE as night_floor= next to raw_dayGate=. Visual-only; no gameplay, geometry, collision or UI change. NOT VISUALLY VERIFIED: engine launches are hard-blocked in this lane."
    },
    {
        "MC2_TRACE_TERRAIN_AMBIENT_FLOOR",
        "MC2_TERRAIN_AMBIENT_FLOOR_TRACE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-AMBIENT-FLOOR-2 trace: prints ONE line at the first terrain-chunk uniform upload giving the resolved floor strength, the daylight gate ACTUALLY UPLOADED, the raw env string, and the three runtime inputs (isNight, nightFactor, terrainLightDir.z). Exists so 'did the daylight gate engage on this mission, and why' is answerable from a log line instead of from a capture diff. It deliberately prints the runtime signals ALONGSIDE the uploaded gate, because those runtime signals are the ones that lie on the below-horizon missions: camera.cpp:2033 substitutes a moon at the mirrored altitude, so on mc2_05 (authored pitch 270) expect isNight=0, nightFactor=0.000, lightDirZ=+0.707 -- i.e. 'noon overhead' -- next to dayGate=0.0000. Seeing that pairing is the confirmation that the authored-pitch gate is doing the work. Default OFF; output only, no pixels change."
    },
    {
        "MC2_TRACE_CEMENT_ATLAS_DUMP",
        "MC2_CEMENT_ATLAS_DUMP",
        EnvVarKind::Trace,
        false,
        "CEMENT-ATLAS-CELL-2: dump the assembled cement/road overlay atlas so "
        "cell art can be inspected directly -- which cells actually carry "
        "painted markings is an empirical question about the ART, not one to "
        "answer from thresholds (this dump is what proved there is no paint in "
        "solid concrete cells). Value is a PATH PREFIX, not a bool: "
        "MC2_CEMENT_ATLAS_DUMP=<prefix> writes <prefix>.rgba (raw BGRA8, "
        "atlasPixelSide^2) + <prefix>.csv (per-cell slot/is_alpha/geometry "
        "stats) and emits [CEMENT_ATLAS_DUMP v1]. Empty or unset = no dump. "
        "Diagnostic-only: writes files and a log line, never alters the atlas "
        "or any render state. Consumer: gos_terrain_indirect.cpp."
    },
    {
        "MC2_TRACE_NO_CURSOR_GRAB",
        "MC2_NO_CURSOR_GRAB",
        EnvVarKind::Trace,
        false,
        "SMOKE-NO-CURSOR-STEAL-1: suppress ALL OS-cursor manipulation -- window "
        "mouse-grab, Win32 ClipCursor clamp, and gos_SetMousePosition warps -- "
        "plus window activation on show, so an unattended launch never yanks the "
        "user's pointer or focus away from another window. Automated runs get "
        "this for free: suppression defaults ON whenever MC2_SMOKE_MODE is set. "
        "=1 forces it on for an interactive launch; =0 forces it off even under "
        "smoke. Unset + no smoke = legacy interactive behavior, unchanged. "
        "META4-CLOSE-1 SCOPE: cursor + show-time activation ONLY. This flag does "
        "NOT strip the taskbar button or Alt-Tab entry, does NOT make the window "
        "refuse keyboard focus, and does NOT suppress the crash dialog -- those "
        "are gos_NoUserPresentRequested() (MC2_WINDOW_OFFSCREEN / MC2_SMOKE_MODE), "
        "so the 'interactive launch' promise above is true again."
    },
    // ── NOFOCUS-HARDEN-1 ───────────────────────────────────────────────────
    // KIND NOTE, stated so a later audit does not have to guess: this is
    // classified Trace to sit beside MC2_TRACE_NO_CURSOR_GRAB above, which is
    // its functional sibling (both are harness/window hands-off gates read in
    // GameOS/gameos/gos_render.cpp). It is NOT a Feature: it gates no render
    // code path and changes no rendered pixel -- the GL drawable keeps its full
    // size, so glReadPixels/FBO capture is bit-identical with it on or off.
    // What it changes is the OS window's POSITION, plus a refusal of fullscreen.
    // `defaultOn` is false and that is what the reader implements
    // (gos_render.cpp window_offscreen_requested(): `e && e[0] && e[0] != '0'`,
    // i.e. unset => OFF). Do not "tidy" it to true: an unset engine launch is
    // exactly-legacy, and the harness turns it on explicitly.
    {
        "MC2_TRACE_WINDOW_OFFSCREEN",
        "MC2_WINDOW_OFFSCREEN",
        EnvVarKind::Trace,
        false,
        "NOFOCUS-HARDEN-1: create the SDL window HIDDEN at (-32000,-32000) and "
        "keep it there, so an agent-launched engine window never occupies "
        "visible desktop area at ANY point in its lifetime. THE DEFECT: the "
        "previous design created the window at options.cfg size in the middle of "
        "the desktop and relied on an EXTERNAL 250ms python poller "
        "(smoke_lib/nofocus.park_window_offscreen) to move it away afterwards, "
        "which guarantees a multi-second desktop-covering flash on every launch "
        "-- measured live at 1719x995 on a 1707x960 desktop. Post-hoc "
        "repositioning is the wrong SHAPE: the only way a window is never seen "
        "is for it never to be created anywhere visible. THE MECHANISM: position "
        "correct at CreateWindow time; re-asserted immediately before the single "
        "SDL_ShowWindow; re-asserted SYNCHRONOUSLY from resize_window() and "
        "set_window_fullscreen() (the path prefs.applyPrefs()'s SetScreenMode "
        "re-issue takes during boot, which is what used to undo a one-shot "
        "park); and fullscreen is REFUSED outright, because a fullscreen window "
        "has no position that hides it. -32000 is off every physical monitor on "
        "a normal multi-mon layout and is what Windows itself uses for minimized "
        "placeholders. NO PIXEL EFFECT: the GL drawable keeps its full size, so "
        "glReadPixels/FBO readback (the golden visual-capture path) is "
        "unaffected -- unlike minimizing, which makes SDL throttle and can zero "
        "the drawable. DEFAULT OFF: unset is exactly-legacy interactive "
        "behaviour; scripts/smoke_lib/nofocus.py sets it to 1 for agent launches "
        "(OFFSCREEN_ENV, apply_env(offscreen=True)). Consumers: "
        "GameOS/gameos/gos_render.cpp, GameOS/gameos/gameos_graphics.cpp. "
        "CAVEAT for harnesses that screenshot the DESKTOP COMPOSITE via "
        "pyautogui/mss rather than reading the engine FBO: an offscreen window "
        "contributes no pixels to the composite, so those call sites must pass "
        "nofocus.apply_env(offscreen=False). MC2_RES_DIAG=1 additionally prints "
        "one [RES_DIAG] line giving the resolved origin and size."
    },
    {
        "MC2_TRACE_NO_ACTIVATE",
        "MC2_NO_ACTIVATE",
        EnvVarKind::Trace,
        false,
        "Mission Editor (editor/EditorMFC.cpp, MainFrm.cpp): come up minimized and "
        "non-activating, never taking the foreground or keyboard focus. Equivalent "
        "to MC2_WINDOW_OFFSCREEN for the editor, which honors both. DEFAULT OFF: "
        "unset is the interactive maximized launch. Name to be standardized with the "
        "launcher audit."
    },
    {
        "MC2_TRACE_FORCE_ALL_VISIBLE",
        "MC2_FORCE_ALL_VISIBLE",
        EnvVarKind::Trace,
        false,
        "FORCE-ALL-VISIBLE-1: mark every unit (mechs, ground vehicles, "
        "elementals, all teams) fully visible and sensed, bypassing sensor / "
        "contact gating so nothing in an automated capture is hidden, "
        "blip-only or mid-fade. Defeats all THREE hiding mechanisms at their "
        "source in Mover::update (mc2_vis::forceVisibleOverride): the "
        "alphaValue fade-to-0 that is the ACTUAL hide, the sensorLevel that "
        "drives the blip shape and the emission predicates (forced to 5), and "
        "the legacy `visible` flag. Also short-circuits the four "
        "mc2_vis predicates and the cone-glow guards. Automated runs get this "
        "for free: scripts/smoke_lib/runner.py -- the shared launcher behind "
        "every capture harness -- defaults it ON whenever MC2_SMOKE_MODE is "
        "set. =0 forces it off even under smoke; unset + no smoke = normal "
        "interactive fog of war, unchanged. CONSEQUENCE: a smoke capture is "
        "no longer a fog-of-war-accurate frame, so any test validating "
        "sensor / contact / visibility behaviour MUST set =0."
    },
    {
        "MC2_TRACE_UI_IMGUI_PAUSE",
        "MC2_UI_IMGUI_PAUSE",
        EnvVarKind::Trace,
        false,
        "PAUSE-IMGUI-SLICE-1: =1 renders the in-mission pause menu as an ImGui "
        "window (GAME PAUSED + objectives/save/load/options/abort/exit/return) "
        "laid out inside gos_ComputeHudCanvasBox; buttons reuse "
        "PauseWindow::handleClick side-effects. Default unset -> legacy sprite "
        "panel, byte-identical. First slice of the in-mission HUD ImGui port."
    },
    {
        "MC2_TRACE_UI_IMGUI_INFO",
        "MC2_UI_IMGUI_INFO",
        EnvVarKind::Trace,
        false,
        "INFO-IMGUI-SLICE-1: =1 renders the in-mission unit-Info panel as an ImGui "
        "window (unit name + health bar + loadout list with scrollbar) laid out "
        "inside gos_ComputeHudCanvasBox; loadout rows come from the shared "
        "InfoWindow::gatherLoadoutRows helper so the legacy and ImGui paths cannot "
        "drift. Default unset -> legacy procedural panel, byte-identical. Mech "
        "paperdoll icon deferred to slice-2."
    },
    {
        "MC2_TRACE_BOOT_INFO",
        "MC2_BOOT_INFO",
        EnvVarKind::Trace,
        false,
        "INFO-CAPTURE fixture: =N force-selects the first home mover into the "
        "in-mission Info window and switches the command bar to the INFO tab "
        "ONCE at frame >= N (ControlGui::update). Headless agents cannot click "
        "the tab / select a unit, so this makes the info panel capturable for "
        "the ImGui port (same pattern as MC2_BOOT_PAUSED). Default unset -> "
        "byte-identical."
    },
    {
        "MC2_TRACE_BOOT_INFO_SCROLL",
        "MC2_BOOT_INFO_SCROLL",
        EnvVarKind::Trace,
        false,
        "INFO-IMGUI-SLICE-4 capture assist: =1 (with MC2_BOOT_INFO=N driving) "
        "pins the info-panel loadout list scrolled to bottom from frame N+3000 "
        "on BOTH paths — the ImGui child (SetScrollY max) and the legacy panel "
        "(setScrollPos clamp) — so the pilot/skill section is pixel-capturable "
        "headless gate-on AND as a legacy reference. Slices 2-3 pinned the "
        "ImGui side on MC2_BOOT_INFO alone; the pin is now opt-in so the "
        "standard fixture recipe captures the top-of-panel state matching the "
        "banked baselines. Default unset -> byte-identical."
    },
    {
        "MC2_TRACE_UI_IMGUI_OBJ",
        "MC2_UI_IMGUI_OBJ",
        EnvVarKind::Trace,
        false,
        "OBJ-IMGUI-SLICE-1: =1 renders the in-mission OBJECTIVES overlay as "
        "ImGui (baked left-edge frame from tools/ui_art_reconstruct.py + "
        "procedural header/objective rows/totals) laid out inside "
        "gos_ComputeHudCanvasBox; rows come from the shared "
        "ControlGui::gatherObjectiveRows so the legacy and ImGui paths cannot "
        "drift. MPlayer player-status variant stays legacy. Default unset -> "
        "legacy tile/font overlay, byte-identical."
    },
    {
        "MC2_TRACE_BOOT_OBJ",
        "MC2_BOOT_OBJ",
        EnvVarKind::Trace,
        false,
        "OBJ-CAPTURE fixture: =N calls ControlGui::startObjectives(1) ONCE at "
        "frame >= N (ControlGui::update), showing the in-mission objectives "
        "overlay. Headless agents cannot reach the pause-menu OBJECTIVES "
        "button, so this makes the overlay capturable for the ImGui port "
        "(same pattern as MC2_BOOT_INFO / MC2_BOOT_PAUSED). Default unset -> "
        "byte-identical."
    },
    {
        "MC2_TRACE_BOOT_OBJ_COMPLETE",
        "MC2_BOOT_OBJ_COMPLETE",
        EnvVarKind::Trace,
        false,
        "OBJ-COMPLETE-CAPTURE fixture: =N force-resolves the first active "
        "non-hidden objective to OS_SUCCESSFUL ONCE at frame >= N "
        "(ControlGui::update) via the public CObjective::Status(newStatus) "
        "setter (no action-list side effects; the mission does not end while "
        "other primaries stay undetermined). Makes the completed-status "
        "render states (green row + check tile) capturable headless — no "
        "mc2_01 objective is completable in a 30 s scripted run. Combine "
        "with MC2_BOOT_OBJ to show the overlay. Default unset -> "
        "byte-identical."
    },
    {
        "MC2_TRACE_BOOT_PAUSED",
        "MC2_BOOT_PAUSED",
        EnvVarKind::Trace,
        false,
        "PAUSE-IMGUI-SLICE-0: =N auto-toggles the in-mission pause menu ONCE at "
        "frame >= N (MissionInterfaceManager::update). Headless agents cannot "
        "press ESC/P, so this is the boot-to-pause fixture for capturing and "
        "iterating on the pause window (same fixture pattern as "
        "MC2_FX_FORCE_SPAWN). Default unset -> byte-identical."
    },
    {
        "MC2_TRACE_BOOT_PAUSED_CONFIRM",
        "MC2_BOOT_PAUSED_CONFIRM",
        EnvVarKind::Trace,
        false,
        "PAUSE-IMGUI-SLICE-3: =abort|exit invokes the ABORT MISSION / EXIT GAME "
        "confirm dialog ONCE after the MC2_BOOT_PAUSED pause menu has settled "
        "(PauseWindow::update), through the same handleClick path a user click "
        "takes. Headless agents cannot click the pause buttons, so this is the "
        "capture fixture for the confirm dialogs. Default unset -> byte-identical."
    },
    {
        "MC2_TRACE_UI_COMMAND_FILE",
        "MC2_UI_COMMAND_FILE",
        EnvVarKind::Trace,
        false,
        "UI-COMMAND-CHANNEL-1 + GAMEPLAY-DRIVER-1/2: =<path> polls that file "
        "once per frame (MissionInterfaceManager::update) and executes its "
        "lines as commands. UI verbs: toggle_pause, press pause.<BTN> "
        "(PauseWindow::handleClick), press dialog.YES|NO (pause confirm "
        "LogisticsOKDialog), debugview <n>, screenshot <name> [backbuffer] "
        "(DEV-SHELL capture point, needs MC2_DEV_SHELL=1). World verbs (route "
        "through the same dispatchers the mouse/keyboard use): select "
        "<partId|all|lance|class <NAME>|name <substr>>, order move <x> <y> "
        "[run|walk] (handleOrders), order attack <targetPartId>, order "
        "stop|hold_fire|powerup|powerdown|withdraw, camera <x> <y> [rot] "
        "[zoomAlt], camera_follow <partId|off> (Camera::setCameraTargetId), "
        "time_scale <f> (simSpeedSetMultiplier -- scenarioTime AND frameLength "
        "in lockstep, clamp 0.25..4). Readback verbs write multi-line payloads "
        "to the ack: status (selected units only), list "
        "[all|player|enemy|team <N>|class <NAME>] (every mover incl. enemies -- "
        "the only source of a target partId), unit <partId> (deep readback "
        "including which DRAW PATH the object took: appearance type, inView, "
        "static recipe index, screenPos), mission_status, light_state. "
        "RENDERER-INPUT-READBACK-1 adds five verbs that read what the RENDERER "
        "was handed rather than what the GAME thinks: light_row <partId> (the "
        "per-object light row from the SAME GatherLightsParameters the draw "
        "calls, fed by the SAME list that appearance class passes -- NULL for "
        "GenericAppearance; per light the UPLOADED vs SOURCE origin so "
        "'evaluated at world origin' is visible per light, plus the BAKED row "
        "still replayed for a registered prop), draw_path <partId> "
        "(recipe_replay|dynamic_submit|legacy_cpu + recipeDrift = baked matrix "
        "vs live position), shadow_feed <partId> (membership in the static-"
        "building and dynamic-prop caster feeds, the pose each SUBMITTED, "
        "shadowDrift vs live, and whether the static pass drew at all), ground "
        "<partId>|<x> <y> (terrain elevation/type/normal, objAboveGround, "
        "recipeAboveGround, recipeVsObj) and pixel <x> <y> [z]|screen <px> <py> "
        "(one scene-FBO texel; srcFrame is CURRENT-1 because the channel is "
        "polled before the frame renders, so a same-line screenshot is one "
        "frame AHEAD). All five are read-only and carry NO extra env gate -- a "
        "second gate in front of a read-only verb behind an already-gated "
        "channel adds a drift surface and no safety. "
        "Lighting verbs: time_of_day <0..1> (full sun+ambient+sky+fog "
        "recompute; 0.25 noon, 0.5 sunset, 0.75 midnight) and sun_pitch <deg> "
        "[yaw] (direction only) make the day AND night verdict rule "
        "enforceable without hunting for a mission that ships as night. "
        "end_mission win|loss|draw forces the termination counter. "
        "spawn/teleport are gated behind MC2_TEST_SPAWN=1. Consumed lines are "
        "truncated and acked to <path>.ack as '[f=<frame>] <cmd> -> "
        "ok|unknown|blocked (reason)' -- the frame number makes at_frame gates "
        "exact instead of wall-clock guesses. Lets agents drive a running "
        "instance headlessly for test. Default unset -> byte-identical."
    },
    {
        "MC2_TRACE_TEST_SPAWN",
        "MC2_TEST_SPAWN",
        EnvVarKind::Trace,
        false,
        "GAMEPLAY-DRIVER-1: =1 arms the test-only spawn/teleport verbs on the "
        "MC2_UI_COMMAND_FILE channel. 'spawn <clone|vehicle|infantry|objNum> "
        "<x> <y> [team] [variant]' runs mission->addMover (the same "
        "reinforcement path); 'vehicle' clones an existing GROUNDVEHICLE so a "
        "mech and a vehicle can be posed side by side under identical light. "
        "'teleport <partId> <x> <y>' sets Mover::setTeleportPosition. KNOWN "
        "DEFECT: spawned movers come up with getStatusRating()==0 and ignore "
        "group move orders (evidence: gameplay-driver capture 2026-07-23); the "
        "spawn ack now reports hp and warns when it is 0 instead of acking a "
        "bare 'ok'. Unset -> both verbs ack blocked; zero behavior change. "
        "ALSO arms 'animate <partId> <gestureId|damaged|normal>' "
        "(ANIMATE-BUILDING-1), which forces one building off the GPU "
        "static-prop batch and onto the per-frame animated path -- the defect "
        "class code/terrobj.cpp's own comment says smoke fly-throughs never "
        "exercise. Enumerate candidates with the ungated 'list buildings'."
    },
    {
        "MC2_TRACE_FREEZE_MOVERS_AT",
        "MC2_FREEZE_MOVERS_AT",
        EnvVarKind::Trace,
        false,
        "FREEZE-MOVERS-1: =<frame> freezes every mover the first frame "
        "g_mc2FrameCounter >= <frame>, WITHOUT needing the command channel "
        "(the 'freeze_movers on [frame]' verb is the same latch). Frozen movers "
        "still draw: this widens the predicate on Mission::update's existing "
        "pause branch, which calls ObjectManager::updateAppearancesOnly -- "
        "recalcBounds + setObjectParameters + updateGeometry still run, "
        "Appearance::update(false) skips the gait, and setMoverParameters is "
        "never called so torso twist and turret aim hold their last value. "
        "That last part is the point: a turret resting at a different aim "
        "angle between two arms produced a retracted '+14.8 luma brighter' "
        "result. Arming by FRAME (not wall clock) is what makes a pose "
        "reproducible across a REBUILD; pair with MC2_SMOKE_FIXED_TIMESTEP=1 "
        "or frameLength stays wall-clock and frame N is a different simulated "
        "instant per build. Also freezes the residual wall-clock accumulators "
        "that survive a pause: mech idleTime (which FLIPS the gesture 2->13 at "
        "idleMAX), mech+vehicle damage-flash blink, and vehicle sensorSpin "
        "(the mech path was frozen by VK-MECH-CAPTURE-DETERMINISM-1, the "
        "vehicle path was not). Those guards test THIS gate only, never the "
        "visual-capture predicate, so existing capture goldens are unchanged. "
        "Freezes the whole SIM TICK, not literally movers alone: scenarioTime, "
        "mcTextureManager::update, SensorManager::update, updateCollisions and "
        "the mission ABL brain are all suspended exactly as the pause path "
        "suspends them, while rendering continues. A half-frozen sim is worse "
        "than either extreme -- mcTextureManager::update's own comment says it "
        "must not run while paused because it uncaches things only "
        "objectManager->update can restore, and a freeze skips precisely that "
        "call, so leaving it live would MANUFACTURE missing/black-texture "
        "artifacts (the defect class this harness exists to reproduce). "
        "Unset -> reader parks at -1 and the latch never fires; default OFF."
    },
    {
        "MC2_DIAG_DEBUG_VIEW",
        "MC2_DEBUG_VIEW",
        EnvVarKind::Trace,
        false,
        "DEBUG-VIEW-SWEEP-1: =N (1..6) boot-seeds the global full-scene debug "
        "render view: 1=shadow-factor 2=normalized-depth 3=unlit-albedo "
        "4=overdraw-proxy 5=world-normals 6=chunk/LOD-tint. Fans out through "
        "gos_SetGlobalDebugView() to every lane's existing debug channel: the "
        "unified 40/50-series lighting channel (terrain gos_terrain.frag, "
        "static_prop.frag; runtime override slot on mc2LightingDebugMode()), "
        "mech.frag u_debugMode (batcher_setMechDebugMode), the VFX billboard "
        "mode (gos_vfx_setDebugMode), and the static-prop material mode "
        "(batcher_setDebugMaterialMode). Runtime switch: UI command-channel "
        "verb 'debugview N' (MC2_UI_COMMAND_FILE). Uncovered lanes (water, "
        "sky, grass cards, decals, legacy gos_vertex* draws, HUD) render "
        "normally in every view. Diagnostic-only; default unset -> mode 0, "
        "byte-identical."
    },
    {
        "MC2_DIAG_DEBUG_VIEW_SWEEP",
        "MC2_DEBUG_VIEW_SWEEP",
        EnvVarKind::Trace,
        false,
        "DEBUG-VIEW-SWEEP-1: ='<startFrame>,<settleFrames>' frame-scheduled "
        "auto-sweep of the six MC2_DEBUG_VIEW views (missiongui.cpp update). "
        "For each view v in 1..6: at frame start+(v-1)*(settle+2) sets the "
        "view, settle frames later schedules dev_shell_out/view_<v>.tga "
        "(scene-FBO source; capture hook needs MC2_DEV_SHELL=1), then restores "
        "view 0. Frame-keyed so two fixed-timestep runs (MC2_SMOKE_MODE=1 + "
        "MC2_SMOKE_FIXED_TIMESTEP=1 + MC2_CAMERA_PARK) capture byte-comparable "
        "sim states — wall-clock command scheduling cannot. Driven by "
        "tools/debug_view_sweep.py. Diagnostic-only; default unset -> one "
        "cached getenv, byte-identical."
    },
    {
        "MC2_DIAG_CAMERA_PARK",
        "MC2_CAMERA_PARK",
        EnvVarKind::Trace,
        false,
        "CAMERA-PARK-1 (ported from vk branch 4b69ef78): ='x,y,rot,proj,alt"
        "[@frame]' hard-sets the live camera pose once at frame >= @frame "
        "(default 300) and pins it against scripted writers "
        "(freezeScriptedCameraForCapture) WITHOUT freezing the sim — "
        "deterministic worst-case views for perf captures and debug-view "
        "sweeps while gameplay keeps running. Malformed value -> off. "
        "Default unset -> one static pointer check, byte-identical."
    },
    {
        "MC2_TRACE_UI_IMGUI_TIMER",
        "MC2_UI_IMGUI_TIMER",
        EnvVarKind::Trace,
        false,
        "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission mission-timer "
        "display as ImGui (right-aligned MM:SS via HudFont + the TimerStatic "
        "atlas tile) laid out through the HUD-shrink + canvas-box transform "
        "chain. Time/warning/blink logic stays shared with the legacy path. "
        "Default unset -> legacy drawShadowText/StaticInfo draw, byte-identical."
    },
    {
        "MC2_TRACE_UI_IMGUI_VIDEO",
        "MC2_UI_IMGUI_VIDEO",
        EnvVarKind::Trace,
        false,
        "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission comm-video panel "
        "as ImGui — VideoStatic chrome tiles + the decoder's frame texture at "
        "the same aspect-fit quad/letterbox video_render computes "
        "(MC2Movie::getLayout), inside gos_ComputeHudCanvasBox. Default unset "
        "-> legacy StaticInfo + gos quad draw, byte-identical."
    },
    {
        "MC2_TRACE_BOOT_TIMER",
        "MC2_BOOT_TIMER",
        EnvVarKind::Trace,
        false,
        "TIMER-CAPTURE fixture: =N force-arms the in-mission mission timer ONCE "
        "at frame >= N by setting mission->m_timeLimit = scenarioTime + 754 s "
        "(ControlGui::update). Tier1 missions ship with no TimeLimit, so the "
        "timer panel is otherwise unreachable headless. Same pattern as "
        "MC2_BOOT_INFO / MC2_BOOT_OBJ. Default unset -> byte-identical."
    },
    {
        "MC2_TRACE_BOOT_TIMER_SECS",
        "MC2_BOOT_TIMER_SECS",
        EnvVarKind::Trace,
        false,
        "TIMER-CAPTURE fixture parameter: =S overrides the remaining seconds "
        "the MC2_BOOT_TIMER fixture arms (default 754). Lets the <5-minute red "
        "warning (S=283) and <2-minute blink (S=110) states of the shared "
        "legacy timer computation be captured headless — the warning logic "
        "itself is unchanged and shared by the legacy and ImGui draws. Only "
        "read when MC2_BOOT_TIMER is set. Default unset -> byte-identical."
    },
    {
        "MC2_TRACE_BOOT_VIDEO",
        "MC2_BOOT_VIDEO",
        EnvVarKind::Trace,
        false,
        "VIDEO-CAPTURE fixture: =1 overrides the smoke-mode movie skip in "
        "MC2Movie::init so in-mission comm videos (mc2_01 V1A.bik ~frame 1600) "
        "play under MC2_SMOKE_MODE and the video panel is capturable headless. "
        "Default unset -> smoke-mode skip preserved, byte-identical."
    },
    {
        "MC2_ABL_COMPILE_FATAL",
        "MC2_ABL_COMPILE_FATAL",
        EnvVarKind::Feature,
        true,
        "ASAN-3-ABL-COMPILE-FATAL-1: fail at preprocess when ABL syntax errors leave partial bytecode; set =0 only for legacy execute-anyway diagnostics."
    },
    {
        "MC2_ABL_COMPILE_FATAL_REPRO",
        "MC2_ABL_COMPILE_FATAL_REPRO",
        EnvVarKind::Trace,
        false,
        "ASAN-3-ABL-COMPILE-FATAL-1: preprocess the supplied malformed-fixture path during ABL init to prove fail-loud behavior."
    },
    // TERRAIN-DECAL-SLICE-0B: live placement knobs for the CLIFF_WALL mesh-decal
    // (mclib/bdactor.cpp registerStatic). Read once at process start; all default
    // to Slice-0A behavior -> byte-identical when unset. Only consulted when
    // MC2_TERRAIN_DECAL is armed and the appearance is "MarbleCliff".
    {
        "MC2_TERRAIN_DECAL_SCALE",
        "MC2_TERRAIN_DECAL_SCALE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: uniform scale (float, default 1.0) of the CLIFF_WALL mesh-decal — multiplies the face-frame's 3 basis rows before translation. Uniform only (no inverse-transpose). Default 1.0 -> Slice-0A byte-identical."
    },
    {
        "MC2_TERRAIN_DECAL_OFFSET",
        "MC2_TERRAIN_DECAL_OFFSET",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: outward offset distance (float world units, default 8.0) along the wall 'facing' axis — replaces the hardcoded 8.0 Slice-0A z-fight offset."
    },
    {
        "MC2_TERRAIN_DECAL_LATERAL",
        "MC2_TERRAIN_DECAL_LATERAL",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: lateral nudge (float world units, default 0.0) along the wall 'tangent' (contour) axis — side-to-side placement."
    },
    {
        "MC2_TERRAIN_DECAL_LIFT",
        "MC2_TERRAIN_DECAL_LIFT",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: vertical nudge (float world units, default 0.0) along world-up — lifts/lowers the CLIFF_WALL mesh-decal."
    },
    {
        "MC2_TERRAIN_DECAL_YAW",
        "MC2_TERRAIN_DECAL_YAW",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: yaw rotation (float degrees, default 0.0) of the wall facing/tangent about world-up — corrects the outward-facing direction. Default 0.0 -> Slice-0A byte-identical."
    },
    {
        "MC2_TERRAIN_DECAL_FILL",
        "MC2_TERRAIN_DECAL_FILL",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-FILL-1: ambient/fill floor (float 0..1, default 0.20) for the CLIFF_WALL mesh-decal's shadow side. static_prop lighting is max(N-L,0) with no ambient floor, so faces pointing away from the sun render black; this raises the per-fragment floor so they read as dark rock. Applied ONLY to the MarbleCliff decal (per-instance kFlagDecalFill bit 3); all other static props are byte-identical regardless. Uniform u_terrainDecalFill; ImGui 'Cliff Decal' Fill slider drives it live via g_terrainDecalFill."
    },
    {
        "MC2_TERRAIN_DECAL_COLORBLEND",
        "MC2_TERRAIN_DECAL_COLORBLEND",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-colormap blend (float 0..1, default 0.35) for the CLIFF_WALL mesh-decal. Mixes the decal albedo toward the terrain COLORMAP sampled at the decal's world-XY (same colormap atlas + world->UV transform as terrain_lod_chunk.frag) so the cliff mesh melts into its surroundings instead of reading as raw light-marble. Applied ONLY to the MarbleCliff decal (per-instance kFlagDecalFill bit 3); all other static props are byte-identical regardless. 0.0 (or flag-unset) = byte-identical OFF. Uniform u_terrainDecalColorBlend; ImGui 'Cliff Decal' Color blend slider drives it live via g_terrainDecalColorBlend."
    },
    {
        "MC2_TERRAIN_DECAL_PITCH",
        "MC2_TERRAIN_DECAL_PITCH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-PITCH: pitch/lean (float degrees, default 0.0) of the wall about the horizontal tangent (contour) axis — positive leans the wall TOP backward toward the hill (recline against the slope). Default 0.0 -> dead-vertical byte-identical."
    },
    // OVERLAY-TILE-HIRES-1: high-resolution terrain overlay tiles.
    {
        "MC2_OVERLAY_TILE_HIRES",
        "MC2_OVERLAY_TILE_HIRES",
        EnvVarKind::Feature,
        false,
        "OVERLAY-TILE-HIRES-1: honor the overlay tile TGA's actual dimensions instead of the legacy hard-assumed 64px (initOverlay header resync mirrors initTexture), probing data/textures/<N>Overlays/ first with per-file fallback to 64Overlays/. =1 selects 256; other pow2 values in [128,1024] select that folder/edge. Also derives the overlay emit half-texel UV inset from the actual tile edge (quad.cpp + decal static VBO). Default-OFF: exact legacy 64px path, byte-identical."
    },
    // GATE-REGISTRATION-BACKFILL-1 (terrain-V2 catch-up): register terrain-V2
    // arc gates flagged UNREGISTERED by check-new-gates / check-env-registry.
    // TERRAIN-CONTROLMAP-SAMPLE-1: authored RGBA control-map override replacing
    // the colormap-colour material classifier (frag u_useControlMap).
    {
        "MC2_TERRAIN_CONTROLMAP",
        "MC2_TERRAIN_CONTROLMAP",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CONTROLMAP-SAMPLE-1: load an authored per-mission RGBA control-map sidecar (data/missions/<stem>.beauty/control_map.png, vertex-resolution) that replaces the colormap-colour material classifier with authored rock/grass/dirt/concrete weights. Default-OFF: absent/empty map -> the frag falls back to the classifier (TERRAIN-MATTEX-BLEND-1). WARNING: a wrong-content (e.g. 4-quadrant test) sidecar renders quadrant terrain."
    },
    {
        "MC2_TERRAIN_CONTROLMAP_FILE",
        "MC2_TERRAIN_CONTROLMAP_FILE",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CONTROLMAP-SAMPLE-1: override the control-map sidecar path (default data/missions/<stem>.beauty/control_map.png). Only consulted when MC2_TERRAIN_CONTROLMAP is armed."
    },
    // TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar.
    {
        "MC2_TERRAIN_OVERLAY_V2",
        "MC2_TERRAIN_OVERLAY_V2",
        EnvVarKind::Feature,
        false,
        "TERRAIN-OVERLAY-V2-PARITY-1: composite an authored world-XY-bounded cement/pad/runway overlay sidecar (data/missions/<stem>.beauty/overlay_v2.png, RGB=diffuse A=coverage) over the terrain in the LOD-chunk frag. Default-OFF -> byte-identical legacy composite. KNOWN-BAD on current content: the authored sidecars are offset/washed vs the base render (retire or regenerate); the base colormap + indirect-overlay tiles already draw roads/runways correctly."
    },
    {
        "MC2_TERRAIN_OVERLAY_V2_FILE",
        "MC2_TERRAIN_OVERLAY_V2_FILE",
        EnvVarKind::Feature,
        false,
        "TERRAIN-OVERLAY-V2-PARITY-1: override the overlay_v2 sidecar path (default data/missions/<stem>.beauty/overlay_v2.png). Only consulted when MC2_TERRAIN_OVERLAY_V2 is armed."
    },
    // TERRAIN-VISUAL-HEIGHT-SAMPLE-1 companion fade.
    {
        "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE",
        "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE",
        EnvVarKind::Feature,
        true,
        "TERRAIN-VISUAL-HEIGHT-SAMPLE-1: when visual terrain displacement is on, fade object grounding across the displaced-vs-gameplay height delta so props/units sit on the visually-displaced surface. Default-ON when MC2_TERRAIN_VISUAL_DISPLACE is armed; =0 disables the fade."
    },
    // DEV-DETERMINISM: single shared RNG stream for reproducible runs.
    {
        "MC2_DETERMINISTIC_RNG",
        "MC2_DETERMINISTIC_RNG",
        EnvVarKind::Feature,
        false,
        "Route all randomness (incl. paths that bypass gos_rand) through a single seeded deterministic RNG stream for reproducible runs / capture parity. Default-OFF: stock RNG behavior."
    },
    // BUILDING-GLASS-1: dither-glass translucency for static-prop ALPHA_TEST packets.
    {
        "MC2_SP_GLASS_DITHER",
        "MC2_SP_GLASS_DITHER",
        EnvVarKind::Feature,
        true,
        "BUILDING-GLASS-1: ordered-dither (Bayer 4x4) translucency for partial-alpha texels on static-prop ALPHA_TEST materials (window glass). Default-ON; =0 killswitch restores the binary a<0.5 discard (glass fully invisible, hollow buildings)."
    },
    // LIGHTBAKE-REBAKE-ON-READY-1: one-shot static-light re-bake after mission ready.
    {
        "MC2_FEATURE_LIGHTBAKE_REBAKE_ON_READY",
        "MC2_LIGHTBAKE_REBAKE_ON_READY",
        EnvVarKind::Feature,
        true,
        "LIGHTBAKE-REBAKE-ON-READY-1: soft-invalidate the per-recipe static-light bake cache once, 8 frames after mission-ready, so recipes baked during boot/load (before mission lighting finalized) cannot freeze a wrong (day-bright) light row for the whole mission. Slot indices/high-water preserved; entries lazily re-gather into the same permanent slots. DEFAULT ON; =0 restores frozen-first-bake behavior."
    },
    // FOGLIGHT-LATCH-1: diagnostic probe for the one-shot per-actor light latch.
    {
        "MC2_PROP_LIGHT_LATCH_DIAG",
        "MC2_PROP_LIGHT_LATCH_DIAG",
        EnvVarKind::Trace,
        false,
        "FOGLIGHT-LATCH-1: dump per-static-prop-actor [PROPLATCH v1] lines -- latch frame + latched lightRGB (the fogLightSet one-shot sample fed to the SUN slot) vs the light the same actor at the same position would sample now (MATCH/STALE). Diagnostic only, never mutates lighting. Default OFF."
    },
    {
        "MC2_PROP_LIGHT_LATCH_DIAG_INTERVAL",
        "MC2_PROP_LIGHT_LATCH_DIAG_INTERVAL",
        EnvVarKind::Trace,
        false,
        "FOGLIGHT-LATCH-1: frame throttle for the [PROPLATCH v1] compare line (default 120). No effect unless MC2_PROP_LIGHT_LATCH_DIAG=1."
    },
    // SP-BATCHER-ALPHASCAN-GATE-1 (framebudget-2 perf).
    {
        "MC2_SP_ALPHASCAN_GATE",
        "MC2_SP_ALPHASCAN_GATE",
        EnvVarKind::Feature,
        false,
        "SP-BATCHER-ALPHASCAN-GATE-1: skip the frame-constant per-static-prop alpha re-resolve scan in the batcher when nothing that affects it changed (gos_static_prop_batcher). Perf, default-OFF -> byte-identical."
    },
    // POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening (GL composite).
    {
        "MC2_POST_CAS",
        "MC2_POST_CAS",
        EnvVarKind::Feature,
        false,
        "POST-FX-CAS-1: gated AMD FidelityFX CAS sharpening folded into the final composite shader (after tonemap/grade, before UI). Default-OFF -> u_casEnabled=0 -> byte-identical."
    },
    {
        "MC2_POST_CAS_SHARPNESS",
        "MC2_POST_CAS_SHARPNESS",
        EnvVarKind::Feature,
        false,
        "POST-FX-CAS-1: CAS sharpness knob, clamped 0..1 (default 0.5). Only consulted when MC2_POST_CAS is armed."
    },
    // POST-FX-TONEMAP-V2 (SCOUR-1 #3): filmic tonemap on the composite.
    {
        "MC2_TONEMAP_V2",
        "MC2_TONEMAP_V2",
        EnvVarKind::Feature,
        false,
        "POST-FX-TONEMAP-V2: optional filmic tonemap curve on the final composited colour (postprocess.frag). 0/unset=OFF (byte-identical no-op branch), 1/agx=AgX (Sobotka minimal), 2/hable=Uncharted2 filmic, 3/reinhard. Replaces the ACES deleted as wrong-for-RTS; RTS-neutral picks. Visual-only, after grade+CAS before UI."
    },
    // SAVE/LOAD-AIFREEZE-TRACE-1: runBrain post-exec diagnostic.
    {
        "MC2_BRAIN_FREEZE_TRACE",
        "MC2_BRAIN_FREEZE_TRACE",
        EnvVarKind::Trace,
        false,
        "SAVE/LOAD-AIFREEZE-TRACE-1 (+ ISSUE-10 extension): one-line stderr trace at end of MechWarrior::runBrain (brainState, curTacOrder.code, movePath.numSteps, moveState, hasBrain, orderState, mainGoal/goalWID, tacOrder[GENERAL].code, position, LastMoveCalcErr) to classify the save/load AI-freeze AND scripted-unit permanent stalls (alarm-loop vs goal-divert vs no-path vs path-but-not-moving). Default-OFF -> byte-identical."
    },
    // TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1.
    {
        "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS",
        "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS",
        EnvVarKind::Feature,
        true,
        "TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: in the static decal bake, skip legacy runway/road/cement decals for tiles fully inside the armed OVERLAY_V2 sidecar bounds (avoids double-draw). Default-ON but only active when OVERLAY_V2 is armed+loaded; =0 reverts. Byte-identical when OVERLAY_V2 off."
    },
    // FOG-HORIZON-CLAMP-1: reshape OOB/edge fog to a horizon elevation profile.
    {
        "MC2_FOG_HORIZON_CLAMP",
        "MC2_FOG_HORIZON_CLAMP",
        EnvVarKind::Feature,
        true,
        "FOG-HORIZON-CLAMP-1: reshape the out-of-bounds / edge fog to a horizon elevation profile (gos_postprocess). Default-ON; kill-switch MC2_FOG_HORIZON_CLAMP=0 restores the previous fog."
    },
    {
        "MC2_FOG_HORIZON_FADE_START",
        "MC2_FOG_HORIZON_FADE_START",
        EnvVarKind::Feature,
        false,
        "FOG-HORIZON-CLAMP-1: override the horizon-fog fade band START (elevation). Tuning param; only consulted under MC2_FOG_HORIZON_CLAMP."
    },
    {
        "MC2_FOG_HORIZON_FADE_END",
        "MC2_FOG_HORIZON_FADE_END",
        EnvVarKind::Feature,
        false,
        "FOG-HORIZON-CLAMP-1: override the horizon-fog fade band END (elevation). Tuning param; only consulted under MC2_FOG_HORIZON_CLAMP."
    },
    // GOM_UPDATE_COST v1: per-phase GameObjectManager update CPU cost trace.
    {
        "MC2_GOM_UPDATE_COST_SPLIT",
        "MC2_GOM_UPDATE_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "GOM_UPDATE_COST v1 (framebudget): =1 emits one coarse per-phase GameObjectManager update CPU-cost summary to stderr (objmgr.cpp). Diagnostic only, default-OFF."
    },
    // VK-P13-SELECTOR-1 / T1: the top-level GL-vs-Vulkan backend selector.
    {
        "MC2_FEATURE_RENDER_BACKEND",
        "MC2_RENDER_BACKEND",
        EnvVarKind::Feature,
        false,
        "VK-P13-SELECTOR-1: top-level render-backend selector, values gl|vulkan "
        "(default gl; unset/unrecognized -> gl, coercion logged). Read ONCE and "
        "memoized by mc2::renderBackend() (GameOS/gameos/render_backend_select.*). "
        "DISTINCT from MC2_RENDER_BACKEND_IFACE (a per-region postprocess interface "
        "toggle). =vulkan births the standalone shared engine Vulkan device "
        "(mc2vk::VulkanContext, GameOS/gameos/vulkan_context.*) at engine init for "
        "P6/P8/P9/P14 to reuse; present is still GL swap in P13 (VK present intercept "
        "is T3). Fail-soft: any VK bring-up failure forces the session back to GL. "
        "The old private-device postprocess interop islands have been retired, "
        "so the shared engine Vulkan device is the only postprocess VkDevice. "
        "On =gl/unset the accessor adds only one memoized "
        "getenv -> byte-identical GL behavior."
    },
    {
        "MC2_FEATURE_VK_IMGUI",
        "MC2_VK_IMGUI",
        EnvVarKind::Feature,
        false,
        "VK-IMGUI-NATIVE-1 (rung 8.6): default-OFF ImGui Vulkan renderer gate. "
        "Current implementation is observable-decline only: GuiRuntime::Render() "
        "logs [VK_IMGUI] requested=1 used_vulkan=0 with fallback_reason="
        "\"tail_handoff_not_split\" and then renders through the unchanged "
        "SDL2/OpenGL3 backend. Native rendering stays blocked until the final-tail "
        "present handoff is split."
    },
    {
        "MC2_FEATURE_VK_PRESENT_TAIL_HOOK",
        "MC2_VK_PRESENT_TAIL_HOOK",
        EnvVarKind::Feature,
        false,
        "VK-TAIL-HOOK-1 (rung 8.6): default-OFF present-command-buffer seam for "
        "native final-tail rendering. When enabled, EnginePresentVk records a "
        "behavior-neutral dependency in presentGLFrame() after the GL->VK handoff "
        "and before the swapchain blit; [VK_PRESENT] tail_hook_frames proves the "
        "native HUD/ImGui insertion point executes."
    },
    {
        "MC2_FEATURE_VK_PRESENT_NO_GL_IMPORT",
        "MC2_VK_PRESENT_NO_GL_IMPORT",
        EnvVarKind::Feature,
        false,
        "VK present-tail ownership gate. Default-OFF. With the strict native "
        "present stack armed, EnginePresentVk allocates the shared present image "
        "as a VK-only RGBA8 target instead of an exportable GL memory-object/FBO. "
        "This proves native composite/HUD/ImGui can produce the present image "
        "without the present-specific GL import."
    },
    {
        "MC2_TRACE_VK_VFX_TRACE",
        "MC2_VK_VFX_TRACE",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-2: per-bolt weapon-fire pipeline trace. Default-OFF. When set, "
        "WeaponBolt::render (code/weaponbolt.cpp) prints [VK_VFX_TRACE] lines per bolt/beam "
        "with the render path taken (IMMEDIATE gos_DrawTriangles vs deferred addVertices), "
        "viewport bounds, projected screen coords, and packed argb; GameObjectManager::render "
        "(code/objmgr.cpp other-block) prints the VK-VFX route state + live weapon/carnage/"
        "artillery counts. Used to diagnose why field-VK weapon fire produced zero bolt pixels "
        "(root cause: drawOldWay==false deferred bolts into the discarded GL scene flush)."
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_PPM",
        "MC2_VK_PRESENT_DUMP_PPM",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-ORACLE-1: headless present-pixel oracle. Default-OFF. Path of a PPM "
        "(P6) file; EnginePresentVk::verifyReadbackContent dumps the FULL composited "
        "present image (sharedImg, the RGBA8 backbuffer VK blits to the swapchain) "
        "there on each verify tick. Requires MC2_VK_PRESENT_VERIFY; verify ticks run "
        "every 60th frame while frame<600 plus (if MC2_VK_PRESENT_DUMP_FRAME is set) "
        "a 600-frame window at/after the pin. The OS/PrintWindow grab of a VK "
        "swapchain returns only the clear colour and the GL scene-FBO capture is "
        "empty under shared-GBuffer, so this is the only true-backbuffer capture."
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_FRAME",
        "MC2_VK_PRESENT_DUMP_FRAME",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-3: frame pin for MC2_VK_PRESENT_DUMP_PPM. Default unset "
        "(overwrite-latest each verify tick). =N captures once at the FIRST verify "
        "tick with g_mc2FrameCounter >= N. gameosmain extends the verify-tick window "
        "to [N, N+600) so a pin past the initial 600-frame verify stretch still "
        "fires (an unpinnable pin silently reports 'PPM never wrote' on a HEALTHY "
        "present path — the 2026-07-12 grey-regression false clue)."
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_ON_FIRE",
        "MC2_VK_PRESENT_DUMP_ON_FIRE",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-5: dynamic fire-event pin for MC2_VK_PRESENT_DUMP_PPM. "
        "Default unset. When set (with MC2_FX_FORCE_SPAWN=loop), the fixture's "
        "FIRST volley arms the capture ~0.5s (30 frames) after itself "
        "(g_mc2FxFireCaptureFrame), so the PPM always lands mid-volley — no "
        "static pin roulette against a mission's intro/scenario clock. Ignored "
        "when MC2_VK_PRESENT_DUMP_FRAME is set (static pin wins). Requires "
        "MC2_VK_PRESENT_VERIFY; suppresses all PPM writes until armed."
    },
    {
        "MC2_TRACE_VISUAL_CAPTURE_ON_FIRE",
        "MC2_VISUAL_CAPTURE_ON_FIRE",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-8: GL twin of MC2_VK_PRESENT_DUMP_ON_FIRE. With "
        "MC2_FX_FORCE_SPAWN + MC2_VISUAL_CAPTURE_FRAME/_DIR (smoke mode), the "
        "engine visual capture triggers at the fixture-armed fire frame "
        "(g_mc2FxFireCaptureFrame, ~0.5s after the first volley) instead of the "
        "static frame pin, so GL-vs-VK weapon-class pairs capture the same "
        "fire-relative instant on both backends."
    },
    {
        "MC2_TRACE_VK_PRESENT_NATIVE_STRICT",
        "MC2_VK_PRESENT_NATIVE_STRICT",
        EnvVarKind::Trace,
        false,
        "VK present-tail deletion-readiness canary. Default-OFF. On nonzero-surface "
        "frames, aborts if the present path would use copy-1, consume a non-VK-owned "
        "present image, or signal the GL->VK handoff instead of the native "
        "MC2_VK_PRESENT_SKIP_GL_HANDOFF path. Minimized zero-extent frames remain "
        "non-proofs and do not trip this canary."
    },
    // VK-P13-SELECTOR-1 / T4: fail-soft proof knob for the engine VK context.
    {
        "MC2_FEATURE_VK_FORCE_FAIL",
        "MC2_VK_FORCE_FAIL",
        EnvVarKind::Trace,
        false,
        "VK-P13-SELECTOR-1: presence-gated =1, default OFF. Forces mc2vk::VulkanContext::init() "
        "to fail deterministically BEFORE creating any Vulkan object, so the =vulkan fail-soft "
        "path (log reason -> mc2::forceGLFallback -> GL for the session, no crash) can be "
        "exercised on demand. Only consulted when MC2_RENDER_BACKEND=vulkan. Diagnostic/proof "
        "only; no effect on the GL path or on a normal =vulkan run."
    },
    // VK-P8a SHADOW-INTEROP: the SECOND VK geometry region + FIRST VK-produces-depth gate.
    {
        "MC2_FEATURE_VK_SHADOW_INTEROP",
        "MC2_VK_SHADOW_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-P8a SHADOW-INTEROP: presence-gated =1, default OFF. Renders the DYNAMIC "
        "static-prop shadow-caster depth in Vulkan (the SECOND VK geometry region and the "
        "FIRST VK-PRODUCES-DEPTH region -- water was GL-produces/VK-consumes; shadow reverses "
        "it) into an exportable D32 VkImageResource. GL binds that shared D32 in place of "
        "gosPostProcess::getDynamicShadowTexture() and samples it via the UNCHANGED hardware "
        "sampler2DShadow calcShadow() path (shadow.hglsl) -- lighting up the vk2gl back-channel "
        "(glVkToGlSem / glInteropWaitFromVk) for the first time on a geometry region: VK submit "
        "SIGNALS vk2gl, GL WAITS it, then samples. Feed (VK-P8a STEP 0): the CPU "
        "GpuStaticPropInstance vector (dynShadowSet, 112B each) the GL path already builds each "
        "frame is memcpy'd into a VK instance SSBO; the shared static-prop VBO/IBO are copied "
        "ONCE per map into VK-owned buffers. Depth-only pipeline: LESS forward-Z (NOT reverse-Z; "
        "matches the GL dynamic shadow map), cull NONE, depthBias set to glPolygonOffset("
        "shadowBiasFactor_, shadowBiasUnits_) EXACTLY (the parity STOP-watch). Reproduces "
        "shadow_static_prop.vert math bit-for-bit (pos*M row-vector -> Stuff->MC2 swizzle -> "
        "lightSpaceMatrix*). BORROWS the shared engine VulkanContext "
        "(ensureEngineVulkanContext), no second device, no volkLoadDevice. Depth "
        "zero-copy; forbidden DEPTH-readback counters hard-asserted ==0. Surfaced in "
        "[VK_SHADOW_INTEROP] health. Gate OFF -> GL shadow path byte-identical. P8a = opaque "
        "static props, single map (foliage-alpha + terrain casters = P8b, mechs = P12)."
    },
    {
        "MC2_TRACE_VK_SHADOW_DIRECT_CHAIN",
        "MC2_VK_SHADOW_DIRECT_CHAIN",
        EnvVarKind::Trace,
        false,
        "VK shadow deletion-readiness proof gate. Default-OFF. When the VK shadow "
        "and CSM producers are armed with same-frame VK consumers, skips the "
        "VK->GL semaphore signal/wait and keeps the produced shadow images VK-owned. "
        "The dynamic single-map producer exposes only its direct VkImageView under "
        "this gate; GL texture-handle fallback remains disabled so no stale GL "
        "sampling can pass a no-interop proof. Normal shadow interop behavior is "
        "unchanged when unset."
    },
    // VK-P14 GATE-CONSOLIDATION-1: the ONE umbrella for the shared-GBuffer scene path.
    {
        "MC2_FEATURE_VK_SHARED_GBUFFER",
        "MC2_VK_SHARED_GBUFFER",
        EnvVarKind::Feature,
        false,
        "VK-P14 GATE-CONSOLIDATION-1: presence-gated =1, default OFF. THE UMBRELLA that "
        "arms the whole VK shared-GBuffer scene path at once — when ON, every member "
        "sub-gate DEFAULTS to ON: MC2_VK_SHARED_SCENE_DEPTH, MC2_VK_PROP_COLOR_TIER1, "
        "MC2_VK_PROP_MATERIAL, MC2_VK_TERRAIN_TIER1, MC2_VK_TERRAIN_LIGHTING, "
        "MC2_VK_TERRAIN_DETAIL, MC2_VK_TERRAIN_LODFADE, MC2_VK_TERRAIN_DYNSHADOW, MC2_VK_MECH_TIER1, "
        "MC2_VK_MECH_MATERIAL, MC2_VK_MECH_LIGHTING (resolved by mc2vkgate::subGate in "
        "GameOS/gameos/vk_shared_gbuffer_gate.h). Explicit sub-gate env ALWAYS wins as a "
        "debug OVERRIDE (=1 force-on, =0 force-off) over the umbrella default. The eventual "
        "default FLIP toggles EXACTLY this one name. Requires MC2_RENDER_BACKEND=vulkan (the "
        "shared engine VulkanContext); fail-soft to GL when absent. INDEPENDENT: the safety "
        "gates (MC2_VK_TERRAIN_HEIGHT_AUDIT verification-only + structural island fail-soft) "
        "and the debug/negative knobs (MC2_VK_PROP_MATERIAL_DEBUG, *_HEIGHT_AUDIT_CORRUPT) are "
        "and the debug/negative knobs (MC2_VK_PROP_MATERIAL_DEBUG, *_HEIGHT_AUDIT_CORRUPT) are "
        "NOT umbrella members. Umbrella unset/!=1 with no sub-gate forced = pure GL, "
        "byte-identical to today (near_land_off self-diff 0.000)."
    },
    {
        "MC2_TRACE_VK_SHARED_SCENE_STRICT",
        "MC2_VK_SHARED_SCENE_STRICT",
        EnvVarKind::Trace,
        false,
        "VK shared-scene deletion-readiness canary. Default-OFF. When =1, any "
        "fallback from a requested VK-backed shared scene color/depth/normal/objectId "
        "slot to the plain GL allocation logs [VK_SHARED_SCENE_STRICT] and aborts, "
        "so final-tail/Metal-gate proof runs cannot silently pass through a GL "
        "substrate rescue. No effect unless a shared-scene slot was requested and "
        "failed to become VK-backed."
    },
    {
        "MC2_TRACE_VK_NO_GL_INTEROP_STRICT",
        "MC2_VK_NO_GL_INTEROP_STRICT",
        EnvVarKind::Trace,
        false,
        "VK Metal-gate canary. Default-OFF. Historically aborted on any production "
        "GL/VK interop boundary call (glInteropSignalToVk / glInteropWaitFromVk). Those "
        "functions were DELETED (VK-RUNG8-ITEM1), so that class is now structurally "
        "impossible. The name survives as a live guard on the terrain island's per-frame "
        "GL depth-readback fallback (vulkan_terrain_island.cpp) -- the remaining GL-touch "
        "class -- and as a checker trip-wire."
    },
    {
        "MC2_TRACE_VK_NO_GL_INTEROP_RESOURCE_STRICT",
        "MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT",
        EnvVarKind::Trace,
        false,
        "VK Metal-gate resource canary. Default-OFF. Historically aborted on creation of "
        "an exportable Win32 VkImageResource or a GL memory-object import (glInteropViewCreate). "
        "Both the exportable vk_image_resource half (VK-RUNG8-ITEM2) and gl_interop_view "
        "(VK-RUNG8-ITEM1) were DELETED, and the Win32 external-memory device extension family "
        "was removed (VK-RUNG8-ITEM3), so no exportable resource can be created -- this canary "
        "is now permanently satisfied BY CONSTRUCTION (no code reader remains). The strict "
        "harness still sets it and the checker still requires it as a provenance receipt."
    },
    {
        "MC2_FEATURE_VK_CSM_FULLMAP_INTEROP",
        "MC2_VK_CSM_FULLMAP_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-CSM-FULLMAP-DIRECT-1: presence-gated =1, default OFF. Companion to "
        "MC2_VK_SHADOW_INTEROP that re-renders the CSM catch-all/full-map cascade into "
        "a separate exportable D32 image on the shared Vulkan device, then lets VK terrain "
        "bind that VkImageView directly at CSM binding 9. Deletes terrain's per-frame "
        "glGetTexImage + vkCmdCopyBufferToImage full-map import when the producer succeeds; "
        "the near cascade array remains on the existing copy path. Gate OFF or producer "
        "decline -> existing GL full-map copy path unchanged."
    },
    {
        "MC2_FEATURE_VK_CSM_ARRAY_INTEROP",
        "MC2_VK_CSM_ARRAY_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-CSM-ARRAY-DIRECT-1: presence-gated =1, default OFF. Companion to "
        "MC2_VK_SHADOW_INTEROP that re-renders the near CSM cascades into layers of "
        "a VK-owned D32 2D array on the shared Vulkan device, then lets VK terrain "
        "bind that array view directly at CSM binding 8. Deletes terrain's per-frame "
        "GL_TEXTURE_2D_ARRAY scratch/cpu-readback/import path when all near layers "
        "produce in the same frame; gate OFF or producer decline -> existing GL array "
        "copy path unchanged."
    },
    // VK-TIER1 SHARED SCENE DEPTH: the shared-depth substrate slice (step 2).
    {
        "MC2_FEATURE_VK_SHARED_SCENE_DEPTH",
        "MC2_VK_SHARED_SCENE_DEPTH",
        EnvVarKind::Feature,
        false,
        "VK-TIER1 SHARED SCENE DEPTH: presence-gated =1, default OFF. Backs the engine "
        "scene-depth texture (sceneDepthTex_) with a VK-BUILT exportable D32_SFLOAT_S8_UINT "
        "VkImageResource imported into GL as a GL_DEPTH24_STENCIL8 texture (depth+stencil "
        "aspect usable for BOTH sampling AND FBO depth-stencil attachment), substituted at "
        "the SINGLE scene-FBO depth alloc+attach site (gos_postprocess createFBOs). GL-ONLY "
        "slice: GL still renders scene depth into it and the 42 depth-sample consumers "
        "(SSAO/edge-fog/OOB-fog/HZB/DOF/god-rays/cluster/particle-copy/stencil-view) sample "
        "it unchanged -- there is NO VK depth consumer yet (that is the P11 Tier-1 slice). "
        "The P5-T0 + depth-test spikes proved the D32S8-backed image imported under "
        "GL_DEPTH24_STENCIL8 samples the DEPTH aspect exactly (0 val errors), so the swap is "
        "transparent. BORROWS the shared engine VulkanContext (ensureEngineVulkanContext), "
        "no second device. FAIL-SOFT: if the shared VK context/interop is unavailable at "
        "scene-FBO creation, falls back to the plain GL GL_DEPTH24_STENCIL8 alloc (logs the "
        "reason) -> gate-ON with no interop degrades to the GL path, never crashes. Rebuilt on "
        "resize (destroyFBOs tears down + createFBOs rebuilds). Surfaced in [VK_SHARED_SCENE_DEPTH] "
        "log line. Gate OFF (or island not compiled) = byte-identical GL scene-depth path."
    },
    {
        "MC2_FEATURE_VK_STATIC_SHADOW_MAP",
        "MC2_VK_STATIC_SHADOW_MAP",
        EnvVarKind::Feature,
        false,
        "DEPRECATED no-op (VK-RUNG8-ITEM6). The VK-backed static-shadow import substrate "
        "(exportable D32_SFLOAT image imported into GL as GL_DEPTH_COMPONENT32F, plus the "
        "whole vulkan_shared_static_shadow TU, the gl_interop_view TU, and the exportable-"
        "image half of vk_image_resource) was DELETED. The static sun shadow map "
        "(ShadowStaticMap / shadowDepthTex_) is now always the plain GL GL_DEPTH_COMPONENT24 "
        "allocation; a non-CSM static-only screen-shadow config declines to the GL screen-"
        "shadow pass (CSM screen-shadow is unaffected -- it samples the direct VK CSM views). "
        "Retiring this closed the last Win32 external-memory MoltenVK/Metal-gate blocker. "
        "Setting the env prints a one-shot deprecation note and changes nothing."
    },
    // VK-P11 TIER-1 PROP COLOR: in-place shared-GBuffer co-write (Tier-1, supersedes Tier-0).
    {
        "MC2_FEATURE_VK_PROP_COLOR_TIER1",
        "MC2_VK_PROP_COLOR_TIER1",
        EnvVarKind::Feature,
        false,
        "VK-P11 TIER-1 PROP COLOR: presence-gated =1, default OFF; REQUIRES "
        "MC2_VK_SHARED_SCENE_DEPTH. Umbrella family member (armed by MC2_VK_SHARED_GBUFFER=1; "
        "explicit =0/1 overrides). Co-writes the VISIBLE static-prop color instances IN-PLACE "
        "into the SHARED scene color(MRT0)+normal(MRT1)+depth GBuffer (GEQUAL reverse-Z, no "
        "composite) instead of the Tier-0 side-image path (which it REPLACES/supersedes). "
        "sceneColorTex_/sceneNormalTex_ become GL<->VK interop-shared exportable RGBA16F images "
        "at their single scene-FBO alloc site (gos_postprocess createFBOs). BORROWS the shared "
        "engine VulkanContext; FAIL-SOFT to the GL flush() prop path when the shared GBuffer/"
        "context is unavailable. Gate OFF (or require missing) = byte-identical GL prop color path."
    },
    // VK-PROP-DIRECT-CHAIN: proof slice for keeping prop color on the Vulkan-owned shared GBuffer.
    {
        "MC2_FEATURE_VK_PROP_DIRECT_CHAIN",
        "MC2_VK_PROP_DIRECT_CHAIN",
        EnvVarKind::Feature,
        false,
        "VK-PROP-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF. Requires "
        "MC2_VK_PROP_COLOR_TIER1 plus MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR. When ON, the prop "
        "color island treats the shared scene color0/normal/depth images as already Vulkan-owned "
        "from the terrain prefix and skips its GL->VK/VK->GL interop semaphore handoff, while "
        "still CPU-waiting its submit fence before the next island. Used with "
        "MC2_VK_NO_GL_INTEROP_STRICT to advance the final Metal-gate canary beyond props."
    },
    // VK-P10 TERRAIN TIER-1: the terrain in-place shared-GBuffer co-write region.
    {
        "MC2_FEATURE_VK_TERRAIN_TIER1",
        "MC2_VK_TERRAIN_TIER1",
        EnvVarKind::Feature,
        false,
        "VK-P10 TERRAIN TIER-1: presence-gated =1, default OFF (requires MC2_VK_SHARED_SCENE_DEPTH). "
        "The VK terrain island co-writes the LIVE LOD-chunk opaque MAIN-triangle draw for the "
        "nearest (LOD0, lodStep==1) patches IN-PLACE into the SHARED scene color(MRT0)+normal(MRT1)+"
        "depth GBuffer as the opaque PREFIX, then GL/props/mechs continue on top. Terrain analogue of "
        "the prop Tier-1 island: REUSES its shared-GBuffer plumbing + LOAD-preserve co-write renderpass "
        "+ reverse-Z GEQUAL double-sided pipeline verbatim. gbuffer1 = vec4(0.5,0.5,1.0,1.0), a=1.0 "
        "(terrain shadow-class 'handles own shadow'; props use a=0.25). Consumes the GL height SSBO "
        "(binding 23) via a per-change interop snapshot (glGetBufferSubData map-load, NOT a per-frame "
        "handoff readback); CPU-side LOD0 chunk selection stays on GL/CPU. When ON the GL LOD-chunk "
        "loop SKIPS the LOD0 main+skirt draws (both paths exclude LOD0 skirts for like-for-like A/B). "
        "SCOPE = LOD0 main-triangle ONLY (skirts / coarser bands / edge-stitch / visual-displace "
        "DEFERRED; non-LOD0 bands stay on GL). Extends the postprocess shared-scene color/normal "
        "conversion to trigger for this gate too (mc2VkSharedSceneColorNeeded). Fires markTerrainDrawn "
        "so terrain-gated post passes stay live. FAIL-SOFT: shared GBuffer not ready / interop "
        "unavailable -> GL fallback. Surfaced in [VK_TERRAIN_TIER1] health/log. Gate OFF (or island "
        "not compiled) = byte-identical GL LOD-chunk terrain path."
    },
    // VK-TERRAIN-NATIVE-PREFIX-CLEAR: RETIRED (VK-RUNG8-ITEM4-TERRAIN-DELETE-1).
    {
        "MC2_FEATURE_VK_TERRAIN_NATIVE_PREFIX_CLEAR",
        "MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR",
        EnvVarKind::Feature,
        false,
        "DEPRECATED no-op (VK-RUNG8-ITEM4-TERRAIN-DELETE-1). The terrain island's hybrid GL<->VK "
        "semaphore-handoff arm (LOAD-preserve renderpass) was DELETED: the armed terrain Tier-1 island "
        "is now unconditionally the native first owner of shared scene color0/normal/depth (CLEAR loadOp "
        "matching the gameplay/menu clear color, clearGBuffer1 sentinel, and reverse-Z far; GL frame-entry "
        "clears skipped via mc2vkgate::terrainNativeFirstOwnerOn()). Setting this env prints a one-line "
        "deprecation note and changes nothing; the name stays parseable for old command lines."
    },
    // VK-TERRAIN-LIGHTING: make the VK Tier-1 terrain LIT (directional relief + baked static shadow).
    {
        "MC2_FEATURE_VK_TERRAIN_LIGHTING",
        "MC2_VK_TERRAIN_LIGHTING",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-LIGHTING: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_TIER1). Makes the "
        "VK terrain island's LOD0 co-write LIT instead of flat: reproduces the GL LOD-chunk NdotL relief "
        "band (terrain_lod_chunk.frag:891-893, mix(0.35,1.20,clamp(NdotL,0.02,1.0))) + the baked STATIC "
        "sun shadow (shaders/include/shadow.hglsl calcShadow, 16-tap Poisson PCF) via a REUSABLE VK GLSL "
        "helper (shaders/vulkan/include/lighting_shadow_vk.glsl; props/mechs reuse computeNdotLBand later). "
        "The static shadow map is copied PER FRAME (glGetTexImage of the GL DEPTH_COMPONENT24 shadow "
        "texture -> VK D32 depth-COMPARE image, compareOp=LESS_OR_EQUAL, LINEAR HW PCF, CLAMP_TO_BORDER "
        "white=1.0) at binding 5. Cement is flat-lit + static-shadow-dropped (mirrors GL pureConcrete). "
        "DEFERRED vs GL: detail-normal/POM/macro relief, DYNAMIC (mech/building) shadow + CSM, hemisphere "
        "ambient fill. FAIL-SOFT: shadow map not importable -> shadow=1.0 (lit, no shadow), never a hole. "
        "Gate OFF = byte-identical to the pre-lighting VK colormap+cement frame."
    },
    {
        "MC2_FEATURE_VK_TERRAIN_DETAIL",
        "MC2_VK_TERRAIN_DETAIL",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-DETAIL: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_LIGHTING). Perturbs the "
        "flat-up terrain normal before NdotL to close the flat-grass residual: (1) macro-slope normal from "
        "the height SSBO bilinear central-difference (smoothTerrainNormal, terrain_lod_chunk.frag:165-173, "
        "always on) + (2) per-material weighted detail-normal from matNormalArray (9-layer 2D_ARRAY, "
        "copy-once import, GL_REPEAT) via the colormap classifier, composed exactly as GL (pert = "
        "(baseN.xy/baseN.z*nfhStrength + dN.xy*detailStrength, 1)). Detail suppressed on pureConcrete; shadow "
        "still sampled with flat-up N (parity). EXACT: macro-slope, classifier, pert, NdotL band. DEFERRED: "
        "POM, anti-tile, per-layer fwidth fade, LOD-tier detail fade, painted-concrete/marble-cliff normal "
        "(oblique/grazing-angle residual remains). FAIL-SOFT: detail array not ready -> macro-slope only "
        "(still a win), no hole. Gate OFF = byte-identical to lighting-only VK frame."
    },
    {
        "MC2_FEATURE_VK_TERRAIN_DYNSHADOW",
        "MC2_VK_TERRAIN_DYNSHADOW",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-DYNSHADOW: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_LIGHTING). Adds the GL "
        "DYNAMIC (building/mech) sun shadow onto the VK terrain (legacy single-map path, non-CSM). Per-frame "
        "copies pp->getDynamicShadowTexture()+getDynamicLightSpaceMatrix() into a 2nd VK D32 depth-compare "
        "image (binding 7), then composes exactly as GL (terrain_lod_chunk.frag:896-907): shadow = "
        "min(static, dyn); on pureConcrete shadow = dyn (static dropped, dynamic kept so structures still "
        "cast onto roads/pads). Reuses computeBakedShadow (VERBATIM legacy calcDynamicShadow: adaptiveScale "
        "2.4->0.8, mix(0.4,1.0,shadow), 8 taps). FAIL-SOFT: GL dynamic map not live / CSM active -> no "
        "dynamic term (lit-only), no hole. Gate OFF = byte-identical to the pre-dynshadow VK frame."
    },
    {
        "MC2_FEATURE_VK_TERRAIN_LODFADE",
        "MC2_VK_TERRAIN_LODFADE",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-LODFADE: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_DETAIL). Ports GL's "
        "camera-distance lodNear fade (gos_terrain.frag:322-328,616-736: LOD_NEAR=4000/LOD_NEAR_FADE=4500 "
        "smoothstep) so the VK terrain's detail-normal + POM fade to 0 across the same LOD0/band boundary GL "
        "fades at, closing the VK-LOD0-vs-GL-band color seam. Feeds gos_GetTerrainCameraPos() (Stuff/MLR "
        "space) into the UBO (existing pad tail, no slot growth) each frame; the frag recomputes camDist/"
        "lodNear verbatim and multiplies the POM offset + the whole detail-normal contribution by it. FAIL-"
        "SOFT: gate off -> lodNear==1.0 always (byte-identical to the pre-fade VK-TERRAIN-DETAIL frame)."
    },
    {
        "MC2_FEATURE_VK_TERRAIN_CSM",
        "MC2_VK_TERRAIN_CSM",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-CSM (biggest-safe-subset): presence-gated =1, default OFF (requires MC2_VK_TERRAIN_DYNSHADOW "
        "chain + CSM active). When MC2_SHADOW_CSM is ON (launcher default) the single-map dynshadow path is "
        "skipped, so VK terrain had NO object shadow up close. This feeds the CSM FULL-MAP cascade (the last "
        "cascade — a single GL_TEXTURE_2D DEPTH_COMPONENT tex, byte-shape-identical to the single dyn map) "
        "through the existing binding-7 slot + computeBakedShadow, giving VK terrain whole-map object shadows. "
        "The near-cascade ARRAY (sampler2DArrayShadow + N matrices) is DEFERRED. FAIL-SOFT: full-map tex not "
        "live -> no dynamic term, no hole. Gate OFF = byte-identical (single-map dynshadow path unchanged)."
    },
    {
        "MC2_FEATURE_VK_PROP_MATERIAL",
        "MC2_VK_PROP_MATERIAL",
        EnvVarKind::Feature,
        false,
        "VK-PROP-MATERIAL: presence-gated =1, default OFF (requires MC2_VK_PROP_COLOR_TIER1). Replaces the "
        "VK prop Tier-1 flat DEBUG teal with real textured VERTEX-BAKED-lit materials (NOT PBR — GL props are "
        "vertex-baked by default, u_pbrV1Strength=0). Copy-once imports BOTH static-prop albedo 2D_ARRAYs "
        "(s_texArrayOff binding 3 + s_texArrayOn binding 6; per-type group-relative layer tables bindings 2/5; "
        "RGBA8->R8G8B8A8_UNORM or BC7->BC7_*_BLOCK per real internalformat, full mips, LINEAR_MIPMAP_LINEAR/"
        "CLAMP). Alpha-ON (class-1, alpha-MIXED) types route to s_texArrayOn with the GL 0.5 alpha-test "
        "discard (static_prop.frag:236). Frag = albedo * vArgb where vArgb = per-vertex base_light seed "
        "(a_aRGBLight loc4) + stand-in ambient + sun*NdotL. GBuffer1 unchanged (a=0.25). FAIL-SOFT: array "
        "not ready / layer<0 -> teal. DEFERRED vs GL parity: ObjectLights ambient + per-type magic hot-color "
        "SSBO + per-actor sun/highlight/fog (stand-in constants; lighting is approximate model, not byte-"
        "parity), MaterialGpu roughness/metallic, BC7-bucketed arrays. SEPARATE parked issue: Tree/Generic "
        "POPULATION coverage (only Building population fed). Gate OFF = byte-identical VK prop path."
    },
    {
        "MC2_FEATURE_VK_PROP_MATERIAL_DEBUG",
        "MC2_VK_PROP_MATERIAL_DEBUG",
        EnvVarKind::Feature,
        false,
        "VK-PROP-MATERIAL-DEBUG: presence-gated =1, default OFF. One-shot [GPUPROPS_HIST v2] instance-weighted "
        "layer histogram (textured/alpha_on/no_primary/total) from the material feed — diagnostic only, no "
        "render effect. Gate OFF = no log, byte-identical."
    },
    {
        "MC2_FEATURE_VK_MECH_TIER1",
        "MC2_VK_MECH_TIER1",
        EnvVarKind::Feature,
        false,
        "VK-MECH-TIER1: presence-gated =1, default OFF (requires MC2_VK_SHARED_SCENE_DEPTH). Renders mechs "
        "IN-PLACE into the shared scene GBuffer (vulkan_mech_island.cpp): VK-skinned (VS linear-blend over "
        "the CPU-filled bone-palette SSBO + MC2 axis swap, mirrors mech.vert; copy-once VBO/IBO, per-frame "
        "memcpy instance+bone SSBOs), LOAD-preserve MRT0 color + MRT1 normal + shared D32S8 depth, GEQUAL "
        "reverse-Z, gbuffer1 .a=0.0 (screen-shadow eligible, GL screen pass shadows). Gate-ON skips the GL "
        "mech color flush for covered types; island co-writes. SKINNING PROVEN (13 mechs, correct biped "
        "silhouette). DEFERRED: material (MC2_VK_MECH_MATERIAL, single copy-once albedo probe; per-actor "
        "paint-scheme handle->descriptor cache = general case), lighting (VS calc_light Lambert + LightsData "
        "UBO — NOT computePBR/vertex-baked). FAIL-SOFT: shared GBuffer/context unavailable -> GL flush. "
        "Gate OFF = byte-identical GL mech path."
    },
    {
        "MC2_FEATURE_VK_VFX",
        "MC2_VK_VFX",
        EnvVarKind::Feature,
        false,
        "VK-FIELD-VFX-1: umbrella sub-gate, default-ON under MC2_VK_SHARED_GBUFFER (explicit =0 "
        "forces off, =1 forces on). Renders the GPU-particle billboard stream (gos_particle_bridge) "
        "IN-PLACE into the shared scene color0 as translucent, depth-tested (GEQUAL reverse-Z, "
        "NO depth write) colored soft billboards (vulkan_vfx_island.cpp), so weapon fire (tracers, "
        "muzzle flashes, impact glows) survives the VK-only present umbrella instead of drawing into "
        "the discarded GL backbuffer. UNTEXTURED slice (per-particle color + radial soft falloff); "
        "textured sprite atlases, the tube-ribbon beams, and the gosFX MESH shapes are the deferred "
        "remainder. Gate resolves OFF when the umbrella is off -> GL particle draw byte-identical."
    },
    {
        "MC2_FEATURE_VK_VFX_COALESCE",
        "MC2_VK_VFX_COALESCE",
        EnvVarKind::Feature,
        true,
        "VK-VFX-COALESCE-1: merges consecutive same-render-state HUD tri-batch draws (gosRenderer::"
        "drawTris IsHUD branch, gameos_graphics.cpp) into ONE hudBatch_ entry instead of one entry PER "
        "TRIANGLE. MLRSortByOrder emits gosFX weapon-VFX geometry (tracers/flashes/impacts) one triangle "
        "at a time (mclib/mlr/mlrsortbyorder.cpp:589-609); under a dense multi-mech volley this measured "
        "a peak of 251-349 hudBatch_ entries/frame vs a ~14-entry quiet floor. Paint order is preserved "
        "exactly (triangles already arrive depth-sorted; a real state change via SetDifferences still "
        "forces a new entry); a same-side-of-the-HUD-shrink-band guard prevents the merge from changing "
        "the bottom-band s_hud_scale shrink decision for any individual triangle. Independent of the "
        "MC2_VK_VFX umbrella -- this is a CPU-side hudBatch_ data-structure merge that benefits both the "
        "VK native HUD tail (fewer VkHudDraw records) and the GL replay path (fewer drawTris() re-entries) "
        "identically. DEFAULT ON: draw-count A/B (mc2_01, forced fire) showed peak collapse 349->109 with "
        "[VK_HUD_AUDIT v1] vertices= totals bit-identical to the legacy path at every matching sample; "
        "zero declines/fallbacks/validation errors; fire-capture matches the banked VFX-9 reference. "
        "MC2_VK_VFX_COALESCE=0 restores the legacy one-record-per-triangle path."
    },
    {
        "MC2_FEATURE_VK_MECH_DIRECT_CHAIN",
        "MC2_VK_MECH_DIRECT_CHAIN",
        EnvVarKind::Feature,
        false,
        "VK-MECH-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF. Requires "
        "MC2_VK_MECH_TIER1 plus MC2_VK_PROP_DIRECT_CHAIN. When ON, the mech island treats "
        "the shared scene color0/normal/depth images as already Vulkan-owned by terrain+prop "
        "and skips its GL->VK/VK->GL interop semaphore handoff, while still CPU-waiting its "
        "submit fence before the next island. Used with MC2_VK_NO_GL_INTEROP_STRICT to advance "
        "the final Metal-gate canary beyond mechs."
    },
    {
        "MC2_FEATURE_VK_WATER_DIRECT_CHAIN",
        "MC2_VK_WATER_DIRECT_CHAIN",
        EnvVarKind::Feature,
        false,
        "VK-WATER-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF. Requires "
        "MC2_VK_WATER_TIER1 plus MC2_VK_MECH_DIRECT_CHAIN. When ON, the water island treats "
        "the shared scene color0/normal/depth images as already Vulkan-owned by terrain+prop+mech "
        "and skips its GL->VK/VK->GL interop semaphore handoff, while still CPU-waiting its submit "
        "fence before the next island. Used with MC2_VK_NO_GL_INTEROP_STRICT to advance the final "
        "Metal-gate canary beyond water."
    },
    {
        "MC2_FEATURE_VK_POST_COMPOSITE_DIRECT_CHAIN",
        "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN",
        EnvVarKind::Feature,
        false,
        "VK-POST-COMPOSITE-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF. "
        "Requires MC2_VK_POST_COMPOSITE plus the full terrain->prop->mech->water direct-chain "
        "gate stack. Samples shared scene color0 as already Vulkan-owned and skips the scene "
        "color GL->VK/VK->GL interop semaphore handoff, while still CPU-waiting the submit "
        "fence before present consumes the VK-owned present image. ObjectId/thermal modes "
        "still synchronize MRT2 separately until objectId ownership is ported."
    },
    {
        "MC2_FEATURE_VK_MECH_MATERIAL",
        "MC2_VK_MECH_MATERIAL",
        EnvVarKind::Feature,
        false,
        "VK-MECH-MATERIAL: presence-gated =1, default OFF (requires MC2_VK_MECH_TIER1). Copy-once imports the "
        "first mech run's live slot-0 paint texture (single 2D, un-lit) so the VK mech is textured instead of "
        "flat. Per-frame per-actor paint-scheme handle->VK-descriptor cache DEFERRED (probe = one static "
        "actor). Gate OFF = flat probe color."
    },
    {
        "MC2_FEATURE_VK_MECH_LIGHTING",
        "MC2_VK_MECH_LIGHTING",
        EnvVarKind::Feature,
        false,
        "VK-MECH-MATERIAL-LIGHTING-1: presence-gated =1, default OFF; REQUIRES MC2_VK_MECH_TIER1 "
        "(visible only with MC2_VK_MECH_MATERIAL=1). Umbrella family member (armed by "
        "MC2_VK_SHARED_GBUFFER=1; explicit =0/1 overrides). Ports the GL DEFAULT mech shading into "
        "the VK mech island: per-vertex calc_light Lambert (verbatim lighting.hglsl 4-param kernel) "
        "+ albedo*v_litColor+highlight + per-actor haze. The live GL LightsData SSBO (binding 20) is "
        "copied per-frame into a VK SSBO (glGetBufferSubData buffer-copy, forbidden_* stay 0). NOT the "
        "PBR/StandardLit-GGX path (needs MC2_MECH_VIEWUNIFORMS+MC2_STANDARD_LIT_V1, both default-OFF) "
        "-> matches the default deploy mech look. FAIL-SOFT: LightsData unavailable -> flat-white lit. "
        "Gate OFF = un-lit albedo (or flat, per material gate)."
    },
    // GOSFX-TUBE-RIBBON-1: gosFX Tube swept-quad ribbon oracle.
    {
        "MC2_VFX_ORACLE_TUBE",
        "MC2_VFX_ORACLE_TUBE",
        EnvVarKind::Feature,
        false,
        "GOSFX-TUBE-RIBBON-1: route supported gosFX Tube trails (MissileSmoke alpha, PPC additive, polygon) through the swept-quad ribbon oracle submit path instead of legacy MLR. REVERTED to Default-OFF (GPU oracle looked broken -- flat grey sheets); =1 opts into the GPU path. Unsupported Tube classes fall back to MLR. No object-ID writes."
    },
    {
        "MC2_VFX_ORACLE_TUBE_LOG",
        "MC2_VFX_ORACLE_TUBE_LOG",
        EnvVarKind::Trace,
        false,
        "GOSFX-TUBE-RIBBON-1: per-Tube ribbon submit counters (FIRST_HARVEST / v2 vertex+index tallies, unsupported->MLR fallbacks). Default-OFF; =1 enables. Diagnostic only, no behavior change."
    },
    {
        "MC2_VFX_ORACLE_TUBE_COVERAGE",
        "MC2_VFX_ORACLE_TUBE_COVERAGE",
        EnvVarKind::Trace,
        false,
        "GOSFX-TUBE-RIBBON-1: wrap the deferred ribbon draw in a GL_SAMPLES_PASSED occlusion query; logs [VFX_ORACLE_TUBE coverage] ribbons/samples/fbo/sceneDrawBufs per frame. Deterministic proof (immune to mech-pose nondeterminism) that oracle tube fragments rasterize+pass-depth into the composited scene FBO. Default-OFF; =1 enables. Diagnostic only (query result read stalls), no behavior change."
    },
    // STATICPROP-PERMANENT-INSTANCE-LIGHTS-1 (Slice 1)
    {
        "MC2_TRACE_LIGHTBAKE_STABILITY",
        "MC2_LIGHTBAKE_STABILITY",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: per-instance lightDataIndex permanence/stability proof. Logs [LIGHTBAKE-PROOF v1] event=first / UNSTABLE (capped 32). Default-off; no behavior change."
    },
    {
        "MC2_TRACE_LIGHTBAKE_PARITY",
        "MC2_LIGHTBAKE_PARITY",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: baked permanent light record == gathered transient record byte/hash parity. Logs [LIGHTBAKE-PROOF v1] event=parity match=1 (capped 32). Default-off; no behavior change."
    },
    {
        "MC2_FEATURE_STATIC_LIGHT_UPLOAD_SPLIT",
        "MC2_STATIC_LIGHT_UPLOAD_SPLIT",
        EnvVarKind::Feature,
        true,
        "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: upload the immutable static light prefix [0..S) once/dirty-only + the dynamic suffix [S..count) per frame, instead of the whole LightsData SSBO every frame. Default-ON; =0 = legacy whole-buffer upload (bit-identical GPU contents). Requires MC2_LIGHTBAKE; bypassed when bake is off."
    },
    {
        "MC2_FEATURE_STATIC_PROP_FLUSH_CACHED_BLOB",
        "MC2_STATIC_PROP_FLUSH_CACHED_BLOB",
        EnvVarKind::Feature,
        false,
        "STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A: replace the per-leaf registry-flush rebuild with cached immutable instance/actor-record blobs bulk-appended into the existing rings. Keeps the per-frame range walk + staleness skip (does NOT zero flush). Default-OFF pending Tracy proof; =1 enables."
    },
    {
        "MC2_TRACE_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE",
        "MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A diagnostic: with the cached path active, also build the legacy temp instance+record per leaf and compare hash/count; logs mismatches. Default-OFF; requires MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1."
    },
    {
        "MC2_FEATURE_STATIC_PROP_PERSISTENT_BUCKETS",
        "MC2_STATIC_PROP_PERSISTENT_BUCKETS",
        EnvVarKind::Feature,
        true,
        "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 (Mechanism B-reinject): persistent static instance store, rebuilt only on registry-generation change, bulk-injected into s_bucketsByType each frame; skips the per-frame static re-push. DEFAULT-ON (user-Tracy StaticPropRegistryFlush 312us->68us at wolfman, ~78%). Kill-switch =0 restores per-frame re-push."
    },
    {
        "MC2_TRACE_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE",
        "MC2_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 oracle: FNV-compare the persistent-store-sourced per-type instance stream vs a freshly-rebuilt legacy stream; logs mismatches. Default-OFF; requires MC2_STATIC_PROP_PERSISTENT_BUCKETS=1."
    },
    {
        "MC2_TRACE_BUCKET_ORDER",
        "MC2_BUCKET_ORDER_TRACE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 Task-0 probe: per-type static-vs-dynamic add counts + order ([BUCKET_ORDER v1]). Diagnostic; default-OFF."
    },
    {
        "MC2_FEATURE_STATIC_PROP_COLORS_FILL",
        "MC2_STATIC_PROP_COLORS_FILL",
        EnvVarKind::Feature,
        false,
        "STATICPROP-COLORS-FILL-DEBUGONLY-1: per-static-instance Colors SSBO (binding 1) zero-fill. No production shader reads colors_ (coalesce multidraw never binds binding 1; addr-mode 4 reads v_argb). DEFAULT-SKIP (~80ns/leaf saved); =1 restores the legacy fill (kill-switch). Dynamic submit() path untouched."
    },
    {
        "MC2_FEATURE_BURNIN_NO_JPG",
        "MC2_BURNIN_NO_JPG",
        EnvVarKind::Feature,
        false,
        "BURNIN-DISK-JPEG kill-switch: when set, skip the <name>.burnin.jpg sidecar probe in TerrainColorMap::init and load the full <name>.burnin.tga (or .tga) instead. Default-unset = JPEG sidecar preferred when present (stb_image decode, ~9:1 disk; lossy but the burn-in colormap is a low-visibility tiled/minified ground texture). On JPEG absence/decode-error the runtime falls through to the .tga path regardless of this var. No gameplay/geometry change; visual delta is JPEG quantization only."
    },
    {
        "MC2_TRACE_FORCE_LOD",
        "MC2_FORCE_LOD",
        EnvVarKind::Trace,
        false,
        "TREE-OVERRIDE-LOD-MVP-1 (Task 5): force the active render-LOD index for override trees (0..MAX_LODS-1), clamped to the highest registered+available LOD. Overrides distance selection. Default-unset = distance-driven. Read per-frame in TreeAppearance::update; validates the per-LOD draw path before distance bands ship. No gameplay change."
    },
    {
        "MC2_TRACE_RENDER_WORLD",
        "MC2_RENDER_WORLD_TRACE",
        EnvVarKind::Trace,
        false,
        "Per-frame [RENDER_WORLD v1] banner + per-event logs (RenderWorld.cpp). Default-off; =1 enables."
    },
    {
        "MC2_TRACE_SHADOW_FRUSTUM",
        "MC2_SHADOW_FRUSTUM_DIAG",
        EnvVarKind::Trace,
        false,
        "SHADOW-FRUSTUM-AUDIT-1: read-only per-frame dynamic sun-shadow coverage probe in buildDynamicLightMatrix (sun dir, frustum XY, fit/xy radius, map clamp, texel WU, ortho WxH, depth). Default-off; =1 enables. No behavior change."
    },
    {
        "MC2_FEATURE_SHADOW_BOUNDED_NEAR_FIT",
        "MC2_SHADOW_BOUNDED_NEAR_FIT",
        EnvVarKind::Feature,
        true,
        "SHADOW-BOUNDED-NEAR-FIT-1: cap the dynamic sun-shadow frustum fit radius to a small camera-centered region (radius from MC2_SHADOW_BOUNDED_NEAR_RADIUS, default 2500wu) for higher texel density. **DEFAULT ON; opt out with MC2_SHADOW_BOUNDED_NEAR_FIT=0.** GATE-DEFAULT-TRUTH-1 (2026-07-30) corrected this entry. THREE written sources said OFF -- this registry entry (`false`, \"Default-off; =1 enables\"), docs/tier1_env_vars.md, and the reader's OWN comment two lines above itself (\"gate, default OFF\") -- while the reader at gos_postprocess.cpp:6749-6752 does `return !(v && v[0] == '0'); // default ON; opt out with =0`. So every default launch has always capped the sun-shadow fit to a 2500wu camera-centered radius, trading far-map coverage for near crispness, and any A/B graded \"=1 vs unset\" compared ON against ON. The reader is what ships, so the documentation now matches it rather than the reverse: flipping the reader to OFF would change default shadow coverage for every user, which is a visual decision and not a documentation fix. **If the original OFF intent was correct, the one-line change is in the reader and it needs a capture pass.** Gate OFF = full-frustum fit. Applied before pow-2/texel snap (snap preserved). Tunable radius is only consulted when this gate is ON."
    },
    {
        "MC2_TUNE_SHADOW_BOUNDED_NEAR_RADIUS",
        "MC2_SHADOW_BOUNDED_NEAR_RADIUS",
        EnvVarKind::Trace,
        false,
        "SHADOW-BOUNDED-NEAR-FIT-1 tunable: bounded near-fit radius in world units (default 2500, clamped 512..mapClampR). Only consulted when MC2_SHADOW_BOUNDED_NEAR_FIT=1. Resolved once at process start."
    },
    {
        "MC2_FEATURE_SHADOW_GUARD_CLAMPED_SUN",
        "MC2_SHADOW_GUARD_CLAMPED_SUN",
        EnvVarKind::Feature,
        true,
        "SHADOW-SUNCLAMP-1: feed the shadow_screen.frag back-face guard (`if (NdotL < 0.05) return 1.0;`) the SAME altitude-clamped sun the shadow matrices are built from. gameos_graphics.cpp:10197 passes -gos_GetTerrainLightDir() into buildStaticLightMatrix (which runs mc2ClampShadowSunAltitude, 15deg min); runScreenShadow uploaded the byte-identical raw expression UNCLAMPED, so on a near-horizontal authored sun the guard degenerated to a purely azimuthal test and discarded real shadows on every surface leaning away from the sun azimuth -- the mc2_10 night 'shadowed half / unshadowed half' world split. Default-ON; =0 reverts to the raw-sun guard. No-op (byte-identical) wherever sin(LightDirPitch) >= sin(15deg): mc2_01/03/24 unaffected; only mc2_10 (LightDirPitch=180 -> 0.0) changes. Shading light dir / terrain N-dot-L is a separate uniform and is NOT touched -- brightness is unchanged, only wrongly-discarded shadows return."
    },
    {
        "MC2_FEATURE_TERRAIN_SHADOW_GUARD_CLAMPED_SUN",
        "MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN",
        EnvVarKind::Feature,
        true,
        "TERRAIN-SHADOW-GUARD-1: the TERRAIN-side twin of SHADOW-SUNCLAMP-1, and the one that actually moves world pixels. shadow.hglsl's back-face guard (`float NdotL = max(dot(normal, lightDir), 0.0); if (NdotL < 0.05) return 1.0;`) and its SHADOW-ALT-SCALE-1 `sunAltSin = clamp(lightDir.z, 0, 1)` consumed the RAW authored terrainLightDir, while the shadow matrices were built from a sun clamped to MC2_SHADOW_MIN_SUN_ALT (15deg default). Under MC2_SHADOW_STATIC_BANDFIX_V2 (default ON) shadow.hglsl resets `normal = vec3(0,0,1)` immediately before the guard, so on terrain the test reduces to `terrainLightDir.z < 0.05`. mc2_10 authors LightDirPitch=180 -> sin(180deg) = 0 -> EVERY terrain/overlay/decal/chunk pixel early-outs fully lit while props and mechs keep their shadows through the unguarded static path -- world-scale, and the reason the mech-pixel-only SHADOW-SUNCLAMP-1 fix captured byte-identical. Implemented as a compile-time shader define (MC2_SHADOW_MIN_SUN_SIN) injected by mc2TerrainShadowSunDefine() at every terrain-family program prefix (main terrain variations, thin, surface, mask-solid, overlay+decal, and the LOD-chunk base+tess programs); shadow.hglsl defaults it to 0.0 so non-injecting programs (props/mechs) are byte-identical. Default-ON; =0 emits no define and reverts to the raw-sun guard. CORRECTED 2026-07-30 (TREE-CONSOLIDATE-1): this sentence used to end 'only mc2_10 changes', which contradicted the no-op rule stated in the same sentence and materially understated the blast radius of a DEFAULT-ON gate. FIVE shipping missions change, not one. Derived from the C++, not from docs: mclib/camera.cpp:2324-2329 builds the default (MC2_SUN_ELEVATION_FIX off) lightDirection as (cos(az), sin(az), sin(pitch)) and normalizes AFTER, so the unit vector's z is sin(p)/sqrt(1+sin(p)^2), and the clamp in shaders/include/shadow.hglsl mc2ShadowClampSunAltitude fires when that z < sin(15deg) = 0.258819 -- i.e. at an AUTHORED pitch below 15.54deg, not 15deg. Against the authored pitches (tools/terrain_daylight_harness kMissions, transcribed from the shipping mission camera blocks) that selects exactly mc2_10 (180deg -> z=0), mc2_19 (0deg -> z=0), mc2_05 and mc2_21 (270deg -> z=-0.7071), and mc2_23 (9deg -> z=0.1546). Nothing else is close: the next-lowest is mc2_14 at 20deg (z=0.3237). TWO DIFFERENT MECHANISMS, and conflating them is how the old text went wrong: on mc2_10/19/05/21 the guard itself early-outed (BANDFIX_V2 pins normal to +Z, so NdotL = max(z,0) = 0 < 0.05) and terrain had NO shadow term at all -- those four go from fully-lit to actually sampling the shadow map, which is where this gate does its most work. mc2_23 is NOT in that group: z=0.1546 is already >= 0.05, so its guard never early-outed; it changes only because the clamp also feeds SHADOW-ALT-SCALE-1's sunAltSin (0.1546 -> 0.2588, moving shadowFadeGains' smoothstep(0.21,0.57) off its lower edge) and the NdotL-derived depth bias. mc2_01 (50deg), mc2_03 (46deg), mc2_24 (75deg) remain byte-identical. NOTE THE DIRECTION on mc2_05/mc2_21: this makes terrain take shadows from an artificial 15deg sun on missions whose authored sun points straight DOWN -- worth grading, not assuming. The SHADING light dir (gos_terrain.frag:820 diffuse N-dot-L) is untouched -- brightness cannot move, only wrongly-discarded shadows return."
    },
    {
        "MC2_TUNE_SHADOW_MIN_SUN_ALT",
        "MC2_SHADOW_MIN_SUN_ALT",
        EnvVarKind::Trace,
        false,
        "SHADOW-SUN-ALT-CLAMP-1 tunable: minimum EFFECTIVE sun altitude in DEGREES used by the shadow projection matrices (mc2ClampShadowSunAltitude, gos_postprocess.cpp). Default 15; 0 or negative disables the clamp entirely. Azimuth is preserved. Applied at BOTH the static full-map matrix and the dynamic cascade matrices. Missions authoring a sun at/below the horizon (mc2_10 LightDirPitch=180 -> lightDirection.z = 0) otherwise smear absurdly elongated caster silhouettes across up-facing slopes ('black streak fans'). Suns already above the minimum are untouched (byte-identical). Since SHADOW-SUNCLAMP-1 this same clamp also governs the shadow_screen back-face guard (see MC2_SHADOW_GUARD_CLAMPED_SUN). Resolved once at process start."
    },
    {
        "MC2_TUNE_IMPOSTOR_DIST",
        "MC2_IMPOSTOR_DIST",
        EnvVarKind::Trace,
        false,
        "Static-prop / tree impostor LOD switch distance in world units (mclib/bdactor.cpp TreeAppearance::selectActiveLOD). Default 800. Hysteresis deadband is applied around it (_near = dist*0.92, _far = dist*1.08), so at the default the LOD flip band is [736, 864]. Lowering it pushes impostor cards nearer the camera (cheaper, more visible pop); raising it keeps full-geometry LODs further out. Resolved once at process start."
    },
    {
        "MC2_FEATURE_PROP_SHADOW_RECEIVE",
        "MC2_PROP_SHADOW_RECEIVE",
        EnvVarKind::Feature,
        false,
        "PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade shadow in shadow_screen.frag (building self-shadow + prop-on-prop; the cascades already carry the full prop caster set via the DynShadowCSM replay). Default-OFF; =1 enables. OFF = legacy static-map-only reception for the GBuffer1.a=0.25 prop class (pixel-identical). Shares the existing PCF ledger (MC2_SHADOW_CSM_SOFTNESS, MC2_SHADOW_OBJ_NORMAL_BIAS) + the MC2_SHADOW_MECH_SOFT terminator smoothstep; floor 0.4 min-combined with the static map (no double-darken)."
    },
    {
        "MC2_FEATURE_SHADOW_STATIC_BUILDINGS",
        "MC2_STATIC_PROP_BUILDING_SHADOW",
        EnvVarKind::Feature,
        false,
        "SHADOW-STATIC-BUILDINGS-2: replay ALL registered rigid-building recipes (full registry, visibility-independent) into the world-fixed static shadow map, once per mission at the static-map build. Trees excluded (population filter). Default-OFF; =1 enables, =2 enables+trace. Independent of MC2_SHADOW_ENABLE. Requires C-pre min-combine. Debt: destroyed buildings keep stale static shadow until mission reload."
    },
    {
        "MC2_FEATURE_SHADOW_DYNAMIC_PROP_CASTERS",
        "MC2_SHADOW_DYNAMIC_PROP_CASTERS",
        EnvVarKind::Feature,
        true,
        "SHADOW-DYNAMIC-PROP-CASTERS-1: feed the per-frame DYNAMIC sun-shadow caster pass from the full registry (registered props, visibility-independent) instead of the camera-visible s_typeRanges set, which only admitted props near the camera (so distant trees never cast into the now-correctly-camera-fit dynamic map). DEFAULT-ON; MC2_SHADOW_DYNAMIC_PROP_CASTERS=0 is the kill-switch (reverts to the legacy camera-visible flushShadow feed); =2 adds trace. Only takes effect when MC2_SHADOW_ENABLE is set (dynamic shadow pass). Buildings: excluded from this feed when MC2_STATIC_PROP_BUILDING_SHADOW is active (they cast via the world-fixed static map), else included so they still cast a dynamic shadow. Debt: no light-box cull yet (draws all map props every frame); HZB planned."
    },
    {
        "MC2_TUNE_STATIC_PROP_IBL_SH_SET",
        "MC2_STATIC_PROP_IBL_SH_SET",
        EnvVarKind::Trace,
        false,
        "V-IBL-STATIC-2: override the active SH coefficient set by name (e.g. 'default'). Optional dev/debug knob. Unset/empty/unknown -> registry-or-default fallback. Does not gate IBL itself (MC2_STATIC_PROP_IBL_SH remains the authoritative on/off gate)."
    },
    {
        "MC2_TUNE_STATIC_PROP_IBL_SH_STRENGTH",
        "MC2_STATIC_PROP_IBL_SH_STRENGTH",
        EnvVarKind::Trace,
        false,
        "V-IBL-STATIC-1: optional default-strength override for SH-L2 ambient (clamped 0..3). ImGui slider g_iblShStrength initial value. Only contributes when MC2_STATIC_PROP_IBL_SH is active; =0 on that gate uploads strength 0.0. Unset/empty -> code fallback 0.5, BUT data/visual_tuning.json normally overrides it (defaults 0.55; mc2_01/mc2_10/mc2_24 0.6) -- setting this env DISABLES that per-mission profile override, so an env value equal to the code fallback is NOT the same as leaving it unset. Resolved once at process start."
    },
    {
        "MC2_FEATURE_STATIC_PROP_IBL_NIGHT_SCALE",
        "MC2_STATIC_PROP_IBL_NIGHT_SCALE",
        EnvVarKind::Feature,
        true,
        "IBL-NIGHT-SCALE-1: night-scales the SH-IBL ambient probe on static props. The probe is projected from a DAY SKY (IblShCoeffs.h 'DaySkyHDRI063B_4K.exr'), there is one SH set for all missions and all times of day (kMissionShMap[] empty), and nothing scaled it by night -- so since the 2026-05-27 default-ON flip a day sky has lit every night mission at full strength (mc2_10 night: props +44%..+83%, one building face clipping to 255 on 2.49% of pixels; stock MC2 had no IBL at all). Curve: strength *= 1 - (1 - floor) * nightFactor. Value = the night floor, clamped 0..1; unset -> 0.25, chosen at the measured knee (clipping 2.49% -> 0.13% for a 13-18% wall/turret dim, where floor 0.0 -- equivalent to the blunt MC2_STATIC_PROP_IBL_SH=0 killswitch -- costs 29-46% and drives walls/turrets near-black). '1.0' -> multiplier exactly 1.0 at every nightFactor -> byte-identical to the pre-slice build (killswitch). '0.0' -> hard (1 - nightFactor). DAY IS BYTE-IDENTICAL BY ARITHMETIC: nightFactor is exactly 0.0f in daylight so the multiplier is exactly 1.0f. DEFAULT-ON. Resolved once at process start."
    },
    {
        "MC2_TUNE_STATIC_PROP_PBR_V1_STRENGTH",
        "MC2_STATIC_PROP_PBR_V1_STRENGTH",
        EnvVarKind::Trace,
        false,
        "V-MATERIAL-PBR-3: optional default-strength override for per-fragment Schlick-Fresnel specular (clamped 0..3). ImGui slider g_pbrV1Strength initial value. Only meaningful when MC2_STATIC_PROP_PBR_V1=1 (env var is the authoritative gate). Unset/empty -> default 1.0. Resolved once at process start."
    },
    {
        "MC2_DIAG_STATIC_PROP_PBR_V1_SUNFOUND",
        "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
        EnvVarKind::Trace,
        false,
        "V-MATERIAL-PBR-3-DIAG: diagnostic visualizer for the forwarded sunFound state. When ON together with MC2_STATIC_PROP_PBR_V1=1, static_prop.frag replaces the PBR result with cyan (sunFound=true) or magenta (sunFound=false), bypassing Schlick math. Diagnostic-only; never affects default-OFF behavior. Unset/'0' -> off."
    },
    {
        "MC2_CONE_SPILL_COUPLING",
        "MC2_CONE_SPILL_COUPLING",
        EnvVarKind::Trace,
        false,
        "CONE-SPILL-GRADE-1: isolates CONE-SPILL-COUPLING-1 (c1f98b45), which shipped UNGATED as a default-behaviour change riding MC2_LIGHT_CONE_NODES and is now DEFAULT-OFF on measured evidence (on mc2_10 it suppressed 7 of 9 building spotlight ground pools while moving the other 2 by only 7 world units; the 200u clamp never fired). '1' -> building/guard-tower spotlight ground pools are placed at the TRUE beam/terrain intersection found by marching the ray (mc2ConeGroundHit), the arbitrary 200.0f spill clamp is gone, and a beam that never meets ground within its own length or leaves the map emits NO pool. '0' -> restores the exact pre-c1f98b45 placement (apex + aim*spill, 200.0f clamp, z snapped to getTerrainElevation, no miss rejection). Exists so the change can be A/B graded on one attributable exe. Resolved once at process start.",
    },
    {
        "MC2_FEATURE_CONE_POOL_POLICY",
        "MC2_CONE_POOL_POLICY",
        EnvVarKind::Feature,
        true,
        "CONE-SPILL-POLICY-1: pool-placement policy for building/beacon spotlight ground pools on the DEFAULT path (MC2_CONE_SPILL_COUPLING OFF). '1' (DEFAULT) -> (a) a beam aimed UP (aim.z > 0.15; the mc2_23 beacons aim exactly (0,0,1)) emits NO ground pool -- it lights the sky, and the legacy snap-z placement planted a pool at the beacon's own base; (b) a downward/near-horizontal beam places its pool at the TRUE beam/terrain intersection (mc2ConeGroundHit) searched over an EXTENDED reach of MC2_CONE_POOL_REACH * coneLen -- the authored cone length is a visual extent, not a physical reach (requiring a hit within it deleted 7 of 9 mc2_10 pools, CONE-SPILL-GRADE-1) -- and a miss within the extended reach suppresses the pool. '0' -> legacy unconditional far-end + snap-z placement with the 200u clamp (killswitch, exact pre-slice behaviour). Constants validated offline by scripts/predict_cone_pools.py. Resolved once at process start.",
    },
    {
        "MC2_TUNE_CONE_POOL_REACH",
        "MC2_CONE_POOL_REACH",
        EnvVarKind::Trace,
        false,
        "CONE-SPILL-POLICY-1: reach multiplier k for the extended beam/terrain intersection search (reach = k * authored coneLen * MC2_LIGHT_CONE_SPILL). Default 4, clamped [1,16]. k=4 keeps every non-upward mc2_10 tower fixture's pool (their near-horizontal beams drop ~15u over the authored length from ~17u apexes, so they need ~1.2-4x the authored length to meet ground) while still rejecting beams that genuinely land nowhere. Only read when MC2_CONE_POOL_POLICY=1 (the default policy). Tuned against scripts/predict_cone_pools.py.",
    },
    {
        "MC2_FULLBAKE_DIAG",
        "MC2_FULLBAKE_DIAG",
        EnvVarKind::Trace,
        false,
        "SPOTLIGHT-DIRECTION-1 measurement instrument. '1' -> mclib/bdactor.cpp emits a running '[FULLBAKE_DIAG v1] calls=.. gpuEligible=.. else_hier=.. else_FULLBAKE=.. dualemit_full=.. ptspot_consumed=.. ptspot_zerodir=..' tally to stderr every 600 BldgAppearance::update() branch executions. else_FULLBAKE is the non-gpuEligible fallback that runs the full TransformMultiShape() bake -- the ONLY path that reaches the msl.cpp TG_LIGHT_POINT/SPOT lighting block, i.e. the only consumer of TG_Light::direction. ptspot_consumed/ptspot_zerodir are incremented at that consumer (mclib/msl.cpp) and split active POINT/SPOT lights by whether ->direction is still the init() default (0,0,0). Exists to separate latent-from-live for the MC2_SPOTLIGHT_DIRECTION_FIX defect and to prove an A/B capture arm actually reached the consumer. The msl.cpp counter sits in a hot inner loop (4.3 M trips in a 15 s mc2_10 run) so it is gated, not unconditional. Unset/'0' (DEFAULT) -> one static bool test, no counting, no output, quiet-build clean. Resolved once at process start.",
    },
    {
        "MC2_SPOTLIGHT_DIRECTION_FIX",
        "MC2_SPOTLIGHT_DIRECTION_FIX",
        EnvVarKind::Trace,
        false,
        "SPOTLIGHT-DIRECTION-1: fixes two coordinate-space defects at the building/guard-tower spotlight registration site (mclib/bdactor.cpp). (a) TG_Light::direction was never set, so it kept the init() default (0,0,0). direction is a POSITION for POINT/SPOT lights -- tgl.h:213 'This is the light spot center for POINT and SPOT' -- and mclib/msl.cpp subtracts it from a world position to derive the per-vertex light vector, so every CPU-baked vertex was lit as though the lamp sat at the WORLD ORIGIN instead of on its own building. (b) SetLightToWorld() was built from the RAW MC2-space childPos rather than the swizzled Stuff-space position every other consumer expects. '1' -> both fields use the MC2->Stuff position swizzle (-x, z, y), matching the sibling terrain-light site in the same file. Unset/'0' (DEFAULT) -> historical behaviour, byte-identical. NOT latent: measured on the deployed build via FULLBAKE_DIAG, mc2_10 (night) consumed 4,310,486 active POINT/SPOT lights in the CPU bake over 15 s, 3,995,686 of them (92.7%) with direction still (0,0,0); mc2_01/mc2_03/mc2_24 measured 0 because these lights are night-gated (eye->isNight). Default-OFF because the correction changes default night appearance on mc2_10. Single reader: mclib/cone_glow.cpp spot_direction_fix_enabled(). Resolved once at process start.",
    },
    {
        "MC2_LIGHT_CONE_NODES",
        "MC2_LIGHT_CONE_NODES",
        EnvVarKind::Trace,
        true,
        "LIGHT-CONE-MODELS-1: DEFAULT-ON. Places building/guard-tower spotlight ground pools using the fixture's authored cone NODE geometry (apex + aim + length read off the shape) instead of the older heuristic that centred each pool on the light node's own origin. Unset/anything-but-'0' (DEFAULT) -> node-derived pool centres. '0' -> restores the pre-LIGHT-CONE-MODELS-1 heuristic/origin-based centres (batch-6 behaviour). Registered retroactively by NIGHT-TAGS-GREEN-1: this shipped default-ON but UNREGISTERED and UNDOCUMENTED, which is how CONE-SPILL-COUPLING-1 (c1f98b45) was later able to ride it into the default render ungated. Single reader: mclib/cone_glow.cpp nodes_enabled(). Resolved once at process start.",
    },
    {
        "MC2_STATIC_PROP_NIGHT_TAGS",
        "MC2_STATIC_PROP_NIGHT_TAGS",
        EnvVarKind::Trace,
        false,
        "EDGE-LIGHT-EMISSION-1: restores the stock hot-colour NIGHT vertex tags on the GPU static-prop path. Runway/taxiway edge lights and beacons paint their post base 0xff00ff00 ('Hot Green -- Building Base Lights -- ONLY at NIGHT') and declare their emission colour per fixture in the .ini //Night Lights block; the legacy CPU path honours these via eye->getIsNight()/getNightFactor() (mclib/tgl.cpp:1901) but the GPU path passed false/0.0 hardcoded into get_base_light(), so every night-only branch took its daytime arm and the lights never came on. Unset/'0' -> u_isNight=0, u_nightFactor=0, u_nightEmissiveScale=0 -> byte-identical. '1' -> stock emission restored. '2' -> verify-by-exaggeration (4x emission delta). Blast radius: relights every night-tagged static prop, including building windows -- stock-correct but broad, hence default-OFF. Resolved once at process start."
    },
    {
        "MC2_STATIC_PROP_FIXTURE_GLOW",
        "MC2_STATIC_PROP_FIXTURE_GLOW",
        EnvVarKind::Trace,
        false,
        "FIXTURE-GLOW-1: DEFAULT-OFF on measured evidence. Bounded, distance-attenuated ADDITIVE glow that makes the runway/taxiway edge lights and beacons read as LIGHT SOURCES at play distance. Successor to MC2_STATIC_PROP_FIXTURE_LIGHTS, which provably cannot do this: get_base_light() clamps at 1.0 (scales 1/4/8 render byte-identical frames) and whatever survives the clamp is then MULTIPLIED by the fixture's dark lens albedo at static_prop.frag `c = tex_color * litRgb` -- a light source is not expressible as a multiplier on a dark texture. This gate adds a separate term in the FRAGMENT shader after the albedo modulate and after fog, which is how it escapes both. Reuses the per-instance fixture flag in the per-type SSBO padding lane (hotPinkRGB.w) from BEACON-FIXTURE-ID-1, so no struct/binding change. Tint is the fixture's own night EMISSION DELTA, which is exactly vec3(0) on the fixture's non-emissive post/base geometry, so the glow lands on the LENS only and each fixture gets its correct colour for free (white runwaylight, red/green/blue beacons). Bounding: NO screen-size clamp, NO billboard, NO geometry or size term of any kind -- the fixture keeps shrinking with distance -- and the magnitude is multiplied by 1-smoothstep(2600,4600,clip.w), which is ARITHMETICALLY ZERO at and beyond view depth 4600. Overview altitude is ~6000, i.e. 1400 units past identically-zero, so the RUNWAY-MARKER-LIGHTS-1 constant-size-dash regression is excluded by the formula, not by a measurement. The near/far window is compile-time, deliberately NOT env-tunable, so the bound cannot be configured away. Also multiplied by the night factor -> identically 0.0 in daylight. Unset/'0' (DEFAULT) -> byte-identical to pre-FIXTURE-GLOW-1; numeric -> strength, clamped [0,8]; 1.0 is the calibrated baseline and 3-4 the 'clearly a light' range. Default is OFF because at strength 1.0 the visible result on the only observable fixtures (mc2_10's green beacons) is a modest pale-ing of an already-green cone rather than a light coming on: the lens already renders at channel max, so the only LDR headroom left is the other channels, i.e. any added glow necessarily desaturates toward white -- an aesthetic call, from a user who already rejected one fixture-lighting attempt as 'atrociously hideous'. The red and blue fixture types were never observed glowing on screen. Needs a strength picked by eye before it ships ON. Resolved once at process start."
    },
    {
        "MC2_STATIC_PROP_FIXTURE_LIGHTS",
        "MC2_STATIC_PROP_FIXTURE_LIGHTS",
        EnvVarKind::Trace,
        true,
        "BEACON-FIXTURE-ID-1: DEFAULT-ON. Turns the runway/taxiway edge lights and beacons ON at night, scoped by FIXTURE IDENTITY rather than vertex colour. Vertex colour cannot scope this: 738 of 2947 stock .ase carry the Hot Green tag and only 8 are fixtures -- the tag's stock name is literally 'Building Base Lights', which the fixtures borrow, so MC2_STATIC_PROP_NIGHT_TAGS relights ~730 buildings to buy 8 fixtures and must stay default-OFF. This gate instead classifies each registered TG_TypeShape at finalizeGeometry via isEmissiveFixtureType(): a fixture declares the SAME saturated primary for all three Hot*RGB slots, buildings declare independent desaturated greys/olives. Validated against all 773 stock data/tgl .ini -> exactly 4 match (runwaylight, red/green/bluebeaconlight, backing ~24 .ase variants), zero buildings. Both halves of the test matter: 'saturated primary' alone leaks 503/773 because bdactor.cpp:813-819 defaults an undeclared slot to the magic tag itself. The flag rides the existing per-type SSBO vec4 padding (hotPinkRGB.w), so no struct/binding change. Unset/'1' (DEFAULT) -> fixtures emit at stock; '0' -> byte-identical to pre-fix; numeric >1 -> verify-by-exaggeration, clamped [0,16]. Non-fixture props take scale 0.0 -> base_light == base_day -> byte-identical, so building windows are provably untouched. Introduces NO screen-size clamp and NO billboard: emission rides real fixture geometry and shrinks with distance, avoiding the RUNWAY-MARKER-LIGHTS-1 overview-altitude streak regression. Resolved once at process start."
    },
    {
        "MC2_TUNE_STATIC_PROP_NIGHT_TAGS_SCALE",
        "MC2_STATIC_PROP_NIGHT_TAGS_SCALE",
        EnvVarKind::Trace,
        false,
        "EDGE-LIGHT-EMISSION-1: scales the NIGHT EMISSION DELTA only (night base_light minus day base_light), never the underlying lighting maths, so 0.0 is exactly the pre-fix render and 1.0 is exact stock. Only meaningful when MC2_STATIC_PROP_NIGHT_TAGS is nonzero. Unset -> 1.0 (or 4.0 under mode 2). Clamped [0,16]. Resolved once at process start."
    },
    {
        "MC2_FEATURE_SHADOW_STATIC_BUILDINGS_REBAKE",
        "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE",
        EnvVarKind::Feature,
        true,
        "STATIC-BLDG-SHADOW-REBAKE-1: re-arm the one-shot world-fixed static-shadow latch (gos_ResetStaticLightMatrix) when a registered building is destroyed -- primarily BldgAppearance::setObjStatus transitioning to OBJECT_STATUS_DESTROYED (the real in-game combat-destroy event), with BldgAppearance::destroy (mission-end/pool-recycle) as cheap insurance -- so txmmgr's static-shadow build block (terrain + buildings) replays next frame WITHOUT the destroyed building instead of keeping its stale baked shadow until mission reload (docs/known_issues.md). Only fires when (a) the destroyed building actually held a static-prop registration and (b) MC2_STATIC_PROP_BUILDING_SHADOW is active -- inert (and costs nothing) on missions that never opted into the parent feature, since the unconditional terrain-only static build must not be forced to redundantly replay. Default ON (independent kill-switch for the parent feature); =0 disables the rebake trigger only; =2 adds a one-line [SHADOW_STATIC_BLDG_REBAKE v1] trace per destroy."
    },
    {
        "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME",
        "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME",
        EnvVarKind::Trace,
        false,
        "STATIC-BLDG-SHADOW-REBAKE-1: deterministic repro hook (test-only, no production callers). At the given frame, inflict lethal damage on the first live building via the real Building::setDamage gameplay API (same effect as a weapon hit reaching the damage threshold), driving it through setObjStatus(OBJECT_STATUS_DESTROYED) and the shadow-rebake trigger above, for before/after capture proofs. Unset = no-op."
    },
    {
        "MC2_TUNE_FAR_BACKDROP_LIT_NEAR",
        "MC2_FAR_BACKDROP_LIT_NEAR",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: view-depth (clip.w, world units) where the night-shade fade begins. Only meaningful when MC2_FAR_BACKDROP_LIT=1. Unset/empty -> default 700 (~0.9 * MC2_IMPOSTOR_DIST). Resolved once at process start."
    },
    {
        "MC2_TUNE_FAR_BACKDROP_LIT_FAR",
        "MC2_FAR_BACKDROP_LIT_FAR",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: view-depth (clip.w, world units) where the night-shade fade completes (full substitution). Only meaningful when MC2_FAR_BACKDROP_LIT=1. Unset/empty -> default 960 (~1.2 * MC2_IMPOSTOR_DIST). Resolved once at process start."
    },
    {
        "MC2_TUNE_FAR_BACKDROP_LIT_STRENGTH",
        "MC2_FAR_BACKDROP_LIT_STRENGTH",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: maximum substitution amount scaling the distance fade (1.0 = full replace of the stale seed with night ambient; >1 exaggerates for verify-by-exaggeration). Only meaningful when MC2_FAR_BACKDROP_LIT=1. Unset/empty -> default 1.0. Resolved once at process start."
    },
    {
        "MC2_DIAG_FAR_BACKDROP_LIT_TINT",
        "MC2_FAR_BACKDROP_LIT_TINT",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: debug/exaggeration additive tint 'r,g,b' added to the substituted night shade so the affected far/impostor pixels are visually obvious (e.g. '1,0,0' paints substituted far geometry red to prove the path fires on the ridge/impostor pixels). Only meaningful when MC2_FAR_BACKDROP_LIT=1. Unset/empty -> 0,0,0 (no tint). Resolved once at process start."
    },
    {
        "MC2_TUNE_NIGHT_FOG_DIM_SCALE",
        "MC2_NIGHT_FOG_DIM_SCALE",
        EnvVarKind::Trace,
        false,
        "BACKDROP-BLIT-ATTRIB-1: multiplier applied per-channel to the edge-fog + OOB fog color when MC2_NIGHT_FOG_DIM=1. 1.0 = no change; 0.35 (default) drops the bright-grey {0.80,0.81,0.83} cloud bank to ~{0.28,0.28,0.29}, matching the measured mc2_10 night HDRI-sky value in the same scene FBO; 0.0 = black fog (verify-by-exaggeration lower bound). Only meaningful when MC2_NIGHT_FOG_DIM=1. Unset/empty -> default 0.35. Resolved once at process start."
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP",
        "MC2_DEBUG_STATE_DUMP",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-1: write a JSON render-state snapshot to debug_state/latest_render_state.json every 300 frames (and at frame 1). Snapshot includes feature gates, RenderSnapshot counters, EngineView state, and StaticPropOpaque visual globals. Read-only; no gameplay or renderer behavior changes. Default-OFF; =1 enables. Output dir override: MC2_DEBUG_STATE_DUMP_DIR. See docs/debug_state_dump.md."
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_DIR",
        "MC2_DEBUG_STATE_DUMP_DIR",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-1: override output directory for MC2_DEBUG_STATE_DUMP. Defaults to debug_state/ relative to working directory. No effect when MC2_DEBUG_STATE_DUMP is not set."
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_HISTORY",
        "MC2_DEBUG_STATE_DUMP_HISTORY",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-2: enable rolling 8-slot history ring alongside latest_render_state.json. When set with MC2_DEBUG_STATE_DUMP=1, each write also produces history_0.json..history_7.json in the same output directory (oldest slot overwritten in order). Bounded to 8 files; no unbounded growth. Default-OFF; =1 enables."
    },
    {
        "MC2_DIAG_TERRAIN_DEBUG_MODE",
        "MC2_TERRAIN_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DEBUG-VIEWS-1: terrain fragment-shader debug-mode selector for the tessellated terrain path (gos_terrain.frag tessDebug.x). Default unset = mode 0 (off, byte-identical to legacy output). Visual modes: 1=DepthComparison, 2=RawColormap, 3=BlurredColormap, 4=MaterialWeights (R=rock,G=grass,B=dirt), 5=NormalLighting, 6=ShadowFactor, 7=CloudShadow. Diagnostics: 8=CementDiag, 9=ThinRecordDiag, -1=TessAliveProbe. When set, env value overrides the runtime mode (gos_*TerrainDebugMode C-API, Surface Debug Mode picker in GraphicsOptionsWindow, and the Terrain Pass inspector mini-control all read it). Diagnostic-only; no gameplay, correctness, or default visual effect. Full mode list table: GuiRuntime/GraphicsOptionsWindow.cpp kTerrainModes."
    },
    {
        "MC2_TUNE_TERRAIN_HEIGHT_RESAMPLE_FACTOR",
        "MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR",
        EnvVarKind::Trace,
        false,
        "TERRAIN-RESAMPLE-1: CPU bilinear resample factor for the per-mission terrain height texture used by TERRAIN-NORMALS-FROM-HEIGHT-1. Accepted values 1, 2, 4 (anything else clamps to 1). Default 1 (byte-equivalent to pre-slice). Render texture side becomes (sourceSide-1)*factor + 1, with source samples preserved EXACTLY at corner positions (factor multiples). Bilinear interpolation between source taps fills intermediate render samples. Resample is read at every gos_uploadTerrainHeightTex() call (i.e. per mission load); toggling mid-mission does not re-upload. Only affects the height-derived normal path: gameplay height (Terrain::getTerrainElevation) is unchanged; no displacement, no geometry move. Memory: 4× factor on a 120² source = ~890 KB; bounded by source-grid * 16. Inspector shows source/render/factor."
    },
    // TERRAIN-CHUNK-POM-1: real view-vector parallax occlusion mapping on the
    // LIVE LOD-chunk terrain path (gos_terrain_lod_chunk.cpp uploads,
    // terrain_lod_chunk.frag chunkParallaxView). Gate OFF preserves the legacy
    // faux-view-vector chunkParallax output VERBATIM (supervisor ruling:
    // byte-identity INCLUDES the faux shear — pomParams is NOT zeroed when OFF).
    {
        "MC2_FEATURE_TERRAIN_POM",
        "MC2_TERRAIN_POM",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CHUNK-POM-1: real per-fragment tangent-space view-vector POM on the LOD-chunk terrain (rock/grass detail layers). Default-OFF = legacy faux constant viewDirTS(0.15,0.85,0.15) march runs VERBATIM (byte-identical incl. the faux shear; pomParams upload unchanged). =1 swaps in the real camera vector (gos_GetTerrainCameraPos, Stuff/MLR frame -> MC2 world (-x,z,y)) with a world-distance fade (MC2_TERRAIN_POM_NEAR/_FAR), triplanar-cliff slope exclusion (|Nz| 0.85->0.55 band), cement/concrete exclusion (w.w), and LOD0/1-only belt. Shading-only parallax: NEVER writes gl_FragDepth (AMD early-Z landmine); shadows sample the true surface point. Read once per process (static). Debug viz: MC2_TERRAIN_LOD_CHUNK_DIAG bit 4096 = |pomOff| heat, bit 8192 = view-vector swizzle oracle (RGB = MC2-world frag->camera dir)."
    },
    {
        "MC2_TUNE_TERRAIN_POM_SCALE",
        "MC2_TERRAIN_POM_SCALE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: POM march scale override (float > 0). Default unset = gos_GetTerrainPOMScale() (0.02). Only consumed when MC2_TERRAIN_POM=1; gate-OFF upload is the stock line regardless. Expect retune vs legacy: the faux up=0.85 divided the effective offset, a real grazing view yields larger P."
    },
    {
        "MC2_TUNE_TERRAIN_POM_STEPS",
        "MC2_TERRAIN_POM_STEPS",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: max POM march layers, accepted 4..16 (else default 16; hard compile-constant cap 16). min layers = min(value,8). More layers at grazing view (oracle orientation, mix(max,min,up)). Only consumed when MC2_TERRAIN_POM=1."
    },
    {
        "MC2_TUNE_TERRAIN_POM_NEAR",
        "MC2_TERRAIN_POM_NEAR",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: POM distance-fade band start in world units (default 1500; 1 tile = 384 wu). Camera ground distance + 0.7*altitude boost (dead-frag LOD semantics); full POM strength inside NEAR. NaN/negative -> default. Only consumed when MC2_TERRAIN_POM=1."
    },
    {
        "MC2_TUNE_TERRAIN_POM_FAR",
        "MC2_TERRAIN_POM_FAR",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: POM distance-fade band end in world units (default 3500). Beyond FAR the march is skipped entirely (strength 0 early-out). Clamped to > NEAR (NEAR+1 floor). NaN/non-positive -> default. Only consumed when MC2_TERRAIN_POM=1."
    },
    // NIGHT-LIGHT-POOLS-1: the LOD-chunk terrain consumes the per-frame terrain
    // light pool (gos_terrain_lighting PackAndDispatch light SSBO) per-fragment,
    // restoring stock-style night up-lighting on ground + cement pads.
    {
        "MC2_FEATURE_TERRAIN_LIGHT_POOLS",
        "MC2_TERRAIN_LIGHT_POOLS",
        EnvVarKind::Feature,
        true,
        "NIGHT-LIGHT-POOLS-1: per-fragment terrain light-pool loop on the LIVE LOD-chunk terrain path (terrain_lod_chunk.frag). Binds the SAME GpuTerrainLight SSBO the legacy vertex-lighting compute consumes (lights [2..N-1] -- building SpotLight_ nodes, light props, mech/vehicle/turret/weapon lights from Camera::terrainLights) at binding 28 and adds color*falloff (TG_Light::GetFalloff math) ADDITIVELY AFTER the shadow multiply, so lamps light shadowed ground and cement pads (rides on top of CEMENT-NIGHT-LIGHT-1's u_missionFlatLight darken). DEFAULT ON; =0 force-uploads u_numPoolLights=0 -> loop never entered -> byte-identical. Also inert whenever MC2_TERRAIN_LIGHTING_GPU=0 (no producer). Driver caps the loop at 256 lights."
    },
    // PERF-HARNESS-TRUTH-1: registered while measuring its cost. It was
    // allowlisted for smoke but neither registered nor documented, so the gate
    // holding ~6 reported visual items had no discoverable contract.
    {
        "MC2_FEATURE_LIGHT_REALISM",
        "MC2_LIGHT_REALISM",
        EnvVarKind::Feature,
        true,
        "LIGHT-REALISM-1: night light-pool + cone-glow realism reshape. Widens each pool light's ground falloff (MC2_LIGHT_REALISM_POOL_SPILL, default 2.2 = user-approved variant B) with a soft core (MC2_LIGHT_REALISM_POOL_CORE, default 0.0) and softens the spotlight cone billboard (MC2_LIGHT_REALISM_CONE_OPACITY, default 0.40 = user-approved variant B). Covers cone transparency, beacon ground discs and pool continuity. DEFAULT FLIPPED ON by CONE-SPILL-POLICY-1 (2026-08-01): variant B is user-approved and the measured cost with the default light grid is zero. '=0' reverts to the legacy hard-edged pools/cones (byte-identical to the pre-LIGHT-REALISM-1 render). PERF (PERF-HARNESS-TRUTH-1, mc2_10 busy night pose, 4K): with MC2_LIGHT_GRID=1 (default) it is FREE -- p50 6.405ms ON vs 6.454ms OFF, inside run-to-run noise -- because the grid holds the per-fragment loop to avg_cell 1.70 even though the reshape triples grid stamping (1962 -> 6945). With MC2_LIGHT_GRID=0 the same gate costs +31% (8.732ms). The grid is what makes it affordable."
    },
    // PERF-HARNESS-TRUTH-1: shipped unregistered, undocumented AND absent from
    // the run_smoke.py passthrough allowlist, so it silently no-opped through
    // the standard harness -- a lighting investigation could set it, see the
    // ordinary lit render, and conclude the debug view showed nothing wrong.
    {
        "MC2_FEATURE_LIGHTING_DEBUG_VIEW",
        "MC2_LIGHTING_DEBUG_VIEW",
        EnvVarKind::Feature,   // not Trace: it REPLACES the lit render output
        false,
        "DEBUG-VIEW-SWEEP-1: replace lit shading with a diagnostic channel across terrain, static props and mechs. String-valued: off|albedo|normal|sun|ambient|shadow|final|overbright|lightcount|lightindex (unknown value = safe fallback to the normal render). Resolved ONCE per process (no setenv path); the `debugview N` command channel overrides it at runtime via gos_SetGlobalDebugView(). Default OFF (unset = -1 = no override). Diagnostic only -- never ship a build relying on it."
    },
    // LIGHT-POOL-CAP-1 (5213d257) shipped this override unregistered and
    // undocumented -- registered here by LIGHT-CLUSTER-PERF-1's meta review.
    {
        "MC2_TUNE_LIGHT_POOL_MAX",
        "MC2_LIGHT_POOL_MAX",
        EnvVarKind::Trace,
        false,
        "LIGHT-POOL-CAP-1: per-mission slot count of the terrain light pool (gos_terrain_lighting.cpp mission_init). Default 256, raised from the callers' 64 because mc2_10 genuinely registers 142 fixtures and the pack loop silently dropped everything past the cap -- so whether a lamp lit the ground depended on its ORDER in Camera::terrainLights, not on anything the player can see. The pool is an unbounded SSBO, so there is no uniform-block limit; the only cost is the consumer loop, which LIGHT-CLUSTER-PERF-1's MC2_LIGHT_GRID removes. Accepts [1,4096]; out-of-range/unparseable -> default. Watch [LIGHT_POOL_CAP v1] STARVED lines for scenes that want more."
    },
    // LIGHT-CLUSTER-PERF-1: recover the frame time LIGHT-POOL-CAP-1 spent when it
    // raised the pool cap 64 -> 256 (the fix was correct -- mc2_10 wants 142
    // fixtures -- but the consumer loop is per-fragment, so cost scaled with the
    // whole pool).
    {
        "MC2_FEATURE_LIGHT_GRID",
        "MC2_LIGHT_GRID",
        EnvVarKind::Feature,
        true,
        "LIGHT-CLUSTER-PERF-1: world-space XY light GRID over the per-frame terrain light pool. The producer (gos_terrain_lighting.cpp BuildAndUploadLightGrid) bins each packed light into every grid cell its ground circle (radius = UNSCALED farDistance) overlaps, and uploads a per-cell {firstIndex,count} header (binding 29) + cell-major index list (binding 30). terrain_lod_chunk.frag maps v_worldPos.xy to one cell and loops ONLY that cell's lights instead of all u_numPoolLights, collapsing a 142-light mc2_10 night pose to a handful of lights per fragment. EXACT, not an approximation: a light can only contribute inside its own circle, and the shader's mover-radius shrink only reduces reach, so the per-cell list is a conservative superset of the contributors -- same lights, same pixels. DEFAULT ON; =0 uploads u_lightGridDim=0 -> frag takes the exact linear full-pool loop (killswitch). Also inert when the pool holds fewer than 8 lights (binning costs more than it saves) or MC2_TERRAIN_LIGHT_POOLS=0."
    },
    {
        "MC2_TUNE_LIGHT_GRID_DIM",
        "MC2_LIGHT_GRID_DIM",
        EnvVarKind::Trace,
        false,
        "LIGHT-CLUSTER-PERF-1 knob: cells per axis for the light grid (grid is DIM x DIM over the XY bbox of this frame's packed lights). Default 64; clamped to [2,256], out-of-range/unparseable -> default. Larger = shorter per-cell lists but more header upload; purely a perf knob, output is identical at every value. Only consumed when MC2_LIGHT_GRID is on."
    },
    // NIGHT-LIGHT-POOLS-3: overlapping pool sums are tone-compressed instead of
    // hard-clamped (5-mech group pools no longer flat-top a saturated blob).
    {
        "MC2_FEATURE_LIGHT_POOL_COMPRESS",
        "MC2_LIGHT_POOL_COMPRESS",
        EnvVarKind::Feature,
        true,
        "NIGHT-LIGHT-POOLS-3: tone-compress the accumulated terrain light-pool sum (chunk + overlay frag): 1-exp(-1.6*sum) instead of min(sum,1). Single light plateaus ~0.80, stacked lights approach 1.0 asymptotically (no blinding saturated union blob over mech groups / base light clusters). DEFAULT ON; =0 restores the exact legacy hard clamp."
    },
    // RUNWAY-EDGE-BLEND-1 (MASTER-FAIL #6): feather solid cement pad/runway edges
    // that abut bare terrain on the indirect terrain path (gos_terrain.frag).
    {
        "MC2_FEATURE_RUNWAY_EDGE_BLEND",
        "MC2_RUNWAY_EDGE_BLEND",
        EnvVarKind::Feature,
        false,
        "RUNWAY-EDGE-BLEND-1 (MASTER-FAIL #6), REVISION 2 = DILATION: cure the hard full-tile stairstep cliff where a SOLID cement pad/runway tile abuts BARE terrain with no authored transition art. REV-1 (erosion; ramped cement->terrain INSIDE the pad tile over its outer half tile) was USER-REJECTED -- it dissolved the apron edge into a blurry rim; user verdict on the mc2_01 day apron A/B: 'gate on is worse. the answer is usually MORE concrete, not less.' Superseded by 8e59de64, reach widened 0.5->1.0 tile by 3c6aad71. SHIPPED MECHANISM: PopulateRecipeCementWords (gos_terrain_indirect.cpp) packs a 4-neighbour open-border mask into cementWord bits 19:16 for solid cement boundary tiles (neighbours carrying ANY valid cement word -- solid OR transition -- count as cement, so authored transition edges are left to the legacy blend). terrain_lod_chunk.frag (the LIVE path) then works on the BARE-TERRAIN tile: solid cement keeps crisp FULL replacement to its own boundary (no erosion), and where a neighbouring solid cement tile's mask flags this side open, that cement is blended over the terrain at full strength at the shared edge falling to zero ONE tile out (alpha exactly 1.0 at the seam, so it is continuous with the crisp tile). Net: the concrete footprint GROWS with a soft outer margin instead of shrinking. gos_terrain.frag (indirect path) had the rev-1 erosion REMOVED and is plain full replacement as in stock -- it is per-quad off a thin record with no neighbour access, so it cannot host the dilation. DEFAULT OFF: no mask bits are written, so no neighbour is ever flagged open and the dilate block is a no-op == byte-identical stock. Baked once per mission load; zero per-frame cost."
    },
    // MC2-03-CEMENT-PINK-1: cement catalog atlas post-processing (build-time, CPU).
    // CEMENT-INTRACELL-1 (ARM C): make the concrete arms obey the user's own
    // scope ruling by detecting the BAKED ROAD ART inside solid cement cells.
    {
        "MC2_FEATURE_CEMENT_ROAD_ART_DETECT",
        "MC2_CEMENT_ROAD_ART_DETECT",
        EnvVarKind::Feature,
        false,
        "CEMENT-INTRACELL-1 ARM C: detect the BAKED ASPHALT ROAD TILESET authored inside SOLID cement catalog cells, and exclude those cells from every concrete treatment (ARM A tone, ARM B canon, and the canonical-cell choice). WHAT WAS MEASURED, and why this arm exists at all: CEMENT-ATLAS-CELL-2 found a large dark region in 78 of 110 mc2_24 solid cells and recorded it as \"authored material (joints, tarmac, shadowed detail)\". Dumping and RENDERING the atlas (MC2_CEMENT_ATLAS_DUMP) shows what it actually is -- straights, curves, T-junctions, crossroads with crosswalk stripes, end caps and a multi-lane highway-with-guardrail piece: a road tileset painted onto a concrete background. The prior marking test could not see it because it tested the BRIGHT side only (L - bandL > 60) and asphalt is DARK. THE REFRAME THIS FORCES: measured with the road texels excluded, the concrete BACKGROUND is already one colour on every map -- mc2_24 144.7..149.5 (spread 4.7, std 0.89), mc2_05 3.2, mc2_03 2.4, mc2_10 2.0; mc2_23 and mc2_01 show a 43-luma span driven entirely by two and one cells respectively, and those cells are wide highway pieces, not concrete. There is no cross-cell concrete tone problem. What makes concrete \"magically become a different color\" inside a cell is a road: background L~147 against asphalt L~82, a ~65-luma hard-edged step. That is precisely the class the user scoped OUT (\"obviously runways, roads, other overlays etc. are their own layer and are currently handled by the shader\") -- true for the &Overlays class, NOT true for this one, which is baked into the terrain texture catalog, so TileIsRoadArt() reports \"solid cement, not road\" and every concrete lever treats a road as concrete. WHAT ARM C PREVENTS, measured offline on the six atlas dumps: ARM B repoints 90-100% of the road-carrying cells at its canonical cell, and on 5 of the 6 maps that canonical cell is ITSELF a road tile (mc2_24 canon road fraction 0.312) -- so ARM B erases every junction, curve and end cap AND stamps one straight road segment across every plain concrete tile on the map. MECHANISM: CementCellComputeStats splits each cell against its own [P60,P95] upper mode (the concrete background) rather than the whole-cell band mean, which on a road-heavy cell is dragged down by the road and hides it; a cell with >5% of texels more than 40 luma below that background is flagged road=1. DEFAULT OFF. ON with both other arms OFF is a PIXEL NO-OP -- no write to atlasBuf and no write to g_cementLayerIndexBySlot -- it only changes which cells ARM A / ARM B are allowed to touch. The [CEMENT_CONCRETE v1] census reports road_frac, bg_lum and the family bg_span regardless of the gate, so the evidence is reproducible with nothing enabled. Once per mission load; zero per-frame cost."
    },
    // CEMENT-PATCHY-CAUSE-1: the concrete patchwork is a TWO-SHADER SEAM, not
    // an atlas-tone problem. Attribution receipt in .claude/CEMENT-PATCHY-CAUSE-1-STATUS.md.
    {
        "MC2_FEATURE_OVERLAY_CEMENT_SHADOW_MATCH",
        "MC2_OVERLAY_CEMENT_SHADOW_MATCH",
        EnvVarKind::Feature,
        false,
        "CEMENT-PATCHY-CAUSE-1: make terrain_overlay.frag apply the SAME concrete lighting exemption terrain_lod_chunk.frag already applies, so the overlay/decal layer and the cement terrain stop disagreeing across the seam where a single paved surface is split between them. WHAT WAS MEASURED (RenderDoc, pre-composite HDR scene FBO resid 231, NOT the backbuffer): at the mc2_23 runway_ground pose, pixel (2000,1700) -- the lighter concrete -- is written by eid 3678, a 48-vertex opaque depth-writing glDrawArrays whose pixel shader (Shader 212) disassembles to shaders/terrain_overlay.frag (SSBO block TerrainCementBufOverlay/cementWordsPG is the fingerprint); pixel (2200,1700) -- the darker concrete 200px away, across one hard line -- is written by eid 465, VS/PS Shader 226/227, the terrain LOD-chunk draw, and eid 3678 is depth-REJECTED there. The step is 1 pixel wide with no transition: L=79.5 -> L=70.4. This is why eleven lanes of cement-atlas work could not move it: the defect is not in the atlas, and half of the surface is not even drawn by the terrain shader. THE DISAGREEMENT: terrain_lod_chunk.frag drops the STATIC (baked) shadow on concrete -- shadow = mix(shadow, dynS, pureConcrete) -- and forces normalLight to 1.0; terrain_overlay.frag multiplies by staticShadow * dynamicShadow unconditionally, with no cement exemption. Where the layers abut, the static-shadow term is present on one side of a polygon edge and absent on the other, which reads as an outline drawn through flat concrete. MECHANISM: an overlay fragment whose OWN 128wu tile carries a VALID cement word (bit31 -- the same word the chunk frag keys cementHit off, read from the SSBO already bound at binding 25 for PAD-AREA-GLOW-1) drops staticShadow and keeps dynamicShadow, exactly as the chunk frag does. No new binding, no new texture fetch, one SSBO word on cement fragments only. DEFAULT OFF: uploads 0, the frag's first compare short-circuits before any SSBO read, output byte-identical. Also inert by construction on the legacy terrain path (u_padGlowMapSide == 0, no chunk cement grid)."
    },
    // CEMENT-ATLAS-CELL-2: the two arms of the "concrete shouldn't magically
    // become a different color" fix, scoped to ACTUAL concrete only.
    {
        "MC2_FEATURE_CEMENT_CONCRETE_TONE",
        "MC2_CEMENT_CONCRETE_TONE",
        EnvVarKind::Feature,
        false,
        "CEMENT-ATLAS-CELL-2 ARM A: tone-normalise ONLY the actual-concrete atlas cells. USER SCOPE RULING 2026-07-28: \"by all concrete I mean only the actual concrete ... obviously runways, roads, other overlays etc. are their own layer and are currently handled by the shader.\" HOW THIS DIFFERS FROM THE USER-REJECTED MC2_CEMENT_TONE_MATCH: the arithmetic is the same family (a per-channel multiplicative gain onto a median reference, per-texel detail preserved), but the SET is not. TONE_MATCH moved EVERY solid cement cell toward a WHOLE-ATLAS median with a [0.70,1.40] clamp, which is exactly why the user rejected it -- an asphalt cell, a dirt-stained pad and a clean apron were all dragged onto one tone and \"authored material variation flattened into a pale wash\". This gate first CLASSIFIES each solid cell (ClassifyConcreteFamily in gos_terrain_indirect.cpp): a cell joins the concrete family only if it is near-neutral (chroma <= 0.20) AND its [P50,P90] band luminance is within [0.72,1.32] of the family centre, where the centre is seeded from the upper (apron) cluster and then re-medianed over the admitted set. Non-family cells -- asphalt, dirt, coloured art -- are left BYTE-IDENTICAL, which is the specific behaviour the rejected gate lacked. Gain clamp is deliberately NARROWER at [0.80,1.25] (a cell needing more than that is by definition not the same material), and runs through the RunwayToneScale soft knee so painted markings inside a concrete cell keep their identity instead of clipping flat. Alpha/transition cells -- where authored runway and road MARKING art lives -- are appended past nSolidCells and are never in range. DEFAULT OFF: atlasBuf left byte-identical. Once per mission load, zero per-frame cost. The [CEMENT_CONCRETE v1] census (family size, family luminance span, per-cell marking fraction by nameId) prints regardless of the gate."
    },
    {
        "MC2_FEATURE_CEMENT_CONCRETE_CANON",
        "MC2_CEMENT_CONCRETE_CANON",
        EnvVarKind::Feature,
        false,
        "CEMENT-ATLAS-CELL-2 ARM B: point every plain-concrete tile at ONE canonical concrete cell, so an apron cannot change colour tile to tile at all. Where ARM A equalises each cell's tone while keeping its own art, this arm removes the per-cell variation outright -- the strongest possible reading of \"all of that concrete should be the same color\", and the arm that costs the most authored detail. MECHANISM: after the concrete-family classification (see MC2_CEMENT_CONCRETE_TONE), the canonical cell is chosen as the UNMARKED family member closest to the family reference (lowest index breaks ties, so it is deterministic across runs), and g_cementLayerIndexBySlot is repointed from each plain-concrete solid slot to that cell. Remapping the SLOT->LAYER map rather than the fragment shader means every downstream reader agrees by construction -- the cement-word bake, cemLayerForVertex, and the TileIsRoadArt oracle (which only tests != 0xFFFF, so road/runway classification is provably unaffected). SCOPE SAFETY, the question that blocked CEMENT-ATLAS-CELL-1: runway and road art does NOT reach this path. It arrives either as an &Overlays id (a separate layer with its own sampler and blend, u_overlaySidecar / terrain_overlay.frag) or as a cement-ALPHA transition cell, and alpha cells are deliberately excluded from g_cementLayerIndexBySlot by CEMENT-UNIFORM-2 and appended past nSolidCells. Solid concrete cells that nonetheless carry painted markings are detected (>2% of texels more than 60 luma ABOVE the cell's own band) and EXCLUDED from the remap, keeping their own art; the census reports them per-cell by nameId so they can be audited. DEFAULT OFF: no remap written => byte-identical. Once per mission load, zero per-frame cost."
    },
    {
        "MC2_FEATURE_SSAO_BLUR_SIGMA",
        "MC2_SSAO_BLUR_SIGMA",
        EnvVarKind::Feature,
        false,
        "SSAO-BLUR-1 tunable: RELATIVE depth tolerance (|d-dc|/dc) for the SSAO bilateral blur's edge-stopping weight. Clamped [0.001,1.0]; unset keeps the built-in default. Env is read once at init and OVERRIDES the built-in; the ImGui slider still moves it live. WHY IT IS A FIRST-CLASS KNOB: at the initial 0.02 (2% relative depth) the blur measured -52.5% HF noise on a flat, near-constant-depth wall (mc2_01 cliff_wall_sideon) but was COMPLETELY INERT on oblique/grazing views (cross_map_view, oblique_fog, oblique_horizon: HF delta ~0.0%, maxAbs 1-4/255). On a grazing surface adjacent half-res texels differ in depth by far more than 2%, so every neighbour tap was rejected, wsum collapsed to the centre weight, and the pass silently became a no-op precisely where the AO buffer is noisiest. Too LARGE a sigma is the opposite failure -- the blur stops respecting silhouettes and drags a prop's contact darkening onto the background behind it. Grade it per-scene against both failure modes. Diagnostic/tuning only when MC2_SSAO is off (default-OFF), so no default launch changes."
    },
    {
        "MC2_FEATURE_SSAO_RADIUS",
        "MC2_SSAO_RADIUS",
        EnvVarKind::Feature,
        false,
        "SSAO-RADIUS-REVERT-1 (VERIFY-DEBT-1): explicit launch-time override for the SSAO world-unit sample radius. Any float, clamped [0.1,64.0] to match gos_SetSsaoRadius; unset keeps the built-in default (24.0). WHY IT EXISTS: SSAO-BLUR-1 changed two coupled things -- it added the bilateral denoise pass AND raised aoRadius 3.0 -> 24.0 -- but shipped a killswitch for only the first. MC2_SSAO_BLUR=0 therefore claimed to \"restore the pre-SSAO-BLUR-1 two-draw chain\" while silently retaining the 8x radius, producing radius-24-without-denoise: a configuration that has never shipped and is precisely the speckle-on-flat-unoccluded-faces failure SSAO-RETUNE-1 documented. The radius change had no revert of its own; this is it. WHY THE RADIUS IS NOT SLAVED TO MC2_SSAO_BLUR: the stated purpose of that gate is to A/B the BLUR on one exe, and flipping the radius alongside it makes that a two-variable change. Worse, 3.0 is STRUCTURALLY DEAD -- one map cell is 42.67 wu (worldUnitsPerVertex 128 / MAPCELL_DIM 3) and the baked 16-tap kernel is not unit-normalised (mean |v| 0.289), so radius 3 lands the mean tap 0.87 wu from centre (sub-pixel, every tap reading the same depth texel) while rangeCheck = smoothstep(0,1,aoRadius/dist) independently zeroes any occluder past 3 wu; measured 0.0000% of AO-buffer pixels below 250/255, i.e. occlusion identically zero. Auto-reverting to 3.0 would silently switch AO OFF for anyone setting MC2_SSAO_BLUR=0 -- a worse failure than the one being fixed. Instead the known-bad pairing (blur off + retuned radius + no explicit override) emits a loud [SSAO v1] WARNING naming both ways out. MC2_SSAO is default-OFF, so this changes no default launch; the radius also remains adjustable at runtime via ImGui and the per-mission visual-tuning-profile `aoRadius` key."
    },
    // RUNWAY-APRON-TONE-2: cross-map absolute runway/apron tone normalisation.
    {
        "MC2_FEATURE_CEMENT_LIT",
        "MC2_CEMENT_LIT",
        EnvVarKind::Feature,
        false,
        "RUNWAY-APRON-TONE-2 (user report, mc2_10 @ bookmark 204937): 0..1 share of the terrain lighting term that CEMENT fragments obey; default 0.0 == stock fully flat-lit slab (terrain_lod_chunk.frag historically forced normalLight = mix(normalLight, 1.0, pureConcrete)). MEASURED ROOT CAUSE: at that pose the apron is drawn from the flat-lit cement catalog atlas at luminance 145..171 while the runway under the parked aircraft is NOT cement-classified at all -- the MC2_TERRAIN_LOD_CHUNK_DIAG=32768 class view shows it uncoloured, i.e. baked colormap only -- and so is drawn through the full terrain lighting at luminance 65. Two sources, one of them lighting-exempt, is why a single runway changes tone along its length and why the apron reads as pasted art. At 1.0 cement obeys the same light as the terrain it sits on, so the two sources stay in a consistent relationship under sun, dusk and night. DEFAULT 0.0 = byte-identical stock. Per-frame cost: one extra uniform upload."
    },
    {
        "MC2_FEATURE_CEMENT_RAMP_WORD",
        "MC2_CEMENT_RAMP_WORD",
        EnvVarKind::Feature,
        false,
        "CEMENT-WORD-RAMP-1: the ALBEDO-side cure for the project's longest-running user complaint, the dark outlines around every concrete pad and taxiway (\"concrete shouldn't magically become a different color\"). MEASURED ROOT CAUSE (CEMENT-EDGE-CURVE-1, confirmed here): the one-tile vertex-interpolated terrainType ramp band (0 < smoothstep(2,3,v_terrainType) < 1) has NO cement word, so terrain_lod_chunk.frag keeps the dark baked colormap as `base` there while the pad interior draws cementAtlas * u_missionFlatLight. Measured on mc2_23 against a MEASURED 0.000%% two-run floor: band L 15.7-21.8 vs pad L 70-123, a ring depth of -30.3 to -58.0 luma below a straight blend. Every prior lever was lighting-side (MC2_CEMENT_LIT, MC2_CEMENT_TONE_MATCH, MC2_RUNWAY_TONE_MATCH) and therefore structurally incapable of closing an albedo-source gap. MECHANISM, two halves. CPU (gos_terrain_indirect.cpp, PopulateRecipeCementWords): the open-border mask pass (cementWord bits 19:16) is widened to admit TRANSITION cement boundary tiles as DONORS, not just SOLID ones -- stock MC2_RUNWAY_EDGE_BLEND marks only 71 tiles on mc2_23 because nearly the whole pad perimeter is authored transition art, and measured it recovers only 1-2.6 of the 30-58 luma deficit. FRAG: on the BARE-TERRAIN tile, borrow the nearest flagged donor's cement atlas cell into `base` with weight sqrt(ramp); since the albedo mix downstream multiplies by concreteColorBlend = sqrt(pureConcrete), the net cement fraction is EXACTLY LINEAR in the ramp -- the same weight the lighting exemption already uses, so albedo and lighting finally agree across the band. Never sets cementHit, so it introduces no new 0.5 contour and no hard step (that snap is why MC2_RUNWAY_EDGE_BLEND measures 0.6-10.8%% hard-step pixels, the same failure mode that made MC2_PAVED_OVERLAY_CLASS worse). The mask lives on the CEMENT tile and is read only by its BARE-TERRAIN neighbour, so the donor's own rendering is unchanged and the user-approved crisp CEMENT-HARD-EDGE-1 marching-squares transitions are structurally untouched (cementTransAlpha is binary by construction and cementHit forces 1.0; neither is on this path). DEFAULT OFF => no mask bits written for transition tiles and the frag block is skipped entirely => byte-identical stock. [RUNWAY_EDGE_BLEND] event=edge_mask_baked now also prints ramp_word=0|1."
    },
    {
        "MC2_FEATURE_CEMENT_ALPHA_ATLAS",
        "MC2_CEMENT_ALPHA_ATLAS",
        EnvVarKind::Feature,
        true,
        "DEFAULT ON since RUNWAY-CLOSE-1 (2026-08-02), measured at the user's mc2_10 saved_204907 bookmark, exe 30c18df1: rescued swath pixels 15 -> 6506, rescued luma 32.4 (bare dirt) -> 54.7, matching the historical ON-CONCRETE runway value 65.0-65.7 at runway_wide -- the user criterion is 'paved art should ALWAYS be the color that they are on top of concrete', and on-concrete runway is DARK, so runway==apron-brightness graders are measuring the wrong target. Control pose byte-identical OFF vs ON. =0 restores the drop. CEMENT-ALPHA-LAND-1 (user report on three maps: \"we are missing part of the runway\"): admit ALPHA (transition) cement slots into the cement catalog atlas as their own cells, and honour the cell's own texel ALPHA as coverage in terrain_lod_chunk.frag, so a transition tile can pave itself instead of borrowing a layer from a corner. ROOT CAUSE the gate closes: BuildCementCatalogAtlas skips isAlpha slots (\"pure-cement only\"), so PopulateRecipeCementWords can only bake a word for a TRANSITION tile when a corner terrainType resolves to a SOLID cement layer -- and where pavement crosses open ground no corner does. Measured drop: mc2_10 278 of 320 transition tiles = 87%, mc2_24 266 of 778 = 34%; MC2_CEMENT_TRANSITION_FALLBACK borrows from a NEIGHBOUR instead and recovers only ~12% because dropped tiles form contiguous runs. When ON, alpha slots are APPENDED after every solid cell (solid layer indices stay byte-stable) and a DROPPED transition tile bakes a word carrying bit23 OWN_ALPHA plus its own slot's layer; the frag composites that cell over the terrain using its alpha. NARROW BY DESIGN: fires only on tiles the legacy path drops, so the user-approved CEMENT-HARD-EDGE-1 marching-squares transitions are untouched. =0 => no atlas cell appended, no word carries bit23, frag branch dead => byte-identical stock. The [CEMENT_ALPHA_ATLAS v1] admit line and the alpha_rescued field of [CEMENT_TRANSITION_DROP v1] print regardless of the gate."
    },
    {
        "MC2_FEATURE_PAVED_OVERLAY_CLASS",
        "MC2_PAVED_OVERLAY_CLASS",
        EnvVarKind::Feature,
        false,
        "DEFAULT 0 (OFF). CONCRETE-DELIVER-1 promoted this to default 1; CONCRETE-DELIVER-2 REVERTED that. The promotion rested on the ROAD-OVERLAY-LIGHT-1 attribution ('closing the paved-classification gap cures the weird coloured edge'), which CEMENT-EDGE-CURVE-1 sec.4 had ALREADY measured and retracted in writing: at the user's own night poses mode 1 measures max delta 20-29/255 and 9.2-21.7%% HARD-STEP pixels -- 'It ADDS hard-edged outlines rather than removing them', and 'This should not propagate into another status doc.' It propagated anyway. Mechanism agrees with the retraction: mode 1 is LIGHTING-ONLY, the tile keeps its dark baked colormap, and CEMENT-EDGE-CURVE-1 sec.3 measured the ceiling at L~24 vs concrete L~60-87, so it structurally CANNOT deliver 'runways the color they are on top of concrete like in mc2_01' -- that gap is albedo-side. The originating lane's own precondition (PAVED-OVERLAY-CLASS-1-STATUS.md: a user click-through on mc2_10 saved_204937 is 'the single decision that gates everything else', plus extended pose coverage 'before any default-ON proposal') was never met. Evaluate with MC2_PAVED_OVERLAY_CLASS=1; nothing in OVERLAY-CEMENT-ADOPT-1 depends on it. ORIGINAL ENTRY FOLLOWS.PAVED-OVERLAY-CLASS-1 (user report on five maps: \"all of that concrete should be the same color, same for the roads\"; one runway changes tone along its own length; a \"weird coloured edge\" where pavement meets pavement). ROOT CAUSE: roads and runways are &Overlays ids (PAVED_ROAD=1, RUNWAY=7, ...) painted over an ARBITRARY base terrain type, while isCement() tests a flag on the BASE SLOT (terrtxm.h, types 10/13-20). The two channels are DISJOINT, so overlay-only pavement is never classified, never baked, renders as LIT baked colormap beside a lighting-EXEMPT cement apron, and increments no drop counter -- it is invisible to the [CEMENT_TRANSITION_DROP v1] census. Measured by [CEMENT_PAVEMENT_GAP v1]: mc2_01 29%, mc2_03 81%, mc2_05 41%, mc2_10 58%, mc2_24 34% of paved-overlay tiles carry no cement word; at the user's mc2_10 @204937 pose the runway is 100% unclassified. Every pre-existing lever (MC2_CEMENT_LIT, MC2_CEMENT_TONE_MATCH, MC2_RUNWAY_TONE_MATCH, MC2_RUNWAY_EDGE_BLEND) sits INSIDE the isCement && isAlpha guard or is multiplied by pureConcrete, which is why none can move these tiles -- ROAD-OVERLAY-LIGHT-1 measured the runway luma BIT-IDENTICAL across all four MC2_CEMENT_LIT strengths. This gate adds an IsPavedOverlayType() predicate OUTSIDE that guard. 1 = LIGHTING mode: the tile gets the same lighting exemption cement receives (cementWord bit 22, baked WITHOUT the bit-31 valid bit so every other cement reader ignores it by construction) but KEEPS its own baked colormap albedo -- this is what cures a runway changing tone along its own length, since that variation IS the per-tile lighting the neighbouring flat-lit cement never receives. 2 = ALBEDO mode (A/B arm): additionally routes the tile into the solid cement atlas so it samples the apron's texture; fuller tone convergence but replaces the baked road art, and the two prior albedo-swapping attempts were user-rejected as \"WAY worse\". A residual albedo gap in mode 1 is EXPECTED: the baked colormap and the cement atlas are different source textures. DEFAULT 0 = OFF = not one word written = byte-identical stock. Diagnostics: [PAVED_OVERLAY_CLASS v1] prints when the gate is on; MC2_TERRAIN_LOD_CHUNK_DIAG=32768 shows these tiles WHITE when classified and BLACK when not."
    },
    {
        "MC2_FEATURE_CEMENT_TRANSITION_FALLBACK",
        "MC2_CEMENT_TRANSITION_FALLBACK",
        EnvVarKind::Feature,
        false,
        "CEMENT-TRANSITION-FALLBACK-1 (MC2-03-CEMENT-PINK-1): cure MISSING ROAD SWATHS (user report, mc2_24). PopulateRecipeCementWords only bakes a cement word for a TRANSITION tile (isCement && isAlpha) when at least one corner terrainType resolves to a SOLID cement catalog layer AND deriveMaskIdFromCorners >= 0. A road tile running across open ground has neither, so it bakes NO word and falls back to the bare baked colormap -- but the paving/road ART lives in the cement catalog (see MC2_TERRAIN_LOD_CHUNK_DIAG=262144), so the road drops out for a stretch and resumes. When ON, a dropped transition tile adopts the cement layer of an orthogonally adjacent tile that DID bake a word (as a transition word with the all-corners mask; the shader re-derives coverage itself via CEMENT-HARD-EDGE-1, so outline shape is unaffected). DEFAULT OFF => no word changes. The [CEMENT_TRANSITION_DROP v1] census prints regardless of the gate."
    },
    {
        "MC2_FEATURE_CEMENT_ATLAS_MAGENTA_KEY",
        "MC2_CEMENT_ATLAS_MAGENTA_KEY",
        EnvVarKind::Feature,
        false,
        "CEMENT-ATLAS-MAGENTA-KEY-1 (MC2-03-CEMENT-PINK-1): solid cement catalog cells reach the atlas via TerrainTextures::textureFromMemory (gos_Texture_Solid), which -- unlike the keyed transition path (textureFromMemoryAlpha / TERRAIN-ROAD-COLORKEY-TRANSPARENCY-1) -- never honours MC2's legacy 0xFF00FF colourkey. Any magenta left in a solid cement cell is packed into the GUTTERLESS atlas and sampled GL_LINEAR, which can show as thin pink lines/patches along road and pad edges. When ON, matching texels (same chroma test as terrtxm.cpp: r>150 && b>150 && r>g+40 && b>g+40) are zeroed to transparent black. DEFAULT OFF. The [CEMENT_ATLAS_MAGENTA v1] census line is printed regardless of the gate, so a map can be judged without enabling it."
    },
    {
        "MC2_TUNE_NIGHT_AMBIENT_FLOOR",
        "MC2_NIGHT_AMBIENT_FLOOR",
        EnvVarKind::Trace,
        false,
        "NIGHT-LIGHT-POOLS-1 knob: minimum per-channel output (0..64, clamped) of Camera::getLightRed/Green/Blue -- the family feeding the colormap night burn-in (terrtxm2.cpp burnInShadows) and all flat-lit terrain/cement consumers. Lifts pitch-black night terrain to a subtle floor without touching values already above it. Default UNSET = 0 = stock data-faithful (identity). Read once per process (boot-cached static)."
    },
    {
        "MC2_FEATURE_TERRAIN_CLIFF_DISPLACE",
        "MC2_TERRAIN_CLIFF_DISPLACE",
        EnvVarKind::Feature,
        false,
        "SLICE 3: near-field cliff tessellation on the LOD-chunk terrain path. 3a = plumbing (GL_PATCHES + pass-through TCS/TES, NO displacement, output-preserving). Default-OFF stays GL_TRIANGLES, byte-identical."
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_MAXTESS",
        "MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS",
        EnvVarKind::Trace,
        false,
        "SLICE 3 knob: max tessellation factor (float; C++ default 4.0, clamped to GL_MAX_TESS_GEN_LEVEL at program build). Only consumed when MC2_TERRAIN_CLIFF_DISPLACE=1."
    },
    // TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo textures on the LIVE
    // LOD-chunk terrain path (gos_terrain_lod_chunk.cpp lazy loader + binder,
    // terrain_lod_chunk.frag u_useMatAlbedo branch). Gate OFF -> u_useMatAlbedo
    // uploads 0 -> the colormap-tint composition runs VERBATIM (byte-identical).
    {
        "MC2_FEATURE_TERRAIN_MATERIAL_TEXTURES",
        "MC2_TERRAIN_MATERIAL_TEXTURES",
        EnvVarKind::Feature,
        false,
        "TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB GL_TEXTURE_2D_ARRAY albedo (rock/grass/dirt/concrete/snow/cliff from data/terrain_layers/<channel>_albedo.ktx2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM -- sampler does the sRGB decode) sampled world-space with the material-lib per-layer tiling (matTiling; rock /3 matches the detail-normal path). matWeights (classifier or control map) + HSV snow select layers; the colormap stays the MACRO tint (2x multiply keeps burn-in shading); steep slopes take the CLIFF layer triplanar (the 'cliffs look like rock' payoff). Mix knob u_matAlbedoStrength: env MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH > JSON matAlbedoStrength (terrain_materials.json; also supports textureRoot + layers.<channel>.albedo path overrides via the TinyJson nested-layers extension) > 0.7 default. Fail-soft: missing/mismatched/non-BC7-sRGB layer files log [TERRAIN_MAT_TEX] and leave the gate visually OFF. VRAM ~32 MiB (6x2048^2 BC7 + mips). Default-OFF byte-identical."
    },
    // CONCRETE-DETAIL-NORMAL-1: the concrete detail normal is DEFAULT OFF.
    {
        "MC2_FEATURE_TERRAIN_CONCRETE_DETAIL_NORMAL",
        "MC2_TERRAIN_CONCRETE_DETAIL_NORMAL",
        EnvVarKind::Feature,
        false,
        "CONCRETE-DETAIL-NORMAL-1 (USER RULING 2026-07-26: 'I have decided I don't like the detail normal for concrete'). Scales the concrete branch of the LOD-chunk detail normal (terrain_lod_chunk.frag:1402-1405, MAT_LAYER_PAINTED_CONC: dN += pureConcrete * matNormalBoost.w * fwC * (cN.rgb*2-1)) by zeroing matNormalBoost.w (nb[3]) at the uniform upload in gos_terrain_lod_chunk.cpp. DEFAULT OFF = the 'original overlay' concrete the user graded and approved in the mc2-win64-v0.3 install -- that install ships WITHOUT data/terrain_layers/, so matNormalArray never loads and the concrete relief never appears; this gate reproduces that look in installs that DO ship the layers, WITHOUT deleting assets or freezing a stale binary. Shader untouched; rock/grass/dirt detail normals (nb[0..2]) and the cement albedo/atlas path are unaffected. '=1' restores the relief treatment. Cost: loses close-up concrete surface relief on runways/aprons -- the flat authored colormap tone is what ships."
    },
    // WATER-SHORE-MASK-1 (3c864eac): shipped default-ON but was never registered.
    {
        "MC2_FEATURE_WATER_SHORE_MASK",
        "MC2_WATER_SHORE_MASK",
        EnvVarKind::Feature,
        true,
        "WATER-SHORE-MASK-1: per-mission shoreline mask sidecar (data/missions/<m>.beauty/shoreline_mask.png) drives the water shore edge. DEFAULT ON; =0 disables (falls back to the legacy ramp). Silently inert when the sidecar is not installed."
    },
    // SHORELINE-GEOMETRY-1 / WATER-MEANDER-CLAMP-1: shore meander is DEFAULT OFF (0).
    {
        "MC2_FEATURE_WATER_SHORE_MEANDER",
        "MC2_WATER_SHORE_MEANDER",
        EnvVarKind::Feature,
        false,
        "SHORELINE-GEOMETRY-1: fBm meander amplitude in world units applied to the shore contour in gos_terrain_water_stream.cpp ShoreMeanderWu(); breaks the tile-quantised staircase waterline into an organic coastline. Value is an AMPLITUDE, not a boolean: unset/empty or <=0 => 0.0f = OFF = the stock, long-shipped waterline. '=75' was the previously-shipped setting and restores that behaviour exactly. DEFAULT PARKED AT 0 by 67b25f47 (WATER-MEANDER-CLAMP-1) because 75 CARVES AWAY WATER on mc2_10 and mc2_17 -- the |grad|-keyed clamp was refuted (cure and defect share a cause), so no separation exists yet. REGISTERED BY REFERENCE-REBUILD-1: this gate had its default changed while unregistered, surviving check-env-registry.sh only via that script's ALLOWLIST -- the same shape as the CONE-SPILL-COUPLING-1 defect the registry exists to catch. NOT byte-identical at 75 vs 0: measured on mc2_10 water poses with a clean cf97ed52 exe, 3 runs/arm, within-arm floor 0.0000% changed (max=0), between-arm 4.07% / 6.52% / 9.74% of pixels changed (max channel delta 73/73/115). Companion knob: MC2_WATER_SHORE_MEANDER_FREQ."
    },
    {
        "MC2_TUNE_WATER_SHORE_MEANDER_FREQ",
        "MC2_WATER_SHORE_MEANDER_FREQ",
        EnvVarKind::Feature,
        false,
        "SHORELINE-GEOMETRY-1 companion tuning knob for MC2_WATER_SHORE_MEANDER: spatial frequency of the shore-contour fBm, in cycles per world unit (gos_terrain_water_stream.cpp ShoreMeanderFreq()). Unset or <=0 => 1/384 wu = one dominant cycle per ~3 tiles (tile = 128 wu) -- long enough to bend a whole riser rather than serrate it, short enough that no single tile edge stays straight across its full length. INERT while MC2_WATER_SHORE_MEANDER is 0 (the shipping default), so this knob is byte-identical by construction unless the amplitude gate is also raised."
    },
    {
        "MC2_TUNE_TERRAIN_MATERIAL_TEXTURES_STRENGTH",
        "MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-MATERIAL-TEXTURES-1 knob: albedo mix strength override (float, clamped 0..1). 0 = legacy tinted colormap, 1 = full textured composite. Wins over the JSON matAlbedoStrength key; unset/NaN falls to JSON then the 0.7 default. Only consumed when MC2_TERRAIN_MATERIAL_TEXTURES=1."
    },
    // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: world-axis triplanar ROCK NORMAL + relief
    // on steep terrain cells (terrain_lod_chunk.frag useTriplanarCliff branch,
    // gos_terrain_lod_chunk.cpp kTglcCliffTriplanar upload). Samples the merged
    // matNormalArray layer 5 (MAT_LAYER_MARBLE_CLIFF, mat5_normal.tga: rgb=normal,
    // a=marble_cliff-derived displacement) projected from world axes so vertical
    // faces stop showing the stretched top-down splat and read as real rock relief.
    // DECOUPLED from the WIP albedo gate MC2_TERRAIN_MATERIAL_TEXTURES: the normal
    // array is the always-loaded legacy terrain normal array, so this needs no
    // albedo path. Default OFF -> uploads useTriplanarCliff=0 -> frag block no-op
    // (byte-identical).
    {
        "MC2_FEATURE_TERRAIN_CLIFF_TRIPLANAR",
        "MC2_TERRAIN_CLIFF_TRIPLANAR",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: on steep terrain cells (macro slope, cliffBlend=smoothstep(0.85,0.55,|macroNz|)), project the MAT_LAYER_MARBLE_CLIFF (layer 5) ROCK NORMAL from WORLD axes (triplanar, ts=256 wu/repeat) into N and darken by its displacement relief (matNormalArray layer5 .a), instead of leaving the near-vertical face shaded by the stretched top-down terrain splat -- so cliff faces read as real rock. Samples the always-loaded legacy terrain normal array (gos_GetTerrainNormalArrayTex) so it is independent of the WIP MC2_TERRAIN_MATERIAL_TEXTURES albedo path. Strength via MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH (default 1.0). Default OFF -> useTriplanarCliff uploads 0 -> frag block skipped (byte-identical)."
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_TRIPLANAR_STRENGTH",
        "MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1 knob: triplanar rock-normal tilt strength (float, default 1.0). Scales how strongly the world-axis rock normal perturbs the terrain normal on cliff faces. Only consumed when MC2_TERRAIN_CLIFF_TRIPLANAR=1."
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH",
        "MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-HEIGHT-NORMAL-1 knob (float, default 2.0): derives the cliff SHADING normal from the HEIGHT GRADIENT of the cooked cliff displacement (MAT_LAYER_MARBLE_CLIFF layer-5 normal-array ALPHA), triplanar forward-difference, and mixes triTilt strongly toward it so the rich rock relief catches light (the smooth marble RGB normal left lit cliffs flat). Larger = more pronounced relief lighting; the mix toward the height normal saturates with strength (hMix=clamp(strength*0.5,0,1)). When POM is on, the gradient is sampled at the POM-offset UV so relief + parallax agree. Only consumed inside the MC2_TERRAIN_CLIFF_TRIPLANAR=1 frag block; the byte-identical path remains MC2_TERRAIN_CLIFF_TRIPLANAR=0."
    },
    {
        "MC2_FEATURE_TERRAIN_CLIFF_POM",
        "MC2_TERRAIN_CLIFF_POM",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CLIFF-POM-1: triplanar Parallax Occlusion Mapping on cliff faces. Extends MC2_TERRAIN_CLIFF_TRIPLANAR: for each of the 3 world-axis triplanar planes (YZ/XZ/XY, weighted by |macroN|), march the REAL marble_cliff displacement (mat5_normal.tga alpha, from marble_cliff_01_disp_4k) along the per-fragment world-space view vector (from cameraPos, MC2 world swizzle (-x,z,y)) projected onto that plane, producing a UV occlusion offset applied to BOTH the cliff normal and albedo triplanar samples so vertical rock reads with real view-dependent depth (unlike top-down MC2_TERRAIN_POM which fades off cliffs). Shading-only (NO gl_FragDepth). Requires MC2_TERRAIN_CLIFF_TRIPLANAR=1 (shares the triplanar block). Default OFF -> u_cliffPom.x=0 -> march skipped -> triplanar block byte-identical to TRIPLANAR-1."
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_POM_DEPTH",
        "MC2_TERRAIN_CLIFF_POM_DEPTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-POM-1 knob: parallax depth = world-unit rock height scale relative to the CLIFF tiling (ts=256 wu/repeat). Float > 0, default 12. Larger = deeper apparent relief / stronger occlusion. Only consumed when MC2_TERRAIN_CLIFF_POM=1."
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_POM_STEPS",
        "MC2_TERRAIN_CLIFF_POM_STEPS",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-POM-1 knob: max POM march layers per plane, clamped 8..32 (default 24; compile-constant loop cap 32). More steps = smoother occlusion at grazing angles, higher cost. Only consumed when MC2_TERRAIN_CLIFF_POM=1."
    },
    {
        "MC2_DIAG_TERRAIN_CLIFF_DEBUG",
        "MC2_TERRAIN_CLIFF_DEBUG",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-DEBUG: bounded debug-viz for the LOD-chunk terrain cliff material path (u_cliffDebug in terrain_lod_chunk.frag). Confirms whether the MC2_TERRAIN_CLIFF_TRIPLANAR / MC2_TERRAIN_CLIFF_POM path executes and contributes on a given cliff. Int 0=off (default, byte-identical). 1=cliffBlend grayscale (steep-face smoothstep(0.85,0.55,|macroNz|) firing? black=0..white=1); 2=branch-taken coverage (green=useTriplanarCliff cliff block entered, red=not); 3=cliff POM UV-offset magnitude as color (parallax march producing non-zero offset?); 4=sampled MAT_LAYER_MARBLE_CLIFF (layer 5) normal-array ALPHA grayscale (cooked displacement actually bound/sampled?). Each mode writes directly to final color and returns early. Default 0 -> block skipped -> byte-identical."
    },
    {
        "MC2_DIAG_VFX_DEBUG_MODE",
        "MC2_VFX_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "VFX-DEBUG-VIEWS-1: GPU particle billboard fragment-shader debug-mode selector (particle_billboard.frag u_debugMode, uploaded by gos_particle_bridge). Default unset = mode 0 (Final, byte-identical to default output). Modes: 1=Albedo (raw atlas texel rgb, no vertex-color tint), 2=Alpha (final alpha as grayscale), 3=ParticleKind (distinct hashed color per kind_flags kind), 4=Overdraw (constant additive proxy to visualize blend buildup). Seeded once at process start from this env var (clamped 0..4); the VFX Pass Object-Inspector panel shows the active mode read-only (gos_vfx_getDebugMode), and the Graphics Options 'VFX Tuning' combo overrides it live (gos_vfx_setDebugMode). Diagnostic-only; no gameplay, emission, lifetime, sorting, or default visual effect; VFX object-IDs remain prohibited. RenderDebugView canonical mapping (kDebugViewMask_Vfx): Final->0, Albedo->1; modes 2-4 are VFX-local (no canonical enum slot)."
    },
    {
        "MC2_TUNE_VFX_BRIGHTNESS",
        "MC2_TUNE_VFX_BRIGHTNESS",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the global GPU-particle RGB brightness scale (particle_billboard.frag u_vfxBrightness, applied to ALL particles). Clamped 0..8. Unset/empty -> 1.0 (byte-identical no-op). Seeded once at process start; the Graphics Options 'VFX Tuning > Brightness' slider overrides at runtime (gos_vfx_setBrightness). Look-only — no emission/lifetime/sorting/timing change. No effect when MC2_GPU_PARTICLES=0."
    },
    {
        "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
        "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the additive-only GPU-particle RGB brightness scale (particle_billboard.frag u_vfxAdditiveBrightness, applied ONLY to additive draw groups via per-group u_vfxIsAdditive). Clamped 0..8. Unset/empty -> 1.0 (byte-identical no-op). Graphics Options 'VFX Tuning > Additive brightness' slider overrides at runtime (gos_vfx_setAdditiveBrightness). Highest-value lever for pre-bloom additive overdraw (see docs/vfx-overdraw-audit.md). Look-only. No effect when MC2_GPU_PARTICLES=0."
    },
    {
        "MC2_TUNE_VFX_ALPHA_SCALE",
        "MC2_TUNE_VFX_ALPHA_SCALE",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the GPU-particle alpha (opacity) scale (particle_billboard.frag u_vfxAlphaScale, applied to ALL particles). Clamped 0..8. Unset/empty -> 1.0 (byte-identical no-op). Graphics Options 'VFX Tuning > Opacity' slider overrides at runtime (gos_vfx_setAlphaScale). Look-only — no emission/lifetime/timing change. No effect when MC2_GPU_PARTICLES=0."
    },
    {
        "MC2_TUNE_VFX_LIT_STRENGTH",
        "MC2_TUNE_VFX_LIT_STRENGTH",
        EnvVarKind::Trace,
        false,
        "VFX-LIT-PARTICLES-MVP-1: startup-default for the scene-lit smoke/dust strength (particle_billboard.frag u_vfxLitStrength), applied ONLY to alpha draw groups (u_vfxIsAdditive==0) and ONLY when the MC2_VFX_LIT_PARTICLES gate is ON. Clamped 0..1. Unset/empty -> 0.7. Mixes the particle color toward the scene sun+ambient fill (eye->light*/ambient*); 0 = unlit (byte-identical). Per-mission profile key 'vfxLitStrength' (env value-var wins) and the Graphics Options 'VFX Tuning' slider override at runtime (gos_vfx_setLitStrength). Look-only; no emission/lifetime/timing change. No effect when MC2_VFX_LIT_PARTICLES is OFF or MC2_GPU_PARTICLES=0."
    },
    {
        "MC2_TUNE_VFX_AGE_FADE",
        "MC2_TUNE_VFX_AGE_FADE",
        EnvVarKind::Trace,
        false,
        "VFX-SHADER-AGE-FADE-PARITY-1: soft death fade for oracle-harvested particles (p.lifetime==1.0f sentinel) in the final 30% of normalized age. Applied in particle_billboard.vert as v_color.a *= mix(1.0, ageFade, u_vfxAgeFade). Clamped 0..1. Unset/empty -> 0.0 (byte-identical no-op). Oracle-only: non-oracle particles have p.lifetime==0.0 and are unaffected. Look-only; no emission/lifetime/sim change. Env wins; gos_vfx_setAgeFade() overrides at runtime for ImGui slider."
    },
    {
        "MC2_DIAG_SSAO_DEBUG",
        "MC2_SSAO_DEBUG",
        EnvVarKind::Trace,
        false,
        "SSAO-GTAO-LITE-MVP-1: when set with MC2_SSAO=1, the SSAO apply pass OVERWRITES the scene with the half-res AO buffer as grayscale (blend disabled) instead of multiplying, so the raw occlusion can be inspected. Diagnostic-only; no effect when MC2_SSAO is off. Default-OFF."
    },
    // SSAO-BLUR-1: the missing denoise. Killswitch, NOT an opt-in -- default ON
    // whenever SSAO runs, because the un-blurred chain is not a shippable config.
    {
        "MC2_FEATURE_SSAO_BLUR",
        "MC2_SSAO_BLUR",
        EnvVarKind::Feature,
        true,
        "SSAO-BLUR-1: separable depth-aware (joint bilateral) blur over the half-res AO buffer, inserted between the AO gen pass and the apply pass (shaders/ssao_blur.frag; H into a scratch half-res target then V back into ssaoColorTex_, so the SsaoOcclusion resource id and the UNCHANGED ssao_apply pass always read the final AO). DEFAULT ON whenever MC2_SSAO=1; MC2_SSAO_BLUR=0 restores the pre-SSAO-BLUR-1 two-draw chain for A/B on one exe. WHY: ssao.frag integrates 16 hemisphere samples with a per-pixel hash12 rotation of the TBN basis -- a stochastic estimator whose noise amplitude GROWS with aoRadius. Before this the ONLY denoise was a 4-tap box spanning +/-0.5 of a half-res texel inside ssao_apply.frag, whose comment claimed it 'denoises'; it cannot, because the rotation decorrelates every pixel. SSAO-RETUNE-1 measured the consequence: at any aoRadius large enough to produce visible occlusion, the added darkening read as speckled dither on FLAT, unoccluded faces (a container's end face) rather than as contact shading. Each tap is weighted by RELATIVE depth difference (|d-dc|/dc, a constant fraction of view distance -- an absolute epsilon would be far too permissive up close and far too strict in the distance under reverse-Z) so the blur averages within a surface and stops at silhouettes instead of dragging a building's contact darkening onto the tarmac behind it. Sky/cleared taps (depth<=0.0001, AO=1) are excluded so the background stays intact. Cost: two half-res 9-tap draws (9x9 effective support for 18 taps, at a quarter of the full-res pixel count). SCOPE: GL path only. The native Vulkan SSAO path (MC2_VK_POST_SSAO, rung 8.2, default-OFF opt-in) reuses the ssao/ssao_apply SPIR-V verbatim and does NOT include this blur, so with that gate ON the noise returns; porting it is unfinished follow-up work. MC2_SSAO itself remains default-OFF, so this changes no default launch."
    },
    {
        "MC2_DIAG_SHADOW_DEBUG_MODE",
        "MC2_SHADOW_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "SHADOW-ENV-DEBUG-MODE-1: selects shadow debug map overlay without ImGui. "
        "Default unset/0/off = overlay OFF (byte-identical). "
        "Values: off|0=OFF, static|1=STATIC (world-fixed 2048^2 depth), dynamic|2=DYNAMIC (camera-centered 1024^2 depth). "
        "Resolved once at process start (gos_postprocess init). RAlt+F2 hotkey still overrides at runtime."
    },
    // TRACKV-TACTICAL-PRESENTATION: ViewMode seed + LowLight tunables + tactical
    // overlay. All default-OFF/inert; Visual path byte-identical when unset.
    // (MC2_VIEWMODE_FRAMEWORK itself is the RendererFeature::ViewmodeFramework
    // enum entry, registered in kFeatureTable above.)
    {
        "MC2_TUNE_VIEW_MODE",
        "MC2_VIEW_MODE",
        EnvVarKind::Trace,
        false,
        "VIEWMODE-POSTPROCESS-PRESENTATION-1: seeds the startup presentation mode (matches RenderCore::ViewMode). Numeric 0..5 or name visual|objectid|tactical|thermal|infrared|lowlight. Only consulted when MC2_VIEWMODE_FRAMEWORK=1; the ImGui combo overrides at runtime. ObjectIdDebug(1) needs MC2_OBJECT_ID_BUFFER; Thermal(3) is a luminance placeholder; TacticalOverlay(2) is drawn in-scene (see MC2_TACTICAL_ARC_OVERLAY), not by the composite."
    },
    {
        "MC2_TUNE_VIEWMODE_LOWLIGHT_GAIN",
        "MC2_VIEWMODE_LOWLIGHT_GAIN",
        EnvVarKind::Trace,
        false,
        "LOWLIGHT-NIGHTVISION-MVP-1: night-vision luminance amplification (clamped 0.1..16, default 2.5). Only affects the LowLight(5) composite branch. Resolved once at gos_postprocess init; ImGui slider overrides at runtime."
    },
    {
        "MC2_TUNE_VIEWMODE_LOWLIGHT_TINT",
        "MC2_VIEWMODE_LOWLIGHT_TINT",
        EnvVarKind::Trace,
        false,
        "LOWLIGHT-NIGHTVISION-MVP-1: night-vision tint as 'r,g,b' floats (default 0.7,1.0,0.6 green-phosphor). Only affects the LowLight(5) composite branch. Resolved once at gos_postprocess init."
    },
    {
        "MC2_FEATURE_TACTICAL_ARC_OVERLAY",
        "MC2_TACTICAL_ARC_OVERLAY",
        EnvVarKind::Feature,
        false,
        "TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line on each SELECTED mover, drawn in-scene by the debug renderer just before its flush (gamecam.cpp). Default-OFF; byte-identical when unset (no draw calls). Requires MC2_DEBUG_RENDERER=1 (the debug renderer's own gate) AND a selected unit to show anything. Read-only; no gameplay coupling."
    },
    {
        "MC2_TRACE_COLORMAP_DISPLACE_PROBE",
        "MC2_COLORMAP_DISPLACE_PROBE",
        EnvVarKind::Trace,
        false,
        "COLORMAP-CPU-RETIRE-1 parity probe: logs displacement magnitude (maxDispMag in wu) from the CPU colormap displacement path. Requires MC2_COLORMAP_CPU_RETIRE=0 (kill-switch) so cpuColorMap is still alive. Default-OFF; set to any non-null value to enable. Every 50000 calls: prints [COLORMAP_PROBE] samples=N maxDispMag=X wu."
    },
    // DrawPacket v8 kill-switch
    {
        "MC2_FEATURE_STATIC_PROP_LIVE_BUILDER",
        "MC2_STATIC_PROP_LIVE_BUILDER",
        EnvVarKind::Feature,
        false,
        "v8 kill-switch: =1 restores live build + snapshot compare"
    },
    // STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1
    {
        "MC2_TRACE_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE",
        "MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1 gate: default OFF. =1 builds s_spRowCache from registry each frame and compares field-by-field with legacy propBuf. Emits [SNAPSHOT_BRIDGE_COMPARE v1] log per frame."
    },
    // TRACKV-CI-GATE-RESTORE-1 diagnostic trace vars
    {
        "MC2_TRACE_ASSIMP_TRACE",
        "MC2_ASSIMP_TRACE",
        EnvVarKind::Trace,
        false,
        "TRACKV-CI-GATE-RESTORE-1: verbose Assimp loader trace. Default-OFF; =1 enables per-mesh import log. Diagnostic output only; no correctness effect."
    },
    {
        "MC2_TRACE_CAMERA_MOVE_DIAG",
        "MC2_CAMERA_MOVE_DIAG",
        EnvVarKind::Trace,
        false,
        "TRACKV-CI-GATE-RESTORE-1: camera movement diagnostic readback probe. WARNING: triggers GPU pipeline stall + glGetNamedBufferSubData readback — diagnostic default-OFF only. =1 enables per-frame camera-move diagnostic logs."
    },
    {
        "MC2_TRACE_FLUSH_PROBE",
        "MC2_FLUSH_PROBE",
        EnvVarKind::Trace,
        false,
        "TRACKV-CI-GATE-RESTORE-1: GPU pipeline flush probe via glFinish. WARNING: triggers GPU pipeline stall — diagnostic default-OFF only. =1 enables glFinish at the probe site to measure GPU-complete latency."
    },
    // TRACKV quick-win: Probe-6 thin-record canary gate
    {
        "MC2_TRACE_THIN_CANARY",
        "MC2_THIN_CANARY",
        EnvVarKind::Trace,
        false,
        "TRACKV: gates Probe-6 thin-record canary readback (per-frame ~80KB glGetBufferSubData x2 + CPU recipeIdx/flags compare, diagnostic for the corrupt-recipeIdx grey-triangle bug). Default-OFF; =1 re-enables. No render effect."
    },
    // Static-prop ORM (PBR) — load-only feature + trace
    {
        "MC2_FEATURE_STATICPROP_MATERIAL_PBR_SLOTS",
        "MC2_STATICPROP_MATERIAL_PBR_SLOTS",
        EnvVarKind::Feature,
        false,
        "Static-prop PBR ORM (occlusion-roughness-metallic) texture-array slots + sidecar feed. Default-OFF (any non-'0' non-empty enables); gate-OFF path is byte-identical (zero ORM arrays, zero sidecar I/O)."
    },
    {
        "MC2_TRACE_STATICPROP_ORM_TRACE",
        "MC2_STATICPROP_ORM_TRACE",
        EnvVarKind::Trace,
        false,
        "Static-prop ORM load trace: logs source path + dims per unique ORM texture. Default-OFF diagnostic; no correctness effect."
    },
    // SPFLUSH-COST-SPLIT-1: RDTSC decomposition of StaticPropRegistryFlush hot zone.
    {
        "MC2_TRACE_STATIC_PROP_FLUSH_COST_SPLIT",
        "MC2_STATIC_PROP_FLUSH_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "SPFLUSH-COST-SPLIT-1: RDTSC cost-split decomposition of GpuStaticPropRegistry::flush(). "
        "Emits [SPFLUSH_COST_SPLIT v1] event=summary every 10 frames with per-bucket ns averages "
        "(submit_loop, inst_build, map_lookup, color_fill, actor_record, world_to_block, "
        "substrate_append, baseinstance_upload) + lifetime/window dirty counters "
        "(invalidates, registrations, recipe_rebuilds, light_writes). "
        "TSC calibrated once at first use (~1ms spin). Default-OFF; =1 enables. ZERO behavior change."
    },
    // GLSTATEGUARD-SLICE-1
    {
        "MC2_GLSTATEGUARD_LOG",
        "MC2_GLSTATEGUARD_LOG",
        EnvVarKind::Trace,
        false,
        "GLSTATEGUARD-SLICE-1: leak-visibility for mc2gl::GlScopedSsboBinding (gl_state_guard.h). "
        "Default-OFF; set to any non-null value to enable. Emits [GLSTATEGUARD v1] one-liner whenever a "
        "guarded SHADER_STORAGE_BUFFER binding-base slot was actually changed inside the guarded region "
        "(i.e. the RAII restore is load-bearing). OBSERVATION-ONLY: the RAII save/restore itself is "
        "structural and always-on, independent of this var — the log only reports it. ZERO behavior change."
    },
    // GLSTATEGUARD-SLICE-2
    {
        "MC2_GLSTATEGUARD_TERRAIN",
        "MC2_GLSTATEGUARD_TERRAIN",
        EnvVarKind::Feature,
        true,
        "GLSTATEGUARD-SLICE-2: kill-switch for the RAII depth/blend/cull ownership of the terrain "
        "LOD chunk draw (gos_TerrainLodChunk_SubmitDrawCommands via mc2gl::GlScopedCapability + "
        "GlScopedDepthState). Default-ON: guards own GL_DEPTH_TEST/WRITEMASK/DEPTH_FUNC/BLEND/CULL_FACE "
        "save+set+restore. =0 reverts to the legacy hand-rolled save/restore (kept verbatim) — the A/B "
        "used to prove the guards are pixel-neutral (byte-identical captures ON vs OFF). Pixel-neutral "
        "refactor; the explicit state set is identical on both paths."
    },
    // MC2_LOG master log gate
    {
        "MC2_LOG",
        "MC2_LOG",
        EnvVarKind::Trace,
        false,
        "Master stdout log gate (gameosmain.cpp, right after setvbuf). stdout is "
        "unbuffered, so per-mission telemetry printf flushes synchronously and "
        "causes 400ms+ console hitches when stdout is connected. Default (unset): "
        "freopen stdout -> NUL so NORMAL PLAY discards all [TAG] printf output and "
        "never hitches. Set (to anything): keep stdout live for the smoke harness "
        "([SMOKE v1] parsing), dev, and hitch-catcher.bat. stderr is never gated "
        "(crashes / [VISUAL_CAPTURE] stay visible). Set =1 by run_smoke.py + "
        "run_visual_capture.py; NOT set by the game launcher (so play stays fast)."
    },
    // BUG-A pick-probe fixture (mouse->pick DPI regression fence)
    {
        "MC2_VK_PICK_PROBE",
        "MC2_VK_PICK_PROBE",
        EnvVarKind::Trace,
        false,
        "BUG-A pick regression fence: default-OFF. When set, MissionInterfaceManager::update "
        "(code/missiongui.cpp) runs ONCE post-settle a deterministic mouse->pick coordinate "
        "proof: it simulates a 2x HiDPI drawable and asserts getMouseX round-trips an SDL "
        "window-point (identity), then aims at a visible home mover's screen-bbox centre and "
        "asserts findObjectByMouse resolves back to that mover. Emits [VK_PICK_PROBE] "
        "identity/object/VERDICT lines. Backend-independent (gl or vk). Proves the "
        "gos_GetMouseInfo window-vs-drawable normalize fix. Zero cost unset."
    },
    // FX-FORCE-SPAWN fixture (mech-fire tube)
    {
        "MC2_FX_FORCE_SPAWN",
        "MC2_FX_FORCE_SPAWN",
        EnvVarKind::Feature,
        false,
        "FX-FORCE-SPAWN v1 fixture: default-OFF. When set, MechWarrior::updateActions "
        "(code/warrior.cpp) forces ONE mech to fire its ENERGY weapons (PPCs -> tube "
        "ribbons) at a point in front of it, so idle smoke/visual captures -- which "
        "spawn NO weapon FX -- have a visible PPC TUBE to observe (target mission: "
        "mc2_24). Fires once (global latch) via fireWeapon(target=NULL, &targetPoint); "
        "mechs always tick so the FX drives+renders+cleans via the weaponbolt path. "
        "Emits [FX_FORCE_SPAWN v1] event=mech_fire (stdout -> needs MC2_LOG=1 to see). "
        "=0/unset disables; zero cost unset."
    },
    // FX-DEFS-SIDECAR-1: EffectDef JSON overlay (VFX-MODERNIZATION-PROPOSAL-1 slice 1)
    {
        "MC2_FX_DEFS",
        "MC2_FX_DEFS",
        EnvVarKind::Feature,
        false,
        "FX-DEFS-SIDECAR-1: per-effect JSON sidecar overlay for the gosFX spec catalog "
        "(mc2::particles::SpecLibrary). Default-OFF; when unset, code/mechcmd2.cpp skips "
        "the overlay entirely and the loaded mc2.fx blob is byte-identical to stock. "
        "When =1, after gosFX::EffectLibrary::Instance->Load() completes, "
        "mc2fxdefs::EffectDefRegistry::instance().applyAll() overlays every "
        "data/effects/defs/<EffectName>.fxdef.json (+ the active mod's own "
        "data/effects/defs/, MC2_ACTIVE_MOD, mod wins on dup key -- mclib/fx_def_registry."
        "{h,cpp}) onto the matching spec found via SpecLibrary::Find (case-insensitive "
        "name match, same as EffectLibrary::Find). v1 overlay fields: disabled (forces "
        "lifeSpan to 0, functional no-op -- SpecLibrary's array has no public remove), "
        "texture (rebinds spec->m_state via MLRTexturePool::Add/lookup, the exact call "
        "MLRState::Load itself makes when parsing a texture name off the stream), blend "
        "(additive=OneOneMode / alpha=AlphaInvAlphaMode via MLRState::SetAlphaMode), and "
        "curves.{alpha,red,green,blue,scale,lifeSpan} (constant-only overrides, collapses "
        "the age curve to a flat value -- mirrors tools/mc2fx's patch.json 'constant' "
        "tier). Unknown top-level JSON keys and reserved v2 keys (flipbook/erosion/"
        "distortion/light) are tolerated for forward-compat; unrecognized 'curves' "
        "sub-keys and unresolved effect names are logged [FXDEF]-prefixed to stderr and "
        "skipped, never fatal. Per-effect failure never blocks other defs or blocks the "
        "level load. No gameplay/emission/lifetime/timing semantics change beyond the "
        "authored curve values themselves (same knobs tools/mc2fx already edits offline); "
        "this just moves the sparse named-effect override in-engine and per-mod-additive. "
        "See docs/modding-effects.md and .claude/VFX-MODERNIZATION-PROPOSAL-1.md §3.1."
    },
    // PPC / direct-fire projectile knobs (gameplay; launcher-toggleable)
    {
        "MC2_PROJECTILE_SPEED_MULT",
        "MC2_PROJECTILE_SPEED_MULT",
        EnvVarKind::Feature,
        false,
        "Direct-fire projectile speed multiplier (PPC/autocannon/gauss = non-arc "
        "bolts). Default 1.0 (stock); clamped (0,10]. ~2.0 = a good snappier feel. "
        "Excludes arcEffect (LRM) bolts. Scales velMag in WeaponBolt::update "
        "(code/weaponbolt.cpp); the de-curve lead uses the same factor. Hit/miss is "
        "pre-rolled and damage applies on proximity, so speed does not change "
        "outcomes. Launcher-toggleable via SetEnvironmentVariableA."
    },
    {
        "MC2_DIRECT_FIRE_STRAIGHT",
        "MC2_DIRECT_FIRE_STRAIGHT",
        EnvVarKind::Feature,
        false,
        "Direct-fire de-curve: PPC/autocannon/gauss bolts fly STRAIGHT instead of "
        "re-tracking the moving target every frame (the janky homing curve). Leads "
        "the aim once at spawn by target velocity over flight time so a pre-rolled "
        "hit still lands. LRM (arcEffect) + beams keep tracking. Default-off. "
        "code/weaponbolt.cpp. Launcher-toggleable."
    },
    {
        "MC2_FX_COST_SPLIT",
        "MC2_FX_COST_SPLIT",
        EnvVarKind::Feature,
        false,
        "FX cost-split instrumentation (default-off). RDTSC buckets over the gosFX "
        "particle/tube path (forces/integrate/birth, animate, bridge flush) -> "
        "[FX_COST v1] stderr summary every 600 frames. "
        "mclib/fx_trace/fx_cost_split.*. Zero cost unset."
    },
    {
        "MC2_TRACE_IFACE_COST_SPLIT",
        "MC2_IFACE_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "MISSION-INTERFACE-PERF-1: chrono cost-split of MissionInterfaceManager::"
        "update() (the GameLogic.Mission.Interface Tracy zone) incl. ControlGui "
        "sub-phases (pauseWnd/btnHover/rosterScan/moverState/tacMap/infoWnd/"
        "vehicleTab/fgBar). Emits one [IFACE_PERF v1] line per 900 frames + at "
        "mission end. Superset of MC2_MIF_SPLIT (either env enables both). "
        "code/missiongui.cpp + code/controlgui.cpp. Zero cost unset."
    },
    {
        "MC2_PICK_FALLBACK_COARSE",
        "MC2_PICK_FALLBACK_COARSE",
        EnvVarKind::Feature,
        false,
        "MISSION-INTERFACE-PERF-1: coarse-to-fine closest-vertex fallback in "
        "Camera::inverseProject (mclib/camera.cpp). The off-map fallback is the "
        "PRODUCTION ground picker when the quadList is empty (LOD-chunk terrain); "
        "legacy code brute-force-projects ALL realVerticesMapSide^2 vertices per "
        "cache-miss frame (~890us on mc2_24, 85-95% of Mission.Interface). ON = "
        "stride-8 coarse pass + full-res +/-9 refine, same nearest-projected-"
        "vertex answer in practice. With MC2_PICK_CAP_TRACE=1 every 32nd walk "
        "re-runs the brute force and logs [PICK_FALLBACK] parity. Default OFF."
    },
    {
        "MC2_XFORM_PARITY_FATAL",
        "MC2_XFORM_PARITY_FATAL",
        EnvVarKind::Trace,
        false,
        "XFORM-CONVENTION-HARNESS-1: promote the F1-3C clip-space parity probe "
        "(code/gamecam.cpp) to a HARD invariant. When =1 AND the ViewUniforms "
        "UBO worldToClipGL diverges from the legacy terrain MVP (max_diff>1e-5), "
        "abort() instead of only logging [VIEW_UNIFORMS v1] ok=0. Default-OFF = "
        "byte-identical to log-only behavior (zero cost unset). Guards against "
        "silent matrix space/handedness/reverse-Z/w-sign regressions; the GL-free "
        "host counterpart is tests/unit/test_xform_convention.cpp."
    },
    {
        "MC2_GPUBUF_RING",
        "MC2_GPUBUF_RING",
        EnvVarKind::Feature,
        false,
        "GPU-BUFFER-WRAPPER-TIER0-HUD-1: route the fixed-capacity gosMesh HUD/2D "
        "meshes (gameos_graphics.cpp draw/drawIndexed) through a 3-frame "
        "persistent-coherent FENCED ring (utils/gpu_ring_buffer.h) instead of the "
        "per-batch glBufferData orphan. Pilot of the GpuRingBuffer<N> shape for "
        "Vulkan-readiness. Default-OFF => legacy orphan path, byte-identical GL "
        "stream + output. ON => the [GPUBUF v1] hud orphan count drops to ~0 (ring "
        "memcpys into a coherent map). Only the self-contained orphan-then-draw "
        "gosMesh methods are routed; uploadBuffers() (terrain) stays on legacy."
    },
    {
        "MC2_GPUBUF_RING_FORCE_WRAP",
        "MC2_GPUBUF_RING_FORCE_WRAP",
        EnvVarKind::Trace,
        false,
        "GPU-BUFFER-WRAPPER-TIER0-HUD-1 NEGATIVE TEST: deterministically advance "
        "the HUD ring twice (beginFrame without an intervening endFrame) so the "
        "one-endFrame-per-beginFrame / fence-per-frame invariant trips. Proves the "
        "safety guard is live (logs [GPUBUF v1] event=begin_without_end; aborts "
        "under a debug build). Requires MC2_GPUBUF_RING=1. Default-OFF; never set "
        "in production."
    },
    {
        "MC2_GPUBUF_RESIDENCY",
        "MC2_GPUBUF_RESIDENCY",
        EnvVarKind::Trace,
        false,
        "GPU-BUFFER-WRAPPER-TIER0-HUD-1: every ~300 frames list the live HUD ring "
        "buffers ([GPUBUF v1] residency tag/kind/frames/vbBytes/ibBytes/liveFences). "
        "Diagnostic only; default-OFF."
    },
    {
        "MC2_GPUBUF_LIGHT_GROWONCE",
        "MC2_GPUBUF_LIGHT_GROWONCE",
        EnvVarKind::Feature,
        false,
        "LIGHT-GROW-ONCE-SUBDATA-1 (AMD-safe intermediate for the light SSBO orphan "
        "churn; NOT the full GpuStorageRing ring). Keeps the single persistent light "
        "SSBO (same handle, slot 20 binding, std430 layout/shader contract) but stops "
        "the per-frame FULL glBufferData orphan re-spec (gameos_graphics.cpp "
        "gos_LightDataSsbo_Upload/_UploadSplit): per frame uploads ONLY the live used "
        "bytes via glBufferSubData; grows the GL buffer ONLY when used bytes exceed "
        "capacity (rare; +128-record headroom matching the CPU backing step; grow "
        "drains via glFinish before delete/recreate/rebind, logged once). Default-OFF "
        "=> the existing LIGHTSSBO-ORPHAN-1 path runs UNCHANGED (byte-identical GL "
        "stream + output; kill switch). ON => [GPUBUF v1] light orphan bytes drop to "
        "~0 (only rare grow frames emit a glBufferData). NVIDIA is a HARD BLOCKER "
        "before default-on: SubData into the in-flight whole-frame/cross-phase light "
        "buffer STALLS on NVIDIA (the stall the orphan dodged); tolerated on AMD."
    },
    {
        "MC2_LIGHT_PREFIX_GPU_COPY",
        "MC2_LIGHT_PREFIX_GPU_COPY",
        EnvVarKind::Feature,
        false,
        "LIGHT-PREFIX-GPU-COPY-1 (TXMMGR-PERF-EASYWINS-1): keep a VRAM stash of the "
        "immutable static light prefix [0..S) and glCopyBufferSubData it into the "
        "freshly-orphaned slot-20 light SSBO each frame instead of re-pushing it over "
        "PCIe; only the dynamic suffix goes through glBufferSubData. Fixes the "
        "STATIC_LIGHT_UPLOAD_SPLIT prefix-skip that LIGHTSSBO-ORPHAN-1 defeated "
        "(measured light_upload ~950us/frame on mc2_24 = ~9.7MB/frame). NVIDIA-safe "
        "by the same orphan discipline: writes only into the fresh store; the stash "
        "is read-only except on prefixDirty (bake/re-bake) frames. Default-OFF "
        "pending soak; unset/=0 -> ORPHAN-1 path byte-identical. Requires "
        "MC2_STATIC_LIGHT_UPLOAD_SPLIT + MC2_LIGHTBAKE (both default-ON); subsumed "
        "by MC2_GPUBUF_LIGHT_GROWONCE when set."
    },
    {
        "MC2_SHADOW_CASTER_CULL_CACHE",
        "MC2_SHADOW_CASTER_CULL_CACHE",
        EnvVarKind::Feature,
        false,
        "SHADOW-CASTER-CULL-CACHE-1 (TXMMGR-PERF-EASYWINS-1): reuse the "
        "Shadow.CasterCull result (mclib/txmmgr.cpp DynamicShadowPass) across "
        "frames when BOTH the static-prop registry generation and the 16-float "
        "dynamic light-space matrix are unchanged (bit-compare). The cull is a "
        "pure function of (caster set, matrix, margin); margin/includeBldg are "
        "session-static. Camera motion or any registry mutation recomputes "
        "exactly as before. Default-OFF pending soak; unset/=0 -> per-frame "
        "recompute (byte-identical)."
    },
    {
        "MC2_RENDERLISTS_COST_SPLIT",
        "MC2_RENDERLISTS_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "TXMMGR-PERF-EASYWINS-1: coarse per-phase CPU cost split of "
        "MC_TextureManager::renderLists() (mclib/txmmgr.cpp). Emits one "
        "[RENDERLISTS_COST v1] stderr summary every 60 frames with per-frame mean "
        "us per phase (preamble/light_upload/sp_registry_flush/dyn_shadow/"
        "sp_batcher_flush/...) + total + self (unattributed). Smoke-visible "
        "complement of the Tracy zones. Default-OFF = zero overhead beyond one "
        "cached-bool test per phase."
    },
    // SPIRV-CONSUMER-PILOT-BUILD-1: runtime SPIR-V consumer (postprocess pilot).
    {
        "MC2_SHADER_SPIRV",
        "MC2_SHADER_SPIRV",
        EnvVarKind::Feature,
        false,
        "SPIRV-CONSUMER-PILOT-BUILD-1: DEFAULT-OFF. =1 loads the offline-baked "
        "OpenGL SPIR-V (OFFLINE-SHADER-VARIANT-BUILD-1, shaders/spv/) for the "
        "postprocess composite vert/frag pilot via glShaderBinary + "
        "glSpecializeShader(\"main\") instead of runtime GLSL compile. Gated on "
        "GLEW_ARB_gl_spirv||GL 4.6 (the 4.3 context request does not imply "
        "support). Falls back to GLSL on: extension absent, artifact missing, "
        "specialize failure, or MC2_SHADER_HOT_RELOAD=1 (dev loop stays GLSL). "
        "Pilot allowlist only (postprocess); all other shaders stay GLSL."
    },
    {
        "MC2_SHADER_SPIRV_FATAL",
        "MC2_SHADER_SPIRV_FATAL",
        EnvVarKind::Trace,
        false,
        "SPIRV-CONSUMER-PILOT-BUILD-1: DEFAULT-OFF debug gate. =1 makes a bad "
        ".spv (glShaderBinary reject / specialize failure) assert/abort instead "
        "of falling back to GLSL — used to surface artifact corruption in "
        "testing. Default-OFF = silent-but-logged GLSL fallback."
    },

    // -------------------------------------------------------------------
    // ENV-REGISTRY-LEGACY-SWEEP-1: promotions from the legacy-var registry
    // sweep (scripts/check-env-registry.sh ALLOWLIST). These are genuinely
    // active feature gates pulled out of the allowlist into the real
    // registry; see scripts/check-env-registry.sh history for the other
    // ~355 vars that stayed allowlisted (trace/override/infra/parity/legacy).
    // -------------------------------------------------------------------
    {
        "MC2_FEATURE_ANIM_CADENCE_FIX",
        "MC2_ANIM_CADENCE_FIX",
        EnvVarKind::Feature,
        true,
        "ANIM-CADENCE-FIX: advance mech gait at most once per render frame; prevents a visible double-step when Mover::getLOSPosition() re-invokes appearance->update() the same frame. Default-ON (user-confirmed fix, mc2_17 Catapult/Bushwacker); =0 disables to A/B the double-step."
    },
    {
        "MC2_FEATURE_ASSIMP_MECH_IMPORT",
        "MC2_ASSIMP_MECH_IMPORT",
        EnvVarKind::Feature,
        false,
        "BT2018 mech import 1A: route mech geometry through the Assimp GLTF/FBX importer instead of the legacy MSL loader. Default-OFF; =1 enables."
    },
    {
        "MC2_FEATURE_BRAIN_RUNTIME",
        "MC2_BRAIN_RUNTIME",
        EnvVarKind::Feature,
        false,
        "TechBrain declarative-brain runtime master gate (warrior.cpp). Default-OFF; =1 enables. Companion MC2_BRAIN_RUNTIME_APPLY gates whether resolved intents are actually applied."
    },
    {
        "MC2_FEATURE_BRAIN_RUNTIME_APPLY",
        "MC2_BRAIN_RUNTIME_APPLY",
        EnvVarKind::Feature,
        false,
        "TechBrain runtime apply gate (warrior.cpp). Default-OFF; requires MC2_BRAIN_RUNTIME=1. =1 applies resolved brain intents instead of only computing them."
    },
    {
        "MC2_FEATURE_BRAIN_DISPATCH",
        "MC2_BRAIN_DISPATCH",
        EnvVarKind::Feature,
        false,
        "TechScript special-dispatch master gate (mission.cpp/brain_special_dispatch.cpp). Default-OFF; requires MC2_BRAIN_RUNTIME=1 + MC2_BRAIN_RUNTIME_APPLY=1."
    },
    {
        "MC2_FEATURE_SMART_LOAD",
        "MC2_SMART_LOAD",
        EnvVarKind::Feature,
        true,
        "SMART-LOAD startup/mission-load perf path (logisticsdata.cpp). **DEFAULT ON; kill-switch MC2_SMART_LOAD=0.** GATE-DEFAULT-TRUTH-1 (2026-07-30) corrected this entry, which read \"Default-OFF; =1 enables\" while code/logisticsdata.cpp:44-48 does `if (!v) return true; // default ON`. Found by scripts/check-gate-default-agreement.py on its first run. MC2_SMART_LOAD_TRACE (allowlisted trace) logs the load-time breakdown."
    },
    {
        "MC2_FEATURE_DYNAMIC_DECALS",
        "MC2_DYNAMIC_DECALS",
        EnvVarKind::Feature,
        false,
        "Dynamic decal ring (mclib/dynamic_decal_ring.cpp): spawn()/gatherToDecalBatch() are no-ops when off. Default-OFF; =1 (or any non-'0' value) enables."
    },
    {
        "MC2_FEATURE_UNIT_PROFILE_DATA",
        "MC2_UNIT_PROFILE_DATA",
        EnvVarKind::Feature,
        false,
        "UNIT-PROFILE-ARC: data-driven unit+equipment definition source (code/unitprofile.cpp). Default LEGACY (var unset/'0' = legacy path); =1 (or any non-'0' value) switches to the generated UnitProfileData source."
    },
    {
        "MC2_FEATURE_FRAMEGRAPH_EXECUTOR",
        "MC2_FRAMEGRAPH_EXECUTOR",
        EnvVarKind::Feature,
        false,
        "RENDER-FRAME-GRAPH arc: top-level frame-graph executor ownership of pass begin/end (mclib/render_contract.cpp, GameOS/gameos/gos_postprocess.cpp). Default-OFF = legacy direct call order, byte-identical; =1 routes through the executor. See docs/renderworld_arc_status.md."
    },
    {
        "MC2_FEATURE_RENDER_PASS_ORDER",
        "MC2_RENDER_PASS_ORDER",
        EnvVarKind::Feature,
        false,
        "CONTRACT-3: per-frame resource-ordering audit (mclib/render_contract.cpp) — checks each pass's reads[] are satisfied by a prior write before beginPass. Default-OFF; =1 enables. MC2_RENDER_PASS_TELEMETRY (allowlisted trace) shares the endPass logging."
    },
    {
        "MC2_FEATURE_TERRAIN_LOD_CHUNK",
        "MC2_TERRAIN_LOD_CHUNK",
        EnvVarKind::Feature,
        true,
        "LOD-chunk terrain renderer (mclib/terrain.cpp/terrain.h), the primary tessellation-free terrain path. Default-ON (reworked/stabilized); =0 opts out to the legacy path (increasingly vestigial per terrain.cpp:149 warning)."
    },
    {
        "MC2_FEATURE_SHADOW_CSM",
        "MC2_SHADOW_CSM",
        EnvVarKind::Feature,
        true,
        "Cascaded shadow maps master gate (GameOS/gameos/gos_postprocess.cpp), reworked 2026-06-18 (commit 8ff13a36). Default-ON; =0 opts out to the legacy single dynamic shadow map path. See docs/tier1_env_vars.md \"Dynamic CSM\"."
    },
    {
        "MC2_FEATURE_TERRAIN_VISUAL_HEIGHT",
        "MC2_TERRAIN_VISUAL_HEIGHT",
        EnvVarKind::Feature,
        false,
        "TERRAIN-VISUAL-HEIGHT: loads the 4x-res VISUAL heightfield bake into an SSBO for corner-pinned interior displacement (mclib/terrain.cpp). Default-OFF = no load, no SSBO, byte-identical; =1 (or MC2_TERRAIN_VISUAL_DISPLACE=1, which implies load) enables. MC2_TERRAIN_VISUAL_HEIGHT_FILE (allowlisted override) picks an explicit bake path."
    },
    {
        "MC2_FEATURE_MOVE_RECON",
        "MC2_MOVE_RECON",
        EnvVarKind::Feature,
        false,
        "Pathfinding cost instrumentation (mclib/move_recon.cpp). Default-OFF, zero behavior change when unset. MC2_MOVE_CHUNK_SHADOW / MC2_MOVE_PATH_CACHE_SHADOW (allowlisted) are alternate enables for the same instrumentation."
    },
    {
        "MC2_FEATURE_STATIC_PROXY_RECON",
        "MC2_STATIC_PROXY_RECON",
        EnvVarKind::Feature,
        false,
        "StaticSceneProxy-arc recon instrumentation (mclib/bdactor.cpp) — counts touchSerialCommit calls for building/tree appearances ahead of the per-frame static-prop-walk meta-fix. Default-OFF; presence-gated."
    },
    {
        "MC2_FEATURE_RENDER_BACKEND_IFACE",
        "MC2_RENDER_BACKEND_IFACE",
        EnvVarKind::Feature,
        false,
        "RENDER-BACKEND-SEAMS arc: routes select GL calls (e.g. scene FBO bind) through the GLBackend seam instead of direct GL. Default-OFF = direct GL, byte-identical; ON exercises the same GL call through the seam (same output, proves the seam routes). See GameOS/gameos/gos_postprocess.cpp."
    },
    {
        "MC2_FEATURE_STATIC_UPDATE_SKIP",
        "MC2_STATIC_UPDATE_SKIP",
        EnvVarKind::Feature,
        true,
        "Skip the per-frame static building/tree update walk when nothing is dirty (GameOS/gameos/gameosmain.cpp, code/terrobj.cpp, code/bldng.cpp). Default-ON (perf); =0 restores the legacy always-walk behavior."
    },
    {
        "MC2_FEATURE_VULKAN_SWAPCHAIN_PRESENT",
        "MC2_VULKAN_SWAPCHAIN_PRESENT",
        EnvVarKind::Feature,
        false,
        "VULKAN-CONTRACT-MANIFEST-ARC Layer 5: engine-owned Vulkan swapchain (create/present/resize) (GameOS/gameos/vulkan_backend_skeleton.cpp, vulkan_swapchain_present.cpp). Default-OFF, presence-gated (=1 enables); validation-clean core+sync per L5 proof. See MEMORY handoff HANDOFF_2026_07_01_vulkan_lib_track."
    },
    {
        "MC2_FEATURE_HUD_READABILITY_V1",
        "MC2_HUD_READABILITY_V1",
        EnvVarKind::Feature,
        false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice A: data-first HUD readability. At the single font-resolve choke point (gui/afont.cpp aFont::loadFont) substitute the next-size-up retail D3F atlas for the worst-offender in-mission HUD fonts (help/objectives/info-window/components/vehicle-tab/pause/hotkey overlay); also brightens the disabled vehicle-tab label color (code/controlgui.cpp renderVehicleTab, stock 0xff7f7f7f gray-on-dark). No layout/hit-test change. Default-OFF => byte-identical."
    },
    {
        "MC2_FEATURE_HUD_IMGUI_TABS",
        "MC2_HUD_IMGUI_TABS",
        EnvVarKind::Feature,
        false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C: data-driven command-bar tab registry (code/hudtabregistry.{h,cpp} + wiring in code/controlgui.cpp). Render/update dispatch, help text, isOverTacMap, click mutual-exclusion and the TAB-hotkey cycle route through the registry; the stock 3 tabs keep their legacy ControlButton chrome with synced PRESSED bits. Shares slice B's pin-to-legacy. Default-OFF => every legacy branch runs unchanged (registry registered but never consulted)."
    },
    {
        "MC2_FEATURE_HUD_TABS_TEST",
        "MC2_HUD_TABS_TEST",
        EnvVarKind::Feature,
        false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C witness: register a 4th 'test' tab (no legacy button; ImGui chrome via HudImGui::hudShrinkPoint + hudToDisplay, trivial ImGui body over the tac area). Only visible when MC2_HUD_IMGUI_TABS is also set — the modder's worked example, left in deliberately. Default-OFF."
    },

    // ENV sweep 2026-07-09 (checker regex widen: digit names + envFlagDefaultOn helper)
    {
        "MC2_FEATURE_GPU_MECH_CULL",
        "MC2_GPU_MECH_CULL",
        EnvVarKind::Feature,
        true,
        "Slice C1 (gos_mech_batcher.cpp): render-only mech GPU cull -- skip GPU mech submit for actors the GPU lagged-readback says were invisible last frame (CPU fallback/AI/updateGeometry still run). Default-ON kill-switch (envFlagDefaultOn); =0 opts out."
    },
    {
        "MC2_FEATURE_GPU_MECH_FAST_TRANSFORM",
        "MC2_GPU_MECH_FAST_TRANSFORM",
        EnvVarKind::Feature,
        true,
        "Slice C3-revised (gos_mech_batcher.cpp): body-only fast transform path for GPU-driven mechs. Default-ON kill-switch (envFlagDefaultOn); =0 opts out to the pre-slice transform path."
    },
    {
        "MC2_FEATURE_GPU_MECH_SHADOW_FAST_TRANSFORM",
        "MC2_GPU_MECH_SHADOW_FAST_TRANSFORM",
        EnvVarKind::Feature,
        true,
        "Slice C3-shadow (gos_mech_batcher.cpp): fast-transform variant for the mech shadow-caster callsite. Default-ON kill-switch (envFlagDefaultOn); =0 opts out."
    },
    {
        "MC2_FEATURE_GPU_MECH_LEAF_SKIP",
        "MC2_GPU_MECH_LEAF_SKIP",
        EnvVarKind::Feature,
        true,
        "Slice D-leaf-skip-v2 (gos_mech_batcher.cpp): strip per-leaf body of mechShape->TransformMultiShape* work when the leaf is provably unaffected. Default-ON kill-switch (envFlagDefaultOn); =0 opts out."
    },
    {
        "MC2_TRACE_LOAD_RACE_DELAY_ATLAS",
        "MC2_LOAD_RACE_DELAY_ATLAS",
        EnvVarKind::Trace,
        false,
        "gos_terrain_indirect.cpp negative control for the mission-load residency-race probes: when set (any value), BuildColormapAtlas() defers the real GL atlas upload until scenarioTime >= 30s, making the colormap atlas genuinely absent for ~30s so reviewers can confirm the LOAD_RACE atlas/texel probes actually fire. Diagnostic/test-only; unset -> no effect."
    },
    {
        "MC2_TRACE_LOAD_RACE_PROBE_FRAMES",
        "MC2_LOAD_RACE_PROBE_FRAMES",
        EnvVarKind::Trace,
        false,
        "gameosmain.cpp: overrides the comma-separated list of frame numbers at which the mission-load residency-race one-shot diagnostics probe fires (see MC2_LOAD_RACE_TRACE). Unset -> falls back to the confirmed transient window default."
    },
    // GitHub-issue fix batch 2026-07-16 (issues #42a, #50, #34)
    {
        "MC2_TRACE_SOAK_SALVAGE_TRADEMOVER",
        "MC2_SOAK_SALVAGE_TRADEMOVER",
        EnvVarKind::Trace,
        false,
        "ISSUE-42a repro fixture (code/mission.cpp soak harness): with MC2_SOAK_AUTOWIN + MC2_SOAK_KILL_ENEMY, exercises the REAL mid-mission salvage-craft flow (recover/setStatus(SHUTDOWN)/tradeMover/orderPowerUp) on one disabled enemy mech before autowin, so the Phase-3 load-bay check verifies LogisticsData::setMissionCompleted's recovered-mech carry-over branch. Default OFF = byte-identical."
    },
    {
        "MC2_TRACE_LINKUP",
        "MC2_LINKUP_TRACE",
        EnvVarKind::Trace,
        false,
        "ISSUE-50 diagnostics (code/mech.cpp BattleMech::update): for every off-GUI mech, periodically (~1/s) logs [LINKUP] part/name/status/los, plus [LINKUP] linkup-complete when the link-up fires — makes the link-up latch (hidden reinforcement mech never appearing) observable in the field. Default OFF = no output, no behavior change."
    },
    // GitHub-issue fix batch 2026-07-16, part 2 (issues #9, #17)
    {
        "MC2_FEATURE_CALLED_SHOT_FIX",
        "MC2_CALLED_SHOT_FIX",
        EnvVarKind::Feature,
        false,
        "ISSUE-9 called-shot effectiveness (gameplay gate, default OFF = stock): (S1) Mover::calcAttackChance floors the aimed-shot modifiers at Head 0.30 / Arm 0.45 / Leg 0.50 (stock gamesys.fit 0.08/0.15/0.20 leaves head calls at the 5% clamp); (S2) a successful AIMED area-effect (LRM) hit delivers its located damage to the called location (weaponbolt.cpp deliverAimedAreaHit, single-player only) with the splash explosion kept cosmetic, instead of stock's randomly-located explosion damage."
    },
    {
        "MC2_TRACE_AIMED_FIRE_FIXTURE",
        "MC2_AIMED_FIRE_FIXTURE",
        EnvVarKind::Trace,
        false,
        "ISSUE-9 headless probe (code/warrior.cpp MechWarrior::updateActions): every home-team mech without a combat order is issued an aimed-HEAD attack (orderAttackObject, COMMANDER origin — the only origin that installs the tac order and thus the aim) at the nearest live enemy mech, re-issued as targets die (2 s global throttle). Pair with MC2_DIAG_TAGS=COMBAT to capture the hit-location distribution (\"loc\" field) for MC2_CALLED_SHOT_FIX A/B. Default OFF = byte-identical."
    },
    {
        "MC2_TRACE_VK_SHADOW_MATRIX_DIAG",
        "MC2_VK_SHADOW_MATRIX_DIAG",
        EnvVarKind::Trace,
        false,
        "VK shadow producer/consumer evidence (default OFF = zero cost; GameOS/gameos/vulkan_shadow_island_live.cpp). At shadow-pass tick 600 and 1200 logs [VK_SHADOW_MATRIX v1] lines: GL dynamic + CSM cascade matrices, the matrices the VK island received per map, and the UBO matrices the VK terrain shader samples with (16 floats, raw memory order, plus map sizes). At tick 600 also dumps vk_shadowdiag_<dyn|csm0|full>_<gl|vk>.png (normalized grayscale depth) into the working directory. Diagnostic only; no behavior change."
    },
    {
        "MC2_TRACE_FIRE_DECISION",
        "MC2_FIRE_DECISION_TRACE",
        EnvVarKind::Trace,
        false,
        "ISSUE-17 fire-refusal instrumentation (default OFF = byte-identical; needs MC2_DIAG_TAGS=...,COMBAT). (1) code/warrior.cpp combatDecisionTree: when a combat order is active and NO weapon fired this combat update, emits one 'nofire' JSONL event with per-gate tallies (rd/am/rn/lk/ch/hot from calcWeaponsStatus), a direct lineOfSight probe (splits LOS from fire-arc within NOT_LOCKED), order code, weaponsStatusResult and distance. (2) code/mech.cpp fireWeapon: emits a 'firegate' event (wid/widx/site) at each silent non-zero early return (status/body/ready/terrain/dead_target/no_target/range/shots/node_recycle) — catches 'decision said fire but weapon silently skipped'. NO behavior change."
    },
    // META-FIX-BATCH-1 (recurrence-proofing, designs C + E — 2026-07-16)
    {
        "MC2_TRACE_SPLIT_PATH_PARITY",
        "MC2_SPLIT_PATH_PARITY_TRACE",
        EnvVarKind::Trace,
        false,
        "META-FIX design C runtime tier (GameOS/gameos/gameos_graphics.cpp gosRenderer::init): =1 emits machine-parseable [SPLIT_PATH_PARITY] marker lines at split-path parity checkpoints. First checkpoint: terrain material load — tess=<0|1> shadow=<0|1> match=<0|1>, where match asserts the ISSUE-34 decouple invariant (modern shadow path stays active whether tess is on, killswitched, or compile-failed). run_smoke's marker-bucket row `split_path_parity` (scripts/smoke_lib/logparse.py MARKER_BUCKETS) turns any match=0 into a red run; the nightly MC2_TERRAIN_TESS=0/1 A/B leg pairs with this gate. Default OFF = static-getenv latch, byte-identical."
    },
    {
        "MC2_TRACE_LOG_FILE_RESOLVE",
        "MC2_LOG_FILE_RESOLVE",
        EnvVarKind::Trace,
        false,
        "META-FIX design E tier 3 (mclib/file.cpp, parsed at InitModSearchPaths): file-resolution ladder diagnostics. =1: level-1 stdout tags ([mod]/[mod-miss]) plus a shutdown summary line '[file-resolve] opens=N misses=M'. =2: level 1 plus a JSONL resolve trace (one record per resolved open: layer in {mod:<id>, base-loose, base-strip, fastfile, cd, MISS}) written to MC2_RESOLVE_TRACE_FILE; fileExists() full-ladder misses also emit MISS records (probe API — the benign-probe baseline of scripts/check-file-resolve-miss.py absorbs them). Promoted from the check-env-registry allowlist in META-FIX-BATCH-1. Default OFF/0 = byte-identical."
    },
    {
        "MC2_TRACE_RESOLVE_TRACE_FILE",
        "MC2_RESOLVE_TRACE_FILE",
        EnvVarKind::Trace,
        false,
        "META-FIX design E tier 3 (mclib/file.cpp): output path for the MC2_LOG_FILE_RESOLVE=2 JSONL resolve trace, opened for append at InitModSearchPaths (relative paths resolve against the game CWD = exe dir). Only consulted at level >= 2. Consumed by the nightly scripts/check-file-resolve-miss.py MISS audit. Promoted from the check-env-registry allowlist in META-FIX-BATCH-1."
    },
    // META-FIX-BATCH-2 (recurrence-proofing, designs D + F — 2026-07-16)
    {
        "MC2_TRACE_RES_ASSERT",
        "MC2_RES_ASSERT",
        EnvVarKind::Trace,
        false,
        "META-FIX design D runtime tier (RenderCore/ResolutionContract.h): resolution/aspect contract audit. unset/0 = off (default; one static bool test, tier1 byte-identical); 1/log = one [RES_ASSERT v1] stdout marker line per violation site; fatal = abort() on violation (nonzero exit = red smoke matrix). Chokepoints: (i) Camera::setOrthogonal projection-aspect==drawable-aspect unless MC2_CAMERA_ASPECT_NATIVE=0 or mech-preview FBO (issue #45 class); (ii) per-frame HUD-clamp 800x600 invariant + drawable freshness (3-frame grace) + #49 windowed-size oracle (gameosmain.cpp draw_screen); (iii) flushHUDBatch HUD-canvas-box-in-drawable + forward-remap∘gos_HudInverseMousePoint identity within 0.5px. Static sibling: scripts/check-res-aspect-contract.py; smoke sibling: tests/smoke/matrices/resolution.json."
    },
    {
        "MC2_TRACE_FIXTURE_ORDER_LIVENESS",
        "MC2_FIXTURE_ORDER_LIVENESS",
        EnvVarKind::Trace,
        false,
        "META-FIX design F behavioral fixture (code/mission.cpp soak namespace; requires MC2_SOAK_AUTOWIN): at 20% of the autowin timer every armed home mech gets a COMMANDER-origin unaimed ATTACK_TO_DESTROY on the nearest live enemy (PLAYER origin never installs the tac order — the trap that burned the first aimed-fire fixture run); at 50% every home mech is issued a COMMANDER-origin move order (300 m toward map center — replaces the attack order, giving a clean 0.4*win displacement window; attack-first ordering is deliberate: the reverse self-sabotages the move bound when targets spawn in range); at 90% one self-asserting 'ORDER_LIVENESS_SUMMARY issued_move=N moved=N refused=N stalled=N verdict=PASS|FAIL' line (moved = net displacement >= 25 m from move-issue; refused = classified LastMoveCalcErr; dead units exempt). [ORDER_FIXTURE] lines mark each issued order. run_smoke.py treats a missing summary or verdict=FAIL as a hard fail ONLY when this var is set in the runner env (VEG-SMOKE-FLOOR activation pattern). Consumed by scripts/check-order-liveness.py. Default OFF = byte-identical."
    },
    // META-FIX-BATCH-3 (recurrence-proofing, designs A + B — 2026-07-16)
    {
        "MC2_TRACE_MOVER_COHERENCE_AUDIT",
        "MC2_MOVER_COHERENCE_AUDIT",
        EnvVarKind::Trace,
        false,
        "META-FIX design A runtime tier (code/mission.cpp auditMoverCoherence, ~1 Hz): read-only audit that the five per-mover registries (isOnGUI render gate, force-bar roster, selection, tacmap blip, commandability) agree for home-commander SP movers. Rules: I1 live onGUI mover holds a force-bar icon (I1b bar-full carve-out = ADVISORY — addMech silently drops the 17th in release); I2 off-GUI mover has no icon and no PLAYER-origin tac order (SELF/COMMANDER origin = ADVISORY I2b: auto-powerup + brain orders are legitimate); I3 disabled-not-destroyed keeps getAppearance()!=NULL (#42b-ADJACENT); I4 every icon unit is ObjectManager-registered (identity scan, read-only); I5 selected => onGUI (#50 class). Emits deduped '[MOVER_COHERENCE] VIOLATION|ADVISORY rule=...' stdout lines (mirrored to the diag sink) + a process-exit summary. POSITION-FREE by contract: off-map staged movers (onGUI=1, authored staging — mc2_10 convoy, INVIS-VEHICLES-FIX-1) are a legitimate state; static rule S6 pins the audit region free of positional terms. run_smoke.py force-sets =1 for --tier runs (read-only; shipping default stays OFF); any VIOLATION line is a red run via the mover_coherence marker bucket. Static sibling: scripts/check-mover-registry-coherence.sh."
    },
    {
        "MC2_TRACE_ASSET_TRACE",
        "MC2_ASSET_TRACE",
        EnvVarKind::Trace,
        false,
        "META-FIX design B PART 0 companion (mclib/msl.cpp loader chokepoint): =1 additionally logs '[ASSET_LOAD v1] event=file_missing ...' for MISSING files at the three shape-load entry points (LoadTGMultiShapeFromASE / LoadBinaryCopy / LoadFromFile). Default OFF because missing OPTIONAL files (arms/shadows/damage shapes/anims) are NORMAL stock degradation — an always-on file_missing line would red-bucket healthy tier1 runs. parse_fail (file present, content bad) and required_missing (tgl_load_guard required-slot check) log ALWAYS regardless of this gate; only those feed the asset_load_fail red bucket. Rate-limited per unique (event,path), cap 64 (mc2AuditOnce)."
    },
    {
        "MC2_FEATURE_ASSET_FAILLOUD",
        "MC2_ASSET_FAILLOUD",
        EnvVarKind::Feature,
        true,
        "META-FIX design B PART 1 (mclib/tgl_load_guard.cpp): default-ON killswitch (MC2_TERRAIN_TESS shape — unset=ON, =0 exact legacy fail-soft). When a REQUIRED base-shape load fails (rc!=0 or GetNumShapes()==0 at the ~17 required slots in mech3d/bdactor/gvactor/genactor), populates a VISIBLE magenta placeholder box with a REAL 4x4 texture registered through mcTextureManager (valid handles, NOT numTextures=0 — the GPU static-prop/mech batchers cull layerForPacket=-1 packets, so a textureless placeholder would itself be invisible). The '[ASSET_LOAD v1] event=required_missing' line + MC2_VERIFY fire + AppearanceType::baseShapeLoadFailed flag happen regardless of this gate. Stock byte-identical: no stock base shape fails, so the branch is cold. =0 restores 'leave the mech without an appearance' (msl.cpp fail-soft) exactly."
    },
    {
        "MC2_FEATURE_ICON_FAILLOUD",
        "MC2_ICON_FAILLOUD",
        EnvVarKind::Feature,
        false,
        "META-FIX design B PART 3 (code/mechicon.cpp iconTileChecked at the three CPU icon-tile blits): default OFF. =1 substitutes a solid MAGENTA tile when an icon index resolves outside the source atlas (last-pixel arithmetic identical to the per-pixel bounds guards — never flags an in-bounds tile). Default keeps the LEGACY wrap/blank semantics, which are load-bearing for MC2X high-index mod content (mechicon.cpp wrap comment) — that is why the substitution is opt-in while the '[ASSET_LOAD v1] event=icon_oob' line is always-on (rate-limited). UV-path sibling: icon_atlas_cell.h cellForIndexChecked (logisticsmechicon/mechlistbox) returns the legacy-computed cell UNCHANGED on OOB. Row bound harness-validated against retail 256x256 / MC2X 512 / MCO 1024-40 in tools/icon_atlas_harness before the icon_oob smoke bucket is armed red."
    },
    {
        "MC2_TRACE_APPEAR_AUDIT",
        "MC2_APPEAR_AUDIT",
        EnvVarKind::Trace,
        false,
        "META-FIX design B PART 4 (code/mission.cpp Mission::start beside mission_phase_mark('mission_ready')): default OFF, OBSERVATIONAL-FIRST. One O(N) pass over registered existing objects at mission_ready: emits '[APPEAR_AUDIT v1] event=missing reason=appearance_null|base_shape_failed ...' (bounded 32 lines) + a 'summary scanned= missing=' line. POSITION-FREE by the off-map-staging carve-out (INVIS-VEHICLES-FIX-1): staged movers have valid appearances; the scan keys on no positional/visibility term. The appear_audit smoke bucket is registered ADVISORY-DISARMED and may only arm red (and the gate join the tier1 force-set) after a full 24-map failed=0 baseline — whether any class legitimately reports NULL appearance at mission_ready is unverified; the tier1-missions baseline ships with META-FIX-BATCH-3, the 24-map run is the documented follow-up."
    },
    {
        "MC2_TRACE_VK_SCENE_GL_PRODUCER_QUERY",
        "MC2_VK_SCENE_GL_PRODUCER_QUERY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps the sceneFBO producer phase in a GL_ANY_SAMPLES_PASSED query and logs "
        "[VK_SCENE_GL_PRODUCER_QUERY v1] any_samples=... so resource-strict work can prove whether any GL scene "
        "producer still rasterizes before attempting to delete shared scene GL interop resources. Default-OFF; "
        "diagnostic only."
    },
    {
        "MC2_TRACE_VK_TERRAIN_GL_FALLBACK_AUDIT",
        "MC2_VK_TERRAIN_GL_FALLBACK_AUDIT",
        EnvVarKind::Trace,
        false,
        "VK terrain ownership diagnostic: under the max-native terrain gates, counts terrain chunks captured by "
        "the Vulkan feed vs chunks that still fall through to GL, bucketed by inactive, uncaptured LOD band, "
        "visual-displace mode, edge-stitch, and other. Logs [VK_TERRAIN_GL_FALLBACK_AUDIT v1]. Default-OFF; "
        "diagnostic only, no render behavior change."
    },
    {
        "MC2_TRACE_VK_SCREENSHADOW_GL_CANARY",
        "MC2_VK_SCREENSHADOW_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps only the legacy GL runScreenShadow fullscreen draw in "
        "GL_ANY_SAMPLES_PASSED and logs [VK_SCREENSHADOW_GL_CANARY v1]. Used after terrain GL fallback "
        "reaches zero to prove whether screen-shadow is the next real sceneFBO GL writer. Default-OFF; "
        "diagnostic only."
    },
    {
        "MC2_TRACE_VK_TERRAIN_OVERLAY_GL_CANARY",
        "MC2_VK_TERRAIN_OVERLAY_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps gosRenderer::drawTerrainOverlays GL draw loop in "
        "GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1] tag=terrain_overlay_batch. "
        "Default-OFF; diagnostic only."
    },
    {
        "MC2_TRACE_VK_DECAL_STATIC_GL_CANARY",
        "MC2_VK_DECAL_STATIC_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps gosRenderer::drawDecalStaticBatch GL draw loop in "
        "GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1] tag=decal_static_batch. "
        "Default-OFF; diagnostic only."
    },
    {
        "MC2_TRACE_VK_TERRAIN_DECAL_GL_CANARY",
        "MC2_VK_TERRAIN_DECAL_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps gosRenderer::drawDecals GL draw loop in "
        "GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1] tag=terrain_decal_batch. "
        "Default-OFF; diagnostic only."
    },
    {
        "MC2_FEATURE_VK_TERRAIN_HEIGHT_AUDIT",
        "MC2_VK_TERRAIN_HEIGHT_AUDIT",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN HEIGHT-AUDIT (verification-only, default OFF; requires MC2_VK_TERRAIN_TIER1). Each frame "
        "re-reads the GL height SSBO (binding 23) and FNV-1a-checksums it, comparing against the checksum "
        "stored at the last version-gated snapshot_height() copy into the VK-owned height buffer. Detects "
        "height-staleness: GL terrain heights mutated (crater/deform) WITHOUT a heightVersion bump "
        "(g_terrainHeightVersion bumps only in UploadHeightFull) -> the VK snapshot silently diverges and "
        "mv_smoothTerrainNormal's macro-slope goes stale. On mismatch logs a LOUD rate-limited "
        "[HEIGHT_AUDIT] MISMATCH line (snapshot_fnv vs live_fnv); NEVER mutates render output. Gate OFF = "
        "no readback, byte-identical. Negative control MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT=1 poisons the "
        "stored checksum once so the audit MUST fire (proves it has teeth)."
    },
    {
        "MC2_FEATURE_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT",
        "MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN HEIGHT-AUDIT NEGATIVE CONTROL (test-only, default OFF; requires "
        "MC2_VK_TERRAIN_HEIGHT_AUDIT). Deliberately XOR-corrupts the stored height snapshot checksum once "
        "after the first snapshot so the per-frame audit compare is GUARANTEED to report a mismatch -> "
        "proves the audit can actually detect divergence (has teeth). NEVER on in production."
    },
    {
        "MC2_TRACE_DIAG_VERBOSE",
        "MC2_DIAG_VERBOSE",
        EnvVarKind::Trace,
        false,
        "QUIET-DEFAULT-BUILD-1: ONE umbrella gate for the engine's undifferentiated developer chatter. "
        "VERIFY-INTEGRITY-3 measured 83 distinct [TAG vN] diagnostic tags printing in a DEFAULT build -- "
        "output a real user sees on a normal launch with no MC2_* env set -- including nine that repeated "
        "every 600 frames forever during play and three that announced features which were switched OFF. "
        "282 call sites across 40 files (GameOS + mclib + code) now route through MC2_DIAGF / MC2_DIAGFE / "
        "MC2_DIAG_PUTS / MC2_DIAG_FPUTS in GameOS/include/mc2_diag_verbose.h, which are no-ops unless this "
        "is set. =1 restores the previous output VERBATIM, so nothing is lost -- it moved behind a switch. "
        "Default-OFF changes no rendering or gameplay behaviour whatsoever: the only effect is whether the "
        "lines are written to stdout/stderr. Read once per translation unit (getenv-seeded static), so it "
        "cannot drift mid-run. Deliberately NOT per-tag: these lines share one purpose, so they share one "
        "switch; anything needing selective or structured control uses the per-subsystem facilities "
        "(MC2_TERRAIN_LOD_CHUNK_DIAG bits, MC2_DIAG_TAGS) instead. Tags a harness parses "
        "(BUILD_FINGERPRINT, INSTR, PERF, TIMING, TGL_POOL, ASSET_SCALE, ASSET_LOAD, RENDER_SNAPSHOT, "
        "QUADSETUP_SKIP, SMOKE) are deliberately EXCLUDED and still print unconditionally -- gating them "
        "would blind verification. Ratchet: scripts/check-default-build-quiet.py."
    },
    // LetterboxOpaque (DETERMINISM-RATCHET-2)
    {
        "MC2_FEATURE_LETTERBOX_OPAQUE",
        "MC2_LETTERBOX_OPAQUE",
        EnvVarKind::Feature,
        true,
        "Cinematic letterbox bars are drawn OPAQUE black; only their EXTENT animates. Default-ON killswitch: "
        "=0 restores the legacy alpha ramp verbatim. Read once at code/gamecam.cpp:841 (getenv-seeded "
        "static), so it cannot drift mid-run. WHY IT EXISTS: during the ~5s swoop-in the legacy code ramped "
        "letterBoxAlpha with the bar extent, so the bars were translucent and the scene showed through. "
        "Measured on mc2_10 night at capture frame 100: extent 0.0986 of 0.15 max => ramp 65.7% => "
        "letterBoxAlpha=168, i.e. 34% transparent. It read as a BROKEN TOP BAR and a fine bottom bar purely "
        "because of what sits behind each: the top bar covers the bright horizon/sky, the bottom covers dark "
        "near-field terrain, so identical translucency is glaring on top and invisible below. The separate "
        "fadeAlpha fade-to-black quad is a real fade and is untouched. WHY THIS ENTRY EXISTS: the gate "
        "shipped UNREGISTERED, UNDOCUMENTED and absent from run_smoke.py's passthrough allowlist, so a smoke "
        "run setting =0 silently ran the =1 path -- an ENV_DROP that voids the verification without saying "
        "so. Registered/documented/allowlisted by DETERMINISM-RATCHET-2. No geometry/objectId/gameplay "
        "change; visual-only, and byte-identical to the shipped build at its default."
    },
    // GATE-HYGIENE-CLOSE-1: static-prop light-bake probe (was legacy-allowlist-escaped).
    {
        "MC2_TRACE_GPU_CULL_STATIC_LIGHT_ZERO_PROBE",
        "MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE",
        EnvVarKind::Trace,
        false,
        "Static-prop light-bake observation gate, default OFF. Drives TWO diagnostics off one env var: "
        "(1) [STATIC_LIGHT_ZERO v1] in gos_static_prop_registry.cpp -- every ~300 registry flushes, counts "
        "alive prop recipes and how many have a permanent light slot with numLights_=0 (never baked); with "
        "MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE also set, zeroLightSlots should reach 0 after mission load. "
        "(2) [BAKEORD v1] in mclib/bdactor.cpp -- one line per real bake (MISS path only) carrying bake "
        "ordinal, pre/post-invalidate timing and sunActive at bake time, the order-vs-time discriminator "
        "for the tree-window light-bake hypothesis. Read-only observation: emits log lines, never alters a "
        "light slot, bake decision or any render state. NOTE: the [BAKEORD v1] arm additionally routes "
        "through MC2_DIAGF, so it emits NOTHING unless MC2_DIAG_VERBOSE=1 is also set -- a probe run "
        "reporting 'zero [BAKEORD v1] lines' proves nothing on its own. WHY THIS ENTRY EXISTS: the gate "
        "shipped registered NOWHERE -- it survived check-env-registry.sh only via that script's legacy "
        "ALLOWLIST escape hatch, and had no docs/tier1_env_vars.md entry, so it was invisible to every "
        "gate inventory. bdactor.cpp's own comment asserted it was 'already-registered', which was false. "
        "Registered + documented by GATE-HYGIENE-CLOSE-1. NOTE for anyone reading the run_smoke.py "
        "passthrough allowlist: this gate is not in it, and that is HARMLESS -- smoke_lib/runner.py builds "
        "the child env with os.environ.copy(), so ambient MC2_* vars reach mc2.exe and the allowlist drops "
        "nothing (HARNESS-HYGIENE-3). The real undriveable set is runner.FORCED_ENV / POPPED_ENV / a "
        "conflicting env_extra, and run_smoke.py already refuses to start on it."
    },
    // BACKEND-DEFAULT-PARITY-1: skybox fog exclusion (was unregistered, and that
    // is what let two readers disagree in silence for the whole life of the gate).
    {
        "MC2_FEATURE_SKYBOX_FOG_EXCLUDE",
        "MC2_SKYBOX_FOG_EXCLUDE",
        EnvVarKind::Feature,
        true,
        "SKYBOX-FOG-EXCLUDE-1/2. DEFAULT-ON (SKYBOX-FOG-EXCLUDE-DEFAULT-ON-1); "
        "MC2_SKYBOX_FOG_EXCLUDE=0 is the single complete revert to the legacy OFF path. "
        "WHAT IT DOES: on HDRI missions the skybox color draw is followed by a stencil-only "
        "tag pass (shaders/hdri_skybox_stencil_tag.frag) marking true-sky pixels stencil=1; "
        "runEdgeFog/runFogOob then sample that stencil through a GL_STENCIL_INDEX texture "
        "view of sceneDepthTex_ and feather-exclude those pixels, so the OOB cloud bank stops "
        "painting over the sky (the user-reported 'can't see the skybox'). INERT ON NON-HDRI "
        "MISSIONS: renderHdriSkybox() early-returns before the tag pass, nothing is ever "
        "tagged, the frags' stencil branch is a no-op and fog_oob.frag's worldDir.z<-0.22 band "
        "fallback is retained -- byte-identical to legacy there. WHY IT IS REGISTERED NOW: it "
        "was accounted for only by scripts/check-env-registry.sh's ALLOWLIST, whose comment "
        "still said '(default OFF)' months after the flip. Being unregistered ALSO made it "
        "invisible to check-gate-default-agreement.py, which iterated the registry -- so its "
        "two readers (GL default-ON, VK default-OFF) contradicted each other unreported until "
        "BACKEND-DEFAULT-PARITY-1 made that rule scan readers instead. There is now exactly "
        "one reader: gos_postprocess.cpp skyboxFogExcludeEnabled(), exported as "
        "gos_SkyboxFogExcludeEnabled() for the Vulkan fog TU's parity guard. NOT VISUALLY "
        "VERIFIED IN THIS LANE: engine launches are hard-blocked here; the flip below is a "
        "reader-default correction on the VK guard only and does not change any GL pixel."
    },
    // ── TREE-DIAG-REGISTER-1 (closed by GATE-PARITY-CLOSE-1) ───────────────
    // Lives in kAuxEnvVars and NOT in kFeatureTable: kFeatureTable is indexed
    // by RendererFeature (see the static_assert on COUNT), so appending there
    // means minting an enum value for a var that gates no render path. This
    // one only prints.
    {
        "MC2_TREE_DIAG_TRACE",
        "MC2_TREE_DIAG_TRACE",
        EnvVarKind::Trace,
        false,
        "[DEFAULT-OFF, but the OFF token is UNSET, not '0'.] Diagnostic (2026-05-05) for the "
        "\"black billboard square\" static-prop bug. Prints up to 8 lines from each of "
        "GpuStaticPropBatcher::submit() (dynamic register/replay) and submitCachedInstance() "
        "(static recipe replay), formatting every GpuStaticPropInstance field identically so a "
        "side-by-side grep diff pinpoints which baked field is stale on the static replay. "
        "READER: GameOS/gameos/gos_static_prop_batcher.cpp:115, "
        "`(getenv(\"MC2_TREE_DIAG_TRACE\") != nullptr)`. THAT IDIOM IS PRESENCE-ONLY, so "
        "MC2_TREE_DIAG_TRACE=0 ENABLES the trace -- the same trap called out for "
        "MC2_BRIDGE_MOVER_STATE above. Do not document it as an `=0` killswitch and do not "
        "\"harmonize\" the reader without checking the callers. WHY IT IS REGISTERED NOW: it "
        "was engine-read but absent from BOTH the run_smoke passthrough allowlist and "
        "docs/tier1_env_vars.md, and was accounted for only by check-env-registry.sh's "
        "ALLOWLIST -- so a smoke run asked to turn the diagnostic ON had the var dropped at "
        "Popen and produced no [TREE_DIAG] lines, which reads as \"the bug did not reproduce\" "
        "rather than \"the trace never ran\" (ENV_DROP). Observation only: no render, gameplay "
        "or sim state changes, so ON vs OFF is pixel-identical.",
    },
    // ── CPU-PHONG-RETIRE-1 ─────────────────────────────────────────────────
    // Lives in kAuxEnvVars, not kFeatureTable: kFeatureTable is indexed by
    // RendererFeature, and minting an enum ordinal for a gate that guards a
    // CPU gameplay-elevation term (no render path) buys nothing. Same placement
    // rationale as MC2_ASSET_FAILLOUD above, which is also a default-ON
    // killswitch registered here.
    {
        "MC2_FEATURE_CPU_PHONG_RETIRE",
        "MC2_CPU_PHONG_RETIRE",
        EnvVarKind::Feature,
        true,
        "[DEFAULT-ON KILLSWITCH; '=0' is the only revert token -- MC2_TERRAIN_TESS / "
        "MC2_COLORMAP_CPU_RETIRE shape.] Retires the CPU Phong-smoothing term that "
        "MapData::terrainElevation() (mclib/mapdata.cpp) adds to GAMEPLAY elevation. "
        "READER: mclib/mapdata.cpp, `!v || v[0] != '0'`. "
        "WHY: that term was added by 4684ec0d (2026-04-10) for ONE stated reason -- "
        "docs/plans/2026-04-10-cpu-terrain-displacement-design.md -- namely that the GPU TES "
        "(shaders/gos_terrain.tese:101-108) Phong-smooths the DRAWN surface, so the CPU had to "
        "match or units would float. That counterpart is GONE: 75b8ebef (2026-06-29, "
        "TERRAIN-DEADCODE-DELETE-1) deleted the legacy tess-solid terrain draw, and the ground "
        "has since been rasterized by shaders/terrain_lod_chunk.vert + .frag -- VS+FS, no "
        "TCS/TES (gos_terrain_lod_chunk.cpp:1257) -- which applies neither Phong nor "
        "displacement. Both surviving call sites of the old funnel "
        "(gameos_graphics.cpp:7590, :7603) only rewind(). So the CPU half of a mirror kept "
        "running after the GPU half was deleted, and gameplay elevation drifted off the drawn "
        "surface with nothing reporting it. The SIBLING half of the very same design doc (dirt "
        "displacement) was already retired default-ON for exactly this reason by "
        "COLORMAP-CPU-RETIRE-1 (f9c12b53); leaving the two halves on opposite defaults would "
        "itself be the gate-halves-disagree defect. "
        "MAGNITUDE (measured offline against the real tier1 mission .pak heightfields using "
        "the scripts/terrain_height_audit.py packet reader -- NOT estimated, NOT from a log): "
        "mean |delta| 0.58-1.35 wu, p99 3.8-6.4 wu, max 12.7-18.9 wu across mc2_01/03/10/17/24; "
        "1 wu = 0.2 m (mclib/terrain.cpp:104), so up to ~3.8 m. 19-49% of sampled positions "
        "exceed 1 wu. The term is EXACTLY zero on locally planar terrain (flat AND uniform "
        "slope) and nonzero only where the surface curves, and it is zero-mean -- pure scatter, "
        "never a tuned value: objects float on some cells and sink on others. "
        "USER-VISIBLE: unset now changes where every object sits, on every map, wherever the "
        "ground curves. NOT SMOKED IN THIS LANE -- engine launches are hard-blocked here; this "
        "needs a tier1 run and a click-through before merge. '=0' restores the exact legacy "
        "term. Enforced against silent re-divergence by scripts/check-cpu-gpu-cogating.py.",
    },
    // ── WATER-SUN-SPEC-2 ───────────────────────────────────────────────────
    // Lives in kAuxEnvVars, not kFeatureTable: kFeatureTable is indexed by
    // RendererFeature and this gate needs no enum ordinal (nothing dispatches
    // on it; the only consumer is a uniform upload). Same placement rationale
    // as MC2_FEATURE_WATER_SHORE_MEANDER above.
    {
        "MC2_FEATURE_WATER_SHINE",
        "MC2_WATER_SHINE",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF OPT-IN. Value is an INTENSITY, not a boolean: unset or '0' => 0.0 = exact "
        "no-op; any other value is parsed with atof() and used verbatim, with a non-numeric "
        "truthy string falling back to 0.25. Recommended 0.2-0.5; 1.0 is deliberately "
        "exaggerated and is the value to use when confirming the mechanism works at all.] "
        "WATER-SUN-SPEC-2: Blinn-Phong SUN specular glint on the water plane. "
        "READER: GameOS/gameos/gameos_graphics.cpp gos_GetWaterSpecIntensity(), canonical "
        "default-OFF opt-in `(v && v[0] && v[0] != '0')` guarding the atof(). "
        "WHY THIS ENTRY EXISTS AND WHY IT SAYS 'SPEC-2': the ORIGINAL gate (WATER-SUN-SPEC-1, "
        "c50e99a6) shipped its C++ half and its env var and NOT its shader half -- the shader "
        "change was reverted in session review and the upload was not. gameos_graphics.cpp "
        "uploaded 'terrainLightDir' and 'u_waterSpecIntensity' to the MDI water program EVERY "
        "FRAME while NEITHER UNIFORM WAS DECLARED IN ANY SHADER IN THIS REPO'S HISTORY -- "
        "`git log -S\"u_waterSpecIntensity\" --all -- shaders/` returned ZERO commits before "
        "this repair; it returns exactly one now (9411c1687), which IS this repair. "
        "The pathspec is written WITHOUT a glob on purpose. The original form quoted a shell "
        "glob whose slash-star pair read as a C BLOCK-COMMENT OPENER to the awk "
        "comment-stripper in scripts/check-env-registry.sh; nothing closed it, so the "
        "stripper swallowed this file from here to EOF and every gate registered BELOW this "
        "row read as UNREGISTERED (measured: 634 raw, 630 stripped, 4 lost). The stripper is "
        "now string-aware and survives it, but do not put a bare block-comment opener in a "
        "registry description -- and if you must name one, spell it out as this row does. "
        "glGetUniformLocation returned -1 and the upload helper skips loc < 0 in silence, so "
        "the gate had headroom EXACTLY 1.000x at every value on every mission, and the water "
        "fragment shader had no sun direction at all -- which is why 'the sun does not glint "
        "off the water' was not fixable by any lighting tuning. SPEC-2 declares and consumes "
        "both uniforms in shaders/gos_terrain_water_mdi.frag. It also DELETES "
        "gos_SetWaterSpecIntensity(), which had zero callers: the old comment claimed 'the "
        "ImGui slider is authoritative once touched' and no such slider exists anywhere in the "
        "tree. "
        "ARITHMETIC PRECONDITION, stated so this is not mistaken for a second dead gate: the "
        "term is multiplied by terrainLightDir.z and skipped entirely when that is <= 0, so it "
        "is EXACTLY 0.0 on every mission whose sun sits at or below the horizon -- including "
        "mc2_05/10/19/21 (LightDirPitch 270/180/0/270). Those are not A/B candidates. "
        "FLIP/GRADE CRITERION: a mission with a sun ABOVE the horizon and water in frame, "
        "camera pitched DOWN toward the water so the half-vector can reach the wave normals; "
        "mc2_17 (flat water shelf, existing bookmark shelf_east_wide) and mc2_01 are the two "
        "with the most water in frame. A top-down pose will show almost nothing -- the lobe "
        "is pow(N.H, 96) and the sun is not at the camera. "
        "Visual-only; no geometry, objectId, UI or gameplay change. NOT VISUALLY VERIFIED -- "
        "engine launches are blocked in this lane.",
    },
    // ── IBL-ROTATION-LAND-1 ────────────────────────────────────────────────
    // Both rows live in kAuxEnvVars, not kFeatureTable: kFeatureTable is
    // INDEXED BY RendererFeature and neither of these needs an enum ordinal --
    // nothing dispatches on them, the only consumer is a uniform upload. Same
    // placement rationale as MC2_FEATURE_WATER_SHORE_MEANDER above.
    {
        "MC2_FEATURE_STATIC_PROP_IBL_SUN_ROTATE",
        "MC2_STATIC_PROP_IBL_SUN_ROTATE",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF OPT-IN. Canonical reader `(v && v[0] && v[0] != '0')` at "
        "gos_static_prop_batcher.cpp s_iblSunRotateEnabled. Unset -> "
        "iblShCoeffsForUpload() returns &s_currentShSet->coeffs[0][0], the SAME POINTER the "
        "two glUniform3fv sites passed before this slice, so the upload is byte-identical -- "
        "not a copy that happens to compare equal.] "
        "IBL-ROTATION-LAND-1: rotate the SH-IBL ambient probe about world-up so its bright "
        "lobe lands on the MISSION SUN instead of staying pinned wherever the source HDRI's "
        "own sun happened to be. "
        "THE DEFECT, measured offline in tools/class_light_band_harness Stage F (59->69 tests; "
        "no launch, no capture -- it is arithmetic over the shipped constexpr tables): there "
        "is ONE coefficient set for the whole game (RenderCore/IblShRegistry.h "
        "kIblShSetCount == 1, and kMissionShMap[] holds a single {nullptr,nullptr} "
        "placeholder so its COUNT IS 1, NOT 0 -- functionally empty, not literally, and a "
        "reader who trusts 'empty' mis-sizes the loop). Its bright axis is therefore a "
        "constant +0.344 deg Stuff azimuth on all 24 missions. The MEDIAN mission misses it "
        "by 106 deg, the worst by 176.7 (mc2_14), and on 17 of the 24 the probe is strictly "
        "BRIGHTER on the wall facing AWAY from the sun. The swing being mis-aimed is 21.05:1 "
        "across the sphere (the widely-quoted 7.71:1 is only the +X/-X PAIR). "
        "WHY THIS IS AN OVERSIGHT AND NOT A DESIGN CHOICE: the VISIBLE skybox drawn from the "
        "SAME HDRI is already sun-synced -- gos_postprocess.cpp:5201-5218 scans the EXR for "
        "its baked sun azimuth and sets skyYaw = hdriBakedSunAz_ - sunAzGL. The probe derived "
        "from that sky is the ONE consumer that skips the step. IblShRegistry.h's creating "
        "commit (5bfd15d8d) says future slices would populate kMissionShMap; none did, and "
        "the V-IBL default-ON flip (2026-05-27) made the unrotated probe live behaviour. "
        "IMPLEMENTATION: CPU-side at the upload site, zero per-vertex cost, "
        "shaders/static_prop.vert NOT TOUCHED. A rotation about the basis POLE is closed form "
        "-- m=0 ([0] L00, [1] L1-1 the pole term, [6] L20) invariant; m=1 pairs ([3],[2]) and "
        "([4],[5]) mix at t; m=2 pair ([8],[7]) at 2t -- and rotating the COEFFICIENTS is "
        "provably identical to counter-rotating the NORMAL "
        "(test_sh_y_rotation_equals_rotating_the_normal, 1e-12). The frame carry is the part "
        "that can silently invert: static_prop.vert feeds evalShL2 the STUFF-space "
        "`worldNormal` (:652), NOT the MC2-swapped `worldNormalMC2` (:664) the diffuse term "
        "uses, so the MC2 sun from gos_GetTerrainLightDir is carried back through the "
        "self-inverse swap and its Stuff azimuth is atan2(mc2.y, -mc2.x). Pinned against an "
        "independent derivation on all 24 missions by "
        "test_shipped_angle_matches_the_stuff_frame_derivation. "
        "WHY OFF: nobody has seen it render -- engine launches are hard-blocked in this lane "
        "-- and it is a large, deliberately visible change. "
        "GRADE IT ON mc2_17 (mean move 0.181 on the clamped multiplier, 54.6% of wall "
        "azimuths unclamped -- most headroom in tier1) or mc2_24 (0.231, largest tier1 move). "
        "NOT on mc2_10 or mc2_22: both are ambient-192 AND sun-192, ~91% of wall azimuths are "
        "already clamped, and a full sun-align rotation moves 0.0037 / 0.0123 -- ONE BYTE. "
        "That trap is why shaders/static_prop.vert's own tuning note used to nominate mc2_10 "
        "and was corrected in the same commit. mc2_05 moves most overall (0.358) but its sun "
        "is below the horizon, where 'aim at the sun' is not well posed. "
        "SHIP IT WITH ITS FLOOR: MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR (default 0.45). "
        "Visual-only; no geometry, objectId, UI or gameplay change. NOT VISUALLY VERIFIED -- "
        "engine launches are blocked in this lane.",
    },
    {
        "MC2_FEATURE_STATIC_PROP_IBL_SUN_ROTATE_FLOOR",
        "MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR",
        EnvVarKind::Feature,
        true,
        "[VALUE, not a boolean: the floor in the probe's own irradiance units. DEFAULT 0.45. "
        "Reader gos_static_prop_batcher.cpp s_iblSunRotateFloor: unset -> 0.45; '0', negative, "
        "NaN and non-numeric junk -> 0.0 (fail to the BARE ROTATION, never to a silent giant "
        "lift); above 2.0 -> clamped to 2.0. INERT UNLESS MC2_STATIC_PROP_IBL_SUN_ROTATE IS "
        "ON -- it is that gate's companion, has no effect of its own, and is registered "
        "default-ON only in the sense that it is non-zero when its parent gate is armed.] "
        "IBL-ROTATION-LAND-1's floor. "
        "WHY A ROTATION NEEDS A FLOOR: a rotation is an ISOMETRY of the sphere, so it cannot "
        "create a probe value darker than the table already had -- min and max are invariant. "
        "What it changes is WHICH wall gets the dark end, and that is the point: once the "
        "probe is aimed correctly its dark lobe COINCIDES with the wall that also has "
        "NdotL == 0, where before they sat apart and partly covered for each other. "
        "AN INHERITED FIGURE CORRECTED, because it overstates the risk: the warning handed to "
        "this lane was 'shadow-side walls darken hard, maxDown -0.705 (mc2_13), -0.704 "
        "(mc2_17), -0.692 (mc2_12/mc2_18)'. Those deltas are real, but a DELTA IS NOT A NEW "
        "MINIMUM. Measured on the darkest-wall statistic a viewer would actually notice "
        "(test_rotation_does_not_create_a_new_darkest_wall), the bare rotation moves those "
        "four missions by -0.012 or by nothing at all -- 0.3076 -> 0.2953 on mc2_12/18, "
        "0.2953 -> 0.2953 on mc2_13/17 -- because that value was ALREADY on another wall of "
        "the same building. The worst darkest-wall regression in the game is mc2_14 at "
        "-0.248, not one of the four the maxDown ranking named. A second inherited detail is "
        "also wrong: the four are indeed all ambient 31/255, but only mc2_12/18 are near 106 "
        "deg of error -- mc2_13 is -75.3 and mc2_17 is -51.6. "
        "THE FLOOR STILL EARNS ITS PLACE, for the honest reason: on those four the darkest "
        "wall sits at 0.295 in the build that SHIPS TODAY, BELOW terrain's own hard floor of "
        "mix(0.35,1.20,0.02) == 0.367 -- a building wall darker than the ground it stands on "
        "-- and the rotation relocates that pre-existing dark value onto the anti-sun wall "
        "where it becomes legible. "
        "THE SHAPE: one UNIFORM lift on L00, the only isotropic coefficient (evalShL2's "
        "spherical mean is exactly c4*L00, because c5 == c3/3 makes the L20 pair's mean "
        "vanish). Consequences, all wanted: the AIM IS UNTOUCHED (L1/L2 are neither read nor "
        "written, so the bright axis still lands on the sun to 1e-9 at every floor value -- "
        "the floor cannot quietly undo the fix); it can only LIFT, so the knob is safe to "
        "tune in either direction; it is UNIFORM across R,G,B rather than per-channel, "
        "because the baked probe's dark end is blue-dominant (min 0.232 R / 0.360 G / 0.477 "
        "B) and a per-channel clamp would neutralise exactly that; and because the sphere "
        "minimum is ROTATION-INVARIANT the lift is searched ONCE per coefficient table, not "
        "per mission. Searched numerically rather than baked as a constant on purpose -- "
        "RenderCore/IblShCoeffs.h is regenerated by tools/ibl/project_sh.py and a hardcoded "
        "lift would go silently stale. "
        "WHERE 0.45 COMES FROM -- an in-engine anchor, not taste. Solve "
        "`ambient + FLOOR * strength >= 0.367` at the darkest shipping ambient (31/255 == "
        "0.1216, shared by mc2_12/13/17/18) and the shipped daytime IBL strength 0.55: "
        "FLOOR >= 0.4462. 0.45 is that rounded up, giving a guaranteed 0.3691 >= 0.367 for "
        "EVERY normal on the sphere. On vertical walls specifically it over-delivers (0.4149) "
        "because the wall great-circle does not reach the sphere minimum. "
        "COST, stated rather than hidden: the lift is +0.218 irradiance and applies to the "
        "BRIGHT end too -- against a maximum of 4.896 that is +4.5%, almost entirely inside "
        "the clamp, and on the saturated mc2_10/mc2_22 it is nearly inert. "
        "'=0' gives the bare rotation and is the arm to use when grading how much the floor "
        "is worth. Contract tests: class_light_band_harness Stage G (10 tests). "
        "Visual-only; no geometry, objectId, UI or gameplay change. NOT VISUALLY VERIFIED -- "
        "engine launches are blocked in this lane.",
    },
};

// ---------------------------------------------------------------------------
// Debug dump
// ---------------------------------------------------------------------------
// Emits a [RENDERER_FEATURES v1] startup banner to stderr.
// Call once after fillDeviceCaps() and GL context creation.
// Prints Feature entries normally; Retired entries with [RETIRED] tag.
// Trace entries (kAuxEnvVars) are not printed here -- they are per-subsystem.

inline void debugDumpFeatureRegistry(const DeviceCaps& caps) {
    fprintf(stderr,
            "[RENDERER_FEATURES v1] gl=%d.%d"
            " ssbo=%d tess=%d compute=%d indirect=%d"
            " bindless=%d clipctrl=%d dsa=%d"
            " max_color_attach=%d max_ssbo_bind=%d max_tess_level=%d\n",
            caps.glMajor, caps.glMinor,
            (int)caps.hasSSBO, (int)caps.hasTessellation,
            (int)caps.hasComputeShaders, (int)caps.hasIndirectDraw,
            (int)caps.hasBindlessTextures, (int)caps.hasClipControl,
            (int)caps.hasDSA,
            caps.maxColorAttachments, caps.maxSSBOBindings, caps.maxTessGenLevel);

    for (int i = 0; i < static_cast<int>(RendererFeature::COUNT); ++i) {
        const EnvVarDesc& d = kFeatureTable[i];
        const char* envStr = d.envVar ? d.envVar : "(none)";
        const char* curStr;
        if (d.kind == EnvVarKind::Retired) {
            curStr = "[RETIRED]";
        } else if (!d.envVar) {
            curStr = "always-on";
        } else {
            const char* v = std::getenv(d.envVar);
            if (!v || !v[0]) {
                curStr = d.defaultOn ? "on(default)" : "off(default)";
            } else if (v[0] == '0') {
                curStr = "off(forced)";
            } else {
                curStr = "on(forced)";
            }
        }
        fprintf(stderr,
                "[RENDERER_FEATURES v1]   %-38s env=%-32s %s\n",
                d.featureId, envStr, curStr);
    }
}

} // namespace RenderCore
