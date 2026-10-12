// Shared surface of the gos_static_prop_batcher translation units (gos_static_prop_batcher*.cpp).
//
// The pieces are contiguous slices of the original file, in file order (core, state, program, mapload, finalize, submit, flush,
// shadow, seam); list them in the build in that order so namespace-scope initialisers run in their original order.
// State more than one slice touches is declared here and defined in the slice that owns it.
#ifndef GOS_STATIC_PROP_BATCHER_INTERNAL_H
#define GOS_STATIC_PROP_BATCHER_INTERNAL_H

#include "gos_static_prop_batcher.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_gpu_sync.h"                 // GPU-SYNC-CONTRACT typed barrier helper
#include "render_snapshot.h"             // ExtractedStaticPropPacket full definition for batcher_getDrawSlotEntry
#include "gos_static_prop_registry.h"    // M1.5: getRecipeIndexForType
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled + objectIdRawForStaticPropRecipe
#include "../../RenderCore/MaterialGpu.h"   // MaterialGpu-2: sidecar upload
#include "../../RenderCore/GpuBufferOwner.h" // STATICPROP-MATERIAL-SSBO-OWNER-1: owner record
#include "../../RenderCore/IblShCoeffs.h"   // V-IBL-STATIC-1: SH-L2 constants
#include "../../RenderCore/IblShRegistry.h"  // V-IBL-STATIC-2: per-mission SH set
#include "../../RenderCore/StaticPropTypeDesc.h"  // v0: cross-seam immutable type table
#include "../../RenderCore/KtxLoader.h"    // KTX2 sidecar loading (MC2_MATERIAL_KTX=1)
#include "../../RenderCore/RenderDebugView.h"  // DEBUG-VIEW-REGISTRY-1
#include "draw_packet_emitter.h"
#include "../../RenderCore/PipelineRegistry.h"   // direct include; do not rely on transitive
#include "pipeline_binder.h"                     // applyPipeline — GL state from PipelineDesc
#include "render_frame_plan.h"                   // STATIC-PROP-COLOR-LABEL-HARDENING-1: pass self-report
#include "mdi_submit.h"                          // MDI-SUBMISSION-SCAFFOLD-1
#include "static_prop_dispatch_meta.h"
#include "gos_postprocess.h"             // getGosPostProcess, getDynamicLightSpaceMatrix
#include "gos_terrain_lighting.h"        // POOL-CONSUMERS-1: UploadObjectLightPoolUniforms
#include "gos_static_prop_killswitch.h"  // gos_GetGLTextureId
#include "gos_mech_killswitch.h"          // g_mechPreviewRenderDepth (mech-bay preview gate)
#include "gos_profiler.h"
#include "gos_object_parity.h"           // Slice 2 Stage 2.D.1 parity harness
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
#include "../../RenderCore/top_level_apply_axes.h"   // FRAMEGRAPH-APPLY-STATE-EXTEND-1: applyTopLevelGenericAxes
#include "../../RenderCore/frame_executor.h"          // PER-PASS-APPLY-COUNTERS-1: ApplyPassId
#include "gameos.hpp"
#include "utils/shader_builder.h"
#include "tgl.h"  // TG_Shape::s_worldToClip
#include "gl_state_guard.h"  // GLSTATE-GUARD-SITE5: mc2gl::GlScoped* epilogue guards
#include "Gates/gate_cold.h"  // typed gate registry: get<>(), isSet<>(), str<>()
#include <GL/glew.h>
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
extern uint32_t g_mc2FrameCounter;

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


// ---------------------------------------------------------------------------
// [SPFLUSH_COST_SPLIT v1] — batcher-side RDTSC cycle accumulators.
// Implemented here (submitCachedInstance is in this TU).
// Gate: MC2_STATIC_PROP_FLUSH_COST_SPLIT=1, default OFF.
// Adder functions are declared in gos_static_prop_batcher.h (file scope);
// the registry + txmmgr TUs call them after measuring spans with __rdtsc().
// ---------------------------------------------------------------------------
#include <intrin.h>  // __rdtsc (MSVC intrinsic)

