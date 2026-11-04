// Shared CPU geometry, material, lighting and diagnostic state for the native static-prop feed.
//
// The pieces are contiguous slices of the original file, in file order (core, state, program, mapload, finalize, submit, flush,
// shadow, seam); list them in the build in that order so namespace-scope initialisers run in their original order.
// State more than one slice touches is declared here and defined in the slice that owns it.
#ifndef GOS_STATIC_PROP_BATCHER_INTERNAL_H
#define GOS_STATIC_PROP_BATCHER_INTERNAL_H

#include <atomic>
#include "gos_static_prop_batcher.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "render_snapshot.h"             // ExtractedStaticPropPacket full definition for batcher_getDrawSlotEntry
#include "gos_static_prop_registry.h"    // M1.5: getRecipeIndexForType
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled + objectIdRawForStaticPropRecipe
#include "../../RenderCore/MaterialGpu.h"   // MaterialGpu-2: sidecar upload
#include "../../RenderCore/IblShCoeffs.h"   // V-IBL-STATIC-1: SH-L2 constants
#include "../../RenderCore/IblShRegistry.h"  // V-IBL-STATIC-2: per-mission SH set
#include "../../RenderCore/StaticPropTypeDesc.h"  // v0: cross-seam immutable type table
#include "../../RenderCore/KtxLoader.h"    // KTX2 sidecar loading
#include "../../RenderCore/RenderDebugView.h"  // DEBUG-VIEW-REGISTRY-1
#include "draw_packet_emitter.h"
#include "../../RenderCore/PipelineRegistry.h"   // direct include; do not rely on transitive
#include "render_frame_plan.h"                   // STATIC-PROP-COLOR-LABEL-HARDENING-1: pass self-report
#include "static_prop_dispatch_meta.h"
#include "gos_postprocess.h"             // getGosPostProcess, getDynamicLightSpaceMatrix
#include "gos_terrain_lighting.h"        // POOL-CONSUMERS-1: UploadObjectLightPoolUniforms
#include "gos_static_prop_killswitch.h"  // gos_GetGLTextureId
#include "gos_mech_killswitch.h"          // g_mechPreviewRenderDepth (mech-bay preview gate)
#include "gos_profiler.h"
#include "gpu_cull_compute.h"            // C1b: compute_isEnabled, getIndirectCmdBuf, getBucketCount
#include "gpu_cull_substrate.h"          // 2026-05-10: substrate_appendStaticPropRecord
#include "gpu_cull_record.h"             // 2026-05-10: GpuActorRecord, Cat_StaticProp
#include "../../mclib/terrain.h"         // C1b temporal-superset: Terrain::worldToBlockIdx()
#include "../../mclib/render_contract.h" // [RENDER_PASS v1] noteRenderPass
#ifdef MC2_VULKAN_ISLAND
extern "C" void mc2_vk_shadow_island_invalidate_geometry();  // vulkan_shadow_island_live.cpp
extern "C" void mc2_vk_color_prop_island_invalidate_geometry();  // vulkan_color_prop_island.cpp
#endif
#include "../../RenderCore/top_level_pass_executor.h" // APPLY-STATE-STATICPROP-1: findTopLevelStateDesc
#include "../../RenderCore/frame_executor.h"          // PER-PASS-APPLY-COUNTERS-1: ApplyPassId
#include "gameos.hpp"
#include "tgl.h"  // TG_MultiShape and light values
#include "Gates/gate_cold.h"  // typed gate registry: get<>(), isSet<>(), str<>()
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>            // LIGHTROW-FRESHNESS-1: per-type stale-row log dedup
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "vk_feed_dump.h"  // VK-OFFLINE-PROOFS-1: MC2_VK_FEED_DUMP golden dump

// LIGHTROW-FRESHNESS-1: the per-frame stamp that cachedFrame_ is compared
// against. Defined in code/gameobj.cpp; declared extern exactly as every other
// consumer does (code/gameobj.cpp:104, code/controlgui.cpp:2390, ...).
// MUST stay at true file scope -- inside the anonymous namespace below this
// would acquire internal linkage and fail to resolve at link time.
extern std::atomic<uint32_t> g_mc2FrameCounter;

