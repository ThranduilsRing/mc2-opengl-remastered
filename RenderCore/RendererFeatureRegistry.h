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
// Adding a new env var: add its row to Gates/gate_table.cpp (with a registry(...) option to list it
// here), run `py -3 tools/gate_gen.py --sync-verified`, then read it with gate::get<name>().
// The rows of kFeatureTable and kAuxEnvVars below are GENERATED from that table; the RendererFeature
// enum stays hand-ordered (kFeatureTable is indexed by it, and the length is static_asserted).
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
// kFeatureTable is positional: each value is the row's index, so append before
// COUNT and keep both lists in the same order. Removing an entry renumbers the rest
// (nothing persists these values; code refers to entries by name).
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
    // Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state.
    RenderSnapshotBuild      = 14,  // MC2_SNAPSHOT_STATIC_PROP_BUILD
    // Extraction v2.3: skip prev-frame zero-instance snapshot slots (snap-cull opt-in).
    SnapCull                 = 15,  // MC2_SNAP_CULL
    // F1-3A ViewUniforms UBO upload (upload-only; F1-3B adds shader consumption).
    ViewUniforms             = 16,  // MC2_VIEW_UNIFORMS
    // MECH-EXTRACTION-0: mech snapshot extraction (observe-only).
    SnapshotMechExtract      = 17,  // MC2_SNAPSHOT_MECH_EXTRACT
    // V-AMBIENT-STATIC-1: hemisphere ambient fill on StaticPropOpaque lane.
    // Default-OFF; strength uniform = 0.0 when env unset/0 -> byte-identical
    // to pre-slice output.
    StaticPropAmbientV1      = 18,  // MC2_STATIC_PROP_AMBIENT_V1
    // V-MATERIAL-DEBUG-1: per-fragment material debug view (StaticPropOpaque).
    // Default-OFF; 0 = byte-identical to legacy output (shader short-circuit).
    StaticPropDebugMaterial  = 19,  // MC2_STATIC_PROP_DEBUG_MATERIAL
    // V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane.
    // Default-ON; MC2_STATIC_PROP_IBL_SH=0 uploads strength 0.0 so
    // shader `if (u_iblShStrength > 0.0)` short-circuits to byte-identical.
    // ImGui slider (g_iblShStrength) modulates strength when env gate is on;
    // the env var is the authoritative gate.
    StaticPropIblSh          = 20,  // MC2_STATIC_PROP_IBL_SH
    // V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular
    // on StaticPropOpaque lane. Default-OFF; strength uniform = 0.0 when
    // env unset/=0 -> shader `if (u_pbrV1Strength > 0.0)` short-circuits.
    // Uses MaterialGpu roughness/metallic when sampling is active, else
    // fallback metallic=0.0 roughness=0.6; F0 is albedo-tinted for metals.
    StaticPropPbrV1          = 21,  // MC2_STATIC_PROP_PBR_V1
    // VFX-AGE-SAMPLE-1: sample GPU-particle spec curves at the effect's real
    // CPU-advanced m_age instead of the fixed 0.5 snapshot. Default-OFF.
    VfxAgeSample             = 22,  // MC2_VFX_AGE_SAMPLE
    // VFX-ORIGINAL-RECORD-ABI-1 (Phase 1): CPU-oracle render — GPU renders the
    // CPU sim's live per-particle records (CardCloud first; PointCloud births 0
    // in tier1). Default-OFF parity bridge; placeholder byte-identical when off.
    VfxOracleRender          = 23,  // MC2_VFX_ORACLE_RENDER
    // VFX-GPU-SIM-CARDCLOUD-BUFFER-1: compare-only GPU sim for CardCloud.
    // _CARDCLOUD gathers live CPU state into a persistent GPU sim SSBO
    // (observe-only, no integration/readback/render); _COMPARE emits integrity
    // logs. Default-OFF. Substrate for the eventual GPU-owned sim.
    VfxGpuSimCardCloud       = 24,  // MC2_VFX_GPU_SIM_CARDCLOUD
    VfxGpuSimCompare         = 25,  // MC2_VFX_GPU_SIM_COMPARE
    // TRACKV-POST-GROUNDING-MVP: experimental visual post-process + grounding
    // gates. ALL default-OFF (enforced by the Track-V guardrail unit test).
    // Promotion to default-ON is a deliberate edit here AND in that test.
    Ssao                     = 26,  // MC2_SSAO
    // TRACKV-VFX-PAYOFF-OPUS-1: VFX visual-payoff gates. Both default-OFF
    // (enforced by the Track-V VFX payoff guardrail unit test). Promotion to
    // default-ON is a deliberate edit here AND in that test.
    VfxSoftParticles         = 27,  // MC2_VFX_SOFT_PARTICLES
    VfxLitParticles          = 28,  // MC2_VFX_LIT_PARTICLES
    // VIEWMODE-POSTPROCESS-PRESENTATION-1: view-mode presentation framework.
    ViewmodeFramework        = 29,  // MC2_VIEWMODE_FRAMEWORK
    // TRACKRV-HZB-VISIBILITY-OPUS-1: gated reverse-Z Hi-Z depth pyramid build
    // from MainDepth (custom MIN reduction, ceil mip ladder). Default-OFF;
    // diagnostic substrate only -- NO culling/draw suppression. See
    // docs/hzb-depth-convention.md + docs/hzb-visibility-mvp.md.
    HzbBuild                 = 30,  // MC2_HZB_BUILD
    // TRACKRV-HZB-VISIBILITY-OPUS-1: diagnostic-only HZB occlusion probe.
    // Default-OFF; requires MC2_HZB_BUILD. Computes would-cull/would-keep +
    // pyramid-integrity counters from HZB readback. NEVER suppresses a draw.
    HzbProbe                 = 31,  // MC2_HZB_PROBE
    // COMPRESSION-BC7-STATICPROP-1: BC7-compressed KTX2 static-prop texture
    // arrays through the existing static-prop KTX path. Default-OFF; requires
    // MC2_MATERIAL_KTX. All-or-nothing + uniform-dim per array (see batcher).
    StaticPropBc7            = 32,  // MC2_STATICPROP_BC7
    // TEXMGR-COMPRESSED-UPLOAD-1: load a BC7 .ktx2 sidecar for a data/textures
    // texture (mech/vehicle/building/prop/UI/decal — via MC_TextureManager) and
    // upload it with glCompressedTexImage2D, single-level. Default-OFF; requires
    // GLEW_ARB_texture_compression_bptc. NOT static-prop arrays / terrain splat.
    TexmgrCompressedUpload   = 33,  // MC2_TEXMGR_COMPRESSED_UPLOAD
    // COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha immediately after
    // BuildColormapAtlas uploads to GPU. The indirect path (default-ON) has no
    // geometry displacement (thin vert, no TES), so the CPU elevation's null-guard
    // at mapdata.cpp:2435 becomes the correct state. Default-ON; kill-switch =0.
    ColormapCpuRetire        = 34,  // MC2_COLORMAP_CPU_RETIRE
    // COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas.
    // Falls back to RGBA8 when sidecar absent or BPTC cap missing. Default-ON.
    ColormapKtx2             = 35,  // MC2_COLORMAP_KTX2
    // TRACKV-CI-GATE-RESTORE-1: mission intro render path kill-switch.
    // Default-OFF = new/optimized intro render path active; ON = legacy path forced.
    MissionIntroLegacyRender = 36,  // MC2_MISSION_INTRO_LEGACY_RENDER
    // TRACKV-CI-GATE-RESTORE-1: setupTextures legacy path kill-switch.
    // Default-OFF = new/optimized setupTextures path active; ON = legacy path forced.
    SetupTexturesLegacyForce = 37,  // MC2_SETUPTEXTURES_LEGACY_FORCE
    // TERRAIN-DECAL-SLICE-0A: terrain mesh-decal system. When ON, cliff-wall
    // decoration types (e.g. "MarbleCliff") are submitted to the static-prop
    // batcher with an explicit CLIFF_WALL face-frame mat4 (up=world vertical,
    // facing=outward cliff normal, tangent=contour) + small outward offset,
    // instead of the yaw-only building recipe. Default-OFF -> byte-identical.
    TerrainDecal             = 38,  // MC2_TERRAIN_DECAL
    // FAR-BACKDROP-LIT: night-shade substitution for far/impostor static-prop
    // LODs (static_prop.vert). Far LODs skip the per-vertex night bake so their
    // base_light is a frozen day/black value -> fullbright/black at night; when
    // ON the shader fades the stale seed toward the mission night object-light
    // ambient across the LOD impostor distance. Default-OFF -> byte-identical.
    FarBackdropLit           = 39,  // MC2_FAR_BACKDROP_LIT
    // NIGHT-FOG-DIM: the edge-fog "cloud bank" (edge_fog.frag) + OOB fog paint a
    // hardcoded bright-grey u_fogColor ({0.80,0.81,0.83}) at the map horizon,
    // never dimmed for the mission's night lighting. On a night map (mc2_10) the
    // horizon cloud bank composites near-white (0.80 * exposure * warm-grade). When
    // ON, runEdgeFog/executor scale the fog color toward the measured night sky
    // shade before upload so the horizon band matches the dark HDRI sky it sits
    // against. Default-OFF -> uploads the unscaled constant -> byte-identical.
    NightFogDim              = 40,  // MC2_NIGHT_FOG_DIM
    // VEHICLE-NIGHT-LIT-1: coordinate-frame fix for the legacy lit shader path
    // (gos_tex_vertex_lighted.vert) — the program that draws ground vehicles.
    // It fed calc_light Stuff-frame position + normal while the ObjectLights
    // rows (and the mech.vert / static_prop.vert consumers) are MC2-frame, so
    // every positional light type failed its falloff distance test and vehicles
    // received no pool / cone light at night. Default-OFF -> byte-identical.
    LegacyLitMc2Frame        = 41,  // MC2_LEGACY_LIT_MC2_FRAME
    // VEHICLE-NIGHT-LIT-2: restore the missing else-branch that clears a ground
    // vehicle's lightsOut flag when it is no longer DESTROYED/DISABLED, matching
    // the mech path. Default-OFF -> legacy one-way latch -> byte-identical.
    GvLightsOutClear         = 42,  // MC2_GV_LIGHTSOUT_CLEAR
    PropBaseLightDir         = 43,  // MC2_PROP_BASE_LIGHT_DIR
    PropFoliageAlphaFlag     = 44,  // MC2_PROP_FOLIAGE_ALPHAFLAG
    LightPosColumnFix        = 45,  // MC2_LIGHT_POS_COLUMN_FIX
    // VEHICLE-LIGHTING-PATH-1: populate the ground-vehicle light row before the
    // legacy draw. GVAppearance called neither CacheGpuLightData nor
    // submitMultiShape, so its lightData_ stayed all-zero and calc_light
    // returned vec3(1,1,1) -> vehicles rendered at raw albedo, fully unlit.
    // Default-ON; =0 restores the shipped unlit behavior.
    VehicleLitPath           = 46,  // MC2_VEHICLE_LIT_PATH
    // OVERLAY-CEMENT-ADOPT-1 (CONCRETE-DELIVER-1): on a cement tile the
    // overlay/decal layer adopts terrain_lod_chunk.frag's concrete albedo +
    // lighting model, closing the two-shader seam that made concrete a
    // patchwork -- including the in-shader distance fog the overlay applies
    // and the chunk frag does not. Default-ON; =0 restores stock.
    OverlayCementAdopt       = 47,  // MC2_OVERLAY_CEMENT_ADOPT
    // VEHICLE-AMBIENT-FLOOR-1: finishes VEHICLE-LIGHTING-PATH-1 by giving the
    // legacy lit vehicle path mech.vert's kAmbientFloor = 0.35 seed and its
    // clamp(0,1). Without them a gathered vehicle computes base_light+A+S*ndl
    // against a mech's clamp(0.35+A+S*ndl,0,1), so the truck still reads darker
    // than the mech beside it. Default-ON; =0 restores no-floor/no-clamp.
    VehicleAmbientFloor      = 48,  // MC2_VEHICLE_AMBIENT_FLOOR
    // LIGHT-ROW-PURE-1 (ARCH-LIGHTING slice 1): object light-ROW PRODUCTION
    // delegated to the pure object_light_row::BuildObjectLightRow, driven by an
    // explicit query + declared per-class policy instead of the shared-light-
    // list mutate/gather/restore dance. Row production only -- no shading
    // change. Default-OFF (killswitch inverted): =1 opts in; in-engine
    // byte-identity is unproven while launches are blocked.
    // Its companion diagnostic MC2_LIGHT_ROW_PURE_VERIFY is a Trace var and
    // lives in kAuxEnvVars, which is deliberately NOT indexed by
    // RendererFeature.
    LightRowPure             = 49,  // MC2_LIGHT_ROW_PURE
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
    ShorelineLegacySuppress  = 50,  // MC2_SHORELINE_LEGACY_SUPPRESS
    // SUN-AXIS-FIX-1: ObjectLights.light_dir is a STUFF-frame vector, but the
    // DIFFUSE consumers of calc_light dot it against MC2-frame normals. The one
    // consumer that swaps first (mech.vert:279, specular) is the correct one.
    // Default-OFF -> shipped expression bit-for-bit.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    SunAxisFix               = 51,  // MC2_SUN_AXIS_FIX
    // SUN-ELEVATION-FIX-1: Camera::lightDirection normalizes a unit-horizontal
    // plus sin(pitch), so its elevation is atan(sin(pitch)) rather than the
    // authored pitch. Large blast radius (terrain, props, SHADOW direction, and
    // the sun colour byte via getLightRed). Default-OFF -> bit-for-bit legacy.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    SunElevationFix          = 52,  // MC2_SUN_ELEVATION_FIX
    // LIGHTROW-FRESHNESS-1: the DYNAMIC static-prop submit path checks the
    // per-frame stamp on TG_MultiShape::cachedFrame_ before trusting
    // cachedGpuLightIndex_, and re-gathers instead of reusing a prior-frame
    // slot. Default-ON (it is a correctness fix, not a look change); =0 restores
    // the pre-slice no-frame-check read bit-for-bit.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    StaticPropLightFreshness = 53,  // MC2_STATIC_PROP_LIGHT_FRESHNESS
    // MECH-LIGHTROW-FRESHNESS-1: the GPU-mech submit path checks the per-frame
    // stamp on TG_MultiShape::cachedFrame_ before trusting cachedGpuLightIndex_,
    // and DECLINES the GPU submit (routing the mech to the existing CPU MLR
    // draw) instead of substituting slot 0. Default-ON (correctness fix, not a
    // look change); =0 restores the pre-slice no-frame-check read bit-for-bit.
    // NOTE: kFeatureTable is INDEXED BY THIS ENUM (see the
    // kFeatureTable[static_cast<int>(...)] lookups in tests/unit/
    // test_rendercore.cpp), so a new entry must be appended in the SAME
    // position in BOTH -- last, matching this value.
    MechLightFreshness       = 54,  // MC2_MECH_LIGHT_FRESHNESS
    // RUNWAY-CLOSE-1: A/B arm — MC2_OVERLAY_ART_FLATLIGHT=0 exempts the
    // terrain_overlay.frag authored-art else-branch from u_missionFlatLight.
    // Default-ON (=stock multiply); appended LAST to match kFeatureTable.
    OverlayArtFlatlight      = 55,  // MC2_OVERLAY_ART_FLATLIGHT
    COUNT                    = 56,
};

// ---------------------------------------------------------------------------
// Feature table (kFeatureTable)
// ---------------------------------------------------------------------------
// Indexed by static_cast<int>(RendererFeature::*).
// static constexpr: one copy per TU (small struct; fine).