#include "mc2_hitch_trace.h"


#define TREE_DIAG(fmt, ...) \
    do { if (s_treeDiagTrace) { fprintf(stderr, "[TREE_DIAG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } } while (0)


// COUNTER-HONESTY-1: [TREE_DIAG] per-instance records are capped at this many
// per path per process. That makes the stream a SAMPLE, and a sample must say so
// in its own output — a reader who counts log lines must not be able to mistake
// them for a total. Both emission sites print `sample=firstN` on every record and
// emit one SAMPLE-CAP-REACHED notice when they stop.
static constexpr int kTreeDiagSample = 8;


// IBL-NIGHT-SCALE-1: declared at GLOBAL scope on purpose. iblShStrengthNightScaled()
// lives inside the anonymous namespace below, so an `extern` declared *inside* it
// would resolve to `(anonymous)::gos_GetNightState` and fail to link against the
// real definition in gameos_graphics.cpp.
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

// Stage 2.D.1.1 (Item 4): STATIC_PROP_RING_FRAMES in the header equals this.
// Enforce in lock-step so a future edit to either side fails loudly.
static_assert(RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "RING_FRAMES and STATIC_PROP_RING_FRAMES must match — "
              "changing one without the other corrupts the parity SSBO readback");

constexpr size_t   INITIAL_INSTANCES_PER_FRAME = 4096;

constexpr size_t   INITIAL_COLORS_PER_FRAME    = 1'000'000;  // uint32 ARGB entries

constexpr uint32_t BASE_INSTANCE_SSBO_BINDING     = 16u;
}  // namespace gos_static_prop_batcher_detail

#define s_dynamicPropShadowSsbo (static_cast<GLuint>(s_dynamicPropShadowOwner.backendHandle))
namespace gos_static_prop_batcher_detail {


//
// The parity SSBO is allocated lazily by gos_object_parity::EnsureParityOutputSSBO()
// the first time flush() runs with MC2_OBJECT_PARITY_CHECK=1 set. It's sized
// kParitySlotBytes per slot * RING_FRAMES (matches s_fence ring depth).
//
// Stock tier1 missions show on the order of low-thousands of GPU-drawn
// instances per frame. Worst per-type vertex count is bounded by the
// largest TG_TypeShape::numTypeVertices in the loaded map (typically
// 100-500 verts for buildings, less for trees). 4 MB per slot covers
// ~1M uint32 entries (1M lit-ARGB writes), which is comfortably above
// the actual draw load (RAlt+0 stress survey ~250-400K verts/frame on
// tier1's busiest shoppingmall+suburb missions). Sized at a power of two
// so the per-type byte alignment trim is cheap.
constexpr size_t kParitySlotBytes = 4 * 1024 * 1024;


// Forward decl -- body appears after state block below, so it can reference
// s_fatalRegistrationFailure which is declared further down in this namespace.
void ensureRingCapacity(size_t neededInstances, size_t neededColorEntries);


// CPU staging for the current frame.
// Instances are staged in per-type buckets (not a flat list) so that
// flush() can write each type's instances into a contiguous SSBO region.
// Binding that region via glBindBufferRange means gl_InstanceID in the
// shader is 0..N-1 within the bucket -- NOT dependent on gl_BaseInstance
// and NOT requiring any extension.
struct PerTypeBucket {
    std::vector<GpuStaticPropInstance> instances;
    std::vector<uint32_t>              colors;  // concatenated per-instance color blocks
    // 2b Stage 2 Task-0 diagnostic (MC2_BUCKET_ORDER_TRACE): per-frame static-vs-
    // dynamic add counts + which path touched this bucket first. Decides Mechanism
    // A (static-front persist) vs B (separate persistent store). Throwaway.
    uint32_t dbgStatic  = 0;
    uint32_t dbgDynamic = 0;
    uint8_t  dbgFirst   = 0;   // 0=none, 1=static-first, 2=dynamic-first
    // VK color feed: range of `instances` that reinjectPersistentStatic() bulk-copied
    // from the persistent store this frame. The VK feed already flattens the store,
    // so it skips this range and appends only the per-frame dynamic remainder.
    uint32_t reinjPos   = 0;
    uint32_t reinjCount = 0;
};


// Populated at flush time: per-type contiguous byte offset into the
// ring-slot SSBO (instance + color), used to bind exactly that range.
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


// COMPRESSION-BC7-STATICPROP-2 — bucketed static-prop arrays (gate ON only).
// When MC2_STATICPROP_BC7=1 the build splits each alpha group into one
// GL_TEXTURE_2D_ARRAY per (group,width,height) "bucket": uniform-dim by
// construction so a bucket can be BC7-compressed (all layers BC7) or RGBA8
// independently. The gate-OFF path is UNTOUCHED and continues to use the two
// named handles above. These vectors stay empty when the gate is OFF.
//
//   s_bucketArrays[b]      = GL_TEXTURE_2D_ARRAY handle for bucket b
//   s_bucketInfo[b]        = {group,w,h,layerCount,isBc7} for logging/inspection
//   s_packetBucketIndex[g] = global bucket index for global packet g (-1 = none)
//   s_slotBucketIndex[s]   = bucket index for sorted draw slot s (gate ON only)
//   s_bucketCmdCount[b]    = number of contiguous draw cmds (sorted slots) in b
//
// texArrayLayer remains BUCKET-RELATIVE (layer within the bound bucket array) —
// identical semantics to the old group-relative layer, so NO shader/ABI change.
struct StaticPropBucketInfo {
    uint8_t  group;       // 0=alpha-OFF, 1=alpha-ON (alpha render order preserved)
    uint16_t w;
    uint16_t h;
    uint32_t layerCount;
    bool     isBc7;
    GLuint   glArray;
};

// FOLIAGE-STATICPROP-DEPTH-PREPASS-1 GL_EQUAL-parity (review CRITICAL-1/IMPORTANT-1):
// the depth program is a DISTINCT GL program object, so its uniform state is
// independent of the color program's. The depth frag mirrors the color frag's
// alpha-derivation, so it needs the SAME u_materialGpuSample (texArrayLayer vs
// MaterialTable.albedoTex select) and u_debugAddrMode (mode-8 LODBUG bypass)
// uploaded to ITS program — uninitialized GL uniforms default to 0, which would
// make the prepass discard differently than the color pass under GL_EQUAL and
// make foliage vanish. Coalesce-only locations; -1 (legacy/absent) skips upload.
struct DepthCoalesceLocs {
    GLint drawIDBase        = -1;
    GLint materialGpuSample = -1;   // CRITICAL-1: u_materialGpuSample
    GLint debugAddrMode     = -1;   // IMPORTANT-1: u_debugAddrMode
};


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
struct ProgramLocs {
    // Shared (both programs).
    GLint terrainMVP       = -1;
    GLint mvp              = -1;
    GLint fogValue         = -1;
    GLint debugAddrMode    = -1;
    // Legacy-only.
    GLint maxLocalVertexID = -1;
    GLint materialFlags    = -1;
    GLint packetID         = -1;
    // Coalesce-only.
    GLint drawIDBase          = -1;
    GLint texArr              = -1;
    GLint materialGpuSample   = -1;   // MaterialGpu-3: u_materialGpuSample
    GLint ambientV1Strength   = -1;   // V-AMBIENT-STATIC-1: u_ambientV1Strength
    GLint propBaseLightDirStrength = -1; // PROP-BASE-LIGHT-DIR-1: u_propBaseLightDirStrength
    GLint debugMaterialMode   = -1;   // V-MATERIAL-DEBUG-1: u_debugMaterialMode
    GLint iblSh               = -1;   // V-IBL-STATIC-1: u_iblSh[9] (vec3 array)
    GLint iblShStrength       = -1;   // V-IBL-STATIC-1: u_iblShStrength
    GLint pbrV1Strength       = -1;   // V-MATERIAL-PBR-2: u_pbrV1Strength
    GLint pbrV1RoughnessOverride = -1; // V-MATERIAL-PBR-2-TUNE-UI: u_pbrV1RoughnessOverride
    GLint ormTexArr           = -1;   // STATICPROP-MATERIAL-ORM-1: u_ormTexArr (sampler2DArray)
    GLint ormSampleEnable     = -1;   // STATICPROP-MATERIAL-ORM-1: u_ormSampleEnable (int)
    GLint pathTint            = -1;   // MC2_SHADER_PATH_TINT debug: u_pathTint (int)
    GLint terrainSunMC2       = -1;   // GREYBEARD-DIFFUSE-TEST: u_terrainSunMC2 (vec3)
    // EDGE-LIGHT-EMISSION-1: stock night-tag emission state.
    GLint isNight            = -1;   // u_isNight (int 0/1)
    GLint nightFactor        = -1;   // u_nightFactor (float 0..1)
    GLint nightEmissiveScale = -1;   // u_nightEmissiveScale (float)
    GLint fixtureEmissiveScale = -1; // u_fixtureEmissiveScale (BEACON-FIXTURE-ID-1)
    GLint fixtureGlowStrength  = -1; // u_fixtureGlowStrength  (FIXTURE-GLOW-1)
    GLint terrainDecalFill    = -1;   // TERRAIN-DECAL-FILL-1: u_terrainDecalFill (float)
    // TERRAIN-DECAL-COLORBLEND-1: colormap sampler + atlas UV params + blend.
    GLint terrainColormap        = -1;  // u_terrainColormap (sampler2D)
    GLint decalAtlasTLX          = -1;  // u_decalAtlasTLX (float)
    GLint decalAtlasTLY          = -1;  // u_decalAtlasTLY (float)
    GLint decalAtlasOOW          = -1;  // u_decalAtlasOOW (float)
    GLint terrainDecalColorBlend = -1;  // u_terrainDecalColorBlend (float)
    GLint glassDither            = -1;  // BUILDING-GLASS-1: u_glassDither (float)
    // FAR-BACKDROP-LIT: night-shade substitution for far/impostor LODs.
    GLint farBackdropLit         = -1;  // u_farBackdropLit (int, 0=off)
    GLint farBackdropNear        = -1;  // u_farBackdropNear (float)
    GLint farBackdropFar         = -1;  // u_farBackdropFar (float)
    GLint farBackdropStrength    = -1;  // u_farBackdropStrength (float)
    GLint farBackdropTint        = -1;  // u_farBackdropTint (vec3)
};


// STATICPROP-MATERIAL-ORM-1 — texture unit reserved for the per-bucket ORM
// sibling array. Unit 0 stays the albedo array (u_texArr); unit 1 is ORM.
static constexpr GLuint kOrmTexUnit = 1;

// TERRAIN-DECAL-COLORBLEND-1 — texture unit for the terrain colormap atlas,
// sampled only by the flagged cliff decal. Clear of albedo(0)/ORM(1).
static constexpr GLuint kColormapTexUnit = 4;



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


extern bool g_useGpuObjects;
extern const bool s_alphaTrace;
const float* gos_GetObjectFixBMVPMat4();
extern const bool s_treeDiagTrace;
extern const bool s_spotlightRealTrace;
extern std::atomic<uint64_t> g_staticPropAlphaGeneration;
extern const bool s_spflushCostSplitEnabled;
extern const bool s_globalPoolLegacy;
extern const bool s_legacyInstancePools;
extern const bool s_v5Enabled;
extern const bool s_v5TraceEnabled;
extern bool s_baseInstanceSupported;
extern bool s_v5Armed;
extern bool s_v5Disarmed;
extern const bool s_v6Enabled;
extern const bool s_v6TraceEnabled;
extern bool s_v6Armed;
extern bool s_v6Disarmed;
extern std::vector<RenderCore::DrawPacket> s_snapV6Packets;
extern std::vector<StaticPropDispatchMeta> s_snapV6Meta;
extern const bool s_snapshotBuildEnabled;
extern const bool s_keepLiveBuilder;
extern bool s_v8ArmLogged;
extern uint32_t s_spBuildAttempted;
extern uint32_t s_spBuildCountMismatch;
extern uint32_t s_spBuildPacketMismatch;
extern uint32_t s_spBuildMetaMismatch;
extern uint32_t s_spBuildFallback;
extern uint32_t s_spBuildRetired;
extern bool s_spBuildFirstFallbackLogged;
extern bool s_spBuildCollisionLoggedOnce;
uint32_t pipelineId_to_group(uint32_t pid);
extern uint32_t s_v6FrameDrawsIssued;
extern uint32_t s_v6FrameZeroInstSkips;
extern uint32_t s_v6FrameSortedOob;
extern uint32_t s_v6FramePacketOob;
extern uint32_t s_v6FrameTypeOob;
extern uint32_t s_v6FrameLockstepViolations;
extern uint32_t s_snapCullSkipped;
extern uint32_t s_snapCullActive;
extern uint32_t s_snapCullSlotMismatch;
extern const bool s_snapCullEnabled;
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
uint32_t buildLiveV6Arrays( uint32_t totalCmds, const uint32_t* baseInstanceMap, std::vector<RenderCore::DrawPacket>& outPackets, std::vector<StaticPropDispatchMeta>& outMeta);
bool objPolyOffsetEnabled();
float objPolyOffsetFactor();
float objPolyOffsetUnits();
bool flushDepthPrepassV6( const std::vector<RenderCore::DrawPacket>&  dispatchPackets, const std::vector<StaticPropDispatchMeta>&  dispatchMeta, uint32_t                                    totalCmds, bool                                        bc7Buckets, int                                         debugAddrMode_);
extern bool s_staticPropStateAppliedByExecutor;
void executorApplyStaticPropOpaqueState();
namespace gos_static_prop_batcher_detail {
extern GLuint s_sharedVbo;
extern GLuint s_sharedIbo;
extern uint64_t s_sharedVboBytes;
extern uint64_t s_sharedIboBytes;
extern GLuint s_sharedVao;
extern GLuint s_perTypeSsbo;
extern GLuint s_staticBldgShadowSsbo;
extern RenderCore::GpuBufferOwner s_dynamicPropShadowOwner;
extern std::unordered_set<uint32_t> s_buildingShadowTypeIDs;
extern GLuint   s_instanceSsbo;
extern GLuint   s_colorSsbo;
extern RenderCore::GpuBufferOwner s_instanceSsboOwner;
extern RenderCore::GpuBufferOwner s_colorSsboOwner;
extern void*    s_instanceMap;
extern void*    s_colorMap;
extern GLsync   s_fence[RING_FRAMES];
extern uint32_t s_frameSlot;
extern size_t   s_instanceCapacity;
extern size_t   s_colorCapacity;
extern size_t   s_parityBytesUsedThisFrame;
extern size_t   s_parityBytesUsedPerSlot[RING_FRAMES];
extern std::vector<uint8_t> s_parityReadbackScratch;
extern std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;
extern const bool s_staticPropColorsFill;
extern const bool s_bucketOrderTrace;
extern std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> s_persistentStaticStore;
extern uint64_t s_persistentStaticGen;
extern std::vector<GpuStaticPropInstance> s_lastBuiltBatch;
extern std::vector<GpuStaticPropPacket>                   s_packets;
extern std::vector<GpuStaticPropType>                     s_types;
extern std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;
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
extern glsl_program* s_staticPropProgramObj;
extern GLuint        s_staticPropProgram;
extern GLint s_loc_u_parityWrite;
extern GLint s_loc_u_parityVertsPerType;
extern GLint s_loc_u_parityBaseVertex;
extern bool s_programLoadTried;
extern bool s_programLoadFailed;
extern GLuint s_coalesceInstanceSsbo;
extern GLuint s_perDrawSsbo;
extern const bool s_materialGpuEnabled;
extern const bool s_materialGpuSampleEnabled;
bool staticPropAmbientV1Enabled();
extern const float s_propBaseLightDirStrength;
extern const int s_nightTagMode;
extern const float s_nightTagScale;
extern const float s_fixtureLightsScale;
extern const float s_fixtureGlowStrength;
bool isEmissiveFixtureType(DWORD pink, DWORD yellow, DWORD green);
extern unsigned s_fixtureTypeCount;
extern const bool s_farBackdropLitEnabled;
extern const float s_farBackdropNear;
extern const float s_farBackdropFar;
extern const float s_farBackdropStrength;
extern float s_farBackdropTint[3];
bool iblShEnabled();
float iblShStrengthNightScaled();
bool pbrV1Enabled();
extern const bool s_ormSlotsEnabled;
extern const bool s_viewUniformsDisabled;
extern const RenderCore::IblShSet* s_currentShSet;
const float* iblShCoeffsForUpload();
extern int s_staticPropDebugMaterialMode;
extern bool s_materialKtxEnabled;
int texTier();
extern bool s_staticPropBc7Enabled;
extern bool s_materialGpuSidecarValid;
extern std::vector<uint32_t>                s_packetMaterialIdx;
extern std::vector<RenderCore::MaterialGpu> s_materialGpuTable;
extern std::vector<StaticPropMaterialInventoryEntry> s_materialInventory;
extern std::vector<int32_t> s_packetTexArrayLayer;
extern std::vector<float> s_packetUvScaleX;
extern std::vector<float> s_packetUvScaleY;
extern std::vector<int32_t> s_typeAlbedoLayer;
extern std::vector<int32_t> s_typeAlphaClass;
extern std::unordered_map<uint32_t, uint32_t> s_typeInstanceCountPrevFrame;
extern bool s_reinjectedThisFrame;
extern RenderCore::GpuBufferOwner           s_materialGpuSsbo;
extern GLuint s_texArrayOff;
extern GLuint s_texArrayOn;
extern std::vector<GLuint>               s_bucketArrays;
extern std::vector<StaticPropBucketInfo> s_bucketInfo;
extern std::vector<int32_t>              s_packetBucketIndex;
extern std::vector<uint8_t>              s_slotBucketIndex;
extern std::vector<uint32_t>             s_bucketCmdCount;
extern std::vector<GLuint>               s_ormBucketArrays;
extern std::vector<std::vector<bool>>    s_ormLayerHasMap;
extern GLuint s_permutationSsbo;
extern GLuint s_staticPropProgramCoalesce;
extern GLuint s_staticPropDepthProgram;
extern DepthCoalesceLocs s_locsDepthCoalesce;
extern void*  s_coalesceInstanceMap;
extern GLsync s_coalesceFence[RING_FRAMES];
extern std::vector<uint32_t> s_sortedTypeOrder;
extern uint32_t s_alphaOffCount;
extern uint32_t s_alphaOnCount;
extern size_t   s_offGroupTotalBytes;
extern size_t   s_onGroupTotalBytes;
extern size_t   s_coalescePerFrameInstanceBytes;
extern std::vector<uint32_t> s_sortedPacketOrder;
extern uint32_t s_alphaOffCmdCount;
extern uint32_t s_alphaOnCmdCount;
extern GLuint   s_cmdToBucketSsbo;
extern std::vector<DWORD> s_coalescePinnedNodes;
extern bool s_coalesceLayoutReady;
extern bool s_coalesceEnabled;
extern bool s_coalesceArmed;
extern bool s_coalesceFirstFlushDone;
extern bool s_coalesceEnvDisabled;
extern bool s_hasShaderDrawParams;
extern CoalesceForceDisarm s_coalesceForceDisarm;
extern const bool s_coalesceTrace;
extern const uint32_t s_globalInstanceCap;
extern std::vector<uint32_t> s_staticBaseInstanceForType;
extern uint64_t              s_staticBaseBuiltGen;
extern GLuint                s_staticInstanceSsbo;
extern void*                 s_staticInstanceMap;
extern size_t                s_staticInstanceBytes;
extern uint64_t              s_staticInstanceFillGen;
extern GLuint                       s_staticIndirectCmdBuf;
extern std::vector<StaticDrawCmd>   s_staticDrawCmds;
extern uint64_t                     s_staticCmdBuiltGen;
extern GLsync                       s_staticDrawFence;
bool staticPopSplitArmed();
extern GLuint   s_baseInstanceByCmdSsbo;
extern void*    s_baseInstanceByCmdMap;
extern size_t   s_baseInstanceByCmdBytesPerFrame;
extern uint32_t s_offGroupCountThisFrame;
extern size_t   s_offGroupBytesThisFrame;
extern size_t   s_onGroupBytesThisFrame;
extern size_t   s_totalUsedBytesThisFrame;
extern const bool s_glassDitherEnabled;
extern ProgramLocs s_locsLegacy;
extern ProgramLocs s_locsCoalesce;
void allocPermutationSsboAsIdentity(uint32_t typeCount);
GLenum bptcInternalFormatFor(bool isSrgb);
void staticPropReleaseBuckets();
void coalesceRollbackTexBuild(std::vector<DWORD>& tempPins);
void loadProgramsIfNeeded();
extern std::unordered_map<const TG_TypeShape*, bool> s_failedTypes;
extern ObjBatcherCounters s_counters;
bool staticPropLightFreshnessEnabled();
extern bool s_objbatcherTrace;
extern bool s_objbatcherTraceInit;
extern bool s_atexitRegistered;
extern std::unordered_map<std::string, uint32_t> s_lateRegisterCounts;
extern std::unordered_set<std::string> s_lateRegisterAllowlist;
extern bool s_lastSubmitWasLateReg;
extern uint64_t s_late_register_recovery_skips;
extern const TG_MultiShape* s_currentSubmittingMulti;
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
bool uploadAllBucketsIfNeeded();
}  // namespace gos_static_prop_batcher_detail

// --- Step 2.4: env-once helper (spec §7). ---
// Resolve the kill-switch ONCE per process. Idempotent guard via a function-
// local static. Called at the top of loadProgramsIfNeeded() (primary site —
// runs before the s_programLoadTried latch) and also as a no-op guard from
// IsCoalesceEnabled() so legacy callers that bypass loadProgramsIfNeeded
// still get a defined env-flag value. Step 12B.1 will extend this body to
// also parse MC2_COALESCE_FORCE_DISARM.
inline void coalesce_resetEnvOnce() {
    static bool s_done = false;
    if (s_done) return;
    s_coalesceEnvDisabled = mc2::gate::get<"MC2_SUBSTRATE_COALESCE_LEGACY">();

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

// MC2_STATICPROP_TEX_TIER — derive the tiered KTX2 path from a SOURCE texture name.
// getTextureName() returns the .tga/.txm source path (e.g. "data/tgl/foo.tga").
// This function constructs the tier-directory path:
//   "data/tgl/foo.tga" + tier=512 -> "data/tgl/512/foo.ktx2"
// The tier subdirectory is inserted between the parent directory and the filename.
// If no tier directory is found the plain sidecar path (flat .ktx2) is returned
// so the existing fallback / miss path handles it gracefully.
static std::string deriveTieredKtxPath(const char* srcName, int tier) {
    std::string s(srcName ? srcName : "");
    const size_t sep = s.find_last_of("/\\");
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        s.erase(dot);
    const size_t lastSep = s.find_last_of("/\\");
    if (lastSep == std::string::npos) {
        return std::to_string(tier) + "/" + s + ".ktx2";
    }
    return s.substr(0, lastSep + 1) + std::to_string(tier) + "/"
           + s.substr(lastSep + 1) + ".ktx2";
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
        s_objbatcherTrace     = mc2::gate::get<"MC2_OBJBATCHER_TRACE">();
        s_objbatcherTraceInit = true;
    }
    if (!s_atexitRegistered) {
        s_atexitRegistered = true;
        atexit(emitFinalSummaryAtExit);
    }
}

#endif // GOS_STATIC_PROP_BATCHER_INTERNAL_H
