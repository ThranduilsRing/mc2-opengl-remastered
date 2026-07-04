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
    TerrainTessellation = 3,   // (no env var -- always-on TCS/TES)
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
    HdrPost                  = 30,  // MC2_HDR_POST
    Bloom                    = 31,  // MC2_BLOOM
    TonemapAces              = 32,  // MC2_TONEMAP_ACES
    Ssao                     = 33,  // MC2_SSAO
    // TRACKV-VFX-PAYOFF-OPUS-1: VFX visual-payoff gates. Both default-OFF
    // (enforced by the Track-V VFX payoff guardrail unit test). Promotion to
    // default-ON is a deliberate edit here AND in that test.
    VfxSoftParticles         = 34,  // MC2_VFX_SOFT_PARTICLES
    VfxLitParticles          = 35,  // MC2_VFX_LIT_PARTICLES
    // VIEWMODE-POSTPROCESS-PRESENTATION-1: view-mode presentation framework.
    ViewmodeFramework        = 36,  // MC2_VIEWMODE_FRAMEWORK
    // TRACKRV-HZB-VISIBILITY-OPUS-1: gated reverse-Z Hi-Z depth pyramid build
    // from MainDepth (custom MIN reduction, ceil mip ladder). Default-OFF;
    // diagnostic substrate only -- NO culling/draw suppression. See
    // docs/hzb-depth-convention.md + docs/hzb-visibility-mvp.md.
    HzbBuild                 = 37,  // MC2_HZB_BUILD
    // TRACKRV-HZB-VISIBILITY-OPUS-1: diagnostic-only HZB occlusion probe.
    // Default-OFF; requires MC2_HZB_BUILD. Computes would-cull/would-keep +
    // pyramid-integrity counters from HZB readback. NEVER suppresses a draw.
    HzbProbe                 = 38,  // MC2_HZB_PROBE
    // COMPRESSION-BC7-STATICPROP-1: BC7-compressed KTX2 static-prop texture
    // arrays through the existing static-prop KTX path. Default-OFF; requires
    // MC2_MATERIAL_KTX. All-or-nothing + uniform-dim per array (see batcher).
    StaticPropBc7            = 39,  // MC2_STATICPROP_BC7
    // TEXMGR-COMPRESSED-UPLOAD-1: load a BC7 .ktx2 sidecar for a data/textures
    // texture (mech/vehicle/building/prop/UI/decal — via MC_TextureManager) and
    // upload it with glCompressedTexImage2D, single-level. Default-OFF; requires
    // GLEW_ARB_texture_compression_bptc. NOT static-prop arrays / terrain splat.
    TexmgrCompressedUpload   = 40,  // MC2_TEXMGR_COMPRESSED_UPLOAD
    // COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha immediately after
    // BuildColormapAtlas uploads to GPU. The indirect path (default-ON) has no
    // geometry displacement (thin vert, no TES), so the CPU elevation's null-guard
    // at mapdata.cpp:2435 becomes the correct state. Default-ON; kill-switch =0.
    ColormapCpuRetire        = 41,  // MC2_COLORMAP_CPU_RETIRE
    // COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas.
    // Falls back to RGBA8 when sidecar absent or BPTC cap missing. Default-ON.
    ColormapKtx2             = 42,  // MC2_COLORMAP_KTX2
    // TRACKV-CI-GATE-RESTORE-1: mission intro render path kill-switch.
    // Default-OFF = new/optimized intro render path active; ON = legacy path forced.
    MissionIntroLegacyRender = 43,  // MC2_MISSION_INTRO_LEGACY_RENDER
    // TRACKV-CI-GATE-RESTORE-1: setupTextures legacy path kill-switch.
    // Default-OFF = new/optimized setupTextures path active; ON = legacy path forced.
    SetupTexturesLegacyForce = 44,  // MC2_SETUPTEXTURES_LEGACY_FORCE
    COUNT                    = 45,
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
        false,
        "R32_UINT MRT attachment-2 with per-pixel object handles (RenderWorld M1.5). Default-off; =1 enables."
    },
    // TerrainTessellation
    {
        "MC2_FEATURE_TERRAIN_TESSELLATION",
        nullptr,
        EnvVarKind::Feature,
        true,
        "TCS/TES terrain tessellation (gameos_graphics.cpp). Always-on; no kill-switch yet."
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
        false,
        "Dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow). Opt-in MC2_SHADOW_ENABLE=1. VAO restore order fixed (SHADOW-DYNAMIC-RESTORE-1); prop-invisibility-with-shadows-on regression RESOLVED (flush-order bug, commit 2764cb65 — registry flush must precede flushShadow)."
    },
    // ImGui
    {
        "MC2_FEATURE_IMGUI",
        "MC2_IMGUI",
        EnvVarKind::Feature,
        false,
        "ImGui overlay (GuiRuntime/GuiRuntime.cpp). Default-off; =1 enables. Editor sets this automatically."
    },
    // ImGuiInspector
    {
        "MC2_FEATURE_IMGUI_INSPECTOR",
        "MC2_IMGUI_INSPECTOR",
        EnvVarKind::Feature,
        false,
        "ImGui inspector panel (GuiRuntime/EditorInspector.cpp). Default-off; =1 enables. Requires MC2_IMGUI."
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
    // HdrPost
    {
        "MC2_FEATURE_HDR_POST",
        "MC2_HDR_POST",
        EnvVarKind::Feature,
        false,
        "[REMOVED 2026-06-22 DEAD-POST-FX-CLEANUP-1: HDR-post stack (bloom/ACES) deleted as wrong-for-RTS; this env var now has NO effect. Entry retained to preserve registry indices.] HDR-POST-SCAFFOLD-1 (Track V): master gate for the HDR post-process stack on the existing RGBA16F scene FBO (gosPostProcess). Default-OFF; =1 enables. The scene color attachment is ALREADY RGBA16F, so this gate does not change buffer formats; it only enables exposure + ACES tonemap + bloom application from env/profile. When OFF, postprocess members keep their legacy defaults (exposure 1.0, tonemap/bloom off) -> byte-identical to pre-Track-V output (the unconditional sunset grade in postprocess.frag is preserved either way). Visual-only; no geometry/depth/objectId/UI change. Bloom + tonemap are independently gated (MC2_BLOOM / MC2_TONEMAP_ACES) and are inert unless this master gate is ON."
    },
    // Bloom
    {
        "MC2_FEATURE_BLOOM",
        "MC2_BLOOM",
        EnvVarKind::Feature,
        false,
        "[REMOVED 2026-06-22 DEAD-POST-FX-CLEANUP-1: bloom deleted as wrong-for-RTS; this env var now has NO effect. Entry retained to preserve registry indices.] BLOOM-MVP-1 (Track V): threshold + half-res ping-pong bloom on the HDR scene (gosPostProcess::runBloom, already present). Default-OFF; =1 enables (sets bloomEnabled_). Auto-depends on MC2_HDR_POST (no effect unless the HDR post stack is ON). Conservative defaults threshold 0.6 / intensity 0.3; ImGui-tunable and per-mission profile fields bloomThreshold / bloomIntensity. When OFF, bloomEnabled_ stays false -> composite skips the bloom add -> byte-identical. UI/HUD are composited after endScene, so they never receive bloom."
    },
    // TonemapAces
    {
        "MC2_FEATURE_TONEMAP_ACES",
        "MC2_TONEMAP_ACES",
        EnvVarKind::Feature,
        false,
        "[REMOVED 2026-06-22 DEAD-POST-FX-CLEANUP-1: ACES tonemap deleted as wrong-for-RTS; this env var now has NO effect. Entry retained to preserve registry indices.] TONEMAP-ACES-MVP-1 (Track V): ACES-ish filmic tonemap curve in the composite shader (postprocess.frag ACESFilm, already present). Default-OFF; =1 enables (sets tonemapEnabled_). Auto-depends on MC2_HDR_POST. Exposure tunable via gos_SetExposure / profile 'exposure'. When OFF, the enableTonemap uniform = 0 -> composite passes color*exposure through unchanged -> byte-identical (exposure default 1.0). UI is composited after endScene so there is no double-tonemap of HUD/ImGui."
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
        false,
        "TEXMGR-COMPRESSED-UPLOAD-1: teach MC_TextureManager to load a BC7 .ktx2 sidecar (same path/stem as the source data/textures texture, extension -> .ktx2) and upload it via glCompressedTexImage2D in MC_TextureNode::get_gosTextureHandle (mclib/txmmgr.cpp), through the new gos_NewCompressedTexture2D GameOS creator. Default-OFF; =1 enables. Requires GLEW_ARB_texture_compression_bptc (falls through + logs once if absent). This is the path for mech/vehicle/building/prop/UI/decal albedo textures — NOT static-prop texture arrays (MC2_STATICPROP_BC7) and NOT terrain splatting. All such textures are albedo/color (key in {Solid,Alpha,Keyed}) so vkFormat 146->GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM and 145->GL_COMPRESSED_RGBA_BPTC_UNORM. These loads use gosHint_DisableMipmap (single-level, GL_LINEAR/no-mip), so the sidecar is uploaded MIP 0 ONLY (byteLen = mipByteOffsets[1] if >1 mip, else pixels.size()); no glGenerateMipmap. Disk-only probe (std::fopen, not File/FastFile). On any failure (no sidecar, not BC7, load/upload fail, no BPTC support) the hook falls through to gos_NewTextureFromMemory unchanged. When OFF, the hook is fully gated (the env check short-circuits before any sidecar probe) -> byte-identical to the legacy RGBA8 path. The new gos_NewCompressedTexture2D mirrors gos_NewTextureFromMemory's gosTexture+addTexture lifecycle (handle integrates with textureList_/bind/destroy)."
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
        "R2 interlock: when =vulkan is active, postprocess-interop island gates "
        "(MC2_VK_INTEROP_BRIDGE / MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP / "
        "MC2_VK_SSAO_INTEROP) are coerced OFF (logged) to avoid the process-global "
        "volk dispatch collision. On =gl/unset the accessor adds only one memoized "
        "getenv -> byte-identical GL behavior."
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
    // VK-P9a WATER-INTEROP: the first Vulkan GEOMETRY region gate.
    {
        "MC2_FEATURE_VK_WATER_INTEROP",
        "MC2_VK_WATER_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-P9a WATER-INTEROP: presence-gated =1, default OFF. Renders the armed water "
        "fast-path BASE layer in Vulkan (the FIRST VK geometry region) into an exportable "
        "RGBA16F VkImageResource that GL composites into MainColor, instead of the GL MDI "
        "water draw (mutually exclusive; this gate is the single arbiter inside "
        "gosRenderer::renderWaterFastPath). Feed (VK-P9a STEP 0): recipe SSBO copied ONCE "
        "at load, per-frame thin records CPU-STAGED from g_thinStaging (byte-identical to the "
        "GPU compute pack), GL scene depth CPU-imported as a read-only depth attachment "
        "(GEQUAL reverse-Z, depth-write ON, per PipelineRegistry WaterArmed row), MRT "
        "color0+GBuffer1, alpha-blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA. Reflection/HDRI OFF. "
        "MVP fudge (clip.z += 0.00025*clip.w) lives in the VS (water_geom.vert), byte-copied "
        "from the GL water VS (WATER-MVP-ORACLE-1 column_major/GL_FALSE). UNLIKE the older "
        "postprocess-interop islands (MC2_VK_SSAO_INTEROP etc.) it BORROWS the shared engine "
        "VulkanContext (getOrCreateEngineVulkanContext) instead of creating its own device, so "
        "there is NO volk process-global dispatch collision and it is NOT subject to the R2 "
        "coerce-off interlock -- it REQUIRES the shared device, i.e. MC2_RENDER_BACKEND=vulkan "
        "(fail-soft to the GL water draw when the context is absent/invalid). Color zero-copy; "
        "forbidden COLOR-op counters hard-asserted ==0 (depth-IN copies EXPECTED, counted "
        "separately). Surfaced in [VK_WATER_INTEROP] health. Gate OFF -> GL water draw as "
        "before (byte-identical)."
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
        false,
        "SHADOW-BOUNDED-NEAR-FIT-1: cap the dynamic sun-shadow frustum fit radius to a small camera-centered region (radius from MC2_SHADOW_BOUNDED_NEAR_RADIUS) for higher texel density. Default-off; =1 enables. Gate OFF = byte-identical full-frustum fit. Trades far-map coverage for crisp near shadows. Applied before pow-2/texel snap (snap preserved)."
    },
    {
        "MC2_TUNE_SHADOW_BOUNDED_NEAR_RADIUS",
        "MC2_SHADOW_BOUNDED_NEAR_RADIUS",
        EnvVarKind::Trace,
        false,
        "SHADOW-BOUNDED-NEAR-FIT-1 tunable: bounded near-fit radius in world units (default 2500, clamped 512..mapClampR). Only consulted when MC2_SHADOW_BOUNDED_NEAR_FIT=1. Resolved once at process start."
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
        "V-IBL-STATIC-1: optional default-strength override for SH-L2 ambient (clamped 0..3). ImGui slider g_iblShStrength initial value. Only contributes when MC2_STATIC_PROP_IBL_SH is active; =0 on that gate uploads strength 0.0. Unset/empty -> default 0.5. Resolved once at process start."
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
    {
        "MC2_FEATURE_VK_INTEROP_BRIDGE",
        "MC2_VK_INTEROP_BRIDGE",
        EnvVarKind::Feature,
        false,
        "VK-INTEROP-BRIDGE-1: zero-copy GL<->VK image interop SEAM proven live on the "
        "EdgeFog color island (GameOS/gameos/vulkan_edge_fog_island.cpp). =1 repoints the "
        "island's color attachment to an exportable VkImageResource imported into GL "
        "(vk_image_resource.{h,cpp} + gl_interop_view.{h,cpp}); the per-frame COLOR "
        "readback/upload (glGetTexImage/vkCmdCopyImageToBuffer/glTexSubImage2D) are gone "
        "(hard-asserted ==0). Requires MC2_VULKAN_EDGE_FOG_ISLAND=1. DEFAULT-OFF, fail-soft "
        "to the readback path on any negative (LUID mismatch / missing exts / import fail); "
        "gate-OFF is byte-identical with zero external probes (blocker #6). Depth still "
        "round-trips (color-only slice). Companion infra gate MC2_VK_INTEROP_SELFTEST runs "
        "the standalone create/import/signal/wait self-check at island init."
    },
    {
        "MC2_FEATURE_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP",
        "MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-INTEROP-BRIDGE-1 P2: puts the already-fused VK<->VK fog subgraph "
        "(edge_fog+fog_oob, GameOS/gameos/vulkan_postprocess_subgraph.cpp) COLOR "
        "boundaries on the zero-copy interop seam. =1 repoints the subgraph render-pass "
        "color attachment to an exportable VkImageResource imported into GL "
        "(vk_image_resource.{h,cpp} + gl_interop_view.{h,cpp}, post-P1.5 vkToGlSem "
        "back-channel); the per-frame COLOR readback (glGetTexImage/vkCmdCopyBufferToImage/"
        "vkCmdCopyImageToBuffer/glTexSubImage2D for color) are gone (hard-asserted ==0). "
        "The internal VK<->VK 2-draw/1-render-pass chain is UNCHANGED. Requires the subgraph "
        "to be selected (MC2_VULKAN_POSTPROCESS_SUBGRAPH=1 or backend=vk). DEFAULT-OFF, "
        "fail-soft to the subgraph readback path on any negative (LUID mismatch / missing "
        "exts / import fail); gate-OFF is byte-identical with zero external probes (blocker "
        "#6). DEPTH still round-trips (color-only slice; P5 scope). Surfaced in the "
        "[VK_PP_SUBGRAPH_INTEROP] health line: interop_active, used_interop, "
        "forbidden_copyBufToImg/copyImgToBuf/getTexImage/texSubImage (all ==0 on interop)."
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