static constexpr EnvVarDesc kFeatureTable[] = {
    // GENERATED by tools/gate_gen.py --write from Gates/gate_table.cpp (rows with a registry(...) option);
    // edit the table, not this list.
    // [0] GpuMechs
    {
        "MC2_FEATURE_GPU_MECHS",
        "MC2_GPU_MECHS",
        EnvVarKind::Retired,
        true,
        "[RETIRED] GPU mech batcher (gos_mech_batcher)."
    },
    // [1] StaticPropIndirect
    {
        "MC2_FEATURE_STATIC_PROP_INDIRECT",
        "MC2_GPU_OBJECTS",
        EnvVarKind::Retired,
        true,
        "[RETIRED] Indirect-draw static-prop batcher (gos_static_prop_batcher)."
    },
    // [2] ObjectIdBuffer
    {
        "MC2_FEATURE_OBJECT_ID_BUFFER",
        "MC2_OBJECT_ID_BUFFER",
        EnvVarKind::Feature,
        true,
        "R32_UINT MRT attachment-2 with per-pixel object handles (RenderWorld M1.5)."
    },
    // [3] TerrainTessellation
    {
        "MC2_FEATURE_TERRAIN_TESSELLATION",
        "MC2_TERRAIN_TESS",
        EnvVarKind::Feature,
        true,
        "TCS/TES terrain tessellation (gameos_graphics.cpp)."
    },
    // [4] ReverseZ
    {
        "MC2_FEATURE_REVERSE_Z",
        nullptr,
        EnvVarKind::Feature,
        true,
        "Reverse-Z depth via glClipControl GL_ZERO_TO_ONE (gameosmain.cpp:992). Always-on; MC2_REVERSE_Z_TRACE for trace."
    },
    // [5] ShadowMaps
    {
        "MC2_FEATURE_SHADOW_MAPS",
        "MC2_SHADOW_ENABLE",
        EnvVarKind::Feature,
        true,
        "Dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow)."
    },
    // [6] ImGui
    {
        "MC2_FEATURE_IMGUI",
        "MC2_IMGUI",
        EnvVarKind::Feature,
        true,
        "ImGui overlay (GuiRuntime/GuiRuntime.cpp)."
    },
    // [7] ImGuiInspector
    {
        "MC2_FEATURE_IMGUI_INSPECTOR",
        "MC2_IMGUI_INSPECTOR",
        EnvVarKind::Feature,
        true,
        "ImGui inspector panel (GuiRuntime/EditorInspector.cpp)."
    },
    // [8] DebugRenderer
    {
        "MC2_FEATURE_DEBUG_RENDERER",
        "MC2_DEBUG_RENDERER",
        EnvVarKind::Feature,
        false,
        "Debug overlay renderer (GuiRuntime/EditorInspector.cpp)."
    },
    // [9] MaterialGpu
    {
        "MC2_FEATURE_MATERIAL_GPU",
        "MC2_MATERIAL_GPU",
        EnvVarKind::Feature,
        true,
        "GPU material table upload/bind/compare for static props (default-ON as of v5, 2026-05-26)."
    },
    // [10] MaterialGpuSample
    {
        "MC2_FEATURE_MATERIAL_GPU_SAMPLE",
        "MC2_MATERIAL_GPU_SAMPLE",
        EnvVarKind::Feature,
        true,
        "GPU material albedo sampling in static_prop.frag (default-ON as of v7, 2026-05-26)."
    },
    // [11] StaticPropRegistry
    {
        "MC2_FEATURE_STATIC_PROP_REGISTRY",
        "MC2_STATIC_PROP_REGISTRY",
        EnvVarKind::Feature,
        true,
        "GpuStaticPropRegistry enable."
    },
    // [12] MaterialKtx
    {
        "MC2_FEATURE_MATERIAL_KTX",
        "MC2_MATERIAL_KTX",
        EnvVarKind::Feature,
        false,
        "KTX2 sidecar loader for static-prop textures (RenderCore/KtxLoader)."
    },
    // [13] StaticPropPacketDispatch
    {
        "MC2_FEATURE_STATIC_PROP_PACKET_DISPATCH",
        "MC2_STATIC_PROP_LEGACY_DISPATCH",
        EnvVarKind::Feature,
        true,
        "v6 DrawPacket+Meta dispatch (default-ON as of v7)."
    },
    // [14] RenderSnapshotBuild
    {
        "MC2_FEATURE_RENDER_SNAPSHOT_BUILD",
        "MC2_SNAPSHOT_STATIC_PROP_BUILD",
        EnvVarKind::Feature,
        true,
        "Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state (gos_static_prop_batcher.cpp)."
    },
    // [15] SnapCull
    {
        "MC2_FEATURE_SNAP_CULL",
        "MC2_SNAP_CULL",
        EnvVarKind::Feature,
        false,
        "Extraction v2.3: skip prev-frame zero-instance slots during snapshot-based dispatch (snap-cull opt-in)."
    },
    // [16] ViewUniforms
    {
        "MC2_FEATURE_VIEW_UNIFORMS",
        "MC2_VIEW_UNIFORMS",
        EnvVarKind::Feature,
        true,
        "ViewUniforms UBO at binding=3."
    },
    // [17] SnapshotMechExtract
    {
        "MC2_FEATURE_SNAPSHOT_MECH_EXTRACT",
        "MC2_SNAPSHOT_MECH_EXTRACT",
        EnvVarKind::Feature,
        false,
        "MECH-EXTRACTION-0: mech snapshot extraction (observe-only, no GL mutation)."
    },
    // [18] StaticPropAmbientV1
    {
        "MC2_FEATURE_STATIC_PROP_AMBIENT_V1",
        "MC2_STATIC_PROP_AMBIENT_V1",
        EnvVarKind::Feature,
        false,
        "V-AMBIENT-STATIC-1: hemisphere ambient fill on StaticPropOpaque lane (static_prop.vert)."
    },
    // [19] StaticPropDebugMaterial
    {
        "MC2_FEATURE_STATIC_PROP_DEBUG_MATERIAL",
        "MC2_STATIC_PROP_DEBUG_MATERIAL",
        EnvVarKind::Feature,
        false,
        "V-MATERIAL-DEBUG-1 / V-MATERIAL-PBR-1: per-fragment material debug view on StaticPropOpaque lane (static_prop.frag)."
    },
    // [20] StaticPropIblSh
    {
        "MC2_FEATURE_STATIC_PROP_IBL_SH",
        "MC2_STATIC_PROP_IBL_SH",
        EnvVarKind::Feature,
        true,
        "V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane (static_prop.vert)."
    },
    // [21] StaticPropPbrV1
    {
        "MC2_FEATURE_STATIC_PROP_PBR_V1",
        "MC2_STATIC_PROP_PBR_V1",
        EnvVarKind::Feature,
        false,
        "V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular on StaticPropOpaque lane (static_prop.frag, inside `#if defined(MC2_USE_VIEW_UNIFORMS)`)."
    },
    // [22] VfxAgeSample
    {
        "MC2_FEATURE_VFX_AGE_SAMPLE",
        "MC2_VFX_AGE_SAMPLE",
        EnvVarKind::Feature,
        false,
        "VFX-AGE-SAMPLE-1: sample GPU-particle spec curves (color/alpha/size/UV) at the routed effect's real CPU-advanced normalized age (gosFX Effect::m_age..."
    },
    // [23] VfxOracleRender
    {
        "MC2_FEATURE_VFX_ORACLE_RENDER",
        "MC2_VFX_ORACLE_RENDER",
        EnvVarKind::Feature,
        false,
        "VFX-ORIGINAL-RECORD-ABI-1 (Phase 1 of the originals-restoration arc; see docs/vfx-originals-restoration-design.md)."
    },
    // [24] VfxGpuSimCardCloud
    {
        "MC2_FEATURE_VFX_GPU_SIM_CARDCLOUD",
        "MC2_VFX_GPU_SIM_CARDCLOUD",
        EnvVarKind::Feature,
        false,
        "VFX-GPU-SIM-CARDCLOUD-BUFFER-1 (Stage 2 of the originals-restoration arc; docs/vfx-gpu-sim-spec.md)."
    },
    // [25] VfxGpuSimCompare
    {
        "MC2_FEATURE_VFX_GPU_SIM_COMPARE",
        "MC2_VFX_GPU_SIM_COMPARE",
        EnvVarKind::Feature,
        false,
        "VFX-GPU-SIM-CARDCLOUD-BUFFER-1: emit the [VFX_GPU_SIM v1] integrity/compare logs (cpuActive vs submitted-live count, age/alpha ranges, world bounds, SSBO..."
    },
    // [26] Ssao
    {
        "MC2_FEATURE_SSAO",
        "MC2_SSAO",
        EnvVarKind::Feature,
        false,
        "SSAO-GTAO-LITE-MVP-1 (Track V): lightweight half-res screen-space ambient-occlusion grounding pass (gosPostProcess::runSSAO) reusing the scene depth +..."
    },
    // [27] VfxSoftParticles
    {
        "MC2_FEATURE_VFX_SOFT_PARTICLES",
        "MC2_VFX_SOFT_PARTICLES",
        EnvVarKind::Feature,
        false,
        "VFX-SOFT-PARTICLES-MVP-1 (Track V): depth-fade GPU particles at scene intersections."
    },
    // [28] VfxLitParticles
    {
        "MC2_FEATURE_VFX_LIT_PARTICLES",
        "MC2_VFX_LIT_PARTICLES",
        EnvVarKind::Feature,
        false,
        "VFX-LIT-PARTICLES-MVP-1 (Track V): simple directional+ambient lighting for alpha-blended smoke/dust particles."
    },
    // [29] ViewmodeFramework
    {
        "MC2_FEATURE_VIEWMODE_FRAMEWORK",
        "MC2_VIEWMODE_FRAMEWORK",
        EnvVarKind::Feature,
        false,
        "VIEWMODE-POSTPROCESS-PRESENTATION-1: enables the view-mode presentation framework that wires ViewMode::ObjectIdDebug (and future sensor modes) through the..."
    },
    // [30] HzbBuild
    {
        "MC2_FEATURE_HZB_BUILD",
        "MC2_HZB_BUILD",
        EnvVarKind::Feature,
        false,
        "TRACKRV-HZB-VISIBILITY-OPUS-1: build a reverse-Z Hi-Z depth pyramid from MainDepth each frame via a custom fragment MIN-reduction pass (hzb_reduce.frag)..."
    },
    // [31] HzbProbe
    {
        "MC2_FEATURE_HZB_PROBE",
        "MC2_HZB_PROBE",
        EnvVarKind::Feature,
        false,
        "TRACKRV-HZB-VISIBILITY-OPUS-1: diagnostic-only HZB occlusion probe."
    },
    // [32] StaticPropBc7
    {
        "MC2_FEATURE_STATICPROP_BC7",
        "MC2_STATICPROP_BC7",
        EnvVarKind::Feature,
        false,
        "COMPRESSION-BC7-STATICPROP-1: upload BC7-compressed KTX2 static-prop texture arrays through the existing static-prop KTX path (gos_static_prop_batcher.cpp)..."
    },
    // [33] TexmgrCompressedUpload
    {
        "MC2_FEATURE_TEXMGR_COMPRESSED_UPLOAD",
        "MC2_TEXMGR_COMPRESSED_UPLOAD",
        EnvVarKind::Feature,
        true,
        "TEXMGR-COMPRESSED-UPLOAD-1: teach MC_TextureManager to load a BC7 .ktx2 sidecar (same path/stem as the source data/textures texture, extension -> .ktx2) and..."
    },
    // [34] ColormapCpuRetire
    {
        "MC2_FEATURE_COLORMAP_CPU_RETIRE",
        "MC2_COLORMAP_CPU_RETIRE",
        EnvVarKind::Feature,
        true,
        "COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha in BuildColormapAtlas immediately after GPU atlas upload."
    },
    // [35] ColormapKtx2
    {
        "MC2_FEATURE_COLORMAP_KTX2",
        "MC2_COLORMAP_KTX2",
        EnvVarKind::Feature,
        true,
        "COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas (gos_terrain_indirect.cpp)."
    },
    // [36] MissionIntroLegacyRender
    {
        "MC2_FEATURE_MISSION_INTRO_LEGACY_RENDER",
        "MC2_MISSION_INTRO_LEGACY_RENDER",
        EnvVarKind::Feature,
        false,
        "TRACKV-CI-GATE-RESTORE-1: mission intro render path kill-switch."
    },
    // [37] SetupTexturesLegacyForce
    {
        "MC2_FEATURE_SETUPTEXTURES_LEGACY_FORCE",
        "MC2_SETUPTEXTURES_LEGACY_FORCE",
        EnvVarKind::Feature,
        false,
        "TRACKV-CI-GATE-RESTORE-1: setupTextures legacy path kill-switch."
    },
    // [38] TerrainDecal
    {
        "MC2_TERRAIN_DECAL",
        "MC2_TERRAIN_DECAL",
        EnvVarKind::Feature,
        false,
        "TERRAIN-DECAL-SLICE-0A: terrain mesh-decal system."
    },
    // [39] FarBackdropLit
    {
        "MC2_FAR_BACKDROP_LIT",
        "MC2_FAR_BACKDROP_LIT",
        EnvVarKind::Feature,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: night-shade substitution for far/impostor static-prop LODs (static_prop.vert)."
    },
    // [40] NightFogDim
    {
        "MC2_NIGHT_FOG_DIM",
        "MC2_NIGHT_FOG_DIM",
        EnvVarKind::Feature,
        false,
        "BACKDROP-BLIT-ATTRIB-1: RenderDoc pixel-history on the pre-composite HDR scene FBO (231) pinned the mc2_10 night 'white horizon band' to the EDGE-FOG..."
    },
    // [41] LegacyLitMc2Frame
    {
        "MC2_LEGACY_LIT_MC2_FRAME",
        "MC2_LEGACY_LIT_MC2_FRAME",
        EnvVarKind::Feature,
        true,
        "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LEGACY_LIT_MC2_FRAME=0 reverts."
    },
    // [42] GvLightsOutClear
    {
        "MC2_GV_LIGHTSOUT_CLEAR",
        "MC2_GV_LIGHTSOUT_CLEAR",
        EnvVarKind::Feature,
        false,
        "VEHICLE-NIGHT-LIT-2: GVAppearance::update (mclib/gvactor.cpp) calls gvShape->SetLightsOut(true) when a ground vehicle is OBJECT_STATUS_DESTROYED or..."
    },
    // [43] PropBaseLightDir
    {
        "MC2_PROP_BASE_LIGHT_DIR",
        "MC2_PROP_BASE_LIGHT_DIR",
        EnvVarKind::Feature,
        false,
        "PROP-BASE-LIGHT-DIR-1: give the static-prop baked per-vertex base_light seed a directional (hemispherical-wrap) orientation term, so faces pointing away..."
    },
    // [44] PropFoliageAlphaFlag
    {
        "MC2_PROP_FOLIAGE_ALPHAFLAG",
        "MC2_PROP_FOLIAGE_ALPHAFLAG",
        EnvVarKind::Feature,
        false,
        "FOLIAGE-ALPHAFLAG-1: make the per-instance flags bit 2 emitted by the static-prop RECIPE paths (BldgAppearance/TreeAppearance::registerStatic..."
    },
    // [45] LightPosColumnFix
    {
        "MC2_LIGHT_POS_COLUMN_FIX",
        "MC2_LIGHT_POS_COLUMN_FIX",
        EnvVarKind::Feature,
        true,
        "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LIGHT_POS_COLUMN_FIX=0 reverts."
    },
    // [46] VehicleLitPath
    {
        "MC2_VEHICLE_LIT_PATH",
        "MC2_VEHICLE_LIT_PATH",
        EnvVarKind::Feature,
        true,
        "VEHICLE-LIGHTING-PATH-1: put GROUND VEHICLES on the same lighting path as every other object class."
    },
    // [47] OverlayCementAdopt
    {
        "MC2_FEATURE_OVERLAY_CEMENT_ADOPT",
        "MC2_OVERLAY_CEMENT_ADOPT",
        EnvVarKind::Feature,
        true,
        "OVERLAY-CEMENT-ADOPT-1: closes the two-shader concrete seam CEMENT-PATCHY-CAUSE-1 attributed but explicitly left open as a design call ('either the overlay..."
    },
    // [48] VehicleAmbientFloor
    {
        "MC2_VEHICLE_AMBIENT_FLOOR",
        "MC2_VEHICLE_AMBIENT_FLOOR",
        EnvVarKind::Feature,
        true,
        "VEHICLE-AMBIENT-FLOOR-1: DEFAULT-ON."
    },
    // [49] LightRowPure
    {
        "MC2_LIGHT_ROW_PURE",
        "MC2_LIGHT_ROW_PURE",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF, KILLSWITCH INVERTED: MC2_LIGHT_ROW_PURE=1 opts IN to the pure producer; unset/0 is the legacy in-place packing loop in GatherLightsParameters..."
    },
    // [50] ShorelineLegacySuppress
    {
        "MC2_SHORELINE_LEGACY_SUPPRESS",
        "MC2_SHORELINE_LEGACY_SUPPRESS",
        EnvVarKind::Feature,
        true,
        "[DEFAULT FLIPPED OFF -> ON by LEGACY-SHORELINE-MISAIM-1 (2026-07-31)."
    },
    // [51] SunAxisFix
    {
        "MC2_SUN_AXIS_FIX",
        "MC2_SUN_AXIS_FIX",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF, OPT-IN: MC2_SUN_AXIS_FIX=1 enables; unset/0 is the shipped expression bit-for-bit."
    },
    // [52] SunElevationFix
    {
        "MC2_SUN_ELEVATION_FIX",
        "MC2_SUN_ELEVATION_FIX",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF, OPT-IN: MC2_SUN_ELEVATION_FIX=1 enables; unset/0 is the legacy three lines bit-for-bit."
    },
    // [53] StaticPropLightFreshness
    {
        "MC2_STATIC_PROP_LIGHT_FRESHNESS",
        "MC2_STATIC_PROP_LIGHT_FRESHNESS",
        EnvVarKind::Feature,
        true,
        "LIGHTROW-FRESHNESS-1: DEFAULT-ON."
    },
    // [54] MechLightFreshness
    {
        "MC2_MECH_LIGHT_FRESHNESS",
        "MC2_MECH_LIGHT_FRESHNESS",
        EnvVarKind::Feature,
        true,
        "MECH-LIGHTROW-FRESHNESS-1, amended by MECH-DECLINE-FIX-1: DEFAULT-ON."
    },
    // [55] OverlayArtFlatlight
    {
        "MC2_FEATURE_OVERLAY_ART_FLATLIGHT",
        "MC2_OVERLAY_ART_FLATLIGHT",
        EnvVarKind::Feature,
        true,
        "RUNWAY-CLOSE-1: A/B arm for the REOPENED 'runway is the wrong color' item (user re-grade 2026-08-02: 'there is no difference here to me')."
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
    // GENERATED by tools/gate_gen.py --write from Gates/gate_table.cpp (rows with a registry(...) option);
    // edit the table, not this list.
    {
        "MC2_ABL_COMPILE_FATAL",
        "MC2_ABL_COMPILE_FATAL",
        EnvVarKind::Feature,
        true,
        "ASAN-3-ABL-COMPILE-FATAL-1: fail at preprocess when ABL syntax errors leave partial bytecode; set =0 only for legacy execute-anyway diagnostics.",
    },
    {
        "MC2_ABL_COMPILE_FATAL_REPRO",
        "MC2_ABL_COMPILE_FATAL_REPRO",
        EnvVarKind::Trace,
        false,
        "ASAN-3-ABL-COMPILE-FATAL-1: preprocess the supplied malformed-fixture path during ABL init to prove fail-loud behavior.",
    },
    {
        "MC2_TRACE_AIMED_FIRE_FIXTURE",
        "MC2_AIMED_FIRE_FIXTURE",
        EnvVarKind::Trace,
        false,
        "ISSUE-9 headless probe (code/warrior.cpp MechWarrior::updateActions): every home-team mech without a combat order is issued an aimed-HEAD attack...",
    },
    {
        "MC2_FEATURE_ANIM_CADENCE_FIX",
        "MC2_ANIM_CADENCE_FIX",
        EnvVarKind::Feature,
        true,
        "ANIM-CADENCE-FIX: advance mech gait at most once per render frame; prevents a visible double-step when Mover::getLOSPosition() re-invokes...",
    },
    {
        "MC2_TRACE_APPEAR_AUDIT",
        "MC2_APPEAR_AUDIT",
        EnvVarKind::Trace,
        false,
        "META-FIX design B PART 4 (code/mission.cpp Mission::start beside mission_phase_mark('mission_ready')): default OFF, OBSERVATIONAL-FIRST.",
    },
    {
        "MC2_FEATURE_ASSET_FAILLOUD",
        "MC2_ASSET_FAILLOUD",
        EnvVarKind::Feature,
        true,
        "META-FIX design B PART 1 (mclib/tgl_load_guard.cpp): default-ON killswitch (MC2_TERRAIN_TESS shape - unset=ON, =0 exact legacy fail-soft).",
    },
    {
        "MC2_TRACE_ASSET_TRACE",
        "MC2_ASSET_TRACE",
        EnvVarKind::Trace,
        false,
        "META-FIX design B PART 0 companion (mclib/msl.cpp loader chokepoint): =1 additionally logs '[ASSET_LOAD v1] event=file_missing ...' for MISSING files at the...",
    },
    {
        "MC2_FEATURE_ASSIMP_MECH_IMPORT",
        "MC2_ASSIMP_MECH_IMPORT",
        EnvVarKind::Feature,
        false,
        "BT2018 mech import 1A: route mech geometry through the Assimp GLTF/FBX importer instead of the legacy MSL loader.",
    },
    {
        "MC2_TRACE_ASSIMP_TRACE",
        "MC2_ASSIMP_TRACE",
        EnvVarKind::Trace,
        false,
        "TRACKV-CI-GATE-RESTORE-1: verbose Assimp loader trace.",
    },
    {
        "MC2_TRACE_BOOT_INFO",
        "MC2_BOOT_INFO",
        EnvVarKind::Trace,
        false,
        "INFO-CAPTURE fixture: =N force-selects the first home mover into the in-mission Info window and switches the command bar to the INFO tab ONCE at frame >= N...",
    },
    {
        "MC2_TRACE_BOOT_INFO_SCROLL",
        "MC2_BOOT_INFO_SCROLL",
        EnvVarKind::Trace,
        false,
        "INFO-IMGUI-SLICE-4 capture assist: =1 (with MC2_BOOT_INFO=N driving) pins the info-panel loadout list scrolled to bottom from frame N+3000 on BOTH paths -...",
    },
    {
        "MC2_TRACE_BOOT_OBJ",
        "MC2_BOOT_OBJ",
        EnvVarKind::Trace,
        false,
        "OBJ-CAPTURE fixture: =N calls ControlGui::startObjectives(1) ONCE at frame >= N (ControlGui::update), showing the in-mission objectives overlay.",
    },
    {
        "MC2_TRACE_BOOT_OBJ_COMPLETE",
        "MC2_BOOT_OBJ_COMPLETE",
        EnvVarKind::Trace,
        false,
        "OBJ-COMPLETE-CAPTURE fixture: =N force-resolves the first active non-hidden objective to OS_SUCCESSFUL ONCE at frame >= N (ControlGui::update) via the...",
    },
    {
        "MC2_TRACE_BOOT_PAUSED",
        "MC2_BOOT_PAUSED",
        EnvVarKind::Trace,
        false,
        "PAUSE-IMGUI-SLICE-0: =N auto-toggles the in-mission pause menu ONCE at frame >= N (MissionInterfaceManager::update).",
    },
    {
        "MC2_TRACE_BOOT_PAUSED_CONFIRM",
        "MC2_BOOT_PAUSED_CONFIRM",
        EnvVarKind::Trace,
        false,
        "PAUSE-IMGUI-SLICE-3: =abort|exit invokes the ABORT MISSION / EXIT GAME confirm dialog ONCE after the MC2_BOOT_PAUSED pause menu has settled...",
    },
    {
        "MC2_TRACE_BOOT_TIMER",
        "MC2_BOOT_TIMER",
        EnvVarKind::Trace,
        false,
        "TIMER-CAPTURE fixture: =N force-arms the in-mission mission timer ONCE at frame >= N by setting mission->m_timeLimit = scenarioTime + 754 s...",
    },
    {
        "MC2_TRACE_BOOT_TIMER_SECS",
        "MC2_BOOT_TIMER_SECS",
        EnvVarKind::Trace,
        false,
        "TIMER-CAPTURE fixture parameter: =S overrides the remaining seconds the MC2_BOOT_TIMER fixture arms (default 754).",
    },
    {
        "MC2_TRACE_BOOT_VIDEO",
        "MC2_BOOT_VIDEO",
        EnvVarKind::Trace,
        false,
        "VIDEO-CAPTURE fixture: =1 overrides the smoke-mode movie skip in MC2Movie::init so in-mission comm videos (mc2_01 V1A.bik ~frame 1600) play under...",
    },
    {
        "MC2_FEATURE_BRAIN_DISPATCH",
        "MC2_BRAIN_DISPATCH",
        EnvVarKind::Feature,
        false,
        "TechScript special-dispatch master gate (mission.cpp/brain_special_dispatch.cpp).",
    },
    {
        "MC2_BRAIN_FREEZE_TRACE",
        "MC2_BRAIN_FREEZE_TRACE",
        EnvVarKind::Trace,
        false,
        "SAVE/LOAD-AIFREEZE-TRACE-1 (+ ISSUE-10 extension): one-line stderr trace at end of MechWarrior::runBrain (brainState, curTacOrder.code, movePath.numSteps...",
    },
    {
        "MC2_FEATURE_BRAIN_RUNTIME",
        "MC2_BRAIN_RUNTIME",
        EnvVarKind::Feature,
        false,
        "TechBrain declarative-brain runtime master gate (warrior.cpp).",
    },
    {
        "MC2_FEATURE_BRAIN_RUNTIME_APPLY",
        "MC2_BRAIN_RUNTIME_APPLY",
        EnvVarKind::Feature,
        false,
        "TechBrain runtime apply gate (warrior.cpp).",
    },
    {
        "MC2_TRACE_BUCKET_ORDER",
        "MC2_BUCKET_ORDER_TRACE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 Task-0 probe: per-type static-vs-dynamic add counts + order ([BUCKET_ORDER v1]).",
    },
    {
        "MC2_FEATURE_BURNIN_NO_JPG",
        "MC2_BURNIN_NO_JPG",
        EnvVarKind::Feature,
        false,
        "BURNIN-DISK-JPEG kill-switch: when set, skip the <name>.burnin.jpg sidecar probe in TerrainColorMap::init and load the full <name>.burnin.tga (or .tga) instead.",
    },
    {
        "MC2_FEATURE_CALLED_SHOT_FIX",
        "MC2_CALLED_SHOT_FIX",
        EnvVarKind::Feature,
        false,
        "ISSUE-9 called-shot effectiveness (gameplay gate, default OFF = stock): (S1) Mover::calcAttackChance floors the aimed-shot modifiers at Head 0.30 / Arm 0.45...",
    },
    {
        "MC2_TRACE_CAMERA_MOVE_DIAG",
        "MC2_CAMERA_MOVE_DIAG",
        EnvVarKind::Trace,
        false,
        "TRACKV-CI-GATE-RESTORE-1: camera movement diagnostic readback probe.",
    },
    {
        "MC2_DIAG_CAMERA_PARK",
        "MC2_CAMERA_PARK",
        EnvVarKind::Trace,
        false,
        "CAMERA-PARK-1 (ported from vk branch 4b69ef78): ='x,y,rot,proj,alt[@frame]' hard-sets the live camera pose once at frame >= @frame (default 300) and pins it...",
    },
    {
        "MC2_FEATURE_CEMENT_ALPHA_ATLAS",
        "MC2_CEMENT_ALPHA_ATLAS",
        EnvVarKind::Feature,
        true,
        "DEFAULT ON since RUNWAY-CLOSE-1 (2026-08-02), measured at the user's mc2_10 saved_204907 bookmark, exe 30c18df1: rescued swath pixels 15 -> 6506, rescued...",
    },
    {
        "MC2_TRACE_CEMENT_ATLAS_DUMP",
        "MC2_CEMENT_ATLAS_DUMP",
        EnvVarKind::Trace,
        false,
        "CEMENT-ATLAS-CELL-2: dump the assembled cement/road overlay atlas so cell art can be inspected directly -- which cells actually carry painted markings is an...",
    },
    {
        "MC2_FEATURE_CEMENT_ATLAS_MAGENTA_KEY",
        "MC2_CEMENT_ATLAS_MAGENTA_KEY",
        EnvVarKind::Feature,
        false,
        "CEMENT-ATLAS-MAGENTA-KEY-1 (MC2-03-CEMENT-PINK-1): solid cement catalog cells reach the atlas via TerrainTextures::textureFromMemory (gos_Texture_Solid)...",
    },
    {
        "MC2_FEATURE_CEMENT_CONCRETE_CANON",
        "MC2_CEMENT_CONCRETE_CANON",
        EnvVarKind::Feature,
        false,
        "CEMENT-ATLAS-CELL-2 ARM B: point every plain-concrete tile at ONE canonical concrete cell, so an apron cannot change colour tile to tile at all.",
    },
    {
        "MC2_FEATURE_CEMENT_CONCRETE_TONE",
        "MC2_CEMENT_CONCRETE_TONE",
        EnvVarKind::Feature,
        false,
        "CEMENT-ATLAS-CELL-2 ARM A: tone-normalise ONLY the actual-concrete atlas cells.",
    },
    {
        "MC2_FEATURE_CEMENT_LIT",
        "MC2_CEMENT_LIT",
        EnvVarKind::Feature,
        false,
        "RUNWAY-APRON-TONE-2 (user report, mc2_10 @ bookmark 204937): 0..1 share of the terrain lighting term that CEMENT fragments obey; default 0.0 == stock fully...",
    },
    {
        "MC2_FEATURE_CEMENT_RAMP_WORD",
        "MC2_CEMENT_RAMP_WORD",
        EnvVarKind::Feature,
        false,
        "CEMENT-WORD-RAMP-1: the ALBEDO-side cure for the project's longest-running user complaint, the dark outlines around every concrete pad and taxiway...",
    },
    {
        "MC2_FEATURE_CEMENT_ROAD_ART_DETECT",
        "MC2_CEMENT_ROAD_ART_DETECT",
        EnvVarKind::Feature,
        false,
        "CEMENT-INTRACELL-1 ARM C: detect the BAKED ASPHALT ROAD TILESET authored inside SOLID cement catalog cells, and exclude those cells from every concrete...",
    },
    {
        "MC2_FEATURE_CEMENT_TRANSITION_FALLBACK",
        "MC2_CEMENT_TRANSITION_FALLBACK",
        EnvVarKind::Feature,
        false,
        "CEMENT-TRANSITION-FALLBACK-1 (MC2-03-CEMENT-PINK-1): cure MISSING ROAD SWATHS (user report, mc2_24).",
    },
    {
        "MC2_TRACE_COLORMAP_DISPLACE_PROBE",
        "MC2_COLORMAP_DISPLACE_PROBE",
        EnvVarKind::Trace,
        false,
        "COLORMAP-CPU-RETIRE-1 parity probe: logs displacement magnitude (maxDispMag in wu) from the CPU colormap displacement path.",
    },
    {
        "MC2_FEATURE_CONE_POOL_POLICY",
        "MC2_CONE_POOL_POLICY",
        EnvVarKind::Feature,
        true,
        "CONE-SPILL-POLICY-1: pool-placement policy for building/beacon spotlight ground pools on the DEFAULT path (MC2_CONE_SPILL_COUPLING OFF).",
    },
    {
        "MC2_TUNE_CONE_POOL_REACH",
        "MC2_CONE_POOL_REACH",
        EnvVarKind::Trace,
        false,
        "CONE-SPILL-POLICY-1: reach multiplier k for the extended beam/terrain intersection search (reach = k * authored coneLen * MC2_LIGHT_CONE_SPILL).",
    },
    {
        "MC2_CONE_SPILL_COUPLING",
        "MC2_CONE_SPILL_COUPLING",
        EnvVarKind::Trace,
        false,
        "CONE-SPILL-GRADE-1: isolates CONE-SPILL-COUPLING-1 (c1f98b45), which shipped UNGATED as a default-behaviour change riding MC2_LIGHT_CONE_NODES and is now...",
    },
    {
        "MC2_FEATURE_CPU_PHONG_RETIRE",
        "MC2_CPU_PHONG_RETIRE",
        EnvVarKind::Feature,
        true,
        "[DEFAULT-ON KILLSWITCH; '=0' is the only revert token -- MC2_TERRAIN_TESS / MC2_COLORMAP_CPU_RETIRE shape.]",
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP",
        "MC2_DEBUG_STATE_DUMP",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-1: write a JSON render-state snapshot to debug_state/latest_render_state.json every 300 frames (and at frame 1).",
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_DIR",
        "MC2_DEBUG_STATE_DUMP_DIR",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-1: override output directory for MC2_DEBUG_STATE_DUMP.",
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_HISTORY",
        "MC2_DEBUG_STATE_DUMP_HISTORY",
        EnvVarKind::Trace,
        false,
        "DEBUG-STATE-DUMP-2: enable rolling 8-slot history ring alongside latest_render_state.json.",
    },
    {
        "MC2_DIAG_DEBUG_VIEW",
        "MC2_DEBUG_VIEW",
        EnvVarKind::Trace,
        false,
        "DEBUG-VIEW-SWEEP-1: =N (1..6) boot-seeds the global full-scene debug render view: 1=shadow-factor 2=normalized-depth 3=unlit-albedo 4=overdraw-proxy...",
    },
    {
        "MC2_DIAG_DEBUG_VIEW_SWEEP",
        "MC2_DEBUG_VIEW_SWEEP",
        EnvVarKind::Trace,
        false,
        "DEBUG-VIEW-SWEEP-1: ='<startFrame>,<settleFrames>' frame-scheduled auto-sweep of the six MC2_DEBUG_VIEW views (missiongui.cpp update).",
    },
    {
        "MC2_DETERMINISTIC_RNG",
        "MC2_DETERMINISTIC_RNG",
        EnvVarKind::Feature,
        false,
        "Route all randomness (incl.",
    },
    {
        "MC2_TRACE_DIAG_VERBOSE",
        "MC2_DIAG_VERBOSE",
        EnvVarKind::Trace,
        false,
        "QUIET-DEFAULT-BUILD-1: ONE umbrella gate for the engine's undifferentiated developer chatter.",
    },
    {
        "MC2_DIRECT_FIRE_STRAIGHT",
        "MC2_DIRECT_FIRE_STRAIGHT",
        EnvVarKind::Feature,
        false,
        "Direct-fire de-curve: PPC/autocannon/gauss bolts fly STRAIGHT instead of re-tracking the moving target every frame (the janky homing curve).",
    },
    {
        "MC2_FEATURE_DYNAMIC_DECALS",
        "MC2_DYNAMIC_DECALS",
        EnvVarKind::Feature,
        false,
        "Dynamic decal ring (mclib/dynamic_decal_ring.cpp): spawn()/gatherToDecalBatch() are no-ops when off.",
    },
    {
        "MC2_TUNE_FAR_BACKDROP_LIT_FAR",
        "MC2_FAR_BACKDROP_LIT_FAR",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: view-depth (clip.w, world units) where the night-shade fade completes (full substitution).",
    },
    {
        "MC2_TUNE_FAR_BACKDROP_LIT_NEAR",
        "MC2_FAR_BACKDROP_LIT_NEAR",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: view-depth (clip.w, world units) where the night-shade fade begins.",
    },
    {
        "MC2_TUNE_FAR_BACKDROP_LIT_STRENGTH",
        "MC2_FAR_BACKDROP_LIT_STRENGTH",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: maximum substitution amount scaling the distance fade (1.0 = full replace of the stale seed with night ambient; >1 exaggerates for...",
    },
    {
        "MC2_DIAG_FAR_BACKDROP_LIT_TINT",
        "MC2_FAR_BACKDROP_LIT_TINT",
        EnvVarKind::Trace,
        false,
        "FAR-BACKDROP-LIT-IMPL-1: debug/exaggeration additive tint 'r,g,b' added to the substituted night shade so the affected far/impostor pixels are visually...",
    },
    {
        "MC2_TRACE_FIRE_DECISION",
        "MC2_FIRE_DECISION_TRACE",
        EnvVarKind::Trace,
        false,
        "ISSUE-17 fire-refusal instrumentation (default OFF = byte-identical; needs MC2_DIAG_TAGS=...,COMBAT).",
    },
    {
        "MC2_TRACE_FIXTURE_ORDER_LIVENESS",
        "MC2_FIXTURE_ORDER_LIVENESS",
        EnvVarKind::Trace,
        false,
        "META-FIX design F behavioral fixture (code/mission.cpp soak namespace; requires MC2_SOAK_AUTOWIN): at 20% of the autowin timer every armed home mech gets a...",
    },
    {
        "MC2_TRACE_FLUSH_PROBE",
        "MC2_FLUSH_PROBE",
        EnvVarKind::Trace,
        false,
        "TRACKV-CI-GATE-RESTORE-1: GPU pipeline flush probe via glFinish.",
    },
    {
        "MC2_FOG_HORIZON_CLAMP",
        "MC2_FOG_HORIZON_CLAMP",
        EnvVarKind::Feature,
        true,
        "FOG-HORIZON-CLAMP-1: reshape the out-of-bounds / edge fog to a horizon elevation profile (gos_postprocess).",
    },
    {
        "MC2_FOG_HORIZON_FADE_END",
        "MC2_FOG_HORIZON_FADE_END",
        EnvVarKind::Feature,
        false,
        "FOG-HORIZON-CLAMP-1: override the horizon-fog fade band END (elevation).",
    },
    {
        "MC2_FOG_HORIZON_FADE_START",
        "MC2_FOG_HORIZON_FADE_START",
        EnvVarKind::Feature,
        false,
        "FOG-HORIZON-CLAMP-1: override the horizon-fog fade band START (elevation).",
    },
    {
        "MC2_TRACE_FORCE_ALL_VISIBLE",
        "MC2_FORCE_ALL_VISIBLE",
        EnvVarKind::Trace,
        false,
        "FORCE-ALL-VISIBLE-1: mark every unit (mechs, ground vehicles, elementals, all teams) fully visible and sensed, bypassing sensor / contact gating so nothing...",
    },
    {
        "MC2_TRACE_FORCE_LOD",
        "MC2_FORCE_LOD",
        EnvVarKind::Trace,
        false,
        "TREE-OVERRIDE-LOD-MVP-1 (Task 5): force the active render-LOD index for override trees (0..MAX_LODS-1), clamped to the highest registered+available LOD.",
    },
    {
        "MC2_FEATURE_FRAMEGRAPH_EXECUTOR",
        "MC2_FRAMEGRAPH_EXECUTOR",
        EnvVarKind::Feature,
        false,
        "RENDER-FRAME-GRAPH arc: top-level frame-graph executor ownership of pass begin/end (mclib/render_contract.cpp, GameOS/gameos/gos_postprocess.cpp).",
    },
    {
        "MC2_TRACE_FREEZE_MOVERS_AT",
        "MC2_FREEZE_MOVERS_AT",
        EnvVarKind::Trace,
        false,
        "FREEZE-MOVERS-1: =<frame> freezes every mover the first frame g_mc2FrameCounter >= <frame>, WITHOUT needing the command channel (the 'freeze_movers on...",
    },
    {
        "MC2_FULLBAKE_DIAG",
        "MC2_FULLBAKE_DIAG",
        EnvVarKind::Trace,
        false,
        "SPOTLIGHT-DIRECTION-1 measurement instrument.",
    },
    {
        "MC2_FX_COST_SPLIT",
        "MC2_FX_COST_SPLIT",
        EnvVarKind::Feature,
        false,
        "FX cost-split instrumentation (default-off).",
    },
    {
        "MC2_FX_DEFS",
        "MC2_FX_DEFS",
        EnvVarKind::Feature,
        false,
        "FX-DEFS-SIDECAR-1: per-effect JSON sidecar overlay for the gosFX spec catalog (mc2::particles::SpecLibrary).",
    },
    {
        "MC2_FX_FORCE_SPAWN",
        "MC2_FX_FORCE_SPAWN",
        EnvVarKind::Feature,
        false,
        "FX-FORCE-SPAWN v1 fixture: default-OFF.",
    },
    {
        "MC2_GLSTATEGUARD_LOG",
        "MC2_GLSTATEGUARD_LOG",
        EnvVarKind::Trace,
        false,
        "GLSTATEGUARD-SLICE-1: leak-visibility for mc2gl::GlScopedSsboBinding (gl_state_guard.h).",
    },
    {
        "MC2_GLSTATEGUARD_TERRAIN",
        "MC2_GLSTATEGUARD_TERRAIN",
        EnvVarKind::Feature,
        true,
        "GLSTATEGUARD-SLICE-2: kill-switch for the RAII depth/blend/cull ownership of the terrain LOD chunk draw (gos_TerrainLodChunk_SubmitDrawCommands via...",
    },
    {
        "MC2_GOM_UPDATE_COST_SPLIT",
        "MC2_GOM_UPDATE_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "GOM_UPDATE_COST v1 (framebudget): =1 emits one coarse per-phase GameObjectManager update CPU-cost summary to stderr (objmgr.cpp).",
    },
    {
        "MC2_GPUBUF_LIGHT_GROWONCE",
        "MC2_GPUBUF_LIGHT_GROWONCE",
        EnvVarKind::Feature,
        false,
        "LIGHT-GROW-ONCE-SUBDATA-1 (AMD-safe intermediate for the light SSBO orphan churn; NOT the full GpuStorageRing ring).",
    },
    {
        "MC2_GPUBUF_RESIDENCY",
        "MC2_GPUBUF_RESIDENCY",
        EnvVarKind::Trace,
        false,
        "GPU-BUFFER-WRAPPER-TIER0-HUD-1: every ~300 frames list the live HUD ring buffers ([GPUBUF v1] residency tag/kind/frames/vbBytes/ibBytes/liveFences).",
    },
    {
        "MC2_GPUBUF_RING",
        "MC2_GPUBUF_RING",
        EnvVarKind::Feature,
        false,
        "GPU-BUFFER-WRAPPER-TIER0-HUD-1: route the fixed-capacity gosMesh HUD/2D meshes (gameos_graphics.cpp draw/drawIndexed) through a 3-frame persistent-coherent...",
    },
    {
        "MC2_GPUBUF_RING_FORCE_WRAP",
        "MC2_GPUBUF_RING_FORCE_WRAP",
        EnvVarKind::Trace,
        false,
        "GPU-BUFFER-WRAPPER-TIER0-HUD-1 NEGATIVE TEST: deterministically advance the HUD ring twice (beginFrame without an intervening endFrame) so the...",
    },
    {
        "MC2_TRACE_GPU_CULL_STATIC_LIGHT_ZERO_PROBE",
        "MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE",
        EnvVarKind::Trace,
        false,
        "Static-prop light-bake observation gate, default OFF.",
    },
    {
        "MC2_FEATURE_GPU_MECH_CULL",
        "MC2_GPU_MECH_CULL",
        EnvVarKind::Feature,
        true,
        "Slice C1 (gos_mech_batcher.cpp): render-only mech GPU cull -- skip GPU mech submit for actors the GPU lagged-readback says were invisible last frame (CPU...",
    },
    {
        "MC2_FEATURE_GPU_MECH_FAST_TRANSFORM",
        "MC2_GPU_MECH_FAST_TRANSFORM",
        EnvVarKind::Feature,
        true,
        "Slice C3-revised (gos_mech_batcher.cpp): body-only fast transform path for GPU-driven mechs.",
    },
    {
        "MC2_FEATURE_GPU_MECH_LEAF_SKIP",
        "MC2_GPU_MECH_LEAF_SKIP",
        EnvVarKind::Feature,
        true,
        "Slice D-leaf-skip-v2 (gos_mech_batcher.cpp): strip per-leaf body of mechShape->TransformMultiShape* work when the leaf is provably unaffected.",
    },
    {
        "MC2_FEATURE_GPU_MECH_SHADOW_FAST_TRANSFORM",
        "MC2_GPU_MECH_SHADOW_FAST_TRANSFORM",
        EnvVarKind::Feature,
        true,
        "Slice C3-shadow (gos_mech_batcher.cpp): fast-transform variant for the mech shadow-caster callsite.",
    },
    {
        "MC2_GPU_PARTICLES",
        "MC2_GPU_PARTICLES",
        EnvVarKind::Feature,
        false,
        "Master gate for GPU billboard particles (mclib/particles/batcher.cpp initialize_env).",
    },
    {
        "MC2_FEATURE_HUD_IMGUI_TABS",
        "MC2_HUD_IMGUI_TABS",
        EnvVarKind::Feature,
        false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C: data-driven command-bar tab registry (code/hudtabregistry.{h,cpp} + wiring in code/controlgui.cpp).",
    },
    {
        "MC2_FEATURE_HUD_READABILITY_V1",
        "MC2_HUD_READABILITY_V1",
        EnvVarKind::Feature,
        false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice A: data-first HUD readability.",
    },
    {
        "MC2_FEATURE_HUD_TABS_TEST",
        "MC2_HUD_TABS_TEST",
        EnvVarKind::Feature,
        false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C witness: register a 4th 'test' tab (no legacy button; ImGui chrome via HudImGui::hudShrinkPoint + hudToDisplay, trivial...",
    },
    {
        "MC2_FEATURE_ICON_FAILLOUD",
        "MC2_ICON_FAILLOUD",
        EnvVarKind::Feature,
        false,
        "META-FIX design B PART 3 (code/mechicon.cpp iconTileChecked at the three CPU icon-tile blits): default OFF. =1 substitutes a solid MAGENTA tile when an icon...",
    },
    {
        "MC2_TRACE_IFACE_COST_SPLIT",
        "MC2_IFACE_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "MISSION-INTERFACE-PERF-1: chrono cost-split of MissionInterfaceManager::update() (the GameLogic.Mission.Interface Tracy zone) incl.",
    },
    {
        "MC2_TUNE_IMPOSTOR_DIST",
        "MC2_IMPOSTOR_DIST",
        EnvVarKind::Trace,
        false,
        "Static-prop / tree impostor LOD switch distance in world units (mclib/bdactor.cpp TreeAppearance::selectActiveLOD).",
    },
    {
        "MC2_FEATURE_LETTERBOX_OPAQUE",
        "MC2_LETTERBOX_OPAQUE",
        EnvVarKind::Feature,
        true,
        "Cinematic letterbox bars are drawn OPAQUE black; only their EXTENT animates.",
    },
    {
        "MC2_TRACE_LIGHTBAKE_PARITY",
        "MC2_LIGHTBAKE_PARITY",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: baked permanent light record == gathered transient record byte/hash parity.",
    },
    {
        "MC2_FEATURE_LIGHTBAKE_REBAKE_ON_READY",
        "MC2_LIGHTBAKE_REBAKE_ON_READY",
        EnvVarKind::Feature,
        true,
        "LIGHTBAKE-REBAKE-ON-READY-1: soft-invalidate the per-recipe static-light bake cache once, 8 frames after mission-ready, so recipes baked during boot/load...",
    },
    {
        "MC2_TRACE_LIGHTBAKE_STABILITY",
        "MC2_LIGHTBAKE_STABILITY",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: per-instance lightDataIndex permanence/stability proof.",
    },
    {
        "MC2_FEATURE_LIGHTING_DEBUG_VIEW",
        "MC2_LIGHTING_DEBUG_VIEW",
        EnvVarKind::Feature,
        false,
        "DEBUG-VIEW-SWEEP-1: replace lit shading with a diagnostic channel across terrain, static props and mechs.",
    },
    {
        "MC2_LIGHT_CONE_NODES",
        "MC2_LIGHT_CONE_NODES",
        EnvVarKind::Trace,
        true,
        "LIGHT-CONE-MODELS-1: DEFAULT-ON.",
    },
    {
        "MC2_FEATURE_LIGHT_GRID",
        "MC2_LIGHT_GRID",
        EnvVarKind::Feature,
        true,
        "LIGHT-CLUSTER-PERF-1: world-space XY light GRID over the per-frame terrain light pool.",
    },
    {
        "MC2_TUNE_LIGHT_GRID_DIM",
        "MC2_LIGHT_GRID_DIM",
        EnvVarKind::Trace,
        false,
        "LIGHT-CLUSTER-PERF-1 knob: cells per axis for the light grid (grid is DIM x DIM over the XY bbox of this frame's packed lights).",
    },
    {
        "MC2_FEATURE_LIGHT_POOL_COMPRESS",
        "MC2_LIGHT_POOL_COMPRESS",
        EnvVarKind::Feature,
        true,
        "NIGHT-LIGHT-POOLS-3: tone-compress the accumulated terrain light-pool sum (chunk + overlay frag): 1-exp(-1.6*sum) instead of min(sum,1).",
    },
    {
        "MC2_TUNE_LIGHT_POOL_MAX",
        "MC2_LIGHT_POOL_MAX",
        EnvVarKind::Trace,
        false,
        "LIGHT-POOL-CAP-1: per-mission slot count of the terrain light pool (gos_terrain_lighting.cpp mission_init).",
    },
    {
        "MC2_LIGHT_PREFIX_GPU_COPY",
        "MC2_LIGHT_PREFIX_GPU_COPY",
        EnvVarKind::Feature,
        false,
        "LIGHT-PREFIX-GPU-COPY-1 (TXMMGR-PERF-EASYWINS-1): keep a VRAM stash of the immutable static light prefix [0..S) and glCopyBufferSubData it into the...",
    },
    {
        "MC2_FEATURE_LIGHT_REALISM",
        "MC2_LIGHT_REALISM",
        EnvVarKind::Feature,
        true,
        "LIGHT-REALISM-1: night light-pool + cone-glow realism reshape.",
    },
    {
        "MC2_LIGHT_ROW_PURE_VERIFY",
        "MC2_LIGHT_ROW_PURE_VERIFY",
        EnvVarKind::Trace,
        false,
        "[DEFAULT-OFF.",
    },
    {
        "MC2_TRACE_LINKUP",
        "MC2_LINKUP_TRACE",
        EnvVarKind::Trace,
        false,
        "ISSUE-50 diagnostics (code/mech.cpp BattleMech::update): for every off-GUI mech, periodically (~1/s) logs [LINKUP] part/name/status/los, plus [LINKUP]...",
    },
    {
        "MC2_TRACE_LOAD_RACE_DELAY_ATLAS",
        "MC2_LOAD_RACE_DELAY_ATLAS",
        EnvVarKind::Trace,
        false,
        "gos_terrain_indirect.cpp negative control for the mission-load residency-race probes: when set (any value), BuildColormapAtlas() defers the real GL atlas...",
    },
    {
        "MC2_TRACE_LOAD_RACE_PROBE_FRAMES",
        "MC2_LOAD_RACE_PROBE_FRAMES",
        EnvVarKind::Trace,
        false,
        "gameosmain.cpp: overrides the comma-separated list of frame numbers at which the mission-load residency-race one-shot diagnostics probe fires (see...",
    },
    {
        "MC2_LOG",
        "MC2_LOG",
        EnvVarKind::Trace,
        false,
        "Master stdout log gate (gameosmain.cpp, right after setvbuf).",
    },
    {
        "MC2_TRACE_LOG_FILE_RESOLVE",
        "MC2_LOG_FILE_RESOLVE",
        EnvVarKind::Trace,
        false,
        "META-FIX design E tier 3 (mclib/file.cpp, parsed at InitModSearchPaths): file-resolution ladder diagnostics.",
    },
    {
        "MC2_TRACE_MOVER_COHERENCE_AUDIT",
        "MC2_MOVER_COHERENCE_AUDIT",
        EnvVarKind::Trace,
        false,
        "META-FIX design A runtime tier (code/mission.cpp auditMoverCoherence, ~1 Hz): read-only audit that the five per-mover registries (isOnGUI render gate...",
    },
    {
        "MC2_FEATURE_MOVE_RECON",
        "MC2_MOVE_RECON",
        EnvVarKind::Feature,
        false,
        "Pathfinding cost instrumentation (mclib/move_recon.cpp).",
    },
    {
        "MC2_TUNE_NIGHT_AMBIENT_FLOOR",
        "MC2_NIGHT_AMBIENT_FLOOR",
        EnvVarKind::Trace,
        false,
        "NIGHT-LIGHT-POOLS-1 knob: minimum per-channel output (0..64, clamped) of Camera::getLightRed/Green/Blue -- the family feeding the colormap night burn-in...",
    },
    {
        "MC2_TUNE_NIGHT_FOG_DIM_SCALE",
        "MC2_NIGHT_FOG_DIM_SCALE",
        EnvVarKind::Trace,
        false,
        "BACKDROP-BLIT-ATTRIB-1: multiplier applied per-channel to the edge-fog + OOB fog color when MC2_NIGHT_FOG_DIM=1.",
    },
    {
        "MC2_TRACE_NO_ACTIVATE",
        "MC2_NO_ACTIVATE",
        EnvVarKind::Trace,
        false,
        "Mission Editor (editor/EditorMFC.cpp, MainFrm.cpp): come up minimized and non-activating, never taking the foreground or keyboard focus.",
    },
    {
        "MC2_TRACE_NO_CURSOR_GRAB",
        "MC2_NO_CURSOR_GRAB",
        EnvVarKind::Trace,
        false,
        "SMOKE-NO-CURSOR-STEAL-1: suppress ALL OS-cursor manipulation -- window mouse-grab, Win32 ClipCursor clamp, and gos_SetMousePosition warps -- plus window...",
    },
    {
        "MC2_FEATURE_OVERLAY_CEMENT_SHADOW_MATCH",
        "MC2_OVERLAY_CEMENT_SHADOW_MATCH",
        EnvVarKind::Feature,
        false,
        "CEMENT-PATCHY-CAUSE-1: make terrain_overlay.frag apply the SAME concrete lighting exemption terrain_lod_chunk.frag already applies, so the overlay/decal...",
    },
    {
        "MC2_OVERLAY_TILE_HIRES",
        "MC2_OVERLAY_TILE_HIRES",
        EnvVarKind::Feature,
        false,
        "OVERLAY-TILE-HIRES-1: honor the overlay tile TGA's actual dimensions instead of the legacy hard-assumed 64px (initOverlay header resync mirrors...",
    },
    {
        "MC2_FEATURE_PAVED_OVERLAY_CLASS",
        "MC2_PAVED_OVERLAY_CLASS",
        EnvVarKind::Feature,
        false,
        "DEFAULT 0 (OFF).",
    },
    {
        "MC2_PICK_FALLBACK_COARSE",
        "MC2_PICK_FALLBACK_COARSE",
        EnvVarKind::Feature,
        false,
        "MISSION-INTERFACE-PERF-1: coarse-to-fine closest-vertex fallback in Camera::inverseProject (mclib/camera.cpp).",
    },
    {
        "MC2_POST_CAS",
        "MC2_POST_CAS",
        EnvVarKind::Feature,
        false,
        "POST-FX-CAS-1: gated AMD FidelityFX CAS sharpening folded into the final composite shader (after tonemap/grade, before UI).",
    },
    {
        "MC2_POST_CAS_SHARPNESS",
        "MC2_POST_CAS_SHARPNESS",
        EnvVarKind::Feature,
        false,
        "POST-FX-CAS-1: CAS sharpness knob, clamped 0..1 (default 0.5).",
    },
    {
        "MC2_PROJECTILE_SPEED_MULT",
        "MC2_PROJECTILE_SPEED_MULT",
        EnvVarKind::Feature,
        false,
        "Direct-fire projectile speed multiplier (PPC/autocannon/gauss = non-arc bolts).",
    },
    {
        "MC2_PROP_LIGHT_LATCH_DIAG",
        "MC2_PROP_LIGHT_LATCH_DIAG",
        EnvVarKind::Trace,
        false,
        "FOGLIGHT-LATCH-1: dump per-static-prop-actor [PROPLATCH v1] lines -- latch frame + latched lightRGB (the fogLightSet one-shot sample fed to the SUN slot) vs...",
    },
    {
        "MC2_PROP_LIGHT_LATCH_DIAG_INTERVAL",
        "MC2_PROP_LIGHT_LATCH_DIAG_INTERVAL",
        EnvVarKind::Trace,
        false,
        "FOGLIGHT-LATCH-1: frame throttle for the [PROPLATCH v1] compare line (default 120).",
    },
    {
        "MC2_FEATURE_PROP_SHADOW_RECEIVE",
        "MC2_PROP_SHADOW_RECEIVE",
        EnvVarKind::Feature,
        false,
        "PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade shadow in shadow_screen.frag (building self-shadow + prop-on-prop; the cascades already...",
    },
    {
        "MC2_RENDERLISTS_COST_SPLIT",
        "MC2_RENDERLISTS_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "TXMMGR-PERF-EASYWINS-1: coarse per-phase CPU cost split of MC_TextureManager::renderLists() (mclib/txmmgr.cpp).",
    },
    {
        "MC2_FEATURE_RENDER_BACKEND",
        "MC2_RENDER_BACKEND",
        EnvVarKind::Feature,
        false,
        "VK-P13-SELECTOR-1: top-level render-backend selector, values gl|vulkan (default gl; unset/unrecognized -> gl, coercion logged).",
    },
    {
        "MC2_FEATURE_RENDER_BACKEND_IFACE",
        "MC2_RENDER_BACKEND_IFACE",
        EnvVarKind::Feature,
        false,
        "RENDER-BACKEND-SEAMS arc: routes select GL calls (e.g.",
    },
    {
        "MC2_FEATURE_RENDER_PASS_ORDER",
        "MC2_RENDER_PASS_ORDER",
        EnvVarKind::Feature,
        false,
        "CONTRACT-3: per-frame resource-ordering audit (mclib/render_contract.cpp) - checks each pass's reads[] are satisfied by a prior write before beginPass.",
    },
    {
        "MC2_TRACE_RENDER_WORLD",
        "MC2_RENDER_WORLD_TRACE",
        EnvVarKind::Trace,
        false,
        "Per-frame [RENDER_WORLD v1] banner + per-event logs (RenderWorld.cpp).",
    },
    {
        "MC2_TRACE_RESOLVE_TRACE_FILE",
        "MC2_RESOLVE_TRACE_FILE",
        EnvVarKind::Trace,
        false,
        "META-FIX design E tier 3 (mclib/file.cpp): output path for the MC2_LOG_FILE_RESOLVE=2 JSONL resolve trace, opened for append at InitModSearchPaths (relative...",
    },
    {
        "MC2_TRACE_RES_ASSERT",
        "MC2_RES_ASSERT",
        EnvVarKind::Trace,
        false,
        "META-FIX design D runtime tier (RenderCore/ResolutionContract.h): audit mode, unset/0 = off, fatal = abort, anything else = log.",
    },
    {
        "MC2_FEATURE_RUNWAY_EDGE_BLEND",
        "MC2_RUNWAY_EDGE_BLEND",
        EnvVarKind::Feature,
        false,
        "RUNWAY-EDGE-BLEND-1 (MASTER-FAIL #6), REVISION 2 = DILATION: cure the hard full-tile stairstep cliff where a SOLID cement pad/runway tile abuts BARE terrain...",
    },
    {
        "MC2_SHADER_SPIRV",
        "MC2_SHADER_SPIRV",
        EnvVarKind::Feature,
        false,
        "SPIRV-CONSUMER-PILOT-BUILD-1: DEFAULT-OFF. =1 loads the offline-baked OpenGL SPIR-V (OFFLINE-SHADER-VARIANT-BUILD-1, shaders/spv/) for the postprocess...",
    },
    {
        "MC2_SHADER_SPIRV_FATAL",
        "MC2_SHADER_SPIRV_FATAL",
        EnvVarKind::Trace,
        false,
        "SPIRV-CONSUMER-PILOT-BUILD-1: DEFAULT-OFF debug gate.",
    },
    {
        "MC2_FEATURE_SHADOW_BOUNDED_NEAR_FIT",
        "MC2_SHADOW_BOUNDED_NEAR_FIT",
        EnvVarKind::Feature,
        true,
        "SHADOW-BOUNDED-NEAR-FIT-1: cap the dynamic sun-shadow frustum fit radius to a small camera-centered region (radius from MC2_SHADOW_BOUNDED_NEAR_RADIUS...",
    },
    {
        "MC2_TUNE_SHADOW_BOUNDED_NEAR_RADIUS",
        "MC2_SHADOW_BOUNDED_NEAR_RADIUS",
        EnvVarKind::Trace,
        false,
        "SHADOW-BOUNDED-NEAR-FIT-1 tunable: bounded near-fit radius in world units (default 2500, clamped 512..mapClampR).",
    },
    {
        "MC2_SHADOW_CASTER_CULL_CACHE",
        "MC2_SHADOW_CASTER_CULL_CACHE",
        EnvVarKind::Feature,
        false,
        "SHADOW-CASTER-CULL-CACHE-1 (TXMMGR-PERF-EASYWINS-1): reuse the Shadow.CasterCull result (mclib/txmmgr.cpp DynamicShadowPass) across frames when BOTH the...",
    },
    {
        "MC2_FEATURE_SHADOW_CSM",
        "MC2_SHADOW_CSM",
        EnvVarKind::Feature,
        true,
        "Cascaded shadow maps master gate (GameOS/gameos/gos_postprocess.cpp), reworked 2026-06-18 (commit 8ff13a36).",
    },
    {
        "MC2_DIAG_SHADOW_DEBUG_MODE",
        "MC2_SHADOW_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "SHADOW-ENV-DEBUG-MODE-1: selects shadow debug map overlay without ImGui.",
    },
    {
        "MC2_FEATURE_SHADOW_DYNAMIC_PROP_CASTERS",
        "MC2_SHADOW_DYNAMIC_PROP_CASTERS",
        EnvVarKind::Feature,
        true,
        "SHADOW-DYNAMIC-PROP-CASTERS-1: feed the per-frame DYNAMIC sun-shadow caster pass from the full registry (registered props, visibility-independent) instead...",
    },
    {
        "MC2_TRACE_SHADOW_FRUSTUM",
        "MC2_SHADOW_FRUSTUM_DIAG",
        EnvVarKind::Trace,
        false,
        "SHADOW-FRUSTUM-AUDIT-1: read-only per-frame dynamic sun-shadow coverage probe in buildDynamicLightMatrix (sun dir, frustum XY, fit/xy radius, map clamp...",
    },
    {
        "MC2_FEATURE_SHADOW_GUARD_CLAMPED_SUN",
        "MC2_SHADOW_GUARD_CLAMPED_SUN",
        EnvVarKind::Feature,
        true,
        "SHADOW-SUNCLAMP-1: feed the shadow_screen.frag back-face guard (`if (NdotL < 0.05) return 1.0;`) the SAME altitude-clamped sun the shadow matrices are built...",
    },
    {
        "MC2_TUNE_SHADOW_MIN_SUN_ALT",
        "MC2_SHADOW_MIN_SUN_ALT",
        EnvVarKind::Trace,
        false,
        "SHADOW-SUN-ALT-CLAMP-1 tunable: minimum EFFECTIVE sun altitude in DEGREES used by the shadow projection matrices (mc2ClampShadowSunAltitude...",
    },
    {
        "MC2_SIM_IDENTITY",
        "MC2_SIM_IDENTITY",
        EnvVarKind::Trace,
        true,
        "SIM-IDENTITY-HASH-1: emit a world-state hash (sorted-by-partId digest over mover position/rotation/gesture/anim-frame/damage/status/team plus...",
    },
    {
        "MC2_SIM_IDENTITY_TRACE",
        "MC2_SIM_IDENTITY_TRACE",
        EnvVarKind::Trace,
        false,
        "SIM-IDENTITY-HASH-1: emit one `[SIMID v1] simframe=N t=T n=C graded=.. visual=.. strict=..` line per SIM frame to stdout.",
    },
    {
        "MC2_SIM_IDENTITY_TRACE_DETAIL",
        "MC2_SIM_IDENTITY_TRACE_DETAIL",
        EnvVarKind::Trace,
        false,
        "SIM-IDENTITY-HASH-1: with MC2_SIM_IDENTITY_TRACE on, additionally emit one `[SIMID v1 mover]` line per mover per sim frame.",
    },
    {
        "MC2_FEATURE_SKYBOX_FOG_EXCLUDE",
        "MC2_SKYBOX_FOG_EXCLUDE",
        EnvVarKind::Feature,
        true,
        "SKYBOX-FOG-EXCLUDE-1/2.",
    },
    {
        "MC2_FEATURE_SMART_LOAD",
        "MC2_SMART_LOAD",
        EnvVarKind::Feature,
        true,
        "SMART-LOAD startup/mission-load perf path (logisticsdata.cpp).",
    },
    {
        "MC2_TRACE_SOAK_SALVAGE_TRADEMOVER",
        "MC2_SOAK_SALVAGE_TRADEMOVER",
        EnvVarKind::Trace,
        false,
        "ISSUE-42a repro fixture (code/mission.cpp soak harness): with MC2_SOAK_AUTOWIN + MC2_SOAK_KILL_ENEMY, exercises the REAL mid-mission salvage-craft flow...",
    },
    {
        "MC2_TRACE_SPLIT_PATH_PARITY",
        "MC2_SPLIT_PATH_PARITY_TRACE",
        EnvVarKind::Trace,
        false,
        "META-FIX design C runtime tier (GameOS/gameos/gameos_graphics.cpp gosRenderer::init): =1 emits machine-parseable [SPLIT_PATH_PARITY] marker lines at...",
    },
    {
        "MC2_SPOTLIGHT_DIRECTION_FIX",
        "MC2_SPOTLIGHT_DIRECTION_FIX",
        EnvVarKind::Trace,
        false,
        "SPOTLIGHT-DIRECTION-1: fixes two coordinate-space defects at the building/guard-tower spotlight registration site (mclib/bdactor.cpp).",
    },
    {
        "MC2_SP_ALPHASCAN_GATE",
        "MC2_SP_ALPHASCAN_GATE",
        EnvVarKind::Feature,
        false,
        "SP-BATCHER-ALPHASCAN-GATE-1: skip the frame-constant per-static-prop alpha re-resolve scan in the batcher when nothing that affects it changed...",
    },
    {
        "MC2_SP_GLASS_DITHER",
        "MC2_SP_GLASS_DITHER",
        EnvVarKind::Feature,
        true,
        "BUILDING-GLASS-1: ordered-dither (Bayer 4x4) translucency for partial-alpha texels on static-prop ALPHA_TEST materials (window glass).",
    },
    {
        "MC2_FEATURE_SSAO_BLUR",
        "MC2_SSAO_BLUR",
        EnvVarKind::Feature,
        true,
        "SSAO-BLUR-1: separable depth-aware (joint bilateral) blur over the half-res AO buffer, inserted between the AO gen pass and the apply pass...",
    },
    {
        "MC2_FEATURE_SSAO_BLUR_SIGMA",
        "MC2_SSAO_BLUR_SIGMA",
        EnvVarKind::Feature,
        false,
        "SSAO-BLUR-1 tunable: RELATIVE depth tolerance (|d-dc|/dc) for the SSAO bilateral blur's edge-stopping weight.",
    },
    {
        "MC2_DIAG_SSAO_DEBUG",
        "MC2_SSAO_DEBUG",
        EnvVarKind::Trace,
        false,
        "SSAO-GTAO-LITE-MVP-1: when set with MC2_SSAO=1, the SSAO apply pass OVERWRITES the scene with the half-res AO buffer as grayscale (blend disabled) instead...",
    },
    {
        "MC2_FEATURE_SSAO_RADIUS",
        "MC2_SSAO_RADIUS",
        EnvVarKind::Feature,
        false,
        "SSAO-RADIUS-REVERT-1 (VERIFY-DEBT-1): explicit launch-time override for the SSAO world-unit sample radius.",
    },
    {
        "MC2_FEATURE_STATICPROP_MATERIAL_PBR_SLOTS",
        "MC2_STATICPROP_MATERIAL_PBR_SLOTS",
        EnvVarKind::Feature,
        false,
        "Static-prop PBR ORM (occlusion-roughness-metallic) texture-array slots + sidecar feed.",
    },
    {
        "MC2_TRACE_STATICPROP_ORM_TRACE",
        "MC2_STATICPROP_ORM_TRACE",
        EnvVarKind::Trace,
        false,
        "Static-prop ORM load trace: logs source path + dims per unique ORM texture.",
    },
    {
        "MC2_FEATURE_STATIC_LIGHT_UPLOAD_SPLIT",
        "MC2_STATIC_LIGHT_UPLOAD_SPLIT",
        EnvVarKind::Feature,
        true,
        "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: upload the immutable static light prefix [0..S) once/dirty-only + the dynamic suffix [S..count) per frame, instead...",
    },
    {
        "MC2_FEATURE_SHADOW_STATIC_BUILDINGS",
        "MC2_STATIC_PROP_BUILDING_SHADOW",
        EnvVarKind::Feature,
        false,
        "SHADOW-STATIC-BUILDINGS-2: replay ALL registered rigid-building recipes (full registry, visibility-independent) into the world-fixed static shadow map, once...",
    },
    {
        "MC2_FEATURE_SHADOW_STATIC_BUILDINGS_REBAKE",
        "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE",
        EnvVarKind::Feature,
        true,
        "STATIC-BLDG-SHADOW-REBAKE-1: re-arm the one-shot world-fixed static-shadow latch (gos_ResetStaticLightMatrix) when a registered building is destroyed --...",
    },
    {
        "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME",
        "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME",
        EnvVarKind::Trace,
        false,
        "STATIC-BLDG-SHADOW-REBAKE-1: deterministic repro hook (test-only, no production callers).",
    },
    {
        "MC2_FEATURE_STATIC_PROP_COLORS_FILL",
        "MC2_STATIC_PROP_COLORS_FILL",
        EnvVarKind::Feature,
        false,
        "STATICPROP-COLORS-FILL-DEBUGONLY-1: per-static-instance Colors SSBO (binding 1) zero-fill.",
    },
    {
        "MC2_STATIC_PROP_FIXTURE_GLOW",
        "MC2_STATIC_PROP_FIXTURE_GLOW",
        EnvVarKind::Trace,
        false,
        "FIXTURE-GLOW-1: DEFAULT-OFF on measured evidence.",
    },
    {
        "MC2_STATIC_PROP_FIXTURE_LIGHTS",
        "MC2_STATIC_PROP_FIXTURE_LIGHTS",
        EnvVarKind::Trace,
        true,
        "BEACON-FIXTURE-ID-1: DEFAULT-ON.",
    },
    {
        "MC2_FEATURE_STATIC_PROP_FLUSH_CACHED_BLOB",
        "MC2_STATIC_PROP_FLUSH_CACHED_BLOB",
        EnvVarKind::Feature,
        false,
        "STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A: replace the per-leaf registry-flush rebuild with cached immutable instance/actor-record blobs bulk-appended into...",
    },
    {
        "MC2_TRACE_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE",
        "MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A diagnostic: with the cached path active, also build the legacy temp instance+record per leaf and compare...",
    },
    {
        "MC2_TRACE_STATIC_PROP_FLUSH_COST_SPLIT",
        "MC2_STATIC_PROP_FLUSH_COST_SPLIT",
        EnvVarKind::Trace,
        false,
        "SPFLUSH-COST-SPLIT-1: RDTSC cost-split decomposition of GpuStaticPropRegistry::flush().",
    },
    {
        "MC2_FEATURE_STATIC_PROP_IBL_NIGHT_SCALE",
        "MC2_STATIC_PROP_IBL_NIGHT_SCALE",
        EnvVarKind::Feature,
        true,
        "IBL-NIGHT-SCALE-1: night-scales the SH-IBL ambient probe on static props.",
    },
    {
        "MC2_TUNE_STATIC_PROP_IBL_SH_SET",
        "MC2_STATIC_PROP_IBL_SH_SET",
        EnvVarKind::Trace,
        false,
        "V-IBL-STATIC-2: override the active SH coefficient set by name (e.g.",
    },
    {
        "MC2_TUNE_STATIC_PROP_IBL_SH_STRENGTH",
        "MC2_STATIC_PROP_IBL_SH_STRENGTH",
        EnvVarKind::Trace,
        false,
        "V-IBL-STATIC-1: optional default-strength override for SH-L2 ambient (clamped 0..3).",
    },
    {
        "MC2_FEATURE_STATIC_PROP_IBL_SUN_ROTATE",
        "MC2_STATIC_PROP_IBL_SUN_ROTATE",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF OPT-IN.",
    },
    {
        "MC2_FEATURE_STATIC_PROP_IBL_SUN_ROTATE_FLOOR",
        "MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR",
        EnvVarKind::Feature,
        true,
        "[VALUE, not a boolean: the floor in the probe's own irradiance units.",
    },
    {
        "MC2_FEATURE_STATIC_PROP_LIVE_BUILDER",
        "MC2_STATIC_PROP_LIVE_BUILDER",
        EnvVarKind::Feature,
        false,
        "v8 kill-switch: =1 restores live build + snapshot compare",
    },
    {
        "MC2_STATIC_PROP_NIGHT_TAGS",
        "MC2_STATIC_PROP_NIGHT_TAGS",
        EnvVarKind::Trace,
        false,
        "EDGE-LIGHT-EMISSION-1: restores the stock hot-colour NIGHT vertex tags on the GPU static-prop path.",
    },
    {
        "MC2_TUNE_STATIC_PROP_NIGHT_TAGS_SCALE",
        "MC2_STATIC_PROP_NIGHT_TAGS_SCALE",
        EnvVarKind::Trace,
        false,
        "EDGE-LIGHT-EMISSION-1: scales the NIGHT EMISSION DELTA only (night base_light minus day base_light), never the underlying lighting maths, so 0.0 is exactly...",
    },
    {
        "MC2_DIAG_STATIC_PROP_PBR_V1_SUNFOUND",
        "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
        EnvVarKind::Trace,
        false,
        "V-MATERIAL-PBR-3-DIAG: diagnostic visualizer for the forwarded sunFound state.",
    },
    {
        "MC2_TUNE_STATIC_PROP_PBR_V1_STRENGTH",
        "MC2_STATIC_PROP_PBR_V1_STRENGTH",
        EnvVarKind::Trace,
        false,
        "V-MATERIAL-PBR-3: optional default-strength override for per-fragment Schlick-Fresnel specular (clamped 0..3).",
    },
    {
        "MC2_FEATURE_STATIC_PROP_PERSISTENT_BUCKETS",
        "MC2_STATIC_PROP_PERSISTENT_BUCKETS",
        EnvVarKind::Feature,
        true,
        "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 (Mechanism B-reinject): persistent static instance store, rebuilt only on registry-generation change...",
    },
    {
        "MC2_TRACE_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE",
        "MC2_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 oracle: FNV-compare the persistent-store-sourced per-type instance stream vs a freshly-rebuilt legacy stream...",
    },
    {
        "MC2_TRACE_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE",
        "MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE",
        EnvVarKind::Trace,
        false,
        "STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1 gate: default OFF. =1 builds s_spRowCache from registry each frame and compares field-by-field with legacy propBuf.",
    },
    {
        "MC2_FEATURE_STATIC_PROXY_RECON",
        "MC2_STATIC_PROXY_RECON",
        EnvVarKind::Feature,
        false,
        "StaticSceneProxy-arc recon instrumentation (mclib/bdactor.cpp) - counts touchSerialCommit calls for building/tree appearances ahead of the per-frame...",
    },
    {
        "MC2_FEATURE_STATIC_UPDATE_SKIP",
        "MC2_STATIC_UPDATE_SKIP",
        EnvVarKind::Feature,
        true,
        "Skip the per-frame static building/tree update walk when nothing is dirty (GameOS/gameos/gameosmain.cpp, code/terrobj.cpp, code/bldng.cpp).",
    },
    {
        "MC2_FEATURE_TACTICAL_ARC_OVERLAY",
        "MC2_TACTICAL_ARC_OVERLAY",
        EnvVarKind::Feature,
        false,
        "TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line on each SELECTED mover, drawn in-scene by the debug renderer just before its flush...",
    },
    {
        "MC2_FEATURE_TERRAIN_AMBIENT_FLOOR",
        "MC2_TERRAIN_AMBIENT_FLOOR",
        EnvVarKind::Feature,
        true,
        "TERRAIN-AMBIENT-FLOOR-2: terrain-only, albedo-scaled ambient floor in terrain_lod_chunk.frag.",
    },
    {
        "MC2_FEATURE_TERRAIN_AMBIENT_FLOOR_NIGHT",
        "MC2_TERRAIN_AMBIENT_FLOOR_NIGHT",
        EnvVarKind::Feature,
        false,
        "TERRAIN-AMBIENT-FLOOR-NIGHT-1: 0..1 FLOOR under TERRAIN-AMBIENT-FLOOR-2's daylight gate; the uploaded gate becomes max(dayGate, floor).",
    },
    {
        "MC2_TRACE_TERRAIN_AMBIENT_FLOOR",
        "MC2_TERRAIN_AMBIENT_FLOOR_TRACE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-AMBIENT-FLOOR-2 trace: prints ONE line at the first terrain-chunk uniform upload giving the resolved floor strength, the daylight gate ACTUALLY...",
    },
    {
        "MC2_DIAG_TERRAIN_CLIFF_DEBUG",
        "MC2_TERRAIN_CLIFF_DEBUG",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-DEBUG: bounded debug-viz for the LOD-chunk terrain cliff material path (u_cliffDebug in terrain_lod_chunk.frag).",
    },
    {
        "MC2_FEATURE_TERRAIN_CLIFF_DISPLACE",
        "MC2_TERRAIN_CLIFF_DISPLACE",
        EnvVarKind::Feature,
        false,
        "SLICE 3: near-field cliff tessellation on the LOD-chunk terrain path.",
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_MAXTESS",
        "MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS",
        EnvVarKind::Trace,
        false,
        "SLICE 3 knob: max tessellation factor (float; C++ default 4.0, clamped to GL_MAX_TESS_GEN_LEVEL at program build).",
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH",
        "MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-HEIGHT-NORMAL-1 knob (float, default 2.0): derives the cliff SHADING normal from the HEIGHT GRADIENT of the cooked cliff displacement...",
    },
    {
        "MC2_FEATURE_TERRAIN_CLIFF_POM",
        "MC2_TERRAIN_CLIFF_POM",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CLIFF-POM-1: triplanar Parallax Occlusion Mapping on cliff faces.",
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_POM_DEPTH",
        "MC2_TERRAIN_CLIFF_POM_DEPTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-POM-1 knob: parallax depth = world-unit rock height scale relative to the CLIFF tiling (ts=256 wu/repeat).",
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_POM_STEPS",
        "MC2_TERRAIN_CLIFF_POM_STEPS",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-POM-1 knob: max POM march layers per plane, clamped 8..32 (default 24; compile-constant loop cap 32).",
    },
    {
        "MC2_FEATURE_TERRAIN_CLIFF_TRIPLANAR",
        "MC2_TERRAIN_CLIFF_TRIPLANAR",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: on steep terrain cells (macro slope, cliffBlend=smoothstep(0.85,0.55,|macroNz|)), project the MAT_LAYER_MARBLE_CLIFF...",
    },
    {
        "MC2_TUNE_TERRAIN_CLIFF_TRIPLANAR_STRENGTH",
        "MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1 knob: triplanar rock-normal tilt strength (float, default 1.0).",
    },
    {
        "MC2_TERRAIN_CONTROLMAP",
        "MC2_TERRAIN_CONTROLMAP",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CONTROLMAP-SAMPLE-1: load an authored per-mission RGBA control-map sidecar (data/missions/<stem>.beauty/control_map.png, vertex-resolution) that...",
    },
    {
        "MC2_TERRAIN_CONTROLMAP_FILE",
        "MC2_TERRAIN_CONTROLMAP_FILE",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CONTROLMAP-SAMPLE-1: override the control-map sidecar path (default data/missions/<stem>.beauty/control_map.png).",
    },
    {
        "MC2_DIAG_TERRAIN_DEBUG_MODE",
        "MC2_TERRAIN_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DEBUG-VIEWS-1: terrain fragment-shader debug-mode selector for the tessellated terrain path (gos_terrain.frag tessDebug.x).",
    },
    {
        "MC2_TERRAIN_DECAL_COLORBLEND",
        "MC2_TERRAIN_DECAL_COLORBLEND",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-colormap blend (float 0..1, default 0.35) for the CLIFF_WALL mesh-decal.",
    },
    {
        "MC2_TERRAIN_DECAL_FILL",
        "MC2_TERRAIN_DECAL_FILL",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-FILL-1: ambient/fill floor (float 0..1, default 0.20) for the CLIFF_WALL mesh-decal's shadow side.",
    },
    {
        "MC2_TERRAIN_DECAL_LATERAL",
        "MC2_TERRAIN_DECAL_LATERAL",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: lateral nudge (float world units, default 0.0) along the wall 'tangent' (contour) axis - side-to-side placement.",
    },
    {
        "MC2_TERRAIN_DECAL_LIFT",
        "MC2_TERRAIN_DECAL_LIFT",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: vertical nudge (float world units, default 0.0) along world-up - lifts/lowers the CLIFF_WALL mesh-decal.",
    },
    {
        "MC2_TERRAIN_DECAL_OFFSET",
        "MC2_TERRAIN_DECAL_OFFSET",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: outward offset distance (float world units, default 8.0) along the wall 'facing' axis - replaces the hardcoded 8.0 Slice-0A z-fight...",
    },
    {
        "MC2_TERRAIN_DECAL_PITCH",
        "MC2_TERRAIN_DECAL_PITCH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-PITCH: pitch/lean (float degrees, default 0.0) of the wall about the horizontal tangent (contour) axis - positive leans the wall TOP backward...",
    },
    {
        "MC2_TERRAIN_DECAL_SCALE",
        "MC2_TERRAIN_DECAL_SCALE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: uniform scale (float, default 1.0) of the CLIFF_WALL mesh-decal - multiplies the face-frame's 3 basis rows before translation.",
    },
    {
        "MC2_TERRAIN_DECAL_YAW",
        "MC2_TERRAIN_DECAL_YAW",
        EnvVarKind::Trace,
        false,
        "TERRAIN-DECAL-SLICE-0B: yaw rotation (float degrees, default 0.0) of the wall facing/tangent about world-up - corrects the outward-facing direction.",
    },
    {
        "MC2_TUNE_TERRAIN_HEIGHT_RESAMPLE_FACTOR",
        "MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR",
        EnvVarKind::Trace,
        false,
        "TERRAIN-RESAMPLE-1: CPU bilinear resample factor for the per-mission terrain height texture used by TERRAIN-NORMALS-FROM-HEIGHT-1.",
    },
    {
        "MC2_FEATURE_TERRAIN_LIGHT_POOLS",
        "MC2_TERRAIN_LIGHT_POOLS",
        EnvVarKind::Feature,
        true,
        "NIGHT-LIGHT-POOLS-1: per-fragment terrain light-pool loop on the LIVE LOD-chunk terrain path (terrain_lod_chunk.frag).",
    },
    {
        "MC2_FEATURE_TERRAIN_LOD_CHUNK",
        "MC2_TERRAIN_LOD_CHUNK",
        EnvVarKind::Feature,
        true,
        "LOD-chunk terrain renderer (mclib/terrain.cpp/terrain.h), the primary tessellation-free terrain path.",
    },
    {
        "MC2_FEATURE_TERRAIN_MATERIAL_TEXTURES",
        "MC2_TERRAIN_MATERIAL_TEXTURES",
        EnvVarKind::Feature,
        false,
        "TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB GL_TEXTURE_2D_ARRAY albedo (rock/grass/dirt/concrete/snow/cliff from...",
    },
    {
        "MC2_TUNE_TERRAIN_MATERIAL_TEXTURES_STRENGTH",
        "MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH",
        EnvVarKind::Trace,
        false,
        "TERRAIN-MATERIAL-TEXTURES-1 knob: albedo mix strength override (float, clamped 0..1).",
    },
    {
        "MC2_TERRAIN_OVERLAY_V2",
        "MC2_TERRAIN_OVERLAY_V2",
        EnvVarKind::Feature,
        false,
        "TERRAIN-OVERLAY-V2-PARITY-1: composite an authored world-XY-bounded cement/pad/runway overlay sidecar (data/missions/<stem>.beauty/overlay_v2.png...",
    },
    {
        "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS",
        "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS",
        EnvVarKind::Feature,
        true,
        "TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: in the static decal bake, skip legacy runway/road/cement decals for tiles fully inside the armed OVERLAY_V2 sidecar...",
    },
    {
        "MC2_TERRAIN_OVERLAY_V2_FILE",
        "MC2_TERRAIN_OVERLAY_V2_FILE",
        EnvVarKind::Feature,
        false,
        "TERRAIN-OVERLAY-V2-PARITY-1: override the overlay_v2 sidecar path (default data/missions/<stem>.beauty/overlay_v2.png).",
    },
    {
        "MC2_FEATURE_TERRAIN_POM",
        "MC2_TERRAIN_POM",
        EnvVarKind::Feature,
        false,
        "TERRAIN-CHUNK-POM-1: real per-fragment tangent-space view-vector POM on the LOD-chunk terrain (rock/grass detail layers).",
    },
    {
        "MC2_TUNE_TERRAIN_POM_FAR",
        "MC2_TERRAIN_POM_FAR",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: POM distance-fade band end in world units (default 3500).",
    },
    {
        "MC2_TUNE_TERRAIN_POM_NEAR",
        "MC2_TERRAIN_POM_NEAR",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: POM distance-fade band start in world units (default 1500; 1 tile = 384 wu).",
    },
    {
        "MC2_TUNE_TERRAIN_POM_SCALE",
        "MC2_TERRAIN_POM_SCALE",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: POM march scale override (float > 0).",
    },
    {
        "MC2_TUNE_TERRAIN_POM_STEPS",
        "MC2_TERRAIN_POM_STEPS",
        EnvVarKind::Trace,
        false,
        "TERRAIN-CHUNK-POM-1 knob: max POM march layers, accepted 4..16 (else default 16; hard compile-constant cap 16).",
    },
    {
        "MC2_FEATURE_TERRAIN_SHADOW_GUARD_CLAMPED_SUN",
        "MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN",
        EnvVarKind::Feature,
        true,
        "TERRAIN-SHADOW-GUARD-1: the TERRAIN-side twin of SHADOW-SUNCLAMP-1, and the one that actually moves world pixels.",
    },
    {
        "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE",
        "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE",
        EnvVarKind::Feature,
        true,
        "TERRAIN-VISUAL-HEIGHT-SAMPLE-1: when visual terrain displacement is on, fade object grounding across the displaced-vs-gameplay height delta so props/units...",
    },
    {
        "MC2_FEATURE_TERRAIN_VISUAL_HEIGHT",
        "MC2_TERRAIN_VISUAL_HEIGHT",
        EnvVarKind::Feature,
        false,
        "TERRAIN-VISUAL-HEIGHT: loads the 4x-res VISUAL heightfield bake into an SSBO for corner-pinned interior displacement (mclib/terrain.cpp).",
    },
    {
        "MC2_TRACE_TEST_SPAWN",
        "MC2_TEST_SPAWN",
        EnvVarKind::Trace,
        false,
        "GAMEPLAY-DRIVER-1: =1 arms the test-only spawn/teleport verbs on the MC2_UI_COMMAND_FILE channel.",
    },
    {
        "MC2_TRACE_THIN_CANARY",
        "MC2_THIN_CANARY",
        EnvVarKind::Trace,
        false,
        "TRACKV: gates Probe-6 thin-record canary readback (per-frame ~80KB glGetBufferSubData x2 + CPU recipeIdx/flags compare, diagnostic for the corrupt-recipeIdx...",
    },
    {
        "MC2_TONEMAP_V2",
        "MC2_TONEMAP_V2",
        EnvVarKind::Feature,
        false,
        "POST-FX-TONEMAP-V2: optional filmic tonemap curve on the final composited colour (postprocess.frag).",
    },
    {
        "MC2_TREE_DIAG_TRACE",
        "MC2_TREE_DIAG_TRACE",
        EnvVarKind::Trace,
        false,
        "[DEFAULT-OFF, but the OFF token is UNSET, not '0'.]",
    },
    {
        "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
        "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the additive-only GPU-particle RGB brightness scale (particle_billboard.frag u_vfxAdditiveBrightness, applied ONLY to...",
    },
    {
        "MC2_TUNE_VFX_AGE_FADE",
        "MC2_TUNE_VFX_AGE_FADE",
        EnvVarKind::Trace,
        false,
        "VFX-SHADER-AGE-FADE-PARITY-1: soft death fade for oracle-harvested particles (p.lifetime==1.0f sentinel) in the final 30% of normalized age.",
    },
    {
        "MC2_TUNE_VFX_ALPHA_SCALE",
        "MC2_TUNE_VFX_ALPHA_SCALE",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the GPU-particle alpha (opacity) scale (particle_billboard.frag u_vfxAlphaScale, applied to ALL particles).",
    },
    {
        "MC2_TUNE_VFX_BRIGHTNESS",
        "MC2_TUNE_VFX_BRIGHTNESS",
        EnvVarKind::Trace,
        false,
        "VFX-TUNING-UI-1: startup-default for the global GPU-particle RGB brightness scale (particle_billboard.frag u_vfxBrightness, applied to ALL particles).",
    },
    {
        "MC2_TUNE_VFX_LIT_STRENGTH",
        "MC2_TUNE_VFX_LIT_STRENGTH",
        EnvVarKind::Trace,
        false,
        "VFX-LIT-PARTICLES-MVP-1: startup-default for the scene-lit smoke/dust strength (particle_billboard.frag u_vfxLitStrength), applied ONLY to alpha draw groups...",
    },
    {
        "MC2_TRACE_UI_COMMAND_FILE",
        "MC2_UI_COMMAND_FILE",
        EnvVarKind::Trace,
        false,
        "UI-COMMAND-CHANNEL-1 + GAMEPLAY-DRIVER-1/2: =<path> polls that file once per frame (MissionInterfaceManager::update) and executes its lines as commands.",
    },
    {
        "MC2_TRACE_UI_IMGUI_INFO",
        "MC2_UI_IMGUI_INFO",
        EnvVarKind::Trace,
        false,
        "INFO-IMGUI-SLICE-1: =1 renders the in-mission unit-Info panel as an ImGui window (unit name + health bar + loadout list with scrollbar) laid out inside...",
    },
    {
        "MC2_TRACE_UI_IMGUI_OBJ",
        "MC2_UI_IMGUI_OBJ",
        EnvVarKind::Trace,
        false,
        "OBJ-IMGUI-SLICE-1: =1 renders the in-mission OBJECTIVES overlay as ImGui (baked left-edge frame from tools/ui_art_reconstruct.py + procedural...",
    },
    {
        "MC2_TRACE_UI_IMGUI_PAUSE",
        "MC2_UI_IMGUI_PAUSE",
        EnvVarKind::Trace,
        false,
        "PAUSE-IMGUI-SLICE-1: =1 renders the in-mission pause menu as an ImGui window (GAME PAUSED + objectives/save/load/options/abort/exit/return) laid out inside...",
    },
    {
        "MC2_TRACE_UI_IMGUI_TIMER",
        "MC2_UI_IMGUI_TIMER",
        EnvVarKind::Trace,
        false,
        "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission mission-timer display as ImGui (right-aligned MM:SS via HudFont + the TimerStatic atlas tile) laid out...",
    },
    {
        "MC2_TRACE_UI_IMGUI_VIDEO",
        "MC2_UI_IMGUI_VIDEO",
        EnvVarKind::Trace,
        false,
        "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission comm-video panel as ImGui - VideoStatic chrome tiles + the decoder's frame texture at the same...",
    },
    {
        "MC2_FEATURE_UNIT_PROFILE_DATA",
        "MC2_UNIT_PROFILE_DATA",
        EnvVarKind::Feature,
        false,
        "UNIT-PROFILE-ARC: data-driven unit+equipment definition source (code/unitprofile.cpp).",
    },
    {
        "MC2_DIAG_VFX_DEBUG_MODE",
        "MC2_VFX_DEBUG_MODE",
        EnvVarKind::Trace,
        false,
        "VFX-DEBUG-VIEWS-1: GPU particle billboard fragment-shader debug-mode selector (particle_billboard.frag u_debugMode, uploaded by gos_particle_bridge).",
    },
    {
        "MC2_VFX_ORACLE_TUBE",
        "MC2_VFX_ORACLE_TUBE",
        EnvVarKind::Feature,
        false,
        "GOSFX-TUBE-RIBBON-1: route supported gosFX Tube trails (MissileSmoke alpha, PPC additive, polygon) through the swept-quad ribbon oracle submit path instead...",
    },
    {
        "MC2_VFX_ORACLE_TUBE_COVERAGE",
        "MC2_VFX_ORACLE_TUBE_COVERAGE",
        EnvVarKind::Trace,
        false,
        "GOSFX-TUBE-RIBBON-1: wrap the deferred ribbon draw in a GL_SAMPLES_PASSED occlusion query; logs [VFX_ORACLE_TUBE coverage] ribbons/samples/fbo/sceneDrawBufs...",
    },
    {
        "MC2_VFX_ORACLE_TUBE_LOG",
        "MC2_VFX_ORACLE_TUBE_LOG",
        EnvVarKind::Trace,
        false,
        "GOSFX-TUBE-RIBBON-1: per-Tube ribbon submit counters (FIRST_HARVEST / v2 vertex+index tallies, unsupported->MLR fallbacks).",
    },
    {
        "MC2_TUNE_VIEWMODE_LOWLIGHT_GAIN",
        "MC2_VIEWMODE_LOWLIGHT_GAIN",
        EnvVarKind::Trace,
        false,
        "LOWLIGHT-NIGHTVISION-MVP-1: night-vision luminance amplification (clamped 0.1..16, default 2.5).",
    },
    {
        "MC2_TUNE_VIEWMODE_LOWLIGHT_TINT",
        "MC2_VIEWMODE_LOWLIGHT_TINT",
        EnvVarKind::Trace,
        false,
        "LOWLIGHT-NIGHTVISION-MVP-1: night-vision tint as 'r,g,b' floats (default 0.7,1.0,0.6 green-phosphor).",
    },
    {
        "MC2_TUNE_VIEW_MODE",
        "MC2_VIEW_MODE",
        EnvVarKind::Trace,
        false,
        "VIEWMODE-POSTPROCESS-PRESENTATION-1: seeds the startup presentation mode (matches RenderCore::ViewMode).",
    },
    {
        "MC2_TRACE_VISUAL_CAPTURE_ON_FIRE",
        "MC2_VISUAL_CAPTURE_ON_FIRE",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-8: GL twin of MC2_VK_PRESENT_DUMP_ON_FIRE.",
    },
    {
        "MC2_FEATURE_VK_CSM_ARRAY_INTEROP",
        "MC2_VK_CSM_ARRAY_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-CSM-ARRAY-DIRECT-1: presence-gated =1, default OFF.",
    },
    {
        "MC2_FEATURE_VK_CSM_FULLMAP_INTEROP",
        "MC2_VK_CSM_FULLMAP_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-CSM-FULLMAP-DIRECT-1: presence-gated =1, default OFF.",
    },
    {
        "MC2_TRACE_VK_DECAL_STATIC_GL_CANARY",
        "MC2_VK_DECAL_STATIC_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps gosRenderer::drawDecalStaticBatch GL draw loop in GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1]...",
    },
    {
        "MC2_FEATURE_VK_FORCE_FAIL",
        "MC2_VK_FORCE_FAIL",
        EnvVarKind::Trace,
        false,
        "VK-P13-SELECTOR-1: presence-gated =1, default OFF.",
    },
    {
        "MC2_FEATURE_VK_IMGUI",
        "MC2_VK_IMGUI",
        EnvVarKind::Feature,
        false,
        "VK-IMGUI-NATIVE-1 (rung 8.6): default-OFF ImGui Vulkan renderer gate.",
    },
    {
        "MC2_FEATURE_VK_MECH_LIGHTING",
        "MC2_VK_MECH_LIGHTING",
        EnvVarKind::Feature,
        false,
        "VK-MECH-MATERIAL-LIGHTING-1: presence-gated =1, default OFF; REQUIRES MC2_VK_MECH_TIER1 (visible only with MC2_VK_MECH_MATERIAL=1).",
    },
    {
        "MC2_FEATURE_VK_MECH_MATERIAL",
        "MC2_VK_MECH_MATERIAL",
        EnvVarKind::Feature,
        false,
        "VK-MECH-MATERIAL: presence-gated =1, default OFF (requires MC2_VK_MECH_TIER1).",
    },
    {
        "MC2_FEATURE_VK_MECH_TIER1",
        "MC2_VK_MECH_TIER1",
        EnvVarKind::Feature,
        false,
        "VK-MECH-TIER1: presence-gated =1, default OFF (requires MC2_VK_SHARED_SCENE_DEPTH).",
    },
    {
        "MC2_TRACE_VK_NO_GL_INTEROP_RESOURCE_STRICT",
        "MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT",
        EnvVarKind::Trace,
        false,
        "VK Metal-gate resource canary.",
    },
    {
        "MC2_TRACE_VK_NO_GL_INTEROP_STRICT",
        "MC2_VK_NO_GL_INTEROP_STRICT",
        EnvVarKind::Trace,
        false,
        "VK Metal-gate canary.",
    },
    {
        "MC2_VK_PICK_PROBE",
        "MC2_VK_PICK_PROBE",
        EnvVarKind::Trace,
        false,
        "BUG-A pick regression fence: default-OFF.",
    },
    {
        "MC2_FEATURE_VK_POST_COMPOSITE_DIRECT_CHAIN",
        "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN",
        EnvVarKind::Feature,
        false,
        "VK-POST-COMPOSITE-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF.",
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_FRAME",
        "MC2_VK_PRESENT_DUMP_FRAME",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-3: frame pin for MC2_VK_PRESENT_DUMP_PPM.",
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_ON_FIRE",
        "MC2_VK_PRESENT_DUMP_ON_FIRE",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-5: dynamic fire-event pin for MC2_VK_PRESENT_DUMP_PPM.",
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_PPM",
        "MC2_VK_PRESENT_DUMP_PPM",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-ORACLE-1: headless present-pixel oracle.",
    },
    {
        "MC2_TRACE_VK_PRESENT_NATIVE_STRICT",
        "MC2_VK_PRESENT_NATIVE_STRICT",
        EnvVarKind::Trace,
        false,
        "VK present-tail deletion-readiness canary.",
    },
    {
        "MC2_FEATURE_VK_PRESENT_NO_GL_IMPORT",
        "MC2_VK_PRESENT_NO_GL_IMPORT",
        EnvVarKind::Feature,
        false,
        "VK present-tail ownership gate.",
    },
    {
        "MC2_FEATURE_VK_PRESENT_TAIL_HOOK",
        "MC2_VK_PRESENT_TAIL_HOOK",
        EnvVarKind::Feature,
        false,
        "VK-TAIL-HOOK-1 (rung 8.6): default-OFF present-command-buffer seam for native final-tail rendering.",
    },
    {
        "MC2_FEATURE_VK_PROP_COLOR_TIER1",
        "MC2_VK_PROP_COLOR_TIER1",
        EnvVarKind::Feature,
        false,
        "VK-P11 TIER-1 PROP COLOR: presence-gated =1, default OFF; REQUIRES MC2_VK_SHARED_SCENE_DEPTH.",
    },
    {
        "MC2_FEATURE_VK_PROP_DIRECT_CHAIN",
        "MC2_VK_PROP_DIRECT_CHAIN",
        EnvVarKind::Feature,
        false,
        "VK-PROP-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF.",
    },
    {
        "MC2_FEATURE_VK_PROP_MATERIAL",
        "MC2_VK_PROP_MATERIAL",
        EnvVarKind::Feature,
        false,
        "VK-PROP-MATERIAL: presence-gated =1, default OFF (requires MC2_VK_PROP_COLOR_TIER1).",
    },
    {
        "MC2_FEATURE_VK_PROP_MATERIAL_DEBUG",
        "MC2_VK_PROP_MATERIAL_DEBUG",
        EnvVarKind::Feature,
        false,
        "VK-PROP-MATERIAL-DEBUG: presence-gated =1, default OFF.",
    },
    {
        "MC2_TRACE_VK_SCENE_GL_PRODUCER_QUERY",
        "MC2_VK_SCENE_GL_PRODUCER_QUERY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps the sceneFBO producer phase in a GL_ANY_SAMPLES_PASSED query and logs [VK_SCENE_GL_PRODUCER_QUERY v1] any_samples=... so...",
    },
    {
        "MC2_TRACE_VK_SCREENSHADOW_GL_CANARY",
        "MC2_VK_SCREENSHADOW_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps only the legacy GL runScreenShadow fullscreen draw in GL_ANY_SAMPLES_PASSED and logs [VK_SCREENSHADOW_GL_CANARY v1].",
    },
    {
        "MC2_FEATURE_VK_SHADOW_INTEROP",
        "MC2_VK_SHADOW_INTEROP",
        EnvVarKind::Feature,
        false,
        "VK-P8a SHADOW-INTEROP: presence-gated =1, default OFF.",
    },
    {
        "MC2_TRACE_VK_SHADOW_MATRIX_DIAG",
        "MC2_VK_SHADOW_MATRIX_DIAG",
        EnvVarKind::Trace,
        false,
        "VK shadow producer/consumer evidence (default OFF = zero cost; GameOS/gameos/vulkan_shadow_island_live.cpp).",
    },
    {
        "MC2_FEATURE_VK_SHARED_GBUFFER",
        "MC2_VK_SHARED_GBUFFER",
        EnvVarKind::Feature,
        false,
        "VK-P14 GATE-CONSOLIDATION-1: presence-gated =1, default OFF.",
    },
    {
        "MC2_FEATURE_VK_SHARED_SCENE_DEPTH",
        "MC2_VK_SHARED_SCENE_DEPTH",
        EnvVarKind::Feature,
        false,
        "VK-TIER1 SHARED SCENE DEPTH: presence-gated =1, default OFF.",
    },
    {
        "MC2_TRACE_VK_SHARED_SCENE_STRICT",
        "MC2_VK_SHARED_SCENE_STRICT",
        EnvVarKind::Trace,
        false,
        "VK shared-scene deletion-readiness canary.",
    },
    {
        "MC2_FEATURE_VK_STATIC_SHADOW_MAP",
        "MC2_VK_STATIC_SHADOW_MAP",
        EnvVarKind::Feature,
        false,
        "DEPRECATED no-op (VK-RUNG8-ITEM6).",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_CSM",
        "MC2_VK_TERRAIN_CSM",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-CSM (biggest-safe-subset): presence-gated =1, default OFF (requires MC2_VK_TERRAIN_DYNSHADOW chain + CSM active).",
    },
    {
        "MC2_TRACE_VK_TERRAIN_DECAL_GL_CANARY",
        "MC2_VK_TERRAIN_DECAL_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps gosRenderer::drawDecals GL draw loop in GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1] tag=terrain_decal_batch.",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_DETAIL",
        "MC2_VK_TERRAIN_DETAIL",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-DETAIL: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_LIGHTING).",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_DYNSHADOW",
        "MC2_VK_TERRAIN_DYNSHADOW",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-DYNSHADOW: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_LIGHTING).",
    },
    {
        "MC2_TRACE_VK_TERRAIN_GL_FALLBACK_AUDIT",
        "MC2_VK_TERRAIN_GL_FALLBACK_AUDIT",
        EnvVarKind::Trace,
        false,
        "VK terrain ownership diagnostic: under the max-native terrain gates, counts terrain chunks captured by the Vulkan feed vs chunks that still fall through to...",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_HEIGHT_AUDIT",
        "MC2_VK_TERRAIN_HEIGHT_AUDIT",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN HEIGHT-AUDIT (verification-only, default OFF; requires MC2_VK_TERRAIN_TIER1).",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT",
        "MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN HEIGHT-AUDIT NEGATIVE CONTROL (test-only, default OFF; requires MC2_VK_TERRAIN_HEIGHT_AUDIT).",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_LIGHTING",
        "MC2_VK_TERRAIN_LIGHTING",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-LIGHTING: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_TIER1).",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_LODFADE",
        "MC2_VK_TERRAIN_LODFADE",
        EnvVarKind::Feature,
        false,
        "VK-TERRAIN-LODFADE: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_DETAIL).",
    },
    {
        "MC2_TRACE_VK_TERRAIN_OVERLAY_GL_CANARY",
        "MC2_VK_TERRAIN_OVERLAY_GL_CANARY",
        EnvVarKind::Trace,
        false,
        "VK final-gate diagnostic: wraps gosRenderer::drawTerrainOverlays GL draw loop in GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1]...",
    },
    {
        "MC2_FEATURE_VK_TERRAIN_TIER1",
        "MC2_VK_TERRAIN_TIER1",
        EnvVarKind::Feature,
        false,
        "VK-P10 TERRAIN TIER-1: presence-gated =1, default OFF (requires MC2_VK_SHARED_SCENE_DEPTH).",
    },
    {
        "MC2_FEATURE_VK_VFX",
        "MC2_VK_VFX",
        EnvVarKind::Feature,
        false,
        "VK-FIELD-VFX-1: umbrella sub-gate, default-ON under MC2_VK_SHARED_GBUFFER (explicit =0 forces off, =1 forces on).",
    },
    {
        "MC2_FEATURE_VK_VFX_COALESCE",
        "MC2_VK_VFX_COALESCE",
        EnvVarKind::Feature,
        true,
        "VK-VFX-COALESCE-1: merges consecutive same-render-state HUD tri-batch draws (gosRenderer::drawTris IsHUD branch, gameos_graphics.cpp) into ONE hudBatch_...",
    },
    {
        "MC2_TRACE_VK_VFX_TRACE",
        "MC2_VK_VFX_TRACE",
        EnvVarKind::Trace,
        false,
        "VK-FIELD-VFX-2: per-bolt weapon-fire pipeline trace.",
    },
    {
        "MC2_FEATURE_VK_WATER_DIRECT_CHAIN",
        "MC2_VK_WATER_DIRECT_CHAIN",
        EnvVarKind::Feature,
        false,
        "VK-WATER-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF.",
    },
    {
        "MC2_FEATURE_WATER_SHINE",
        "MC2_WATER_SHINE",
        EnvVarKind::Feature,
        false,
        "[DEFAULT-OFF OPT-IN.",
    },
    {
        "MC2_TUNE_WATER_SHORE3_DEPTH_K",
        "MC2_WATER_SHORE3_DEPTH_K",
        EnvVarKind::Feature,
        false,
        "Shore mode 3 tuning: u_shoreDepthK, fraction of clip.z added to the flat water plane depth (depth-only lift so coarse terrain triangles do not carve...",
    },
    {
        "MC2_TUNE_WATER_SHORE3_EXTRA_WU",
        "MC2_WATER_SHORE3_EXTRA_WU",
        EnvVarKind::Feature,
        false,
        "Shore mode 3 (MC2_WATER_SHORE_MODE=3) tuning: extra world units added to the water admission extent beyond the sidecar contour so geometry exists for the...",
    },
    {
        "MC2_FEATURE_WATER_SHORE_MASK",
        "MC2_WATER_SHORE_MASK",
        EnvVarKind::Feature,
        true,
        "WATER-SHORE-MASK-1: per-mission shoreline mask sidecar (data/missions/<m>.beauty/shoreline_mask.png) drives the water shore edge.",
    },
    {
        "MC2_FEATURE_WATER_SHORE_MEANDER",
        "MC2_WATER_SHORE_MEANDER",
        EnvVarKind::Feature,
        false,
        "SHORELINE-GEOMETRY-1: fBm meander amplitude in world units applied to the shore contour in gos_terrain_water_stream.cpp ShoreMeanderWu(); breaks the...",
    },
    {
        "MC2_TUNE_WATER_SHORE_MEANDER_FREQ",
        "MC2_WATER_SHORE_MEANDER_FREQ",
        EnvVarKind::Feature,
        false,
        "SHORELINE-GEOMETRY-1 companion tuning knob for MC2_WATER_SHORE_MEANDER: spatial frequency of the shore-contour fBm, in cycles per world unit...",
    },
    {
        "MC2_TRACE_WINDOW_OFFSCREEN",
        "MC2_WINDOW_OFFSCREEN",
        EnvVarKind::Trace,
        false,
        "NOFOCUS-HARDEN-1: create the SDL window HIDDEN at (-32000,-32000) and keep it there, so an agent-launched engine window never occupies visible desktop area...",
    },
    {
        "MC2_XFORM_PARITY_FATAL",
        "MC2_XFORM_PARITY_FATAL",
        EnvVarKind::Trace,
        false,
        "XFORM-CONVENTION-HARNESS-1: promote the F1-3C clip-space parity probe (code/gamecam.cpp) to a HARD invariant.",
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
