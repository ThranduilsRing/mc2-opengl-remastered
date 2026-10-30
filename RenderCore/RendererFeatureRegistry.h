// RenderCore/RendererFeatureRegistry.h
//
// Header-only inventory of renderer feature gates and device capabilities.
// INVENTORY ONLY -- no feature decisions here. Each system's own env-flag
// accessor (e.g. RenderWorld::IsObjectIdBufferEnabled) remains
// authoritative for runtime decisions. This header is the single place to
// look up what env vars exist and what their defaults are.
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
//   debugDumpFeatureRegistry(caps);   // optional legacy diagnostic banner
//
// Firewall: MUST NOT include any GL header or game-side header.

#pragma once

#include <cstdint>
#include <cstdio>
#include "Gates/gate_cold.h"   // boolState(): the dump names gates from its own table

namespace RenderCore {

// ---------------------------------------------------------------------------
// Device capabilities
// ---------------------------------------------------------------------------
// Legacy diagnostic payload; field names and layout are retained for compatibility.
// Zero-initialised default = "unknown / not queried yet". These fields do not
// describe the active Vulkan device; see GameOS/gameos/vk_device_profile.h.
// Nothing in this header queries a graphics API.

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
    GpuMechs            = 0,   // (retired; env var MC2_GPU_MECHS removed)
    // [Retired] superseded by RenderWorld::upsertStaticProp; no new getenv consumers
    StaticPropIndirect  = 1,   // (retired; env var MC2_GPU_OBJECTS removed)
    // Feature gates
    ObjectIdBuffer      = 2,   // MC2_OBJECT_ID_BUFFER
    TerrainTessellation = 3,   // MC2_TERRAIN_TESS (default-ON kill-switch, ISSUE-34)
    ReverseZ            = 4,   // reverse-Z is unconditional
    ShadowMaps          = 5,   // MC2_SHADOW_ENABLE
    ImGui               = 6,   // MC2_IMGUI
    ImGuiInspector      = 7,   // MC2_IMGUI_INSPECTOR
    DebugRenderer       = 8,   // retired; preserve the numeric identifier
    MaterialGpu         = 9,   // MC2_MATERIAL_GPU
    MaterialGpuSample   = 10,  // MC2_MATERIAL_GPU_SAMPLE
    MaterialKtx         = 11,  // authored sidecars, always enabled
    // F1-3A ViewUniforms UBO upload (upload-only; F1-3B adds shader consumption).
    ViewUniforms             = 12,  // MC2_VIEW_UNIFORMS
    // MECH-EXTRACTION-0: mech snapshot extraction (observe-only).
    SnapshotMechExtract      = 13,  // retired; now the MC2_SNAPSHOT_MECH_EXTRACT trace gate
    // Retired snapshot-spawn gate; preserve the numeric identifier.
    VfxAgeSample             = 14,  // retired
    // Retired harvest selector; preserve the numeric identifier.
    VfxOracleRender          = 15,  // retired
    // TRACKV-POST-GROUNDING-MVP: experimental visual post-process + grounding
    // gates. ALL default-OFF (enforced by the Track-V guardrail unit test).
    // Promotion to default-ON is a deliberate edit here AND in that test.
    // VIEWMODE-POSTPROCESS-PRESENTATION-1: view-mode presentation framework.
    ViewmodeFramework        = 16,  // MC2_VIEWMODE_FRAMEWORK
    // Default-ON render-only prop occlusion from same-frame Vulkan depth.
    HzbBuild                 = 17,  // MC2_HZB_BUILD
    // Compatible BC7 sidecars use compressed buckets; absent or incompatible
    // sidecars retain the RGBA fallback.
    StaticPropBc7            = 18,  // retired; preserve the numeric identifier
    // COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha immediately after
    // BuildColormapAtlas uploads to GPU. The indirect path (default-ON) has no
    // geometry displacement (thin vert, no TES), so the CPU elevation's null-guard
    // at mapdata.cpp:2435 becomes the correct state. Default-ON; kill-switch =0.
    ColormapCpuRetire        = 19,  // MC2_COLORMAP_CPU_RETIRE
    // COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas.
    // Falls back to RGBA8 when sidecar absent or BPTC cap missing. Default-ON.
    ColormapKtx2             = 20,  // MC2_COLORMAP_KTX2
    // TRACKV-CI-GATE-RESTORE-1: setupTextures legacy path kill-switch.
    // Default-OFF = new/optimized setupTextures path active; ON = legacy path forced.
    SetupTexturesLegacyForce = 21,  // retired; preserve the numeric identifier
    // VEHICLE-NIGHT-LIT-1: coordinate-frame fix for the legacy lit shader path
    // (gos_tex_vertex_lighted.vert) — the program that draws ground vehicles.
    // It fed calc_light Stuff-frame position + normal while the ObjectLights
    // rows (and the mech.vert / static_prop.vert consumers) are MC2-frame, so
    // every positional light type failed its falloff distance test and vehicles
    // received no pool / cone light at night. Default-OFF -> byte-identical.
    LegacyLitMc2Frame        = 22,  // MC2_LEGACY_LIT_MC2_FRAME
    LightPosColumnFix        = 23,  // MC2_LIGHT_POS_COLUMN_FIX
    // VEHICLE-LIGHTING-PATH-1: populate the ground-vehicle light row before the
    // legacy draw. GVAppearance called neither CacheGpuLightData nor
    // submitMultiShape, so its lightData_ stayed all-zero and calc_light
    // returned vec3(1,1,1) -> vehicles rendered at raw albedo, fully unlit.
    // Default-ON; =0 restores the shipped unlit behavior.
    VehicleLitPath           = 24,  // MC2_VEHICLE_LIT_PATH
    // OVERLAY-CEMENT-ADOPT-1 (CONCRETE-DELIVER-1): on a cement tile the
    // overlay/decal layer adopts terrain_lod_chunk.frag's concrete albedo +
    // lighting model, closing the two-shader seam that made concrete a
    // patchwork -- including the in-shader distance fog the overlay applies
    // and the chunk frag does not. Default-ON; =0 restores stock.
    OverlayCementAdopt       = 25,  // MC2_OVERLAY_CEMENT_ADOPT
    // VEHICLE-AMBIENT-FLOOR-1: finishes VEHICLE-LIGHTING-PATH-1 by giving the
    // legacy lit vehicle path mech.vert's kAmbientFloor = 0.35 seed and its
    // clamp(0,1). Without them a gathered vehicle computes base_light+A+S*ndl
    // against a mech's clamp(0.35+A+S*ndl,0,1), so the truck still reads darker
    // than the mech beside it. Default-ON; =0 restores no-floor/no-clamp.
    VehicleAmbientFloor      = 26,  // MC2_VEHICLE_AMBIENT_FLOOR
    COUNT                    = 27,
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
        "MC2_FEATURE_GPU_MECHS", nullptr, EnvVarKind::Retired, true,
        "Retired: the GPU mech batcher is the only world-mech path."
    },
    // [1] StaticPropIndirect
    {
        "MC2_FEATURE_STATIC_PROP_INDIRECT", nullptr, EnvVarKind::Retired, true,
        "Retired: the static-prop batcher is the only world-prop path."
    },
    // [2] ObjectIdBuffer
    {
        "MC2_FEATURE_OBJECT_ID_BUFFER", "MC2_OBJECT_ID_BUFFER", EnvVarKind::Feature, true,
        "R32_UINT MRT attachment-2 with per-pixel object handles (RenderWorld M1.5)."
    },
    // [3] TerrainTessellation
    {
        "MC2_FEATURE_TERRAIN_TESSELLATION", "MC2_TERRAIN_TESS", EnvVarKind::Feature, true,
        "TCS/TES terrain tessellation (gameos_graphics.cpp)."
    },
    // [4] ReverseZ
    {
        "MC2_FEATURE_REVERSE_Z", nullptr, EnvVarKind::Feature, true,
        "Reverse-Z depth via glClipControl GL_ZERO_TO_ONE (gameosmain.cpp:992). Always-on; MC2_REVERSE_Z_TRACE for trace."
    },
    // [5] ShadowMaps
    {
        "MC2_FEATURE_SHADOW_MAPS", "MC2_SHADOW_ENABLE", EnvVarKind::Feature, true,
        "Dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow)."
    },
    // [6] ImGui
    {
        "MC2_FEATURE_IMGUI", "MC2_IMGUI", EnvVarKind::Feature, true,
        "ImGui overlay (GuiRuntime/GuiRuntime.cpp)."
    },
    // [7] ImGuiInspector
    {
        "MC2_FEATURE_IMGUI_INSPECTOR", "MC2_IMGUI_INSPECTOR", EnvVarKind::Feature, true,
        "ImGui inspector panel (GuiRuntime/EditorInspector.cpp)."
    },
    // [8] DebugRenderer
    {
        "MC2_FEATURE_DEBUG_RENDERER", nullptr, EnvVarKind::Retired, false,
        "Retired: the debug primitive renderer is always on."
    },
    // [9] MaterialGpu
    {
        "MC2_FEATURE_MATERIAL_GPU", "MC2_MATERIAL_GPU", EnvVarKind::Feature, true,
        "GPU material table upload/bind/compare for static props (default-ON as of v5, 2026-05-26)."
    },
    // [10] MaterialGpuSample
    {
        "MC2_FEATURE_MATERIAL_GPU_SAMPLE", "MC2_MATERIAL_GPU_SAMPLE", EnvVarKind::Feature, true,
        "GPU material albedo sampling in static_prop.frag (default-ON as of v7, 2026-05-26)."
    },
    // [11] MaterialKtx
    {
        "MC2_FEATURE_MATERIAL_KTX", nullptr, EnvVarKind::Feature, true,
        "Load authored KTX2 texture sidecars, falling back to stock textures when absent."
    },
    // [12] ViewUniforms
    {
        "MC2_FEATURE_VIEW_UNIFORMS", "MC2_VIEW_UNIFORMS", EnvVarKind::Feature, true,
        "ViewUniforms UBO at binding=3."
    },
    // [13] SnapshotMechExtract
    {
        "MC2_FEATURE_SNAPSHOT_MECH_EXTRACT", nullptr, EnvVarKind::Retired, false,
        "Retired: reclassified as the MC2_SNAPSHOT_MECH_EXTRACT trace gate (observe-only diagnostic)."
    },
    // [14] VfxAgeSample
    {
        "MC2_FEATURE_VFX_AGE_SAMPLE", nullptr, EnvVarKind::Retired, false,
        "Retired: unused snapshot spawners removed; live gosFX state supplies particle ages."
    },
    // [15] VfxOracleRender
    {
        "MC2_FEATURE_VFX_ORACLE_RENDER", nullptr, EnvVarKind::Retired, false,
        "Retired: live-state harvesting is unconditional within each migrated draw route."
    },
    // [16] ViewmodeFramework
    {
        "MC2_FEATURE_VIEWMODE_FRAMEWORK", "MC2_VIEWMODE_FRAMEWORK", EnvVarKind::Feature, false,
        "VIEWMODE-POSTPROCESS-PRESENTATION-1: enables the view-mode presentation framework that wires ViewMode::ObjectIdDebug (and future sensor modes) through the..."
    },
    // [17] HzbBuild
    {
        "MC2_FEATURE_HZB_BUILD", "MC2_HZB_BUILD", EnvVarKind::Feature, true,
        "Render-only static-prop culling from same-frame Vulkan depth; exact ceil-ladder MIN footprints and per-object LOD. MC2_HZB_BUILD=0 keeps every instance."
    },
    // [18] StaticPropBc7
    {
        "MC2_FEATURE_STATICPROP_BC7", nullptr, EnvVarKind::Retired, false,
        "Retired: static-prop arrays use RGBA uploads; tiered BC7 sidecars are no longer consumed."
    },
    // [19] ColormapCpuRetire
    {
        "MC2_FEATURE_COLORMAP_CPU_RETIRE", "MC2_COLORMAP_CPU_RETIRE", EnvVarKind::Feature, true,
        "COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha in BuildColormapAtlas immediately after GPU atlas upload."
    },
    // [20] ColormapKtx2
    {
        "MC2_FEATURE_COLORMAP_KTX2", "MC2_COLORMAP_KTX2", EnvVarKind::Feature, true,
        "COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas (gos_terrain_indirect.cpp)."
    },
    // [21] SetupTexturesLegacyForce
    {
        "MC2_FEATURE_SETUPTEXTURES_LEGACY_FORCE", nullptr, EnvVarKind::Retired, false,
        "Retired: legacy terrain texture force path removed."
    },
    // [22] LegacyLitMc2Frame
    {
        "MC2_LEGACY_LIT_MC2_FRAME", "MC2_LEGACY_LIT_MC2_FRAME", EnvVarKind::Feature, true,
        "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LEGACY_LIT_MC2_FRAME=0 reverts."
    },
    // [23] LightPosColumnFix
    {
        "MC2_LIGHT_POS_COLUMN_FIX", "MC2_LIGHT_POS_COLUMN_FIX", EnvVarKind::Feature, true,
        "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LIGHT_POS_COLUMN_FIX=0 reverts."
    },
    // [24] VehicleLitPath
    {
        "MC2_VEHICLE_LIT_PATH", "MC2_VEHICLE_LIT_PATH", EnvVarKind::Feature, true,
        "VEHICLE-LIGHTING-PATH-1: put GROUND VEHICLES on the same lighting path as every other object class."
    },
    // [25] OverlayCementAdopt
    {
        "MC2_FEATURE_OVERLAY_CEMENT_ADOPT", "MC2_OVERLAY_CEMENT_ADOPT", EnvVarKind::Feature, true,
        "OVERLAY-CEMENT-ADOPT-1: closes the two-shader concrete seam CEMENT-PATCHY-CAUSE-1 attributed but explicitly left open as a design call ('either the overlay..."
    },
    // [26] VehicleAmbientFloor
    {
        "MC2_VEHICLE_AMBIENT_FLOOR", "MC2_VEHICLE_AMBIENT_FLOOR", EnvVarKind::Feature, true,
        "VEHICLE-AMBIENT-FLOOR-1: DEFAULT-ON."
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
        "MC2_ABL_COMPILE_FATAL", "MC2_ABL_COMPILE_FATAL", EnvVarKind::Feature, true,
        "ASAN-3-ABL-COMPILE-FATAL-1: fail at preprocess when ABL syntax errors leave partial bytecode; set =0 only for legacy execute-anyway diagnostics.",
    },
    {
        "MC2_ABL_COMPILE_FATAL_REPRO", "MC2_ABL_COMPILE_FATAL_REPRO", EnvVarKind::Trace, false,
        "ASAN-3-ABL-COMPILE-FATAL-1: preprocess the supplied malformed-fixture path during ABL init to prove fail-loud behavior.",
    },
    {
        "MC2_TRACE_AIMED_FIRE_FIXTURE", "MC2_AIMED_FIRE_FIXTURE", EnvVarKind::Trace, false,
        "ISSUE-9 headless probe (code/warrior.cpp MechWarrior::updateActions): every home-team mech without a combat order is issued an aimed-HEAD attack...",
    },
    {
        "MC2_TRACE_APPEAR_AUDIT", "MC2_APPEAR_AUDIT", EnvVarKind::Trace, false,
        "META-FIX design B PART 4 (code/mission.cpp Mission::start beside mission_phase_mark('mission_ready')): default OFF, OBSERVATIONAL-FIRST.",
    },
    {
        "MC2_FEATURE_ASSIMP_MECH_IMPORT", "MC2_ASSIMP_MECH_IMPORT", EnvVarKind::Feature, true,
        "BT2018 mech import 1A: route mech geometry through the Assimp GLTF/FBX importer instead of the legacy MSL loader.",
    },
    {
        "MC2_TRACE_BOOT_INFO", "MC2_BOOT_INFO", EnvVarKind::Trace, false,
        "INFO-CAPTURE fixture: =N force-selects the first home mover into the in-mission Info window and switches the command bar to the INFO tab ONCE at frame >= N...",
    },
    {
        "MC2_TRACE_BOOT_INFO_SCROLL", "MC2_BOOT_INFO_SCROLL", EnvVarKind::Trace, false,
        "INFO-IMGUI-SLICE-4 capture assist: =1 (with MC2_BOOT_INFO=N driving) pins the info-panel loadout list scrolled to bottom from frame N+3000 on BOTH paths -...",
    },
    {
        "MC2_TRACE_BOOT_OBJ", "MC2_BOOT_OBJ", EnvVarKind::Trace, false,
        "OBJ-CAPTURE fixture: =N calls ControlGui::startObjectives(1) ONCE at frame >= N (ControlGui::update), showing the in-mission objectives overlay.",
    },
    {
        "MC2_TRACE_BOOT_OBJ_COMPLETE", "MC2_BOOT_OBJ_COMPLETE", EnvVarKind::Trace, false,
        "OBJ-COMPLETE-CAPTURE fixture: =N force-resolves the first active non-hidden objective to OS_SUCCESSFUL ONCE at frame >= N (ControlGui::update) via the...",
    },
    {
        "MC2_TRACE_BOOT_PAUSED", "MC2_BOOT_PAUSED", EnvVarKind::Trace, false,
        "PAUSE-IMGUI-SLICE-0: =N auto-toggles the in-mission pause menu ONCE at frame >= N (MissionInterfaceManager::update).",
    },
    {
        "MC2_TRACE_BOOT_PAUSED_CONFIRM", "MC2_BOOT_PAUSED_CONFIRM", EnvVarKind::Trace, false,
        "PAUSE-IMGUI-SLICE-3: =abort|exit invokes the ABORT MISSION / EXIT GAME confirm dialog ONCE after the MC2_BOOT_PAUSED pause menu has settled...",
    },
    {
        "MC2_TRACE_BOOT_TIMER", "MC2_BOOT_TIMER", EnvVarKind::Trace, false,
        "TIMER-CAPTURE fixture: =N force-arms the in-mission mission timer ONCE at frame >= N by setting mission->m_timeLimit = scenarioTime + 754 s...",
    },
    {
        "MC2_TRACE_BOOT_TIMER_SECS", "MC2_BOOT_TIMER_SECS", EnvVarKind::Trace, false,
        "TIMER-CAPTURE fixture parameter: =S overrides the remaining seconds the MC2_BOOT_TIMER fixture arms (default 754).",
    },
    {
        "MC2_TRACE_BOOT_VIDEO", "MC2_BOOT_VIDEO", EnvVarKind::Trace, false,
        "VIDEO-CAPTURE fixture: =1 overrides the smoke-mode movie skip in MC2Movie::init so in-mission comm videos (mc2_01 V1A.bik ~frame 1600) play under...",
    },
    {
        "MC2_BRAIN_FREEZE_TRACE", "MC2_BRAIN_FREEZE_TRACE", EnvVarKind::Trace, false,
        "SAVE/LOAD-AIFREEZE-TRACE-1 (+ ISSUE-10 extension): one-line stderr trace at end of MechWarrior::runBrain (brainState, curTacOrder.code, movePath.numSteps...",
    },
    {
        "MC2_FEATURE_CALLED_SHOT_FIX", "MC2_CALLED_SHOT_FIX", EnvVarKind::Feature, false,
        "ISSUE-9 called-shot effectiveness (gameplay gate, default OFF = stock): (S1) Mover::calcAttackChance floors the aimed-shot modifiers at Head 0.30 / Arm 0.45...",
    },
    {
        "MC2_DIAG_CAMERA_PARK", "MC2_CAMERA_PARK", EnvVarKind::Trace, false,
        "CAMERA-PARK-1 (ported from vk branch 4b69ef78): ='x,y,rot,proj,alt[@frame]' hard-sets the live camera pose once at frame >= @frame (default 300) and pins it...",
    },
    {
        "MC2_TRACE_CEMENT_ATLAS_DUMP", "MC2_CEMENT_ATLAS_DUMP", EnvVarKind::Trace, false,
        "CEMENT-ATLAS-CELL-2: dump the assembled cement/road overlay atlas so cell art can be inspected directly -- which cells actually carry painted markings is an...",
    },
    {
        "MC2_TRACE_COLORMAP_DISPLACE_PROBE", "MC2_COLORMAP_DISPLACE_PROBE", EnvVarKind::Trace, false,
        "COLORMAP-CPU-RETIRE-1 parity probe: logs displacement magnitude (maxDispMag in wu) from the CPU colormap displacement path.",
    },
    {
        "MC2_COMMAND_REPLAY_PLAY", "MC2_COMMAND_REPLAY_PLAY", EnvVarKind::Trace, false,
        "Play a command replay through the existing Replay queue source.",
    },
    {
        "MC2_COMMAND_REPLAY_RECORD", "MC2_COMMAND_REPLAY_RECORD", EnvVarKind::Trace, false,
        "Record accepted tactical commands and per-tick subsystem hashes.",
    },
    {
        "MC2_FEATURE_CONE_POOL_POLICY", "MC2_CONE_POOL_POLICY", EnvVarKind::Feature, true,
        "CONE-SPILL-POLICY-1: pool-placement policy for building/beacon spotlight ground pools on the DEFAULT path (MC2_CONE_SPILL_COUPLING OFF).",
    },
    {
        "MC2_TUNE_CONE_POOL_REACH", "MC2_CONE_POOL_REACH", EnvVarKind::Trace, false,
        "CONE-SPILL-POLICY-1: reach multiplier k for the extended beam/terrain intersection search (reach = k * authored coneLen * MC2_LIGHT_CONE_SPILL).",
    },
    {
        "MC2_CONE_SPILL_COUPLING", "MC2_CONE_SPILL_COUPLING", EnvVarKind::Trace, false,
        "CONE-SPILL-GRADE-1: isolates CONE-SPILL-COUPLING-1 (c1f98b45), which shipped UNGATED as a default-behaviour change riding MC2_LIGHT_CONE_NODES and is now...",
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP", "MC2_DEBUG_STATE_DUMP", EnvVarKind::Trace, false,
        "DEBUG-STATE-DUMP-1: write a JSON render-state snapshot to debug_state/latest_render_state.json every 300 frames (and at frame 1).",
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_DIR", "MC2_DEBUG_STATE_DUMP_DIR", EnvVarKind::Trace, false,
        "DEBUG-STATE-DUMP-1: override output directory for MC2_DEBUG_STATE_DUMP.",
    },
    {
        "MC2_DIAG_DEBUG_STATE_DUMP_HISTORY", "MC2_DEBUG_STATE_DUMP_HISTORY", EnvVarKind::Trace, false,
        "DEBUG-STATE-DUMP-2: enable rolling 8-slot history ring alongside latest_render_state.json.",
    },
    {
        "MC2_DIAG_DEBUG_VIEW", "MC2_DEBUG_VIEW", EnvVarKind::Trace, false,
        "DEBUG-VIEW-SWEEP-1: =N (1..6) boot-seeds the global full-scene debug render view: 1=shadow-factor 2=normalized-depth 3=unlit-albedo 4=overdraw-proxy...",
    },
    {
        "MC2_DIAG_DEBUG_VIEW_SWEEP", "MC2_DEBUG_VIEW_SWEEP", EnvVarKind::Trace, false,
        "DEBUG-VIEW-SWEEP-1: ='<startFrame>,<settleFrames>' frame-scheduled auto-sweep of the six MC2_DEBUG_VIEW views (missiongui.cpp update).",
    },
    {
        "MC2_DETERMINISTIC_RNG", "MC2_DETERMINISTIC_RNG", EnvVarKind::Feature, true,
        "Route all randomness (incl.",
    },
    {
        "MC2_TRACE_DIAG_VERBOSE", "MC2_DIAG_VERBOSE", EnvVarKind::Trace, false,
        "QUIET-DEFAULT-BUILD-1: ONE umbrella gate for the engine's undifferentiated developer chatter.",
    },
    {
        "MC2_DIRECT_FIRE_STRAIGHT", "MC2_DIRECT_FIRE_STRAIGHT", EnvVarKind::Feature, false,
        "Direct-fire de-curve: PPC/autocannon/gauss bolts fly STRAIGHT instead of re-tracking the moving target every frame (the janky homing curve).",
    },
    {
        "MC2_FEATURE_STATICPROP_MATERIAL_PBR_SLOTS", nullptr, EnvVarKind::Feature, true,
        "Reserved: no effect. Static-prop ORM is fed by the separate authored-material feed; the batcher always uses neutral factors.",
    },
    {
        "MC2_TRACE_FIRE_DECISION", "MC2_FIRE_DECISION_TRACE", EnvVarKind::Trace, false,
        "ISSUE-17 fire-refusal instrumentation (default OFF = byte-identical; needs MC2_DIAG_TAGS=...,COMBAT).",
    },
    {
        "MC2_TRACE_FIXTURE_ORDER_LIVENESS", "MC2_FIXTURE_ORDER_LIVENESS", EnvVarKind::Trace, false,
        "META-FIX design F behavioral fixture (code/mission.cpp soak namespace; requires MC2_SOAK_AUTOWIN): at 20% of the autowin timer every armed home mech gets a...",
    },
    {
        "MC2_TRACE_FORCE_ALL_VISIBLE", "MC2_FORCE_ALL_VISIBLE", EnvVarKind::Trace, false,
        "FORCE-ALL-VISIBLE-1: mark every unit (mechs, ground vehicles, elementals, all teams) fully visible and sensed, bypassing sensor / contact gating so nothing...",
    },
    {
        "MC2_FEATURE_FRAMEGRAPH_EXECUTOR", "MC2_FRAMEGRAPH_EXECUTOR", EnvVarKind::Feature, true,
        "RENDER-FRAME-GRAPH arc: top-level frame-graph executor ownership of pass begin/end (mclib/render_contract.cpp, GameOS/gameos/gos_postprocess.cpp).",
    },
    {
        "MC2_TRACE_FREEZE_MOVERS_AT", "MC2_FREEZE_MOVERS_AT", EnvVarKind::Trace, false,
        "FREEZE-MOVERS-1: =<frame> freezes every mover the first frame g_mc2FrameCounter >= <frame>, WITHOUT needing the command channel (the 'freeze_movers on...",
    },
    {
        "MC2_FX_DEFS", nullptr, EnvVarKind::Feature, true,
        "Authored effect sidecars overlay the stock catalog; absent definitions are a no-op.",
    },
    {
        "MC2_FX_FORCE_SPAWN", "MC2_FX_FORCE_SPAWN", EnvVarKind::Trace, false,
        "FX-FORCE-SPAWN v1 fixture: default-OFF. =impact: impact-decal volley.",
    },
    {
        "MC2_GOM_UPDATE_COST_SPLIT", "MC2_GOM_UPDATE_COST_SPLIT", EnvVarKind::Trace, false,
        "GOM_UPDATE_COST v1 (framebudget): =1 emits one coarse per-phase GameObjectManager update CPU-cost summary to stderr (objmgr.cpp).",
    },
    {
        "MC2_TRACE_GPU_CULL_STATIC_LIGHT_ZERO_PROBE", "MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE", EnvVarKind::Trace, false,
        "Static-prop light-bake observation gate, default OFF.",
    },
    {
        "MC2_GPU_PARTICLES", "MC2_GPU_PARTICLES", EnvVarKind::Feature, true,
        "Native Card/CardCloud rendering; =0 suppresses these draws (CPU routes retired).",
    },
    {
        "MC2_FEATURE_HUD_IMGUI_TABS", "MC2_HUD_IMGUI_TABS", EnvVarKind::Feature, false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C: data-driven command-bar tab registry (code/hudtabregistry.{h,cpp} + wiring in code/controlgui.cpp).",
    },
    {
        "MC2_FEATURE_HUD_READABILITY_V1", "MC2_HUD_READABILITY_V1", EnvVarKind::Feature, false,
        "IN-MISSION-HUD-IMGUI-SCOPE-1 slice A: data-first HUD readability.",
    },
    {
        "MC2_TRACE_IFACE_COST_SPLIT", "MC2_IFACE_COST_SPLIT", EnvVarKind::Trace, false,
        "MISSION-INTERFACE-PERF-1: chrono cost-split of MissionInterfaceManager::update() (the GameLogic.Mission.Interface Tracy zone) incl.",
    },
    {
        "MC2_FEATURE_LIGHTBAKE_REBAKE_ON_READY", "MC2_LIGHTBAKE_REBAKE_ON_READY", EnvVarKind::Feature, true,
        "LIGHTBAKE-REBAKE-ON-READY-1: soft-invalidate the per-recipe static-light bake cache once, 8 frames after mission-ready, so recipes baked during boot/load...",
    },
    {
        "MC2_LIGHT_CONE_NODES", "MC2_LIGHT_CONE_NODES", EnvVarKind::Trace, true,
        "LIGHT-CONE-MODELS-1: DEFAULT-ON.",
    },
    {
        "MC2_FEATURE_LIGHT_GRID", "MC2_LIGHT_GRID", EnvVarKind::Feature, true,
        "LIGHT-CLUSTER-PERF-1: world-space XY light GRID over the per-frame terrain light pool.",
    },
    {
        "MC2_TUNE_LIGHT_GRID_DIM", "MC2_LIGHT_GRID_DIM", EnvVarKind::Trace, false,
        "LIGHT-CLUSTER-PERF-1 knob: cells per axis for the light grid (grid is DIM x DIM over the XY bbox of this frame's packed lights).",
    },
    {
        "MC2_FEATURE_LIGHT_POOL_COMPRESS", "MC2_LIGHT_POOL_COMPRESS", EnvVarKind::Feature, true,
        "NIGHT-LIGHT-POOLS-3: tone-compress the accumulated terrain light-pool sum (chunk + overlay frag): 1-exp(-1.6*sum) instead of min(sum,1).",
    },
    {
        "MC2_TUNE_LIGHT_POOL_MAX", "MC2_LIGHT_POOL_MAX", EnvVarKind::Trace, false,
        "LIGHT-POOL-CAP-1: per-mission slot count of the terrain light pool (gos_terrain_lighting.cpp mission_init).",
    },
    {
        "MC2_FEATURE_LIGHT_REALISM", "MC2_LIGHT_REALISM", EnvVarKind::Feature, true,
        "LIGHT-REALISM-1: night light-pool + cone-glow realism reshape.",
    },
    {
        "MC2_TRACE_LINKUP", "MC2_LINKUP_TRACE", EnvVarKind::Trace, false,
        "ISSUE-50 diagnostics (code/mech.cpp BattleMech::update): for every off-GUI mech, periodically (~1/s) logs [LINKUP] part/name/status/los, plus [LINKUP]...",
    },
    {
        "MC2_TRACE_LOAD_RACE_DELAY_ATLAS", "MC2_LOAD_RACE_DELAY_ATLAS", EnvVarKind::Trace, false,
        "gos_terrain_indirect.cpp negative control for the mission-load residency-race probes: when set (any value), BuildColormapAtlas() defers the real GL atlas...",
    },
    {
        "MC2_TRACE_LOAD_RACE_PROBE_FRAMES", "MC2_LOAD_RACE_PROBE_FRAMES", EnvVarKind::Trace, false,
        "gameosmain.cpp: overrides the comma-separated list of frame numbers at which the mission-load residency-race one-shot diagnostics probe fires (see...",
    },
    {
        "MC2_LOG", "MC2_LOG", EnvVarKind::Trace, false,
        "Master stdout log gate (gameosmain.cpp, right after setvbuf).",
    },
    {
        "MC2_TRACE_LOG_FILE_RESOLVE", "MC2_LOG_FILE_RESOLVE", EnvVarKind::Trace, false,
        "META-FIX design E tier 3 (mclib/file.cpp, parsed at InitModSearchPaths): file-resolution ladder diagnostics.",
    },
    {
        "MC2_TRACE_MOVER_COHERENCE_AUDIT", "MC2_MOVER_COHERENCE_AUDIT", EnvVarKind::Trace, false,
        "META-FIX design A runtime tier (code/mission.cpp auditMoverCoherence, ~1 Hz): read-only audit that the five per-mover registries (isOnGUI render gate...",
    },
    {
        "MC2_FEATURE_MOVE_RECON", "MC2_MOVE_RECON", EnvVarKind::Trace, false,
        "Pathfinding cost instrumentation (mclib/move_recon.cpp).",
    },
    {
        "MC2_TUNE_NIGHT_AMBIENT_FLOOR", "MC2_NIGHT_AMBIENT_FLOOR", EnvVarKind::Trace, false,
        "NIGHT-LIGHT-POOLS-1 knob: minimum per-channel output (0..64, clamped) of Camera::getLightRed/Green/Blue -- the family feeding the colormap night burn-in...",
    },
    {
        "MC2_TRACE_NO_ACTIVATE", "MC2_NO_ACTIVATE", EnvVarKind::Trace, false,
        "Mission Editor (editor/EditorMFC.cpp, MainFrm.cpp): come up minimized and non-activating, never taking the foreground or keyboard focus.",
    },
    {
        "MC2_TRACE_NO_CURSOR_GRAB", "MC2_NO_CURSOR_GRAB", EnvVarKind::Trace, false,
        "SMOKE-NO-CURSOR-STEAL-1: suppress ALL OS-cursor manipulation -- window mouse-grab, Win32 ClipCursor clamp, and gos_SetMousePosition warps -- plus window...",
    },
    {
        "MC2_OVERLAY_TILE_HIRES", "MC2_OVERLAY_TILE_HIRES", EnvVarKind::Feature, true,
        "OVERLAY-TILE-HIRES-1: honor the overlay tile TGA's actual dimensions instead of the legacy hard-assumed 64px (initOverlay header resync mirrors...",
    },
    {
        "MC2_PRESENTATION_DISABLED", "MC2_PRESENTATION_DISABLED", EnvVarKind::Trace, false,
        "Replay proof: suppress presentation extraction updates while simulation continues.",
    },
    {
        "MC2_PRESENT_THREAD", "MC2_PRESENT_THREAD", EnvVarKind::Feature, true,
        "P2 present worker; set 0 for the inline rollback. Retire within three merge windows.",
    },
    {
        "MC2_PROJECTILE_SPEED_MULT", "MC2_PROJECTILE_SPEED_MULT", EnvVarKind::Feature, false,
        "Direct-fire projectile speed multiplier (PPC/autocannon/gauss = non-arc bolts).",
    },
    {
        "MC2_DIAG_PROP_DEBUG_VIEW", "MC2_PROP_DEBUG_VIEW", EnvVarKind::Trace, false,
        "Prop material view: 0 off, 1 albedo, 2 world normal, 3 raw normal sample (BC5 RG), 4 roughness, 5 metallic, 6 AO, 7 material id hash.",
    },
    {
        "MC2_FEATURE_RENDER_BACKEND", "MC2_RENDER_BACKEND", EnvVarKind::Feature, false,
        "Legacy selector, read only to report an ignored value: the engine renders on Vulkan only (the gl opt-out was removed).",
    },
    {
        "MC2_FEATURE_RENDER_PASS_ORDER", "MC2_RENDER_PASS_ORDER", EnvVarKind::Trace, false,
        "CONTRACT-3: per-frame resource-ordering audit (mclib/render_contract.cpp) - checks each pass's reads[] are satisfied by a prior write before beginPass.",
    },
    {
        "MC2_REPLAY_CONTENT", "MC2_REPLAY_CONTENT", EnvVarKind::Trace, false,
        "Mission/mod content digest supplied and checked by tools/command_replay.py.",
    },
    {
        "MC2_TRACE_RESOLVE_TRACE_FILE", "MC2_RESOLVE_TRACE_FILE", EnvVarKind::Trace, false,
        "META-FIX design E tier 3 (mclib/file.cpp): output path for the MC2_LOG_FILE_RESOLVE=2 JSONL resolve trace, opened for append at InitModSearchPaths (relative...",
    },
    {
        "MC2_TRACE_RES_ASSERT", "MC2_RES_ASSERT", EnvVarKind::Trace, false,
        "META-FIX design D runtime tier (RenderCore/ResolutionContract.h): audit mode, unset/0 = off, fatal = abort, anything else = log.",
    },
    {
        "MC2_DIAG_SHADOW_DEBUG_MODE", "MC2_SHADOW_DEBUG_MODE", EnvVarKind::Trace, false,
        "SHADOW-ENV-DEBUG-MODE-1: selects shadow debug map overlay without ImGui.",
    },
    {
        "MC2_TUNE_SHADOW_MIN_SUN_ALT", "MC2_SHADOW_MIN_SUN_ALT", EnvVarKind::Trace, false,
        "SHADOW-SUN-ALT-CLAMP-1 tunable: minimum EFFECTIVE sun altitude in DEGREES used by the shadow projection matrices (mc2ClampShadowSunAltitude...",
    },
    {
        "MC2_SIM_IDENTITY", "MC2_SIM_IDENTITY", EnvVarKind::Trace, true,
        "SIM-IDENTITY-HASH-1: emit a world-state hash (sorted-by-partId digest over mover position/rotation/gesture/anim-frame/damage/status/team plus...",
    },
    {
        "MC2_SIM_IDENTITY_TRACE", "MC2_SIM_IDENTITY_TRACE", EnvVarKind::Trace, false,
        "SIM-IDENTITY-HASH-1: emit one `[SIMID v1] simframe=N t=T n=C graded=.. visual=.. strict=..` line per SIM frame to stdout.",
    },
    {
        "MC2_SIM_IDENTITY_TRACE_DETAIL", "MC2_SIM_IDENTITY_TRACE_DETAIL", EnvVarKind::Trace, false,
        "SIM-IDENTITY-HASH-1: with MC2_SIM_IDENTITY_TRACE on, additionally emit one `[SIMID v1 mover]` line per mover per sim frame.",
    },
    {
        "MC2_FEATURE_SMART_LOAD", "MC2_SMART_LOAD", EnvVarKind::Feature, true,
        "SMART-LOAD startup/mission-load perf path (logisticsdata.cpp).",
    },
    {
        "MC2_DIAG_SNAPSHOT_MECH_EXTRACT", "MC2_SNAPSHOT_MECH_EXTRACT", EnvVarKind::Trace, false,
        "MECH-EXTRACTION-0: mech snapshot extraction (observe-only, no GL mutation).",
    },
    {
        "MC2_TRACE_SOAK_SALVAGE_TRADEMOVER", "MC2_SOAK_SALVAGE_TRADEMOVER", EnvVarKind::Trace, false,
        "ISSUE-42a repro fixture (code/mission.cpp soak harness): with MC2_SOAK_AUTOWIN + MC2_SOAK_KILL_ENEMY, exercises the REAL mid-mission salvage-craft flow...",
    },
    {
        "MC2_SPOTLIGHT_DIRECTION_FIX", "MC2_SPOTLIGHT_DIRECTION_FIX", EnvVarKind::Trace, false,
        "SPOTLIGHT-DIRECTION-1: fixes two coordinate-space defects at the building/guard-tower spotlight registration site (mclib/bdactor.cpp).",
    },
    {
        "MC2_FEATURE_STATIC_LIGHT_UPLOAD_SPLIT", "MC2_STATIC_LIGHT_UPLOAD_SPLIT", EnvVarKind::Feature, true,
        "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: upload the immutable static light prefix [0..S) once/dirty-only + the dynamic suffix [S..count) per frame, instead...",
    },
    {
        "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME", "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME", EnvVarKind::Trace, false,
        "STATIC-BLDG-SHADOW-REBAKE-1: deterministic repro hook (test-only, no production callers).",
    },
    {
        "MC2_FEATURE_TACTICAL_ARC_OVERLAY", "MC2_TACTICAL_ARC_OVERLAY", EnvVarKind::Trace, false,
        "TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line on each SELECTED mover, drawn in-scene by the debug renderer just before its flush...",
    },
    {
        "MC2_TERRAIN_CONTROLMAP", "MC2_TERRAIN_CONTROLMAP", EnvVarKind::Feature, false,
        "TERRAIN-CONTROLMAP-SAMPLE-1: load an authored per-mission RGBA control-map sidecar (data/missions/<stem>.beauty/control_map.png, vertex-resolution) that...",
    },
    {
        "MC2_TERRAIN_CONTROLMAP_FILE", "MC2_TERRAIN_CONTROLMAP_FILE", EnvVarKind::Feature, false,
        "TERRAIN-CONTROLMAP-SAMPLE-1: override the control-map sidecar path (default data/missions/<stem>.beauty/control_map.png).",
    },
    {
        "MC2_DIAG_TERRAIN_DEBUG_MODE", "MC2_TERRAIN_DEBUG_MODE", EnvVarKind::Trace, false,
        "TERRAIN-DEBUG-VIEWS-1: terrain fragment-shader debug-mode selector for the tessellated terrain path (gos_terrain.frag tessDebug.x).",
    },
    {
        "MC2_FEATURE_TERRAIN_LOD_CHUNK", "MC2_TERRAIN_LOD_CHUNK", EnvVarKind::Feature, true,
        "LOD-chunk terrain renderer (mclib/terrain.cpp/terrain.h), the primary tessellation-free terrain path.",
    },
    {
        "MC2_FEATURE_TERRAIN_MATERIAL_TEXTURES", "MC2_TERRAIN_MATERIAL_TEXTURES", EnvVarKind::Feature, false,
        "TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB GL_TEXTURE_2D_ARRAY albedo (rock/grass/dirt/concrete/snow/cliff from...",
    },
    {
        "MC2_TUNE_TERRAIN_MATERIAL_TEXTURES_STRENGTH", "MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH", EnvVarKind::Trace, false,
        "TERRAIN-MATERIAL-TEXTURES-1 knob: albedo mix strength override (float, clamped 0..1).",
    },
    {
        "MC2_TERRAIN_OVERLAY_V2", "MC2_TERRAIN_OVERLAY_V2", EnvVarKind::Feature, false,
        "TERRAIN-OVERLAY-V2-PARITY-1: composite an authored world-XY-bounded cement/pad/runway overlay sidecar (data/missions/<stem>.beauty/overlay_v2.png...",
    },
    {
        "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS", "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS", EnvVarKind::Feature, true,
        "TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: in the static decal bake, skip legacy runway/road/cement decals for tiles fully inside the armed OVERLAY_V2 sidecar...",
    },
    {
        "MC2_TERRAIN_OVERLAY_V2_FILE", "MC2_TERRAIN_OVERLAY_V2_FILE", EnvVarKind::Feature, false,
        "TERRAIN-OVERLAY-V2-PARITY-1: override the overlay_v2 sidecar path (default data/missions/<stem>.beauty/overlay_v2.png).",
    },
    {
        "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE", "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE", EnvVarKind::Feature, true,
        "TERRAIN-VISUAL-HEIGHT-SAMPLE-1: when visual terrain displacement is on, fade object grounding across the displaced-vs-gameplay height delta so props/units...",
    },
    {
        "MC2_FEATURE_TERRAIN_VISUAL_HEIGHT", "MC2_TERRAIN_VISUAL_HEIGHT", EnvVarKind::Feature, false,
        "TERRAIN-VISUAL-HEIGHT: loads the 4x-res VISUAL heightfield bake into an SSBO for corner-pinned interior displacement (mclib/terrain.cpp).",
    },
    {
        "MC2_TRACE_TEST_SPAWN", "MC2_TEST_SPAWN", EnvVarKind::Trace, false,
        "GAMEPLAY-DRIVER-1: =1 arms the test-only spawn/teleport verbs on the MC2_UI_COMMAND_FILE channel.",
    },
    {
        "MC2_TREE_DIAG_TRACE", "MC2_TREE_DIAG_TRACE", EnvVarKind::Trace, false,
        "[DEFAULT-OFF, but the OFF token is UNSET, not '0'.]",
    },
    {
        "MC2_TRACE_UI_COMMAND_FILE", "MC2_UI_COMMAND_FILE", EnvVarKind::Trace, false,
        "UI-COMMAND-CHANNEL-1 + GAMEPLAY-DRIVER-1/2: =<path> polls that file once per frame (MissionInterfaceManager::update) and executes its lines as commands.",
    },
    {
        "MC2_TRACE_UI_IMGUI_INFO", "MC2_UI_IMGUI_INFO", EnvVarKind::Trace, false,
        "INFO-IMGUI-SLICE-1: =1 renders the in-mission unit-Info panel as an ImGui window (unit name + health bar + loadout list with scrollbar) laid out inside...",
    },
    {
        "MC2_TRACE_UI_IMGUI_OBJ", "MC2_UI_IMGUI_OBJ", EnvVarKind::Trace, false,
        "OBJ-IMGUI-SLICE-1: =1 renders the in-mission OBJECTIVES overlay as ImGui (baked left-edge frame from tools/ui_art_reconstruct.py + procedural...",
    },
    {
        "MC2_TRACE_UI_IMGUI_PAUSE", "MC2_UI_IMGUI_PAUSE", EnvVarKind::Trace, false,
        "PAUSE-IMGUI-SLICE-1: =1 renders the in-mission pause menu as an ImGui window (GAME PAUSED + objectives/save/load/options/abort/exit/return) laid out inside...",
    },
    {
        "MC2_TRACE_UI_IMGUI_TIMER", "MC2_UI_IMGUI_TIMER", EnvVarKind::Trace, false,
        "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission mission-timer display as ImGui (right-aligned MM:SS via HudFont + the TimerStatic atlas tile) laid out...",
    },
    {
        "MC2_TRACE_UI_IMGUI_VIDEO", "MC2_UI_IMGUI_VIDEO", EnvVarKind::Trace, false,
        "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission comm-video panel as ImGui - VideoStatic chrome tiles + the decoder's frame texture at the same...",
    },
    {
        "MC2_FEATURE_UNIT_PROFILE_DATA", "MC2_UNIT_PROFILE_DATA", EnvVarKind::Feature, false,
        "UNIT-PROFILE-ARC: data-driven unit+equipment definition source (code/unitprofile.cpp).",
    },
    {
        "MC2_DIAG_VFX_DEBUG_MODE", "MC2_VFX_DEBUG_MODE", EnvVarKind::Trace, false,
        "VFX-DEBUG-VIEWS-1: GPU particle billboard fragment-shader debug-mode selector (particle_billboard.frag u_debugMode, uploaded by gos_particle_bridge).",
    },
    {
        "MC2_VFX_ORACLE_TUBE", "MC2_VFX_ORACLE_TUBE", EnvVarKind::Feature, true,
        "GOSFX-TUBE-RIBBON-1: route supported gosFX Tube trails (MissileSmoke alpha, PPC additive, polygon) through the swept-quad ribbon oracle submit path instead...",
    },
    {
        "MC2_TUNE_VIEW_MODE", "MC2_VIEW_MODE", EnvVarKind::Trace, false,
        "VIEWMODE-POSTPROCESS-PRESENTATION-1: seeds the startup presentation mode (matches RenderCore::ViewMode).",
    },
    {
        "MC2_TRACE_VISUAL_CAPTURE_ON_FIRE", "MC2_VISUAL_CAPTURE_ON_FIRE", EnvVarKind::Trace, false,
        "VK-FIELD-VFX-8: GL twin of MC2_VK_PRESENT_DUMP_ON_FIRE.",
    },
    {
        "MC2_FEATURE_VK_FORCE_FAIL", "MC2_VK_FORCE_FAIL", EnvVarKind::Trace, false,
        "VK-P13-SELECTOR-1: presence-gated =1, default OFF.",
    },
    {
        "MC2_VK_PICK_PROBE", "MC2_VK_PICK_PROBE", EnvVarKind::Trace, false,
        "BUG-A pick regression fence: default-OFF.",
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_FRAME", "MC2_VK_PRESENT_DUMP_FRAME", EnvVarKind::Trace, false,
        "VK-FIELD-VFX-3: frame pin for MC2_VK_PRESENT_DUMP_PPM.",
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_ON_FIRE", "MC2_VK_PRESENT_DUMP_ON_FIRE", EnvVarKind::Trace, false,
        "VK-FIELD-VFX-5: dynamic fire-event pin for MC2_VK_PRESENT_DUMP_PPM.",
    },
    {
        "MC2_TRACE_VK_PRESENT_DUMP_PPM", "MC2_VK_PRESENT_DUMP_PPM", EnvVarKind::Trace, false,
        "VK-FIELD-ORACLE-1: headless present-pixel oracle.",
    },
    {
        "MC2_TRACE_VK_PRESENT_NATIVE_STRICT", "MC2_VK_PRESENT_NATIVE_STRICT", EnvVarKind::Trace, false,
        "VK present-tail deletion-readiness canary.",
    },
    {
        "MC2_TRACE_VK_SHADOW_MATRIX_DIAG", "MC2_VK_SHADOW_MATRIX_DIAG", EnvVarKind::Trace, false,
        "VK shadow producer/consumer evidence (default OFF = zero cost; GameOS/gameos/vulkan_shadow_island_live.cpp).",
    },
    {
        "MC2_FEATURE_VK_SHARED_GBUFFER", "MC2_VK_SHARED_GBUFFER", EnvVarKind::Feature, true,
        "Retired: the Vulkan scene path is unconditional; =0 is ignored with a [RENDER_BACKEND] line (render_backend_select.cpp).",
    },
    {
        "MC2_TRACE_VK_VFX_TRACE", "MC2_VK_VFX_TRACE", EnvVarKind::Trace, false,
        "VK-FIELD-VFX-2: per-bolt weapon-fire pipeline trace.",
    },
    {
        "MC2_FEATURE_WATER_REFLECTION_RT", "MC2_WATER_REFLECTION_RT", EnvVarKind::Feature, true,
        "Half-resolution spatial water reflection; =0 falls back to sky only. WaterReflections in options.cfg selects Off/Sky only/Half-res.",
    },
    {
        "MC2_FEATURE_WATER_SHORE_MASK", "MC2_WATER_SHORE_MASK", EnvVarKind::Feature, true,
        "WATER-SHORE-MASK-1: per-mission shoreline mask sidecar (data/missions/<m>.beauty/shoreline_mask.png) drives the water shore edge.",
    },
    {
        "MC2_TRACE_WINDOW_OFFSCREEN", "MC2_WINDOW_OFFSCREEN", EnvVarKind::Trace, false,
        "NOFOCUS-HARDEN-1: create the SDL window HIDDEN at (-32000,-32000) and keep it there, so an agent-launched engine window never occupies visible desktop area...",
    },
    {
        "MC2_WORLD_ORDER_TRACE", "MC2_WORLD_ORDER_TRACE", EnvVarKind::Trace, false,
        "W2: emit ordered world views, terrain activity, lifecycle events and update calls per mission tick. Diagnostic only.",
    },
    {
        "MC2_XFORM_PARITY_FATAL", "MC2_XFORM_PARITY_FATAL", EnvVarKind::Trace, false,
        "XFORM-CONVENTION-HARNESS-1: promote the F1-3C clip-space parity probe (code/gamecam.cpp) to a HARD invariant.",
    },
};

// ---------------------------------------------------------------------------
// Debug dump
// ---------------------------------------------------------------------------
// Emits a [RENDERER_FEATURES v1] startup banner to stderr.
// Optional legacy diagnostic; preserves the v1 field names for log consumers.
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
            const auto st = mc2::gate::boolState(d.envVar);   // the name comes from the table being dumped
            if (!st || !st->set) {
                curStr = d.defaultOn ? "on(default)" : "off(default)";
            } else if (!st->on) {
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