#define ALPHA_TRACE(fmt, ...) \
    do { if (s_alphaTrace) { printf("[ALPHA_TEST] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// PROP-FIXB-MVP: symmetric-mirror of the terrain Fix-B frame-of-reference (see
// gos_terrain_lod_chunk.cpp ~:33-41 + docs/superpowers/specs/
// 2026-05-18-AUTHORITATIVE-zfight-matrix-share-design.md §11). When the solid
// pass is armed, the LOD-chunk terrain draws with the snapshotted dispatch MVP
// (frame N-1), but objects upload the LIVE current-frame MVP -> a 1-frame
// matrix-share lag -> depth shimmer at the object/terrain contact under camera
// motion. Mirroring the SAME consume expression here makes object draws agree
// with the terrain depth they sit on. The snapshot itself is NOT touched; this
// only CONSUMES the already-published terrain snapshot. Signatures copied
// verbatim from gos_terrain_lod_chunk.cpp.
#include "gos_object_draw_mvp.h"  // RENDER-VIEW-CURRENCY-1 currency-checked accessor

#include "mc2_hitch_trace.h"

#define TREE_DIAG(fmt, ...) \
    do { if (s_treeDiagTrace) { fprintf(stderr, "[TREE_DIAG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } } while (0)

// COUNTER-HONESTY-1: [TREE_DIAG] per-instance records are capped at this many
// per path per process. That makes the stream a SAMPLE, and a sample must say so
// in its own output — a reader who counts log lines must not be able to mistake
// them for a total. Both emission sites print `sample=firstN` on every record and
// emit one SAMPLE-CAP-REACHED notice when they stop.
static constexpr int kTreeDiagSample = 8;

void gos_GetNightState(bool* isNight, float* nightFactor);
namespace gos_static_prop_batcher_detail {

// Per-vertex stride in the shared VBO. Layout:
//   vec3  a_position         (0..11)
//   vec3  a_normal           (12..23)
//   vec2  a_uv               (24..31)
//   uint  a_localVertexID    (32..35)
//   float _pad               (36..39)
// Kept in sync with shaders/static_prop.vert (Task 9).
constexpr size_t kVertexStride = 40;

constexpr uint32_t RING_FRAMES = 3;

// STATIC_PROP_RING_FRAMES in the header equals this.
// Enforce in lock-step so a future edit to either side fails loudly.
static_assert(RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "RING_FRAMES and STATIC_PROP_RING_FRAMES must match");

constexpr size_t   INITIAL_INSTANCES_PER_FRAME = 4096;

constexpr size_t   INITIAL_COLORS_PER_FRAME    = 1'000'000;  // uint32 ARGB entries

constexpr uint32_t BASE_INSTANCE_SSBO_BINDING     = 16u;
}  // namespace gos_static_prop_batcher_detail

namespace gos_static_prop_batcher_detail {

// Forward decl -- body appears after state block below, so it can reference
// s_fatalRegistrationFailure which is declared further down in this namespace.

struct PerTypeBucket {
    std::vector<GpuStaticPropInstance> instances;
};

struct TypeRangeSsbo {
    size_t instanceByteOffset;
    size_t instanceByteSize;
    size_t colorByteOffset;
    size_t colorByteSize;
    uint32_t instanceCount;
};

// ===========================================================================
// Substrate Multi-Draw Coalesce — file-scope state (plan v3.8 Step group 2).
// All state below is the coalesce path's; it is INDEPENDENT of the legacy
// per-type/per-packet path which remains the authoritative draw method.
// Coalesce is a side-attempt: failures disarm coalesce only, never legacy.
// ===========================================================================

// Step 2.3 — forward decl so coalesce_resetEnvOnce / IsCoalesceEnabled can
// reference each other across the helpers below.
bool IsCoalesceEnabled();
}  // namespace gos_static_prop_batcher_detail

static const float (*s_iblShRotatedSrc)[3] = nullptr;
namespace gos_static_prop_batcher_detail {

// Step 12B.1 — forced-disarm test hook env state. Plan v3.8: process-once,
// resolved by coalesce_resetEnvOnce() from MC2_COALESCE_FORCE_DISARM.
// Default None means hooks are inert; any other value forces a specific
// disarm path so smoke can verify each disarm's legacy-fallback works
// without contriving real failure inputs.
enum class CoalesceForceDisarm : uint8_t {
    None = 0,
    MixedAlpha,
    SizeMismatch,
    NoExtension,
    AllocFailed,
};
}  // namespace gos_static_prop_batcher_detail

#define COALESCE_TRACE(fmt, ...) \
    do { if (s_coalesceTrace) { \
        MC2_DIAGFE(stderr, "[COALESCE v1] " fmt "\n", ##__VA_ARGS__); \
        std::fflush(stderr); \
    } } while (0)
namespace gos_static_prop_batcher_detail {

// Task 3: StaticPopulation indirect commands — own buffer, CPU-written (counts
// are the frozen static population, NOT GPU cull; first-N draw, see patch 2).
// 20-byte DrawElementsIndirectCommand: count, instanceCount, firstIndex,
// baseVertex, baseInstance (matches gpu_cull_compute.cpp's DrawCmd).
struct StaticDrawCmd { uint32_t count; uint32_t instanceCount; uint32_t firstIndex;
                       int32_t baseVertex; uint32_t baseInstance; };

static_assert(sizeof(StaticDrawCmd) == 20, "StaticDrawCmd must be 20 bytes");

// --- Step 2.2: ProgramLocs uniform-location cache (§6.X). ---
// One instance per program. -1 in any field means glGetUniformLocation
// reported "uniform absent" — treat as skip-upload. Default-initialize
// every field to -1 so an uncached program still skips cleanly.

// STATICPROP-MATERIAL-ORM-1 — texture unit reserved for the per-bucket ORM
// sibling array. Unit 0 stays the albedo array (u_texArr); unit 1 is ORM.

// TERRAIN-DECAL-COLORBLEND-1 — texture unit for the terrain colormap atlas,
// sampled only by the flagged cliff decal. Clear of albedo(0)/ORM(1).

// Population indexed by GpuStaticPropPopulation enum value:
//   0=Building, 1=Tree, 2=Generic, 3=Legacy.
// Legacy is excluded from Gate F's fallback-rate computation
// (slice-1 populations only).
constexpr int kPopCount = 4;

struct ObjBatcherCounters {
    // Per-frame, indexed by population:
    uint32_t eligible_actors_by_pop[kPopCount]     = {0};
    uint32_t submitted_instances_by_pop[kPopCount] = {0};  // success only
    uint32_t cpu_fallback_by_pop[kPopCount]        = {0};
    uint32_t submitted_children                    = 0;
    uint32_t skipped_children                      = 0;
    // Per-flush:
    uint32_t gpu_drawn_instances                   = 0;
    // Monotonic since process start:
    uint64_t mono_eligible_actors_by_pop[kPopCount]     = {0};
    uint64_t mono_submitted_instances_by_pop[kPopCount] = {0};
    uint64_t mono_cpu_fallback_by_pop[kPopCount]        = {0};
    uint64_t mono_submitted_children                = 0;
    uint64_t mono_skipped_children                  = 0;
    uint64_t mono_gpu_drawn_instances               = 0;
    uint64_t frame_count                            = 0;
    // LIGHTROW-FRESHNESS-1: dynamic-path light-row staleness accounting.
    // stale_light_row_regathered = the defect this slice fixes -- a multi whose
    // cachedGpuLightIndex_ was stamped on an EARLIER frame and would previously
    // have been trusted verbatim, now re-gathered. Non-zero here in steady state
    // means the bug was live on this run.
    uint64_t mono_stale_light_row_regathered        = 0;
    // Could not re-gather (no SHAPE_NODE leaf) -- the row is unresolvable and the
    // actor is dropped rather than drawn against an arbitrary slot.
    uint64_t mono_stale_light_row_unresolvable      = 0;
    // Invariant break: a resolved row index outside [0, getLightStructCount()).
    // The shader indexes light[] unchecked, so this must be zero.
    uint64_t mono_light_row_out_of_range            = 0;
};

void accumulateMonotonicAndMaybeEmit(bool forceEmit);
}  // namespace gos_static_prop_batcher_detail

// APPLY-STATE-STATICPROP-1: third top-level scene-pass apply-state. Forward-decl of
// the cross-TU apply-state counter bump (defined in gos_postprocess.cpp). Bumps the
// SAME g_applyStatePasses that the PostProcess apply-state islands + the TerrainDecal/
// TerrainOverlay top-level applies use, so executor_apply_state_passes aggregates with
// no double-count (single increment per apply).
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);

namespace gos_static_prop_batcher_detail {}
using namespace gos_static_prop_batcher_detail;

extern const bool s_alphaTrace;
const float* gos_GetObjectFixBMVPMat4();
extern const bool s_treeDiagTrace;
extern const bool s_spotlightRealTrace;
extern const bool s_globalPoolLegacy;
extern bool s_baseInstanceSupported;
extern bool s_v5Armed;
extern bool s_v5Disarmed;
extern bool s_v6Armed;
extern bool s_v6Disarmed;
extern std::vector<RenderCore::DrawPacket> s_snapV6Packets;
extern std::vector<StaticPropDispatchMeta> s_snapV6Meta;
extern uint32_t s_spBuildAttempted;
extern uint32_t s_spBuildCountMismatch;
extern uint32_t s_spBuildPacketMismatch;
extern uint32_t s_spBuildMetaMismatch;
extern uint32_t s_spBuildFallback;
uint32_t pipelineId_to_group(uint32_t pid);
extern uint32_t s_v6FrameDrawsIssued;
extern uint32_t s_v6FrameZeroInstSkips;
extern uint32_t s_v6FrameSortedOob;
extern uint32_t s_v6FramePacketOob;
extern uint32_t s_v6FrameTypeOob;
extern uint32_t s_v6FrameLockstepViolations;
extern uint32_t s_v6FrameGlErrors;
extern uint32_t s_v6TotalFrameCount;
extern uint32_t s_v5FrameDrawsIssued;
extern uint32_t s_v5FrameZeroInstSkips;
extern uint32_t s_v5FrameSortedOob;
extern uint32_t s_v5FramePacketOob;
extern uint32_t s_v5FrameTypeOob;
extern uint32_t s_v5FrameBaseInstMissing;
extern uint32_t s_v5FrameGlErrors;
extern uint32_t s_v5TotalFrameCount;
extern int g_lightProbeSetupPath;
extern float g_iblShStrength;
extern float g_terrainDecalFill;
extern float g_terrainDecalColorBlend;
extern float g_pbrV1Strength;
extern bool  g_pbrV1RoughnessOverrideEnabled;
extern float g_pbrV1RoughnessOverrideValue;
namespace gos_static_prop_batcher_detail {
extern uint64_t s_sharedVboBytes;
extern uint64_t s_sharedIboBytes;
extern uint32_t s_frameSlot;
extern size_t   s_instanceCapacity;
extern size_t   s_colorCapacity;
extern std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;
extern std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> s_persistentStaticStore;
extern uint64_t s_persistentStaticGen;
extern std::vector<GpuStaticPropInstance> s_lastBuiltBatch;
extern std::vector<GpuStaticPropPacket>                   s_packets;
extern std::vector<uint8_t>                                s_packetAlphaBaked;   // per-packet alpha-test answer baked at finalize; cleared with s_packets
extern std::vector<GpuStaticPropType>                     s_types;
extern std::unordered_map<TG_TypeNodeId, uint32_t>  s_typeIndex;
// Distance LOD for model-override statics: base (LOD0) typeID -> the typeID that draws each LOD of the same shape node.
// target[0] == the base type; dist[k] is the authored switch distance of LOD k (render units == MC2 units).
struct PropLodGroup { uint32_t target[3]; float dist[3]; int count; };
extern std::unordered_map<uint32_t, PropLodGroup> s_lodGroups;
extern std::unordered_map<uint64_t, uint8_t>      s_lodState;   // per-instance hysteresis memory, keyed by (typeID, position)
extern std::vector<RenderCore::StaticPropTypeDesc>        s_typeDescTable;
extern bool s_gpuPropsTrace;
extern unsigned g_regCall[4];
extern unsigned g_regLateDrop[4];
extern std::vector<uint32_t> s_perTypePeak;
extern uint32_t s_coalesceFrameSlot;
extern std::vector<uint8_t>  s_stagingVbo;
extern std::vector<uint32_t> s_stagingIbo;
extern bool s_geometryFinalized;
extern bool s_fatalRegistrationFailure;
extern const bool s_materialGpuEnabled;
extern const bool s_materialGpuSampleEnabled;
bool staticPropAmbientV1Enabled();
bool iblShEnabled();
float iblShStrengthNightScaled();
bool pbrV1Enabled();
extern const bool s_viewUniformsDisabled;
extern const RenderCore::IblShSet* s_currentShSet;
const float* iblShCoeffsForUpload();
extern int s_staticPropDebugMaterialMode;
extern bool s_materialGpuSidecarValid;
extern std::vector<uint32_t>                s_packetMaterialIdx;
extern std::vector<RenderCore::MaterialGpu> s_materialGpuTable;
extern std::vector<StaticPropMaterialInventoryEntry> s_materialInventory;
extern std::vector<int32_t> s_packetTexArrayLayer;
extern std::vector<float> s_packetUvScaleX;
extern std::vector<float> s_packetUvScaleY;
extern std::vector<int32_t> s_typeAlbedoLayer;
extern std::vector<int32_t> s_typeAlphaClass;
extern std::vector<float> s_typeHotColors;
extern std::unordered_map<uint32_t, uint32_t> s_typeInstanceCountPrevFrame;
extern std::vector<uint8_t>              s_slotBucketIndex;
extern std::vector<uint32_t>             s_bucketCmdCount;
extern std::vector<uint32_t> s_sortedTypeOrder;
extern uint32_t s_alphaOffCount;
extern uint32_t s_alphaOnCount;
extern size_t   s_offGroupTotalBytes;
extern size_t   s_onGroupTotalBytes;
extern size_t   s_coalescePerFrameInstanceBytes;
extern std::vector<uint32_t> s_sortedPacketOrder;
extern uint32_t s_alphaOffCmdCount;
extern uint32_t s_alphaOnCmdCount;
extern std::vector<DWORD> s_coalescePinnedNodes;
extern bool s_coalesceLayoutReady;
extern bool s_coalesceEnabled;
extern bool s_coalesceArmed;
extern bool s_coalesceFirstFlushDone;
extern bool s_coalesceEnvDisabled;
extern CoalesceForceDisarm s_coalesceForceDisarm;
extern const bool s_coalesceTrace;
extern const uint32_t s_globalInstanceCap;
extern std::vector<uint32_t> s_staticBaseInstanceForType;
extern uint64_t              s_staticBaseBuiltGen;
extern std::vector<StaticDrawCmd>   s_staticDrawCmds;
bool staticPopSplitArmed();
extern uint32_t s_offGroupCountThisFrame;
extern size_t   s_offGroupBytesThisFrame;
extern size_t   s_onGroupBytesThisFrame;
extern size_t   s_totalUsedBytesThisFrame;
void coalesceRollbackTexBuild(std::vector<DWORD>& tempPins);
extern std::unordered_map<TG_TypeNodeId, bool> s_failedTypes;
extern ObjBatcherCounters s_counters;
extern bool s_objbatcherTrace;
extern bool s_objbatcherTraceInit;
extern bool s_atexitRegistered;
extern std::unordered_map<std::string, uint32_t> s_lateRegisterCounts;
extern std::unordered_set<std::string> s_lateRegisterAllowlist;
extern bool s_lastSubmitWasLateReg;
extern uint64_t s_late_register_recovery_skips;
extern GpuStaticPropPopulation s_currentSubmitPop;
extern float s_currentSubmittingActorCenter[3];
extern bool  s_currentSubmittingActorCenterValid;
extern uint64_t s_lateReg_allowed_events;
extern uint64_t s_lateReg_disallowed_events;
extern bool                       s_submitTypeHistEnabled;
extern bool                       s_submitTypeHistInit;
extern std::array<uint64_t, 1024> s_submitTypeHist;
extern uint64_t                   s_submitTypeHistCalls;
void emitSubmitTypeHistDump(std::string_view trigger);
void emitSubmitTypeHistAtExit();
void emitFinalSummaryAtExit();
void loadLateRegisterAllowlistOnce();
extern std::unordered_map<uint32_t, TypeRangeSsbo> s_typeRanges;
extern uint32_t s_lastUploadedSlot;
}  // namespace gos_static_prop_batcher_detail

inline void coalesce_resetEnvOnce() {
    static bool s_done = false;
    if (s_done) return;
    s_coalesceEnvDisabled = false;

    // Step 12B.1 — exact, case-sensitive string match on the four valid
    // values; anything else (including unset) leaves None. Process-once.
    {
        // Enum row: an unrecognized value falls back to the (empty) default token and leaves None, which
        // guards against typos activating a hook with an unintended disarm path.
        if      (mc2::gate::is<"MC2_COALESCE_FORCE_DISARM", "mixed_alpha">())   s_coalesceForceDisarm = CoalesceForceDisarm::MixedAlpha;
        else if (mc2::gate::is<"MC2_COALESCE_FORCE_DISARM", "size_mismatch">()) s_coalesceForceDisarm = CoalesceForceDisarm::SizeMismatch;
        else if (mc2::gate::is<"MC2_COALESCE_FORCE_DISARM", "no_extension">())  s_coalesceForceDisarm = CoalesceForceDisarm::NoExtension;
        else if (mc2::gate::is<"MC2_COALESCE_FORCE_DISARM", "alloc_failed">())  s_coalesceForceDisarm = CoalesceForceDisarm::AllocFailed;
    }
    s_done = true;
}

// STATICPROP-MATERIAL-ORM-1 — derive the ORM sidecar path from a SOURCE texture
// name (getTextureName returns the .tga/.txm source path, never .ktx2). Strip
// the last extension that appears AFTER the last path separator (so a dot in a
// directory name is never mistaken for an extension), then append ".orm.ktx2".
static std::string deriveOrmSidecar(const char* srcName) {
    std::string s(srcName ? srcName : "");
    size_t sep = s.find_last_of("/\\");
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        s.erase(dot);
    return s + ".orm.ktx2";
}

// The [TREE_DIAG] stream is named for the 2026-05-05 "black billboard square"
// bug, but submit() runs for EVERY static-prop leaf of EVERY population — the
// name has never scoped the output. On 2026-07-29 that cost real time: eight
// `path=dyn` records were read as proof that trees were dynamically submitting,
// and a root cause was written on top of that reading. (They were building
// leaves: the same run reported submit_buildings=3061, submit_trees=0,
// cpu_fallback=0 — every submitMultiShape succeeded, and every success was
// tagged Building.) A trace that cannot say which population it is describing
// invites exactly that inference, so every record now names it.
inline const char* popShortName(GpuStaticPropPopulation p) {
    switch (p) {
        case GpuStaticPropPopulation::Building: return "bldg";
        case GpuStaticPropPopulation::Tree:     return "tree";
        case GpuStaticPropPopulation::Generic:  return "generic";
        case GpuStaticPropPopulation::Legacy:   return "legacy";
    }
    return "?";
}

inline int popIndex(GpuStaticPropPopulation pop) {
    return static_cast<int>(pop);
}

inline void initTraceOnce() {
    if (!s_objbatcherTraceInit) {
        s_objbatcherTrace     = false;
        s_objbatcherTraceInit = true;
    }
    if (!s_atexitRegistered) {
        s_atexitRegistered = true;
        atexit(emitFinalSummaryAtExit);
    }
}

#endif // GOS_STATIC_PROP_BATCHER_INTERNAL_H
