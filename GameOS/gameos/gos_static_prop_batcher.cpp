#include "gos_static_prop_batcher.h"
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
#include "gos_static_prop_killswitch.h"  // gos_GetGLTextureId
#include "gos_mech_killswitch.h"          // g_mechPreviewRenderDepth (mech-bay preview gate)
#include "gos_profiler.h"
#include "gos_object_parity.h"           // Slice 2 Stage 2.D.1 parity harness
#include "gpu_cull_compute.h"            // C1b: compute_isEnabled, getIndirectCmdBuf, getBucketCount
#include "gpu_cull_substrate.h"          // 2026-05-10: substrate_appendStaticPropRecord
#include "gpu_cull_record.h"             // 2026-05-10: GpuActorRecord, Cat_StaticProp
#include "../../mclib/terrain.h"         // C1b temporal-superset: Terrain::worldToBlockIdx()
#include "../../mclib/render_contract.h" // [RENDER_PASS v1] noteRenderPass
#include "../../RenderCore/top_level_pass_executor.h" // APPLY-STATE-STATICPROP-1: findTopLevelStateDesc
#include "../../RenderCore/top_level_apply_axes.h"   // FRAMEGRAPH-APPLY-STATE-EXTEND-1: applyTopLevelGenericAxes
#include "../../RenderCore/frame_executor.h"          // PER-PASS-APPLY-COUNTERS-1: ApplyPassId
#include "gameos.hpp"
#include "utils/shader_builder.h"
#include "tgl.h"  // TG_Shape::s_worldToClip
#include <GL/glew.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Global runtime toggle for the GPU static-prop renderer. Defined here
// (in the gameos lib) so every consumer — mc2.exe, aseconv, other data
// tools that link mclib — resolves the symbol.
bool g_useGpuStaticProps = false;
// Default-on as of 2026-05-04. Flipped without Stage 2.E visual-diff gate
// passing — Stage 2.D parity check (29 missions, 0 mismatches, +0 destroys)
// establishes numerical correctness; visual confirmation is via prior
// power-user testing with MC2_GPU_OBJECTS=1. Set MC2_GPU_OBJECTS=0 in
// env to opt out.
bool g_useGpuObjects = true;

static const bool s_alphaTrace = (getenv("MC2_ALPHA_TEST_TRACE") != nullptr);
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
// NOTE: raw gos_terrain_indirect_getDispatchMvp16() is phase-private to terrain/
// water passes — object draw uses gos_GetObjectDrawMVP() (the accessor above).
// Enforced by scripts/check-object-mvp-currency.py.

// MC2_PROP_FIXB_MVP — default ON (=0 reverts, killswitch). Consume the
// dispatch-MVP snapshot when the solid pass is armed so object draws agree with
// terrain depth during camera motion (the un-armed arm returns the live pointer
// anyway, so this is byte-identical when the solid pass is not armed). Sampled
// once at process start.
static bool s_propFixBMvpEnabled() {
    static const bool on = []() {
        const char* v = std::getenv("MC2_PROP_FIXB_MVP");
        return !(v && std::atoi(v) == 0);  // default ON; only =0 reverts
    }();
    return on;
}
// Returns the MVP the object draw should upload (GL_FALSE, axisSwap*worldToClip).
// Gate OFF -> live terrain MVP. Gate ON + solid pass armed -> dispatch snapshot,
// with live-MVP nullptr-safety identical to the terrain chunk fallback.
// RENDER-VIEW-CURRENCY-1: snapshot only when its view epoch matches the current
// view; otherwise the live MVP. Closes the BUG1/3/5 staleness (objects projected
// through a stale / mid-frame-superseded camera). Killswitch preserved via the arg.
static const float* gos_GetObjectFixBMVPMat4() {
    return gos_GetObjectDrawMVP(s_propFixBMvpEnabled());
}

// MC2_TEX_HANDOFF_TRACE=1 — logs texture handle resolution at register and draw time.
// Prints once per unique (multiShape,slot) pair at registration, and once per draw type
// at flush (first flush only). Use to diagnose the GPU-path upscale-texture miss.
static const bool s_texHandoffTrace = (getenv("MC2_TEX_HANDOFF_TRACE") != nullptr);
#define TEX_HANDOFF(fmt, ...) \
    do { if (s_texHandoffTrace) { printf("[TEX_HANDOFF] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// MC2_TREE_DIAG_TRACE=1 — diagnostic 2026-05-05 for the "black billboard square" bug.
// Prints up to 8 lines from each of submit() (dynamic register/replay path) and
// submitCachedInstance() (static recipe replay path), with all GpuStaticPropInstance
// fields formatted identically so a side-by-side grep diff pinpoints which baked
// field is stale on the static replay. Counters are function-local statics so they
// reset per process lifetime; format matches both call sites verbatim.
static const bool s_treeDiagTrace = (getenv("MC2_TREE_DIAG_TRACE") != nullptr);

// MC2_SPOTLIGHT_REAL_TRACE=1 — (E) SpotLight_ → real illumination baseline counter.
// Stage 0 instrumentation (T0.1, plan 2026-05-20). Counts per-frame static-prop
// submitMultiShape events where the child has isSpotlight==true (i.e. the per-instance
// shape whose source typeShape carried the "SpotLight_*" node-name prefix and that
// the CPU path tags via tgl.cpp:259/475). First-hit is always-on (one-line stderr
// confirms a SpotLight_ child was observed at least once even without the env);
// per-frame and 600-frame summary streams are env-gated to keep the log quiet by
// default. Pattern mirrors the [INSTR v1] schema family + the 600-frame window
// used by [OBJBATCHER v1] / [TGL_POOL v1].
static const bool s_spotlightRealTrace = (getenv("MC2_SPOTLIGHT_REAL_TRACE") != nullptr);

// ---------------------------------------------------------------------------
// [SPFLUSH_COST_SPLIT v1] — batcher-side RDTSC cycle accumulators.
// Implemented here (submitCachedInstance is in this TU).
// Gate: MC2_STATIC_PROP_FLUSH_COST_SPLIT=1, default OFF.
// Adder functions are declared in gos_static_prop_batcher.h (file scope);
// the registry + txmmgr TUs call them after measuring spans with __rdtsc().
// ---------------------------------------------------------------------------
#include <intrin.h>  // __rdtsc (MSVC intrinsic)
#include "mc2_hitch_trace.h"

// SP-BATCHER-ALPHASCAN-GATE-1: see header. Starts at 1 so a fresh scan (cached
// gen 0) always runs its first pass regardless of any early destruction events.
std::atomic<uint64_t> g_staticPropAlphaGeneration{1};

static const bool s_spflushCostSplitEnabled = []() {
    const char* v = getenv("MC2_STATIC_PROP_FLUSH_COST_SPLIT");
    return v && v[0] == '1' && v[1] == '\0';
}();

namespace {
unsigned long long s_spflush_submit_map_lookup_cyc = 0;
unsigned long long s_spflush_color_zero_fill_cyc   = 0;
}  // namespace

namespace spflush_cost_split {
void AddSubmitMapLookupCycles(unsigned long long c) {
    if (s_spflushCostSplitEnabled) s_spflush_submit_map_lookup_cyc += c;
}
void AddColorZeroFillCycles(unsigned long long c) {
    if (s_spflushCostSplitEnabled) s_spflush_color_zero_fill_cyc += c;
}
unsigned long long ConsumeSubmitMapLookupCycles() {
    const unsigned long long v = s_spflush_submit_map_lookup_cyc;
    s_spflush_submit_map_lookup_cyc = 0;
    return v;
}
unsigned long long ConsumeColorZeroFillCycles() {
    const unsigned long long v = s_spflush_color_zero_fill_cyc;
    s_spflush_color_zero_fill_cyc = 0;
    return v;
}
}  // namespace spflush_cost_split

// MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1 — forces the legacy per-type-cap
// coalesce layout (keeps existing path; new global-pool path is disabled).
// Env-read once at process start; survives until soak completes.
static const bool s_globalPoolLegacy = []() {
    const char* v = getenv("MC2_STATIC_PROP_GLOBAL_POOL_LEGACY");
    return v != nullptr && v[0] != '0';
}();

// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03) — when set, forces the LEGACY
// per-instance frame-pool path (_PositionsOnly + non-null listOfVertices gate)
// for GPU-registered static-prop types. Default (unset) is the new GPU-only
// path: registered instances run the zero-pool hierarchy walk in *Appearance::
// update and submit straight from rec.shapeToWorld, so the TGL frame pools stay
// flat regardless of override mesh size × instance count. This flag is the A/B
// escape hatch. Env-read once at process start.
static const bool s_legacyInstancePools = []() {
    const char* v = getenv("MC2_LEGACY_INSTANCE_POOLS");
    return v != nullptr && v[0] != '0';
}();
bool gos_StaticPropLegacyInstancePools() { return s_legacyInstancePools; }

// DrawPacket v5: per-draw-call substitutive dispatch.
// Gate: MC2_DRAW_PACKET_COALESCE_V5=1
// Extension: ARB_base_instance (GL 4.2 core, available on all tier1 GPUs).
static const bool s_v5Enabled = []() -> bool {
    const char* v = std::getenv("MC2_DRAW_PACKET_COALESCE_V5");
    return v && v[0] == '1';
}();
static const bool s_v5TraceEnabled = []() -> bool {
    const char* v = std::getenv("MC2_DRAW_PACKET_COALESCE_V5_TRACE");
    return v && v[0] == '1';
}();
// GLboolean: GL_TRUE on any GL 4.2+ context. Evaluated lazily via GLEW at first use.
// Safe: GLEW is initialized before any batcher code runs (gos_Init() order).
static bool s_baseInstanceSupported = false;  // set on first gate-ON flush
static bool s_v5Armed      = false;  // true once gate-arm checks have run
static bool s_v5Disarmed   = false;  // true if gate-arm check failed for session

// DrawPacket v7: canonical packet+meta array dispatch — DEFAULT ON.
// Kill-switch: MC2_STATIC_PROP_LEGACY_DISPATCH=1 reverts to legacy multidraw.
// MC2_DRAW_PACKET_STATIC_PROP_V6 (old opt-in) is a no-op; gate plumbing removed in v7.1.
// Requires: ARB_base_instance (checked independently of v5 arm block).
static const bool s_v6Enabled = []() -> bool {
    const char* kill = std::getenv("MC2_STATIC_PROP_LEGACY_DISPATCH");
    return !(kill && kill[0] == '1');
}();
static const bool s_v6TraceEnabled = []() -> bool {
    const char* v = std::getenv("MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE");
    return v && v[0] == '1';
}();
static bool s_v6Armed    = false;
static bool s_v6Disarmed = false;

// v3 snapshot build: separate dispatch arrays for snapshot-built path.
// Reused each flush via resize() — no heap alloc after first frame.
static std::vector<RenderCore::DrawPacket> s_snapV6Packets;
static std::vector<StaticPropDispatchMeta> s_snapV6Meta;

// v3 env gate. File-local — not exported in header. Cached at process start.
// DEFAULT ON (STATIC-PROP-V3-FLIP, 2026-05-27). Kill-switch: MC2_SNAPSHOT_STATIC_PROP_BUILD=0.
// Unset or any value other than literal '0' = ON. Live builder + compare authority retained as fallback.
static const bool s_snapshotBuildEnabled = []() -> bool {
    const char* v = std::getenv("MC2_SNAPSHOT_STATIC_PROP_BUILD");
    return !(v && v[0] == '0');
}();
// STATIC-PROP-SNAPSHOT-FINISH (v8): snapshot is the sole draw-packet owner.
// DEBUG/LEGACY kill-switch — default OFF (retired). =1 restores the v3-flip dual
// build (live + snapshot_packet_build) + compare path for regression bisect / A-B.
static const bool s_keepLiveBuilder = []() -> bool {
    const char* keep = std::getenv("MC2_STATIC_PROP_LIVE_BUILDER");
    return keep && keep[0] == '1';
}();
// One-shot arm log latch (the arm log itself is emitted in a LATER task, when retirement is real).
static bool s_v8ArmLogged = false;

// v3 per-flush counters. Reset each flush by the runV6 block.
// Read by batcher_getSnapshotBuildStats() for render_snapshot.cpp ok gate.
static uint32_t s_spBuildAttempted      = 0u;
static uint32_t s_spBuildCountMismatch  = 0u;
static uint32_t s_spBuildPacketMismatch = 0u;
static uint32_t s_spBuildMetaMismatch   = 0u;
static uint32_t s_spBuildFallback       = 0u;
static uint32_t s_spBuildRetired        = 0u;  // v8: 1 when live builder + compare retired this flush
// Latched on first fallback; never reset. Guards the first-occurrence log line.
static bool s_spBuildFirstFallbackLogged = false;
// Latched on first snap-cull collision; never reset. Guards the one-shot collision log line.
static bool s_spBuildCollisionLoggedOnce = false;

// Returns 0 (opaque) or 1 (alpha-test). Returns 0xFFFFFFFFu for unknown pipelineId.
// Used by snapshot builder to derive group from the snapshot row's stored pipelineId.
static uint32_t pipelineId_to_group(uint32_t pid) {
    using P = RenderCore::PipelineId;
    if (pid == static_cast<uint32_t>(P::StaticPropOpaque))    return 0u;
    if (pid == static_cast<uint32_t>(P::StaticPropAlphaTest)) return 1u;
    return 0xFFFFFFFFu;
}

// v6 per-frame counters (reset each v6-active flush).
static uint32_t s_v6FrameDrawsIssued        = 0u;
static uint32_t s_v6FrameZeroInstSkips      = 0u;
static uint32_t s_v6FrameSortedOob          = 0u;
static uint32_t s_v6FramePacketOob          = 0u;
static uint32_t s_v6FrameTypeOob            = 0u;
static uint32_t s_v6FrameLockstepViolations = 0u;

// v2.3 snap-cull: per-frame counters. Reset at top of flush(); read via batcher_getSnapCullStats().
static uint32_t s_snapCullSkipped      = 0u;
static uint32_t s_snapCullActive       = 0u;
static uint32_t s_snapCullSlotMismatch = 0u;
// Env-var gate: strictly MC2_SNAP_CULL=1. Unset and MC2_SNAP_CULL=0 both leave snap-cull OFF.
static const bool s_snapCullEnabled = [] {
    const char* v = std::getenv("MC2_SNAP_CULL");
    return v && v[0] == '1';
}();
static uint32_t s_v6FrameGlErrors           = 0u;
static uint32_t s_v6TotalFrameCount         = 0u;

// v5 per-frame counters (reset at start of each v5-active flush).
static uint32_t s_v5FrameDrawsIssued     = 0u;
static uint32_t s_v5FrameZeroInstSkips   = 0u;
static uint32_t s_v5FrameSortedOob       = 0u;
static uint32_t s_v5FramePacketOob       = 0u;
static uint32_t s_v5FrameTypeOob         = 0u;
static uint32_t s_v5FrameBaseInstMissing = 0u;
static uint32_t s_v5FrameGlErrors        = 0u;
static uint32_t s_v5TotalFrameCount      = 0u;

#define TREE_DIAG(fmt, ...) \
    do { if (s_treeDiagTrace) { fprintf(stderr, "[TREE_DIAG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } } while (0)

// [GPUPROPS v1] setup-path attribution for registerType calls.
// 0=unknown 1=mission_init_firstload 2=ims_objmgr_load 3=sp_logistics_postinit
// Declared extern in gos_static_prop_batcher.h; set by mission.cpp / saveload.cpp / logistics.cpp.
int g_lightProbeSetupPath = 0;

// V-IBL-STATIC-1: ImGui slider value (extern decl in ibl_sh_runtime.h).
// Default 1.0 -> nominal projector output strength when env-gate is on.
// Defined at file scope (NOT in anonymous namespace) for external linkage
// so GuiRuntime/EditorInspector.cpp can drive the slider.
//
// V-IBL-STATIC-1-SOAK: optional MC2_STATIC_PROP_IBL_SH_STRENGTH env override
// for the default. ImGui slider remains the interactive tuning surface; the
// env exists so headless capture harnesses can drive strength deterministically
// from a parent process. Parsed once at process start, clamped 0.0..3.0.
// Env unset/empty -> default 0.5f (V-IBL-STATIC-1-TUNE: user eyeball pass
// at e061e44c selected 0.5 as 'looks good' — subtle ambient fill without
// over-brightening shadowed sides).
float g_iblShStrength = []() -> float {
    const char* v = std::getenv("MC2_STATIC_PROP_IBL_SH_STRENGTH");
    if (!v || !v[0]) return 0.5f;
    float f = (float)std::atof(v);
    if (f < 0.0f) f = 0.0f;
    if (f > 3.0f) f = 3.0f;
    return f;
}();

// TERRAIN-DECAL-FILL-1: ambient/fill floor for the cliff-wall mesh-decal's
// shadow side. static_prop lighting is max(N·L,0) with no ambient floor, so a
// face pointing away from the sun goes to ~0 (black void). This uniform is a
// per-fragment floor applied ONLY to fragments whose instance carries the
// kFlagDecalFill bit (bit 3), set on the MarbleCliff decal instances at
// registration (bdactor.cpp). All other static props (mechs/buildings/trees)
// never carry that bit, so they are byte-identical regardless of this value.
// Default 0.20 (modest — shadow side reads as dark rock, not black; lit side
// unaffected because litRgb is already >= the floor there). MC2_TERRAIN_DECAL_FILL
// overrides (clamped 0..1). External linkage so GuiRuntime.cpp can drive a slider.
float g_terrainDecalFill = []() -> float {
    const char* v = std::getenv("MC2_TERRAIN_DECAL_FILL");
    if (!v || !v[0]) return 0.20f;
    float f = (float)std::atof(v);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}();

// TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-color match for the cliff mesh
// decal. Blends the flagged decal's albedo toward the terrain COLORMAP sampled
// at the decal's world-XY (same atlas + world->UV transform the terrain chunk
// shader uses). Applied ONLY to kFlagDecalFill (bit 3) fragments in the shader;
// 0.0 (default when MC2_TERRAIN_DECAL_COLORBLEND unset) OR flag-unset OR
// blend==0 -> byte-identical for every other static prop AND the decal itself.
// Default 0.35 (subtle base-color pull toward terrain). Env override clamped
// 0..1. External linkage so GuiRuntime.cpp can drive the "Color blend" slider.
float g_terrainDecalColorBlend = []() -> float {
    const char* v = std::getenv("MC2_TERRAIN_DECAL_COLORBLEND");
    if (!v || !v[0]) return 0.35f;
    float f = (float)std::atof(v);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}();

// V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel specular slider value
// (interactive). Default 1.0f. Only contributes when the env-gate
// s_pbrV1Enabled is ON (and MC2_VIEW_UNIFORMS is not disabled). Optional env
// override MC2_STATIC_PROP_PBR_V1_STRENGTH wins over the default; clamped
// 0..3 to match the slider range. External linkage so EditorInspector.cpp
// can drive the slider (same pattern as g_iblShStrength).
float g_pbrV1Strength = []() -> float {
    const char* v = std::getenv("MC2_STATIC_PROP_PBR_V1_STRENGTH");
    // 2026-05-28: default dialled back from 1.0 → 0.5 per user direction.
    // Full-strength Schlick specular was too blunt on flat-roofed legacy
    // assets without material specular masking; 0.5 is the new "looks
    // sensible without bespoke per-material tuning" baseline. Env override
    // still wins (clamped 0..3).
    if (!v || !v[0]) return 0.5f;
    float f = (float)std::atof(v);
    if (f < 0.0f) f = 0.0f;
    if (f > 3.0f) f = 3.0f;
    return f;
}();

// V-MATERIAL-PBR-2-TUNE-UI: ImGui-driven runtime roughness override layer
// on top of the static_prop.vert 0.6 literal baseline. Default DISABLED;
// per-frame upload sends -1.0 sentinel which the shader treats as "use
// literal" -> byte-identical to V-MATERIAL-PBR-2-TUNE. When the inspector
// flips _Enabled, the slider value (0.05..1.0) is uploaded and overrides
// the literal. Debug/tuning surface only -- no MaterialGpu read, no
// batcher default change, no schema change.
// 2026-05-28: defaults flipped to enabled=true / value=0.95 per user
// direction. The 0.6 literal in static_prop.vert produced too-glossy
// highlights on legacy assets; the runtime override at 0.95 makes
// surfaces visibly rougher (closer to the look the assets were authored
// for) without touching the shader. Override stays env-disable-able via
// the Static Prop Tuning ImGui checkbox.
bool  g_pbrV1RoughnessOverrideEnabled = true;
float g_pbrV1RoughnessOverrideValue   = 0.95f;

namespace {

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

// Slice 1 (global instance pool). Default global cap and base-instance SSBO binding.
constexpr uint32_t STATIC_PROP_GLOBAL_CAP_DEFAULT = 131072u;
constexpr uint32_t BASE_INSTANCE_SSBO_BINDING     = 16u;

// Immutable per-map geometry.
GLuint s_sharedVbo = 0;
GLuint s_sharedIbo = 0;
GLuint s_sharedVao = 0;

// Slice 2 (object-offload) — Stage 2.C.2: per-type hot-color SSBO.
// Holds {hotPinkRGB, hotYellowRGB, hotGreenRGB} per registered TG_TypeShape,
// indexed by inst.typeID at draw time. Built once at finalizeGeometry from
// TG_TypeShape::hotPinkRGB/hotYellowRGB/hotGreenRGB (mclib/tgl.h:575-577) and
// uploaded as immutable storage. Consumed by static_prop.vert via std430
// binding 2 to feed get_base_light()'s per-type magic-color parameters.
// 48 bytes per type × ~50 types ≈ 2.4 KB total.
GLuint s_perTypeSsbo = 0;
// SHADOW-STATIC-BUILDINGS-2: one-shot all-buildings static shadow instance SSBO
// (declared here so onMapUnload can free it; built in drawStaticBuildingShadows).
static GLuint s_staticBldgShadowSsbo = 0;
// SHADOW-DYNAMIC-PROP-CASTERS-1: one-shot all-non-building-props dynamic shadow
// instance SSBO (declared here so onMapUnload can free it; built per-frame in
// drawDynamicPropShadows). Separate from the static building SSBO so the two
// passes never alias.
// SCENE-SSBO-OWNER-SWEEP-1: this is the ONE live default-path buffer in COMPLETENESS-
// SWEEP gap B#5 -- the dynamic prop-shadow caster path is gated MC2_SHADOW_DYNAMIC_PROP_
// CASTERS (DEFAULT ON) inside the always-on dynamic shadow pass, so it is created on the
// stock run. Narrowed behind a GpuBufferOwner identity record; every gen/bind/bufferData/
// bindbase/delete site reaches the raw handle ONLY via s_dynamicPropShadowSsbo (the
// #define below aliases the bare name to owner.glName -> zero site churn). Same GL args/
// slot/flags/order. Mission lifetime: rebuilt per frame but freed on onMapUnload;
// registered at create (bufferData), invalidated on destroy.
// The sibling buffers in gap B#5 are DEFERRED to the exclusion ledger (created only under
// default-OFF gates): s_staticBldgShadowSsbo (MC2_STATIC_PROP_BUILDING_SHADOW, default
// OFF); s_blockVisSsbo (gos_vegetation.cpp, MC2_VEGETATION_CARDS, default OFF); particle
// s_ssbo (gos_particle_bridge.cpp, MC2_GPU_PARTICLES, default OFF -- CPU/MLR is the stock
// path) + s_tubePos/Col/UvSsbo (MC2_VFX_ORACLE_TUBE, default OFF/parked).
static RenderCore::GpuBufferOwner s_dynamicPropShadowOwner{
    RenderCore::RenderResourceId::DynamicPropShadowSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "DynamicPropShadowSsbo",
    0u};
#define s_dynamicPropShadowSsbo (s_dynamicPropShadowOwner.glName)
// typeIDs drawn into the static building shadow map. When the static building
// shadow is active, flushShadow(skipStaticBuildingTypes=true) skips these in the
// DYNAMIC pass so buildings don't cast a redundant (fuzzy) second shadow.
static std::unordered_set<uint32_t> s_buildingShadowTypeIDs;

// Per-frame persistent-mapped rings.
GLuint   s_instanceSsbo = 0;
GLuint   s_colorSsbo    = 0;
void*    s_instanceMap  = nullptr;
void*    s_colorMap     = nullptr;
GLsync   s_fence[RING_FRAMES] = {0};
uint32_t s_frameSlot = 0;
size_t   s_instanceCapacity = 0;
size_t   s_colorCapacity    = 0;

// Slice 2 (object-offload) — Stage 2.D.1: parity readback harness.
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

// Per-slot byte usage of the parity SSBO. Tracks how much of slot
// kParitySlotBytes was actually written in the most recent visit to that
// slot, so the next-visit readback at the top of flush() can scope its
// glGetBufferSubData to exactly the live range. Indexed by s_frameSlot.
size_t   s_parityBytesUsedThisFrame = 0;
size_t   s_parityBytesUsedPerSlot[RING_FRAMES] = {0};
// Discard buffer for the readback. Sized once on first parity-on visit;
// std::vector keeps it alive for the lifetime of the process so we don't
// reallocate per frame. Bytes are read into this and DISCARDED — Stage
// 2.D.1 does no compare; 2.D.2/2.D.3 will use these bytes against a CPU
// recompute for the byte-wise compare.
std::vector<uint8_t> s_parityReadbackScratch;

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
};
std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;

// STATICPROP-COLORS-FILL-DEBUGONLY-1: the per-instance Colors SSBO (binding 1) is
// read by NO production shader — static_prop.vert declares colors_ but never uses
// it, firstColorOffset is unused, and the coalesce multidraw never binds binding 1
// (only the legacy non-coalesce draw does, gos_static_prop_batcher.cpp ~:6256).
// So the per-static-instance colors zero-fill (~80ns/leaf at wolfman) is pure
// waste. DEFAULT-SKIP for static-registry instances; MC2_STATIC_PROP_COLORS_FILL=1
// restores the legacy fill (kill-switch). Dynamic submit()/submitMultiShape path
// is untouched.
static const bool s_staticPropColorsFill =
    (std::getenv("MC2_STATIC_PROP_COLORS_FILL") != nullptr &&
     std::getenv("MC2_STATIC_PROP_COLORS_FILL")[0] != '0');

// 2b Stage 2 Task-0: bucket static/dynamic order + overlap probe (throwaway).
static const bool s_bucketOrderTrace = (std::getenv("MC2_BUCKET_ORDER_TRACE") != nullptr);

// 2b Stage 2 (Mechanism B-reinject): persistent per-type static instance store,
// rebuilt only when the registry generation changes. Each frame the registry
// flush BULK-injects these blocks into s_bucketsByType (one memcpy per type)
// instead of re-walking the registry per leaf — killing the per-leaf walk/push/
// map overhead. Downstream (uploadAllBucketsIfNeeded + coalesce pool build) read
// s_bucketsByType unchanged. Static blocks carry 0 colors (Stage 1).
static std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> s_persistentStaticStore;
static uint64_t s_persistentStaticGen = 0xFFFFFFFFFFFFFFFFull;  // sentinel = never built

// Stage 3.C: per-submitMultiShape batch accumulator. Cleared at the start
// of each submitMultiShape(); populated by submit() per leaf. After
// submitMultiShape() returns true, getLastBuiltBatch() returns this vector
// for snapshot registration in GpuStaticPropRegistry.
std::vector<GpuStaticPropInstance> s_lastBuiltBatch;

// Populated at flush time: per-type contiguous byte offset into the
// ring-slot SSBO (instance + color), used to bind exactly that range.
struct TypeRangeSsbo {
    size_t instanceByteOffset;
    size_t instanceByteSize;
    size_t colorByteOffset;
    size_t colorByteSize;
    uint32_t instanceCount;
};

// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;
std::vector<RenderCore::StaticPropTypeDesc>        s_typeDescTable; // v0: cross-seam mirror

// [GPUPROPS v1] setup-path instrumentation (internal; path tag lives at file scope).
static bool s_gpuPropsTrace = (getenv("MC2_GPUPROPS_TRACE") != nullptr);
// g_regCall counts EVERY registerType invocation after the null guard and
// BEFORE the idempotent s_typeIndex.count early-return, so 0 for a path means
// registerType was genuinely never invoked on it.
static unsigned g_regCall[4]     = {0,0,0,0};
static unsigned g_regLateDrop[4] = {0,0,0,0};

// Peak instance count per typeID since mission load; resized lazily in the
// per-frame flush loop. Slice-1: tracked but not consumed (foundation for
// slice-2 GPU-emit, where peak[t] drives slot allocation).
static std::vector<uint32_t> s_perTypePeak;

// Coalesce ring frame slot — separate from legacy s_frameSlot (line ~:95).
// Advanced inside batcher_prepareBaseInstanceTable() each frame under
// non-legacy mode; mirrors s_frameSlot under legacy mode. Reset per-mission
// in onMapLoad() for hygiene.
static uint32_t s_coalesceFrameSlot = 0;


// CPU-side staging during registration (cleared after finalizeGeometry).
std::vector<uint8_t>  s_stagingVbo;
std::vector<uint32_t> s_stagingIbo;

bool s_geometryFinalized = false;
bool s_fatalRegistrationFailure = false;

// Main static-prop program (Task 9). Lazy-loaded on first flush()/flushShadow().
// We keep a glsl_program* around for any future uniform-introspection needs,
// but most call sites will read the raw GL handle via s_staticPropProgram.
glsl_program* s_staticPropProgramObj = nullptr;
GLuint        s_staticPropProgram    = 0;

// Stage 2.D.1.1 (Item 2): parity uniform locations cached at link time.
// Querying glGetUniformLocation every flush was a per-flush driver round-trip;
// cache once here. The shader is not hot-reloadable (s_programLoadTried latches
// after first attempt, no in-process relink path), so the cache is valid for
// the lifetime of the program handle. -1 signals "not found in shader" and is
// safe to pass to glUniform*i (the spec says glUniform* is a no-op for loc=-1).
GLint s_loc_u_parityWrite        = -1;
GLint s_loc_u_parityVertsPerType = -1;
GLint s_loc_u_parityBaseVertex   = -1;

// Latched once a compile/link attempt has failed. We never retry inside a
// session because shader source can only change between runs. With this
// latched true, submit() returns false (so callers CPU-fallback), and
// flush()/flushShadow() short-circuit immediately. The user can keep the
// killswitch ON or OFF with no behavioral difference until the next build.
bool s_programLoadTried  = false;
bool s_programLoadFailed = false;

// ===========================================================================
// Substrate Multi-Draw Coalesce — file-scope state (plan v3.8 Step group 2).
// All state below is the coalesce path's; it is INDEPENDENT of the legacy
// per-type/per-packet path which remains the authoritative draw method.
// Coalesce is a side-attempt: failures disarm coalesce only, never legacy.
// ===========================================================================

// Step 2.3 — forward decl so coalesce_resetEnvOnce / IsCoalesceEnabled can
// reference each other across the helpers below.
static bool IsCoalesceEnabled();

// --- Step 2.1: GL handles for coalesce SSBOs and texture arrays ---
GLuint s_coalesceInstanceSsbo      = 0;  // ring-buffered, persistent-mapped
GLuint s_perDrawSsbo               = 0;  // PerDrawEntry per type, sorted (binding 4)

// MaterialGpu-2 sidecar — active by default (v5: default-ON).
// No shader consumer until MaterialGpu-3.
// s_packetMaterialIdx[i] maps draw slot i (= s_sortedPacketOrder[i] position)
// to its entry in s_materialGpuTable.
// Size invariant: s_packetMaterialIdx.size() == s_sortedPacketOrder.size().

// MC2_MATERIAL_GPU — defaults ON.
// Set MC2_MATERIAL_GPU=0 to disable table upload, SSBO bind, and compare.
// Upload gate: active by default; powers compare invariant (albedoTex == texArrayLayer).
static const bool s_materialGpuEnabled = []() {
    const char* v = getenv("MC2_MATERIAL_GPU");
    return v == nullptr || (v[0] != '0');
}();

// MC2_MATERIAL_GPU_SAMPLE — defaults ON (shader sampling active by default).
// Set MC2_MATERIAL_GPU_SAMPLE=0 to disable shader sampling and fall back to texArrayLayer.
// sampleOn requires s_materialGpuEnabled (MC2_MATERIAL_GPU) also active — see sampleOn gate.
// Invariant: materials[materialIdx].albedoTex == texArrayLayer must hold while both paths live.
static const bool s_materialGpuSampleEnabled = []() {
    const char* v = getenv("MC2_MATERIAL_GPU_SAMPLE");
    return v == nullptr || (v[0] != '0');
}();

// V-AMBIENT-STATIC-1: hemisphere ambient fill. Default-OFF; only ON when
// MC2_STATIC_PROP_AMBIENT_V1 is set to a non-"0" value. When OFF (default),
// the uploaded strength is 0.0 so static_prop.vert's hemisphere term is a
// bitwise no-op and the output is byte-identical to pre-slice af314d22.
static const bool s_staticPropAmbientV1Enabled = []() {
    const char* v = getenv("MC2_STATIC_PROP_AMBIENT_V1");
    return v != nullptr && v[0] != '0' && v[0] != '\0';
}();

// V-IBL-STATIC-1: SH-L2 image-based ambient lane.
//   - env var MC2_STATIC_PROP_IBL_SH is the AUTHORITATIVE GATE
//     (resolved once at process start; matches V-AMBIENT-STATIC-1 pattern).
//   - runtime global g_iblShStrength (below) is the ImGui slider value;
//     it ONLY contributes when the env-gate is on.
//   - per-frame upload: s_iblShEnabled ? g_iblShStrength : 0.0f.
//     => env unset/0 -> upload 0.0 -> shader `if (u_iblShStrength > 0.0)`
//        short-circuits to byte-identical pre-slice output (proof of
//        default-OFF kill-switch parity, stronger than multiply-by-zero).
//     => env=1 + slider=0.0 -> upload 0.0 -> visually disabled but the
//        feature flag IS active in the registry (env-only gate).
//     => env=1 + slider=N -> upload N (range 0.0..3.0 enforced by slider UI).
static const bool s_iblShEnabled = []() {
    // V-IBL-DEFAULT-FLIP (2026-05-27): default-ON; explicit "=0" is kill-switch.
    // env unset -> true; env="0" -> false; env="1"/anything else -> true.
    const char* v = getenv("MC2_STATIC_PROP_IBL_SH");
    return !(v != nullptr && v[0] == '0');
}();

// V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular gate.
// Default-OFF; "=1" / any non-"0" value enables. Matches V-IBL-STATIC-1
// pre-flip env-parse pattern. When OFF, the per-frame upload pushes
// u_pbrV1Strength=0.0f -> shader `if (u_pbrV1Strength > 0.0)` short-circuits
// before any u_cameraWorldPos access -> mathematically byte-identical
// (strength=0 -> lit += specular * 0 = lit unchanged).
static const bool s_pbrV1Enabled = []() {
    const char* v = getenv("MC2_STATIC_PROP_PBR_V1");
    return v != nullptr && v[0] != '0' && v[0] != '\0';
}();

// STATICPROP-MATERIAL-ORM-1: gate for per-bucket linear ORM (occlusion-
// roughness-metallic) texture-array slots + sidecar feed. Default-OFF; "=1"
// / any non-"0", non-empty value enables. Mirrors the s_pbrV1Enabled env-parse
// idiom. When OFF, zero ORM arrays are built and zero extra sidecar I/O occurs
// so the gate-OFF path is byte-identical.
static const bool s_ormSlotsEnabled = []() {
    const char* v = getenv("MC2_STATICPROP_MATERIAL_PBR_SLOTS");
    return v != nullptr && v[0] != '0' && v[0] != '\0';
}();

// V-MATERIAL-PBR-2 safety interlock: when MC2_VIEW_UNIFORMS=0 the shader's
// PBR block is excluded by `#if defined(MC2_USE_VIEW_UNIFORMS)` (compile-
// time guard). This runtime flag is the belt to the compile-time
// suspenders: even if a build slips through with the guard misconfigured,
// strength is force-zeroed here so the path stays dormant. Cached once
// at process start; mirrors the IBL env-gate idiom.
static const bool s_viewUniformsDisabled = []() {
    const char* v = getenv("MC2_VIEW_UNIFORMS");
    return v != nullptr && v[0] == '0';
}();

// V-MATERIAL-PBR-2-DIAG: diagnostic visualizer gate. Default-OFF; "=1" / any
// non-"0" value enables. Mirrors s_pbrV1Enabled parse pattern. When ON AND
// PBR gate is also ON, shader replaces lit with cyan (sunFound=true) /
// magenta (false) instead of running Schlick math. Diagnostic-only; never
// commits a behavior change to the default path.
static const bool s_pbrV1DiagSunFound = []() {
    const char* v = getenv("MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND");
    return v != nullptr && v[0] != '0' && v[0] != '\0';
}();
// Note: definition of `g_iblShStrength` lives at file scope above the
// anonymous namespace (line ~191) so it has external linkage. The lambda-
// initialized s_iblShEnabled stays local here; the slider value is global.

// V-IBL-STATIC-2: per-mission SH set selection.
//   - s_currentShSet points at the active set; initialized to "default"
//     (kIblShSets[0]) so every code path -- including pre-mission frames
//     and code paths that never call setMissionForIbl (editor, savegame
//     load before mission name is plumbed) -- uploads the V-IBL-STATIC-1
//     baseline coefficients. Never nullptr.
//   - setMissionForIbl(name) is called from Mission::init right after
//     onMapLoad(); it consults the compile-time registry (kMissionShMap
//     in IblShRegistry.h) and falls back to "default" for unknown names.
//   - Optional env override MC2_STATIC_PROP_IBL_SH_SET=<name> wins over
//     the registry when set + matches an existing set; unset/empty/typo
//     -> registry. Resolved once at process start.
static const RenderCore::IblShSet* s_currentShSet = &RenderCore::kIblShSets[0];
static const char* s_iblShSetEnvOverride = []() -> const char* {
    const char* v = getenv("MC2_STATIC_PROP_IBL_SH_SET");
    return (v != nullptr && v[0] != '\0') ? v : nullptr;
}();

// V-MATERIAL-DEBUG-1: per-fragment material debug view mode for the
// StaticPropOpaque lane. Default 0 = OFF (byte-identical to legacy output —
// shader short-circuits via `if (u_debugMaterialMode != 0) { ...; return; }`).
// Env var MC2_STATIC_PROP_DEBUG_MATERIAL accepts integer 0..N:
//   0 = off (default)
//   1 = albedo (raw texture)
//   2 = materialIdx (hashed palette)
//   3 = normal (worldNormal as RGB)
//   4 = texArrayLayer (hashed palette)
//   5 = roughnessFactor as grayscale (V-MATERIAL-PBR-1)
//   6 = metallicFactor as grayscale  (V-MATERIAL-PBR-1)
// Values outside the implemented range render as hot-pink in the shader.
// Resolved once at process start (atoi semantics: invalid -> 0).
static int s_staticPropDebugMaterialMode = []() {
    const char* v = getenv("MC2_STATIC_PROP_DEBUG_MATERIAL");
    if (v == nullptr || v[0] == '\0') return 0;
    int m = atoi(v);
    if (m < 0) m = 0;
    if (m > 6) m = 6;  // V-MATERIAL-PBR-1: extended to modes 5 (roughness) + 6 (metallic)
    return m;
}();
static bool s_materialKtxEnabled = (std::getenv("MC2_MATERIAL_KTX") != nullptr &&
                                     std::getenv("MC2_MATERIAL_KTX")[0] != '0');   // MC2_MATERIAL_KTX=1
// MC2_STATICPROP_TEX_TIER: 128 | 256 | 512 (default) | 1024
// Selects which cooked tier directory (data/tgl/<tier>/<name>.ktx2) is loaded
// when MC2_MATERIAL_KTX=1. Launcher sets this from the Graphics / Texture Quality
// slider; engine reads it once at process start.
static int s_texTier = []() -> int {
    const char* e = getenv("MC2_STATICPROP_TEX_TIER");
    if (!e) return 512;
    int v = atoi(e);
    if (v == 128 || v == 256 || v == 1024) return v;
    return 512;
}();
// COMPRESSION-BC7-STATICPROP-1: gate for BC7-compressed KTX2 static-prop arrays.
// Default-OFF; effective only when s_materialKtxEnabled is also true. When the
// per-group BC7 conditions (all-or-nothing + uniform-dim) are not met, the group
// falls back to the existing RGBA8 path → byte-identical to gate-OFF.
static bool s_staticPropBc7Enabled = (std::getenv("MC2_STATICPROP_BC7") != nullptr &&
                                      std::getenv("MC2_STATICPROP_BC7")[0] != '0'); // MC2_STATICPROP_BC7=1
// Tracks whether finalizeGeometry() produced a correctly-sized sidecar.
// Reset to false at the start of every finalizeGeometry() call; set to true
// only after the sidecar loop completes with size == emitted count.
// Initialized false: no table built yet.
static bool s_materialGpuSidecarValid = false;
static std::vector<uint32_t>                s_packetMaterialIdx;  // per draw slot
static std::vector<RenderCore::MaterialGpu> s_materialGpuTable;   // deduplicated
// V-MATERIAL-STATIC-0: inventory snapshot — one row per s_materialGpuTable entry.
// Built in lockstep with s_materialGpuTable; cleared in onMapUnload. Read-only.
static std::vector<StaticPropMaterialInventoryEntry> s_materialInventory;
// v2: per-packet texArrayLayer sidecar. Indexed by global packet index.
// Copied from layerForPacket[] in finalizeGeometry() before that local is discarded.
// Valid until next finalizeGeometry() or onMapUnload(). -1 for unavailable packets.
static std::vector<int32_t> s_packetTexArrayLayer;
// v2.1: per-type instance count snapshot from the most recent flush().
// Keyed by typeID. Cleared in onMapUnload().
// Same prior-frame semantics as s_recipeHasSubstrateRecord / hasCullRecord:
//   flush(frame N) snapshots -> extraction(frame N+1) reads -> 0 on frame 1.
static std::unordered_map<uint32_t, uint32_t> s_typeInstanceCountPrevFrame;
// STATICPROP-MATERIAL-SSBO-OWNER-1: static-prop MaterialGpu table SSBO (binding 5),
// narrowed behind a GpuBufferOwner identity record (logical id + lifetime + debug
// name + GLuint value). GL calls happen at the same sites with the same
// args/order/flags/slot; the raw handle is reached only via owner.glName.
// Lifetime Mission: created in finalizeGeometry(), torn down on onMapUnload();
// rebuilt per map. Registered at create (upload), invalidated on destroy.
static RenderCore::GpuBufferOwner           s_materialGpuSsbo{
    RenderCore::RenderResourceId::StaticPropMaterialGpuBuffer,
    RenderCore::RenderResourceLifetime::Mission,
    "StaticPropMaterialGpuBuffer",
    0u};

GLuint s_texArrayOff               = 0;  // alpha-OFF group GL_TEXTURE_2D_ARRAY
GLuint s_texArrayOn                = 0;  // alpha-ON  group GL_TEXTURE_2D_ARRAY

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
std::vector<GLuint>               s_bucketArrays;       // owns GL handles (delete all on teardown)
std::vector<StaticPropBucketInfo> s_bucketInfo;
std::vector<int32_t>              s_packetBucketIndex;  // per global packet; -1 if unassigned
std::vector<uint8_t>              s_slotBucketIndex;    // per sorted draw slot
std::vector<uint32_t>             s_bucketCmdCount;     // per bucket, in sorted-slot order

// STATICPROP-MATERIAL-ORM-1 — per-bucket LINEAR ORM (occlusion-roughness-
// metallic) sibling arrays. INVARIANT: s_ormBucketArrays.size() ==
// s_bucketArrays.size(), and ORM layer k in bucket b corresponds to the SAME
// unique as albedo layer k in bucket b (metallicRoughnessTex == albedoTex).
// Each ORM array is DENSE/bindable: layers without a real sidecar get a 1x1-
// replicated neutral (255,255,255,255) texel. s_ormLayerHasMap[b][k] records
// whether bucket b layer k had a REAL ORM map (drives the material flag).
// All three vectors stay empty when the gate (s_ormSlotsEnabled) is OFF.
std::vector<GLuint>               s_ormBucketArrays;    // owns GL handles (delete all on teardown)
std::vector<std::vector<bool>>    s_ormLayerHasMap;     // per bucket, per bucket-relative layer
GLuint s_permutationSsbo           = 0;  // sortedSlot[typeID] mapping (binding 15)
GLuint s_staticPropProgramCoalesce = 0;  // coalesce variant (Step 7.5)

// FOLIAGE-STATICPROP-DEPTH-PREPASS-1: depth-prepass program object — same
// static_prop.vert (invariant gl_Position), but the cheap static_prop_depth.frag
// that does ONLY the alpha-test discard (no color/lighting/object-id). It is a
// DISTINCT GL program object, so its uniform locations differ from the color
// program's — the prepass must use THIS program's u_drawIDBase, not the color
// program's. 0 = the program failed to link this session → flushDepthPrepass()
// must no-op (color path unaffected).
GLuint s_staticPropDepthProgram = 0;
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
static DepthCoalesceLocs s_locsDepthCoalesce;

// Step 2.1 — persistent map pointer + fence ring.
void*  s_coalesceInstanceMap = nullptr;
GLsync s_coalesceFence[RING_FRAMES] = {};

// Step 2.1 — sort vectors / counts (populated in Step 5.7).
std::vector<uint32_t> s_sortedTypeOrder;
uint32_t s_alphaOffCount = 0;
uint32_t s_alphaOnCount  = 0;
size_t   s_offGroupTotalBytes = 0;
size_t   s_onGroupTotalBytes  = 0;
size_t   s_coalescePerFrameInstanceBytes = 0;  // populated in Step 5.7

// 2026-05-11 per-packet rework (multi-packet correctness fix). Each indirect
// draw command corresponds to ONE packet (not one type). Multi-packet types
// in stock content (S_admin_Core has 44 packets, fences/doors with separate
// alpha-mesh and frame packets) need per-packet textures and per-packet
// alpha-test bits — the previous per-type architecture collapsed all packets
// to the first packet's texture, dropping detail and alpha. See
// memory/substrate_coalesce_armed_multi_packet_limitation.md.
//
// Sorted-packet order: alpha-OFF packets first, alpha-ON packets second.
// A packet's alpha-class group comes from its TYPE's alphaClass (OR-reduce
// of its packets' alpha-test bits) — all packets of one type render in the
// same group so they can read instance data from the same per-group SSBO
// range; the fragment's per-packet materialFlags then drives discard.
std::vector<uint32_t> s_sortedPacketOrder;     // global packet indices in [OFF | ON] order
uint32_t s_alphaOffCmdCount = 0;               // number of packets in alpha-OFF group
uint32_t s_alphaOnCmdCount  = 0;               // number of packets in alpha-ON group
GLuint   s_cmdToBucketSsbo  = 0;               // uint typeID per cmd (binding 7, patch shader)

// Step 2.1 — pin tracker (refcount-aware unpin in Step 4.1).
std::vector<DWORD> s_coalescePinnedNodes;

// Step 2.1 — state-machine flags (§7).
bool s_coalesceLayoutReady = false;
bool s_coalesceEnabled     = false;
bool s_coalesceArmed       = false;

// Step 2.1 — per-mission ready latch (reset in onMapLoad, set in 11.7.k).
bool s_coalesceFirstFlushDone = false;

// Step 2.1 — env-resolved-once + extension-probe-persisting flags.
// Set inside coalesce_resetEnvOnce() / loadProgramsIfNeeded(), never cleared
// across mission loads (process-lifetime values).
bool s_coalesceEnvDisabled = false;
bool s_hasShaderDrawParams = false;

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
static CoalesceForceDisarm s_coalesceForceDisarm = CoalesceForceDisarm::None;

// Step 12.1 — trace gate for noisy non-lifecycle COALESCE diagnostics.
// Lifecycle events (armed/disarmed/ready/permutation_state when forced)
// are always-on; future per-frame diagnostics gated behind this env.
static const bool s_coalesceTrace =
    (getenv("MC2_SUBSTRATE_COALESCE_TRACE") != nullptr);
#define COALESCE_TRACE(fmt, ...) \
    do { if (s_coalesceTrace) { \
        std::fprintf(stderr, "[COALESCE v1] " fmt "\n", ##__VA_ARGS__); \
        std::fflush(stderr); \
    } } while (0)

// --- Global instance pool (slice 1, plan v3.8 step group 3). ---
// s_globalInstanceCap: per-ring-frame instance capacity for the global pool.
// Override via MC2_STATIC_PROP_GLOBAL_CAP env; defaults to STATIC_PROP_GLOBAL_CAP_DEFAULT.
// s_baseInstanceByCmdSsbo: persistent-mapped SSBO, one uint32_t baseInstance per
// draw command per ring frame. Binding BASE_INSTANCE_SSBO_BINDING (16).
// Allocated inside finalizeGeometry under !s_globalPoolLegacy after the per-packet
// sort builds s_alphaOffCmdCount + s_alphaOnCmdCount.
static const uint32_t s_globalInstanceCap = []() {
    const char* v = getenv("MC2_STATIC_PROP_GLOBAL_CAP");
    if (v && v[0] != '\0') {
        const uint32_t cap = static_cast<uint32_t>(std::atoi(v));
        if (cap > 0) return cap;
    }
    return STATIC_PROP_GLOBAL_CAP_DEFAULT;
}();

// M2a POPULATION-SPLIT state (defined early so onMapUnload :1729 can tear the
// SSBO down; the build/fill helpers + accessors live near s_baseInstanceForType).
static std::vector<uint32_t> s_staticBaseInstanceForType;
static uint64_t              s_staticBaseBuiltGen     = 0xFFFFFFFFFFFFFFFFull;
static GLuint                s_staticInstanceSsbo     = 0;
static void*                 s_staticInstanceMap      = nullptr;   // GL_MAP_PERSISTENT|COHERENT
static size_t                s_staticInstanceBytes    = 0;         // == cap * sizeof(instance)
static uint64_t              s_staticInstanceFillGen  = 0xFFFFFFFFFFFFFFFFull;
// Task 3: StaticPopulation indirect commands — own buffer, CPU-written (counts
// are the frozen static population, NOT GPU cull; first-N draw, see patch 2).
// 20-byte DrawElementsIndirectCommand: count, instanceCount, firstIndex,
// baseVertex, baseInstance (matches gpu_cull_compute.cpp's DrawCmd).
struct StaticDrawCmd { uint32_t count; uint32_t instanceCount; uint32_t firstIndex;
                       int32_t baseVertex; uint32_t baseInstance; };
static_assert(sizeof(StaticDrawCmd) == 20, "StaticDrawCmd must be 20 bytes");
static GLuint                       s_staticIndirectCmdBuf = 0;
static std::vector<StaticDrawCmd>   s_staticDrawCmds;          // CPU mirror
static uint64_t                     s_staticCmdBuiltGen = 0xFFFFFFFFFFFFFFFFull;
// Task 4 criterion 2: fence covering the last StaticPopulation draw. The single-
// region s_staticInstanceSsbo is overwritten in place on a dirty fill; wait on
// this before overwriting so a prior frame's in-flight draw has finished reading
// (GPU executes in submission order -> the latest fence covers all earlier).
static GLsync                       s_staticDrawFence = nullptr;

// Gate: MC2_STATIC_POP_SPLIT env AND the G2 frozen-records path armed. Defined
// early (before onMapUnload / the draw / reinject) so all use sites resolve.
static bool staticPopSplitArmed() {
    static const bool s_on = (getenv("MC2_STATIC_POP_SPLIT") != nullptr);
    return s_on && GpuStaticPropRegistry::frozenRecordsArmed();
}

GLuint   s_baseInstanceByCmdSsbo          = 0;
void*    s_baseInstanceByCmdMap           = nullptr;
size_t   s_baseInstanceByCmdBytesPerFrame = 0;

// Per-frame accumulators — zeroed by batcher_prepareBaseInstanceTable() each frame (step 4).
uint32_t s_offGroupCountThisFrame  = 0;
size_t   s_offGroupBytesThisFrame  = 0;
size_t   s_onGroupBytesThisFrame   = 0;
size_t   s_totalUsedBytesThisFrame = 0;

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
    GLint debugMaterialMode   = -1;   // V-MATERIAL-DEBUG-1: u_debugMaterialMode
    GLint iblSh               = -1;   // V-IBL-STATIC-1: u_iblSh[9] (vec3 array)
    GLint iblShStrength       = -1;   // V-IBL-STATIC-1: u_iblShStrength
    GLint pbrV1Strength       = -1;   // V-MATERIAL-PBR-2: u_pbrV1Strength
    GLint pbrV1RoughnessOverride = -1; // V-MATERIAL-PBR-2-TUNE-UI: u_pbrV1RoughnessOverride
    GLint pbrV1DiagSunFound   = -1;   // V-MATERIAL-PBR-2-DIAG: u_pbrV1DiagSunFound
    GLint ormTexArr           = -1;   // STATICPROP-MATERIAL-ORM-1: u_ormTexArr (sampler2DArray)
    GLint ormSampleEnable     = -1;   // STATICPROP-MATERIAL-ORM-1: u_ormSampleEnable (int)
    GLint pathTint            = -1;   // MC2_SHADER_PATH_TINT debug: u_pathTint (int)
    GLint terrainSunMC2       = -1;   // GREYBEARD-DIFFUSE-TEST: u_terrainSunMC2 (vec3)
    GLint terrainDecalFill    = -1;   // TERRAIN-DECAL-FILL-1: u_terrainDecalFill (float)
    // TERRAIN-DECAL-COLORBLEND-1: colormap sampler + atlas UV params + blend.
    GLint terrainColormap        = -1;  // u_terrainColormap (sampler2D)
    GLint decalAtlasTLX          = -1;  // u_decalAtlasTLX (float)
    GLint decalAtlasTLY          = -1;  // u_decalAtlasTLY (float)
    GLint decalAtlasOOW          = -1;  // u_decalAtlasOOW (float)
    GLint terrainDecalColorBlend = -1;  // u_terrainDecalColorBlend (float)
};

// STATICPROP-MATERIAL-ORM-1 — texture unit reserved for the per-bucket ORM
// sibling array. Unit 0 stays the albedo array (u_texArr); unit 1 is ORM.
static constexpr GLuint kOrmTexUnit = 1;
// TERRAIN-DECAL-COLORBLEND-1 — texture unit for the terrain colormap atlas,
// sampled only by the flagged cliff decal. Clear of albedo(0)/ORM(1).
static constexpr GLuint kColormapTexUnit = 4;
static ProgramLocs s_locsLegacy;
static ProgramLocs s_locsCoalesce;

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
    s_coalesceEnvDisabled = (getenv("MC2_SUBSTRATE_COALESCE_LEGACY") != nullptr);

    // Step 12B.1 — exact, case-sensitive string match on the four valid
    // values; anything else (including unset) leaves None. Process-once.
    if (const char* fd = getenv("MC2_COALESCE_FORCE_DISARM")) {
        if      (!strcmp(fd, "mixed_alpha"))   s_coalesceForceDisarm = CoalesceForceDisarm::MixedAlpha;
        else if (!strcmp(fd, "size_mismatch")) s_coalesceForceDisarm = CoalesceForceDisarm::SizeMismatch;
        else if (!strcmp(fd, "no_extension"))  s_coalesceForceDisarm = CoalesceForceDisarm::NoExtension;
        else if (!strcmp(fd, "alloc_failed"))  s_coalesceForceDisarm = CoalesceForceDisarm::AllocFailed;
        // Unrecognized values silently leave None — guards against typos
        // accidentally activating a hook with an unintended disarm path.
    }
    s_done = true;
}

// --- Step 2.6: identity-permutation alloc helper (plan v3.8). ---
// Called UNCONDITIONALLY by Step 5.4 before the Step 5.5 early-return so
// that even disarmed missions have a non-zero s_permutationSsbo with
// identity contents. Step 10.3 binds slot 15 every patch dispatch
// regardless of armed state; an identity permutation makes legacy fallback
// behave as if no remapping happened.
//
// Lifecycle: GL_STATIC_DRAW (matches spec §9 lifecycle table). The buffer
// is finalize-uploaded; Step 5.12 may overwrite it once via glBufferSubData
// in the same finalize pass, then it is read-only thereafter.
static void allocPermutationSsboAsIdentity(uint32_t typeCount) {
    if (typeCount == 0) return;
    std::vector<uint32_t> identity(typeCount);
    for (uint32_t i = 0; i < typeCount; ++i) identity[i] = i;
    // TIER2-EXCLUDED: dead-path
    if (s_permutationSsbo == 0) glGenBuffers(1, &s_permutationSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
    MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                 typeCount * sizeof(uint32_t),
                 identity.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// --- Step 5.10.c rollback helper (plan v3.7). ---
// Factored out of finalizeGeometry()'s per-group texture-array build so
// Step 12B.5's forced-`size_mismatch` test hook reuses the same code.
// One rollback site, two callers (natural failure + forced disarm).
//
// Per v2r15 out3-MIN-F rollback contract: delete BOTH texture arrays if
// either is non-zero (the OFF group may have succeeded before the ON
// group fails — the half-built array still leaks if not deleted), then
// unpin only the CURRENT group's temp pins. Do NOT touch the other
// group's already-promoted pins in s_coalescePinnedNodes — those release
// at onMapUnload().
//
// Caller responsibility: set s_coalesceLayoutReady / s_coalesceEnabled /
// s_coalesceArmed to false AFTER calling the helper, then return. The
// helper deliberately does NOT mutate the flags so Step 12B.5 (which
// logs a different "(forced)" reason string before invoking the
// rollback) keeps logging-vs-state-change ordering symmetric with the
// natural failure path.
// STATICPROP-MATERIAL-ORM-1 — pick the BPTC internalformat. Albedo arrays are
// sRGB; ORM arrays are LINEAR data (occlusion/roughness/metallic are not color)
// so they MUST use the non-sRGB variant or the values would be gamma-decoded.
static GLenum bptcInternalFormatFor(bool isSrgb) {
    return isSrgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                  : GL_COMPRESSED_RGBA_BPTC_UNORM;
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
    // Strip extension: find last dot after the last path separator.
    const size_t sep = s.find_last_of("/\\");
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        s.erase(dot);
    // s is now the path without extension, e.g. "data/tgl/foo"
    // Find the last separator again to split directory from filename stem.
    const size_t lastSep = s.find_last_of("/\\");
    if (lastSep == std::string::npos) {
        // No directory component — just prepend tier and append .ktx2.
        return std::to_string(tier) + "/" + s + ".ktx2";
    }
    // Insert tier directory: "data/tgl" + "/" + "512" + "/" + "foo" + ".ktx2"
    return s.substr(0, lastSep + 1) + std::to_string(tier) + "/"
           + s.substr(lastSep + 1) + ".ktx2";
}

// COMPRESSION-BC7-STATICPROP-2 — release all bucketed arrays + reset the
// bucket bookkeeping vectors. No-op (vectors empty) when the gate is OFF, so
// gate-OFF teardown is byte-identical.
static void staticPropReleaseBuckets() {
    for (GLuint a : s_bucketArrays) {
        if (a) glDeleteTextures(1, &a);
    }
    s_bucketArrays.clear();
    s_bucketInfo.clear();
    s_packetBucketIndex.clear();
    s_slotBucketIndex.clear();
    s_bucketCmdCount.clear();
    // STATICPROP-MATERIAL-ORM-1 — release the per-bucket ORM sibling arrays.
    // Empty (no-op) when the gate is OFF.
    for (GLuint a : s_ormBucketArrays) {
        if (a) glDeleteTextures(1, &a);
    }
    s_ormBucketArrays.clear();
    s_ormLayerHasMap.clear();
}

static void coalesceRollbackTexBuild(std::vector<DWORD>& tempPins) {
    if (s_texArrayOff) { glDeleteTextures(1, &s_texArrayOff); s_texArrayOff = 0; }
    if (s_texArrayOn)  { glDeleteTextures(1, &s_texArrayOn);  s_texArrayOn  = 0; }
    staticPropReleaseBuckets();
    if (mcTextureManager) {
        for (DWORD nodeIdx : tempPins) {
            mcTextureManager->unpinNode(nodeIdx);
        }
    }
    tempPins.clear();
}

// ===========================================================================
// (end substrate-coalesce file-scope state)
// ===========================================================================

void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    std::fprintf(stderr, "[GPUPROPS-DIAG] loadProgramsIfNeeded ENTER\n");
    // Log GL / GLSL version so we know what this driver/context supports.
    const char* glv   = (const char*)glGetString(GL_VERSION);
    const char* glslv = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    std::fprintf(stderr, "[GPUPROPS-DIAG] GL_VERSION=%s\n", glv ? glv : "(null)");
    std::fprintf(stderr, "[GPUPROPS-DIAG] GL_SHADING_LANGUAGE_VERSION=%s\n",
                 glslv ? glslv : "(null)");
    s_programLoadTried = true;

    // Plan v3.8 Step 7.2 — env decision + extension probe MUST land between
    // the latch flip and the legacy makeProgram call. If they ran later
    // (e.g. inside finalizeGeometry's Step 5.3), the latch would already
    // have prevented the coalesce program from compiling and the second
    // makeProgram below would never fire. Pattern verified at
    // gameosmain.cpp:930.
    coalesce_resetEnvOnce();
    s_hasShaderDrawParams = (glewIsSupported("GL_ARB_shader_draw_parameters") != 0);
    // Step 12B.2 — forced-no_extension hook. After the probe but BEFORE any
    // downstream code reads s_hasShaderDrawParams, force the flag false
    // when MC2_COALESCE_FORCE_DISARM=no_extension is set. finalizeGeometry's
    // Step 5.5 then naturally routes through `event=disarmed reason=no_extension`.
    if (s_coalesceForceDisarm == CoalesceForceDisarm::NoExtension) {
        s_hasShaderDrawParams = false;
    }
    std::fprintf(stderr, "[GPUPROPS-DIAG] coalesceEnvDisabled=%d hasShaderDrawParams=%d "
                 "forceDisarm=%d\n",
                 (int)s_coalesceEnvDisabled, (int)s_hasShaderDrawParams,
                 (int)s_coalesceForceDisarm);

    // makeProgram() is the project's shader loader (see gos_postprocess.cpp
    // for existing usage). Pass the "#version 430\n" prefix explicitly — the
    // shader files must NOT contain a #version directive.
    // GLSL 430 required for std430 SSBO. gos_render.cpp now requests a GL
    // 4.3 core context (bumped from 4.0) to match.
    //
    // Plan v3.8 Step 7.1 — two prefixes. Coalesce variant requires
    // GL_ARB_shader_draw_parameters (gl_DrawIDARB / gl_BaseInstanceARB)
    // and gates legacy-only uniforms out via #define MC2_COALESCE 1.
    // M1.5 M2 plan-review fix: both prefixes are runtime std::string
    // builders so MC2_OBJECT_ID_BUFFER can append a #define conditionally.
    // GLSL preprocessor does not inherit C++ build flags
    // (memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
    std::string legacyPrefix = "#version 430\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        legacyPrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }
    // F1-3D flip: MC2_USE_VIEW_UNIFORMS injected by default (kill-switch
    // pattern matching s_viewUniformsDisabled). Disabled only when
    // MC2_VIEW_UNIFORMS=0. Pre-flip F1-3B required explicit =1; that left the
    // shader compiled without the define (PBR/ViewUniforms block compiled out)
    // even though the UBO upload was already default-ON, causing all PBR
    // uniforms to resolve to -1. Gate is process-lifetime: shaders compiled
    // once at startup. Only static_prop.{vert,frag} affected.
    static const bool s_viewUniformsShaderEnabled = []() {
        const char* v = std::getenv("MC2_VIEW_UNIFORMS");
        return !(v != nullptr && v[0] == '0');
    }();
    if (s_viewUniformsShaderEnabled) {
        legacyPrefix += "#define MC2_USE_VIEW_UNIFORMS 1\n";
    }

    std::string coalescePrefix =
        "#version 430\n"
        "#extension GL_ARB_shader_draw_parameters : require\n"
        "#define MC2_COALESCE 1\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        coalescePrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }
    if (s_viewUniformsShaderEnabled) {
        coalescePrefix += "#define MC2_USE_VIEW_UNIFORMS 1\n";
    }
    // STATICPROP-PBR-SLOTS: gate ORM (roughness/metallic) sampling in the
    // fragment shader. Appended to BOTH prefixes; the legacy lane has no
    // MC2_COALESCE / materialTable_, so its compile-guard is false → no effect
    // there (intended). Gate-OFF (s_ormSlotsEnabled == false) is byte-identical.
    if (s_ormSlotsEnabled) {
        legacyPrefix   += "#define MC2_STATICPROP_PBR_SLOTS 1\n";
        coalescePrefix += "#define MC2_STATICPROP_PBR_SLOTS 1\n";
    }

    // Step 7.3 — legacy program (unchanged identity / no rename).
    s_staticPropProgramObj = glsl_program::makeProgram(
        "static_prop",
        "shaders/static_prop.vert",
        "shaders/static_prop.frag",
        legacyPrefix.c_str());
    if (!s_staticPropProgramObj || !s_staticPropProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[GPUPROPS] failed to compile/link static_prop shader pair — "
            "GPU path disabled for this session; all static props will "
            "CPU-fallback via submit()==false\n");
        s_staticPropProgramObj = nullptr;
        s_staticPropProgram    = 0;
        s_programLoadFailed    = true;
        return;
    }
    s_staticPropProgram = s_staticPropProgramObj->shp_;

    // FOLIAGE-STATICPROP-DEPTH-PREPASS-1: the depth-prepass program is built
    // LATER, after the coalesce color program below — it must use the SAME
    // shader variant (legacy vs coalesce prefix) as the program that is
    // actually bound for the static-prop color pass, or GL_EQUAL parity fails
    // (the depth it lays would derive vertex transform / alpha differently).
    // See the "depth-prepass program (variant-matched)" block at the end of
    // this function.

    // Wire GL program name into PipelineRegistry so applyPipeline() can bind it.
    // Both IDs share the same program — alpha distinction is texture-array + shader
    // discard, not a separate program object.
    RenderCore::bindProgram(RenderCore::PipelineId::StaticPropOpaque,    s_staticPropProgram);
    RenderCore::bindProgram(RenderCore::PipelineId::StaticPropAlphaTest, s_staticPropProgram);
    // VERTEXLAYOUT-AUTHORITY-1: record the fixed static-prop 40B VAO identity
    // (gos_static_prop_batcher.cpp:2310-2323) into the pipeline-key path. Both
    // ids share the VAO; layout is invariant across legacy/coalesce/depth
    // variants. Pure metadata; no GL change.
    RenderCore::recordPipelineVertexLayout(RenderCore::PipelineId::StaticPropOpaque,    RenderCore::VertexLayoutId::StaticProp40B);
    RenderCore::recordPipelineVertexLayout(RenderCore::PipelineId::StaticPropAlphaTest, RenderCore::VertexLayoutId::StaticProp40B);
    // Stage 2.D.1.1 (Item 2): cache parity uniform locations once at link time.
    s_loc_u_parityWrite        = glGetUniformLocation(s_staticPropProgram, "u_parityWrite");
    s_loc_u_parityVertsPerType = glGetUniformLocation(s_staticPropProgram, "u_parityVertsPerType");
    s_loc_u_parityBaseVertex   = glGetUniformLocation(s_staticPropProgram, "u_parityBaseVertex");
    std::fprintf(stderr, "[GPUPROPS-DIAG] loadProgramsIfNeeded OK prog=%u "
                 "loc_parityWrite=%d loc_parityVertsPerType=%d loc_parityBaseVertex=%d\n",
                 s_staticPropProgram,
                 s_loc_u_parityWrite, s_loc_u_parityVertsPerType,
                 s_loc_u_parityBaseVertex);

    // Plan v3.8 Step 7.4 — populate s_locsLegacy. GLSL string literals are
    // mixed-prefix (terrainMVP has NO u_ prefix; the rest do); reproduce
    // them exactly — verified against existing flush() upload sites
    // (terrainMVP at static_prop.vert:69; u_mvp,
    // u_fogValue, u_debugAddrMode, u_maxLocalVertexID, u_materialFlags,
    // u_packetID at the corresponding upload sites in flush()).
    // The existing flush() upload sites still call glGetUniformLocation
    // inline; this cache is a forward-compat addition for the coalesce
    // branch's per-frame upload path. Step 7's "Legacy path keeps working"
    // guardrail: do NOT redirect the existing legacy uploads to read from
    // s_locsLegacy here — that's a follow-up cleanup, not a Step 7 edit.
    s_locsLegacy.terrainMVP        = glGetUniformLocation(s_staticPropProgram, "u_worldToClipGL");
    s_locsLegacy.mvp               = glGetUniformLocation(s_staticPropProgram, "u_mvp");
    s_locsLegacy.fogValue          = glGetUniformLocation(s_staticPropProgram, "u_fogValue");
    s_locsLegacy.debugAddrMode     = glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode");
    s_locsLegacy.maxLocalVertexID  = glGetUniformLocation(s_staticPropProgram, "u_maxLocalVertexID");
    s_locsLegacy.materialFlags     = glGetUniformLocation(s_staticPropProgram, "u_materialFlags");
    s_locsLegacy.packetID          = glGetUniformLocation(s_staticPropProgram, "u_packetID");
    // V-AMBIENT-STATIC-1: hemisphere ambient strength uniform (default 0.0 = OFF).
    s_locsLegacy.ambientV1Strength = glGetUniformLocation(s_staticPropProgram, "u_ambientV1Strength");
    // V-MATERIAL-DEBUG-1: per-fragment material debug view mode (default 0 = OFF).
    s_locsLegacy.debugMaterialMode = glGetUniformLocation(s_staticPropProgram, "u_debugMaterialMode");
    s_locsLegacy.pathTint          = glGetUniformLocation(s_staticPropProgram, "u_pathTint");
    // GREYBEARD-DIFFUSE-TEST: terrain sun (raw MC2) for forced-diffuse probe.
    s_locsLegacy.terrainSunMC2     = glGetUniformLocation(s_staticPropProgram, "u_terrainSunMC2");
    // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor (flag-gated in frag).
    s_locsLegacy.terrainDecalFill  = glGetUniformLocation(s_staticPropProgram, "u_terrainDecalFill");
    // TERRAIN-DECAL-COLORBLEND-1: colormap sampler + atlas UV params + blend (flag-gated in frag).
    s_locsLegacy.terrainColormap        = glGetUniformLocation(s_staticPropProgram, "u_terrainColormap");
    s_locsLegacy.decalAtlasTLX          = glGetUniformLocation(s_staticPropProgram, "u_decalAtlasTLX");
    s_locsLegacy.decalAtlasTLY          = glGetUniformLocation(s_staticPropProgram, "u_decalAtlasTLY");
    s_locsLegacy.decalAtlasOOW          = glGetUniformLocation(s_staticPropProgram, "u_decalAtlasOOW");
    s_locsLegacy.terrainDecalColorBlend = glGetUniformLocation(s_staticPropProgram, "u_terrainDecalColorBlend");
    // V-IBL-STATIC-1: SH-L2 coeffs + strength (default strength 0.0 = OFF).
    s_locsLegacy.iblSh             = glGetUniformLocation(s_staticPropProgram, "u_iblSh");
    s_locsLegacy.iblShStrength     = glGetUniformLocation(s_staticPropProgram, "u_iblShStrength");
    // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular (default strength 0.0 = OFF).
    s_locsLegacy.pbrV1Strength     = glGetUniformLocation(s_staticPropProgram, "u_pbrV1Strength");
    s_locsLegacy.pbrV1RoughnessOverride = glGetUniformLocation(s_staticPropProgram, "u_pbrV1RoughnessOverride");
    s_locsLegacy.pbrV1DiagSunFound = glGetUniformLocation(s_staticPropProgram, "u_pbrV1DiagSunFound");
    // s_locsLegacy.drawIDBase / texArr stay -1 (coalesce-only; legacy
    // shader has no such uniforms).

    // Plan v3.8 Step 7.5 — second program, gated on extension + env. Link
    // failure of the coalesce program does NOT poison the legacy path:
    // s_staticPropProgramCoalesce stays 0; IsCoalesceEnabled() catches
    // it via the (s_staticPropProgramCoalesce == 0) check.
    if (s_hasShaderDrawParams && !s_coalesceEnvDisabled) {
        // Distinct program name — shader_builder.cpp:611-614 cache rejects
        // duplicate names across calls.
        glsl_program* coalesceObj = glsl_program::makeProgram(
            "static_prop_coalesce",
            "shaders/static_prop.vert",
            "shaders/static_prop.frag",
            coalescePrefix.c_str());
        if (coalesceObj && coalesceObj->is_valid()) {
            s_staticPropProgramCoalesce = coalesceObj->shp_;

            // Step 7.6 — populate s_locsCoalesce. Legacy-only uniforms
            // (u_materialFlags / u_maxLocalVertexID / u_packetID) are
            // removed by the MC2_COALESCE preprocessor branch in
            // static_prop.frag (Step 8.5); glGetUniformLocation returns
            // -1 for them, ProgramLocs default-init keeps them -1, and
            // Step 11.7.d's upload helper skips -1 locations.
            s_locsCoalesce.terrainMVP        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_worldToClipGL");
            s_locsCoalesce.mvp               = glGetUniformLocation(s_staticPropProgramCoalesce, "u_mvp");
            s_locsCoalesce.fogValue          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_fogValue");
            s_locsCoalesce.debugAddrMode     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_debugAddrMode");
            s_locsCoalesce.drawIDBase        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_drawIDBase");
            s_locsCoalesce.texArr            = glGetUniformLocation(s_staticPropProgramCoalesce, "u_texArr");
            s_locsCoalesce.materialGpuSample = glGetUniformLocation(s_staticPropProgramCoalesce, "u_materialGpuSample");
            // V-AMBIENT-STATIC-1: hemisphere ambient strength uniform (default 0.0 = OFF).
            s_locsCoalesce.ambientV1Strength = glGetUniformLocation(s_staticPropProgramCoalesce, "u_ambientV1Strength");
            // V-MATERIAL-DEBUG-1: per-fragment material debug view mode (default 0 = OFF).
            s_locsCoalesce.debugMaterialMode = glGetUniformLocation(s_staticPropProgramCoalesce, "u_debugMaterialMode");
            s_locsCoalesce.pathTint          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_pathTint");
            // GREYBEARD-DIFFUSE-TEST: terrain sun (raw MC2) for forced-diffuse probe.
            s_locsCoalesce.terrainSunMC2     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainSunMC2");
            // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor (flag-gated in frag).
            s_locsCoalesce.terrainDecalFill  = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainDecalFill");
            // TERRAIN-DECAL-COLORBLEND-1: colormap sampler + atlas UV params + blend (flag-gated in frag).
            s_locsCoalesce.terrainColormap        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainColormap");
            s_locsCoalesce.decalAtlasTLX          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_decalAtlasTLX");
            s_locsCoalesce.decalAtlasTLY          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_decalAtlasTLY");
            s_locsCoalesce.decalAtlasOOW          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_decalAtlasOOW");
            s_locsCoalesce.terrainDecalColorBlend = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainDecalColorBlend");
            // V-IBL-STATIC-1: SH-L2 coeffs + strength (default strength 0.0 = OFF).
            s_locsCoalesce.iblSh             = glGetUniformLocation(s_staticPropProgramCoalesce, "u_iblSh");
            s_locsCoalesce.iblShStrength     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_iblShStrength");
            // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular (default strength 0.0 = OFF).
            s_locsCoalesce.pbrV1Strength     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_pbrV1Strength");
            s_locsCoalesce.pbrV1RoughnessOverride = glGetUniformLocation(s_staticPropProgramCoalesce, "u_pbrV1RoughnessOverride");
            s_locsCoalesce.pbrV1DiagSunFound = glGetUniformLocation(s_staticPropProgramCoalesce, "u_pbrV1DiagSunFound");
            // STATICPROP-MATERIAL-ORM-1: ORM sampler + sample-enable. The shader
            // does not declare these yet (separate later task) so both resolve to
            // -1 here; the ProgramLocs default-init keeps them -1 and every upload
            // site below is `if (loc >= 0)`-guarded → no-op until the shader lands.
            s_locsCoalesce.ormTexArr       = glGetUniformLocation(s_staticPropProgramCoalesce, "u_ormTexArr");
            s_locsCoalesce.ormSampleEnable = glGetUniformLocation(s_staticPropProgramCoalesce, "u_ormSampleEnable");

            // M3 fix: if both gates are ON and the uniform is absent, log an error.
            // This can only happen if the shader wasn't recompiled with v3 changes.
            // The if(loc >= 0) guard in flush() already handles -1 safely (no-op),
            // but the error makes the failure observable without a debugger.
            if (s_materialGpuEnabled && s_materialGpuSampleEnabled &&
                s_locsCoalesce.materialGpuSample < 0) {
                std::fputs("[MATERIAL_GPU v4] ERROR uniform_missing name=u_materialGpuSample\n",
                           stderr);
            }

            // s_locsCoalesce.materialFlags / maxLocalVertexID / packetID
            // stay -1 (legacy-only; removed under MC2_COALESCE).

            // Step 7.6 (out-MAJ-5 / spec §6 sampler hygiene) — bind
            // sampler2DArray uniform u_texArr to texture unit 0 once
            // here so the per-frame draw branch only needs to bind the
            // texture handle, not re-issue glUniform1i.
            if (s_locsCoalesce.texArr >= 0) {
                // INIT-ONLY: bind sampler uniform once here so per-frame flush()
                // only needs to bind the texture handle. Not a flush-path state switch.
                glUseProgram(s_staticPropProgramCoalesce);
                glUniform1i(s_locsCoalesce.texArr, 0);
                glUseProgram(0);
            }
            // STATICPROP-MATERIAL-ORM-1: bind the ORM sampler uniform to kOrmTexUnit
            // once (loc-guarded; -1 until the shader declares u_ormTexArr).
            if (s_locsCoalesce.ormTexArr >= 0) {
                glUseProgram(s_staticPropProgramCoalesce);
                glUniform1i(s_locsCoalesce.ormTexArr, static_cast<GLint>(kOrmTexUnit));
                glUseProgram(0);
            }
            std::fprintf(stderr, "[GPUPROPS-DIAG] static_prop_coalesce program=%u "
                         "loc_drawIDBase=%d loc_texArr=%d\n",
                         s_staticPropProgramCoalesce,
                         s_locsCoalesce.drawIDBase, s_locsCoalesce.texArr);

            // v1: coalesce path is active — overwrite PipelineRegistry with
            // the coalesce program so applyPipeline() binds the right handle.
            // Both IDs share the same coalesce program object (alpha distinction
            // is texture-array + shader discard, not a separate program).
            RenderCore::bindProgram(RenderCore::PipelineId::StaticPropOpaque,    s_staticPropProgramCoalesce);
            RenderCore::bindProgram(RenderCore::PipelineId::StaticPropAlphaTest, s_staticPropProgramCoalesce);
            // Re-cache parity uniform locations from the coalesce program.
            // s_loc_u_parity* were initially cached from s_staticPropProgram, but
            // applyPipeline() now binds s_staticPropProgramCoalesce — the locations
            // are program-specific, so calling glUniform1i with a location from the
            // wrong program yields GL_INVALID_OPERATION (0x502).
            s_loc_u_parityWrite        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_parityWrite");
            s_loc_u_parityVertsPerType = glGetUniformLocation(s_staticPropProgramCoalesce, "u_parityVertsPerType");
            s_loc_u_parityBaseVertex   = glGetUniformLocation(s_staticPropProgramCoalesce, "u_parityBaseVertex");
        } else {
            // Compile/link failure — leave handle zero. Legacy path is
            // unaffected; finalizeGeometry's Step 5.5 will see
            // !s_hasShaderDrawParams resolve via IsCoalesceEnabled()
            // instead, but the simpler check here is that
            // s_staticPropProgramCoalesce stays 0 and IsCoalesceEnabled
            // catches it on every frame.
            std::fprintf(stderr, "[GPUPROPS] failed to compile/link "
                         "static_prop_coalesce — coalesce path disabled "
                         "for this session; legacy path unaffected\n");
            s_staticPropProgramCoalesce = 0;
        }
    }

    // FOLIAGE-STATICPROP-DEPTH-PREPASS-1: depth-prepass program (variant-matched).
    // Built AFTER the coalesce color program so it uses the SAME shader variant
    // (and thus the same vertex transform + alpha-flag derivation) as the program
    // that applyPipeline() will actually bind for the static-prop COLOR pass —
    // the load-bearing precondition for the later GL_EQUAL depth test.
    //
    // The active color program is the coalesce one (s_staticPropProgramCoalesce)
    // exactly when it linked above (lines just before this rebound StaticPropOpaque/
    // AlphaTest to it). In that case use coalescePrefix (with MC2_COALESCE); the
    // depth frag's #ifdef MC2_COALESCE branch then derives materialFlags/tex_color.a
    // via the PerDrawEntry SSBO + u_texArr + u_drawIDBase exactly like the color
    // frag. Otherwise the legacy program is active, so use legacyPrefix (no
    // MC2_COALESCE; u_materialFlags + u_tex path). Same MC2_OBJECT_ID_BUFFER /
    // MC2_USE_VIEW_UNIFORMS / MC2_STATICPROP_PBR_SLOTS defines are already baked
    // into both prefixes above, so this matches the color program byte-for-byte.
    //
    // This is a DISTINCT program object, so we capture its OWN u_drawIDBase
    // location (uniform locations are per-program). Link failure disables the
    // prepass only; the color path is untouched (s_staticPropDepthProgram stays 0
    // → flushDepthPrepass() no-ops).
    {
        const bool depthUsesCoalesce = (s_staticPropProgramCoalesce != 0);
        const char* depthPrefix =
            depthUsesCoalesce ? coalescePrefix.c_str() : legacyPrefix.c_str();
        glsl_program* depthObj = glsl_program::makeProgram(
            "static_prop_depth",
            "shaders/static_prop.vert",
            "shaders/static_prop_depth.frag",
            depthPrefix);
        if (depthObj && depthObj->is_valid()) {
            s_staticPropDepthProgram = depthObj->shp_;
            RenderCore::bindProgram(RenderCore::PipelineId::StaticPropDepth,
                                    s_staticPropDepthProgram);
            // VERTEXLAYOUT-AUTHORITY-1: depth-prepass reuses static_prop.vert →
            // same 40B VAO. Record it into the pipeline-key path.
            RenderCore::recordPipelineVertexLayout(RenderCore::PipelineId::StaticPropDepth,
                                                   RenderCore::VertexLayoutId::StaticProp40B);
            // u_drawIDBase exists ONLY in the coalesce variant; legacy returns -1
            // (and the prepass never uploads it on the legacy path).
            s_locsDepthCoalesce.drawIDBase =
                glGetUniformLocation(s_staticPropDepthProgram, "u_drawIDBase");
            // GL_EQUAL-parity (review CRITICAL-1/IMPORTANT-1): capture the depth
            // program's OWN u_materialGpuSample + u_debugAddrMode so the prepass
            // can upload the SAME values the color pass uploads to its program.
            // u_materialGpuSample is coalesce-only (legacy returns -1, harmless);
            // u_debugAddrMode exists in both variants. Per-program locations.
            s_locsDepthCoalesce.materialGpuSample =
                glGetUniformLocation(s_staticPropDepthProgram, "u_materialGpuSample");
            s_locsDepthCoalesce.debugAddrMode =
                glGetUniformLocation(s_staticPropDepthProgram, "u_debugAddrMode");
            // The coalesce frag samples u_texArr via a sampler2DArray uniform; bind
            // it to texture unit 0 once (matches the color program's init-only bind
            // at the coalesce block above) so the per-draw prepass loop only binds
            // the texture handle, not re-issue glUniform1i.
            if (depthUsesCoalesce) {
                const GLint depthTexArrLoc =
                    glGetUniformLocation(s_staticPropDepthProgram, "u_texArr");
                if (depthTexArrLoc >= 0) {
                    glUseProgram(s_staticPropDepthProgram);
                    glUniform1i(depthTexArrLoc, 0);
                    glUseProgram(0);
                }
            }
            std::fprintf(stderr, "[GPUPROPS-DIAG] static_prop_depth program=%u "
                         "variant=%s loc_drawIDBase=%d loc_materialGpuSample=%d "
                         "loc_debugAddrMode=%d\n",
                         s_staticPropDepthProgram,
                         depthUsesCoalesce ? "coalesce" : "legacy",
                         s_locsDepthCoalesce.drawIDBase,
                         s_locsDepthCoalesce.materialGpuSample,
                         s_locsDepthCoalesce.debugAddrMode);
        } else {
            std::fprintf(stderr,
                "[GPUPROPS] static_prop_depth program failed to compile/link — "
                "depth-prepass disabled this session (color path unaffected)\n");
            s_staticPropDepthProgram = 0;   // flushDepthPrepass() must no-op when 0
        }
    }
}

// --- Step 7.7: strengthened IsCoalesceEnabled() body (plan v3.8). ---
// Returns true only when EVERY precondition for the coalesce draw branch
// is satisfied. False on any precondition lets flush()'s gate fall back
// to the legacy per-type/per-packet loop with no other state change.
// Cheap (all field reads + a few comparisons); safe to call per-frame.
static bool IsCoalesceEnabled() {
    coalesce_resetEnvOnce();  // idempotent — primary call is in loadProgramsIfNeeded
    if (s_coalesceEnvDisabled)              return false;
    if (!s_hasShaderDrawParams)             return false;
    if (!s_geometryFinalized)               return false;
    if (!s_coalesceLayoutReady)             return false;
    if (!s_coalesceEnabled)                 return false;
    if (!s_coalesceArmed)                   return false;
    if (s_staticPropProgramCoalesce == 0)   return false;
    if (s_coalesceInstanceSsbo == 0)        return false;
    if (s_perDrawSsbo == 0)                 return false;
    if (s_permutationSsbo == 0)             return false;
    if (s_alphaOffCount == 0 && s_alphaOnCount == 0) return false;
    if (gos_object_parity::IsParityCheckEnabled())   return false;
    return true;
}

// Layer B fallback: types we failed to register (logged once, fall back to CPU path).
std::unordered_map<const TG_TypeShape*, bool> s_failedTypes;

void ensureRingCapacity(size_t neededInstances, size_t neededColorEntries) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededColorEntries > s_colorCapacity;
    if (!needGrow) return;

    // Wait for all in-flight frames before resizing.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_fence[i]) {
            MC2_GL_ClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_colorSsbo)    { glDeleteBuffers(1, &s_colorSsbo);    s_colorSsbo    = 0; s_colorMap    = nullptr; }

    s_instanceCapacity = std::max(neededInstances,
        s_instanceCapacity ? s_instanceCapacity * 2 : INITIAL_INSTANCES_PER_FRAME);
    s_colorCapacity    = std::max(neededColorEntries,
        s_colorCapacity    ? s_colorCapacity    * 2 : INITIAL_COLORS_PER_FRAME);

    // SSBO-BIND-ALIGN: the legacy (non-coalesce) per-type draw binds at
    // slotInstByteBase = s_frameSlot * s_instanceCapacity * sizeof (and the color
    // base likewise). Those offsets must be multiples of
    // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT or ring slots 1+ bind misaligned on
    // NVIDIA -> GL_INVALID_VALUE -> SSBO unbound -> props invisible. This is NOT
    // dormant: the coalesce path disarms on NVIDIA (the guard trips because
    // sizeof(GpuStaticPropInstance)=112 is not alignment-aligned), so the LEGACY
    // path is the active one there. Round capacity up to an alignment-multiple
    // (elements) so cap*sizeof is aligned; the alloc and the slot base both derive
    // from capacity, so this aligns every slot consistently.
    {
        const unsigned long long a = (unsigned long long)gpuSsboOffsetAlignment();
        s_instanceCapacity = (size_t)gpuAlignUp((unsigned long long)s_instanceCapacity, a);
        s_colorCapacity    = (size_t)gpuAlignUp((unsigned long long)s_colorCapacity,    a);
    }

    const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield mapFlags     = storageFlags;

    // TIER2-EXCLUDED: ring
    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance)),
                    nullptr, storageFlags);
    s_instanceMap = MC2_GL_MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance)),
                    mapFlags);

    // TIER2-EXCLUDED: ring
    glGenBuffers(1, &s_colorSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_colorSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t)),
                    nullptr, storageFlags);
    s_colorMap = MC2_GL_MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t)),
                    mapFlags);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_colorMap) {
        std::fprintf(stderr, "[GPUPROPS] persistent map failed; disabling GPU path\n");
        s_fatalRegistrationFailure = true;
    }
}

// ---------------------------------------------------------------------------
// Counter state added in Task 6 (Stage 1.D — Gate F + late-registration).
// ---------------------------------------------------------------------------

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
};
ObjBatcherCounters s_counters;
bool s_objbatcherTrace     = false;
bool s_objbatcherTraceInit = false;
bool s_atexitRegistered    = false;

// [SPOTLIGHT_REAL_TRACE v1] (E) Stage 0 / T0.1 baseline. Counts submitMultiShape
// events whose child has isSpotlight==true. Separate state (not in
// ObjBatcherCounters) so the schema stays self-contained and easy to retire.
// First-hit emits one-line stderr always-on so an operator without the env still
// sees confirmation that SpotLight_ children are observed at all.
uint64_t s_spotlightReal_window  = 0;  // resets every 600 frames
uint64_t s_spotlightReal_mono    = 0;  // monotonic since process start
bool     s_spotlightReal_firstHit = false;

// Late-registration aggregate per-type accounting + allowlist.
std::unordered_map<std::string, uint32_t> s_lateRegisterCounts;
std::unordered_set<std::string> s_lateRegisterAllowlist;
bool s_lateRegisterAllowlistLoaded = false;

// Slice 2, Stage 2.A — late-registration recovery signal.
// Set true inside the late-reg branch of submitMultiShape; cleared to false
// at the top of every submitMultiShape call. Read by the public method
// wasLastFailureLateRegistration() so callers (Stage 2.B) can set
// needsFullBakeNextFrame on the owning actor without adding a return-value
// channel to submitMultiShape.
bool s_lastSubmitWasLateReg = false;

// Monotonic counter of times submitMultiShape returned false on the
// late-registration branch (across all frames since process start).
// Added to the [OBJBATCHER v1] summary line.
uint64_t s_late_register_recovery_skips = 0;

// Slice 2 (Stage 2.D.3) — "currently-submitting multishape" pointer used to
// gate per-leaf snapshot capture in submit() to only the sampled actor.
//
// Set at the top of submitMultiShape (after the registered-types check), used
// inside submit() to compare against the sampler's current pick, and reset to
// nullptr at the bottom of submitMultiShape so a stray submit() call from a
// non-multi path can't leak gating state. In SampledOnly arm mode, only
// submit() calls whose enclosing multishape == s_currentSampledShape will
// record an instance snapshot — the GPU parity SSBO still receives writes
// from every type's draw (those bytes are ignored on compare because the
// snapshot map only carries entries for the sampled actor's types).
const TG_MultiShape* s_currentSubmittingMulti = nullptr;

// 2026-05-10 actor-center fix: parent multishape's world position captured
// at the top of submitMultiShape and read by submit() when populating the
// substrate-record worldCenter. Every leaf of one multishape shares the same
// cull-side center so the GPU frustum sphere-test accepts/rejects all leaves
// together (matches the original one-record-per-actor model). Reset to zero
// at every entry to submitMultiShape.
//
// Coordinate convention: stored in raw MC2 world coords already unswapped
// from Stuff/MLR (.x=east, .y=north, .z=elev), so the consumer in submit()
// can copy directly into rec.worldCenter without re-unswapping.
float s_currentSubmittingActorCenter[3] = {0.0f, 0.0f, 0.0f};
bool  s_currentSubmittingActorCenterValid = false;

// Slice 2 (Stage 2.D.3) — late-registration EVENT counters split by
// allowlist disposition. Incremented in the late-reg branch of
// submitMultiShape() per occurrence (NOT once-per-type — the dedup
// printf logic is unchanged, but every event ticks one of these so the
// parity 600-frame summary surfaces actual frequency).
//
//   s_lateReg_allowed_events:    nodeId IS in objbatcher_late_register_allowlist.txt
//   s_lateReg_disallowed_events: nodeId is NOT (or empty)
//
// Read by GpuStaticPropBatcher::getAllowedLateRegEventCount() and
// getDisallowedLateRegEventCount(); consumed by gos_object_parity::ParityFrameTick.
uint64_t s_lateReg_allowed_events    = 0;
uint64_t s_lateReg_disallowed_events = 0;

// 2026-05-10 diag: per-typeID submit() histogram. MC2_SUBMIT_TYPEHIST=1 to
// enable. Answers H1 from
// docs/superpowers/plans/2026-05-10-substrate-coalesce-detail-and-perf-followup.md:
// "do detail-leaf typeIDs (Litwin_*, Door*, roof tiles) actually reach the
// per-leaf submit() under coalesce, or does the eligibility filter at
// submitMultiShape:2212-2231 strip them first?". Cross-reference the dump's
// non-zero typeIDs against the typeID→name table emitted by [STATIC_PROP_REG]
// at mission_load_byarr. Mirrors the [REGFLUSH_TYPEHIST v1] pattern.
bool                       s_submitTypeHistEnabled = false;
bool                       s_submitTypeHistInit    = false;
std::array<uint64_t, 1024> s_submitTypeHist{};
uint64_t                   s_submitTypeHistCalls   = 0;

void emitSubmitTypeHistDump(const char* trigger) {
    if (!s_submitTypeHistEnabled) return;
    // Build a typeID → name lookup by walking s_typeIndex once. n is ≤ a few
    // hundred types; quadratic walk is trivial for a 3-dump cadence.
    std::fprintf(stderr,
        "[SUBMIT_TYPEHIST v1] trigger=%s calls=%llu non-zero buckets:\n",
        trigger, (unsigned long long)s_submitTypeHistCalls);
    for (size_t t = 0; t < s_submitTypeHist.size(); ++t) {
        if (s_submitTypeHist[t] > 0) {
            const char* name = "<unknown>";
            for (const auto& kv : s_typeIndex) {
                if (kv.second == t && kv.first) {
                    // const_cast: getNodeId is non-const in tgl.h but returns
                    // a pointer to a char[TG_NODE_ID] field — we only read it.
                    name = const_cast<TG_TypeShape*>(kv.first)->getNodeId();
                    break;
                }
            }
            // alphaClass tells us which coalesce group this type lives in.
            // packets / indices: if a type has 0 packets or 0 indices, its
            // glMultiDrawElementsIndirect command has cmd.count=0 → no
            // triangles drawn even when instanceCount>0 (H3 of plan 2026-05-10).
            uint32_t aClass = 0xFFu, packets = 0u, indices = 0u, vertCount = 0u;
            if (t < s_types.size()) {
                const auto& ty = s_types[t];
                aClass    = ty.alphaClass;
                packets   = ty.packetCount;
                vertCount = ty.vertexCount;
                for (uint32_t p = 0; p < ty.packetCount; ++p) {
                    indices += s_packets[ty.firstPacket + p].indexCount;
                }
            }
            std::fprintf(stderr,
                "[SUBMIT_TYPEHIST v1] typeID=%zu name=%s count=%llu alphaClass=%u packets=%u verts=%u indices=%u\n",
                t, (name && *name) ? name : "<empty>",
                (unsigned long long)s_submitTypeHist[t],
                aClass, packets, vertCount, indices);
        }
    }
    std::fflush(stderr);
}

void emitSubmitTypeHistAtExit() { emitSubmitTypeHistDump("atexit"); }

inline int popIndex(GpuStaticPropPopulation pop) {
    return static_cast<int>(pop);
}

// Forward declaration; defined below.
void accumulateMonotonicAndMaybeEmit(bool forceEmit);

void emitFinalSummaryAtExit() {
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/true);
}

inline void initTraceOnce() {
    if (!s_objbatcherTraceInit) {
        s_objbatcherTrace     = (getenv("MC2_OBJBATCHER_TRACE") != nullptr);
        s_objbatcherTraceInit = true;
    }
    if (!s_atexitRegistered) {
        s_atexitRegistered = true;
        atexit(emitFinalSummaryAtExit);
    }
}

void accumulateMonotonicAndMaybeEmit(bool forceEmit) {
    s_counters.frame_count++;
    for (int p = 0; p < kPopCount; ++p) {
        s_counters.mono_eligible_actors_by_pop[p]     += s_counters.eligible_actors_by_pop[p];
        s_counters.mono_submitted_instances_by_pop[p] += s_counters.submitted_instances_by_pop[p];
        s_counters.mono_cpu_fallback_by_pop[p]        += s_counters.cpu_fallback_by_pop[p];
    }
    s_counters.mono_submitted_children  += s_counters.submitted_children;
    s_counters.mono_skipped_children    += s_counters.skipped_children;
    s_counters.mono_gpu_drawn_instances += s_counters.gpu_drawn_instances;

    const bool periodic = (s_counters.frame_count % 600 == 0
                           && s_counters.frame_count > 0);
    if (s_objbatcherTrace || periodic || forceEmit) {
        // Slice-1 fallback rate uses ONLY Building+Tree+Generic
        // populations (Legacy excluded — it's the prior killswitch path).
        uint64_t slice1_eligible = 0, slice1_fallback = 0;
        for (int p = 0; p <= 2; ++p) {  // Building, Tree, Generic
            slice1_eligible += s_counters.mono_eligible_actors_by_pop[p];
            slice1_fallback += s_counters.mono_cpu_fallback_by_pop[p];
        }
        const double fb_rate = (slice1_eligible > 0)
            ? (double)slice1_fallback / (double)slice1_eligible
            : 0.0;

        uint64_t total_submitted = 0;
        for (int p = 0; p < kPopCount; ++p) {
            total_submitted += s_counters.mono_submitted_instances_by_pop[p];
        }

        std::fprintf(stderr,
               "[OBJBATCHER v1] event=summary frames=%llu "
               "eligible_actors=%llu submitted_instances=%llu "
               "submitted_children=%llu skipped_children=%llu "
               "cpu_fallback=%llu gpu_drawn_instances=%llu "
               // STALE-COUNTER-RETIRE-1: gpu_drawn_instances is incremented ONLY on the
               // legacy/per-type path (line ~7011, CPU-side TypeRangeSsbo.instanceCount).
               // Under the live GPU-authority coalesce-v6 / C1b path the instance counts
               // are GPU-authored into the indirect buffer and never read back, so this
               // reads 0 there. submitted_instances is the honest live-path signal.
               "gpu_drawn_note=legacy_path_only_v6_uses_submitted "
               "fallback_rate=%.4f "
               "submit_buildings=%llu submit_trees=%llu "
               "submit_generics=%llu submit_legacy=%llu "
               "late_register_recovery_skips=%llu\n",
               (unsigned long long)s_counters.frame_count,
               (unsigned long long)slice1_eligible,
               (unsigned long long)total_submitted,
               (unsigned long long)s_counters.mono_submitted_children,
               (unsigned long long)s_counters.mono_skipped_children,
               (unsigned long long)slice1_fallback,
               (unsigned long long)s_counters.mono_gpu_drawn_instances,
               fb_rate,
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[0],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[1],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[2],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[3],
               (unsigned long long)s_late_register_recovery_skips);
        std::fflush(stderr);
    }

    // [SPOTLIGHT_REAL_TRACE v1] summary line, env-gated. Post-T3.2 the
    // increment site is gone (spotlight children are unconditionally skipped
    // at submit, the bit-2 flag is no longer emitted). Window/mono counters
    // therefore print 0 — that 0 IS the substitutive evidence (criterion #2:
    // zero static-prop draws contain spotlight-tagged packets). Demoted, not
    // deleted, per the Debug Instrumentation Rule.
    if (s_spotlightRealTrace && periodic) {
        std::fprintf(stderr,
               "[SPOTLIGHT_REAL_TRACE v1] event=summary site=static_prop_batcher "
               "frames=%llu window_spotlight_submits=%llu mono_spotlight_submits=%llu\n",
               (unsigned long long)s_counters.frame_count,
               (unsigned long long)s_spotlightReal_window,
               (unsigned long long)s_spotlightReal_mono);
        std::fflush(stderr);
    }
    if (periodic) {
        s_spotlightReal_window = 0;
    }

    // Reset per-frame counters for next frame.
    for (int p = 0; p < kPopCount; ++p) {
        s_counters.eligible_actors_by_pop[p]     = 0;
        s_counters.submitted_instances_by_pop[p] = 0;
        s_counters.cpu_fallback_by_pop[p]        = 0;
    }
    s_counters.submitted_children  = 0;
    s_counters.skipped_children    = 0;
    s_counters.gpu_drawn_instances = 0;
}

void loadLateRegisterAllowlistOnce() {
    if (s_lateRegisterAllowlistLoaded) return;
    s_lateRegisterAllowlistLoaded = true;
    FILE* f = fopen("data/objbatcher_late_register_allowlist.txt", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip the # comment delimiter and the trailing newline.
        char* p = line;
        while (*p && *p != '\n' && *p != '\r' && *p != '#') ++p;
        *p = '\0';
        // Trim leading whitespace.
        char* start = line;
        while (*start == ' ' || *start == '\t') ++start;
        // Trim trailing whitespace.
        char* end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        *end = '\0';
        if (*start == '\0') continue;
        s_lateRegisterAllowlist.insert(start);
    }
    fclose(f);
}

} // namespace

// ---------------------------------------------------------------------------
// DEBUG-VIEW-REGISTRY-1: static-prop shader mode <-> RenderDebugView mapping.
// Defined outside the anonymous namespace so they have external linkage and
// can be called from gui_runtime (EditorInspector) via the header declarations.
// Shader branch numbers MUST NOT change (would cause shader_reflect drift).
// 0=Final, 1=Albedo, 2=MaterialIdx, 3=Normal, 4=TexArrayLayer, 5=Roughness, 6=Metallic
// ---------------------------------------------------------------------------
int StaticPropViewToShaderMode(RenderDebugView v) {
    switch (v) {
        case RenderDebugView::Final:          return 0;
        case RenderDebugView::Albedo:         return 1;
        case RenderDebugView::MaterialIdx:    return 2;
        case RenderDebugView::Normal:         return 3;
        case RenderDebugView::TexArrayLayer:  return 4;
        case RenderDebugView::Roughness:      return 5;
        case RenderDebugView::Metallic:       return 6;
        default:                              return 0;  // unsupported -> Final
    }
}

RenderDebugView StaticPropShaderModeToView(int m) {
    switch (m) {
        case 0: return RenderDebugView::Final;
        case 1: return RenderDebugView::Albedo;
        case 2: return RenderDebugView::MaterialIdx;
        case 3: return RenderDebugView::Normal;
        case 4: return RenderDebugView::TexArrayLayer;
        case 5: return RenderDebugView::Roughness;
        case 6: return RenderDebugView::Metallic;
        default: return RenderDebugView::Final;
    }
}

void batcher_setDebugMaterialMode(int shaderMode) {
    if (shaderMode < 0) shaderMode = 0;
    if (shaderMode > 6) shaderMode = 6;
    s_staticPropDebugMaterialMode = shaderMode;
}

int batcher_getDebugMaterialMode() {
    return s_staticPropDebugMaterialMode;
}

GpuStaticPropBatcher& GpuStaticPropBatcher::instance() {
    static GpuStaticPropBatcher s;
    return s;
}

void GpuStaticPropBatcher::onMapLoad() {
    // Reset everything; called at every map boundary.
    s_packets.clear();
    s_types.clear();
    s_perTypePeak.clear();
    s_coalesceFrameSlot = 0;  // reset coalesce ring slot per-mission for hygiene
    s_typeIndex.clear();
    s_typeDescTable.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_failedTypes.clear();
    s_geometryFinalized = false;
    s_fatalRegistrationFailure = false;

    // Substrate-coalesce per-mission resets (plan v3.8 Step 3.1). Cleared
    // here so finalizeGeometry() can repopulate fresh; do NOT clear
    // s_coalesceEnvDisabled / s_hasShaderDrawParams (process-lifetime
    // values resolved once per process via coalesce_resetEnvOnce / the
    // GLEW probe inside loadProgramsIfNeeded). Do NOT clear
    // s_coalescePinnedNodes — that vector is owned by onMapUnload's
    // refcount-aware unpin loop (Step 4.1).
    s_sortedTypeOrder.clear();
    s_alphaOffCount = 0;
    s_alphaOnCount  = 0;
    s_offGroupTotalBytes = 0;
    s_onGroupTotalBytes  = 0;
    s_coalescePerFrameInstanceBytes = 0;
    s_coalesceLayoutReady    = false;
    s_coalesceEnabled        = false;
    s_coalesceArmed          = false;
    s_coalesceFirstFlushDone = false;

    // Stage 2.D.2: re-arm the dual-emit latch for this mission so the first
    // eligible frame after map load triggers the compare.
    gos_object_parity::OnMissionLoad();

    // V-IBL-STATIC-2: re-default to baseline set at every map boundary so a
    // mission that calls setMissionForIbl(nullptr) (or never calls it -- e.g.
    // editor / savegame paths) lands on "default" regardless of what the
    // previous mission resolved to.
    s_currentShSet = &RenderCore::kIblShSets[0];
}

// V-IBL-STATIC-2: select per-mission SH coefficient set. Called from
// Mission::init right after onMapLoad(); safe to call with nullptr/empty
// (resolves to "default"). Env override MC2_STATIC_PROP_IBL_SH_SET wins
// when set + names an existing set, else registry, else default.
void GpuStaticPropBatcher::setMissionForIbl(const char* missionName) {
    if (s_iblShSetEnvOverride) {
        if (const RenderCore::IblShSet* s =
                RenderCore::findShSetByName(s_iblShSetEnvOverride)) {
            s_currentShSet = s;
            return;
        }
    }
    s_currentShSet = &RenderCore::lookupShSet(missionName);
}

// V-IBL-STATIC-2: inspector accessor. Never returns nullptr.
const char* GpuStaticPropBatcher::getCurrentShSetName() {
    return s_currentShSet ? s_currentShSet->name : "default";
}

// V-IBL-STATIC-2: ibl_sh_runtime.h bridge. GuiRuntime/EditorInspector.cpp
// includes ibl_sh_runtime.h (no Stuff/ dependency) and calls this to
// surface the active set name without pulling the batcher header.
const char* ibl_sh_runtime_currentSetName() {
    return GpuStaticPropBatcher::getCurrentShSetName();
}

void GpuStaticPropBatcher::onMapUnload() {
    GpuStaticPropRegistry::staticPropRegistryClearMaterialCache();
    s_packetTexArrayLayer.clear();  // v2: sidecar is per-map; clear on unload
    s_typeInstanceCountPrevFrame.clear(); // v2.1: per-map; clear on unload
    s_typeDescTable.clear();        // v0: cross-seam mirror is per-map
    if (s_sharedVbo) { glDeleteBuffers(1, &s_sharedVbo); s_sharedVbo = 0; }
    if (s_sharedIbo) { glDeleteBuffers(1, &s_sharedIbo); s_sharedIbo = 0; }
    if (s_sharedVao) { glDeleteVertexArrays(1, &s_sharedVao); s_sharedVao = 0; }
    // Slice 2 (object-offload) — Stage 2.C.2: per-type hot-color SSBO is
    // also per-map; rebuild on next finalizeGeometry.
    if (s_perTypeSsbo) { glDeleteBuffers(1, &s_perTypeSsbo); s_perTypeSsbo = 0; }
    // SHADOW-STATIC-BUILDINGS-2: per-map one-shot building shadow SSBO.
    if (s_staticBldgShadowSsbo) { glDeleteBuffers(1, &s_staticBldgShadowSsbo); s_staticBldgShadowSsbo = 0; }
    if (s_dynamicPropShadowSsbo) {
        GLuint dynShadowLocal = static_cast<GLuint>(s_dynamicPropShadowOwner.glName);
        glDeleteBuffers(1, &dynShadowLocal); s_dynamicPropShadowSsbo = 0;
        // SCENE-SSBO-OWNER-SWEEP-1: mark the registry slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id    = RenderCore::RenderResourceId::DynamicPropShadowSsbo;
        invalid.valid = false;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    // Ring buffers are kept across maps (sized to map's worst case -- grow on demand).

    // Substrate-coalesce per-mission cleanup (plan v3.8 Step group 4).
    //
    // 4.1 — refcount-aware unpin loop. Pattern matches
    // gos_static_prop_registry.cpp:111-124 (releasePinsForRange).
    // Idempotent via vector clear — rebuild populates fresh on next finalize.
    if (mcTextureManager) {
        for (DWORD nodeIdx : s_coalescePinnedNodes) {
            mcTextureManager->unpinNode(nodeIdx);
        }
    }
    s_coalescePinnedNodes.clear();

    // 4.2 — unconditional fence cleanup. Coalesce fences are independent
    // of legacy s_fence[]; drain both rings on map unload.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_coalesceFence[i]) {
            glDeleteSync(s_coalesceFence[i]);
            s_coalesceFence[i] = nullptr;
        }
    }

    // Slice 1 step group 3 — teardown base-instance table SSBO before legacy ring.
    if (s_baseInstanceByCmdSsbo) {
        if (s_baseInstanceByCmdMap) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_baseInstanceByCmdSsbo);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            s_baseInstanceByCmdMap = nullptr;
        }
        glDeleteBuffers(1, &s_baseInstanceByCmdSsbo);
        s_baseInstanceByCmdSsbo             = 0;
        s_baseInstanceByCmdBytesPerFrame    = 0;
    }

    // 4.3 — unmap and delete the persistent-mapped instance SSBO.
    // Handle the partial-failure case where alloc succeeded but mapping
    // failed (Step 5.9 leaves s_coalesceInstanceSsbo non-zero with
    // s_coalesceInstanceMap nullptr in that path).
    if (s_coalesceInstanceSsbo) {
        if (s_coalesceInstanceMap) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_coalesceInstanceSsbo);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            s_coalesceInstanceMap = nullptr;
        }
        glDeleteBuffers(1, &s_coalesceInstanceSsbo);
        s_coalesceInstanceSsbo = 0;
    }

    // M2a POPULATION-SPLIT: tear down the static instance SSBO on map unload,
    // mirroring s_coalesceInstanceSsbo above. ensureStaticInstanceCapacity()
    // early-returns on a non-zero handle, so without this the persistent map
    // handle would survive a context recreation as a dangling pointer. Reset the
    // dirty-gens so the next map's first dirty flush rebuilds base + fill.
    if (s_staticInstanceSsbo) {
        if (s_staticInstanceMap) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_staticInstanceSsbo);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            s_staticInstanceMap = nullptr;
        }
        glDeleteBuffers(1, &s_staticInstanceSsbo);
        s_staticInstanceSsbo  = 0;
        s_staticInstanceBytes = 0;
    }
    if (s_staticIndirectCmdBuf) {
        glDeleteBuffers(1, &s_staticIndirectCmdBuf);
        s_staticIndirectCmdBuf = 0;
    }
    if (s_staticDrawFence) { glDeleteSync(s_staticDrawFence); s_staticDrawFence = nullptr; }
    s_staticDrawCmds.clear();
    s_staticInstanceFillGen = 0xFFFFFFFFFFFFFFFFull;
    s_staticBaseBuiltGen    = 0xFFFFFFFFFFFFFFFFull;
    s_staticCmdBuiltGen     = 0xFFFFFFFFFFFFFFFFull;

    // 4.4 — delete remaining coalesce GL resources (only if non-zero).
    if (s_texArrayOff)     { glDeleteTextures(1, &s_texArrayOff);     s_texArrayOff     = 0; }
    if (s_texArrayOn)      { glDeleteTextures(1, &s_texArrayOn);      s_texArrayOn      = 0; }
    staticPropReleaseBuckets();  // COMPRESSION-BC7-STATICPROP-2 (no-op when gate OFF)
    if (s_perDrawSsbo)      { glDeleteBuffers(1,  &s_perDrawSsbo);      s_perDrawSsbo      = 0; }
    if (s_permutationSsbo)  { glDeleteBuffers(1,  &s_permutationSsbo);  s_permutationSsbo  = 0; }
    if (s_cmdToBucketSsbo)  { glDeleteBuffers(1,  &s_cmdToBucketSsbo);  s_cmdToBucketSsbo  = 0; }
    // MaterialGpu-2 teardown: runs regardless of gate state (defensive).
    // When gate is OFF, s_materialGpuSsbo == 0 and the if-block is skipped entirely.
    if (s_materialGpuSsbo.glName != 0) {
        const size_t byteSize =
            s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "[MATERIAL_GPU v4] event=unload materials=%zu bytes=%zu\n",
                      s_materialGpuTable.size(), byteSize);
        std::fputs(buf, stderr);
        GLuint local = static_cast<GLuint>(s_materialGpuSsbo.glName);
        glDeleteBuffers(1, &local);
        s_materialGpuSsbo.glName = 0;

        // STATICPROP-MATERIAL-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id    = RenderCore::RenderResourceId::StaticPropMaterialGpuBuffer;
        invalid.valid = false;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    s_packetMaterialIdx.clear();
    s_materialGpuTable.clear();
    s_materialInventory.clear();  // V-MATERIAL-STATIC-0
    s_materialGpuSidecarValid = false;  // MAJ-1: reset so flush() cannot sample stale sidecar on reload
    s_sortedPacketOrder.clear();
    s_sortedPacketOrder.shrink_to_fit();
    s_alphaOffCmdCount = 0;
    s_alphaOnCmdCount  = 0;
}

// ---------------------------------------------------------------------------
// Task 6: Type registration.
//
// Packet enumeration: TG_TypeMultiShape::listOfTypeShapes[] nodes are leaves
// (each is either a TG_TypeShape with geometry or a SHAPE_NODE-less bone).
// Callers iterate listOfTypeShapes in author order and call registerType() on
// each SHAPE_NODE leaf, so per-type packet order within this function only
// needs to preserve the flat listOfTypeTriangles author order.
//
// Vertex layout note: in this fork TG_TypeVertex has no UVs; UVs live on
// TG_TypeTriangle::uvdata as per-corner u0/v0/u1/v1/u2/v2. The same vertex can
// carry different UVs on different triangles, so we cannot emit one shared
// vertex per TG_TypeVertex and share it across triangles with an index buffer.
// We expand each triangle to 3 fresh vertices (triangle-soup) and emit a
// trivial 0..N*3-1 index buffer. baseVertex points at the start of this
// type's vertex run in the shared VBO. Packet indexCount = runTris * 3.
// ---------------------------------------------------------------------------
void GpuStaticPropBatcher::registerType(TG_TypeShape* typeShape, TG_TypeMultiShape* multiShape) {
    // multiShape parameter introduced by 5327c4b API cleanup for upcoming
    // alpha-test self-awareness work (Path 4 — texture-name-based material
    // classification at register time). Spec:
    // docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md
    // The header signature was committed in 5327c4b without the matching .cpp
    // signature update, leaving the build broken. This commit closes the gap
    // — multiShape is captured here but not yet consumed; the consumption
    // logic lands in the follow-up alpha-test self-awareness slice.
    (void)multiShape;
    if (!typeShape) return;
    if (s_gpuPropsTrace) ++g_regCall[g_lightProbeSetupPath & 3];   // [GPUPROPS v1]
    if (s_typeIndex.count(typeShape)) return;  // idempotent
    if (s_geometryFinalized) {
        // Layer B: register-after-finalize is a bug in the map-load walk.
        if (!s_failedTypes[typeShape]) {
            std::fprintf(stderr, "[GPUPROPS] late registerType for %p -- "
                         "CPU-fallback for this type\n", (void*)typeShape);
            s_failedTypes[typeShape] = true;
        }
        if (s_gpuPropsTrace) ++g_regLateDrop[g_lightProbeSetupPath & 3];
        return;
    }

    const uint32_t numTris = typeShape->numTypeTriangles;
    if (numTris == 0 || !typeShape->listOfTypeTriangles ||
        !typeShape->listOfTypeVertices) {
        // Empty / helper node -- register with zero packets so duplicate
        // calls remain idempotent.
        GpuStaticPropType emptyType{};
        emptyType.firstPacket = static_cast<uint32_t>(s_packets.size());
        emptyType.packetCount = 0;
        emptyType.vertexCount = 0;
        emptyType.source      = typeShape;
        s_typeIndex[typeShape] = static_cast<uint32_t>(s_types.size());
        s_types.push_back(emptyType);
        return;
    }

    const uint32_t baseVertex = static_cast<uint32_t>(s_stagingVbo.size() / kVertexStride);
    const uint32_t newTypeID  = static_cast<uint32_t>(s_types.size());

    // Group triangles with the same localTextureHandle into contiguous packets,
    // preserving authored listOfTypeTriangles order. Each packet emits 3
    // vertices per triangle into s_stagingVbo and 3 consecutive indices into
    // s_stagingIbo (triangle-soup -- see vertex layout note above).
    uint32_t runStart = 0;
    uint32_t packetCountForThisType = 0;
    while (runStart < numTris) {
        const DWORD runTextureIdx =
            typeShape->listOfTypeTriangles[runStart].localTextureHandle;
        uint32_t runEnd = runStart;
        while (runEnd < numTris &&
               typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTextureIdx) {
            ++runEnd;
        }

        const uint32_t packetFirstIndex = static_cast<uint32_t>(s_stagingIbo.size());

        for (uint32_t t = runStart; t < runEnd; ++t) {
            const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];

            const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
            const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

            for (int c = 0; c < 3; ++c) {
                const uint32_t localVertIdx = tri.Vertices[c];
                // localVertIdx is an index into listOfTypeVertices for the
                // source TG_TypeVertex; we still pass it through to the shader
                // as a_localVertexID for per-instance color indexing.
                const TG_TypeVertex& src = typeShape->listOfTypeVertices[localVertIdx];

                uint8_t vert[kVertexStride] = {};
                std::memcpy(vert +  0, &src.position.x, 4);
                std::memcpy(vert +  4, &src.position.y, 4);
                std::memcpy(vert +  8, &src.position.z, 4);
                std::memcpy(vert + 12, &src.normal.x,   4);
                std::memcpy(vert + 16, &src.normal.y,   4);
                std::memcpy(vert + 20, &src.normal.z,   4);
                std::memcpy(vert + 24, &cornerU[c],     4);
                std::memcpy(vert + 28, &cornerV[c],     4);
                std::memcpy(vert + 32, &localVertIdx,   4);
                // Slice 2 (object-offload) — Stage 2.C.2: per-vertex aRGBLight
                // tag at VBO offset 36 (was zero-filled in Stage 2.A's substrate).
                // Source is TG_TypeVertex::aRGBLight at mclib/tgl.h:54 — the per-
                // type vertex hot-color tag (e.g. 0xffff00ff = "lit window at
                // night", 0xffffff00 = "outside building light"). lighting.hglsl
                // get_base_light() decodes the magic via its expected B,G,R,A
                // byte order on little-endian x86, matching memory/mc2_argb_packing.md.
                // Raw DWORD memcpy preserves the exact bit pattern.
                std::memcpy(vert + 36, &src.aRGBLight,  4);
                s_stagingVbo.insert(s_stagingVbo.end(), vert, vert + kVertexStride);

                const uint32_t expandedIdx =
                    static_cast<uint32_t>((s_stagingVbo.size() / kVertexStride) -
                                          1 - baseVertex);
                s_stagingIbo.push_back(expandedIdx);
            }
        }

        GpuStaticPropPacket pkt{};
        pkt.firstIndex    = packetFirstIndex;
        pkt.indexCount    = (runEnd - runStart) * 3;
        pkt.baseVertex    = static_cast<int32_t>(baseVertex);
        // Store the texture slot index, not the handle. MC2 updates the
        // handle each frame via SetTextureHandle; resolving it at draw
        // time picks up the current value instead of a stale snapshot.
        pkt.textureSlot   = runTextureIdx;
        // alphaTestOn captures shape-level alpha test (trees, via SetAlphaTest).
        // textureAlpha per-slot is resolved at draw time (after bdactor.cpp init completes).
        pkt.materialFlags = typeShape->alphaTestOn ? STATIC_PROP_FLAG_ALPHA_TEST : 0;
        pkt.owningTypeID  = newTypeID;
        s_packets.push_back(pkt);
        ++packetCountForThisType;

        runStart = runEnd;
    }

    const uint32_t numVerts = typeShape->numTypeVertices;

    GpuStaticPropType type{};
    type.firstPacket = static_cast<uint32_t>(s_packets.size()) - packetCountForThisType;
    type.packetCount = packetCountForThisType;
    type.vertexCount = numVerts;
    type.source      = typeShape;

    s_typeIndex[typeShape] = newTypeID;
    s_types.push_back(type);

    // [SEAMPROBE] stage 4: record geometry of each freshly-registered type
    // (capped) so override render-shape leaves can be matched by ptr in the log.
    {
        static int s_seamRegLogged = 0;
        if (getenv("MC2_MODOVERRIDE_TRACE") && s_seamRegLogged < 400) {
            ++s_seamRegLogged;
            fprintf(stderr, "[SEAMPROBE] registerType src=%p typeID=%u numTris=%u numVerts=%u finalized=%d\n",
                (void*)typeShape, newTypeID, numTris, numVerts, (int)s_geometryFinalized);
            fflush(stderr);
        }
    }
}

void GpuStaticPropBatcher::registerMultiShape(TG_TypeMultiShape* multiShape, bool isOverride) {
    if (!multiShape) return;
    const long n       = multiShape->GetNumShapes();
    const long numTxms = multiShape->GetNumTextures();
    for (long i = 0; i < n; ++i) {
        TG_TypeNodePtr node = multiShape->GetTypeNode(i);
        if (node && node->GetNodeType() == SHAPE_NODE) {
            TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(node);
            registerType(typeShape, multiShape);
            // MODEL-OVERRIDE-GPU-BATCHER-SEAM: tag the just-registered (or
            // pre-existing idempotent) type as override-backed so the coalesce
            // layer-build routes its untextured packets to a valid default
            // layer instead of culling them. Look up by source ptr.
            if (isOverride) {
                auto tit = s_typeIndex.find(typeShape);
                if (tit != s_typeIndex.end() && tit->second < s_types.size()) {
                    s_types[tit->second].isOverride = true;
                }
            }
            // GPU-offloaded actors bypass TransformMultiShape, so the leaf
            // TG_TypeShape::listOfTextures[j].gosTextureHandle is never set by
            // TMS (msl.cpp:1380). Prime it now from the multi-type's
            // mcTextureNodeIndex — the same value TMS would use. This ensures
            // flush()'s draw-time resolve picks up the correct (upscaled) handle
            // instead of the 0xffffffff left by TG_TypeShape::init().
            for (long j = 0; j < numTxms; ++j) {
                const DWORD nodeIdx = multiShape->GetTextureHandle(j);
                typeShape->SetTextureHandle(j, nodeIdx);
                // Session-wide cap on registration-time TEX_HANDOFF prints. Without
                // this cap the trace fires ~10K times during map load (one per
                // multiShape × numTxms<4) which floods stdout and can mask later
                // diagnostics. Matches the flush-time per-type cap of 8 at line
                // ~1573 (`s_traceCount < 8`). Counter is function-local static
                // so it resets per process lifetime.
                static int s_texHandoffRegPrinted = 0;
                if (s_texHandoffTrace && j < 4 && s_texHandoffRegPrinted < 8) {
                    const DWORD gosH = (typeShape->listOfTextures && j < typeShape->numTextures)
                                       ? typeShape->listOfTextures[j].gosTextureHandle
                                       : 0xdeadbeef;
                    TEX_HANDOFF("register multiShape=%p leaf=%p slot=%ld nodeIdx=0x%08x gosHandle=0x%08x",
                                (void*)multiShape, (void*)typeShape, j, nodeIdx, gosH);
                    ++s_texHandoffRegPrinted;
                }
            }
        }
    }
}

void GpuStaticPropBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;

    // One-shot log: always emit so the log confirms which tier is active.
    std::printf("[StaticPropBatcher] tex tier: %dpx (MC2_STATICPROP_TEX_TIER)"
                " ktx=%s\n", s_texTier, s_materialKtxEnabled ? "ON" : "OFF");
    std::fflush(stdout);

    GpuStaticPropRegistry::staticPropRegistryClearMaterialCache();
    s_packetTexArrayLayer.clear();  // v2: will be repopulated from layerForPacket below

    // Compile shader programs NOW, at map-load time, while we're on the
    // same code path that compiles every other engine shader. Doing it
    // from inside a mid-render submit() triggers a crash somewhere inside
    // shader_builder — possibly related to the shadow_screen compile
    // failure also seen at map load. Mid-render compile is not a pattern
    // this engine is tested for, so we hoist it here.
    loadProgramsIfNeeded();

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingVbo.size()),
                    s_stagingVbo.data(),
                    0);  // flags=0 -> fully immutable, GPU-only (AMD-safe)

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(),
                    0);

    // Vertex attribute layout -- position MUST be location 0 (AMD invariant 1).
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)24);
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT,      kVertexStride, (void*)32);
    // Slice 2 (object-offload) — Stage 2.C.2: per-vertex aRGBLight tag at
    // offset 36, written by registerType from TG_TypeVertex::aRGBLight.
    // Consumed by static_prop.vert -> get_base_light() to decode the
    // hot-color magic and emit per-vertex base lighting before calc_light.
    glEnableVertexAttribArray(4);
    glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT,      kVertexStride, (void*)36);

    glBindVertexArray(0);

    // Slice 2 (object-offload) — Stage 2.C.2: build per-type hot-color SSBO.
    // 3 vec4 per type, 48 B per type. Decoded from TG_TypeShape::hotPinkRGB
    // (DWORD ARGB) into vec3 + 0 padding (std430 vec4 alignment). Indexed
    // in the shader by inst.typeID — entry 0 corresponds to s_types[0].
    {
        struct PerTypeShaderData {
            float hotPinkRGB[4];
            float hotYellowRGB[4];
            float hotGreenRGB[4];
        };
        static_assert(sizeof(PerTypeShaderData) == 48, "PerTypeShaderData layout must be 48 bytes (3 × vec4 std430)");

        std::vector<PerTypeShaderData> perTypeBlob(s_types.size());
        auto unpack = [](DWORD argb, float out[4]) {
            // Stored as 0xAARRGGBB on the C++ side (matching DWORD convention
            // in tgl.cpp:1799-1801 where the per-vertex hot-color decode
            // pulls (argb>>16) & 0xff = R). Match that.
            out[0] = ((argb >> 16) & 0xFF) / 255.0f;  // R
            out[1] = ((argb >>  8) & 0xFF) / 255.0f;  // G
            out[2] = ((argb >>  0) & 0xFF) / 255.0f;  // B
            out[3] = 0.0f;
        };
        for (size_t i = 0; i < s_types.size(); ++i) {
            const TG_TypeShape* ts = s_types[i].source;
            DWORD pink = 0, yellow = 0, green = 0;
            if (ts) {
                pink   = ts->hotPinkRGB;
                yellow = ts->hotYellowRGB;
                green  = ts->hotGreenRGB;
            }
            unpack(pink,   perTypeBlob[i].hotPinkRGB);
            unpack(yellow, perTypeBlob[i].hotYellowRGB);
            unpack(green,  perTypeBlob[i].hotGreenRGB);
        }

        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &s_perTypeSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perTypeSsbo);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                        static_cast<GLsizeiptr>(perTypeBlob.size() * sizeof(PerTypeShaderData)),
                        perTypeBlob.data(),
                        0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Free CPU staging.
    s_stagingVbo.clear(); s_stagingVbo.shrink_to_fit();
    s_stagingIbo.clear(); s_stagingIbo.shrink_to_fit();

    std::fprintf(stderr, "[GPUPROPS] finalize: %zu types, %zu packets\n",
                 s_types.size(), s_packets.size());

    if (s_gpuPropsTrace) {
        std::fprintf(stderr,
            "[GPUPROPS v1] event=register_summary path=%d "
            "regCalls[init=%u ims=%u splog=%u unk=%u] "
            "lateDrops[init=%u ims=%u splog=%u unk=%u] "
            "typeIndexSize=%zu\n",
            g_lightProbeSetupPath,
            g_regCall[1], g_regCall[2], g_regCall[3], g_regCall[0],
            g_regLateDrop[1], g_regLateDrop[2], g_regLateDrop[3], g_regLateDrop[0],
            s_typeIndex.size());
    }

    s_geometryFinalized = true;

    // =======================================================================
    // Substrate Multi-Draw Coalesce — finalizeGeometry extension.
    // Plan v3.8 Step group 5 (CORE EDIT, §5.0 ordering).
    //
    // Step 5.1 — §CRITICAL-C invariant (pure documentation):
    //
    // From this point on, EVERY return path must leave:
    //   * s_geometryFinalized == true (guaranteed: legacy already wrote it).
    //   * s_permutationSsbo != 0 (identity OR sorted) — Step 10.3 binds
    //     slot 15 every patch dispatch unconditionally; a zero handle
    //     unbinds slot 15 → AMD-specific UB → invisible static props on
    //     legacy fallback. Step 5.4 alloc is UNCONDITIONAL before any
    //     Step 5.5 early-return.
    //   * Three coalesce flags ALL true (success) or ALL false (any
    //     failure path) — no half-armed state.
    //
    // Failure rollback contract (out3-MIN-F): tex-array build's temp
    // pins are released via coalesceRollbackTexBuild before return; the
    // OFF/ON group already-promoted pins in s_coalescePinnedNodes stay
    // until onMapUnload(). Legacy state (s_sharedVbo/Ibo/Vao,
    // s_perTypeSsbo, s_staticPropProgram, s_geometryFinalized) is NEVER
    // touched — that's the whole point of §5.0's "legacy first" rule.
    // =======================================================================

    const auto coalesceStart = std::chrono::steady_clock::now();

    // Step 5.3 — env decision (defensive idempotent guard; loadProgramsIfNeeded
    // already called this once before the latch — Step 7.2 wires that).
    coalesce_resetEnvOnce();
    const bool coalesceWanted =
        !s_coalesceEnvDisabled && s_hasShaderDrawParams;

    // Step 5.4 — ALWAYS alloc identity permutation FIRST, before any
    // not-wanted return. Patch shader (Step 10.3) binds slot 15 every
    // dispatch regardless of armed state.
    allocPermutationSsboAsIdentity(static_cast<uint32_t>(s_types.size()));

    // Step 12.5 — readback the identity permutation when forced-disarm is
    // active so smoke can verify the slot-15 buffer holds [0,1,2,3,...]
    // even after the disarm path runs. Bounded to 16 bytes per mission
    // load when forced; zero cost when env unset.
    if (s_coalesceForceDisarm != CoalesceForceDisarm::None) {
        uint32_t first4[4] = { 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu };
        const uint32_t want = std::min<uint32_t>(4u, static_cast<uint32_t>(s_types.size()));
        if (s_permutationSsbo && want > 0u) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                               static_cast<GLsizeiptr>(want * sizeof(uint32_t)),
                               first4);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        std::fprintf(stderr, "[COALESCE v1] event=permutation_state ssbo=%u "
                     "typeCount=%zu first4=%u,%u,%u,%u\n",
                     s_permutationSsbo, s_types.size(),
                     first4[0], first4[1], first4[2], first4[3]);
        std::fflush(stderr);
    }

    // Step 5.NEW — alpha-class OR-reduce: unconditional (emitter/v0 prerequisite in all modes).
    // Malformed types disarm coalesce when coalesceWanted; in legacy-only mode, alphaClass=0
    // (conservative: emitter treats type as opaque, legacy loop draws it unchanged).
    for (uint32_t typeID = 0; typeID < static_cast<uint32_t>(s_types.size()); ++typeID) {
        auto& type = s_types[typeID];
        bool typeAlpha = false;
        bool malformed = false;
        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const auto& pkt = s_packets[type.firstPacket + p];
            const TG_TypeShape* src = type.source;
            if (!src || !src->listOfTextures) { malformed = true; break; }
            bool pktAlpha = (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
            if (pkt.textureSlot < src->numTextures &&
                src->listOfTextures[pkt.textureSlot].textureAlpha)
                pktAlpha = true;
            typeAlpha = typeAlpha || pktAlpha;
        }
        if (malformed) {
            if (coalesceWanted) {
                std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=malformed_type type=%u\n",
                             typeID);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }
            // Non-coalesce: conservative fallback — legacy draws it correctly regardless.
            std::fprintf(stderr, "[DRAW_PACKET v1] WARNING: type=%u malformed src; alphaClass=0\n",
                         typeID);
            type.alphaClass = 0u;
            continue;
        }
        type.alphaClass = typeAlpha ? 1u : 0u;
    }

    // Step 5.NEW.2 — s_typeDescTable population: unconditional (emitter/v0 prerequisite).
    // Must run after alpha-class OR-reduce above so desc.alphaClass is correct.
    {
        s_typeDescTable.clear();
        s_typeDescTable.reserve(s_types.size());
        uint32_t noGeom = 0u, alphaOn = 0u;
        for (uint32_t i = 0u; i < static_cast<uint32_t>(s_types.size()); ++i) {
            const GpuStaticPropType& t = s_types[i];
            RenderCore::StaticPropTypeDesc desc{};
            desc.typeId      = i;
            desc.firstPacket = t.firstPacket;
            desc.packetCount = t.packetCount;
            desc.alphaClass  = static_cast<uint32_t>(t.alphaClass);
            if (t.packetCount == 0u) ++noGeom;
            if (t.alphaClass  == 1u) ++alphaOn;
            s_typeDescTable.push_back(desc);
        }
        std::fprintf(stderr,
            "[STATIC_PROP_TYPE_TABLE v0] types=%u packet_ranges=%u alpha_on=%u noGeom=%u\n",
            static_cast<uint32_t>(s_typeDescTable.size()),
            static_cast<uint32_t>(s_packets.size()),
            alphaOn, noGeom);
    }

    // Step 5.5 — coalesce-not-wanted early return.
    if (!coalesceWanted) {
        s_coalesceLayoutReady = false;
        s_coalesceEnabled     = false;
        s_coalesceArmed       = false;
        const char* reason = s_coalesceEnvDisabled ? "env_killswitch"
                                                   : "no_extension";
        // Plan v3.8 12B.2 — distinguish forced vs organic in smoke logs.
        // env_killswitch is never forced (MC2_SUBSTRATE_COALESCE_LEGACY is
        // user-driven, not a force-disarm value); only no_extension can be
        // forced via MC2_COALESCE_FORCE_DISARM=no_extension.
        const bool forced = (s_coalesceForceDisarm == CoalesceForceDisarm::NoExtension)
                            && !s_coalesceEnvDisabled;
        std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=%s%s\n",
                     reason, forced ? " (forced)" : "");
        return;
    }

    const uint32_t typeCount = static_cast<uint32_t>(s_types.size());
    if (typeCount == 0) {
        // No types registered — nothing to coalesce; legacy will draw
        // nothing either. Leave flags false; permutation is empty (5.4
        // helper no-ops on typeCount==0). Treat as silent disarm.
        s_coalesceLayoutReady = false;
        s_coalesceEnabled     = false;
        s_coalesceArmed       = false;
        return;
    }

    // Step 12B.4 — MixedAlpha forced-disarm hook (coalesce testing only).
    // alphaClass already computed unconditionally above (Step 5.NEW).
    // Simulates disarm at the coalesce-arm decision point; legacy path unaffected.
    if (s_coalesceForceDisarm == CoalesceForceDisarm::MixedAlpha &&
        typeCount > 0u) {
        std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=mixed_alpha "
                             "type=0 (forced)\n");
        s_coalesceLayoutReady = false;
        s_coalesceEnabled     = false;
        s_coalesceArmed       = false;
        return;
    }

    // Step 5.7 — sort + per-type caps + group totals + per-type byte offsets.
    // Capacity formula per spec §5.1 (verbatim — no ceiling, no grow path):
    //   MIN_PER_TYPE_CAP; globalCap = max(s_instanceCapacity,
    //   INITIAL_INSTANCES_PER_FRAME); cap = max(MIN, (globalCap*2)/typeCount).
    //
    // 2026-05-11 NOTE: per-type cap is a known band-aid. Wolfman zoom can
    // still hit `event=disarmed reason=type_overflow` on tree-heavy stock
    // content (observed type=280 count=259 cap=256 in mc2_10), and on
    // future denser maps any fixed cap will break. The durable fix is a
    // single global instance pool replacing per-type slot allocation.
    // See `docs/superpowers/plans/2026-05-11-global-instance-pool.md`.
    // Until that ships, callers can fall back to MC2_SUBSTRATE_COALESCE_LEGACY=1
    // for wolfman-zoom-correct rendering.
    constexpr uint32_t MIN_PER_TYPE_CAP = 256;
    const uint32_t globalCap = std::max<uint32_t>(
        static_cast<uint32_t>(s_instanceCapacity),
        static_cast<uint32_t>(INITIAL_INSTANCES_PER_FRAME));
    const uint32_t avg2x = (globalCap * 2u) / std::max<uint32_t>(1, typeCount);
    const uint32_t perTypeCap = std::max(MIN_PER_TYPE_CAP, avg2x);
    for (auto& t : s_types) {
        t.instanceCap = perTypeCap;
    }

    s_sortedTypeOrder.clear();
    s_sortedTypeOrder.reserve(typeCount);
    for (uint32_t t = 0; t < typeCount; ++t) {
        if (s_types[t].alphaClass == 0u) s_sortedTypeOrder.push_back(t);
    }
    s_alphaOffCount = static_cast<uint32_t>(s_sortedTypeOrder.size());
    for (uint32_t t = 0; t < typeCount; ++t) {
        if (s_types[t].alphaClass == 1u) s_sortedTypeOrder.push_back(t);
    }
    s_alphaOnCount = static_cast<uint32_t>(s_sortedTypeOrder.size())
                   - s_alphaOffCount;

    {
        size_t offByteCursor = 0;
        size_t onByteCursor  = 0;
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            auto& t = s_types[typeID];
            const size_t typeBytes =
                static_cast<size_t>(t.instanceCap) * sizeof(GpuStaticPropInstance);
            if (t.alphaClass == 0u) {
                t.coalesceByteOffsetWithinGroup =
                    static_cast<uint32_t>(offByteCursor);
                offByteCursor += typeBytes;
            } else {
                t.coalesceByteOffsetWithinGroup =
                    static_cast<uint32_t>(onByteCursor);
                onByteCursor += typeBytes;
            }
        }
        s_offGroupTotalBytes = offByteCursor;
        s_onGroupTotalBytes  = onByteCursor;
    }
    s_coalescePerFrameInstanceBytes =
        s_offGroupTotalBytes + s_onGroupTotalBytes;
    // Step 4.0 — override with global-pool size under non-legacy mode.
    // s_coalescePerFrameInstanceBytes now drives both the alloc (Step 5.9) and
    // batcher_getCoalescePerFrameInstanceBytes() (legacy returns off+on sum).
    if (!s_globalPoolLegacy) {
        s_coalescePerFrameInstanceBytes =
            static_cast<size_t>(s_globalInstanceCap) * sizeof(GpuStaticPropInstance);
    }

    // Step 5.8 — alignment asserts (§3.Z). The std430 stride is 112 bytes
    // for GpuStaticPropInstance (compile-time static_assert in the header),
    // which is a multiple of 16 on every driver we target. The runtime
    // glBindBufferRange offset alignment can be stricter — query and
    // disarm if our group-base alignment is insufficient.
    {
        GLint ssboAlign = 0;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlign);
        if (ssboAlign <= 0) ssboAlign = 16;  // safety floor
        const auto isAligned = [&](size_t v) {
            return (v % static_cast<size_t>(ssboAlign)) == 0;
        };
        if (!isAligned(sizeof(GpuStaticPropInstance)) ||
            !isAligned(s_offGroupTotalBytes) ||
            !isAligned(s_coalescePerFrameInstanceBytes)) {
            std::fprintf(stderr,
                "[COALESCE v1] event=disarmed reason=alloc_failed "
                "(alignment: ssboAlign=%d sizeofInst=%zu offTotal=%zu perFrame=%zu)\n",
                ssboAlign,
                sizeof(GpuStaticPropInstance),
                s_offGroupTotalBytes,
                s_coalescePerFrameInstanceBytes);
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }
    }

    // Step 5.9 — allocate s_coalesceInstanceSsbo ring + persistent map.
    {
        // SSBO-BIND-ALIGN: pad the per-frame ring stride to the SSBO offset
        // alignment IDENTICALLY to batcher_getCoalescePerFrameInstanceBytes() (which
        // drives the per-slot bind offset fr_off_bytes_d), so slot offsets are
        // aligned AND never overrun this allocation.
        const size_t alignedPerFrame = (size_t)gpuAlignUp(
            (unsigned long long)s_coalescePerFrameInstanceBytes,
            (unsigned long long)gpuSsboOffsetAlignment());
        const size_t totalBytes =
            static_cast<size_t>(RING_FRAMES) * alignedPerFrame;
        // TIER2-EXCLUDED: ring
        glGenBuffers(1, &s_coalesceInstanceSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_coalesceInstanceSsbo);
        const GLbitfield mapFlags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        // Drain any pre-existing GL error so our check is clean.
        while (glGetError() != GL_NO_ERROR) {}
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, mapFlags);
        const GLenum storageErr = glGetError();
        if (storageErr != GL_NO_ERROR) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            glDeleteBuffers(1, &s_coalesceInstanceSsbo);
            s_coalesceInstanceSsbo = 0;
            std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                                 "(glBufferStorage err=0x%04x bytes=%zu)\n",
                         storageErr, totalBytes);
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }
        s_coalesceInstanceMap = MC2_GL_MapBufferRange(
            GL_SHADER_STORAGE_BUFFER, 0, totalBytes, mapFlags);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (!s_coalesceInstanceMap) {
            // Step 4.3 handles "buffer alloc'd, map failed" via inner null
            // check on s_coalesceInstanceMap; leave the buffer for unload
            // to release.
            std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                                 "(glMapBufferRange returned null)\n");
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }

        // Plan v3.8 Step 12B.3 — alloc_failed forced-disarm hook. Fires
        // AFTER the real alloc completes successfully, then simulates the
        // failure by tearing down the resources we just allocated. Mirrors
        // the partial-failure cleanup path in Step 4.3 / above.
        if (s_coalesceForceDisarm == CoalesceForceDisarm::AllocFailed) {
            if (s_coalesceInstanceMap) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_coalesceInstanceSsbo);
                glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                s_coalesceInstanceMap = nullptr;
            }
            if (s_coalesceInstanceSsbo) {
                glDeleteBuffers(1, &s_coalesceInstanceSsbo);
                s_coalesceInstanceSsbo = 0;
            }
            std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed (forced)\n");
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }
    }

    s_coalesceLayoutReady = true;

    // Step 5.10 — build per-group GL_TEXTURE_2D_ARRAY (§5.4).
    // layerForType[typeID] = group-relative layer in {s_texArrayOff,
    // s_texArrayOn}; -1 sentinel for zero-packet / no-dim types
    // (Step 5.11 emits a no-op draw entry but keeps array indexing
    // aligned).
    //
    // v3.8 mixed-size handling: stock content has both 32x32 and 64x64
    // textures in the same alpha group. Strict same-size assertion
    // would disarm coalesce on every mc2_NN mission. Instead, allocate
    // the array at MAX dimensions across the group's uniques and blit
    // each smaller texture into the upper-left sub-region (0,0,W,H) of
    // its layer. Per-type uvScaleX/Y in PerDrawEntry (§5.4) maps
    // [0,1] UV → the actual texture sub-region in the larger layer.
    // Shader applies `fract(v_uv) * uvScale` before sampling so
    // GL_REPEAT semantics work for tiling textures.
    std::vector<int32_t> layerForType(typeCount, -1);
    // 2026-05-11 per-packet rework: layerForPacket[globalPacketIdx] gives the
    // group-relative texture-array layer for that packet. The previous
    // per-type layerForType is kept for backward-compat but now records the
    // FIRST packet's layer (used only by zero-packet guards in stale call
    // sites). Multi-packet types had their detail collapsed under per-type
    // layering — see memory/substrate_coalesce_armed_multi_packet_limitation.md.
    std::vector<int32_t> layerForPacket(s_packets.size(), -1);
    // Per-packet uv scale (sub-region size / array size). Default 1.0.
    std::vector<float>   uvScaleXByPacket(s_packets.size(), 1.0f);
    std::vector<float>   uvScaleYByPacket(s_packets.size(), 1.0f);

    struct UniqueTex {
        GLuint glTexId;
        GLint  w;
        GLint  h;
        DWORD  nodeIdx;   // MC_TextureManager node index for KTX2 sidecar lookup
    };

    // COMPRESSION-BC7-STATICPROP-2 — bucket bookkeeping is rebuilt every
    // finalizeGeometry() under the gate. Release any prior arrays + size the
    // per-packet bucket index now (parallel to layerForPacket). Gate-OFF leaves
    // all of this empty (and untouched below), preserving byte-identical output.
    staticPropReleaseBuckets();
    if (s_staticPropBc7Enabled) {
        s_packetBucketIndex.assign(s_packets.size(), -1);
    }

    // COMPRESSION-BC7-STATICPROP-2 — build ONE uniform-dim GL_TEXTURE_2D_ARRAY
    // from a list of same-dimension uniques. Tries BC7 first (all-or-nothing per
    // array — a GL array has a single internalformat); falls back to the RGBA8
    // upload path on any condition miss. Returns the GL handle (0 on hard
    // failure) and sets outIsBc7. This is the per-bucket equivalent of the
    // group-level BC7/RGBA8 logic below; bucketDims == every layer's dims so
    // uvScale is always 1.0 and no sub-region blits occur.
    auto buildBucketArray =
        [&](const std::vector<UniqueTex>& bUniques, GLint bw, GLint bh,
            uint8_t group, bool& outIsBc7, size_t& outBytes) -> GLuint {
        outIsBc7 = false;
        outBytes = 0;
        GLuint arr = 0;

        // ---- BC7 attempt (mirrors the group-level fast path) ----
        if (s_materialKtxEnabled && mcTextureManager &&
            GLEW_ARB_texture_compression_bptc) {
            std::vector<RenderCore::KtxImage> bc7Imgs(bUniques.size());
            bool bc7Ok = true;
            const char* failReason = nullptr;
            int bc7MipCount = 0;
            uint32_t bc7VkFormat = 0;
            for (size_t k = 0; bc7Ok && k < bUniques.size(); ++k) {
                const auto& u = bUniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { bc7Ok = false; failReason = "load_fail"; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (!srcName || !*srcName) { bc7Ok = false; failReason = "load_fail"; break; }
                const std::string ktxPath = deriveTieredKtxPath(srcName, s_texTier);
                if (!RenderCore::ktxLoadRgba8(ktxPath.c_str(), bc7Imgs[k])) {
                    bc7Ok = false; failReason = "load_fail"; break;
                }
                const RenderCore::KtxImage& img = bc7Imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) {
                    bc7Ok = false; failReason = "not_all_bc7"; break;
                }
                if (img.width != bw || img.height != bh) {
                    bc7Ok = false; failReason = "dim_mismatch"; break;
                }
                if (k == 0) { bc7VkFormat = img.vkFormat; bc7MipCount = img.mipCount; }
                if (img.mipCount != bc7MipCount) { bc7Ok = false; failReason = "dim_mismatch"; break; }
            }
            if (bc7Ok) {
                // linear BC7 (UNORM): matches GL_RGBA8 baseline; engine has no GL_FRAMEBUFFER_SRGB
                const GLenum internalformat = GL_COMPRESSED_RGBA_BPTC_UNORM;
                const int levels = (bc7MipCount > 0) ? bc7MipCount : 1;
                // TEX-CLASS: asset-pool -- static-prop BC7 albedo 2D_ARRAY (content)
                glGenTextures(1, &arr);
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               bw, bh, static_cast<GLsizei>(bUniques.size()));
                for (size_t k = 0; k < bUniques.size(); ++k) {
                    const RenderCore::KtxImage& img = bc7Imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (bw >> lvl) ? (bw >> lvl) : 1;
                        const int lh = (bh >> lvl) ? (bh >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(k),
                            lw, lh, 1, internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                        outBytes += static_cast<size_t>(imageSize);
                    }
                }
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                outIsBc7 = true;
                std::fprintf(stderr,
                    "[STATICPROP_BC7] bucket group=%u dims=%dx%d layers=%zu format=BC7 "
                    "levels=%d srcVkFormat=%u bytes=%zu\n",
                    (unsigned)group, bw, bh, bUniques.size(), levels,
                    bc7VkFormat, outBytes);
                std::fflush(stderr);
                return arr;
            } else {
                std::fprintf(stderr,
                    "[STATICPROP_BC7] bucket fallback group=%u dims=%dx%d reason=%s\n",
                    (unsigned)group, bw, bh, failReason ? failReason : "unknown");
                std::fflush(stderr);
            }
        }

        // ---- RGBA8 fallback (mirrors the group-level RGBA8 path; uniform-dim
        //      so every layer fills the whole level → no sub-region blits) ----
        // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
        glGenTextures(1, &arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, bw, bh,
                     static_cast<GLsizei>(bUniques.size()),
                     0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        std::vector<uint8_t> pixelBuf(static_cast<size_t>(bw) * bh * 4u);
        std::vector<std::pair<size_t, RenderCore::KtxImage>> ktxMipLayers;
        for (size_t k = 0; k < bUniques.size(); ++k) {
            const auto& u = bUniques[k];
            bool ktxUsed = false;
            if (s_materialKtxEnabled && u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    const std::string ktxPath = deriveTieredKtxPath(srcName, s_texTier);
                    RenderCore::KtxImage ktxImg;
                    const bool ktxOk = RenderCore::ktxLoadRgba8(ktxPath.c_str(), ktxImg);
                    // Reject compressed sidecars here — block bytes are not RGBA8.
                    const bool dimOk = ktxOk && !ktxImg.isCompressed &&
                                       ktxImg.width == u.w && ktxImg.height == u.h;
                    if (dimOk) {
                        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                        u.w, u.h, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        ktxImg.pixels.data());
                        ktxUsed = true;
                        outBytes += ktxImg.pixels.size();
                        if (ktxImg.mipCount > 1) ktxMipLayers.emplace_back(k, std::move(ktxImg));
                    }
                }
            }
            if (!ktxUsed) {
                glBindTexture(GL_TEXTURE_2D, u.glTexId);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                u.w, u.h, 1, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                outBytes += static_cast<size_t>(bw) * bh * 4u;
            }
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        { GLint w = bw, h = bh; while (w > 1 || h > 1) { ++maxLevel; if (w>1) w>>=1; if (h>1) h>>=1; } }
        if (s_materialKtxEnabled && !ktxMipLayers.empty()) {
            for (const auto& lm : ktxMipLayers) {
                const size_t layerK = lm.first;
                const RenderCore::KtxImage& img = lm.second;
                const int lastLvl = (img.mipCount - 1 < maxLevel) ? (img.mipCount - 1) : maxLevel;
                for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                    int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                    int lh = img.height >> lvl; if (lh < 1) lh = 1;
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(layerK),
                                    lw, lh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    img.pixels.data() +
                                        img.mipByteOffsets[static_cast<size_t>(lvl)]);
                }
            }
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        std::fprintf(stderr,
            "[STATICPROP_BC7] bucket group=%u dims=%dx%d layers=%zu format=RGBA8 bytes=%zu\n",
            (unsigned)group, bw, bh, bUniques.size(), outBytes);
        std::fflush(stderr);
        return arr;
    };

    // STATICPROP-MATERIAL-ORM-1 — build the per-bucket LINEAR ORM sibling array.
    // bUniques / bw / bh are the SAME inputs the albedo buildBucketArray got, so
    // ORM layer k corresponds to albedo layer k (the layer-alignment invariant).
    // Tries a BC7-linear fast path (all-or-nothing per array — a GL array has a
    // single internalformat) when every layer has a compressed sidecar matching
    // bw x bh with identical mipCount; otherwise an RGBA8 array where each layer
    // is either the loaded uncompressed sidecar (dims must equal bw x bh) or a
    // 1x1-replicated neutral (255,255,255,255) texel so the array stays DENSE/
    // bindable. outHasMap[k] = true only where layer k got a REAL ORM map.
    // ORM is LINEAR: bptcInternalFormatFor(false) for BC7, GL_RGBA8 otherwise.
    auto buildOrmBucketArray =
        [&](const std::vector<UniqueTex>& bUniques, GLint bw, GLint bh,
            uint8_t group, std::vector<bool>& outHasMap) -> GLuint {
        outHasMap.assign(bUniques.size(), false);
        GLuint arr = 0;
        const size_t layerCount = bUniques.size();

        // ---- BC7-linear attempt (all-or-nothing, mirrors albedo fast path) ----
        if (mcTextureManager && GLEW_ARB_texture_compression_bptc) {
            std::vector<RenderCore::KtxImage> imgs(layerCount);
            bool ok = true;
            int  mipCount = 0;
            for (size_t k = 0; ok && k < layerCount; ++k) {
                const auto& u = bUniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { ok = false; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (!srcName || !*srcName) { ok = false; break; }
                const std::string ormPath = deriveOrmSidecar(srcName);
                if (!RenderCore::ktxLoadRgba8(ormPath.c_str(), imgs[k])) { ok = false; break; }
                const RenderCore::KtxImage& img = imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) { ok = false; break; }
                if (img.width != bw || img.height != bh) { ok = false; break; }
                if (k == 0) mipCount = img.mipCount;
                if (img.mipCount != mipCount) { ok = false; break; }
            }
            if (ok) {
                const GLenum internalformat = bptcInternalFormatFor(false); // LINEAR
                const int levels = (mipCount > 0) ? mipCount : 1;
                // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
                glGenTextures(1, &arr);
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               bw, bh, static_cast<GLsizei>(layerCount));
                for (size_t k = 0; k < layerCount; ++k) {
                    const RenderCore::KtxImage& img = imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (bw >> lvl) ? (bw >> lvl) : 1;
                        const int lh = (bh >> lvl) ? (bh >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(k),
                            lw, lh, 1, internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                    }
                    outHasMap[k] = true;
                }
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                std::fprintf(stderr,
                    "[STATICPROP_ORM] bucket group=%u dims=%dx%d layers=%zu format=BC7-linear "
                    "levels=%d realMaps=%zu\n",
                    (unsigned)group, bw, bh, layerCount, levels, layerCount);
                std::fflush(stderr);
                return arr;
            }
        }

        // ---- RGBA8 fallback (per-layer: real sidecar or neutral fill) ----
        // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
        glGenTextures(1, &arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, bw, bh,
                     static_cast<GLsizei>(layerCount),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        std::vector<uint8_t> neutral(static_cast<size_t>(bw) * bh * 4u, 255u);
        size_t realMaps = 0;
        std::vector<std::pair<size_t, RenderCore::KtxImage>> ormMipLayers;
        for (size_t k = 0; k < layerCount; ++k) {
            const auto& u = bUniques[k];
            bool used = false;
            if (u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    const std::string ormPath = deriveOrmSidecar(srcName);
                    // STATICPROP-ORM-VISUAL-CHECK-1: observability — log every ORM
                    // unique probed (src path + expected sidecar + dims) so an
                    // operator can author/place a matching .orm.ktx2. Gated; off by default.
                    static const bool s_ormProbeTrace =
                        (getenv("MC2_STATICPROP_ORM_TRACE") != nullptr);
                    if (s_ormProbeTrace) {
                        std::fprintf(stderr,
                            "[STATICPROP_ORM_PROBE] group=%u layer=%zu dims=%dx%d src=%s sidecar=%s\n",
                            (unsigned)group, k, bw, bh, srcName, ormPath.c_str());
                        std::fflush(stderr);
                    }
                    RenderCore::KtxImage img;
                    const bool loadOk = RenderCore::ktxLoadRgba8(ormPath.c_str(), img);
                    // Reject compressed sidecars (block bytes are not RGBA8) and
                    // dim mismatches (must equal the albedo unique's dims).
                    const bool dimOk = loadOk && !img.isCompressed &&
                                       img.width == bw && img.height == bh;
                    if (loadOk && !dimOk) {
                        std::fprintf(stderr,
                            "[STATICPROP_ORM] reject layer=%zu group=%u sidecar=%s "
                            "reason=%s got=%dx%d want=%dx%d\n",
                            k, (unsigned)group, ormPath.c_str(),
                            img.isCompressed ? "compressed_in_rgba8_path" : "dim_mismatch",
                            img.width, img.height, bw, bh);
                        std::fflush(stderr);
                    }
                    if (dimOk) {
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                        bw, bh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        img.pixels.data());
                        used = true;
                        outHasMap[k] = true;
                        ++realMaps;
                        if (img.mipCount > 1) ormMipLayers.emplace_back(k, std::move(img));
                    }
                }
            }
            if (!used) {
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                bw, bh, 1, GL_RGBA, GL_UNSIGNED_BYTE, neutral.data());
            }
        }
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        { GLint w = bw, h = bh; while (w > 1 || h > 1) { ++maxLevel; if (w>1) w>>=1; if (h>1) h>>=1; } }
        for (const auto& lm : ormMipLayers) {
            const size_t layerK = lm.first;
            const RenderCore::KtxImage& img = lm.second;
            const int lastLvl = (img.mipCount - 1 < maxLevel) ? (img.mipCount - 1) : maxLevel;
            for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                int lh = img.height >> lvl; if (lh < 1) lh = 1;
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(layerK),
                                lw, lh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                img.pixels.data() +
                                    img.mipByteOffsets[static_cast<size_t>(lvl)]);
            }
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        std::fprintf(stderr,
            "[STATICPROP_ORM] bucket group=%u dims=%dx%d layers=%zu format=RGBA8 realMaps=%zu\n",
            (unsigned)group, bw, bh, layerCount, realMaps);
        std::fflush(stderr);
        return arr;
    };

    for (uint8_t group = 0; group <= 1; ++group) {
        std::vector<DWORD> newlyPinnedThisBuild;
        GLuint& outArray = (group == 0u) ? s_texArrayOff : s_texArrayOn;

        std::vector<UniqueTex> uniques;             // unique textures in this group
        std::unordered_map<GLuint, int32_t> glTexIdToLayer;
        // Track packets that landed in this group, for the second-pass uvScale
        // assignment. Each entry: { globalPacketIdx, layer }.
        std::vector<std::pair<uint32_t, int32_t>> packetsInGroup;

        // 5.10.a — walk types in sorted order belonging to this group; for
        // each type, walk ALL its packets. Each packet gets its own layer
        // (deduped by glTexId) — this is the per-packet rework's core change.
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            auto& type = s_types[typeID];
            if (type.alphaClass != group) continue;

            // Zero-packet guard mirrors batcher_getTypeDrawInfo pattern;
            // leave layer = -1 (already initialized) and continue.
            if (type.packetCount == 0u) {
                continue;
            }

            for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                const uint32_t globalPktIdx = type.firstPacket + pIdx;
                const auto& pkt = s_packets[globalPktIdx];
                const TG_TypeShape* src = type.source;
                if (!src || !src->listOfTextures ||
                    pkt.textureSlot >= src->numTextures) {
                    std::fprintf(stderr, "[COALESCE v1] event=disarmed "
                                 "reason=malformed_type type=%u pkt=%u group=%s\n",
                                 typeID, pIdx, group == 0u ? "off" : "on");
                    coalesceRollbackTexBuild(newlyPinnedThisBuild);
                    s_coalesceLayoutReady = false;
                    s_coalesceEnabled     = false;
                    s_coalesceArmed       = false;
                    return;
                }

                const DWORD nodeIdx =
                    src->listOfTextures[pkt.textureSlot].mcTextureNodeIndex;
                if (mcTextureManager && nodeIdx != 0xFFFFFFFFu) {
                    mcTextureManager->pinNode(nodeIdx);
                    newlyPinnedThisBuild.push_back(nodeIdx);
                }

                DWORD gosHandle = (mcTextureManager && nodeIdx != 0xFFFFFFFFu)
                    ? mcTextureManager->get_gosTextureHandle(nodeIdx)
                    : src->listOfTextures[pkt.textureSlot].gosTextureHandle;
                // Snapshot first-packet handle for the runtime eviction
                // detect (Step 11.4 still per-type). Multi-packet types use
                // their first packet as the canary — sufficient for the
                // mid-mission cache-evict guard (multi-textured types
                // typically share underlying texture lifetime).
                if (pIdx == 0) {
                    type.lastSeenGosHandle = gosHandle;
                }

                const GLuint glTexId = static_cast<GLuint>(gos_GetGLTextureId(gosHandle));

                // 5.10.b — dedupe per glTexId; assign group-relative layer.
                auto it = glTexIdToLayer.find(glTexId);
                if (it != glTexIdToLayer.end()) {
                    layerForPacket[globalPktIdx] = it->second;
                    if (pIdx == 0) layerForType[typeID] = it->second;
                    packetsInGroup.emplace_back(globalPktIdx, it->second);
                    continue;
                }

            // v3.8 dimension fetch: use mcTextureManager metadata first
            // (avoids the lazy-GL-upload trap where glGetTexLevelParameteriv
            // returns 0x0 for textures that have a handle but haven't been
            // glTexImage2D'd yet). MC2 textures are square (only a single
            // `width` field exists in MC_TextureNode). Fall back to GL if
            // metadata is unavailable.
            GLint W = (mcTextureManager && nodeIdx != 0xFFFFFFFFu)
                ? static_cast<GLint>(mcTextureManager->getWidth(nodeIdx))
                : 0;
            GLint H = W;
            if (W <= 0) {
                glBindTexture(GL_TEXTURE_2D, glTexId);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &W);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &H);
            }
            // If both metadata AND GL fail, the texture is genuinely
            // unavailable at finalize-time (no MC node, never cached,
            // and no GL upload). Skip this packet via layer=-1 sentinel
            // rather than disarming the whole coalesce build — graceful
            // degradation: other valid packets still arm.
            if (W <= 0 || H <= 0) {
                COALESCE_TRACE("skip_packet type=%u pkt=%u group=%s reason=no_dims "
                               "nodeIdx=%lu gosHandle=%lu glTexId=%u",
                               typeID, pIdx, group == 0u ? "off" : "on",
                               (unsigned long)nodeIdx,
                               (unsigned long)gosHandle, glTexId);
                // [SEAMPROBE] stage 10: layer-assignment census. When the
                // override prop/tree packets land here, they get layer=-1 and
                // are dropped from the multidraw (the bdactor.cpp:262 skip
                // rule) -> override never rasterizes. Gated, capped.
                {
                    static int s_seamLayerSkip = 0;
                    if (getenv("MC2_MODOVERRIDE_TRACE") && s_seamLayerSkip < 200) {
                        ++s_seamLayerSkip;
                        std::fprintf(stderr, "[SEAMPROBE] layer=-1 SKIP type=%u pkt=%u "
                            "nodeIdx=%lu gosHandle=%lu glTexId=%u W=%d H=%d override=%d uniques=%zu\n",
                            typeID, pIdx, (unsigned long)nodeIdx,
                            (unsigned long)gosHandle, glTexId, (int)W, (int)H,
                            (int)type.isOverride, uniques.size());
                        std::fflush(stderr);
                    }
                }
                // MODEL-OVERRIDE-GPU-BATCHER-SEAM-PROBE-1 fix: a modder glTF
                // render-override imports with unresolved "NULLTXM" texture
                // handles (nodeIdx=0xFFFFFFFF, no GL upload -> W<=0 here). The
                // stock skip rule (layer=-1 -> dropped from the coalesce
                // multidraw, see bdactor.cpp:262) makes the override geometry
                // invisible. For override-backed types ONLY, route the packet
                // to an already-built valid layer in this group (layer 0) so it
                // RASTERIZES with that texture (wrong albedo, but visible) —
                // the MVP success criterion is a box that draws, not correct
                // texturing. Stock/damage-shape packets are UNAFFECTED: they
                // keep the layer=-1 skip (no isOverride flag), so the orange-
                // ghost destroyed-building regression cannot recur. If no valid
                // layer exists yet in this group, fall through to the skip.
                if (type.isOverride && !uniques.empty()) {
                    layerForPacket[globalPktIdx] = 0;
                    if (pIdx == 0) layerForType[typeID] = 0;
                    packetsInGroup.emplace_back(globalPktIdx, 0);
                    if (getenv("MC2_MODOVERRIDE_TRACE")) {
                        std::fprintf(stderr, "[SEAMPROBE] OVERRIDE_ROUTE type=%u pkt=%u "
                            "-> layer=0 (default) group=%s\n",
                            typeID, pIdx, group == 0u ? "off" : "on");
                        std::fflush(stderr);
                    }
                    continue;
                }
                continue;
            }

            // Plan v3.8 Step 12B.5 — size_mismatch forced-disarm hook.
            if (s_coalesceForceDisarm == CoalesceForceDisarm::SizeMismatch &&
                group == 0u && uniques.size() == 1u) {
                std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=size_mismatch "
                             "group=off expected=%dx%d got=%dx%d (forced)\n",
                             uniques[0].w, uniques[0].h,
                             uniques[0].w + 1, uniques[0].h + 1);
                coalesceRollbackTexBuild(newlyPinnedThisBuild);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }

            // v3.8 mixed-size: append unique with its actual W,H.
            // Array allocation uses max(W,H) across all uniques.
            const int32_t layer = static_cast<int32_t>(uniques.size());
            uniques.push_back({glTexId, W, H, nodeIdx});
            glTexIdToLayer[glTexId] = layer;
            layerForPacket[globalPktIdx] = layer;
            if (pIdx == 0) layerForType[typeID] = layer;
            packetsInGroup.emplace_back(globalPktIdx, layer);
            } // end per-packet loop
        }

        if (uniques.empty()) {
            // Empty group — Step 11.7.g/h skip the bind+draw pair when
            // count == 0 (out-CRIT-5). No array allocated; no pins to
            // promote (newlyPinnedThisBuild is empty).
            continue;
        }

        // ====================================================================
        // COMPRESSION-BC7-STATICPROP-2 — bucketed build (gate ON only).
        // Partition this group's uniques by (w,h). Each bucket is uniform-dim
        // by construction → eligible for an independent BC7 or RGBA8 array.
        // layerForPacket becomes BUCKET-RELATIVE (layer within the bucket
        // array, same semantics the shader already consumes). uvScale = 1.0.
        // The gate-OFF group-build below is left UNTOUCHED and never runs here.
        // ====================================================================
        if (s_staticPropBc7Enabled) {
            // Group uniques (by group-relative layer index) into dim buckets.
            struct DimKey { GLint w, h; };
            std::vector<DimKey>               dimKeys;       // unique dims, stable order of first appearance
            std::vector<std::vector<int32_t>> bucketLayers;  // per dim: list of group-relative layer indices
            std::vector<int32_t>              layerToBucketLocal(uniques.size(), -1);   // group-rel layer -> local bucket
            std::vector<int32_t>              layerToBucketRelLayer(uniques.size(), -1); // group-rel layer -> bucket-relative layer

            for (size_t lyr = 0; lyr < uniques.size(); ++lyr) {
                const GLint w = uniques[lyr].w, h = uniques[lyr].h;
                int found = -1;
                for (size_t b = 0; b < dimKeys.size(); ++b) {
                    if (dimKeys[b].w == w && dimKeys[b].h == h) { found = static_cast<int>(b); break; }
                }
                if (found < 0) {
                    found = static_cast<int>(dimKeys.size());
                    dimKeys.push_back({w, h});
                    bucketLayers.emplace_back();
                }
                layerToBucketLocal[lyr]    = found;
                layerToBucketRelLayer[lyr] = static_cast<int32_t>(bucketLayers[found].size());
                bucketLayers[found].push_back(static_cast<int32_t>(lyr));
            }

            // Build one GL array per dim bucket; record into the global vectors.
            // localBucketToGlobal maps this group's local bucket index -> global
            // s_bucketArrays index (used to tag packets).
            std::vector<int32_t> localBucketToGlobal(dimKeys.size(), -1);
            size_t groupTotalBytes = 0;
            for (size_t b = 0; b < dimKeys.size(); ++b) {
                std::vector<UniqueTex> sub;
                sub.reserve(bucketLayers[b].size());
                for (int32_t lyr : bucketLayers[b]) sub.push_back(uniques[static_cast<size_t>(lyr)]);

                bool isBc7 = false; size_t bytes = 0;
                const GLuint arr = buildBucketArray(sub, dimKeys[b].w, dimKeys[b].h,
                                                    group, isBc7, bytes);
                groupTotalBytes += bytes;

                const int32_t globalBucket = static_cast<int32_t>(s_bucketArrays.size());
                localBucketToGlobal[b] = globalBucket;
                s_bucketArrays.push_back(arr);
                StaticPropBucketInfo info{};
                info.group      = group;
                info.w          = static_cast<uint16_t>(dimKeys[b].w);
                info.h          = static_cast<uint16_t>(dimKeys[b].h);
                info.layerCount = static_cast<uint32_t>(sub.size());
                info.isBc7      = isBc7;
                info.glArray    = arr;
                s_bucketInfo.push_back(info);

                // STATICPROP-MATERIAL-ORM-1 — build the layer-aligned ORM sibling
                // for THIS bucket from the SAME `sub` uniques, so ORM array index
                // == albedo array index (globalBucket) and ORM layer k == albedo
                // layer k. Gate-OFF builds nothing (vectors stay empty); gate-ON
                // keeps s_ormBucketArrays.size() == s_bucketArrays.size().
                if (s_ormSlotsEnabled) {
                    std::vector<bool> ormHasMap;
                    const GLuint ormArr = buildOrmBucketArray(sub, dimKeys[b].w, dimKeys[b].h,
                                                              group, ormHasMap);
                    s_ormBucketArrays.push_back(ormArr);
                    s_ormLayerHasMap.push_back(std::move(ormHasMap));
                }
            }

            // Reassign layerForPacket -> bucket-relative layer, tag each packet
            // with its global bucket index, and set uvScale = 1.0 (uniform-dim).
            for (const auto& kv : packetsInGroup) {
                const uint32_t globalPktIdx = kv.first;
                const int32_t  grpLayer     = kv.second;   // group-relative layer
                if (grpLayer < 0) continue;
                const int32_t localB   = layerToBucketLocal[static_cast<size_t>(grpLayer)];
                const int32_t relLayer = layerToBucketRelLayer[static_cast<size_t>(grpLayer)];
                layerForPacket[globalPktIdx]      = relLayer;            // bucket-relative
                s_packetBucketIndex[globalPktIdx] = localBucketToGlobal[static_cast<size_t>(localB)];
                uvScaleXByPacket[globalPktIdx]    = 1.0f;
                uvScaleYByPacket[globalPktIdx]    = 1.0f;
            }

            std::fprintf(stderr,
                "[STATICPROP_BC7] group=%u buckets=%zu total_bytes=%zu\n",
                (unsigned)group, dimKeys.size(), groupTotalBytes);
            std::fflush(stderr);

            // Promote temp pins (same as both group-level paths) and skip the
            // original 2-group build for this group.
            s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                          newlyPinnedThisBuild.begin(),
                                          newlyPinnedThisBuild.end());
            continue;
        }

        // 5.10.d — find max dimensions across the group's uniques.
        // The texture array is allocated at maxW × maxH; smaller
        // textures get blitted into upper-left sub-region of their
        // layer; per-type uvScale maps [0,1] UV → sub-region.
        GLint maxW = 0, maxH = 0;
        for (const auto& u : uniques) {
            if (u.w > maxW) maxW = u.w;
            if (u.h > maxH) maxH = u.h;
        }
        if (maxW <= 0 || maxH <= 0) {
            // Defensive — shouldn't happen because skip-path filtered W/H<=0.
            std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                         "(group=%s maxDims=%dx%d after skip-no-dims pass)\n",
                         group == 0u ? "off" : "on", maxW, maxH);
            coalesceRollbackTexBuild(newlyPinnedThisBuild);
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }

        // -------------------------------------------------------------------
        // COMPRESSION-BC7-STATICPROP-1 — BC7-compressed array fast path.
        // Gate: MC2_MATERIAL_KTX=1 AND MC2_STATICPROP_BC7=1.
        // A GL array has ONE internalformat, so BC7 is ALL-OR-NOTHING per
        // array. Additionally we require the group to be UNIFORM-DIM (every
        // layer fills the whole level) so compressed sub-region uploads never
        // violate 4x4 block alignment at small mips. If any condition fails we
        // fall through to the existing RGBA8 path below, byte-identical.
        // -------------------------------------------------------------------
        if (s_materialKtxEnabled && s_staticPropBc7Enabled && mcTextureManager) {
            // Probe ALL uniques: load each sidecar, require all load && all BC7
            // && all dims match && uniform-dim.
            std::vector<RenderCore::KtxImage> bc7Imgs(uniques.size());
            bool bc7Ok      = true;
            const char* failReason = nullptr;
            uint32_t bc7VkFormat = 0;   // shared format across the array
            int      bc7MipCount = 0;   // shared mip count across the array

            // Uniform-dim check first (cheap, no file IO).
            for (const auto& u : uniques) {
                if (u.w != maxW || u.h != maxH) { bc7Ok = false; failReason = "mixed_dims"; break; }
            }

            for (size_t k = 0; bc7Ok && k < uniques.size(); ++k) {
                const auto& u = uniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { bc7Ok = false; failReason = "load_fail"; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (!srcName || !*srcName) { bc7Ok = false; failReason = "load_fail"; break; }
                const std::string ktxPath = deriveTieredKtxPath(srcName, s_texTier);
                if (!RenderCore::ktxLoadRgba8(ktxPath.c_str(), bc7Imgs[k])) {
                    bc7Ok = false; failReason = "load_fail"; break;
                }
                const RenderCore::KtxImage& img = bc7Imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) {
                    bc7Ok = false; failReason = "not_all_bc7"; break;
                }
                if (img.width != u.w || img.height != u.h) {
                    bc7Ok = false; failReason = "dim_mismatch"; break;
                }
                if (k == 0) { bc7VkFormat = img.vkFormat; bc7MipCount = img.mipCount; }
                // Require a consistent mip count so glTexStorage3D's level count
                // is valid for every layer.
                if (img.mipCount != bc7MipCount) { bc7Ok = false; failReason = "dim_mismatch"; break; }
            }

            // Capability check: BPTC (GL 4.3 / ARB_texture_compression_bptc).
            if (bc7Ok && !GLEW_ARB_texture_compression_bptc) {
                bc7Ok = false; failReason = "no_bptc_support";
            }

            if (bc7Ok) {
                // linear BC7 (UNORM): matches GL_RGBA8 baseline; engine has no GL_FRAMEBUFFER_SRGB
                const GLenum internalformat = GL_COMPRESSED_RGBA_BPTC_UNORM;
                const int levels = (bc7MipCount > 0) ? bc7MipCount : 1;

                // TEX-CLASS: asset-pool -- static-prop material 2D_ARRAY (content)
                glGenTextures(1, &outArray);
                glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               maxW, maxH, static_cast<GLsizei>(uniques.size()));

                size_t bc7TotalBytes = 0;
                for (size_t k = 0; k < uniques.size(); ++k) {
                    const RenderCore::KtxImage& img = bc7Imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (maxW >> lvl) ? (maxW >> lvl) : 1;
                        const int lh = (maxH >> lvl) ? (maxH >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl,
                            0, 0, static_cast<GLint>(k),
                            lw, lh, 1,
                            internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                        bc7TotalBytes += static_cast<size_t>(imageSize);
                    }
                }

                // Sampler: match the RGBA8 path. NO glGenerateMipmap (compressed
                // mips come straight from the cooked KTX). MAX_LEVEL = levels-1.
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

                // uvScale bookkeeping: uniform-dim → uvScale = 1.0 (full level).
                for (const auto& kv : packetsInGroup) {
                    const uint32_t globalPktIdx = kv.first;
                    if (kv.second < 0) continue;
                    uvScaleXByPacket[globalPktIdx] = 1.0f;
                    uvScaleYByPacket[globalPktIdx] = 1.0f;
                }

                std::fprintf(stderr,
                    "[STATICPROP_BC7] group=%u layers=%zu dims=%dx%d levels=%d "
                    "internalformat=0x%x bytes=%zu\n",
                    (unsigned)group, uniques.size(), maxW, maxH, levels,
                    (unsigned)internalformat, bc7TotalBytes);
                std::fprintf(stderr,
                    "[STATICPROP_BC7] summary group=%u built BC7 array "
                    "(srcVkFormat=%u, %zu layers)\n",
                    (unsigned)group, bc7VkFormat, uniques.size());
                std::fflush(stderr);

                COALESCE_TRACE("group_built_bc7 group=%s uniques=%zu maxDims=%dx%d levels=%d",
                               group == 0u ? "off" : "on",
                               uniques.size(), maxW, maxH, levels);

                // Promote temp pins (same as the RGBA8 path) and skip the
                // remaining RGBA8 array-build for this group.
                s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                              newlyPinnedThisBuild.begin(),
                                              newlyPinnedThisBuild.end());
                continue;
            } else {
                std::fprintf(stderr,
                    "[STATICPROP_BC7] fallback group=%u reason=%s\n",
                    (unsigned)group, failReason ? failReason : "unknown");
                std::fflush(stderr);
                // Fall through to the existing RGBA8 path unchanged.
            }
        }

        // 5.10.d — allocate the array at max dims.
        // TEX-CLASS: asset-pool -- static-prop material 2D_ARRAY (content)
        glGenTextures(1, &outArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                     maxW, maxH,
                     static_cast<GLsizei>(uniques.size()),
                     0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

        // 5.10.e — per-unique blit. Each unique blits at sub-region
        // (0,0,W,H) of its layer; the unused (W..maxW, H..maxH) area
        // is left at the glTexImage3D-zeroed state. Shader uvScale
        // ensures sampling never reaches that area for the type's UVs.
        size_t maxBytes = static_cast<size_t>(maxW) *
                          static_cast<size_t>(maxH) * 4u;
        std::vector<uint8_t> pixelBuf(maxBytes);
        // KTX2 layers that carry a pre-baked mip chain: cache the loaded image
        // keyed by layer index so we can OVERWRITE the auto-generated mips with
        // the sharp pre-baked levels after the single glGenerateMipmap pass.
        std::vector<std::pair<size_t, RenderCore::KtxImage>> s_ktxMipLayers;
        for (size_t k = 0; k < uniques.size(); ++k) {
            const auto& u = uniques[k];
            bool ktxUsed = false;
            if (s_materialKtxEnabled && u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    // Derive tiered .ktx2 path: data/tgl/<tier>/foo.ktx2
                    const std::string ktxPath = deriveTieredKtxPath(srcName, s_texTier);
                    RenderCore::KtxImage ktxImg;
                    const bool ktxOk = RenderCore::ktxLoadRgba8(ktxPath.c_str(), ktxImg);
                    // This RGBA8 upload path cannot consume compressed (BC7) KTX2:
                    // its bytes are block data, not RGBA8 texels. If a BC7 sidecar
                    // is present but this group fell back to the RGBA8 path (e.g.
                    // mixed-dim, so the BC7 array path was declined), reject it here
                    // (dimOk=false) so we fall through to the original GL texture
                    // upload below instead of mis-uploading block data as RGBA8.
                    const bool dimOk = ktxOk && !ktxImg.isCompressed &&
                                       ktxImg.width == u.w && ktxImg.height == u.h;
                    // Always-on debug: log attempt result so we can diagnose path/dim issues.
                    std::fprintf(stderr,
                        "[KTX_SIDECAR] node=%lu srcName=%s path=%s "
                        "load=%d ktxDims=%dx%d glDims=%dx%d match=%d\n",
                        (unsigned long)u.nodeIdx, srcName, ktxPath.c_str(),
                        (int)ktxOk,
                        ktxOk ? ktxImg.width : 0, ktxOk ? ktxImg.height : 0,
                        u.w, u.h, (int)dimOk);
                    std::fflush(stderr);
                    if (dimOk) {
                        // KTX2 pixels are RGBA order; GL_RGBA matches.
                        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                        0, 0, static_cast<GLint>(k),
                                        u.w, u.h, 1,
                                        GL_RGBA, GL_UNSIGNED_BYTE, ktxImg.pixels.data());
                        ktxUsed = true;
                        // Stash for post-glGenerateMipmap pre-baked mip overwrite.
                        if (ktxImg.mipCount > 1) {
                            s_ktxMipLayers.emplace_back(k, std::move(ktxImg));
                        }
                        COALESCE_TRACE("ktx_sidecar_hit type=? node=%lu path=%s",
                                       (unsigned long)u.nodeIdx, ktxPath.c_str());
                    }
                }
            }
            if (!ktxUsed) {
                glBindTexture(GL_TEXTURE_2D, u.glTexId);
                const size_t need = static_cast<size_t>(u.w) *
                                    static_cast<size_t>(u.h) * 4u;
                if (pixelBuf.size() < need) pixelBuf.resize(need);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                0, 0, static_cast<GLint>(k),
                                u.w, u.h, 1,
                                GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
            }
        }

        // 5.10.f — mipmap + sampler params (forestall AMD strict-fail).
        // GL_CLAMP_TO_EDGE on the array (was GL_REPEAT pre-v3.8) so
        // sub-region textures don't bleed into the unused area at the
        // edge. Tiling textures rely on the shader's `fract(v_uv) *
        // uvScale` to keep sampling within the sub-region.
        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        {
            GLint w = maxW, h = maxH;
            while (w > 1 || h > 1) {
                ++maxLevel;
                if (w > 1) w >>= 1;
                if (h > 1) h >>= 1;
            }
        }
        // 5.10.f.ktx — OVERWRITE auto-generated mips with the pre-baked KTX2
        // mip chain for layers that carried one. glGenerateMipmap above filled
        // mips for ALL layers (KTX + non-KTX) from level 0; here we replace the
        // blurry auto-gen mips on KTX layers with the sharp baked levels.
        // Non-KTX layers keep their auto-generated mips. Only overwrite levels
        // that exist both in the baked chain AND within the array's maxLevel.
        if (s_materialKtxEnabled && !s_ktxMipLayers.empty()) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
            int totalMipsUploaded = 0;
            for (const auto& lm : s_ktxMipLayers) {
                const size_t layerK = lm.first;
                const RenderCore::KtxImage& img = lm.second;
                const int lastLvl = (img.mipCount - 1 < maxLevel)
                                        ? (img.mipCount - 1) : maxLevel;
                int uploaded = 0;
                for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                    int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                    int lh = img.height >> lvl; if (lh < 1) lh = 1;
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl,
                                    0, 0, static_cast<GLint>(layerK),
                                    lw, lh, 1,
                                    GL_RGBA, GL_UNSIGNED_BYTE,
                                    img.pixels.data() +
                                        img.mipByteOffsets[static_cast<size_t>(lvl)]);
                    ++uploaded;
                }
                totalMipsUploaded += uploaded;
                std::fprintf(stderr,
                    "[KTX2_MIP] uploaded %d pre-baked mips for layer %zu (%dx%d)\n",
                    uploaded, layerK, img.width, img.height);
            }
            std::fprintf(stderr,
                "[KTX2_MIP] summary: %zu KTX layers, %d pre-baked mips overwritten\n",
                s_ktxMipLayers.size(), totalMipsUploaded);
            std::fflush(stderr);
        }

        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 2026-05-11 per-packet rework: uvScale is per-PACKET, derived from
        // each packet's unique texture dimensions vs the array max dims.
        const float invMaxW = 1.0f / static_cast<float>(maxW);
        const float invMaxH = 1.0f / static_cast<float>(maxH);
        for (const auto& kv : packetsInGroup) {
            const uint32_t globalPktIdx = kv.first;
            const int32_t layer = kv.second;
            if (layer < 0) continue;
            const auto& u = uniques[static_cast<size_t>(layer)];
            uvScaleXByPacket[globalPktIdx] = static_cast<float>(u.w) * invMaxW;
            uvScaleYByPacket[globalPktIdx] = static_cast<float>(u.h) * invMaxH;
        }

        COALESCE_TRACE("group_built group=%s uniques=%zu maxDims=%dx%d",
                       group == 0u ? "off" : "on",
                       uniques.size(), maxW, maxH);

        // 5.10.g — promote temp pins.
        s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                      newlyPinnedThisBuild.begin(),
                                      newlyPinnedThisBuild.end());
    }

    // v2: persist per-packet texArrayLayer before layerForPacket goes stale.
    // layerForPacket is fully populated here; the PerDrawEntry loop below reads
    // but does not modify it. -1 entries = packets with unavailable textures.
    s_packetTexArrayLayer.assign(layerForPacket.begin(), layerForPacket.end());

    // 2026-05-11 per-packet rework: build s_sortedPacketOrder, s_alphaOff/OnCmdCount,
    // s_perDrawSsbo (one entry per packet in sorted order), and s_cmdToBucketSsbo
    // (one uint typeID per cmd). Fragment shader reads
    // entries[v_drawID + u_drawIDBase] where v_drawID = gl_DrawID = sorted packet
    // slot, u_drawIDBase = 0 for alpha-OFF group, s_alphaOffCmdCount for alpha-ON.
    s_sortedPacketOrder.clear();
    s_alphaOffCmdCount = 0;
    s_alphaOnCmdCount  = 0;
    {
        // 2026-05-11 packet-skip rule: only enroll packets whose texture-
        // array layer was successfully assigned. Damage-shape leaves
        // (bldgDmgShape) typically have nodeIdx=0xFFFFFFFF at finalize
        // because their textures are loaded only at destruction time per-
        // instance — they end up with layerForPacket=-1. Without skipping,
        // the indirect cmd still emits, the fragment samples layer 0 of
        // the array, and a wrong-texture (orange-rectangle) ghost renders
        // at the prop's location. Skipping leaves their geometry out of
        // the multidraw entirely.
        // COMPRESSION-BC7-STATICPROP-2: when the gate is ON, sorted slots must
        // be grouped into contiguous per-bucket RUNS so each dispatch can bind
        // one bucket array and draw its run. We order buckets ascending by
        // GLOBAL bucket index — and group buckets carry the alpha group in
        // s_bucketInfo, with all alpha-OFF buckets built before alpha-ON ones
        // (the build walks group 0 then group 1) → bucket index order already
        // respects alpha order (OFF runs precede ON runs). s_slotBucketIndex /
        // s_bucketCmdCount are populated in lockstep. Gate-OFF leaves both
        // empty and uses the original 2-group emission below (byte-identical).
        s_slotBucketIndex.clear();
        s_bucketCmdCount.assign(s_bucketArrays.size(), 0u);
        for (uint8_t group = 0; group <= 1; ++group) {
            if (s_staticPropBc7Enabled) {
                // Emit this group's packets bucket-by-bucket (global bucket order).
                for (uint32_t b = 0; b < s_bucketInfo.size(); ++b) {
                    if (s_bucketInfo[b].group != group) continue;
                    for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
                        const uint32_t typeID = s_sortedTypeOrder[i];
                        const auto& type = s_types[typeID];
                        if (type.alphaClass != group) continue;
                        for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                            const uint32_t globalPktIdx = type.firstPacket + pIdx;
                            if (layerForPacket[globalPktIdx] < 0) continue;
                            if (s_packetBucketIndex[globalPktIdx] != static_cast<int32_t>(b)) continue;
                            s_sortedPacketOrder.push_back(globalPktIdx);
                            s_slotBucketIndex.push_back(static_cast<uint8_t>(b));
                            ++s_bucketCmdCount[b];
                        }
                    }
                }
            } else {
                for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
                    const uint32_t typeID = s_sortedTypeOrder[i];
                    const auto& type = s_types[typeID];
                    if (type.alphaClass != group) continue;
                    for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                        const uint32_t globalPktIdx = type.firstPacket + pIdx;
                        if (layerForPacket[globalPktIdx] < 0) continue; // texture unavailable; skip
                        s_sortedPacketOrder.push_back(globalPktIdx);
                    }
                }
            }
            if (group == 0u) {
                s_alphaOffCmdCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
            } else {
                s_alphaOnCmdCount = static_cast<uint32_t>(s_sortedPacketOrder.size())
                                  - s_alphaOffCmdCount;
            }
        }

        // --- MaterialGpu-2 sidecar (MC2_MATERIAL_GPU=1 only) ---
        // Runs after s_sortedPacketOrder is populated (skip-filtered, layer >= 0 only).
        // Iterates s_sortedPacketOrder in draw-slot order so that:
        //   s_packetMaterialIdx.size() == s_sortedPacketOrder.size() == PerDrawEntry count
        // This is the v3 invariant: draw slot i reads materials[s_packetMaterialIdx[i]].
        if (s_materialGpuEnabled) {
            s_materialGpuSidecarValid = false;  // MAJ-1: reset before sidecar loop
            s_packetMaterialIdx.clear();
            s_materialGpuTable.clear();
            s_materialInventory.clear();  // V-MATERIAL-STATIC-0: rebuild in lockstep

            std::unordered_map<int64_t, uint32_t> layerToMaterialIdx;
            const uint32_t emittedCount =
                static_cast<uint32_t>(s_sortedPacketOrder.size());

            // Iterate in draw-slot order (same as PerDrawEntry build below).
            // All entries in s_sortedPacketOrder have layerForPacket >= 0 (skip rule above).
            for (uint32_t i = 0; i < emittedCount; ++i) {
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                const int32_t  layer        = layerForPacket[globalPktIdx]; // >= 0 guaranteed

                // COMPRESSION-BC7-STATICPROP-2: layer is BUCKET-relative when the
                // gate is ON, so the same layer index can recur across buckets.
                // Dedup material rows by (bucket,layer) to keep distinct textures
                // distinct. albedoTex still stores `layer` (bucket-relative) so the
                // texArrayLayer==albedoTex compare below stays valid. Gate-OFF
                // (bucketIdx 0) → key == layer → byte-identical dedup.
                const int32_t bucketIdx = s_staticPropBc7Enabled
                    ? s_packetBucketIndex[globalPktIdx] : 0;
                const int64_t matKey =
                    (static_cast<int64_t>(bucketIdx) << 32) | static_cast<uint32_t>(layer);

                auto [it, inserted] = layerToMaterialIdx.try_emplace(
                    matKey, static_cast<uint32_t>(s_materialGpuTable.size()));
                if (inserted) {
                    RenderCore::MaterialGpu m = {};
                    m.albedoTex            = static_cast<uint32_t>(layer);
                    m.normalTex            = RenderCore::kMaterialTexAbsent;
                    m.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
                    m.emissiveTex          = RenderCore::kMaterialTexAbsent;
                    m.flags                = 0;
                    m.baseColorFactor      = 1.0f;   // neutral: full brightness
                    m.metallicFactor       = 0.0f;  // V-MATERIAL-PBR-1: dielectric default
                    // MATERIAL-M0: this is the AUTHORITATIVE roughness default
                    // (the MaterialGpu record producer). The static_prop.frag
                    // fallback and the asset viewer (LocalPbrMaterialBackend.cpp /
                    // MaterialRenderBackend.h) are pinned to MATCH this value.
                    // All three sites must agree. See docs/material-m0-contract.md.
                    m.roughnessFactor      = 1.0f;  // V-MATERIAL-PBR-1: fully rough default (was 0.0)

                    // STATICPROP-MATERIAL-ORM-1 — keep metallicRoughnessTex ==
                    // albedoTex (the SAME bucket-relative layer in the SAME bucket;
                    // the ORM array is a layer-aligned sibling of the albedo array).
                    // Set the kMetallicRoughness flag ONLY when this (bucket,layer)
                    // had a REAL ORM map; otherwise leave it kMaterialTexAbsent so
                    // the shader uses the metallic/roughness factor defaults.
                    if (s_ormSlotsEnabled && layer >= 0 &&
                        bucketIdx >= 0 &&
                        static_cast<size_t>(bucketIdx) < s_ormLayerHasMap.size() &&
                        static_cast<size_t>(layer) < s_ormLayerHasMap[static_cast<size_t>(bucketIdx)].size() &&
                        s_ormLayerHasMap[static_cast<size_t>(bucketIdx)][static_cast<size_t>(layer)]) {
                        m.metallicRoughnessTex = static_cast<uint32_t>(layer);
                        m.flags |= RenderCore::MaterialFlags::kMetallicRoughness;
                    }
                    s_materialGpuTable.push_back(m);

                    // V-MATERIAL-STATIC-0: build a parallel inventory row.
                    // Resolve back to the source TG_TypeShape's listOfTextures
                    // for nodeIdx / human-readable name / dimensions.
                    StaticPropMaterialInventoryEntry inv = {};
                    inv.materialIdx    = static_cast<uint32_t>(s_materialGpuTable.size() - 1);
                    inv.albedoTexLayer = static_cast<uint32_t>(layer);
                    inv.alphaGroup     = (i < s_alphaOffCmdCount) ? 0u : 1u;
                    inv.flags          = m.flags;
                    inv.nodeIdx        = 0xFFFFFFFFu;
                    inv.textureWidth   = 0u;
                    inv.textureHeight  = 0u;
                    inv.usageCount     = 0u;  // accumulated below
                    inv.metallicFactor  = m.metallicFactor;   // V-MATERIAL-PBR-1
                    inv.roughnessFactor = m.roughnessFactor;  // V-MATERIAL-PBR-1
                    std::snprintf(inv.textureName, sizeof(inv.textureName), "(absent)");
                    if (globalPktIdx < s_packets.size()) {
                        const auto& pkt = s_packets[globalPktIdx];
                        if (pkt.owningTypeID < s_types.size()) {
                            const TG_TypeShape* src = s_types[pkt.owningTypeID].source;
                            if (src && src->listOfTextures &&
                                pkt.textureSlot < src->numTextures) {
                                const DWORD nodeIdx =
                                    src->listOfTextures[pkt.textureSlot].mcTextureNodeIndex;
                                inv.nodeIdx = static_cast<uint32_t>(nodeIdx);
                                if (mcTextureManager && nodeIdx != 0xFFFFFFFFu) {
                                    const char* nm = mcTextureManager->getTextureName(nodeIdx);
                                    if (nm && *nm) {
                                        std::snprintf(inv.textureName,
                                                      sizeof(inv.textureName),
                                                      "%s", nm);
                                    } else {
                                        std::snprintf(inv.textureName,
                                                      sizeof(inv.textureName),
                                                      "(unnamed)");
                                    }
                                    const DWORD w = mcTextureManager->getWidth(nodeIdx);
                                    if (w != 0xFFFFFFFFu) {
                                        inv.textureWidth  = static_cast<uint32_t>(w);
                                        inv.textureHeight = static_cast<uint32_t>(w);
                                    }
                                }
                            }
                        }
                    }
                    inv.placeholder = (inv.nodeIdx == 0xFFFFFFFFu)
                                    || (inv.textureName[0] == '(')
                                    || (inv.textureWidth == 0u);
                    s_materialInventory.push_back(inv);
                }
                // Per-draw-slot usage counter accumulates on every reference (insert OR hit).
                if (it->second < s_materialInventory.size())
                    s_materialInventory[it->second].usageCount += 1u;
                s_packetMaterialIdx.push_back(it->second);
            }

            // --- Upload ---
            // GL_STATIC_DRAW: table is mission/map lifetime, not per-frame.
            // Guard byteSize > 0: some drivers misbehave on a zero-byte glBufferData.
            // When no static props exist, s_materialGpuSsbo stays 0 and flush() skips the bind.
            const size_t byteSize =
                s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);

            if (byteSize > 0) {
                // Idempotent: delete any buffer from a prior finalizeGeometry call.
                // finalizeGeometry() should be once-per-map, but defensive cleanup
                // prevents leaks if it is ever called again (partial reinit, hot-reload, etc.).
                if (s_materialGpuSsbo.glName != 0) {
                    GLuint local = static_cast<GLuint>(s_materialGpuSsbo.glName);
                    glDeleteBuffers(1, &local);
                    s_materialGpuSsbo.glName = 0;
                }
                while (glGetError() != GL_NO_ERROR) {}  // drain stale BEFORE operation
                {
                    GLuint local = 0;
                    glGenBuffers(1, &local);
                    s_materialGpuSsbo.glName = static_cast<uint32_t>(local);
                }
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_materialGpuSsbo.glName));
                MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(byteSize),
                             s_materialGpuTable.data(),
                             GL_STATIC_DRAW);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                const GLenum uploadErr = glGetError();  // sample THIS operation AFTER
                if (uploadErr != GL_NO_ERROR) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "[MATERIAL_GPU v4] GL ERROR after upload: 0x%x — buffer deleted, sampling disabled\n",
                                  uploadErr);
                    std::fputs(buf, stderr);
                    // Delete the corrupt/incomplete buffer so s_materialGpuSsbo returns to 0.
                    // sampleOn checks s_materialGpuSsbo.glName != 0, so this guarantees sampling is
                    // disabled without needing a separate s_materialGpuSidecarValid invalidation.
                    GLuint local = static_cast<GLuint>(s_materialGpuSsbo.glName);
                    glDeleteBuffers(1, &local);
                    s_materialGpuSsbo.glName = 0;

                    // STATICPROP-MATERIAL-SSBO-OWNER-1: corrupt buffer dropped — keep slot unregistered.
                    RenderCore::RenderResourceDesc invalid;
                    invalid.id    = RenderCore::RenderResourceId::StaticPropMaterialGpuBuffer;
                    invalid.valid = false;
                    RenderCore::registerOrUpdateRenderResource(invalid);
                } else {
                    // STATICPROP-MATERIAL-SSBO-OWNER-1: register the live material SSBO
                    // (observe-only metadata; never read by the draw path). Registered at
                    // create, since the byte size is known once the table is uploaded.
                    RenderCore::RenderResourceDesc d;
                    d.id        = s_materialGpuSsbo.id;
                    d.kind      = RenderCore::RenderResourceKind::Buffer;
                    d.lifetime  = s_materialGpuSsbo.lifetime;
                    d.format    = RenderCore::RenderResourceFormat::BufferRaw;
                    d.debugName = s_materialGpuSsbo.debugName;
                    d.glName    = s_materialGpuSsbo.glName;
                    d.sizeBytes = static_cast<uint64_t>(byteSize);
                    d.valid     = true;
                    RenderCore::registerOrUpdateRenderResource(d);
                }
            }
            // s_materialGpuSsbo remains 0 when byteSize == 0.

            // --- Log upload ---
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=table_upload"
                              " materials=%zu bytes=%zu emitted=%u\n",
                              s_materialGpuTable.size(), byteSize, emittedCount);
                std::fputs(buf, stderr);
            }

            // --- Debug compare ---
            // Load-bearing invariant check: MaterialGpu[materialIdx].albedoTex == texArrayLayer.
            // texArrayLayer is the compare authority until it is deliberately retired.
            // mismatches > 0 means the MaterialGpu table has diverged from the legacy path.
            // This check runs by default (s_materialGpuEnabled is default-ON).
            int mismatches = 0;
            for (uint32_t i = 0; i < emittedCount; ++i) {
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                const uint32_t idx          = s_packetMaterialIdx[i];
                const uint32_t albedo       = s_materialGpuTable[idx].albedoTex;
                const uint32_t expected     =
                    static_cast<uint32_t>(layerForPacket[globalPktIdx]);
                if (albedo != expected) {
                    if (mismatches < 10) {  // first-10-only: throttle per-slot noise
                        char buf[128];
                        std::snprintf(buf, sizeof(buf),
                                      "[MATERIAL_GPU v4] MISMATCH slot=%u pkt=%u"
                                      " materialIdx=%u albedoTex=%u expected=%u\n",
                                      i, globalPktIdx, idx, albedo, expected);
                        std::fputs(buf, stderr);
                    }
                    ++mismatches;
                }
            }
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=compare emitted=%u mismatches=%d\n",
                              emittedCount, mismatches);
                std::fputs(buf, stderr);
            }

            // C1 fix: record whether the sidecar is aligned with the emitted draw count.
            // This flag gates sampling in flush(). If sizes diverge, sampling is
            // disabled for the whole pass — legacy texArrayLayer is the fallback.
            s_materialGpuSidecarValid =
                (s_packetMaterialIdx.size() == s_sortedPacketOrder.size());

            if (!s_materialGpuSidecarValid) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] ERROR materialIdx sidecar size mismatch"
                              " emitted=%zu sidecar=%zu sample_forced=0\n",
                              s_sortedPacketOrder.size(), s_packetMaterialIdx.size());
                std::fputs(buf, stderr);
            }
        } // end s_materialGpuEnabled (sidecar)

        // MC2_EDITOR_STATIC_PRIME_DIAG=1 — compact aggregate DrawCmd census at
        // finalize. Counts types that carry geometry vs types that ended up with
        // a live DrawCmd vs types whose every packet was dropped at the
        // texture-unavailable skip rule (no layer -> no DrawCmd -> invisible).
        // Aggregate-only; bounded; behind the env gate.
        static const bool s_editorStaticPrimeDiag =
            (getenv("MC2_EDITOR_STATIC_PRIME_DIAG") != nullptr);
        if (s_editorStaticPrimeDiag) {
            // O(1) presence lookup: mark each globalPktIdx that made it into
            // s_sortedPacketOrder. Built once before the type loop.
            std::vector<char> inSorted(s_packets.size(), 0);
            for (uint32_t s = 0; s < s_sortedPacketOrder.size(); ++s) {
                const uint32_t gp = s_sortedPacketOrder[s];
                if (gp < inSorted.size()) inSorted[gp] = 1;
            }

            uint32_t typesWithGeom = 0;
            uint32_t typesWithDrawCmd = 0;
            uint32_t typesDroppedNoLayer = 0;
            for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
                const auto& type = s_types[typeID];
                if (type.packetCount == 0u) continue;
                ++typesWithGeom;

                int hasDrawCmd = 0;
                for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                    const uint32_t globalPktIdx = type.firstPacket + pIdx;
                    if (globalPktIdx < inSorted.size() && inSorted[globalPktIdx]) {
                        hasDrawCmd = 1;
                        break;
                    }
                }

                if (hasDrawCmd) ++typesWithDrawCmd;
                else            ++typesDroppedNoLayer;
            }
            std::fprintf(stderr,
                "[EDITOR_STATIC_PRIME] typesWithGeom=%u typesWithDrawCmd=%u "
                "typesDroppedNoLayer=%u alphaOffCmd=%u alphaOnCmd=%u\n",
                typesWithGeom, typesWithDrawCmd, typesDroppedNoLayer,
                s_alphaOffCmdCount, s_alphaOnCmdCount);
            std::fflush(stderr);
        }

        std::vector<PerDrawEntry> entries(s_sortedPacketOrder.size());
        std::vector<uint32_t> cmdToBucket(s_sortedPacketOrder.size());
        for (uint32_t i = 0; i < s_sortedPacketOrder.size(); ++i) {
            const uint32_t globalPktIdx = s_sortedPacketOrder[i];
            const auto& pkt = s_packets[globalPktIdx];
            const uint32_t typeID = pkt.owningTypeID;
            cmdToBucket[i] = typeID;
            const auto& type = s_types[typeID];
            PerDrawEntry e{};
            if (layerForPacket[globalPktIdx] >= 0) {
                e.packetID = static_cast<int32_t>(globalPktIdx);
                // Per-packet alpha-test bit: use the packet's own
                // materialFlags from registerType. This is the load-bearing
                // fix for fence-fall / multi-textured buildings — each
                // packet's own alpha bit drives the fragment-shader discard.
                bool packetAlpha =
                    (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
                if (!packetAlpha && type.source && type.source->listOfTextures &&
                    pkt.textureSlot < type.source->numTextures &&
                    type.source->listOfTextures[pkt.textureSlot].textureAlpha) {
                    packetAlpha = true;
                }
                e.materialFlags    = packetAlpha
                    ? static_cast<int32_t>(STATIC_PROP_FLAG_ALPHA_TEST) : 0;
                e.maxLocalVertexID = (type.vertexCount > 0u)
                    ? static_cast<int32_t>(type.vertexCount - 1u) : 0;
                e.texArrayLayer    = layerForPacket[globalPktIdx];
                e.uvScaleX         = uvScaleXByPacket[globalPktIdx];
                e.uvScaleY         = uvScaleYByPacket[globalPktIdx];
                // M1.5 C1 fix: three-owner chain.
                //   1. Registry resolves typeID -> recipeIndex (-1 on miss).
                //   2. RenderWorld encodes recipeIndex -> handle bits (0 if invalid).
                //   3. Batcher writes objectIdRaw.
                // Env-OFF behavior: writes nonzero bits, but shader does not declare
                // out uint (Task 8 controls the macro), so writes are invisible.
                // Cheaper than a branch per entry.
                const int32_t m1_5_recipeIndex =
                    GpuStaticPropRegistry::getRecipeIndexForType(typeID);
                e.objectIdRaw =
                    static_cast<int32_t>(RenderWorld::objectIdRawForStaticPropRecipe(m1_5_recipeIndex));

                // MaterialGpu-3: fill materialIdx from v2 sidecar.
                // Guard: sidecar valid means s_packetMaterialIdx.size() == entries.size(),
                // so index i is in bounds.
                if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
                    e.materialIdx = s_packetMaterialIdx[i];
                } else {
                    e.materialIdx = 0u;
                }
                // v1.1: cache primary material per typeID for extraction snapshot.
                // First-time-wins with prefer-alpha-off; s_sortedPacketOrder is alpha-off-first
                // so alpha-off primaries are naturally seen before alpha-on ones.
                {
                    const bool wasAlphaOn = (i >= s_alphaOffCmdCount);
                    const bool hasMat     = (s_materialGpuEnabled && s_materialGpuSidecarValid);
                    GpuStaticPropRegistry::staticPropCacheTypePrimaryMaterial(
                        typeID,
                        e.texArrayLayer,
                        hasMat ? e.materialIdx : 0xFFFFFFFFu,
                        hasMat,
                        wasAlphaOn,
                        type.packetCount > 1u,
                        type.alphaClass,          // v2: 0=alpha-off, 1=alpha-on
                        type.packetCount,         // v2: total packet count for this type
                        type.firstPacket);        // v2: global index of first packet
                }
            }   // end if (layerForPacket[globalPktIdx] >= 0)
            entries[i] = e;
        }

        // MAJ-1: cross-system check — verify entries[i].materialIdx maps to a table entry
        // whose albedoTex equals layerForPacket[s_sortedPacketOrder[i]].
        // This is a non-tautological end-to-end check (PerDrawEntry -> MaterialTable -> layer).
        if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
            int entryMismatches = 0;
            const uint32_t emittedCount2 = static_cast<uint32_t>(s_sortedPacketOrder.size());
            for (uint32_t i = 0; i < emittedCount2; ++i) {
                const uint32_t globalPktIdx  = s_sortedPacketOrder[i];
                const uint32_t expectedAlbedo = static_cast<uint32_t>(layerForPacket[globalPktIdx]);
                const uint32_t mIdx          = entries[i].materialIdx;
                const uint32_t actualAlbedo  = (mIdx < static_cast<uint32_t>(s_materialGpuTable.size()))
                    ? s_materialGpuTable[mIdx].albedoTex
                    : 0xFFFFFFFFu;
                if (actualAlbedo != expectedAlbedo) {
                    ++entryMismatches;
                }
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "[MATERIAL_GPU v4] event=entry_material_idx emitted=%u mismatches=%d\n",
                          emittedCount2, entryMismatches);
            std::fputs(buf, stderr);
        }

        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &s_perDrawSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perDrawSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(entries.size() * sizeof(PerDrawEntry)),
                     entries.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // Cmd-to-bucket SSBO (binding 7 in patch shader): each cmd index
        // maps to its parent typeID so the patch shader can write the
        // type's instanceCount into all cmds (= all packets) of that type.
        if (s_cmdToBucketSsbo == 0) {
            // TIER2-EXCLUDED: dead-path
            glGenBuffers(1, &s_cmdToBucketSsbo);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_cmdToBucketSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(cmdToBucket.size() * sizeof(uint32_t)),
                     cmdToBucket.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // v1.1 material cache summary (always logged at finalize time).
    {
        GpuStaticPropRegistry::MaterialCacheStats mcs{};
        GpuStaticPropRegistry::staticPropGetMaterialCacheStats(&mcs);
        std::fprintf(stderr,
            "[GPUPROPS] material_cache cache_slots=%u tex_wired=%u mat_wired=%u "
            "multi_packet=%u alpha_on_fallback=%u no_primary=%u\n",
            mcs.cacheVectorSize, mcs.texWired, mcs.matWired,
            mcs.multiPacket, mcs.alphaOnFallback, mcs.noPrimary);
    }

    // Slice 1 step group 3 — allocate base-instance table under !s_globalPoolLegacy.
    // Must come after the per-packet sort (lines above) so s_alphaOffCmdCount /
    // s_alphaOnCmdCount are valid. One uint32_t per draw command per ring frame.
    if (!s_globalPoolLegacy) {
        const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;
        // SSBO-BIND-ALIGN: this per-frame byte count is used as a ring-slot
        // glBindBufferRange offset (slot * stride) at
        // batcher_bindBaseInstanceByCmdSsboForPatch. totalCmds*4 is not generally
        // a multiple of GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, so slots 1+ would
        // misalign and the bind would fail on NVIDIA (GL_INVALID_VALUE). Pad the
        // per-frame stride; alloc (RING_FRAMES*stride), CPU write (slot*stride) and
        // bind all read this same value so they stay consistent.
        s_baseInstanceByCmdBytesPerFrame = static_cast<size_t>(gpuAlignUp(
            static_cast<unsigned long long>(totalCmds) * sizeof(uint32_t),
            static_cast<unsigned long long>(gpuSsboOffsetAlignment())));
        const size_t totalBytes =
            static_cast<size_t>(RING_FRAMES) * s_baseInstanceByCmdBytesPerFrame;
        if (totalBytes > 0) {
            // TIER2-EXCLUDED: dead-path
            glGenBuffers(1, &s_baseInstanceByCmdSsbo);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_baseInstanceByCmdSsbo);
            const GLbitfield mapFlags =
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            while (glGetError() != GL_NO_ERROR) {}
            glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, mapFlags);
            const GLenum storageErr = glGetError();
            if (storageErr != GL_NO_ERROR) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glDeleteBuffers(1, &s_baseInstanceByCmdSsbo);
                s_baseInstanceByCmdSsbo          = 0;
                s_baseInstanceByCmdBytesPerFrame = 0;
                std::fprintf(stderr,
                    "[COALESCE v1] event=disarmed reason=alloc_failed "
                    "(baseInstanceByCmdSsbo glBufferStorage err=0x%04x bytes=%zu)\n",
                    storageErr, totalBytes);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }
            s_baseInstanceByCmdMap = MC2_GL_MapBufferRange(
                GL_SHADER_STORAGE_BUFFER, 0, totalBytes, mapFlags);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            if (!s_baseInstanceByCmdMap) {
                std::fprintf(stderr,
                    "[COALESCE v1] event=disarmed reason=alloc_failed "
                    "(baseInstanceByCmdSsbo glMapBufferRange returned null)\n");
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }
            std::fprintf(stderr,
                "[COALESCE v1] event=baseInstance_buffer_ok cmds=%u "
                "bytes_per_frame=%zu total_bytes=%zu\n",
                totalCmds, s_baseInstanceByCmdBytesPerFrame, totalBytes);
        }
    }

    // Step 5.12 — sort permutation overwrite (LAST step on success). The
    // identity from Step 5.4 stays if any earlier step returned.
    {
        std::vector<uint32_t> permutation(typeCount, 0u);
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            permutation[typeID] = i;  // typeID → sortedSlot
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        static_cast<GLsizeiptr>(permutation.size() * sizeof(uint32_t)),
                        permutation.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Step 5.13 — set armed flags + log event=armed with elapsed_ms.
    s_coalesceEnabled = true;
    s_coalesceArmed   = true;
    {
        const auto coalesceEnd = std::chrono::steady_clock::now();
        const long long elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                coalesceEnd - coalesceStart).count();
        // unique_tex_off / unique_tex_on counts: peak texture-array layer
        // count per group (≤ count of types in that group; equal when
        // every type has a unique texture). Computed by scanning
        // layerForType against s_sortedTypeOrder.
        uint32_t uniqueTexOff = 0, uniqueTexOn = 0;
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            const auto& t = s_types[typeID];
            if (layerForType[typeID] < 0) continue;
            const uint32_t layer = static_cast<uint32_t>(layerForType[typeID]);
            if (t.alphaClass == 0u) {
                if (layer + 1u > uniqueTexOff) uniqueTexOff = layer + 1u;
            } else {
                if (layer + 1u > uniqueTexOn)  uniqueTexOn  = layer + 1u;
            }
        }
        std::fprintf(stderr, "[COALESCE v1] event=armed types=%u "
                     "off_types=%u on_types=%u unique_tex_off=%u unique_tex_on=%u "
                     "per_frame_inst_bytes=%zu elapsed_ms=%lld\n",
                     typeCount,
                     s_alphaOffCount, s_alphaOnCount,
                     uniqueTexOff, uniqueTexOn,
                     s_coalescePerFrameInstanceBytes,
                     elapsedMs);
    }
}

bool GpuStaticPropBatcher::submit(TG_Shape* shape,
                                  const Stuff::Matrix4D& shapeToWorld,
                                  uint32_t highlightARGB,
                                  uint32_t fogARGB,
                                  uint32_t flags,
                                  uint32_t lightDataIndex) {
    if (!shape || s_fatalRegistrationFailure) return false;
    if (s_programLoadFailed) return false;

    // TG_Shape::myType is a TG_TypeNodePtr; for SHAPE_NODE leaves it's a TG_TypeShape.
    TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);
    if (!typeShape) return false;

    auto it = s_typeIndex.find(typeShape);
    if (it == s_typeIndex.end()) {
        if (!s_failedTypes[typeShape]) {
            std::fprintf(stderr, "[GPUPROPS] unregistered type %p for shape %p -- "
                         "caller must CPU-fallback\n", (void*)typeShape, (void*)shape);
            s_failedTypes[typeShape] = true;
        }
        return false;  // Layer B: caller calls shape->Render() on false.
    }

    const uint32_t typeID = it->second;
    const GpuStaticPropType& type = s_types[typeID];
    PerTypeBucket& bucket = s_bucketsByType[typeID];

    // 2026-05-10 diag: per-typeID submit() histogram (H1 of substrate-coalesce
    // detail-pass plan). Init-once env probe + atexit registration; tally on
    // every leaf that reached this point with a registered typeID; dump once
    // after a useful sample window AND at process exit (whichever fires first).
    if (!s_submitTypeHistInit) {
        s_submitTypeHistInit    = true;
        s_submitTypeHistEnabled = (getenv("MC2_SUBMIT_TYPEHIST") != nullptr);
        if (s_submitTypeHistEnabled) atexit(emitSubmitTypeHistAtExit);
    }
    if (s_submitTypeHistEnabled) {
        if (typeID < s_submitTypeHist.size()) ++s_submitTypeHist[typeID];
        ++s_submitTypeHistCalls;
        // Empirically (mc2_10 substrate=ON sweep 2026-05-10):
        // submitted_children≈40/frame avg → ~24K calls/600 frames. The full
        // 30s 360° orbit is ~1800 frames ≈ 72K calls; we sample in three
        // windows so coverage growth across the orbit is visible (different
        // buildings revealed at different camera angles). atexit is suppressed
        // by quick_sweep's `taskkill /F`, so threshold dumps are load-bearing.
        if (s_submitTypeHistCalls == 20000  ||
            s_submitTypeHistCalls == 50000  ||
            s_submitTypeHistCalls == 100000) {
            emitSubmitTypeHistDump("threshold");
        }
    }

    // firstColorOffset is the index into the bucket's color array:
    // instance K's colors start at K * type.vertexCount (= bucket.colors.size()
    // BEFORE this push). The shader binds the bucket's color range, so this
    // becomes an index relative to the bound range.
    const uint32_t firstColorOffset =
        static_cast<uint32_t>(bucket.colors.size());

    GpuStaticPropInstance inst{};
    // Matrix4D is a plain row-major Scalar[16] (see stuff/matrix.hpp). Copy
    // as-is; shader uploads the worldToClip uniform with GL_FALSE.
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    inst.typeID           = typeID;
    inst.firstColorOffset = firstColorOffset;
    inst.flags            = flags;
    // Slice 2 (object-offload) — Stage 2.C: per-actor dedup-cache index
    // into the LightsData[32] UBO. Same value for every leaf of one
    // multishape (caller hoisted the gather between the two loops).
    inst.lightDataIndex   = lightDataIndex;
    inst.aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    inst.aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    inst.aRGBHighlight[2] = ((highlightARGB >>  0) & 0xFF) / 255.0f;
    inst.aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    inst.fogRGB[0] = ((fogARGB >> 16) & 0xFF) / 255.0f;
    inst.fogRGB[1] = ((fogARGB >>  8) & 0xFF) / 255.0f;
    inst.fogRGB[2] = ((fogARGB >>  0) & 0xFF) / 255.0f;
    inst.fogRGB[3] = ((fogARGB >> 24) & 0xFF) / 255.0f;
    {
        // MC2_TREE_DIAG_TRACE diagnostic. See macro definition near line ~50.
        static int s_dynPrinted = 0;
        if (s_treeDiagTrace && s_dynPrinted < 8) {
            ++s_dynPrinted;
            TREE_DIAG("path=dyn typeID=%u flags=0x%x lightIdx=%u colOff=%u tx=%.2f ty=%.2f tz=%.2f fog=%.3f,%.3f,%.3f,%.3f hi=%.3f,%.3f,%.3f,%.3f bucketInsts=%zu",
                inst.typeID, inst.flags, inst.lightDataIndex, inst.firstColorOffset,
                // Stuff::Matrix4D translation lives at (3,0)/(3,1)/(3,2) which the
                // operator()(row,col) maps to entries[(col<<2)+row] = [3]/[7]/[11].
                // The W-column is at [12]/[13]/[14] which is always (0,0,0) for
                // affine matrices — wrong field. Verified via mclib/stuff/matrix.cpp:214 BuildTranslation.
                inst.modelMatrix[3], inst.modelMatrix[7], inst.modelMatrix[11],
                inst.fogRGB[0], inst.fogRGB[1], inst.fogRGB[2], inst.fogRGB[3],
                inst.aRGBHighlight[0], inst.aRGBHighlight[1], inst.aRGBHighlight[2], inst.aRGBHighlight[3],
                bucket.instances.size());
        }
    }
    if (s_bucketOrderTrace) { if (!bucket.dbgFirst) bucket.dbgFirst = 2; ++bucket.dbgDynamic; }
    bucket.instances.push_back(inst);
    s_lastBuiltBatch.push_back(inst);  // Stage 3.C: batch accumulator

    // 2026-05-10: dynamic-path substrate sync. Without this, an actor whose
    // appearance class lacks a registerStatic override (notably
    // GenericAppearance — warehouses, S_admin, watchtowers, control buildings)
    // populates bucket.instances every frame via submitMultiShape but never
    // appears in the substrate's GpuActorRecord stream. The compute cull then
    // writes 0 to bucketCountData[typeID] for those types, and the coalesce
    // multi-draw uses that GPU-authoritative count → renders zero instances of
    // those types even though the bucket has CPU data. The legacy per-bucket
    // draw branch (substrate killswitch) is unaffected because it draws
    // bucket.instances.size() directly and does not consult bucketCountData.
    //
    // Mirrors GpuStaticPropRegistry::flush()'s substrate append: same
    // worldCenter (Stuff/MLR → raw MC2 axis swap), same boundingRadius=200,
    // same category encoding (Cat_StaticProp + typeID<<4). Static-registry
    // and dynamic-submit paths now produce equivalent substrate records.
    if (gpu_cull::substrate_isEnabled()) {
        gpu_cull::GpuActorRecord rec{};
        // 2026-05-10 actor-center fix: every leaf of one multishape shares
        // the SAME substrate worldCenter so the GPU frustum sphere-test
        // accepts/rejects all leaves together (one cull unit per actor).
        // The first submit() call in a submitMultiShape pass captures the
        // current leaf's translation as the actor center; subsequent calls
        // reuse it. This matches registry::flush's per-range parent-center
        // (the first leaf's modelMatrix is the actor's xlatPosition because
        // the root leaf's local-to-actor transform is identity for stock
        // ASE buildings, trees, and animated buildings alike).
        if (!s_currentSubmittingActorCenterValid) {
            s_currentSubmittingActorCenter[0] = -inst.modelMatrix[3];   // raw.x = -stuff.x
            s_currentSubmittingActorCenter[1] =  inst.modelMatrix[11];  // raw.y =  stuff.z
            s_currentSubmittingActorCenter[2] =  inst.modelMatrix[7];   // raw.z =  stuff.y (elev)
            s_currentSubmittingActorCenterValid = true;
        }
        rec.worldCenter[0]  = s_currentSubmittingActorCenter[0];
        rec.worldCenter[1]  = s_currentSubmittingActorCenter[1];
        rec.worldCenter[2]  = s_currentSubmittingActorCenter[2];
        rec.boundingRadius  = 200.0f;
        rec.worldAabbMin[0] = rec.worldCenter[0] - rec.boundingRadius;
        rec.worldAabbMin[1] = rec.worldCenter[1] - rec.boundingRadius;
        rec.worldAabbMin[2] = rec.worldCenter[2] - rec.boundingRadius;
        rec.worldAabbMax[0] = rec.worldCenter[0] + rec.boundingRadius;
        rec.worldAabbMax[1] = rec.worldCenter[1] + rec.boundingRadius;
        rec.worldAabbMax[2] = rec.worldCenter[2] + rec.boundingRadius;
        rec.category        = (static_cast<uint32_t>(typeID) << 4)
                            | static_cast<uint32_t>(gpu_cull::Cat_StaticProp);
        // 2026-05-10 diag: MC2_STATIC_FORCE_ADMIT mirrors registry::flush
        // behavior so the env-var A/B (cull-bypassed vs cull-active) covers
        // BOTH substrate-record producers — otherwise dynamic-path leaves
        // (GenericAppearance buildings post-b2fb1cc) stay subject to cull
        // even when FORCE_ADMIT is on, masking the diagnostic signal.
        static const bool s_diag_forceAdmit =
            (getenv("MC2_STATIC_FORCE_ADMIT") != nullptr);
        rec.flags           = s_diag_forceAdmit
                              ? static_cast<uint32_t>(gpu_cull::Flag_AlwaysVisible)
                              : gpu_cull::Flag_None;
        rec.actorId         = 0u;
        rec.prevVisibilityBit = 1u;
        rec.consumerFlags   = 0u;
        // C1b temporal-superset Slice 1: real terrain block index so the
        // block rollup can stamp the right block. worldCenter[0] is raw-MC2
        // east, [1] raw-MC2 north (the -inst.modelMatrix[3] unswap above
        // produced the east-frame). Feed [0],[1] ONLY; NEVER [2] (elev).
        // worldCenter fully populated above (rec.worldCenter[0..2]).
        rec.blockIdx        = static_cast<uint32_t>(
            Terrain::worldToBlockIdx(rec.worldCenter[0],
                                     rec.worldCenter[1]));
        // [BLKIDX v1] env-gated GEOMETRIC probe (demote-not-delete).
        {
            static const bool s_blkidxTrace =
                (getenv("MC2_BLKIDX_TRACE") != nullptr);
            if (s_blkidxTrace) {
                const float pwx = rec.worldCenter[0];
                const float pwy = rec.worldCenter[1];
                long mx = ((long)pwx >> 7) + Terrain::halfVerticesMapSide;
                long bx = (long)(mx * Terrain::oneOverVerticesBlockSide);
                long my = Terrain::halfVerticesMapSide -
                          (((long)pwy >> 7) + 1);
                long by = (long)(my * Terrain::oneOverVerticesBlockSide);
                long cpuBlk = bx + (by * Terrain::blocksMapSide);
                long helperBlk = Terrain::worldToBlockIdx(pwx, pwy);
                fprintf(stderr,
                    "[BLKIDX v1] event=geom_check src=batcher"
                    " wx=%.1f wy=%.1f wz=%.1f helper=%ld cpu=%ld"
                    " match=%d\n",
                    pwx, pwy, rec.worldCenter[2],
                    helperBlk, cpuBlk, (helperBlk == cpuBlk) ? 1 : 0);
                fprintf(stderr,
                    "[BLKIDX v1] event=zero_verify src=batcher"
                    " blockIdx=%u z_excluded=1\n",
                    rec.blockIdx);
                fflush(stderr);
            }
        }
        gpu_cull::substrate_appendStaticPropRecord(rec);
    }

    // Stage 2.D.2 — dual-emit snapshot collection.
    // When the latch is Armed, capture per-vertex lit ARGB in triangle-soup
    // expanded order (matching gl_VertexID in static_prop.vert). The shader
    // writes parityOut_[gl_InstanceID * parityVerts + gl_VertexID] where
    // gl_VertexID is the expanded VBO index (0 .. numTris*3-1). We build the
    // snapshot in the same order: corner (j,c) → index 3*j+c.
    // Note: typeShape->numTypeTriangles is the TOTAL triangle count across all
    // packets (same as the total triangles emitted into the VBO by registerType).
    //
    // Source: listOfVertices[typeTriangles[j].Vertices[c]].argb — NOT
    //   listOfTriangles[j].aRGBLight[c]. The face-loop only writes
    //   aRGBLight[c] for FRONT-FACING triangles; back-facing positions
    //   retain stale pool memory from previous frames (not zeroed on alloc).
    //   listOfVertices[v].argb is always written by the vertex lighting loop
    //   for every vertex (front AND back facing), making it the correct
    //   per-vertex ground truth. Both values are identical for front-facing
    //   vertices when useFaceLighting=false (stock mc2_01 condition).
    // Stage 2.D.3: per-actor narrowing of snapshot capture. The mclib gate
    // sites only fire the full TransformMultiShape (CPU lighting bake) for
    // the sampled actor in SampledOnly arm mode — non-sampled actors have
    // stale listOfVertices[].argb. Capturing their bytes would corrupt the
    // snapshot map. Use the per-actor gate here too: when the enclosing
    // multishape (s_currentSubmittingMulti, set by submitMultiShape) doesn't
    // match the sampler's pick, skip the snapshot. In bootstrap arm (state=
    // Armed + armMode=All) IsDualEmitArmedForActor returns true for every
    // shape, so 2.D.2 baseline coverage is preserved.
    if (gos_object_parity::IsDualEmitArmedForActor(s_currentSubmittingMulti) &&
        shape->listOfVertices &&
        typeShape->listOfTypeTriangles &&
        typeShape->numTypeTriangles > 0) {
        const DWORD numTris = typeShape->numTypeTriangles;
        const uint32_t expandedVerts = numTris * 3u;
        std::vector<uint32_t> perVertexARGB(expandedVerts, 0u);
        // Stage 2.D.2 diagnostic: one-shot dump of original aRGBLight tags vs
        // computed listOfVertices[vi].argb for first instance of typeID=82.
        // Controlled by MC2_OBJECT_PARITY_TRACE env var.
        static bool s_parity82Printed = false;
        const bool doTrace82 = (typeID == 82 && !s_parity82Printed &&
                                gos_object_parity::IsParityTraceEnabled());
        if (doTrace82) s_parity82Printed = true;
        for (DWORD j = 0; j < numTris; ++j) {
            for (int c = 0; c < 3; ++c) {
                const DWORD vi = typeShape->listOfTypeTriangles[j].Vertices[c];
                perVertexARGB[j * 3u + c] = shape->listOfVertices[vi].argb;
                if (doTrace82 && (j * 3u + c) < 20u) {
                    std::fprintf(stderr,
                        "[PARITY_DIAG v1] typeId=82 inst=first vert=%u "
                        "aRGBLight_tag=0x%08X computed_argb=0x%08X\n",
                        (unsigned)(j * 3u + c),
                        (unsigned)typeShape->listOfTypeVertices[vi].aRGBLight,
                        (unsigned)shape->listOfVertices[vi].argb);
                    std::fflush(stderr);
                }
            }
        }
        // Stage 2.D.3: pass explicit instanceIdx = bucket position this leaf
        // just took. bucket.instances.push_back ran above, so size()-1 is
        // the just-pushed entry's index. Required so SampledOnly mode can
        // address the correct GPU slot in CompareAndReport (the sampled
        // leaf might not be at instance 0 — there are typically many
        // unrelated submissions of the same type per frame).
        const uint32_t instanceIdx =
            static_cast<uint32_t>(bucket.instances.size() - 1u);
        gos_object_parity::RecordInstanceSnapshot(typeID,
                                                   instanceIdx,
                                                   perVertexARGB.data(),
                                                   expandedVerts);
    }

    // Append this instance's per-vertex ARGB block.
    // IMPORTANT: listOfColors (TG_Vertex: fog+redSpec+greenSpec+blueSpec) is
    // specular-only and is zero for most buildings — reading it produces
    // black. The real per-vertex lit ARGB is computed by TransformShape
    // into `listOfVertices[j].argb` (mclib/tgl.cpp:2119). gos_VERTEX is
    // 32 bytes with `argb` at offset 16.
    const uint32_t numColors = type.vertexCount;
    if (numColors > 0 && shape->listOfVertices) {
        bucket.colors.reserve(bucket.colors.size() + numColors);
        const gos_VERTEX* src = shape->listOfVertices;
        for (uint32_t v = 0; v < numColors; ++v) {
            bucket.colors.push_back(src[v].argb);
        }
    } else {
        // No source vertices -- pad with zeros so the color block still
        // matches type.vertexCount and indexing math stays valid.
        bucket.colors.insert(bucket.colors.end(), numColors, 0u);
    }

    return true;
}

void GpuStaticPropBatcher::recordEligibleActor(GpuStaticPropPopulation pop) {
    initTraceOnce();
    s_counters.eligible_actors_by_pop[popIndex(pop)]++;
}

void GpuStaticPropBatcher::recordCpuFallback(GpuStaticPropPopulation pop) {
    s_counters.cpu_fallback_by_pop[popIndex(pop)]++;
}

bool GpuStaticPropBatcher::submitMultiShape(TG_MultiShape* multi,
                                            GpuStaticPropPopulation pop,
                                            const char* callerName) {
    initTraceOnce();
    s_lastBuiltBatch.clear();  // Stage 3.C: reset per-call accumulator
    // Clear the late-reg signal at the top of every call so a stale "true"
    // from a prior submitMultiShape never masquerades as a signal for this one.
    s_lastSubmitWasLateReg = false;
    // Stage 2.D.3: track the currently-submitting multishape so per-leaf
    // submit() calls can compare against the sampler's pick. Cleared at every
    // exit path of this function to keep the window tight.
    s_currentSubmittingMulti = multi;
    // 2026-05-10 actor-center fix: cleared per-call so a stale value from a
    // prior submitMultiShape can't leak into a multishape that has zero valid
    // SHAPE_NODE leaves. Populated below when firstShapeNodeLeaf is captured.
    s_currentSubmittingActorCenterValid = false;
    s_currentSubmittingActorCenter[0]   = 0.0f;
    s_currentSubmittingActorCenter[1]   = 0.0f;
    s_currentSubmittingActorCenter[2]   = 0.0f;
    // pop consumed by counters below.
    if (!multi || s_fatalRegistrationFailure) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }
    if (s_programLoadFailed || s_staticPropProgram == 0) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    // Skip-child-not-fail-multishape policy. The CPU path (TG_Shape::Render
    // in tgl.cpp ~2530) silently returns for any of:
    //   - helper/bone node (numVertices == 0; GetNodeType() != SHAPE_NODE)
    //   - listOfVertices == NULL (TransformShape early-out for
    //       isSpotlight && !isNight, tgl.cpp ~1657)
    //   - listOfColors == NULL (shape not yet transformed this frame)
    //   - lastTurnTransformed != turn (stale)
    // Previously we failed the entire multishape on any of these, which
    // cascaded one helper/spotlight child into a full CPU fallback of every
    // building. Now we skip only the ineligible children and submit the
    // rest. Only fail the whole multishape if a SHAPE_NODE child has an
    // unregistered type — that's a plumbing problem that invalidates
    // self-consistency.

    // First pass: check for unregistered SHAPE_NODE types (fatal for this
    // multishape). All other ineligibility is handled in the submit pass.
    // Slice 2 (object-offload) — Stage 2.C: capture the first valid
    // SHAPE_NODE leaf as the per-actor anchor for the GatherGpuObjectLightDataOnly
    // call hoisted between this loop and the second submit loop. Recon
    // Section 9 Item 5 confirmed all leaves of a multishape see identical
    // lightData_, so any non-null SHAPE_NODE works as the gather receiver.
    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < n; ++i) {
        const TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        const TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;  // skip helpers
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(child->myType);
        if (firstShapeNodeLeaf == nullptr) firstShapeNodeLeaf = rec.node;
        if (s_typeIndex.find(ts) == s_typeIndex.end()) {
            loadLateRegisterAllowlistOnce();
            // Stage 2.C+ instrumentation: build a richer identifier for the
            // unregistered type. The dedup key still uses the pointer so the
            // count-once-per-type logic stays stable across name lookups,
            // but the human-readable log line includes:
            //   - caller actor's appearType->name (e.g. "house3.ase")
            //   - the TG_TypeShape's nodeId (inherited from TG_TypeNode;
            //     getNodeId() is public, returns char*; const_cast is the
            //     minimal-touch workaround for the const TG_TypeShape* we
            //     have here)
            //   - the bare pointer (still useful for cross-referencing
            //     across log files / ts identity over a process lifetime)
            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "%p", (const void*)ts);
            const std::string typeKey = addrBuf;  // dedup key — pointer-stable within one run
            auto& count = s_lateRegisterCounts[typeKey];
            // Stage 2.D.3: compute `allowed` for EVERY event so the per-event
            // counters tick correctly. The PRINT (and registered_dump) below
            // remain gated by `count == 0` so the log stays one-line-per-type.
            const char* nodeIdEvt = nullptr;
            if (ts) {
                nodeIdEvt = const_cast<TG_TypeShape*>(ts)->getNodeId();
            }
            const bool allowedEvt =
                nodeIdEvt && nodeIdEvt[0] &&
                (s_lateRegisterAllowlist.find(nodeIdEvt)
                 != s_lateRegisterAllowlist.end());
            if (allowedEvt) ++s_lateReg_allowed_events;
            else            ++s_lateReg_disallowed_events;
            if (count == 0) {
                const char* nodeId = nodeIdEvt;
                // Allowlist matching uses the nodeId, NOT the pointer-key.
                // Pointers are not stable across process runs, so a pointer
                // entry in data/objbatcher_late_register_allowlist.txt is
                // useless. nodeId comes from the .ase author and is stable
                // across runs and across the same nodeId-family of types
                // (LOD variants of the same shape share a nodeId — e.g.,
                // multiple TG_TypeShape* with nodeId="Centipede" at
                // different vertexCounts; allowlisting "Centipede" matches
                // the family). Empty-name shapes cannot be allowlisted —
                // the file would have no useful key for them.
                const bool allowed = allowedEvt;
                std::fprintf(stderr,
                       "[OBJBATCHER v1] event=late_register type=%s nodeId=%s caller=%s allowed=%d\n",
                       typeKey.c_str(),
                       (nodeId && nodeId[0]) ? nodeId : "<unnamed>",
                       (callerName && callerName[0]) ? callerName : "<unknown>",
                       allowed ? 1 : 0);
                std::fflush(stderr);

                // Stage 2.C+ instrumentation: one-shot registered-types
                // dump. Emitted on the FIRST late-register event of the run
                // only (s_emittedRegisteredDump is file-static, set true
                // here). Lists the registered TG_TypeShape pointers AND
                // their nodeIds so an operator can grep this output to find
                // "is the late-reg one in the same family as registered
                // ones, just with a different nodeId variant?" or "is the
                // late-reg type entirely absent from the s_typeIndex, vs.
                // registered as a peer?". Cap at 30 to keep the log
                // tractable; if more types are registered, the count is
                // shown so the truncation is visible.
                // Gated behind MC2_STATIC_PROP_TRACE 2026-05-17: one-shot
                // but ~600-line load-time dump; demote-not-delete.
                static const bool s_gpuPropsRegDump =
                    (getenv("MC2_STATIC_PROP_TRACE") != nullptr);
                static bool s_emittedRegisteredDump = false;
                if (s_gpuPropsRegDump && !s_emittedRegisteredDump) {
                    s_emittedRegisteredDump = true;
                    const size_t total = s_types.size();
                    // 2026-05-10 diag: bumped 30 -> 600 to surface buildings.
                    const size_t cap = total < 600 ? total : 600;
                    std::fprintf(stderr,
                        "[GPUPROPS_REG] event=registered_dump total=%zu shown=%zu\n",
                        total, cap);
                    for (size_t k = 0; k < cap; ++k) {
                        const TG_TypeShape* rt = s_types[k].source;
                        const char* rnode = nullptr;
                        if (rt) rnode = const_cast<TG_TypeShape*>(rt)->getNodeId();
                        std::fprintf(stderr,
                            "[GPUPROPS_REG] entry typeID=%zu ptr=%p nodeId=%s vertexCount=%u\n",
                            k, (const void*)rt,
                            (rnode && rnode[0]) ? rnode : "<unnamed>",
                            s_types[k].vertexCount);
                    }
                    std::fflush(stderr);
                }
            }
            ++count;
            // Stage 2.A: signal the caller that this failure was specifically
            // a late-registration miss (not a fatal GPU error). Caller (Stage
            // 2.B) queries wasLastFailureLateRegistration() and sets
            // needsFullBakeNextFrame=true on the owning actor.
            s_lastSubmitWasLateReg = true;
            ++s_late_register_recovery_skips;
            s_currentSubmittingMulti = nullptr;
            return false;
        }
    }

    // Slice 2 (object-offload) — Stage 2.C: per-actor light-data gather.
    // Stage 2.D.2 fix: use cachedGpuLightIndex_ if it was pre-gathered during
    // update() (while worldLights[0]->aRGB was per-actor-correct).
    // Fallback to GatherGpuObjectLightDataOnly() here only when the cache is
    // invalid (0xFFFFFFFF sentinel = not yet cached, e.g. first frame).
    //
    // Root cause of the timing bug this fixes:
    //   BldgAppearance::update() sets worldLights[0]->aRGB = terrainLight(pos).
    //   GatherGpuObjectLightDataOnly() called here (during renderLists()) reads
    //   worldLights[0]->aRGB AFTER all actors have updated — by then later
    //   actors have overwritten it with their own terrain-position-scaled values.
    //   The last-actor's value (often white at high-elevation positions) is what
    //   the GPU UBO receives, while the CPU vertex loop used the per-building
    //   terrain-scaled value during its own update(). This causes the hot-green
    //   (daytime base=0) vertex mismatch: GPU applies white directional light,
    //   CPU applied the terrain-tinted (~0.88) directional light.
    uint32_t lightDataIndex = 0;
    if (multi->cachedGpuLightIndex_ != 0xFFFFFFFFu) {
        // Happy path: lights were gathered at update() time (correct per-actor).
        lightDataIndex = multi->cachedGpuLightIndex_;
    } else if (firstShapeNodeLeaf != nullptr) {
        // Fallback: first frame or non-GPU-object path cached nothing.
        lightDataIndex = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();
    }

    // Second pass: submit each eligible child, silently skipping those the
    // CPU path would also skip (helpers, untransformed, daytime spotlights).
    for (int i = 0; i < n; ++i) {
        TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) { s_counters.skipped_children++; continue; }
        TG_Shape* child = rec.node;
        if (!child->myType) { s_counters.skipped_children++; continue; }
        // Helper/bone nodes (non-SHAPE). CPU Render silently returns because
        // numVertices == 0. Do the same — no instance to emit.
        if (child->myType->GetNodeType() != SHAPE_NODE) {
            s_counters.skipped_children++;
            continue;
        }
        // Spotlights during day + other early-outs in TransformShape leave
        // listOfVertices NULL. CPU Render early-outs on the same condition.
        // Also listOfColors NULL: CPU also early-outs. Submit's zero-pad
        // path would render this child black, which is the bug we're
        // avoiding — so skip here.
        //
        // GPU-INSTANCE-SKIP-POOLS-1: in the new GPU-only path, registered
        // types intentionally run the zero-pool hierarchy walk
        // (TransformMultiShape_HierarchyOnly) in *Appearance::update, which
        // leaves listOfVertices/listOfColors NULL but DOES populate
        // rec.shapeToWorld (used at :4391). Such a child is a legitimate GPU
        // instance: its geometry is the immutable per-type VBO and its colors
        // are the debug-only (zero-padded) Colors SSBO the lit shader ignores
        // (static_prop.vert:5). So admit a NULL-vertex child IFF its type is
        // registered in s_typeIndex AND we are not in the legacy escape hatch.
        // A NULL-vertex child whose type is NOT registered (true day-spotlight
        // early-out) is still skipped, preserving stock behavior.
        if (!child->listOfVertices || !child->listOfColors) {
            bool admitAsGpuOnly = false;
            if (!s_legacyInstancePools && child->myType) {
                const TG_TypeShape* ts =
                    static_cast<const TG_TypeShape*>(child->myType);
                admitAsGpuOnly = (s_typeIndex.find(ts) != s_typeIndex.end());
            }
            if (!admitAsGpuOnly) {
                s_counters.skipped_children++;
                continue;
            }
        }

        // [T3.1] SpotLight_-prefixed children are illuminated as real TG_Lights
        // by BldgAppearance::update (T1.4 / per-instance). The legacy cone
        // billboard packet is suppressed at the source so substitutive-completion
        // criterion #2 ("zero static-prop draws contain spotlight-tagged
        // packets") is satisfied by construction. Public accessor
        // GetIsSpotlight() is forward-compatible; direct field access also
        // works because GpuStaticPropBatcher is in the TG_MultiShape friend
        // list at msl.h:251-256. Gate retired in T3.1; behavior is now
        // unconditional.
        if (const_cast<TG_Shape*>(child)->GetIsSpotlight()) {
            s_counters.skipped_children++;
            continue;
        }

        uint32_t flags = 0;
        if (child->lightsOut)   flags |= (1u << 0);
        if (child->isWindow)    flags |= (1u << 1);
        // Bit-2: foliage/alpha-test. Enables abs(NdotL) two-sided lighting in
        // static_prop.vert so cross-quad tree faces light consistently from
        // all camera angles (face normals rotate with view → dark face shadow
        // rotates with camera without this). alphaTestOn lives on TG_TypeShape.
        if (const TG_TypeShape* ts2 = static_cast<const TG_TypeShape*>(child->myType)) {
            if (ts2->alphaTestOn) flags |= (1u << 2);
        }
        // [T3.2] Bit-2 was kFlagIsSpotlight (deleted). After T3.1 the
        // spotlight `continue` skip above is unconditional, so this code path
        // is structurally unreachable for spotlight children. The dead
        // shader-side read in static_prop.vert is removed in lockstep per
        // memory/cpp_glsl_ubo_struct_lockstep.md. The static-prop branch of
        // [SPOTLIGHT_REAL_TRACE v1] never fires post-T3.1; the per-actor
        // mech/gv first-hit traces remain via the registration sites in
        // mech3d.cpp / gvactor.cpp (Debug Instrumentation Rule). Bit-2 is now
        // reused as kFlagIsAlphaTest above.

        // rec.shapeToWorld is LinearMatrix4D; convert to Matrix4D for submit().
        Stuff::Matrix4D xform(rec.shapeToWorld);
        if (!submit(child, xform,
                    child->aRGBHighlight, child->fogRGB, flags,
                    lightDataIndex)) {
            // submit() rejected after we passed the registration gate —
            // typically a buffer-full condition. Fall back for this frame
            // to keep the visual self-consistent.
            s_currentSubmittingMulti = nullptr;
            return false;
        }
        s_counters.submitted_children++;
    }
    s_counters.submitted_instances_by_pop[popIndex(pop)]++;
    // Stage 2.D.3 — sampler observation. Record the multishape pointer in
    // the parity sidecar's per-frame observation list. Default-off short-
    // circuits internally when MC2_OBJECT_PARITY_CHECK is unset; safe to
    // call unconditionally. Only successful submissions reach here, so
    // late-registered actors (skybox/compass) are structurally excluded
    // from the sampler pool — matching the "registered types only"
    // contract from the dispatch.
    gos_object_parity::ObserveSubmittedShape(multi);
    s_currentSubmittingMulti = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
// Task 10: flush() — per-packet instanced draw
// ---------------------------------------------------------------------------
namespace {

// Per-frame upload state shared between flushShadow() (Task 13) and flush().
// Whichever runs first this frame owns the upload; the other skips it when
// s_lastUploadedSlot == s_frameSlot.
std::unordered_map<uint32_t, TypeRangeSsbo> s_typeRanges;
uint32_t s_lastUploadedSlot = 0xFFFFFFFFu;

bool uploadAllBucketsIfNeeded() {
    if (s_lastUploadedSlot == s_frameSlot) return true;

    // M2a POPULATION-SPLIT: when staticPopSplitArmed(), statics are excluded from
    // s_bucketsByType (reinjectPersistentStatic early-out). If there are also no
    // dynamic props, the bucket is empty but flush() must still proceed so the M2a
    // block can draw all statics from s_staticInstanceSsbo. Do NOT return false here
    // when armed — fall through to the fence wait + slot advance below.
    if (s_bucketsByType.empty() && !staticPopSplitArmed()) return false;

    loadProgramsIfNeeded();
    if (s_fatalRegistrationFailure) return false;

    // SSBO offset alignment must be queried to size the ring correctly.
    // glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ..., offset, size) requires
    // offset % GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT == 0 (minimum 256).
    // Each per-type range starts at an aligned offset, which wastes up to
    // (alignment - 1) bytes per bucket. The CAPACITY request must include
    // that slack or we overrun the mapped buffer on zoom-out (more buckets
    // active -> more padding overhead).
    static GLint s_ssboAlignment = 0;
    if (s_ssboAlignment == 0) {
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &s_ssboAlignment);
        if (s_ssboAlignment < 16) s_ssboAlignment = 256;  // sane fallback
    }
    auto alignUp = [](size_t v, size_t a) {
        return (v + (a - 1)) & ~(a - 1);
    };

    // Compute EXACT total byte usage with per-bucket alignment padding.
    size_t instBytesNeeded = 0;
    size_t colBytesNeeded  = 0;
    for (auto& kv : s_bucketsByType) {
        instBytesNeeded = alignUp(instBytesNeeded, (size_t)s_ssboAlignment);
        colBytesNeeded  = alignUp(colBytesNeeded,  (size_t)s_ssboAlignment);
        instBytesNeeded += kv.second.instances.size() * sizeof(GpuStaticPropInstance);
        colBytesNeeded  += kv.second.colors.size() * sizeof(uint32_t);
    }
    // Same reasoning: M2a armed with no dynamic props → instBytesNeeded stays 0
    // but we still need the ring slot advanced + fence waited so flush() can proceed.
    if (instBytesNeeded == 0 && !staticPopSplitArmed()) return false;

    // Convert back to element counts (ceil) for ensureRingCapacity, which is
    // element-based. Round up so subsequent ring-indexing in bytes fits.
    const size_t instCountNeeded =
        (instBytesNeeded + sizeof(GpuStaticPropInstance) - 1) / sizeof(GpuStaticPropInstance);
    const size_t colCountNeeded =
        (colBytesNeeded + sizeof(uint32_t) - 1) / sizeof(uint32_t);

    ensureRingCapacity(instCountNeeded, colCountNeeded);
    if (s_fatalRegistrationFailure) return false;

    s_frameSlot = (s_frameSlot + 1) % RING_FRAMES;
    // Step 4.4 — legacy mode: coalesce ring mirrors legacy ring slot.
    // Non-legacy mode: s_coalesceFrameSlot is advanced inside
    // batcher_prepareBaseInstanceTable() (called from txmmgr.cpp).
    if (s_globalPoolLegacy) {
        s_coalesceFrameSlot = s_frameSlot;
    }
    if (s_fence[s_frameSlot]) {
        ZoneScopedN("GpuSP.FenceWaitRender");
        MC2_GL_ClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    const size_t slotInstByteBase = s_frameSlot * s_instanceCapacity * sizeof(GpuStaticPropInstance);
    const size_t slotColByteBase  = s_frameSlot * s_colorCapacity    * sizeof(uint32_t);
    auto* instMapBase = static_cast<uint8_t*>(s_instanceMap) + slotInstByteBase;
    auto* colMapBase  = static_cast<uint8_t*>(s_colorMap)    + slotColByteBase;

    // Deterministic ascending typeID iteration — makes Tracy / RenderDoc
    // diffs stable and shader-debug repro repeatable across runs.
    std::vector<uint32_t> sortedTypeIDs;
    sortedTypeIDs.reserve(s_bucketsByType.size());
    for (auto& kv : s_bucketsByType) sortedTypeIDs.push_back(kv.first);
    std::sort(sortedTypeIDs.begin(), sortedTypeIDs.end());

    s_typeRanges.clear();
    size_t instCursor = 0;
    size_t colCursor  = 0;
    for (uint32_t typeID : sortedTypeIDs) {
        PerTypeBucket& b = s_bucketsByType[typeID];

        // Align the start of each per-type region to the SSBO alignment
        // requirement before writing.
        instCursor = alignUp(instCursor, static_cast<size_t>(s_ssboAlignment));
        colCursor  = alignUp(colCursor,  static_cast<size_t>(s_ssboAlignment));

        TypeRangeSsbo r{};
        r.instanceByteOffset = slotInstByteBase + instCursor;
        r.instanceByteSize   = b.instances.size() * sizeof(GpuStaticPropInstance);
        r.colorByteOffset    = slotColByteBase  + colCursor;
        r.colorByteSize      = b.colors.size() * sizeof(uint32_t);
        r.instanceCount      = static_cast<uint32_t>(b.instances.size());

        if (r.instanceByteSize)
            std::memcpy(instMapBase + instCursor, b.instances.data(), r.instanceByteSize);
        if (r.colorByteSize)
            std::memcpy(colMapBase  + colCursor,  b.colors.data(),    r.colorByteSize);

        instCursor += r.instanceByteSize;
        colCursor  += r.colorByteSize;
        s_typeRanges[typeID] = r;
    }

    // 2b Stage 2 Task-0 diagnostic: aggregate static/dynamic type overlap + order.
    if (s_bucketOrderTrace) {
        static uint64_t s_botCall = 0, s_botEmitted = 0;
        ++s_botCall;
        if ((s_botCall % 60u) == 0u && s_botEmitted < 16) {
            ++s_botEmitted;
            uint32_t sOnly = 0, dOnly = 0, mixed = 0, mixSF = 0, mixDF = 0;
            for (auto& kv : s_bucketsByType) {
                const PerTypeBucket& b = kv.second;
                const bool hs = b.dbgStatic > 0, hd = b.dbgDynamic > 0;
                if (hs && hd) { ++mixed; if (b.dbgFirst == 1) ++mixSF; else ++mixDF; }
                else if (hs)  ++sOnly;
                else if (hd)  ++dOnly;
            }
            std::fprintf(stderr,
                "[BUCKET_ORDER v1] uploadCall=%llu types=%zu static_only=%u dynamic_only=%u mixed=%u "
                "mixed_static_first=%u mixed_dynamic_first=%u\n",
                (unsigned long long)s_botCall, s_bucketsByType.size(), sOnly, dOnly, mixed, mixSF, mixDF);
            std::fflush(stderr);
        }
    }

    s_lastUploadedSlot = s_frameSlot;
    return true;
}

} // namespace

// v8: single source of truth for the live v6 array build. Writes outPackets/outMeta
// (lockstep) and the s_v6Frame* diagnostic counters as a side effect. Returns the
// lockstep-violation count. Behavior is identical to the inlined loop it replaces.
static uint32_t buildLiveV6Arrays(
        uint32_t totalCmds,
        const uint32_t* baseInstanceMap,
        std::vector<RenderCore::DrawPacket>& outPackets,
        std::vector<StaticPropDispatchMeta>& outMeta)
{
    outPackets.resize(totalCmds);
    outMeta.resize(totalCmds);
    uint32_t lockstepViolations = 0u;

    constexpr uint32_t kNoObjectIndex = 0xFFFFFFFFu;

    // --- Builder loop ---
    for (uint32_t i = 0u; i < totalCmds; ++i) {
        // Guard 1: sorted_oob — i must be valid index into s_sortedPacketOrder
        if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
            ++s_v6FrameSortedOob;
            outMeta[i]    = {};
            outPackets[i] = {};
            continue;
        }
        const uint32_t globalPktIdx = s_sortedPacketOrder[i];

        // Guard 2: packet_oob — globalPktIdx must be valid index into s_packets
        if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) {
            ++s_v6FramePacketOob;
            outMeta[i]    = {};
            outPackets[i] = {};
            continue;
        }
        const GpuStaticPropPacket& pkt = s_packets[globalPktIdx];

        // Guard 3: type_oob — owningTypeID outside static all-types table
        if (pkt.owningTypeID >= static_cast<uint32_t>(s_types.size())) {
            ++s_v6FrameTypeOob;
            outMeta[i]    = {};
            outPackets[i] = {};
            continue;
        }

        const auto typeIt = s_typeRanges.find(pkt.owningTypeID);
        const uint32_t instCount =
            (typeIt != s_typeRanges.end()) ? typeIt->second.instanceCount : 0u;

        const uint32_t grp = (i < s_alphaOffCmdCount) ? 0u : 1u;

        outMeta[i].sortedSlot      = i;
        outMeta[i].globalPacketIdx = globalPktIdx;
        outMeta[i].typeId          = pkt.owningTypeID;
        outMeta[i].group           = grp;
        outMeta[i].instanceCount   = instCount;
        outMeta[i].baseInstance    = baseInstanceMap[i];
        outMeta[i].drawIDBase      = i;
        outMeta[i].baseVertex      = pkt.baseVertex;

        outPackets[i].pipelineId    = static_cast<uint32_t>(
            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                      : RenderCore::PipelineId::StaticPropAlphaTest);
        outPackets[i].mesh          = {};
        outPackets[i].material      = {};
        outPackets[i].objectIndex   = kNoObjectIndex;
        outPackets[i].lightIndex    = kNoObjectIndex;
        outPackets[i].firstIndex    = pkt.firstIndex;
        outPackets[i].indexCount    = pkt.indexCount;
        outPackets[i].instanceCount = instCount;
        outPackets[i].sortKey       = 0u;
    }

    // --- Lockstep validator (ALL checks before any glDraw) ---
    if (outPackets.size() != outMeta.size()) {
        std::fprintf(stderr,
            "[DRAW_PACKET_V6] frame=%u event=lockstep_violation"
            " slot=- field=array_size expected=%zu got=%zu\n",
            s_v6TotalFrameCount, outMeta.size(), outPackets.size());
        ++lockstepViolations;
    }

    for (uint32_t i = 0u; i < totalCmds; ++i) {
        // Skip slots zeroed by builder guard continues (sorted/packet/type OOB).
        if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) continue;
        const uint32_t gIdx = s_sortedPacketOrder[i];
        if (gIdx >= static_cast<uint32_t>(s_packets.size())) continue;
        const GpuStaticPropPacket& p = s_packets[gIdx];
        if (p.owningTypeID >= static_cast<uint32_t>(s_types.size())) continue;

        const uint32_t grp = (i < s_alphaOffCmdCount) ? 0u : 1u;
        const uint32_t expectedPipelineId = static_cast<uint32_t>(
            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                      : RenderCore::PipelineId::StaticPropAlphaTest);

#define V6_CHECK(field, expected_expr, got_expr) \
    if ((expected_expr) != (got_expr)) { \
        std::fprintf(stderr, \
            "[DRAW_PACKET_V6] frame=%u event=lockstep_violation slot=%u" \
            " field=" #field " expected=%u got=%u\n", \
            s_v6TotalFrameCount, i, \
            static_cast<unsigned>(expected_expr), \
            static_cast<unsigned>(got_expr)); \
        ++lockstepViolations; \
    }
        V6_CHECK(sortedSlot,     i,                  outMeta[i].sortedSlot)
        V6_CHECK(globalPacketIdx, gIdx,              outMeta[i].globalPacketIdx)
        V6_CHECK(drawIDBase,     i,                  outMeta[i].drawIDBase)
        V6_CHECK(typeId,         p.owningTypeID,     outMeta[i].typeId)
        V6_CHECK(group,          grp,                outMeta[i].group)
        V6_CHECK(pipelineId,     expectedPipelineId, outPackets[i].pipelineId)
        V6_CHECK(indexCount,     p.indexCount,       outPackets[i].indexCount)
        V6_CHECK(firstIndex,     p.firstIndex,       outPackets[i].firstIndex)
        V6_CHECK(instanceCount,  outMeta[i].instanceCount, outPackets[i].instanceCount)
#undef V6_CHECK
        if (outMeta[i].baseVertex != p.baseVertex) {
            std::fprintf(stderr,
                "[DRAW_PACKET_V6] frame=%u event=lockstep_violation slot=%u"
                " field=baseVertex expected=%d got=%d\n",
                s_v6TotalFrameCount, i, p.baseVertex, outMeta[i].baseVertex);
            ++lockstepViolations;
        }
    }

    return lockstepViolations;
}

// FOLIAGE-STATICPROP-DEPTH-PREPASS-1 ----------------------------------------
// In-flush depth prepass for the v6/coalesced static-prop dispatch.
//
// DELIBERATE DEVIATION from the plan's txmmgr-level call: the plan called
// flushDepthPrepass() from txmmgr between compute_dispatch() and flush(). But
// the per-frame dispatch lists (pDispatchPackets / pDispatchMeta / totalCmds)
// are built INSIDE flush() (snapshot vs live selection). A txmmgr-level prepass
// cannot see those lists and could draw a DIFFERENT set, which breaks the
// GL_EQUAL color pass (every color fragment must have a matching depth from the
// prepass). So the prepass runs here, inside flush(), consuming the EXACT same
// lists the color loop draws — ONE source of truth for the draw set.
//
// Lays the nearest reverse-Z depth (GEQUAL + write) with the cheap depth-only
// program (alpha-test discard only), color writes masked off. Caller flips the
// color pass to GL_EQUAL + depthWrite-off afterward, keyed off this function's
// inline return value (true == prepass drew this frame).
//
// Mirrors the v6 color draw loop's per-packet GL exactly: same VAO/IBO/SSBO,
// same OFF→ON texture-array flip at the alpha-ON group boundary, same
// glDrawElementsInstancedBaseVertexBaseInstance args. Returns true iff it drew.
//
// Gated by MC2_STATIC_PROP_DEPTH_PREPASS (default OFF). No-op + log-once when
// the depth program failed to link or the bc7-bucket texture path is active
// (per-bucket array bind differs from the simple OFF/ON bind — handled in a
// later slice). It does NOT touch SSBO slot 0 (the coalesce instance range
// bound earlier in flush()) so that binding survives for the color loop.
// OBJECT-TERRAIN-COPLANAR-OFFSET-1: slope-scaled polygon offset for opaque world
// objects (props/buildings/turrets/mechs) so they win the coplanar GEQUAL tie
// over true-depth terrain WITHOUT the constant-NDC-bias tradeoff (too small ->
// far-zoom z-fight; too large -> near show-through). Polygon offset tracks the
// per-fragment depth slope/precision, which is the correct shape for this.
// Gated default-OFF (gate OFF == committed TERRAIN-DEPTH-CONTRACT-1 behavior).
// Tunable; reverse-Z/GEQUAL so sign is determined by tuning (start -1/-1).
static bool objPolyOffsetEnabled() {
    static const char* e = std::getenv("MC2_OBJECT_POLY_OFFSET");
    return e != nullptr && e[0] != '0';
}
static float objPolyOffsetFactor() {
    static const float v = [](){ const char* s = std::getenv("MC2_OBJECT_POLY_OFFSET_FACTOR"); return (s && s[0]) ? (float)atof(s) : -1.0f; }();
    return v;
}
static float objPolyOffsetUnits() {
    static const float v = [](){ const char* s = std::getenv("MC2_OBJECT_POLY_OFFSET_UNITS"); return (s && s[0]) ? (float)atof(s) : -1.0f; }();
    return v;
}

static bool flushDepthPrepassV6(
    const std::vector<RenderCore::DrawPacket>&  dispatchPackets,
    const std::vector<StaticPropDispatchMeta>&  dispatchMeta,
    uint32_t                                    totalCmds,
    bool                                        bc7Buckets,
    int                                         debugAddrMode_)  // IMPORTANT-1: mode-8 bypass parity
{
    ZoneScopedN("GpuSP.DepthPrepass");

    // OBJECT-TERRAIN-COPLANAR-OFFSET-1: skip the prepass when the polygon-offset
    // gate is on, so the color pass uses single-pass GEQUAL (no GL_EQUAL parity
    // constraint) and the offset applied to the color draw fully governs depth.
    if (objPolyOffsetEnabled()) return false;

    static const bool s_prepassEnabled =
        (std::getenv("MC2_STATIC_PROP_DEPTH_PREPASS") != nullptr);
    if (!s_prepassEnabled)              return false;
    if (s_staticPropDepthProgram == 0)  return false;   // program didn't link
    if (totalCmds == 0u)                return false;

    // bc7-bucket path: the color loop binds a different per-bucket texture array
    // per slot for alpha sampling. The depth shader must sample the SAME array to
    // make the SAME alpha decision; the simple OFF/ON bind below would diverge.
    // Skip + log for now (handle bc7 in a follow-up slice).
    if (bc7Buckets) {
        static bool warnedBc7 = false;
        if (!warnedBc7) {
            warnedBc7 = true;
            std::fprintf(stderr, "[GPUPROPS] depth-prepass skipped: bc7-bucket "
                                 "texture path active (per-bucket array bind "
                                 "unsupported in this slice)\n");
        }
        return false;
    }

    // GL_EQUAL-parity (review CRITICAL-2): when MC2_VIEW_UNIFORMS=0 the VS uses a
    // plain `uniform mat4 u_worldToClipGL` (cached on the COLOR program as
    // s_locsCoalesce.terrainMVP) instead of the view-uniform UBO. The depth
    // program is a distinct GL object and the prepass does NOT upload that MVP
    // to it, so its u_worldToClipGL would be the zero matrix → gl_Position = 0 →
    // the prepass lays no usable depth and GL_EQUAL makes all props vanish.
    // s_viewUniformsDisabled (file-scope, == !s_viewUniformsShaderEnabled) is the
    // non-default kill-switch combo; skip the prepass entirely so the color path
    // falls back to single-pass GEQUAL (return false → caller draws unchanged).
    if (s_viewUniformsDisabled) {
        static bool warnedNoUbo = false;
        if (!warnedNoUbo) {
            warnedNoUbo = true;
            std::fprintf(stderr, "[GPUPROPS] depth-prepass skipped: "
                                 "MC2_VIEW_UNIFORMS=0 (non-UBO MVP path "
                                 "unsupported)\n");
        }
        return false;
    }

    // Save the color write mask so the color pass writes normally afterward.
    GLboolean prevColorMask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

    // Depth-only: bind the depth program + GEQUAL/write via the StaticPropDepth
    // desc, then mask color. applyPipeline also (re)sets blend-off + cull-back,
    // matching the color pass.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropDepth),
        "StaticPropDepth");   // STATIC-PROP-COLOR-LABEL-HARDENING-1: label the live depth-prepass bind
    render_frame_plan::trace(render_frame_plan::Phase::StaticOpaque, "StaticPropDepth",
        render_frame_plan::PathKind::Batcher, -1, "StaticPropDepth");
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // Re-assert the geometry bindings the v6 loop relies on. SSBO slot 0 (the
    // coalesce instance range) and slot 2 (per-type hot-color) were bound earlier
    // in flush(); we do NOT touch slot 0 (so it survives for the color loop) but
    // re-bind VAO/IBO/slot-2/texture defensively in case applyPipeline or a prior
    // pass left them in an unexpected state.
    glBindVertexArray(s_sharedVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    if (s_perTypeSsbo) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_perTypeSsbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);   // alpha-OFF group default

    // GL_EQUAL-parity (review CRITICAL-1): the depth frag derives effectiveLayer
    // (texArrayLayer vs MaterialTable.albedoTex @ binding 5) under u_materialGpuSample
    // EXACTLY like the color frag. Mirror the color pass's slot-5 bind + sampleOn
    // computation (flush() ~line 5598-5621) on THIS program so the alpha-test UV →
    // tex_color.a → discard decision is byte-identical. sampleOn replicates the
    // color pass's five-condition gate verbatim (incl. s_materialGpuSidecarValid).
    if (s_materialGpuEnabled && s_materialGpuSsbo.glName != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, static_cast<GLuint>(s_materialGpuSsbo.glName));
    const bool depthSampleOn = s_materialGpuEnabled
                            && s_materialGpuSampleEnabled
                            && s_materialGpuSsbo.glName != 0
                            && s_materialGpuSidecarValid
                            && s_locsDepthCoalesce.materialGpuSample >= 0;
    if (s_locsDepthCoalesce.materialGpuSample >= 0)
        glUniform1i(s_locsDepthCoalesce.materialGpuSample, depthSampleOn ? 1 : 0);
    // GL_EQUAL-parity (review IMPORTANT-1): mode-8 LODBUG bypass must match the
    // color pass's u_debugAddrMode upload (flush() ~line 5532) so the depth frag's
    // `u_debugAddrMode != 8` discard gate agrees with the color frag.
    if (s_locsDepthCoalesce.debugAddrMode >= 0)
        glUniform1i(s_locsDepthCoalesce.debugAddrMode, debugAddrMode_);

    bool enteredOnGroup = false;
    for (uint32_t i = 0u; i < totalCmds; ++i) {
        const StaticPropDispatchMeta& m  = dispatchMeta[i];
        const RenderCore::DrawPacket&  dp = dispatchPackets[i];
        if (m.instanceCount == 0u) continue;

        // Texture-array switch at the alpha-ON boundary (mirror flush()'s loop:
        // alpha-OFF slots come first, then alpha-ON; one bind flip total).
        if (m.group == 1u && !enteredOnGroup) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
            enteredOnGroup = true;
        }

        // The DEPTH program's OWN u_drawIDBase location (coalesce variant).
        if (s_locsDepthCoalesce.drawIDBase >= 0)
            glUniform1i(s_locsDepthCoalesce.drawIDBase,
                        static_cast<GLint>(m.drawIDBase));

        glDrawElementsInstancedBaseVertexBaseInstance(
            GL_TRIANGLES,
            static_cast<GLsizei>(dp.indexCount),
            GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(dp.firstIndex) * sizeof(uint32_t)),
            static_cast<GLsizei>(m.instanceCount),
            m.baseVertex,
            m.baseInstance);
    }
    if (enteredOnGroup)
        glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

    // Restore the color write mask. Depth state + program are reset by the color
    // applyPipeline the caller issues immediately after this returns.
    glColorMask(prevColorMask[0], prevColorMask[1],
                prevColorMask[2], prevColorMask[3]);
    return true;
}

// APPLY-STATE-STATICPROP-1: third top-level scene-pass apply-state. Forward-decl of
// the cross-TU apply-state counter bump (defined in gos_postprocess.cpp). Bumps the
// SAME g_applyStatePasses that the PostProcess apply-state islands + the TerrainDecal/
// TerrainOverlay top-level applies use, so executor_apply_state_passes aggregates with
// no double-count (single increment per apply).
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);

// APPLY-STATE-STATICPROP-1: file-static flag mirroring decalStateAppliedByExecutor_.
// When MC2_FRAMEGRAPH_EXECUTOR is ON, executorApplyStaticPropOpaqueState() pre-applies
// the StaticPropOpaque pipeline and sets this; flush()'s COLOR-pass body then skips its
// own applyPipeline (one-shot reset). Gate OFF -> flag stays false -> body applies as
// before -> byte-identical.
static bool s_staticPropStateAppliedByExecutor = false;

// APPLY-STATE-STATICPROP-1: pre-apply the declared StaticPropOpaque pipeline. Top-level
// analog of gosRenderer::executorApplyTerrainDecalState() / ...OverlayState(): performs
// the SOLE entry render-state of the static-prop COLOR pass (applyPipeline(StaticPropOpaque)
// at flush ~:5503), bumps the shared apply-state counter, and flags the body to skip its
// own applyPipeline (one-shot). Only the pipeline is lifted; FBO/MRT/drawBuffers/viewport
// are inherited from the preceding pass and are NOT touched here (kTopLevelStateDesc records
// them as Unknown/Inherit). The separate StaticPropDepth prepass (~:5252) is NOT lifted.
// Idempotent: the body makes the identical call when the flag is false. Only reached when
// MC2_FRAMEGRAPH_EXECUTOR is ON and the pass runs.
static void executorApplyStaticPropOpaqueState() {
    // FRAMEGRAPH-APPLY-STATE-EXTEND-1 SELF-PROOF: apply the EXPLICIT generic axes the
    // StaticPropOpaque row now declares (fboTarget=MainColor, viewport=MainScene, clear=None)
    // BEFORE the pipeline. StaticProp already inherits exactly the scene FBO + full-scene
    // viewport, so this explicit bind is idempotent and byte-identical to inheritance — it
    // proves the richer apply path at runtime (fbo_mismatches must stay 0). If the scene FBO/
    // size are unavailable we skip the generic-axes (defensive; the desc still drives the
    // pipeline apply below) — but getGosPostProcess() is established by the time static props
    // flush every tier1 frame.
    const RenderCore::framegraph::TopLevelStateDesc* desc =
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::StaticPropOpaque);
    gosPostProcess* pp = getGosPostProcess();
    if (desc && pp) {
        RenderCore::framegraph::applyTopLevelGenericAxes(
            *desc, (unsigned)pp->getSceneFBO(), pp->getWidth(), pp->getHeight());
    }
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque),
        "StaticPropOpaque");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::StaticPropOpaque);
    s_staticPropStateAppliedByExecutor = true;
}

// [RENDER_CONTRACT:Pass=StaticProp id=GpuStaticPropBatcher_flush]
//   Routes through static_prop.frag which writes
//   rc_gbuffer1_screenShadowEligible (production) or
//   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible (debug).
void GpuStaticPropBatcher::flush(const RenderSnapshot* snap) {
    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::StaticProp,
                                              "GpuStaticPropBatcher_flush");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::StaticProp,
                                                    "GpuStaticPropBatcher_flush");
        }
    } _tlGuard;
    ZoneScopedN("GpuStaticProps.Flush");
    mc2_hitch::HitchScope _hitchFlush(mc2_hitch::HitchSpanKind::GpuStaticPropsFlush);
    initTraceOnce();
    // LODBUG probe: env-var override for debugAddrMode_.  RAlt+9 cycling is
    // unreliable on some hosts; set MC2_GPU_PROPS_DEBUG_MODE=8 to force the
    // magenta "did this draw call land?" mode at startup.  Applied once
    // per process; the env value wins over any subsequent RAlt+9 cycling
    // for the FIRST flush only, after which the cycler is authoritative
    // again (so the user can still cycle if they want).
    {
        static bool s_dbgEnvInit = false;
        if (!s_dbgEnvInit) {
            s_dbgEnvInit = true;
            const char* dbgEnv = getenv("MC2_GPU_PROPS_DEBUG_MODE");
            if (dbgEnv && dbgEnv[0]) {
                const int m = atoi(dbgEnv);
                if (m >= 0 && m <= 8) {
                    debugAddrMode_ = m;
                    std::fprintf(stderr,
                        "[LODBUG v1] event=debug_mode_env_override mode=%d\n", m);
                    std::fflush(stderr);
                }
            }
        }
    }
    // 2026-05-11 perf diag: wall-clock timer for substrate-coalesce perf hunt.
    // Mean us reported every 600 calls when MC2_BATCHER_FLUSH_TIMING=1.
    static uint64_t s_btf_calls = 0;
    static uint64_t s_btf_ns_total = 0;
    static uint64_t s_btf_ns_coalesce_write = 0;
    static uint64_t s_btf_ns_coalesce_draw = 0;
    static const bool s_btf_enabled = (getenv("MC2_BATCHER_FLUSH_TIMING") != nullptr);
    const auto _btf_t0 = std::chrono::steady_clock::now();

    // v2.3 snap-cull: reset per-frame counters unconditionally so batcher_getSnapCullStats()
    // always returns valid (zero) data even when geometry is not yet finalized or runV6 is false.
    s_snapCullSkipped      = 0u;
    s_snapCullActive       = 0u;
    s_snapCullSlotMismatch = 0u;

    if (!s_geometryFinalized || s_fatalRegistrationFailure) {
        s_bucketsByType.clear();
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        return;
    }
    if (!uploadAllBucketsIfNeeded()) {
        s_bucketsByType.clear();
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        gos_object_parity::ParityFrameTick();
        return;
    }

    // [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
    render_contract::noteRenderPass(render_contract::PassIdentity::StaticProp,
                                    "GpuStaticPropBatcher_flush");
    // Program compile/link latch. submitMultiShape already gates submissions
    // on this, so reaching here with an empty program is a logic bug — but
    // guard anyway so we never pump uniform calls against a null program.
    if (s_programLoadFailed || s_staticPropProgram == 0) {
        s_bucketsByType.clear();
        s_lastUploadedSlot = 0xFFFFFFFFu;
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        gos_object_parity::ParityFrameTick();
        return;
    }

    batcher_buildCandidateLog();

    // Slice 2 (object-offload) — Stage 2.D.1: parity readback handshake.
    //
    // uploadAllBucketsIfNeeded() above just glClientWaitSync'd on
    // s_fence[s_frameSlot] (the fence from RING_FRAMES frames ago). At
    // this point the parity SSBO writes from that prior cycle are
    // GPU-complete and safe to read on the CPU.
    //
    // 2.D.1 acceptance is "the readback runs without crash or stall" — we
    // glGetBufferSubData the live byte range from this slot into a discard
    // scratch buffer and DROP the bytes. 2.D.2/2.D.3 will swap the discard
    // for a CPU recompute compare.
    //
    // Skips entirely when MC2_OBJECT_PARITY_CHECK is unset
    // (IsParityCheckEnabled() returns false → EnsureParityOutputSSBO()
    // returns 0 → the bind/uniform/readback all short-circuit). This is
    // the load-bearing default-off contract per the advisor.
    GLuint parityBuffer = 0;
    if (gos_object_parity::IsParityCheckEnabled()) {
        parityBuffer = gos_object_parity::EnsureParityOutputSSBO(kParitySlotBytes);
        if (parityBuffer != 0) {
            const size_t bytesToRead = s_parityBytesUsedPerSlot[s_frameSlot];
            if (bytesToRead > 0) {
                if (s_parityReadbackScratch.size() < bytesToRead) {
                    s_parityReadbackScratch.resize(bytesToRead);
                }
                const GLintptr slotBase =
                    static_cast<GLintptr>(s_frameSlot) *
                    static_cast<GLintptr>(kParitySlotBytes);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, parityBuffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                   slotBase,
                                   static_cast<GLsizeiptr>(bytesToRead),
                                   s_parityReadbackScratch.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                gos_object_parity::Counters_AddReadbackBytesThisFrame(
                    static_cast<uint64_t>(bytesToRead));
                // Stage 2.D.2: if the latch is WaitingForReadback, compare
                // the GPU bytes against the CPU snapshot captured on frame N.
                // After compare, advance latch to Done and free the snapshot.
                // The fence wait above (glClientWaitSync in
                // uploadAllBucketsIfNeeded) guarantees GPU writes are complete.
                // Stage 2.D.2: compare only when the ring has returned to the
                // exact slot the Armed frame wrote to (IsDualEmitReadyForSlot
                // checks both state==WaitingForReadback AND slot match).
                if (gos_object_parity::IsDualEmitReadyForSlot(s_frameSlot)) {
                    gos_object_parity::CompareAndReport(
                        s_parityReadbackScratch.data(),
                        bytesToRead,
                        static_cast<unsigned>(s_counters.frame_count));
                    gos_object_parity::AdvanceDualEmitToDone();
                }
            }
        }
        // Reset this slot's usage; it will be re-populated by the per-type
        // bind loop below.
        s_parityBytesUsedPerSlot[s_frameSlot] = 0;
    }
    s_parityBytesUsedThisFrame = 0;

    // Save binding state (program/VAO/SSBO/texture) for restore at exit.
    // Pipeline state (depth/blend/cull) is set by applyPipeline() below and
    // NOT restored: gos_InvalidateRenderStateCache() at exit ensures the next
    // applyRenderStates() re-applies from scratch. Removed 6 glGet* roundtrips
    // (render-hygiene-s1).
    GLint prevProgram=0, prevVao=0, prevArrayBuf=0, prevElemBuf=0;
    GLint prevActiveTex=0, prevTexUnit0=0;
    // Slice 2 (object-offload) — Stage 2.C.2: also save/restore SSBO slot 2
    // (per-type hot-color SSBO; bound once for the whole flush, not per-type).
    // Stage 2.D.1: also save/restore SSBO slot 3 (parity readback harness;
    // bound only when MC2_OBJECT_PARITY_CHECK=1).
    GLint prevSsbo0=0, prevSsbo1=0, prevSsbo2=0, prevSsbo3=0, prevSsbo5=0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit0);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 1, &prevSsbo1);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 2, &prevSsbo2);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 3, &prevSsbo3);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &prevSsbo5);
    // Bind program + depth/blend/cull via PipelineDesc.
    // Uses StaticPropOpaque — both alpha and opaque packets share the same
    // program and fixed-function state; the alpha distinction is texture-array
    // selection and shader discard, not a different GL pipeline state.
    // applyPipeline: binds program, depth (test+write+func=GL_GEQUAL), blend, cull.
    // PipelineDesc v1: DepthFunc::GreaterEqual encoded in the table row — no
    // explicit glDepthFunc() call needed here any more.
    // APPLY-STATE-STATICPROP-1: third top-level scene-pass apply-state. Gated by the
    // SAME predicate executorOwnBeginTopLevel uses internally (MC2_FRAMEGRAPH_EXECUTOR).
    // Dispatched here (after the early-return guards, immediately before the body bind)
    // so the executor apply only fires when the COLOR pass actually reaches its bind —
    // exactly the body's applyPipeline call lifted up, no double-count. Idempotent ON.
    // ★ Unlike decal/overlay, StaticPropOpaque runs every tier1 frame, so this body-skip
    // path is genuinely exercised at runtime.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::StaticPropOpaque) != nullptr) {
        executorApplyStaticPropOpaqueState();
    }
    // APPLY-STATE-STATICPROP-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Gate OFF -> flag false ->
    // body applies -> byte-identical. The separate StaticPropDepth prepass is untouched.
    if (!s_staticPropStateAppliedByExecutor) {
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque),
        "StaticPropOpaque");  // STATIC-PROP-COLOR-LABEL-HARDENING-1: label the live color bind
    }
    s_staticPropStateAppliedByExecutor = false;
    render_frame_plan::trace(render_frame_plan::Phase::StaticOpaque, "StaticPropOpaque",
        render_frame_plan::PathKind::Batcher, -1, "StaticPropOpaque");
    // MDI-SUBMISSION-SCAFFOLD-1: flush-level submission self-report. Per-variant command
    // counts (s_alphaOff/OnCmdCount, BC7 buckets) and the CPU fallback are computed later
    // in this function; this names the static-prop indirect submitter family (live default
    // = GPU-produced coalesce v6 reading gpu_cull's indirect buffer). commands=-1 here
    // (not yet known at bind time); the [STATIC_PROP]/[DRAW_PACKET_V6] traces carry counts.
    mdi_submit::trace("StaticPropOpaque", "MultiDrawElementsIndirect", -1,
                      "GPU", "gpu_cull_indirect", false);
    glBindVertexArray(s_sharedVao);
    // Explicit IBO rebind: GL_ELEMENT_ARRAY_BUFFER is VAO state and can be
    // left at 0 if flushShadow() or any other pass clobbers s_sharedVao's
    // element-buffer slot (e.g. wrong restore order while s_sharedVao was
    // active). Matching the explicit-bind pattern in flushShadow() makes
    // flush() self-healing regardless of prior VAO state. Belt-and-suspenders.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Slice 2 (object-offload) — Stage 2.C.2: bind per-type hot-color SSBO
    // once for the whole flush. The data is per-map immutable so a single
    // bind covers every typeID in the per-type loop below — static_prop.vert
    // indexes by inst.typeID. Restored at the bottom alongside slots 0+1.
    if (s_perTypeSsbo) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_perTypeSsbo);
    }

    // Slice 2 (object-offload) — Stage 2.D.1: parity write defaults OFF.
    // Per-type loop below flips it to 1 only for draws whose bound
    // parity-buffer range fits in this slot's remaining byte budget.
    // When MC2_OBJECT_PARITY_CHECK is unset, parityBuffer == 0 and the
    // uniform stays 0 for every draw (no shader writes happen).
    // Stage 2.D.1.1 (Item 2): use link-time cached locations instead of
    // per-flush glGetUniformLocation (one driver round-trip per flush saved).
    const GLint locParityWrite      = s_loc_u_parityWrite;
    const GLint locParityVerts      = s_loc_u_parityVertsPerType;
    const GLint locParityBaseVertex = s_loc_u_parityBaseVertex;
    if (locParityWrite >= 0) glUniform1i(locParityWrite, 0);

    // Stage 2.D.2: zero the Armed frame's parity SSBO slot before drawing so
    // that back-facing vertices (GPU-culled by GL_CULL_FACE GL_BACK, not
    // written by the shader) read as zero on the compare frame — matching the
    // zero-initialized CPU snapshot (vector initialized to 0u in submit()).
    // Without this clear, stale uint32 values from RING_FRAMES ago remain in
    // the slot for unwritten (back-facing) positions and cause false mismatches
    // against the CPU's 0x00000000 for those same positions.
    if (parityBuffer != 0 && gos_object_parity::IsDualEmitArmedAnyActor()) {
        const GLintptr slotBase =
            static_cast<GLintptr>(s_frameSlot) *
            static_cast<GLintptr>(kParitySlotBytes);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, parityBuffer);
        const GLuint zero = 0u;
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                             slotBase,
                             static_cast<GLsizeiptr>(kParitySlotBytes),
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // C1b: when compute cull is enabled and the indirect command buffer is
    // ready, write the per-type CPU instance counts into the indirect buffer
    // before the draw loop (shadow mode: CPU counts match what GPU will see
    // once C1b->C1c flips GPU-driven counts ON). Then the draw loop uses
    // glDrawElementsIndirect (one draw per type) instead of the instanced call.
    //
    // State setup for C1b: three inheritance traps (gpu_direct_renderer_bringup_checklist.md):
    //   1. Sampler state: bind REPEAT/LINEAR before first C1b draw (below).
    //   2. Depth state:   glEnable(GL_DEPTH_TEST) + GL_GEQUAL (reverse-Z) — set by applyPipeline() above.
    //   3. Blend state:   glDisable(GL_BLEND) — set by applyPipeline() above.
    // Verified: depth and blend are set unconditionally by applyPipeline() regardless of C1b path.
    // C1B-DRAW-FIX: glDrawElementsIndirect uses GPU bucket count (how many visible by
    // count) but draws the FIRST N instances in SSBO registration order — NOT the
    // actually-visible N instances identified by visibleIds[]. When the camera moves,
    // the GPU cull count changes and instances at high SSBO indices (e.g. center-screen
    // props registered late) get cut off even though they ARE visible. The visibleIds
    // compaction path (vertex shader reading visibleIds[gl_BaseInstance+gl_InstanceID])
    // is not wired to the per-type legacy draw loop, so the C1b indirect count is
    // architecturally incorrect here. Use CPU instance count (r.instanceCount) always.
    // The GPU cull still runs (bucket counts → readback/lifecycle), just not for draw.
    // Kill-switch: MC2_STATIC_C1B_DRAW=1 re-enables the (buggy) GPU indirect count.
    static const bool s_c1bDrawOverride = (getenv("MC2_STATIC_C1B_DRAW") != nullptr);
    const bool useC1bIndirect = s_c1bDrawOverride &&
                                gpu_cull::compute_isEnabled() &&
                                (gpu_cull::compute_getIndirectCmdBuf() != 0) &&
                                (gpu_cull::compute_getBucketCount() == s_types.size());

    // C1b GPU authority flip: compute_dispatch() now runs BEFORE flush() and writes
    // GPU-computed per-type instanceCounts via the patch shader. The CPU overwrite
    // below is REMOVED — glMultiDrawElementsIndirect reads GPU-authoritative counts.
    //
    // Shadow mode is no longer needed: static prop substrate records are emitted by
    // GpuStaticPropRegistry::flush() with category = Cat_StaticProp | (typeID<<4),
    // so the cull shader correctly scatters each instance into its typeID bucket.
    //
    // Invariant: compute_dispatch() → GL_COMMAND_BARRIER_BIT → flush() ordering is
    // guaranteed by txmmgr.cpp (registry::flush → compute_dispatch → batcher::flush).
    // The barrier in compute_dispatch() after the patch shader covers GL_COMMAND_BARRIER_BIT.
    //
    // The CPU-overwrite block (shadow mode) is intentionally REMOVED here.
    // If visual corruption occurs, re-enable by reverting this comment block to the
    // original glBufferSubData loop (see git history for the shadow mode code).

    // =======================================================================
    // Substrate Multi-Draw Coalesce — flush() extensions (plan v3.8 Step 11).
    //
    // The CPU write loop below (11.2) and the draw branch later (11.7) form
    // the runtime side of the coalesce path. Per-frame write happens
    // unconditionally if coalesce is armed (legacy s_instanceSsbo write
    // already happened above via uploadAllBucketsIfNeeded — both buffers
    // get filled so a runtime-disarm transition leaves legacy with valid
    // data). The legacy DRAW loop below is gated on !IsCoalesceEnabled()
    // by Step 11.7 (coalesce armed) vs Step 11.8 (coalesce off).
    // =======================================================================

    // Step 11.1 — unconditional coalesce fence cleanup. Runs whether or
    // not coalesce is currently armed: out-MAJ-2 ensures fences from
    // disarmed-mid-mission frames drain. Otherwise outstanding GLsync
    // objects leak until onMapUnload (or longer). Per Step 11.1
    // first-flush latch: do NOT set s_coalesceFirstFlushDone here — the
    // latch is for first ARMED flush (placed in Step 11.7.k).
    // Step 4.4 — legacy mode only: wait on coalesce fence here.
    // Non-legacy mode: prepare-table already waited and cleared this fence;
    // skip to avoid a double-wait on the same sync object.
    if (s_globalPoolLegacy && s_coalesceFence[s_coalesceFrameSlot]) {
        MC2_GL_ClientWaitSync(s_coalesceFence[s_coalesceFrameSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT,
                         GL_TIMEOUT_IGNORED);
        glDeleteSync(s_coalesceFence[s_coalesceFrameSlot]);
        s_coalesceFence[s_coalesceFrameSlot] = nullptr;
    }

    // Steps 11.2 / 11.3 / 11.4 / 11.5 — coalesce per-frame CPU write loop.
    // Step 4.7 — split into global-pool (sorted prefix-sum) and legacy (per-type-cap) branches.
    const auto _btf_t_writeStart = std::chrono::steady_clock::now();
    if (IsCoalesceEnabled() && !s_globalPoolLegacy) {
        // Global-pool path: iterate s_sortedTypeOrder (same order as
        // batcher_prepareBaseInstanceTable) and write instances tightly packed.
        // Uses s_coalesceFrameSlot — the coalesce ring, advanced in prepare-table.
        const size_t fr_off_bytes =
            static_cast<size_t>(s_coalesceFrameSlot) * batcher_getCoalescePerFrameInstanceBytes();
        uint8_t* coalesceMapBase =
            static_cast<uint8_t*>(s_coalesceInstanceMap) + fr_off_bytes;

        std::array<uint32_t, 2> groupCursor = {0u, 0u};
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            if (typeID >= s_types.size()) continue;
            auto kvIt = s_bucketsByType.find(typeID);
            if (kvIt == s_bucketsByType.end()) continue;
            const PerTypeBucket& bucket = kvIt->second;
            GpuStaticPropType& type = s_types[typeID];

            // Peak tracker — runs unconditionally so slice-2 GPU-emit can read peak[t].
            if (typeID >= s_perTypePeak.size()) s_perTypePeak.resize(typeID + 1, 0u);
            const uint32_t cnt = static_cast<uint32_t>(bucket.instances.size());
            if (cnt > s_perTypePeak[typeID]) s_perTypePeak[typeID] = cnt;

            // Overflow gate is dead under global-pool mode: detected in prepare-table.

            // Eviction-detect (adapted from legacy loop — same logic, loop-local var names).
            const TG_TypeShape* src = type.source;
            if (src && src->listOfTextures && type.packetCount > 0u &&
                type.lastSeenGosHandle != 0xFFFFFFFFu) {
                const auto& firstPkt = s_packets[type.firstPacket];
                if (firstPkt.textureSlot < src->numTextures) {
                    const DWORD now = src->listOfTextures[firstPkt.textureSlot].gosTextureHandle;
                    if (now != type.lastSeenGosHandle) {
                        std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=tex_evicted "
                                     "type=%u old_handle=%lu new_handle=%lu\n",
                                     typeID,
                                     (unsigned long)type.lastSeenGosHandle,
                                     (unsigned long)now);
                        s_coalesceArmed = false;
                        break;
                    }
                }
            }

            // Prefix-sum write: all types of each group are packed contiguously.
            const uint32_t group = type.alphaClass;
            const uint32_t typeBaseInstance = groupCursor[group]
                                            + (group == 1u ? s_offGroupCountThisFrame : 0u);
            uint8_t* dst = coalesceMapBase + typeBaseInstance * sizeof(GpuStaticPropInstance);
            std::memcpy(dst, bucket.instances.data(),
                        bucket.instances.size() * sizeof(GpuStaticPropInstance));
            groupCursor[group] += static_cast<uint32_t>(bucket.instances.size());
        }
    }



    if (IsCoalesceEnabled() && s_globalPoolLegacy) {
        // Legacy per-type-cap path (unchanged from substrate-coalesce). Uses s_coalesceFrameSlot
        // (which mirrors s_frameSlot under legacy mode — set at the :2644 advance site).
        const size_t fr_off_bytes =
            static_cast<size_t>(s_coalesceFrameSlot) * batcher_getCoalescePerFrameInstanceBytes();
        uint8_t* coalesceMapBase =
            static_cast<uint8_t*>(s_coalesceInstanceMap) + fr_off_bytes;

        for (auto& kv : s_bucketsByType) {
            const uint32_t typeID = kv.first;
            const PerTypeBucket& bucket = kv.second;
            if (typeID >= s_types.size()) continue;
            GpuStaticPropType& type = s_types[typeID];

            // Peak tracker.
            if (typeID >= s_perTypePeak.size()) s_perTypePeak.resize(typeID + 1, 0u);
            const uint32_t cnt = static_cast<uint32_t>(bucket.instances.size());
            if (cnt > s_perTypePeak[typeID]) s_perTypePeak[typeID] = cnt;

            // Per-type overflow guard.
            if (bucket.instances.size() > type.instanceCap) {
                std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=type_overflow "
                             "type=%u count=%zu cap=%u\n",
                             typeID, bucket.instances.size(), type.instanceCap);
                s_coalesceArmed = false;
                break;
            }

            // Eviction-detect.
            const TG_TypeShape* src = type.source;
            if (src && src->listOfTextures && type.packetCount > 0u &&
                type.lastSeenGosHandle != 0xFFFFFFFFu) {
                const auto& firstPkt = s_packets[type.firstPacket];
                if (firstPkt.textureSlot < src->numTextures) {
                    const DWORD now = src->listOfTextures[firstPkt.textureSlot].gosTextureHandle;
                    if (now != type.lastSeenGosHandle) {
                        std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=tex_evicted "
                                     "type=%u old_handle=%lu new_handle=%lu\n",
                                     typeID,
                                     (unsigned long)type.lastSeenGosHandle,
                                     (unsigned long)now);
                        s_coalesceArmed = false;
                        break;
                    }
                }
            }

            // Per-type-cap memcpy into group-relative slot.
            const size_t groupBase_bytes =
                (type.alphaClass == 1u) ? s_offGroupTotalBytes : 0u;
            uint8_t* dst = coalesceMapBase
                         + groupBase_bytes
                         + type.coalesceByteOffsetWithinGroup;
            std::memcpy(dst,
                        bucket.instances.data(),
                        bucket.instances.size() * sizeof(GpuStaticPropInstance));
        }
    }
    // Step 11.6 — legacy CPU write path is unchanged: it already ran via
    // uploadAllBucketsIfNeeded() above (which filled s_instanceSsbo). No
    // gating on IsCoalesceEnabled() — that lets a runtime-disarm transition
    // mid-frame still draw legally via the legacy loop below.
    const auto _btf_t_drawStart = std::chrono::steady_clock::now();
    s_btf_ns_coalesce_write += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            _btf_t_drawStart - _btf_t_writeStart).count());

    // Per-type drawing: bind per-type instance & color SSBO ranges, then
    // issue one instanced draw per packet. gl_InstanceID in the shader
    // addresses 0..N-1 within the bound range (no gl_BaseInstance needed).
    //
    // [INSTDIAG] one-shot path-state log so we know which draw path is active on the
    // tester's NVIDIA (and therefore whether the per-draw [INSTDIAG] below fires from the
    // legacy loop or whether the coalesce/MDI path is in play instead).
    {
        static const bool s_instDiag = (getenv("MC2_STATIC_INST_DIAG") != nullptr);
        static bool s_pathLogged = false;
        if (s_instDiag && !s_pathLogged) {
            s_pathLogged = true;
            fprintf(stderr,
                "[INSTDIAG] PATH coalesceEnabled=%d globalPoolLegacy=%d instanceCapacity=%u "
                "frameSlot=%u typeRanges=%zu\n",
                (int)IsCoalesceEnabled(), (int)s_globalPoolLegacy,
                (unsigned)s_instanceCapacity, (unsigned)s_frameSlot,
                s_typeRanges.size());
            fflush(stderr);
        }
    }

    // Plan v3.8 Step 11.7 / 11.8 — coalesce branch first; on disarm,
    // fall through to the legacy per-type loop with no other state change.
    if (IsCoalesceEnabled()) {
        // 2026-05-11 per-packet alpha-test re-resolve. s_perDrawSsbo's
        // materialFlags is baked at finalizeGeometry time. Damage-shape
        // textures aren't loaded until the moment of destruction (bdactor
        // setObjStatus runs the texture-load + SetTextureAlpha(true) loop
        // on demand) — so at finalize, leaf-level `textureAlpha` is false
        // and the bake leaves materialFlags=0 → no alpha-test discard.
        // After destruction, the leaf-propagation fix in
        // TG_TypeMultiShape::SetTextureAlpha (msl.cpp) updates every leaf
        // TG_TypeShape's TG_TinyTexture; this loop refreshes the SSBO so
        // the shader sees the live bit. Cost: ~packet count entries × OR-
        // reduce + glBufferSubData when changed (~10 µs/frame in mc2_10).
        {
            static std::vector<int32_t> s_lastSeenMaterialFlags;
            const size_t pktCount = s_sortedPacketOrder.size();
            if (s_lastSeenMaterialFlags.size() != pktCount) {
                s_lastSeenMaterialFlags.assign(pktCount, -1);
            }
            // SP-BATCHER-ALPHASCAN-GATE-1: the per-packet OR-reduce scan below is
            // frame-constant except immediately after a destruction/damage event
            // (which flips a type's texture alpha via TG_TypeMultiShape::SetTextureAlpha
            // and bumps g_staticPropAlphaGeneration). When the gate is armed and the
            // generation is unchanged since the last scan AND the packet count is
            // unchanged (buffer layout stable), skip the whole scan — the SSBO already
            // holds the correct flags from the last event. Default OFF => scan always
            // runs (byte-identical). First scan always runs (cached gen 0 != live >=1).
            static const bool s_alphaScanGate = []() {
                const char* v = getenv("MC2_SP_ALPHASCAN_GATE");
                return v && v[0] != '0';
            }();
            static uint64_t s_lastScannedAlphaGen = 0;
            static size_t   s_lastScannedPktCount = (size_t)-1;
            const uint64_t liveAlphaGen = g_staticPropAlphaGeneration.load(std::memory_order_relaxed);
            const bool alphaScanSkip = s_alphaScanGate &&
                s_lastScannedAlphaGen == liveAlphaGen &&
                s_lastScannedPktCount == pktCount;
            if (!alphaScanSkip) {
                s_lastScannedAlphaGen = liveAlphaGen;
                s_lastScannedPktCount = pktCount;
                bool anyChanged = false;
                for (size_t i = 0; i < pktCount; ++i) {
                    const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                    if (globalPktIdx >= s_packets.size()) continue;
                    const auto& pkt = s_packets[globalPktIdx];
                    int32_t flags = (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST)
                                    ? STATIC_PROP_FLAG_ALPHA_TEST : 0;
                    if (flags == 0 && pkt.owningTypeID < s_types.size()) {
                        const TG_TypeShape* src = s_types[pkt.owningTypeID].source;
                        if (src && src->listOfTextures &&
                            pkt.textureSlot < src->numTextures &&
                            src->listOfTextures[pkt.textureSlot].textureAlpha) {
                            flags = STATIC_PROP_FLAG_ALPHA_TEST;
                        }
                    }
                    if (s_lastSeenMaterialFlags[i] != flags) {
                        s_lastSeenMaterialFlags[i] = flags;
                        anyChanged = true;
                    }
                }
                if (anyChanged && s_perDrawSsbo) {
                    const size_t entryStride = sizeof(PerDrawEntry);
                    const size_t flagOffset  = offsetof(PerDrawEntry, materialFlags);
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perDrawSsbo);
                    for (size_t i = 0; i < pktCount; ++i) {
                        const int32_t f = s_lastSeenMaterialFlags[i];
                        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER,
                                        static_cast<GLintptr>(i * entryStride + flagOffset),
                                        sizeof(int32_t), &f);
                    }
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                }
            }
        }
        // ---- Step 11.7 coalesce draw branch ----
        // Step 4.8 — use s_coalesceFrameSlot (coalesce ring slot).
        const size_t fr_off_bytes_d =
            static_cast<size_t>(s_coalesceFrameSlot) * batcher_getCoalescePerFrameInstanceBytes();
        // Legacy mode: separate off/on group byte boundaries.
        // Global-pool mode: single contiguous range covering both groups.
        const size_t off_total_bytes = s_globalPoolLegacy
            ? s_offGroupTotalBytes
            : s_offGroupBytesThisFrame;
        const size_t on_total_bytes  = s_globalPoolLegacy
            ? (batcher_getCoalescePerFrameInstanceBytes() - s_offGroupTotalBytes)
            : s_onGroupBytesThisFrame;

        // 11.7.a — save SSBO slot 4 + unit-0 GL_TEXTURE_BINDING_2D_ARRAY.
        // Active texture is already GL_TEXTURE0 (set by prologue at line
        // ~2192). Slot 15 is NOT saved here: the draw branch never binds
        // it; Step 10.3 envelope in compute_dispatch() honors the spec
        // §3.X / §9 binding-hygiene contract for slot 15.
        GLint prevSsbo4       = 0;
        GLint prevTex2DArray  = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 4, &prevSsbo4);
        glGetIntegerv  (GL_TEXTURE_BINDING_2D_ARRAY,         &prevTex2DArray);

        // 11.7.b — legacy prologue at lines ~2192-2247 ran unconditionally
        // (slot 2 perTypeSsbo bind inherited). Verified by inspection.

        // 11.7.c — program already bound by applyPipeline() above (v1: PipelineRegistry
        // holds s_staticPropProgramCoalesce after loadProgramsIfNeeded; no explicit
        // glUseProgram needed here).

        // 11.7.d — upload shared uniforms to the coalesce program. Source
        // values match the existing legacy upload sites; -1 cached
        // locations are skipped (legacy-only uniforms removed under
        // MC2_COALESCE).
        // [MVP_DIAG v1] S2.7 — null-guard the coalesce MVP upload to match
        // the legacy path at line ~3284. RenderDoc capture (frame 488, EID
        // 7714) showed gl_Position = NaN/0 with $Globals.terrainMVP = all
        // zeros while the legacy path's u_mvp uploaded correctly; AMD
        // driver appears to zero the uniform when handed nullptr. Capture
        // pointer once, branch on it. Probe logs row0 (or "null") throttled
        // to frames {1,5,30,120} to keep silent at steady state.
        {
            extern long g_mvpDiagFrame;
            const float* mvpPtr = gos_GetObjectFixBMVPMat4();
            if (g_mvpDiagFrame == 1 || g_mvpDiagFrame == 5 ||
                g_mvpDiagFrame == 30 || g_mvpDiagFrame == 120) {
                if (mvpPtr) {
                    fprintf(stderr,
                            "[MVP_DIAG v1] event=coalesce_upload frame=%ld loc=%d ptr=%p row0=[%g %g %g %g]\n",
                            g_mvpDiagFrame, (int)s_locsCoalesce.terrainMVP,
                            (void*)mvpPtr, mvpPtr[0], mvpPtr[1], mvpPtr[2], mvpPtr[3]);
                } else {
                    fprintf(stderr,
                            "[MVP_DIAG v1] event=coalesce_upload frame=%ld loc=%d ptr=%p row0=null\n",
                            g_mvpDiagFrame, (int)s_locsCoalesce.terrainMVP, (void*)mvpPtr);
                }
                fflush(stderr);
            }
            if (s_locsCoalesce.terrainMVP >= 0 && mvpPtr)
                glUniformMatrix4fv(s_locsCoalesce.terrainMVP, 1, GL_FALSE, mvpPtr);
        }
        if (s_locsCoalesce.mvp             >= 0)
            glUniformMatrix4fv(s_locsCoalesce.mvp,           1, GL_TRUE,  gos_GetProj2ScreenMat4());
        if (s_locsCoalesce.fogValue        >= 0)
            glUniform1f       (s_locsCoalesce.fogValue,      1.0f);
        if (s_locsCoalesce.debugAddrMode   >= 0)
            glUniform1i       (s_locsCoalesce.debugAddrMode, debugAddrMode_);
        // V-AMBIENT-STATIC-1: hemisphere ambient fill strength. Default 0.0
        // (env unset or =0) -> shader hemisphere term contributes vec3(0) ->
        // byte-identical to pre-slice output. =1 -> 1.0 -> visible subtle
        // fill in shadowed faces. Skipped for window-flag nodes inside .vert.
        if (s_locsCoalesce.ambientV1Strength >= 0)
            glUniform1f       (s_locsCoalesce.ambientV1Strength,
                               s_staticPropAmbientV1Enabled ? 1.0f : 0.0f);
        // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor. Uploaded
        // globally but the frag only applies it to fragments whose instance
        // carries kFlagDecalFill (bit 3) — set solely on MarbleCliff decal
        // instances. Other props are byte-identical regardless of this value.
        if (s_locsCoalesce.terrainDecalFill >= 0)
            glUniform1f       (s_locsCoalesce.terrainDecalFill, g_terrainDecalFill);
        // TERRAIN-DECAL-COLORBLEND-1: bind the terrain colormap atlas + atlas-UV
        // params so the flagged cliff decal can pull its albedo toward the
        // surrounding terrain colour. Only touches GL texture state when the
        // feature is active (blend>0); with blend==0 (default) we still upload
        // the 0.0 scalar so the shader's flag-gated branch is a no-op and every
        // prop stays byte-identical.
        if (s_locsCoalesce.terrainDecalColorBlend >= 0)
            glUniform1f       (s_locsCoalesce.terrainDecalColorBlend, g_terrainDecalColorBlend);
        if (g_terrainDecalColorBlend > 0.0f) {
            extern GLuint gos_terrain_indirect_getAtlasGLTex();
            extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
            extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
            extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
            glActiveTexture(GL_TEXTURE0 + kColormapTexUnit);
            glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
            glActiveTexture(GL_TEXTURE0);
            if (s_locsCoalesce.terrainColormap >= 0)
                glUniform1i(s_locsCoalesce.terrainColormap, static_cast<GLint>(kColormapTexUnit));
            if (s_locsCoalesce.decalAtlasTLX >= 0)
                glUniform1f(s_locsCoalesce.decalAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
            if (s_locsCoalesce.decalAtlasTLY >= 0)
                glUniform1f(s_locsCoalesce.decalAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
            if (s_locsCoalesce.decalAtlasOOW >= 0)
                glUniform1f(s_locsCoalesce.decalAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        }
        // V-MATERIAL-DEBUG-1: per-frag material debug view. Default 0 = OFF;
        // shader skips entire debug branch when uniform == 0 (byte-identical
        // pixel invariant — proof at static_prop.frag `if (u_debugMaterialMode != 0)`).
        if (s_locsCoalesce.debugMaterialMode >= 0)
            glUniform1i       (s_locsCoalesce.debugMaterialMode,
                               s_staticPropDebugMaterialMode);
        // LIGHTING-DEBUG-VIEWS-1B: unified lighting debug channel (40-series).
        // Resolver returns -1 when MC2_LIGHTING_DEBUG_VIEW is unset/unknown ->
        // upload 0 -> shader skips all channels (pixel-invariant default).
        {
            extern int mc2LightingDebugMode();
            int lvm = mc2LightingDebugMode();
            glUniform1i(glGetUniformLocation(s_staticPropProgramCoalesce, "u_lightingDebugView"),
                        lvm < 0 ? 0 : lvm);
        }
        // MC2_SHADER_PATH_TINT: solid-color path-id debug (default 0 = OFF).
        if (s_locsCoalesce.pathTint >= 0)
            glUniform1i       (s_locsCoalesce.pathTint, mc2ShaderPathTint());
        // GREYBEARD-DIFFUSE-TEST: upload raw MC2 terrain sun for forced diffuse.
        if (s_locsCoalesce.terrainSunMC2 >= 0) {
            extern void gos_GetTerrainLightDir(float*, float*, float*);
            float tsx = 0.0f, tsy = 0.0f, tsz = 1.0f;
            gos_GetTerrainLightDir(&tsx, &tsy, &tsz);
            glUniform3f(s_locsCoalesce.terrainSunMC2, tsx, tsy, tsz);
        }
        // V-IBL-STATIC-1: SH-L2 image-based ambient. Strength gate is the env
        // var (s_iblShEnabled); when OFF -> upload 0.0 -> shader short-circuits
        // before evalShL2 (byte-identical to pre-slice output). When ON, the
        // ImGui slider g_iblShStrength modulates magnitude (range 0..3).
        // V-IBL-STATIC-2: source coeffs from the per-mission set (defaults to
        // "default" == kIblShCoeffs, so byte-identical to V-IBL-STATIC-1 when
        // no registry entry matches).
        if (s_locsCoalesce.iblSh >= 0)
            glUniform3fv      (s_locsCoalesce.iblSh, 9,
                               &s_currentShSet->coeffs[0][0]);
        if (s_locsCoalesce.iblShStrength >= 0)
            glUniform1f       (s_locsCoalesce.iblShStrength,
                               s_iblShEnabled ? g_iblShStrength : 0.0f);
        // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel specular. Default OFF;
        // strength=0.0 when env unset/=0 -> shader short-circuits (mathematical
        // proof: lit += specular * 0.0 = lit unchanged). Safety interlock:
        // if MC2_VIEW_UNIFORMS=0, force-zero regardless of gate/slider
        // (shader's #if defined(MC2_USE_VIEW_UNIFORMS) excludes the block
        // and u_cameraWorldPos read; this is belt-and-suspenders).
        if (s_locsCoalesce.pbrV1Strength >= 0) {
            const float pbrStrength =
                (s_pbrV1Enabled && !s_viewUniformsDisabled)
                    ? g_pbrV1Strength : 0.0f;
            glUniform1f(s_locsCoalesce.pbrV1Strength, pbrStrength);
        }
        // V-MATERIAL-PBR-2-TUNE-UI: upload roughness override sentinel.
        // Disabled (default) or MC2_VIEW_UNIFORMS=0 -> -1.0 -> shader falls
        // through to its literal 0.6 -> byte-identical to PBR-2-TUNE.
        if (s_locsCoalesce.pbrV1RoughnessOverride >= 0) {
            const float pbrRough =
                (g_pbrV1RoughnessOverrideEnabled && !s_viewUniformsDisabled)
                    ? g_pbrV1RoughnessOverrideValue : -1.0f;
            glUniform1f(s_locsCoalesce.pbrV1RoughnessOverride, pbrRough);
        }
        // V-MATERIAL-PBR-2-DIAG: diagnostic visualizer upload. Default 0
        // (off) -> shader runs existing PBR math unchanged.
        if (s_locsCoalesce.pbrV1DiagSunFound >= 0) {
            glUniform1i(s_locsCoalesce.pbrV1DiagSunFound,
                        s_pbrV1DiagSunFound ? 1 : 0);
        }

        // 11.7.e — slot 1 is NOT bound (per v2r18 §3.X.1: colors_.c[]
        // unread in any live shader path; coalesce branch does not bind
        // it, and the legacy bind site at the per-type loop is dead state).

        // 11.7.f — bind slot 4 (PerDraw SSBO).
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_perDrawSsbo);
        // MaterialGpu-2/v3: v3+ static_prop.frag declares MaterialTable at binding 5.
        // The terrain pass uses slot 5 for WaterRecipeBuf in a different GL program —
        // that is not a conflict (terrain re-binds WaterRecipeBuf before its own draw).
        // prevSsbo5 is saved at flush() entry and restored at flush() exit.
        if (s_materialGpuEnabled && s_materialGpuSsbo.glName != 0) {
            while (glGetError() != GL_NO_ERROR) {}  // drain stale before bind check
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, static_cast<GLuint>(s_materialGpuSsbo.glName));
            const GLenum bindErr = glGetError();
            if (bindErr != GL_NO_ERROR) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] GL ERROR after bind (real): 0x%x\n", bindErr);
                std::fputs(buf, stderr);
            }
        }

        // MaterialGpu-3: compute sampleOn once per flush.
        // sampleOn: all five conditions required.
        // s_materialGpuEnabled is default-ON; s_materialGpuSampleEnabled is default-ON (set MC2_MATERIAL_GPU_SAMPLE=0 to disable).
        // When sampleOn=0, static_prop.frag falls back to texArrayLayer (no pixel change).
        const bool sampleOn = s_materialGpuEnabled
                           && s_materialGpuSampleEnabled
                           && s_materialGpuSsbo.glName != 0
                           && s_materialGpuSidecarValid
                           && s_locsCoalesce.materialGpuSample >= 0;

        if (s_locsCoalesce.materialGpuSample >= 0) {
            glUniform1i(s_locsCoalesce.materialGpuSample, sampleOn ? 1 : 0);
            const GLenum uniformErr = glGetError();  // sample AFTER
            if (uniformErr != GL_NO_ERROR) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] GL ERROR after sample uniform: 0x%x\n",
                              uniformErr);
                std::fputs(buf, stderr);
            }
        }
        // If loc == -1: M3 error already logged at loadProgramsIfNeeded(); no-op here.

        // STATICPROP-MATERIAL-ORM-1: per-frame baseline for u_ormSampleEnable.
        // S5-B hardening: the ORM texture arrays (s_ormBucketArrays) are ONLY built
        // and bound inside the BC7-bucketed paths (guarded by s_staticPropBc7Enabled).
        // The legacy group-array paths (s_texArrayOff/On) bind albedo only and leave
        // the ORM unit holding whatever was last bound. So sampling must be enabled
        // ONLY when BOTH gates are on — gate the baseline on s_staticPropBc7Enabled
        // so every draw branch (incl. the v5 snap-cull multidraw path, which falls
        // back to legacy arrays when bc7Buckets is false) is covered at the source,
        // not just the non-bucketed else branch below. s_staticPropBc7Enabled is set
        // once at startup and never mutated, so build-time array existence and this
        // draw-time decision agree. loc-guarded → no-op until the shader declares it.
        if (s_locsCoalesce.ormSampleEnable >= 0)
            glUniform1i(s_locsCoalesce.ormSampleEnable,
                        (s_ormSlotsEnabled && s_staticPropBc7Enabled) ? 1 : 0);

        // M2: diagnostic log — first frame + every 600 frames (mirrors accumulateMonotonicAndMaybeEmit cadence).
        // C1: guard fires on every run now that s_materialGpuEnabled is default-ON (v5).
        // Set MC2_MATERIAL_GPU=0 to silence. reason cascade mirrors sampleOn condition order.
        if ((s_materialGpuEnabled || s_materialGpuSampleEnabled)
                && (s_counters.frame_count == 1
                    || (s_counters.frame_count % 600 == 0 && s_counters.frame_count > 0))) {
            const char* reason = "ok";
            if (!s_materialGpuEnabled)                    reason = "upload_env_off";
            else if (!s_materialGpuSampleEnabled)          reason = "sample_env_off";
            else if (s_materialGpuSsbo.glName == 0)        reason = "no_ssbo";
            else if (!s_materialGpuSidecarValid)           reason = "sidecar_invalid";
            else if (s_locsCoalesce.materialGpuSample < 0) reason = "uniform_missing";
            // else: reason == "ok" -> sampleOn is true (all 5 conditions met)

            char buf[96];
            if (sampleOn) {
                // sampleOn=true guarantees loc >= 0, so loc is always valid here.
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=sample_mode enabled=1 loc=%d\n",
                              s_locsCoalesce.materialGpuSample);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=sample_mode enabled=0 reason=%s\n",
                              reason);
            }
            std::fputs(buf, stderr);
        }

        // DrawPacket v5: gate-arm checks (run once per process on first gate-ON flush).
        if (s_v5Enabled && !s_v5Armed && !s_v5Disarmed) {
            s_v5Armed = true;
            s_baseInstanceSupported = (GLEW_ARB_base_instance == GL_TRUE);
            bool disarm = false;

            // Precondition 1: base-instance map must be non-null.
            // Under MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1, s_baseInstanceByCmdSsbo is
            // never allocated and s_baseInstanceByCmdMap stays nullptr.
            // Check here (not per-slot) to avoid 753x base_instance_missing masking root cause.
            if (s_baseInstanceByCmdMap == nullptr) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=disarmed reason=legacy_pool_no_baseinst_map\n");
                disarm = true;
            }

            // Precondition 2: u_drawIDBase uniform location must be valid.
            // If -1, glUniform1i is a GL no-op and the shader reads the wrong PerDrawEntry.
            if (!disarm && s_locsCoalesce.drawIDBase < 0) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] WARNING event=disarmed reason=drawid_loc_missing"
                    " loc=%d\n",
                    static_cast<int>(s_locsCoalesce.drawIDBase));
                disarm = true;
            }

            if (disarm) {
                s_v5Disarmed = true;
            } else {
                const uint32_t totalSlots = s_alphaOffCmdCount + s_alphaOnCmdCount;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=armed slots=%u ext_supported=%d"
                    " drawid_loc=%d\n",
                    totalSlots,
                    static_cast<int>(s_baseInstanceSupported),
                    static_cast<int>(s_locsCoalesce.drawIDBase));
            }
        }

        // DrawPacket v6: gate-arm checks (run once per process on first gate-ON flush).
        if (s_v6Enabled && !s_v6Armed && !s_v6Disarmed) {
            s_v6Armed = true;
            bool disarm = false;
            const char* disarmReason = nullptr;

            // Check ARB_base_instance independently — do NOT read s_baseInstanceSupported
            // (written only inside v5 arm block; may be false if v5 gate is OFF).
            if (GLEW_ARB_base_instance != GL_TRUE) {
                disarmReason = "base_instance_ext_missing";
                disarm = true;
            }

            if (!disarm && s_baseInstanceByCmdMap == nullptr) {
                disarmReason = "legacy_pool_no_baseinst_map";
                disarm = true;
            }

            if (!disarm && s_locsCoalesce.drawIDBase < 0) {
                disarmReason = "drawid_loc_missing";
                disarm = true;
            }

            if (!disarm &&
                static_cast<uint32_t>(RenderCore::PipelineId::StaticPropOpaque) >=
                static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
                disarmReason = "pipeline_enum_stale_opaque";
                disarm = true;
            }

            if (!disarm &&
                static_cast<uint32_t>(RenderCore::PipelineId::StaticPropAlphaTest) >=
                static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
                disarmReason = "pipeline_enum_stale_alphatest";
                disarm = true;
            }

            if (disarm) {
                s_v6Disarmed = true;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] event=disarmed reason=%s\n",
                    disarmReason);
            } else {
                const uint32_t totalSlots = s_alphaOffCmdCount + s_alphaOnCmdCount;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] event=armed slots=%u alpha_off=%u alpha_on=%u"
                    " ext_supported=1 drawid_loc=%d default=1\n",
                    totalSlots, s_alphaOffCmdCount, s_alphaOnCmdCount,
                    static_cast<int>(s_locsCoalesce.drawIDBase));
            }
        }

        // === M2a Task 4: StaticPopulation draw (gate MC2_STATIC_POP_SPLIT) ===
        // Emitted ONCE here, before the dynamic draw chain (runV6/runV5/BC7/
        // existing), so it is path-independent: it reuses the program + s_sharedVao
        // + depth(GEQUAL)/blend(disabled) state set by applyPipeline() at the top of
        // the draw section, and s_perDrawSsbo (slot 4, prologue), then binds its OWN
        // instance SSBO at binding 0 + its own indirect cmds and issues OFF then ON
        // multidraws. Static opaque draws before all alpha; alpha is alpha-test with
        // blend disabled, so cross-population alpha order is not a correctness
        // concern (depth-writing cutout). De-merge (reinjectPersistentStatic
        // early-out under the gate) keeps statics out of the dynamic bucket so they
        // are not double-drawn. instanceCount = full static population (first-N, NOT
        // GPU-culled — visibleIds authority is M2b).
        if (staticPopSplitArmed() && s_staticIndirectCmdBuf && !s_staticDrawCmds.empty()) {
            size_t staticTotal = 0;
            for (auto& kv : s_persistentStaticStore) staticTotal += kv.second.size();
            gpuBindSsboRange(0, s_staticInstanceSsbo, 0LL,
                (long long)((staticTotal ? staticTotal : 1) * sizeof(GpuStaticPropInstance)),
                "sp.staticInstance");
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_staticIndirectCmdBuf);
            const uintptr_t cmdSize =
                static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
            // GPU-SYNC-CONTRACT: s_staticInstanceSsbo (bound above) was filled via
            // a persistent GL_MAP_COHERENT_BIT mapping (fillStaticInstanceBufferIfDirty).
            // Order those CPU writes before the indirect draws read them. Without
            // this, NVIDIA may draw from stale instance data; AMD tolerated it.
            // (M2a path -- default-off MC2_STATIC_POP_SPLIT, latent until enabled;
            // routed through the typed helper.)
            gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::MultiDrawIndirect,
                           "staticpop_m2a_instance");
            if (s_alphaOffCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0) glUniform1i(s_locsCoalesce.drawIDBase, 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(0)),
                    static_cast<GLsizei>(s_alphaOffCmdCount), 0);
            }
            if (s_alphaOnCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(s_alphaOffCmdCount));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(s_alphaOffCmdCount) * cmdSize),
                    static_cast<GLsizei>(s_alphaOnCmdCount), 0);
            }
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            // Criterion 2: fence this draw so the next dirty in-place fill waits on it.
            if (s_staticDrawFence) glDeleteSync(s_staticDrawFence);
            s_staticDrawFence = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }

        // 2026-05-11 per-packet rework: each indirect cmd is per-PACKET, so
        // the multidraw counts and the alpha-ON byte offset use per-PACKET
        // counts (s_alphaOffCmdCount / s_alphaOnCmdCount). The PerDrawEntry
        // SSBO is also per-packet, indexed by gl_DrawID = sorted-packet slot.

        // DrawPacket v6: build packet+meta arrays then validate before dispatch.
        const bool runV6 = s_v6Enabled && !s_v6Disarmed;

        static std::vector<RenderCore::DrawPacket>  v6Packets;
        static std::vector<StaticPropDispatchMeta>  v6Meta;
        uint32_t v6LockstepViolations = 0u;
        // v8: snapshot-sole-owner decision. Set in the runV6 preamble, read again in
        // the dispatch block (a separate scope) to select dispatch + structural fallback.
        bool retireLiveBuilder = false;

        if (runV6) {
            s_v6FrameDrawsIssued        = 0u;
            s_v6FrameZeroInstSkips      = 0u;
            s_v6FrameSortedOob          = 0u;
            s_v6FramePacketOob          = 0u;
            s_v6FrameTypeOob            = 0u;
            s_v6FrameLockstepViolations = 0u;
            s_v6FrameGlErrors           = 0u;
            ++s_v6TotalFrameCount;

            // v8: retirement predicate. Consumed just below to gate the live builder,
            // set s_spBuildRetired, and emit the one-shot arm log.
            //
            // Why !s_snapCullEnabled is a condition: the snap-cull path (MC2_SNAP_CULL,
            // opt-in, default OFF) still relies on the live<->snapshot compare for slot
            // ownership in v8; it has not been ported to sole-owner packet build. Keep
            // the live builder whenever snap-cull is active until that port lands.
            const bool retireLiveBuilderPlanned =
                !s_keepLiveBuilder && s_snapshotBuildEnabled && !s_snapCullEnabled;

            // v8: snapshot is now the SOLE owner when retired. Consume the planned
            // predicate, set telemetry, emit the one-shot arm log.
            retireLiveBuilder = retireLiveBuilderPlanned;
            if (!s_v8ArmLogged) {
                s_v8ArmLogged = true;
                const char* reason =
                    retireLiveBuilder       ? "snapshot_sole_owner" :
                    s_keepLiveBuilder       ? "live_builder_forced" :
                    !s_snapshotBuildEnabled ? "snapshot_packet_build_disabled_keep_live" :
                                              "snap_cull_collision_keep_live";
                std::fprintf(stderr,
                    "[STATIC_PROP_PACKET_DISPATCH v8] event=arm live_builder_retired=%d"
                    " snapshot_packet_build=%d live_builder_forced=%d reason=%s\n",
                    retireLiveBuilder ? 1 : 0, s_snapshotBuildEnabled ? 1 : 0,
                    s_keepLiveBuilder ? 1 : 0, reason);
                std::fflush(stderr);
            }

            // v3: reset per-flush build counters so stale stats never persist.
            s_spBuildAttempted = s_spBuildCountMismatch = s_spBuildPacketMismatch =
            s_spBuildMetaMismatch = s_spBuildFallback = s_spBuildRetired = 0u;

            // v8: set retirement telemetry AFTER the unconditional reset above so it
            // is not clobbered. 1 = live builder skipped this frame (snapshot sole owner).
            s_spBuildRetired = retireLiveBuilder ? 1u : 0u;

            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            if (g_mc2HitchEnabled) {
                g_mc2HitchAccum.staticRecords  = static_cast<uint32_t>(s_bucketsByType.size());
                g_mc2HitchAccum.staticDrawCmds = totalCmds;
            }

            // Ring-slot base-instance map pointer (same arithmetic as v5).
            const size_t fr_off_bi_v6 =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi_v6);

            // When retired and the snapshot builds clean, s_v6FrameLockstepViolations
            // stays 0 from the unconditional per-flush reset above (no live build runs).
            if (!retireLiveBuilder) {
                ZoneScopedN("StaticProp.LiveBuild");
                v6LockstepViolations = buildLiveV6Arrays(totalCmds, baseInstanceMap, v6Packets, v6Meta);
                s_v6FrameLockstepViolations = v6LockstepViolations;
            }
        }

        // DrawPacket v5: per-draw-call dispatch (MC2_DRAW_PACKET_COALESCE_V5=1).
        // When active, replaces both glMultiDrawElementsIndirect calls with a
        // per-slot glDrawElementsInstancedBaseVertexBaseInstance loop.
        const bool runV5 = s_v5Enabled && s_baseInstanceSupported && !s_v5Disarmed;

        if (runV6 && v6LockstepViolations == 0u) {
            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            // v8: re-home the ring-slot base-instance map for this dispatch. The snapshot
            // build borrows baseInstance from here (NOT from the live build, which is
            // skipped when retired). Same arithmetic as the live path's baseInstanceMap.
            const size_t fr_off_bi_disp =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMapDisp = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi_disp);

            // Unconditionally bind SSBO slot 0 (same as v5).
            {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.v6a");
            }

            glActiveTexture(GL_TEXTURE0);
            // COMPRESSION-BC7-STATICPROP-2: gate ON binds per-bucket inside the
            // loop (init to an invalid bucket so the first drawn slot binds).
            // Gate OFF: original OFF→ON flip, byte-identical.
            const bool bc7Buckets = s_staticPropBc7Enabled && !s_slotBucketIndex.empty();
            int32_t curBucket = -1;  // gate-ON tracker
            if (!bc7Buckets) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
            bool enteredOnGroup = false;

            // v2.3 snap-cull: determine whether to use prev-frame snapshot for slot skipping.
            // Activation requires:
            //   - MC2_SNAP_CULL=1 and snap non-null
            //   - snap->ok == 1u (snapshot was structurally valid — v2.2 gates passed)
            //   - count matches totalCmds (snapshot covers exactly the live draw slots)
            // Warmup guard: even when all conditions above hold, if ALL instanceCounts are zero
            // (frame 1 or all-invisible scene) don't cull — avoids frame-1 blank screen.
            bool snapCullActive = false;
            uint32_t snapN      = 0u;
            if (s_snapCullEnabled &&
                snap != nullptr &&
                snap->ok == 1u &&
                snap->staticPropPackets.data != nullptr &&
                snap->staticPropPackets.count == totalCmds &&
                snap->staticPropPackets.count > 0u) {
                bool allZero = true;
                for (size_t scj = 0; scj < snap->staticPropPackets.count; ++scj) {
                    if (snap->staticPropPackets.data[scj].instanceCount != 0u) {
                        allZero = false;
                        break;
                    }
                }
                snapCullActive = !allZero;
                snapN          = static_cast<uint32_t>(snap->staticPropPackets.count);
            }

            // ---------------------------------------------------------------
            // v3 snapshot builder: activation guard → build → compare
            // ---------------------------------------------------------------
            bool snapBuilt = false;
            s_spBuildAttempted = s_snapshotBuildEnabled ? 1u : 0u;

            if (s_snapshotBuildEnabled) {
                // Stage 1: snap-cull collision — both gates active → disable v3.
                if (s_snapCullEnabled) {
                    if (!s_spBuildCollisionLoggedOnce) {
                        s_spBuildCollisionLoggedOnce = true;
                        std::fprintf(stderr,
                            "[RENDER_SNAPSHOT v3] frame=%u disabled — MC2_SNAP_CULL collision\n",
                            s_v6TotalFrameCount);
                    }
                    ++s_spBuildFallback;
                }
                // Stage 2: structural guards — snap unusable (no counter).
                else if (snap == nullptr || snap->ok != 1u ||
                         snap->staticPropPackets.data == nullptr) {
                    /* no counter — snap is unusable, not a v3 failure */
                }
                // Stage 3: v2.2 structural must be clean.
                else if (snap->spCountMismatch       != 0u ||
                         snap->spSortedSlotMismatch  != 0u ||
                         snap->spGlobalPacketMismatch != 0u ||
                         snap->spPipelineMismatch     != 0u ||
                         snap->spMaterialIdxMismatch  != 0u ||
                         snap->spTexLayerMismatch     != 0u) {
                    /* no counter — v2.2 gate failed; ordering unreliable */
                }
                // Stage 4: count mismatch → record and fall back.
                else if (snap->staticPropPackets.count != totalCmds) {
                    ++s_spBuildCountMismatch;
                    ++s_spBuildFallback;
                }
                else {
                    // All guards passed — build snapshot arrays.
                    {
                    ZoneScopedN("StaticProp.SnapshotBuild");
                    mc2_hitch::HitchScope _hitchSnap(mc2_hitch::HitchSpanKind::GpuStaticPropsSnapshot);
                    s_snapV6Packets.resize(totalCmds);
                    s_snapV6Meta.resize(totalCmds);

                    for (uint32_t si = 0u; si < totalCmds; ++si) {
                        const ExtractedStaticPropPacket& row =
                            snap->staticPropPackets.data[si];

                        // Snapshot owns slot identity — row must claim this slot.
                        if (row.sortedSlot != si) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Guard: globalPacketIdx in bounds.
                        if (row.globalPacketIdx >=
                                static_cast<uint32_t>(s_packets.size())) {
                            ++s_spBuildPacketMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        const GpuStaticPropPacket& spkt = s_packets[row.globalPacketIdx];

                        // Guard: typeId in type table.
                        if (row.typeId >= static_cast<uint32_t>(s_types.size())) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Fail closed on unknown pipelineId.
                        const uint32_t grp = pipelineId_to_group(row.pipelineId);
                        if (grp == 0xFFFFFFFFu) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // instanceCount from current-frame live (NOT snapshot prev-frame).
                        const auto typeIt = s_typeRanges.find(row.typeId);
                        const uint32_t instCount =
                            (typeIt != s_typeRanges.end())
                                ? typeIt->second.instanceCount : 0u;

                        s_snapV6Meta[si].sortedSlot      = row.sortedSlot;
                        s_snapV6Meta[si].globalPacketIdx = row.globalPacketIdx;
                        s_snapV6Meta[si].typeId          = row.typeId;
                        s_snapV6Meta[si].group           = grp;
                        s_snapV6Meta[si].instanceCount   = instCount;
                        s_snapV6Meta[si].baseInstance    = baseInstanceMapDisp[si];
                        s_snapV6Meta[si].drawIDBase      = si;
                        s_snapV6Meta[si].baseVertex      = spkt.baseVertex;

                        s_snapV6Packets[si]            = RenderCore::DrawPacket{};
                        s_snapV6Packets[si].pipelineId = static_cast<uint32_t>(
                            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                                      : RenderCore::PipelineId::StaticPropAlphaTest);
                        s_snapV6Packets[si].firstIndex = spkt.firstIndex;
                        s_snapV6Packets[si].indexCount = spkt.indexCount;
                    }

                    }  // end StaticProp.SnapshotBuild

                    // Compare snapshot-built vs live-built, field-by-field per slot.
                    // v8: skip when the live builder is retired — v6Meta/v6Packets are
                    // STALE (not rebuilt this frame), so the compare is meaningless and
                    // would set spurious mismatches. Snapshot is sole owner in that mode.
                    if (!retireLiveBuilder) {
                    ZoneScopedN("StaticProp.BuildCompare");
                    for (uint32_t ci = 0u; ci < totalCmds; ++ci) {
                        const StaticPropDispatchMeta& sm = s_snapV6Meta[ci];
                        const StaticPropDispatchMeta& lm = v6Meta[ci];
                        const RenderCore::DrawPacket&  sp = s_snapV6Packets[ci];
                        const RenderCore::DrawPacket&  lp = v6Packets[ci];

                        if (sm.globalPacketIdx != lm.globalPacketIdx) ++s_spBuildMetaMismatch;
                        if (sm.typeId          != lm.typeId)          ++s_spBuildMetaMismatch;
                        if (sm.group           != lm.group)           ++s_spBuildMetaMismatch;
                        if (sm.sortedSlot      != lm.sortedSlot)      ++s_spBuildMetaMismatch;
                        if (sm.baseVertex      != lm.baseVertex)      ++s_spBuildMetaMismatch;
                        if (sm.drawIDBase      != lm.drawIDBase)      ++s_spBuildMetaMismatch;

                        if (sp.pipelineId != lp.pipelineId) ++s_spBuildPacketMismatch;
                        if (sp.firstIndex  != lp.firstIndex)  ++s_spBuildPacketMismatch;
                        if (sp.indexCount  != lp.indexCount)  ++s_spBuildPacketMismatch;
                    }
                    }  // end StaticProp.BuildCompare

                    snapBuilt = true;
                }
            }

            // Dispatch ref-swap.
            bool useSnapshot;
            bool skipDispatch = false;
            if (retireLiveBuilder) {
                // Snapshot is sole owner. Dispatch unless STRUCTURALLY invalid.
                // snapshotInvalid == NOT built (guards failed: snap nullptr / ok!=1 /
                // count mismatch / malformed metadata). An empty-but-valid snapshot
                // (snapBuilt with totalCmds==0) is VALID → dispatch zero, NEVER fallback.
                //
                // A per-row guard failure during the snapshot build increments these
                // (and zeroes the bad slot) but still leaves snapBuilt=true. Treat any
                // such divergence as structurally invalid so we fall back to the live
                // build for the whole frame — matching the dual path's mismatch->fallback
                // safety. This also makes the spBuildFallback==0 merge gate cover it.
                const bool snapshotInvalid = !snapBuilt
                    || s_spBuildMetaMismatch   != 0u
                    || s_spBuildPacketMismatch != 0u;
                if (snapshotInvalid) {
                    // Stage 4 (count mismatch) already counted a fallback for this frame;
                    // avoid double-counting so spBuildFallback reflects frames, not events.
                    if (s_spBuildCountMismatch == 0u) ++s_spBuildFallback;
                    ZoneScopedN("StaticProp.LiveBuild.Fallback");  // rare fallback build, counted
                    v6LockstepViolations =
                        buildLiveV6Arrays(totalCmds, baseInstanceMapDisp, v6Packets, v6Meta);
                    s_v6FrameLockstepViolations = v6LockstepViolations;
                    useSnapshot = false;
                    if (v6LockstepViolations != 0u) {
                        // Fallback build itself is inconsistent — do not issue draws from a
                        // broken array this frame (matches dual-mode lockstep gating).
                        skipDispatch = true;
                    }
                } else {
                    useSnapshot = true;
                }
            } else {
                // Legacy dual path: use snapshot arrays only when compare clean.
                useSnapshot = snapBuilt
                    && s_spBuildPacketMismatch == 0
                    && s_spBuildMetaMismatch   == 0;

                if (snapBuilt && !useSnapshot) {
                    ++s_spBuildFallback;
                    if (!s_spBuildFirstFallbackLogged) {
                        s_spBuildFirstFallbackLogged = true;
                        std::fprintf(stderr,
                            "[RENDER_SNAPSHOT v3] frame=%u first-fallback"
                            " attempted=1 count_mismatch=%u pkt_mismatch=%u"
                            " meta_mismatch=%u fallback=1\n",
                            s_v6TotalFrameCount,
                            s_spBuildCountMismatch,
                            s_spBuildPacketMismatch,
                            s_spBuildMetaMismatch);
                    }
                }
            }

            const std::vector<RenderCore::DrawPacket>* pDispatchPackets =
                useSnapshot ? &s_snapV6Packets : &v6Packets;
            const std::vector<StaticPropDispatchMeta>* pDispatchMeta =
                useSnapshot ? &s_snapV6Meta    : &v6Meta;

            // FOLIAGE-STATICPROP-DEPTH-PREPASS-1 (B + C): run the depth prepass
            // over the SAME dispatch lists the color loop below draws, then flip
            // the color pass to GL_EQUAL + depthWrite-off. Gated/no-op by default
            // (flushDepthPrepassV6 returns false → color path byte-identical to
            // pre-change behavior, no re-applyPipeline issued).
            //
            // The prepass clobbers the bound program + depth state + color mask;
            // it restores the color mask itself. We re-apply the color pipeline
            // here (with the EQUAL/no-write override) so the color loop below runs
            // against the right program + depth state. SSBO slot 0 (instance
            // range) and the VAO/IBO survive (prepass does not rebind slot 0; the
            // color loop re-binds the OFF texture array via enteredOnGroup logic).
            if (!skipDispatch &&
                flushDepthPrepassV6(*pDispatchPackets, *pDispatchMeta,
                                    totalCmds, bc7Buckets, debugAddrMode_)) {
                // Color pass: keep ONLY the front-most fragment the prepass laid
                // (reverse-Z, so GEQUAL prepass stored the nearest depth). EQUAL +
                // no depth re-write. Object-ID (frag loc=2) is unaffected — it is a
                // color attachment write, not a depth-state field.
                RenderCore::PipelineDesc colorDesc =
                    RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque);
                colorDesc.depthFunc        = RenderCore::DepthFunc::Equal;
                colorDesc.depthWriteEnable = false;
                pipeline_binder::applyPipeline(colorDesc, "StaticPropOpaque");  // re-test variant (Equal depth)
                // applyPipeline rebinds the program but NOT the VAO/IBO/SSBO/
                // texture; re-assert the geometry bindings the color loop expects
                // (the prepass left the OFF texture array bound, slot-2 bound, and
                // slot 0 untouched, so this is belt-and-suspenders).
                glBindVertexArray(s_sharedVao);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
            }

            // OBJECT-TERRAIN-COPLANAR-OFFSET-1: slope-scaled depth push so object
            // bodies win the coplanar tie over true-depth terrain (gate default OFF).
            const bool s_objPO = objPolyOffsetEnabled();
            if (s_objPO) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(objPolyOffsetFactor(), objPolyOffsetUnits());
            }
            if (!skipDispatch)
            for (uint32_t i = 0u; i < totalCmds; ++i) {
                const StaticPropDispatchMeta& m  = (*pDispatchMeta)[i];
                const RenderCore::DrawPacket&  dp = (*pDispatchPackets)[i];

                // v2.3 snap-cull: skip slots whose prev-frame instanceCount was zero.
                // Per-row slot identity check guards against any snapshot/live ordering drift.
                // On identity mismatch: don't skip — count it and fall through to current-frame logic.
                if (snapCullActive && i < snapN) {
                    const ExtractedStaticPropPacket& scRow = snap->staticPropPackets.data[i];
                    if (scRow.sortedSlot != i) {
                        // Slot identity mismatch — snapshot and live order diverged.
                        ++s_snapCullSlotMismatch;
                        // Fall through to current-frame logic.
                    } else if (scRow.instanceCount == 0u) {
                        // Prev-frame zero — safe to skip.
                        ++s_snapCullSkipped;
                        continue;
                    } else {
                        // Prev-frame non-zero — draw; count as snap-cull active.
                        ++s_snapCullActive;
                    }
                }

                if (m.instanceCount == 0u) {
                    ++s_v6FrameZeroInstSkips;
                    continue;
                }

                if (bc7Buckets) {
                    // Bind this slot's bucket array (contiguous runs → at most
                    // one bind per bucket). s_slotBucketIndex is in sorted-slot
                    // order, same as the dispatch loop index i.
                    if (i < s_slotBucketIndex.size()) {
                        const int32_t b = static_cast<int32_t>(s_slotBucketIndex[i]);
                        if (b != curBucket &&
                            b >= 0 && b < static_cast<int32_t>(s_bucketArrays.size())) {
                            glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                            // STATICPROP-MATERIAL-ORM-1: bind the layer-aligned ORM
                            // sibling to kOrmTexUnit, then restore active unit 0.
                            if (s_ormSlotsEnabled &&
                                static_cast<size_t>(b) < s_ormBucketArrays.size()) {
                                glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                                glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                                glActiveTexture(GL_TEXTURE0);
                            }
                            curBucket = b;
                        }
                    }
                } else if (m.group == 1u && !enteredOnGroup) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                    enteredOnGroup = true;
                }

                glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(m.drawIDBase));

                glDrawElementsInstancedBaseVertexBaseInstance(
                    GL_TRIANGLES,
                    static_cast<GLsizei>(dp.indexCount),
                    GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(dp.firstIndex) * sizeof(uint32_t)),
                    static_cast<GLsizei>(m.instanceCount),
                    m.baseVertex,
                    m.baseInstance);

                if (s_v6TraceEnabled)
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V6] slot=%u pkt=%u type=%u group=%u"
                        " inst=%u base=%u drawID=%u first=%u count=%u baseV=%d\n",
                        m.sortedSlot, m.globalPacketIdx, m.typeId, m.group,
                        m.instanceCount, m.baseInstance, m.drawIDBase,
                        dp.firstIndex, dp.indexCount, m.baseVertex);

                ++s_v6FrameDrawsIssued;
            }

            if (s_objPO) glDisable(GL_POLYGON_OFFSET_FILL);  // OBJECT-TERRAIN-COPLANAR-OFFSET-1

            if (enteredOnGroup)
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

            {
                const GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    ++s_v6FrameGlErrors;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V6] event=gl_error frame=%u err=0x%x\n",
                        s_v6TotalFrameCount, static_cast<unsigned>(err));
                }
            }

            if ((s_v6TotalFrameCount % 600u) == 0u) {
                const uint32_t slotsConsidered = totalCmds;
                const int ok = (s_v6FrameSortedOob          == 0u &&
                                s_v6FramePacketOob           == 0u &&
                                s_v6FrameTypeOob             == 0u &&
                                s_v6FrameLockstepViolations  == 0u &&
                                s_v6FrameGlErrors            == 0u &&
                                (s_v6FrameDrawsIssued + s_v6FrameZeroInstSkips) == slotsConsidered)
                               ? 1 : 0;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] frame=%u event=dispatch_summary"
                    " slots_considered=%u draws_issued=%u zero_instance_skips=%u"
                    " sorted_oob=%u packet_oob=%u type_oob=%u lockstep_violations=%u"
                    " gl_errors=%u ok=%d\n",
                    s_v6TotalFrameCount, slotsConsidered,
                    s_v6FrameDrawsIssued, s_v6FrameZeroInstSkips,
                    s_v6FrameSortedOob, s_v6FramePacketOob, s_v6FrameTypeOob,
                    s_v6FrameLockstepViolations, s_v6FrameGlErrors, ok);
                if (s_snapshotBuildEnabled) {
                    std::fprintf(stderr,
                        "[RENDER_SNAPSHOT v3] frame=%u"
                        " attempted=%u count_mismatch=%u pkt_mismatch=%u"
                        " meta_mismatch=%u fallback=%u using_snapshot=%d\n",
                        s_v6TotalFrameCount,
                        s_spBuildAttempted,
                        s_spBuildCountMismatch,
                        s_spBuildPacketMismatch,
                        s_spBuildMetaMismatch,
                        s_spBuildFallback,
                        useSnapshot ? 1 : 0);
                }
            }

        } else if (runV5) {
            s_v5FrameDrawsIssued     = 0u;
            s_v5FrameZeroInstSkips   = 0u;
            s_v5FrameSortedOob       = 0u;
            s_v5FramePacketOob       = 0u;
            s_v5FrameTypeOob         = 0u;
            s_v5FrameBaseInstMissing = 0u;
            s_v5FrameGlErrors        = 0u;
            ++s_v5TotalFrameCount;

            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            // Compute base-instance map pointer for this ring slot.
            // Arithmetic is in BYTES: multiply ring-slot index by bytes-per-frame,
            // byte-cast the void*, then reinterpret as uint32_t*.
            // Do NOT use index arithmetic on the void* base directly.
            const size_t fr_off_bi =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi);

            // Unconditionally bind SSBO slot 0 before the loop.
            // The prologue only binds inside if(s_alphaOffCmdCount > 0u).
            // If all props are alpha-ON (OFF count == 0), slot 0 is stale.
            // v5 only runs in global-pool mode (legacy mode disarms at gate-arm).
            {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.v5");
            }

            // Bind alpha-OFF texture array before loop.
            glActiveTexture(GL_TEXTURE0);
            // COMPRESSION-BC7-STATICPROP-2: gate ON binds per-bucket in-loop.
            const bool bc7Buckets = s_staticPropBc7Enabled && !s_slotBucketIndex.empty();
            int32_t curBucket = -1;
            if (!bc7Buckets) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

            bool enteredOnGroup = false;

            for (uint32_t i = 0u; i < totalCmds; ++i) {
                if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=sorted_oob\n", i);
                    ++s_v5FrameSortedOob;
                    continue;
                }
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];

                if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=packet_oob\n", i);
                    ++s_v5FramePacketOob;
                    continue;
                }
                const GpuStaticPropPacket& pkt = s_packets[globalPktIdx];

                // Genuine OOB: owningTypeID outside the static type table.
                if (pkt.owningTypeID >= static_cast<uint32_t>(s_types.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=type_oob\n", i);
                    ++s_v5FrameTypeOob;
                    continue;
                }
                // s_typeRanges only has snapshot (instance-bearing) types.
                // Missing = zero instances this frame; not an error.
                const auto typeIt = s_typeRanges.find(pkt.owningTypeID);
                const uint32_t instanceCount =
                    (typeIt != s_typeRanges.end()) ? typeIt->second.instanceCount : 0u;

                if (instanceCount == 0u) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=zero_inst\n", i);
                    ++s_v5FrameZeroInstSkips;
                    continue;
                }
                const GLuint baseInstance = baseInstanceMap[i];

                // Group boundary: switch texture array when entering alpha-ON group.
                if (bc7Buckets) {
                    if (i < s_slotBucketIndex.size()) {
                        const int32_t b = static_cast<int32_t>(s_slotBucketIndex[i]);
                        if (b != curBucket &&
                            b >= 0 && b < static_cast<int32_t>(s_bucketArrays.size())) {
                            glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                            // STATICPROP-MATERIAL-ORM-1: bind layer-aligned ORM sibling.
                            if (s_ormSlotsEnabled &&
                                static_cast<size_t>(b) < s_ormBucketArrays.size()) {
                                glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                                glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                                glActiveTexture(GL_TEXTURE0);
                            }
                            curBucket = b;
                        }
                    }
                } else if (i == s_alphaOffCmdCount) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                    enteredOnGroup = true;
                }

                // Per-draw uniform: u_drawIDBase = absolute sorted slot.
                // In multidraw: u_drawIDBase = group-start, gl_DrawID = within-group index.
                // In per-draw:  u_drawIDBase = i,           gl_DrawID = always 0.
                // Net shader index (gl_DrawID + u_drawIDBase) is identical in both paths.
                glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(i));

                // firstIndex is uint32_t; multiply in uintptr_t space to avoid 32-bit overflow.
                glDrawElementsInstancedBaseVertexBaseInstance(
                    GL_TRIANGLES,
                    static_cast<GLsizei>(pkt.indexCount),
                    GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                    static_cast<GLsizei>(instanceCount),
                    static_cast<GLint>(pkt.baseVertex),
                    baseInstance);

                if (s_v5TraceEnabled)
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] slot=%u type=%u inst=%u base_inst=%u draw_id_base=%u\n",
                        i, pkt.owningTypeID, instanceCount, static_cast<unsigned>(baseInstance), i);

                ++s_v5FrameDrawsIssued;
            }

            // Restore texture array if loop entered the alpha-ON group.
            if (enteredOnGroup) {
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
            }

            // GL error check (one glGetError per v5-active flush).
            {
                const GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    ++s_v5FrameGlErrors;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=gl_error frame=%u err=0x%x\n",
                        s_v5TotalFrameCount, static_cast<unsigned>(err));
                }
            }

            // Per-frame summary log: every 600 frames.
            if ((s_v5TotalFrameCount % 600u) == 0u) {
                const int ok = (s_v5FrameSortedOob       == 0u &&
                                s_v5FramePacketOob        == 0u &&
                                s_v5FrameTypeOob          == 0u &&
                                s_v5FrameBaseInstMissing  == 0u &&
                                s_v5FrameGlErrors         == 0u) ? 1 : 0;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] frame=%u event=dispatch_summary"
                    " slots_considered=%u draws_issued=%u zero_instance_skips=%u"
                    " sorted_oob=%u packet_oob=%u type_oob=%u base_instance_missing=%u"
                    " gl_errors=%u ok=%d\n",
                    s_v5TotalFrameCount,
                    s_alphaOffCmdCount + s_alphaOnCmdCount,
                    s_v5FrameDrawsIssued,
                    s_v5FrameZeroInstSkips,
                    s_v5FrameSortedOob,
                    s_v5FramePacketOob,
                    s_v5FrameTypeOob,
                    s_v5FrameBaseInstMissing,
                    s_v5FrameGlErrors,
                    ok);
            }

        } else if (s_staticPropBc7Enabled && !s_slotBucketIndex.empty() &&
                   !s_bucketCmdCount.empty()) {
            // ---- COMPRESSION-BC7-STATICPROP-2 bucketed multidraw path ----
            // One glMultiDrawElementsIndirect per contiguous bucket run. Bucket
            // runs are ordered alpha-OFF buckets first, then alpha-ON (build
            // order), so alpha render ordering is preserved. drawIDBase = the
            // run's first global cmd index; the indirect-cmd buffer is indexed
            // by global sorted slot so per-run offset = cmdBase * cmdSize.
            // SSBO bind: legacy mode needs the group's sub-range; non-legacy
            // binds the full used range once. A bucket is wholly within one
            // group, so we select by the bucket's alpha group.
            glActiveTexture(GL_TEXTURE0);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
            if (!s_globalPoolLegacy) {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.bucket");
            }
            uint32_t cmdBase = 0u;
            for (uint32_t b = 0u; b < s_bucketCmdCount.size(); ++b) {
                const uint32_t count = s_bucketCmdCount[b];
                if (count == 0u) continue;
                if (b < s_bucketArrays.size() && s_bucketArrays[b] != 0u)
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                // STATICPROP-MATERIAL-ORM-1: bind the layer-aligned ORM sibling to
                // kOrmTexUnit, then restore active unit 0 for the next albedo bind.
                if (s_ormSlotsEnabled && b < s_ormBucketArrays.size() &&
                    s_ormBucketArrays[b] != 0u) {
                    glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                    glActiveTexture(GL_TEXTURE0);
                }
                if (s_globalPoolLegacy) {
                    const bool isOnGroup = (b < s_bucketInfo.size() && s_bucketInfo[b].group == 1u);
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)(fr_off_bytes_d + (isOnGroup ? off_total_bytes : 0u)),
                                     (long long)(isOnGroup ? on_total_bytes : off_total_bytes),
                                     "sp.coalesce.bucket.legacy");
                }
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(cmdBase));
                const uintptr_t runOffset =
                    static_cast<uintptr_t>(cmdBase) *
                    static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(runOffset),
                    static_cast<GLsizei>(count),
                    0);
                cmdBase += count;
            }
        } else {
            // ---- Existing multidraw path (unchanged) ----
            // STATICPROP-MATERIAL-ORM-1: this path binds NO ORM array (it uses the
            // legacy s_texArrayOff/On group arrays, not the per-bucket siblings), so
            // force u_ormSampleEnable=0 to keep the shader from sampling a stale /
            // unbound ORM unit. S5-B: the per-frame baseline above already drives this
            // to 0 whenever !s_staticPropBc7Enabled (this branch only runs in that
            // case), so this is now defensive belt-and-suspenders. loc-guarded → no-op
            // until the shader declares it.
            if (s_locsCoalesce.ormSampleEnable >= 0)
                glUniform1i(s_locsCoalesce.ormSampleEnable, 0);
            // 11.7.g — alpha-OFF group draw.
            // Step 4.8: under global-pool mode bind once covering both groups; under legacy bind per-group.
            if (s_alphaOffCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, 0);
                if (s_globalPoolLegacy) {
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)(fr_off_bytes_d + 0u),
                                     (long long)off_total_bytes,
                                     "sp.coalesce.alphaOff.legacy");
                } else {
                    const size_t totalUsed = s_totalUsedBytesThisFrame;
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)fr_off_bytes_d,
                                     (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                     "sp.coalesce.alphaOff");
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(0)),
                    static_cast<GLsizei>(s_alphaOffCmdCount),
                    0);
            }

            // 11.7.h — alpha-ON group draw.
            if (s_alphaOnCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(s_alphaOffCmdCount));
                if (s_globalPoolLegacy) {
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)(fr_off_bytes_d + off_total_bytes),
                                     (long long)on_total_bytes,
                                     "sp.coalesce.alphaOn.legacy");
                }
                // else: single bind from alpha-OFF draw still covers alpha-ON (same range).
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                const uintptr_t alphaOnOffset =
                    static_cast<uintptr_t>(s_alphaOffCmdCount) *
                    static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(alphaOnOffset),
                    static_cast<GLsizei>(s_alphaOnCmdCount),
                    0);
            }
        }

        // 11.7.i — restore indirect-buffer binding.
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        // STATICPROP-MATERIAL-ORM-1: unbind the ORM unit and restore active unit 0
        // so we leave GL state exactly as the gate-OFF path does. Only touch the
        // ORM unit when the gate is ON (no extra state churn when OFF).
        if (s_ormSlotsEnabled) {
            glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            glActiveTexture(GL_TEXTURE0);
        }

        // 11.7.j — restore slot 4 + unit-0 GL_TEXTURE_BINDING_2D_ARRAY.
        // Active texture stays GL_TEXTURE0 (epilogue restores). Slot 15
        // restore lives at the Step 10.3 envelope in compute_dispatch().
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, (GLuint)prevSsbo4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)prevTex2DArray);

        // 11.7.k — first-flush latch (per-mission). event=ready is logged
        // here, not at finalize, because finalize emits event=armed and
        // we want a separate ready signal that confirms the GPU actually
        // accepted the coalesce draw without disarm.
        if (!s_coalesceFirstFlushDone) {
            std::fprintf(stderr,
                "[COALESCE v1] event=ready buckets_off=%u buckets_on=%u "
                "cmds_off=%u cmds_on=%u\n",
                s_alphaOffCount, s_alphaOnCount,
                s_alphaOffCmdCount, s_alphaOnCmdCount);
            s_coalesceFirstFlushDone = true;
        }
    } else {
        // ---- Step 11.8 legacy per-type/per-packet draw loop (unchanged) ----

        // GPU-SYNC-CONTRACT: the legacy instance/color SSBOs were CPU-written this
        // frame through a persistent GL_MAP_COHERENT_BIT mapping (instMapBase /
        // colMapBase). The ring's glClientWaitSync handles cross-frame WAR reuse, but
        // ordering THIS frame's CPU writes BEFORE the draws reads them still needs
        // GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT. Without it, NVIDIA reads stale instance
        // data each running frame -> props invisible during gameplay, reappearing only
        // when paused (the per-frame rewrite stops). The legacy path is the ACTIVE one
        // on NVIDIA (coalesce disarms there), so this is the live fix. AMD tolerated it.
        gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                       "staticprop_legacy_instance");

        // v1: applyPipeline() above bound the coalesce program (if available).
        // Restore the legacy program before issuing legacy uniform uploads and draws.
        glUseProgram(s_staticPropProgram);

        // Direct uniforms for the legacy path. Static props use the same
        // CPU-composed terrainMVP as terrain/terrain_overlay.vert: axisSwap *
        // worldToClip, row-major rewritten in gamecam.cpp, uploaded GL_FALSE.
        {
            const GLint locTerrainMVP = glGetUniformLocation(s_staticPropProgram, "u_worldToClipGL");
            const float* terrainMVP = gos_GetObjectFixBMVPMat4();
            if (locTerrainMVP >= 0 && terrainMVP)
                glUniformMatrix4fv(locTerrainMVP, 1, GL_FALSE, terrainMVP);
            const GLint locMVP = glGetUniformLocation(s_staticPropProgram, "u_mvp");
            const float* mm = gos_GetProj2ScreenMat4();
            if (locMVP >= 0 && mm) glUniformMatrix4fv(locMVP, 1, GL_TRUE, mm);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_tex"),           0);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode"), debugAddrMode_);
            // FIXME(task-10): no clean per-scene global fog scalar source
            // available for static props; per-instance fog color is on v_fog.
            // 1.0 == "clear" per shader convention. Revisit for distance fog.
            glUniform1f(glGetUniformLocation(s_staticPropProgram, "u_fogValue"),      1.0f);
            // V-AMBIENT-STATIC-1: hemisphere ambient strength (legacy program).
            // Same semantics as coalesce site: 0.0 default -> byte-identical OFF.
            if (s_locsLegacy.ambientV1Strength >= 0)
                glUniform1f(s_locsLegacy.ambientV1Strength,
                            s_staticPropAmbientV1Enabled ? 1.0f : 0.0f);
            // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor (legacy
            // program). Frag applies it only to kFlagDecalFill (bit 3) fragments.
            if (s_locsLegacy.terrainDecalFill >= 0)
                glUniform1f(s_locsLegacy.terrainDecalFill, g_terrainDecalFill);
            // TERRAIN-DECAL-COLORBLEND-1: colormap atlas + UV params + blend
            // (legacy program). Same flag-gated no-op contract as coalesce.
            if (s_locsLegacy.terrainDecalColorBlend >= 0)
                glUniform1f(s_locsLegacy.terrainDecalColorBlend, g_terrainDecalColorBlend);
            if (g_terrainDecalColorBlend > 0.0f) {
                extern GLuint gos_terrain_indirect_getAtlasGLTex();
                extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
                extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
                extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
                glActiveTexture(GL_TEXTURE0 + kColormapTexUnit);
                glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
                glActiveTexture(GL_TEXTURE0);
                if (s_locsLegacy.terrainColormap >= 0)
                    glUniform1i(s_locsLegacy.terrainColormap, static_cast<GLint>(kColormapTexUnit));
                if (s_locsLegacy.decalAtlasTLX >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
                if (s_locsLegacy.decalAtlasTLY >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
                if (s_locsLegacy.decalAtlasOOW >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
            }
            // V-MATERIAL-DEBUG-1: per-frag material debug view (legacy program).
            // Default 0 = OFF; shader skips entire debug branch when 0.
            if (s_locsLegacy.debugMaterialMode >= 0)
                glUniform1i(s_locsLegacy.debugMaterialMode,
                            s_staticPropDebugMaterialMode);
            // LIGHTING-DEBUG-VIEWS-1B: unified lighting debug channel (legacy
            // program). Resolver -1 -> upload 0 -> pixel-invariant default.
            {
                extern int mc2LightingDebugMode();
                int lvm = mc2LightingDebugMode();
                glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_lightingDebugView"),
                            lvm < 0 ? 0 : lvm);
            }
            // MC2_SHADER_PATH_TINT: solid-color path-id debug (default 0 = OFF).
            if (s_locsLegacy.pathTint >= 0)
                glUniform1i(s_locsLegacy.pathTint, mc2ShaderPathTint());
            // GREYBEARD-DIFFUSE-TEST: upload raw MC2 terrain sun for forced diffuse.
            if (s_locsLegacy.terrainSunMC2 >= 0) {
                extern void gos_GetTerrainLightDir(float*, float*, float*);
                float tsx = 0.0f, tsy = 0.0f, tsz = 1.0f;
                gos_GetTerrainLightDir(&tsx, &tsy, &tsz);
                glUniform3f(s_locsLegacy.terrainSunMC2, tsx, tsy, tsz);
            }
            // V-IBL-STATIC-1: SH-L2 image-based ambient (legacy program). Same
            // semantics as coalesce site: strength 0.0 default -> byte-identical OFF.
            // V-IBL-STATIC-2: same per-mission source as coalesce site.
            if (s_locsLegacy.iblSh >= 0)
                glUniform3fv(s_locsLegacy.iblSh, 9,
                             &s_currentShSet->coeffs[0][0]);
            if (s_locsLegacy.iblShStrength >= 0)
                glUniform1f(s_locsLegacy.iblShStrength,
                            s_iblShEnabled ? g_iblShStrength : 0.0f);
            // V-MATERIAL-PBR-2: legacy program upload. Same gating as coalesce
            // site (env + ViewUniforms-disabled interlock). Default-OFF is
            // upload 0.0f -> shader short-circuit.
            if (s_locsLegacy.pbrV1Strength >= 0) {
                const float pbrStrength =
                    (s_pbrV1Enabled && !s_viewUniformsDisabled)
                        ? g_pbrV1Strength : 0.0f;
                glUniform1f(s_locsLegacy.pbrV1Strength, pbrStrength);
            }
            // V-MATERIAL-PBR-2-TUNE-UI: legacy program override upload (mirror).
            if (s_locsLegacy.pbrV1RoughnessOverride >= 0) {
                const float pbrRough =
                    (g_pbrV1RoughnessOverrideEnabled && !s_viewUniformsDisabled)
                        ? g_pbrV1RoughnessOverrideValue : -1.0f;
                glUniform1f(s_locsLegacy.pbrV1RoughnessOverride, pbrRough);
            }
            // V-MATERIAL-PBR-2-DIAG: legacy program diag visualizer upload.
            if (s_locsLegacy.pbrV1DiagSunFound >= 0) {
                glUniform1i(s_locsLegacy.pbrV1DiagSunFound,
                            s_pbrV1DiagSunFound ? 1 : 0);
            }
        }

    // [SEAMPROBE] stage 8: one-shot per-type draw census (which types have a
    // range / instances / packets at first legacy draw). Gated on MC2_MODOVERRIDE_TRACE.
    {
        static bool s_seamDrawCensus = false;
        if (!s_seamDrawCensus && getenv("MC2_MODOVERRIDE_TRACE")) {
            s_seamDrawCensus = true;
            for (uint32_t tid = 0; tid < s_types.size(); ++tid) {
                auto rit2 = s_typeRanges.find(tid);
                bool hasRange = (rit2 != s_typeRanges.end());
                uint32_t ic = hasRange ? rit2->second.instanceCount : 0;
                const GpuStaticPropType& ty = s_types[tid];
                if (ty.packetCount > 0 && (!hasRange || ic == 0)) {
                    fprintf(stderr, "[SEAMPROBE] draw-census typeID=%u src=%p packetCount=%u vertexCount=%u hasRange=%d instanceCount=%u (NOT DRAWN)\n",
                        tid, (void*)ty.source, ty.packetCount, ty.vertexCount, (int)hasRange, ic);
                }
            }
            fflush(stderr);
        }
    }
    for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
        auto rit = s_typeRanges.find(typeID);
        if (rit == s_typeRanges.end()) continue;
        const TypeRangeSsbo& r = rit->second;
        const GpuStaticPropType& type = s_types[typeID];
        if (r.instanceCount == 0 || type.packetCount == 0) continue;

        // Gate F: count actors that produced ≥1 packet draw this frame.
        // Per-type increment, not per-packet — we want "actors drawn,"
        // not "draw-call count."
        // STALE-COUNTER-RETIRE-1: this is the ONLY increment of gpu_drawn_instances and it
        // reads the CPU-side TypeRangeSsbo.instanceCount, which is meaningful only on this
        // legacy/per-type path. The live GPU-authority coalesce-v6 / C1b path authors
        // instance counts on the GPU (indirect buffer) and never reaches here, so the
        // counter stays 0 there. The [OBJBATCHER v1] summary carries gpu_drawn_note to say
        // so; read submitted_instances for the live signal. (Do NOT add a per-frame GPU
        // readback to "fix" this — that would stall the frame.)
        s_counters.gpu_drawn_instances += r.instanceCount;

        // [INSTDIAG] MC2_STATIC_INST_DIAG=1 — peek the EXACT instance data the GPU will
        // read at the bound offset (s_instanceMap is the coherent CPU mapping). Decisive
        // for the no-trees-on-registered-path bug: if data here is ZERO, the bucket/upload
        // dropped it (LEAFDIAG already proved recipe leaves are valid -> reinject/submit
        // bug); if VALID, the data is fine and the bug is downstream (shader/cull/ring).
        // Focus on high-instance types (trees). Default-off: zero behavior change.
        {
            static const bool s_instDiag = (getenv("MC2_STATIC_INST_DIAG") != nullptr);
            static int s_instDiagLogged = 0;
            if (s_instDiag && r.instanceCount >= 20 && s_instDiagLogged < 40 && s_instanceMap) {
                ++s_instDiagLogged;
                const uint8_t* base = static_cast<const uint8_t*>(s_instanceMap)
                                    + static_cast<size_t>(r.instanceByteOffset);
                const float* m0 = reinterpret_cast<const float*>(base);
                const size_t midOff = static_cast<size_t>(r.instanceCount / 2u)
                                    * sizeof(GpuStaticPropInstance);
                const float* mm = reinterpret_cast<const float*>(base + midOff);
                const bool z0 = (m0[0]==0.0f && m0[3]==0.0f && m0[7]==0.0f && m0[11]==0.0f);
                fprintf(stderr,
                    "[INSTDIAG] typeID=%u slot=%u instByteOff=%zu count=%u byteSize=%zu "
                    "drawData0_rawXYZ=(%.1f,%.1f,%.1f) zero0=%d midXYZ=(%.1f,%.1f,%.1f)\n",
                    typeID, (unsigned)s_frameSlot, (size_t)r.instanceByteOffset,
                    r.instanceCount, (size_t)r.instanceByteSize,
                    -m0[3], m0[11], m0[7], (int)z0, -mm[3], mm[11], mm[7]);
                fflush(stderr);
            }
        }

        gpuBindSsboRange(0, s_instanceSsbo,
                         (long long)r.instanceByteOffset, (long long)r.instanceByteSize,
                         "sp.instance");
        // STATICPROP-COLORS-FILL-DEBUGONLY-1: colorByteSize is 0 for static types
        // when the colors fill is skipped (the default). A 0-size bind is GL_INVALID_VALUE
        // — and no shader reads binding 1 — so skip it (gpuBindSsboRange would catch it too).
        if (r.colorByteSize)
            gpuBindSsboRange(1, s_colorSsbo,
                             (long long)r.colorByteOffset, (long long)r.colorByteSize,
                             "sp.color");

        // SEMANTIC: max VALID local vertex index, not count. Lets the
        // gradient debug mode hit t=1.0 at the last vertex.
        // NOTE: shader declares u_* ints (uniform uint crashes this engine's
        // shader compile); values are always positive and < 2^31, so
        // signed int is lossless here. Upload via glUniform1i.
        const int maxID = (type.vertexCount > 0u) ? (int)(type.vertexCount - 1u) : 0;
        glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_maxLocalVertexID"), maxID);

        // Slice 2 (object-offload) — Stage 2.D.1: per-type parity bind.
        //
        // The shader writes parityOut[gl_InstanceID * vertsPerType + gl_VertexID].
        // We bind a tight glBindBufferRange so that index always lands inside
        // the bound region. Each per-type draw consumes its bytes from a
        // monotonic cursor within slot s_frameSlot of the parity SSBO; if
        // the slot's budget would be exceeded by this draw, we fall back to
        // u_parityWrite=0 and the shader skips the write (the readback at
        // top of next visit then doesn't see those vertices — fine for
        // 2.D.1 acceptance, will be tightened in 2.D.2/2.D.3 if real-world
        // peak exceeds kParitySlotBytes).
        // Stage 2.D.2: the parity SSBO uses the expanded (triangle-soup) vertex
        // count so gl_VertexID in static_prop.vert stays in-bounds. gl_VertexID
        // runs 0..numTris*3-1 (per the VBO layout in registerType), so the
        // correct u_parityVertsPerType and SSBO range size is numTris*3, not
        // type.vertexCount (which is numTypeVertices, the shared-vertex count).
        const uint32_t parityVerts = (type.source && type.source->numTypeTriangles > 0u)
            ? type.source->numTypeTriangles * 3u
            : type.vertexCount;

        bool wroteParityThisDraw = false;
        if (parityBuffer != 0 && parityVerts > 0u) {
            const size_t needBytes = static_cast<size_t>(r.instanceCount) *
                                     static_cast<size_t>(parityVerts) *
                                     sizeof(uint32_t);
            // SSBO offset alignment requirement. Mirrors the per-type
            // instance/color bind alignment used by uploadAllBucketsIfNeeded
            // above (its s_ssboAlignment is function-scope, so query once
            // here too — single static, set on first hit).
            static GLint s_parityAlignment = 0;
            if (s_parityAlignment == 0) {
                glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT,
                              &s_parityAlignment);
                if (s_parityAlignment < 16) s_parityAlignment = 256;
            }
            const size_t alignMask = static_cast<size_t>(s_parityAlignment) - 1u;
            const size_t cursor = (s_parityBytesUsedThisFrame + alignMask) & ~alignMask;
            if (needBytes > 0 && cursor + needBytes <= kParitySlotBytes) {
                const GLintptr slotBase =
                    static_cast<GLintptr>(s_frameSlot) *
                    static_cast<GLintptr>(kParitySlotBytes);
                gpuBindSsboRange(3, (unsigned int)parityBuffer,
                                 (long long)(slotBase + (GLintptr)cursor),
                                 (long long)needBytes,
                                 "sp.parity");
                // u_parityVertsPerType = expanded (triangle-soup) vertex count.
                if (locParityVerts >= 0)
                    glUniform1i(locParityVerts, (int)parityVerts);
                // u_parityBaseVertex = type's VBO baseVertex; shader subtracts
                // this from gl_VertexID to get the type-local index.
                if (locParityBaseVertex >= 0) {
                    const int typeBase = (type.packetCount > 0)
                        ? s_packets[type.firstPacket].baseVertex
                        : 0;
                    glUniform1i(locParityBaseVertex, typeBase);
                }
                if (locParityWrite >= 0)
                    glUniform1i(locParityWrite, 1);
                s_parityBytesUsedThisFrame = cursor + needBytes;
                gos_object_parity::Counters_AddVerticesWrittenThisFrame(
                    static_cast<uint64_t>(r.instanceCount) *
                    static_cast<uint64_t>(parityVerts));
                wroteParityThisDraw = true;
                // Stage 2.D.2: record slot-relative cursor + expanded vertex
                // count so CompareAndReport can decode this type's bytes on
                // the readback frame. parityVerts (not type.vertexCount) here.
                if (gos_object_parity::IsDualEmitArmedAnyActor()) {
                    gos_object_parity::RecordParityTypeRange(
                        typeID, cursor, r.instanceCount, parityVerts);
                }
            } else if (needBytes > 0) {
                // Stage 2.D.1.1 (Item 1): slot budget exhausted — record it.
                // u_parityWrite stays 0 so the shader skips the write;
                // the draw still happens but no parity bytes are produced.
                gos_object_parity::Counters_AddSlotOverflowThisFrame(
                    typeID, needBytes, kParitySlotBytes);
            }
        }

        // Per-type draw-time trace: fires once per type per flush call.
        static thread_local uint32_t s_traceCount = 0;
        const bool doTypeTrace = s_texHandoffTrace && (s_traceCount < 8);
        if (doTypeTrace) ++s_traceCount;

        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
            // Resolve the texture handle at draw time. MC2 mutates
            // TG_TypeShape::listOfTextures[slot].gosTextureHandle each frame
            // in TransformMultiShape (msl.cpp:1321 via SetTextureHandle),
            // so capturing the handle at registration time gives stale
            // (usually zero) reads.
            uint32_t gosHandle = 0;
            const TG_TypeShape* src = type.source;
            if (src && src->listOfTextures && pkt.textureSlot < src->numTextures) {
                gosHandle = src->listOfTextures[pkt.textureSlot].gosTextureHandle;
            }
            const uint32_t glTexId = gos_GetGLTextureId(gosHandle);
            if (doTypeTrace && p < 2) {
                TEX_HANDOFF("flush  typeID=%u pkt=%u slot=%u src=%p gosHandle=0x%08x glTexId=%u numTex=%d",
                            typeID, p, pkt.textureSlot, (void*)src, gosHandle, glTexId,
                            src ? src->numTextures : -1);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            // Re-resolve materialFlags at draw time so the textureAlpha flag set during
            // bdactor.cpp init (after registerType) is captured. Same pattern as gosTextureHandle.
            // textureAlpha==true means the CPU path routes via MC2_DRAWALPHA; the GPU path
            // approximates this with shader discard (valid for binary-alpha fence/gate textures).
            uint32_t effectiveMaterialFlags = pkt.materialFlags;
            if (src && src->listOfTextures && pkt.textureSlot < src->numTextures &&
                src->listOfTextures[pkt.textureSlot].textureAlpha) {
                effectiveMaterialFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
            }
            ALPHA_TRACE("draw type=%u pkt=%u slot=%u pktFlags=0x%x effective=0x%x texAlpha=%d",
                        typeID, p, pkt.textureSlot, pkt.materialFlags, effectiveMaterialFlags,
                        (src && src->listOfTextures && pkt.textureSlot < src->numTextures)
                            ? (int)src->listOfTextures[pkt.textureSlot].textureAlpha : -1);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_materialFlags"),
                        (int)effectiveMaterialFlags);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_packetID"),
                        (int)(type.firstPacket + p));
            // Drain any stale GL error first so our check is clean.
            while (glGetError() != GL_NO_ERROR) {}
            if (useC1bIndirect) {
                // C1b GPU authority flip: glDrawElementsIndirect reads the command
                // at offset typeID*20 from the GPU indirect buffer. instanceCount
                // was written by the patch shader (compute_dispatch ran before flush).
                // baseVertex in the struct handles the VBO offset — no separate pass.
                // Sampler state trap: ensure REPEAT/LINEAR is bound before first
                // indirect draw so world-scale UVs don't collapse to texture edge.
                // (The per-packet texture bind above uses GL_TEXTURE_2D default
                //  sampler which inherits from prior state — explicitly set here.)
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                const GLintptr cmdOffset = static_cast<GLintptr>(typeID * 20);
                glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                       reinterpret_cast<const void*>(cmdOffset));
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            } else {
                glDrawElementsInstancedBaseVertex(
                    GL_TRIANGLES,
                    pkt.indexCount,
                    GL_UNSIGNED_INT,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                    r.instanceCount,
                    pkt.baseVertex);
            }
        }
        // Stage 2.D.1: clear u_parityWrite back to 0 so a subsequent type
        // that gets rejected by the slot-budget check (or has zero
        // vertices) can't inherit a stale write authorization against this
        // type's binding range.
        if (wroteParityThisDraw && locParityWrite >= 0) {
            glUniform1i(locParityWrite, 0);
        }
    }
    } // end Step 11.8 else (!IsCoalesceEnabled() legacy draw loop)

    s_fence[s_frameSlot] = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // Plan v3.8 Step 11.9 — coalesce fence insert after all draws issued.
    // Independent of legacy s_fence[]: tracks s_coalesceInstanceSsbo's
    // ring-slot lifecycle so the next visit (RING_FRAMES frames from now)
    // can wait on this fence before overwriting the slot's CPU writes.
    if (IsCoalesceEnabled()) {
        // Step 4.4 — use s_coalesceFrameSlot (coalesce ring), not s_frameSlot (legacy ring).
        s_coalesceFence[s_coalesceFrameSlot] = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    // Stage 2.D.1: record this slot's parity-byte usage so the next visit
    // (RING_FRAMES frames from now) knows exactly how much to glGetBufferSubData.
    s_parityBytesUsedPerSlot[s_frameSlot] = s_parityBytesUsedThisFrame;
    // Stage 2.D.2: after the fence is inserted (GPU draw committed), advance
    // the dual-emit latch from Armed → WaitingForReadback. Pass s_frameSlot
    // so the compare only fires when the ring revisits this exact slot (after
    // glClientWaitSync ensures GPU writes are complete — RING_FRAMES later).
    if (gos_object_parity::IsDualEmitArmedAnyActor()) {
        gos_object_parity::AdvanceDualEmitToWaiting(s_frameSlot);
    }

    // Restore GL state to EXACTLY what it was at flush start.
    // SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)prevSsbo1);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, (GLuint)prevSsbo2);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, (GLuint)prevSsbo3);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, (GLuint)prevSsbo5);
    // Texture binding on unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit0);
    glActiveTexture((GLenum)prevActiveTex);
    // Program / VAO / buffer bindings
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);
    // Pipeline state: no explicit restore needed — applyPipeline() set state
    // above, and gos_InvalidateRenderStateCache() below ensures the next
    // applyRenderStates() call re-applies from scratch regardless of what
    // applyPipeline left. Six glGet* roundtrips removed (render-hygiene-s1).
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);

    // Slice 2 (object-offload) — Stage 2.D.1: tick parity counters and emit
    // the 600-frame summary line if the cadence is hit. Internally a no-op
    // when MC2_OBJECT_PARITY_CHECK is unset.
    gos_object_parity::ParityFrameTick();

    // Stage 2.D.3 — round-robin sampler pick for next frame.
    //
    // Called AFTER ParityFrameTick so the summary's parity_compared_actors
    // sees the just-finalized count, and AFTER AdvanceDualEmitToWaiting so
    // the state machine has progressed for this frame. PickNextSampleIfReady:
    //   - rotates this frame's submitMultiShape observation list into the
    //     prior-frame pool (always),
    //   - if state == Done AND the pool is non-empty, picks one shape via
    //     round-robin and re-arms (state=Armed, armMode=SampledOnly,
    //     s_currentSampledShape=picked).
    // Default-off inside the sidecar; safe to call unconditionally.
    gos_object_parity::PickNextSampleIfReady();

    s_bucketsByType.clear();
    s_lastUploadedSlot = 0xFFFFFFFFu;  // reset for next frame

    // RENDER_STATES v1: this path mutates GL_TEXTURE_WRAP_S/T (lines 1700-1701
    // above) on the bound texture object — persistent state that survives
    // function exit and is not tracked by applyRenderStates' cache. Without
    // invalidation, a subsequent applyRenderStates call would early-out on
    // matching tracked-slot values while wrap stays at REPEAT even when
    // gos_State_TextureAddress=Clamp was requested. CRITICAL-1 from the
    // 2026-05-08 adversarial review of the state-equality early-out.
    gos_InvalidateRenderStateCache();

    // v2.1: snapshot per-type instance counts for next-frame extraction.
    // s_typeRanges is module-static and holds this frame's counts.
    // Reading here (before s_bucketsByType.clear) gives prior-frame
    // semantics to batcher_getDrawSlotEntry() called from ExtractRenderSnapshot().
    s_typeInstanceCountPrevFrame.clear();
    for (const auto& kv : s_typeRanges)
        s_typeInstanceCountPrevFrame[kv.first] = kv.second.instanceCount;

    // 2026-05-11 perf diag: flush() total + coalesce sub-stage timings.
    {
        const auto _btf_tEnd = std::chrono::steady_clock::now();
        const uint64_t total_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                _btf_tEnd - _btf_t0).count());
        s_btf_ns_total += total_ns;
        s_btf_ns_coalesce_draw += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                _btf_tEnd - _btf_t_drawStart).count());
        ++s_btf_calls;
        if (s_btf_enabled && (s_btf_calls % 600) == 0) {
            const double mean_total_us =
                static_cast<double>(s_btf_ns_total) / s_btf_calls / 1000.0;
            const double mean_write_us =
                static_cast<double>(s_btf_ns_coalesce_write) / s_btf_calls / 1000.0;
            const double mean_draw_us =
                static_cast<double>(s_btf_ns_coalesce_draw) / s_btf_calls / 1000.0;
            std::fprintf(stderr,
                "[BTF_TIMING v1] calls=%llu mean_total_us=%.2f "
                "mean_coalesce_write_us=%.2f mean_drawpath_us=%.2f\n",
                (unsigned long long)s_btf_calls,
                mean_total_us, mean_write_us, mean_draw_us);
            std::fflush(stderr);
        }
    }

    // FOLIAGE-STATICPROP-DEPTH-PREPASS-1 (C): the color EQUAL/no-write override is
    // applied inline above directly off flushDepthPrepassV6()'s return value — there
    // is no per-frame "prepass ran" flag to consume (review MINOR-1 removed the dead
    // s_depthPrepassRanThisFrame state that was set+reset but never read).
}

// SHADOW-PROP-ALPHA-1 -------------------------------------------------------
// Foliage/tree cards are textured quads with a binary-alpha leaf cutout. The
// legacy prop shadow caster used the empty depth frag (shadow_instanced.frag,
// `void main(){}`) so the WHOLE quad wrote depth -> square tree shadows. The
// new "shadow_static_prop_alpha" program (shadow_static_prop.vert forwarding
// a_uv + shadow_static_prop.frag) alpha-tests the card so the cast silhouette
// matches the visible foliage. MC2_SHADOW_PROP_ALPHA=0 = A/B kill switch
// (revert to the empty-frag program).
static bool shadowPropAlphaEnabled() {
    static const bool s_on = []{
        const char* v = getenv("MC2_SHADOW_PROP_ALPHA");
        return !(v && v[0] == '0');           // default ON
    }();
    return s_on;
}

// Resolve the prop shadow caster program. Returns the program handle and sets
// outIsAlpha=true iff we landed on the alpha-test program (so the caller knows
// to do the per-packet texture/material binds). Falls back to the empty-frag
// program if the alpha program failed to link this session.
static GLuint resolvePropShadowProgram(bool& outIsAlpha) {
    outIsAlpha = false;
    if (shadowPropAlphaEnabled()) {
        auto a = glsl_program::s_programs.find("shadow_static_prop_alpha");
        if (a != glsl_program::s_programs.end() && a->second && a->second->shp_) {
            outIsAlpha = true;
            return a->second->shp_;
        }
    }
    auto pit = glsl_program::s_programs.find("shadow_static_prop");
    if (pit == glsl_program::s_programs.end() || !pit->second || !pit->second->shp_)
        return 0;
    return pit->second->shp_;
}

// Per-program u_tex / u_debugAddrMode setup for the alpha-test prop shadow
// program. Sets the sampler unit (0) + mode-8 bypass parity once after
// glUseProgram. Cheap (called once per pass).
static void setupPropShadowAlphaUniforms(GLuint prog, int debugAddrMode) {
    const GLint locTex = glGetUniformLocation(prog, "u_tex");
    if (locTex >= 0) glUniform1i(locTex, 0);
    const GLint locDbg = glGetUniformLocation(prog, "u_debugAddrMode");
    if (locDbg >= 0) glUniform1i(locDbg, debugAddrMode);
}

// Bind the per-packet foliage texture (unit 0, REPEAT/LINEAR) + u_materialFlags
// for the alpha-test prop shadow program. Mirrors the LEGACY color flush loop
// (gos_static_prop_batcher.cpp ~7042-7106). Bind ONCE per packet. Opaque
// packets (ALPHA_TEST clear) still bind so the shader's `u_materialFlags &
// ALPHA_TEST_BIT` gate skips the sample correctly.
//
// NOTE: glTexId + effFlags are resolved by the CALLER (a GpuStaticPropBatcher
// member, which is a `friend` of TG_TypeShape and so may read the protected
// listOfTextures/numTextures — this free helper is not a friend).
static void bindPropShadowAlphaPacket(GLuint prog, uint32_t glTexId,
                                      uint32_t effFlags) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glTexId);
    // Sampler-state inheritance trap: the shadow pass binds no sampler object,
    // so the inherited GL_TEXTURE_2D wrap may be CLAMP (corrupting atlas-edge
    // alpha) — match the color path's explicit REPEAT.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    const GLint locFlags = glGetUniformLocation(prog, "u_materialFlags");
    if (locFlags >= 0) glUniform1i(locFlags, (int)effFlags);
}

// File-scope counters written by flushShadow() and read by Task 6 probe.
static int s_shadowTypesDrawn = 0;
static int s_shadowInstDrawn  = 0;

void GpuStaticPropBatcher::flushShadow(bool skipStaticBuildingTypes) {
    // Non-indirect, full-range, depth-only draw for GPU static props.
    // Uses the shadow_static_prop program (Task 1) and the same per-frame
    // SSBO upload as flush() (shared via uploadAllBucketsIfNeeded /
    // s_lastUploadedSlot). Never uses compute_getIndirectCmdBuf() --
    // the indirect buffer carries camera-cull-narrowed counts and must
    // not be used for a shadow pass that needs all instances.
    //
    // Spec §6.Y: IsCoalesceEnabled() is irrelevant for the shadow path;
    // the coalesce program uses gl_DrawIDARB / gl_BaseInstanceARB which
    // is not available here. Always use the legacy non-indirect path.

    // Geometry-readiness guard, mirroring the color flush() path (:2749).
    // The new txmmgr shadow region calls this from frame ~1; before
    // finalizeGeometry() s_sharedVao==0 and s_packets is empty while
    // s_typeRanges/type.packetCount can already be non-zero, so the
    // per-packet loop below would index an empty s_packets and fault in
    // glDrawElementsInstancedBaseVertex. flush() bails on the same
    // condition; flushShadow() must honor the identical precondition.
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return;

    // DEFAULT ON. Kill-switch: MC2_SHADOW_ENABLE=0.
    static const bool s_shadowEnabled = !(getenv("MC2_SHADOW_ENABLE") != nullptr &&
                                          getenv("MC2_SHADOW_ENABLE")[0] == '0');
    if (!s_shadowEnabled) return;

    if (!uploadAllBucketsIfNeeded()) return;

    // SHADOW-PROP-ALPHA-1: prefer the alpha-tested prop shadow program so
    // foliage/tree cards cast a leaf-shaped (not square) silhouette. Falls back
    // to the empty-frag program when MC2_SHADOW_PROP_ALPHA=0 or the alpha
    // program failed to link. flushShadow is the camera-visible static-prop sun
    // shadow caster (it routes tree types), so it is the primary fix site.
    bool usingAlphaProg = false;
    const GLuint shadowProg = resolvePropShadowProgram(usingAlphaProg);
    if (shadowProg == 0) return;

    if (s_typeRanges.empty()) return;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Save/restore the GL state this pass perturbs, mirroring flush()'s
    // bracket (:2844-2854 / :3509-3521). flushShadow runs BEFORE
    // gpu_cull::compute_dispatch() and the main flush(); leaking the
    // program / VAO / element-buffer / SSBO binding-0 here poisons the
    // cull dispatch and the indirect draw -> the entire prop+mech render
    // is suppressed (the invisible-casters regression). All early returns
    // above are before any GL mutation, so a single capture here + restore
    // at the one exit is correct.
    GLint prevProgram = 0, prevVao = 0, prevElemBuf = 0, prevSsbo0 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);

    glUseProgram(shadowProg);

    // SSBO-BIND-ALIGN: flushShadow binds a per-type RANGE (r.instanceByteOffset) with
    // gl_InstanceID relative to the range start, so the shared u_instBase uniform
    // (added in shadow_static_prop.vert for the building/dynamic WHOLE-buffer passes)
    // MUST be 0 here. Uniforms persist across glUseProgram, so without this reset
    // flushShadow inherits a stale non-zero base left by drawDynamicPropShadows ->
    // instances_.i[staleBase + gl_InstanceID] reads out of range -> shadows garbled.
    { const GLint ibLoc = glGetUniformLocation(shadowProg, "u_instBase");
      if (ibLoc >= 0) glUniform1i(ibLoc, 0); }

    // Upload the dynamic (per-frame) light-space matrix.
    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

    // SHADOW-PROP-ALPHA-1: one-time u_tex(=0) + u_debugAddrMode(mode-8 bypass)
    // setup for the alpha-test program. No-op for the empty-frag fallback.
    if (usingAlphaProg) setupPropShadowAlphaUniforms(shadowProg, debugAddrMode_);

    glBindVertexArray(s_sharedVao);
    // Root cause of the frame-2 0x2A18 crash: the GPU-cull/indirect flush()
    // path runs between this frame's and last frame's flushShadow and leaves
    // s_sharedVao's VAO-resident GL_ELEMENT_ARRAY_BUFFER binding at 0. An
    // indexed draw with no element buffer treats firstIndex*4 as a client
    // pointer (2694*4 == 0x2A18) and faults. Do NOT rely on the VAO carrying
    // the IBO; bind s_sharedIbo explicitly (also repairs the VAO binding).
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    const bool s_shDiag = (getenv("MC2_SHADOW_DIAG") != nullptr);
    if (s_shDiag) {
        GLint elemBuf = -1, linkOk = -1;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
        glGetProgramiv(shadowProg, GL_LINK_STATUS, &linkOk);
        GLenum ePre = glGetError();
        fprintf(stderr,
            "[SHADOW_DIAG] sp finalized=%d vao=%u isVao=%d ssbo=%u isBuf=%d "
            "elemBind=%d prog=%u link=%d errPre=0x%x typeRanges=%zu types=%zu packets=%zu\n",
            (int)s_geometryFinalized, s_sharedVao, (int)glIsVertexArray(s_sharedVao),
            s_instanceSsbo, (int)glIsBuffer(s_instanceSsbo), elemBuf, shadowProg,
            linkOk, (unsigned)ePre, s_typeRanges.size(), s_types.size(), s_packets.size());
        fflush(stderr);
    }

    int typesDrawn = 0;
    int instDrawn  = 0;

    for (uint32_t typeID = 0; typeID < static_cast<uint32_t>(s_types.size()); ++typeID) {
        auto rit = s_typeRanges.find(typeID);
        if (rit == s_typeRanges.end()) continue;
        const TypeRangeSsbo& r = rit->second;
        const GpuStaticPropType& type = s_types[typeID];
        if (r.instanceCount == 0 || type.packetCount == 0) continue;
        // SHADOW-STATIC-BUILDINGS-2: when the static building shadow map is active,
        // skip building types in the DYNAMIC pass — they already cast a crisp
        // shadow via the world-fixed static map; the dynamic copy is a redundant,
        // fuzzy double-shadow. Trees/mechs/other props still cast dynamically.
        if (skipStaticBuildingTypes && s_buildingShadowTypeIDs.count(typeID)) continue;

        // Bind the per-type instance SSBO range (binding 0 = Instances block
        // in shadow_static_prop.vert, indexed by gl_InstanceID).
        gpuBindSsboRange(0, s_instanceSsbo,
                         (long long)r.instanceByteOffset, (long long)r.instanceByteSize,
                         "sp.instance.shadow");

        // Per-packet draw -- mirrors the legacy non-indirect branch of flush().
        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const uint32_t pktIdx = type.firstPacket + p;
            if (pktIdx >= s_packets.size()) break;  // defense-in-depth: stale range
            const GpuStaticPropPacket& pkt = s_packets[pktIdx];
            // SHADOW-PROP-ALPHA-1: bind the foliage texture + material flags so
            // the alpha-test frag can cut the leaf silhouette (legacy color
            // flush parity). Skipped under the empty-frag fallback. Resolution
            // is inline here (member = TG_TypeShape friend) so the protected
            // listOfTextures/numTextures stay accessible.
            if (usingAlphaProg) {
                uint32_t shGosHandle = 0;
                const TG_TypeShape* shSrc = type.source;
                if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures)
                    shGosHandle = shSrc->listOfTextures[pkt.textureSlot].gosTextureHandle;
                uint32_t shEffFlags = pkt.materialFlags;
                if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures &&
                    shSrc->listOfTextures[pkt.textureSlot].textureAlpha)
                    shEffFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
                bindPropShadowAlphaPacket(shadowProg, gos_GetGLTextureId(shGosHandle), shEffFlags);
            }
            if (s_shDiag) {
                fprintf(stderr,
                    "[SHADOW_DIAG] sp draw type=%u byteOff=%llu byteSize=%llu instCnt=%u "
                    "firstPkt=%u pktCnt=%u pktIdx=%u idxCnt=%u firstIdx=%u baseV=%d errPre=0x%x\n",
                    typeID, (unsigned long long)r.instanceByteOffset,
                    (unsigned long long)r.instanceByteSize, r.instanceCount,
                    type.firstPacket, type.packetCount, pktIdx,
                    pkt.indexCount, pkt.firstIndex, pkt.baseVertex,
                    (unsigned)glGetError());
                fflush(stderr);
            }
            glDrawElementsInstancedBaseVertex(
                GL_TRIANGLES,
                static_cast<GLsizei>(pkt.indexCount),
                GL_UNSIGNED_INT,
                reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                static_cast<GLsizei>(r.instanceCount),
                pkt.baseVertex);
        }

        ++typesDrawn;
        instDrawn += static_cast<int>(r.instanceCount);
    }

    s_shadowTypesDrawn = typesDrawn;
    s_shadowInstDrawn  = instDrawn;

    // Restore exactly what we changed so compute_dispatch()/flush() see
    // the GL state they expect. ORDER MATTERS: restore VAO before element-
    // buffer so that glBindBuffer(ELEMENT_ARRAY_BUFFER, prevElemBuf) writes
    // into prevVao's state, not into s_sharedVao's. Reversing this order
    // (element first, then VAO) leaves s_sharedVao.elemBuf=0 — flush() then
    // binds s_sharedVao expecting s_sharedIbo to be resident and faults
    // (firstIndex*4 as client ptr, e.g. 0x2A18). flush()'s own restore uses
    // this same correct order (glBindVertexArray before glBindBuffer(ELEM)).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindVertexArray((GLuint)prevVao);                          // VAO first
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf); // then elem
    glUseProgram((GLuint)prevProgram);
}

// ---------------------------------------------------------------------------
// SHADOW-STATIC-BUILDINGS-2: world-fixed static building shadow caster pass.
// `instances` = all registered rigid-building recipe leaves (from the registry,
// visibility-INDEPENDENT). Depth-only; shadow_static_prop reads only modelMatrix
// and GpuStaticPropInstance is binary-compatible with its Instance block, so the
// registry records upload directly. Uses the STATIC world-fixed light matrix.
// ---------------------------------------------------------------------------
// s_staticBldgShadowSsbo declared near s_perTypeSsbo (so onMapUnload can free it).
static int    s_staticBldgShadowTypes = 0;
static int    s_staticBldgShadowInst  = 0;
static int    s_staticBldgShadowDraws = 0;

void GpuStaticPropBatcher::drawStaticBuildingShadows(
        const std::vector<GpuStaticPropInstance>& instances) {
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return;
    if (instances.empty()) return;

    auto pit = glsl_program::s_programs.find("shadow_static_prop");
    if (pit == glsl_program::s_programs.end() || !pit->second || !pit->second->shp_)
        return;
    const GLuint shadowProg = pit->second->shp_;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Group leaves into contiguous per-typeID runs so each type draws from one
    // SSBO range with gl_InstanceID 0..n-1. Sort an INDEX array, not the struct
    // array: GpuStaticPropInstance is alignas(16) (over-aligned), and
    // std::stable_sort's temp buffer trips MSVC's std::aligned_storage extended-
    // alignment static_assert (C2338). Index-sort (uint32_t) + gather avoids it.
    std::vector<uint32_t> order(instances.size());
    for (uint32_t k = 0; k < static_cast<uint32_t>(order.size()); ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [&instances](uint32_t a, uint32_t b) {
            return instances[a].typeID < instances[b].typeID;
        });
    std::vector<GpuStaticPropInstance> sorted;
    sorted.reserve(instances.size());
    for (uint32_t idx : order) sorted.push_back(instances[idx]);

    // Rebuild the building-typeID set so the dynamic pass can skip these (they
    // cast via this crisp static map; the dynamic copy is redundant + fuzzy).
    s_buildingShadowTypeIDs.clear();

    // Save the GL state this pass perturbs (mirror flushShadow's bracket) so the
    // subsequent dynamic shadow pass + main flush see the state they expect.
    GLint prevProgram = 0, prevVao = 0, prevElemBuf = 0, prevSsbo0 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);

    // TIER2-EXCLUDED: substrate-gated
    if (s_staticBldgShadowSsbo == 0) glGenBuffers(1, &s_staticBldgShadowSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_staticBldgShadowSsbo);
    // GPU-UPDATE-BUFFER-COUNTER-1: per-render transient shadow SSBO orphan-on-write.
    MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLsizeiptr>(sorted.size() * sizeof(GpuStaticPropInstance)),
        sorted.data(), GL_STATIC_DRAW, StaticPropShadow);

    glUseProgram(shadowProg);
    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getLightSpaceMatrix()); // static matrix

    glBindVertexArray(s_sharedVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Contact-acne bias (the static terrain prepass sets none).
    // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: enable polygon offset via the
    // ShadowStaticProp pipeline row (polygonOffsetEnable=true). applyPipeline also
    // re-asserts the identical shadow base (depth LESS, cull none, frontFace ccw,
    // depthMask) — a redundant no-op vs the bracket. glProgramName=0 → the bound
    // shadow program is untouched. The MAGNITUDE below and the teardown glDisable
    // stay exactly where they are (factor/units ownership unchanged).
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowStaticProp), "ShadowStaticProp");
    glPolygonOffset(pp->shadowBiasFactor_, pp->shadowBiasUnits_);

    int typesDrawn = 0, instDrawn = 0, drawCalls = 0;
    size_t i = 0;
    while (i < sorted.size()) {
        const uint32_t tid = sorted[i].typeID;
        size_t j = i;
        while (j < sorted.size() && sorted[j].typeID == tid) ++j;
        const uint32_t instCount = static_cast<uint32_t>(j - i);
        if (tid < static_cast<uint32_t>(s_types.size())) {
            const GpuStaticPropType& type = s_types[tid];
            if (type.packetCount > 0) {
                // SSBO-BIND-ALIGN: bind the WHOLE buffer at offset 0 (always aligned)
                // and pass the per-type instance base as a uniform. A range bind at
                // i*sizeof(GpuStaticPropInstance) (=i*112) is not a multiple of
                // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (32 on NVIDIA) -> rejected
                // with GL_INVALID_VALUE -> the SSBO never binds -> shadow casters drop.
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_staticBldgShadowSsbo);
                { const GLint locB = glGetUniformLocation(shadowProg, "u_instBase");
                  if (locB >= 0) glUniform1i(locB, (GLint)i); }
                for (uint32_t p = 0; p < type.packetCount; ++p) {
                    const uint32_t pk = type.firstPacket + p;
                    if (pk >= s_packets.size()) break;
                    const GpuStaticPropPacket& pkt = s_packets[pk];
                    glDrawElementsInstancedBaseVertex(
                        GL_TRIANGLES, static_cast<GLsizei>(pkt.indexCount), GL_UNSIGNED_INT,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                        static_cast<GLsizei>(instCount), pkt.baseVertex);
                    ++drawCalls;
                }
                ++typesDrawn;
                instDrawn += static_cast<int>(instCount);
                s_buildingShadowTypeIDs.insert(tid);  // skip in the dynamic pass
            }
        }
        i = j;
    }

    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);

    // Restore (ssbo0, VAO, elem, program — same order as flushShadow).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);

    s_staticBldgShadowTypes = typesDrawn;
    s_staticBldgShadowInst  = instDrawn;
    s_staticBldgShadowDraws = drawCalls;
    static const char* s_sbTrace = getenv("MC2_STATIC_PROP_BUILDING_SHADOW");
    if (s_sbTrace && s_sbTrace[0] == '2') {
        fprintf(stderr,
            "[SHADOW_STATIC_BLDG v1] recipes_in=%zu types=%d inst=%d draws=%d "
            "(buildings only; full registry, NOT per-frame buckets)\n",
            instances.size(), typesDrawn, instDrawn, drawCalls);
        fflush(stderr);
    }
}

// SHADOW-STABILITY-1: read-only accessor so the shadow-pass state trace
// (gameos_graphics.cpp) can emit registry-driven static-building caster
// counts without duplicating the per-element counting. Reuses the latched
// totals from the last drawStaticBuildingShadows; zero when the gate is off.
void gos_GetStaticBuildingShadowCounts(int& types, int& inst, int& draws) {
    types = s_staticBldgShadowTypes;
    inst  = s_staticBldgShadowInst;
    draws = s_staticBldgShadowDraws;
}

// ---------------------------------------------------------------------------
// SHADOW-DYNAMIC-PROP-CASTERS-1: depth-only draw of registry-supplied NON-building
// prop leaves into the currently-bound DYNAMIC shadow FBO, using the per-frame
// dynamic light matrix. Mirror of drawStaticBuildingShadows EXCEPT: (a) binds
// getDynamicLightSpaceMatrix() not the static matrix; (b) uses its own per-frame
// SSBO (s_dynamicPropShadowSsbo, GL_DYNAMIC_DRAW — rebuilt every frame); (c) does
// NOT touch s_buildingShadowTypeIDs. Replaces flushShadow's camera-visible
// s_typeRanges feed for props so EVERY registered prop casts, fixing "only
// near-camera trees shadow". Caller brackets with gos_BeginDynamicShadowPass().
static int s_dynPropShadowTypes = 0;
static int s_dynPropShadowInst  = 0;
static int s_dynPropShadowDraws = 0;

void GpuStaticPropBatcher::drawDynamicPropShadows(
        const std::vector<GpuStaticPropInstance>& instances) {
    s_dynPropShadowTypes = 0; s_dynPropShadowInst = 0; s_dynPropShadowDraws = 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return;
    if (instances.empty()) return;
    // MECH-BAY-SHADOW-CRASH (2026-06-19): skip the terrain static-prop dynamic-shadow
    // caster replay during a SimpleCamera UI preview (mech bay / purchase / mechlab).
    // After a mission ends, the prop registry still holds the prior mission's
    // instances, but their TG_TypeShape sources (type.source) are freed when the bay
    // loads preview shapes -> dereferencing type.source->listOfTextures here faults
    // (READ at 0xFFFF... in MechBayScreen::render). The preview only needs the mech's
    // own shadow (GpuMechBatcher::flushShadow), not terrain props. Crash repro:
    // ClanEagle, win mission, enter mech bay.
    if (g_mechPreviewRenderDepth > 0) return;

    // SHADOW-PROP-ALPHA-1: alpha-tested prop shadow program (foliage silhouette)
    // with empty-frag fallback. Full-registry dynamic prop caster.
    bool usingAlphaProg = false;
    const GLuint shadowProg = resolvePropShadowProgram(usingAlphaProg);
    if (shadowProg == 0) return;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Group leaves into contiguous per-typeID runs (index-sort + gather; the
    // struct is over-aligned alignas(16) so std::stable_sort on it trips MSVC's
    // aligned_storage static_assert — same constraint as drawStaticBuildingShadows).
    std::vector<uint32_t> order(instances.size());
    for (uint32_t k = 0; k < static_cast<uint32_t>(order.size()); ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [&instances](uint32_t a, uint32_t b) {
            return instances[a].typeID < instances[b].typeID;
        });
    std::vector<GpuStaticPropInstance> sorted;
    sorted.reserve(instances.size());
    for (uint32_t idx : order) sorted.push_back(instances[idx]);

    // Save the GL state this pass perturbs (mirror flushShadow's bracket).
    GLint prevProgram = 0, prevVao = 0, prevElemBuf = 0, prevSsbo0 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);

    if (s_dynamicPropShadowSsbo == 0) {
        GLuint dynShadowGen = 0;
        glGenBuffers(1, &dynShadowGen);
        s_dynamicPropShadowOwner.glName = dynShadowGen;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_dynamicPropShadowSsbo);
    // GPU-UPDATE-BUFFER-COUNTER-1: per-frame transient shadow SSBO orphan-on-write.
    const GLsizeiptr dynShadowBytes =
        static_cast<GLsizeiptr>(sorted.size() * sizeof(GpuStaticPropInstance));
    MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER,
        dynShadowBytes,
        sorted.data(), GL_DYNAMIC_DRAW, StaticPropShadow);   // rebuilt every frame
    {
        // SCENE-SSBO-OWNER-SWEEP-1: (re)register the live dynamic prop-shadow SSBO
        // (observe-only metadata; never read by the draw path). Registered at create;
        // glGenBuffers above allocates the handle once per map (Mission lifetime), and
        // the per-frame orphaning bufferData refreshes the size without a new handle.
        RenderCore::RenderResourceDesc d;
        d.id        = s_dynamicPropShadowOwner.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_dynamicPropShadowOwner.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_dynamicPropShadowOwner.debugName;
        d.glName    = s_dynamicPropShadowOwner.glName;
        d.sizeBytes = static_cast<uint64_t>(dynShadowBytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    glUseProgram(shadowProg);
    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix()); // DYNAMIC matrix

    // SHADOW-PROP-ALPHA-1: one-time u_tex(=0)+u_debugAddrMode setup (alpha prog).
    if (usingAlphaProg) setupPropShadowAlphaUniforms(shadowProg, debugAddrMode_);

    glBindVertexArray(s_sharedVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Contact-acne bias (same as the building static pass).
    // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: enable polygon offset via the
    // ShadowStaticProp pipeline row (polygonOffsetEnable=true). Re-asserts the
    // identical shadow base (redundant no-op). MAGNITUDE below + teardown glDisable
    // stay put (factor/units ownership unchanged); glProgramName=0 keeps the bound
    // shadow program.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowStaticProp), "ShadowStaticProp");
    glPolygonOffset(pp->shadowBiasFactor_, pp->shadowBiasUnits_);

    int typesDrawn = 0, instDrawn = 0, drawCalls = 0;
    size_t i = 0;
    while (i < sorted.size()) {
        const uint32_t tid = sorted[i].typeID;
        size_t j = i;
        while (j < sorted.size() && sorted[j].typeID == tid) ++j;
        const uint32_t instCount = static_cast<uint32_t>(j - i);
        if (tid < static_cast<uint32_t>(s_types.size())) {
            const GpuStaticPropType& type = s_types[tid];
            if (type.packetCount > 0) {
                // SSBO-BIND-ALIGN: bind whole buffer at offset 0 (aligned) + per-type
                // base uniform, instead of a misaligned i*112 range bind (see the
                // building shadow pass above; NVIDIA rejects misaligned SSBO offsets).
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_dynamicPropShadowSsbo);
                { const GLint locB = glGetUniformLocation(shadowProg, "u_instBase");
                  if (locB >= 0) glUniform1i(locB, (GLint)i); }
                for (uint32_t p = 0; p < type.packetCount; ++p) {
                    const uint32_t pk = type.firstPacket + p;
                    if (pk >= s_packets.size()) break;
                    const GpuStaticPropPacket& pkt = s_packets[pk];
                    // SHADOW-PROP-ALPHA-1: per-packet foliage texture + flags so
                    // the alpha-test frag cuts the leaf silhouette. Inline
                    // resolution (member = TG_TypeShape friend).
                    if (usingAlphaProg) {
                        uint32_t shGosHandle = 0;
                        const TG_TypeShape* shSrc = type.source;
                        if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures)
                            shGosHandle = shSrc->listOfTextures[pkt.textureSlot].gosTextureHandle;
                        uint32_t shEffFlags = pkt.materialFlags;
                        if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures &&
                            shSrc->listOfTextures[pkt.textureSlot].textureAlpha)
                            shEffFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
                        bindPropShadowAlphaPacket(shadowProg, gos_GetGLTextureId(shGosHandle), shEffFlags);
                    }
                    glDrawElementsInstancedBaseVertex(
                        GL_TRIANGLES, static_cast<GLsizei>(pkt.indexCount), GL_UNSIGNED_INT,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                        static_cast<GLsizei>(instCount), pkt.baseVertex);
                    ++drawCalls;
                }
                ++typesDrawn;
                instDrawn += static_cast<int>(instCount);
            }
        }
        i = j;
    }

    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);

    // Restore (ssbo0, VAO, elem, program — same order as flushShadow).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);

    s_dynPropShadowTypes = typesDrawn;
    s_dynPropShadowInst  = instDrawn;
    s_dynPropShadowDraws = drawCalls;
    static const char* s_dpTrace = getenv("MC2_SHADOW_DYNAMIC_PROP_CASTERS");
    if (s_dpTrace && s_dpTrace[0] == '2') {
        fprintf(stderr,
            "[SHADOW_DYN_PROP v1] recipes_in=%zu types=%d inst=%d draws=%d "
            "(registry props; full registry, NOT per-frame buckets)\n",
            instances.size(), typesDrawn, instDrawn, drawCalls);
        fflush(stderr);
    }
}

void GpuStaticPropBatcher::setDebugAddrMode(int mode) { debugAddrMode_ = mode; }

// SHADOW-SPINE-0: read-only accessors for static-prop shadow caster counts
// written at the end of flushShadow(). Returns last-frame values; not reset.
extern "C" uint32_t gos_getStaticPropShadowTypesDrawn() {
    return (uint32_t)s_shadowTypesDrawn;
}
extern "C" uint32_t gos_getStaticPropShadowInstDrawn() {
    return (uint32_t)s_shadowInstDrawn;
}

// ---------------------------------------------------------------------------
// Slice 2, Stage 2.A — new method and free-function definitions
// ---------------------------------------------------------------------------

// GpuStaticPropBatcher::wasLastFailureLateRegistration (lines added below)
// Returns whether the most recent submitMultiShape() call returned false
// because of a late-registration miss. The flag s_lastSubmitWasLateReg is
// cleared at the TOP of every submitMultiShape() call (before any checks)
// so the window of confusion is exactly one submit.
bool GpuStaticPropBatcher::wasLastFailureLateRegistration() const {
    return s_lastSubmitWasLateReg;
}

// Slice 2 (Stage 2.D.3) — late-registration event-counter queries.
// Static methods so the parity sidecar can call them without a singleton
// hop. The counters live in the file-static block above; they are
// monotonic since process start.
uint64_t GpuStaticPropBatcher::getAllowedLateRegEventCount() {
    return s_lateReg_allowed_events;
}
uint64_t GpuStaticPropBatcher::getDisallowedLateRegEventCount() {
    return s_lateReg_disallowed_events;
}

// GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects
// Cheap, side-effect-free eligibility check used by the addRenderShape gate
// (tgl.cpp:2522 Stage 2.B) and by *Appearance::update (Stage 2.B) before
// calling submitMultiShape. Returns true iff g_useGpuObjects is enabled AND
// every SHAPE_NODE leaf under `multi` is registered in s_typeIndex.
// MUST NOT mutate any batcher state, log, or increment counters.
bool GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects(
        const TG_MultiShape* multi) const {
    if (!g_useGpuObjects) return false;
    if (!multi) return false;

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) return false;

    for (int i = 0; i < n; ++i) {
        const TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        const TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;  // skip helpers/bones
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(child->myType);
        if (s_typeIndex.find(ts) == s_typeIndex.end()) return false;
    }
    return true;
}

// eligibleForGpuObjects — free function (declared in gos_static_prop_batcher.h)
// Per-leaf analog of isMultiShapeEligibleForGpuObjects. Called from the
// addRenderShape gate in tgl.cpp:2522 inside the bShadersDrawPathEnabled block.
// Returns true iff g_useGpuObjects is enabled AND `shape->myType` is a
// registered TG_TypeShape in s_typeIndex.
// Under default g_useGpuObjects=false this returns false unconditionally —
// the addRenderShape gate stays byte-for-byte equivalent in stock builds.
// MUST NOT mutate any batcher state.
bool eligibleForGpuObjects(TG_Shape* shape) {
    if (!g_useGpuObjects) return false;
    if (!shape) return false;
    if (!shape->myType) return false;
    if (shape->myType->GetNodeType() != SHAPE_NODE) return false;
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(shape->myType);
    return s_typeIndex.find(ts) != s_typeIndex.end();
}

void gos_GpuPropsCycleDebugMode() {
    auto& b = GpuStaticPropBatcher::instance();
    // 0..7 are the legacy bisection modes; 8 is the LODBUG probe (solid
    // magenta with alpha-test discard bypassed — see static_prop.frag).
    int next = (b.getDebugAddrMode() + 1) % 9;
    b.setDebugAddrMode(next);
}
int gos_GpuPropsGetDebugMode() {
    return GpuStaticPropBatcher::instance().getDebugAddrMode();
}

// Stage 3.C ----------------------------------------------------------------

const std::vector<GpuStaticPropInstance>& GpuStaticPropBatcher::getLastBuiltBatch() const {
    return s_lastBuiltBatch;
}

void GpuStaticPropBatcher::submitCachedInstance(const GpuStaticPropInstance& inst) {
    if (inst.typeID >= s_types.size()) return;
    // [SPFLUSH_COST_SPLIT v1] submit_map_lookup span.
    const unsigned long long _t_lookup0 = s_spflushCostSplitEnabled ? __rdtsc() : 0ULL;
    const GpuStaticPropType& type = s_types[inst.typeID];
    PerTypeBucket& bucket = s_bucketsByType[inst.typeID];
    if (s_spflushCostSplitEnabled) spflush_cost_split::AddSubmitMapLookupCycles(__rdtsc() - _t_lookup0);

    // firstColorOffset must be updated to the current bucket color position
    // so the GPU shader indexes colors correctly for this frame's layout.
    // Diagnostic counters (submitted_instances_by_pop etc.) are NOT incremented
    // for registry-injected instances — they measure the dynamic compute path.
    GpuStaticPropInstance updated = inst;
    // firstColorOffset is unused by any production shader; only meaningful when the
    // (default-skipped) colors fill runs. Keep it accurate when filling, else 0.
    updated.firstColorOffset = s_staticPropColorsFill
        ? static_cast<uint32_t>(bucket.colors.size()) : 0u;
    {
        // MC2_TREE_DIAG_TRACE diagnostic. Same format as submit() so a grep
        // diff (`grep "[TREE_DIAG]" log | sort`) lines up dyn-vs-static fields.
        static int s_staticPrinted = 0;
        if (s_treeDiagTrace && s_staticPrinted < 8) {
            ++s_staticPrinted;
            TREE_DIAG("path=stat typeID=%u flags=0x%x lightIdx=%u colOff=%u tx=%.2f ty=%.2f tz=%.2f fog=%.3f,%.3f,%.3f,%.3f hi=%.3f,%.3f,%.3f,%.3f bucketInsts=%zu",
                updated.typeID, updated.flags, updated.lightDataIndex, updated.firstColorOffset,
                // Same offset fix as submit() — translation at [3]/[7]/[11], not [12..14].
                updated.modelMatrix[3], updated.modelMatrix[7], updated.modelMatrix[11],
                updated.fogRGB[0], updated.fogRGB[1], updated.fogRGB[2], updated.fogRGB[3],
                updated.aRGBHighlight[0], updated.aRGBHighlight[1], updated.aRGBHighlight[2], updated.aRGBHighlight[3],
                bucket.instances.size());
        }
    }
    if (s_bucketOrderTrace) { if (!bucket.dbgFirst) bucket.dbgFirst = 1; ++bucket.dbgStatic; }
    bucket.instances.push_back(updated);
    // STATICPROP-COLORS-FILL-DEBUGONLY-1: default-SKIP the colors zero-fill (no
    // production shader reads colors_; addr-mode 4 reads v_argb). MC2_STATIC_PROP_COLORS_FILL=1
    // restores it. [SPFLUSH_COST_SPLIT v1] color_zero_fill span (now ~0 by default).
    const unsigned long long _t_color0 = s_spflushCostSplitEnabled ? __rdtsc() : 0ULL;
    if (s_staticPropColorsFill)
        bucket.colors.insert(bucket.colors.end(), type.vertexCount, 0u);
    if (s_spflushCostSplitEnabled) spflush_cost_split::AddColorZeroFillCycles(__rdtsc() - _t_color0);
}

void GpuStaticPropBatcher::submitCachedInstanceRange(const GpuStaticPropInstance* arr,
                                                     uint32_t count) {
    if (!arr || count == 0u) return;
    // Group consecutive same-typeID runs so the map lookup + the color resize
    // happen ONCE per run, not per leaf.
    uint32_t i = 0u;
    while (i < count) {
        const uint32_t typeID = arr[i].typeID;
        if (typeID >= s_types.size()) { ++i; continue; }
        const GpuStaticPropType& type = s_types[typeID];
        const unsigned long long _t_lk0 = s_spflushCostSplitEnabled ? __rdtsc() : 0ULL;
        PerTypeBucket& bucket = s_bucketsByType[typeID];          // one lookup per run
        if (s_spflushCostSplitEnabled) spflush_cost_split::AddSubmitMapLookupCycles(__rdtsc() - _t_lk0);
        uint32_t j = i;
        while (j < count && arr[j].typeID == typeID) ++j;
        const uint32_t n = j - i;
        // patch 2: ONE color resize/fill per run, not per-instance insert.
        // 2A REGRESSION FIX: do NOT reserve(size+n) per call — for single-leaf ranges
        // (n==1, the common case) that sets capacity to EXACTLY size+1 each call,
        // defeating std::vector geometric growth → a reallocation per leaf (O(N^2)).
        // Plain push_back (like legacy submitCachedInstance) grows geometrically.
        // STATICPROP-COLORS-FILL-DEBUGONLY-1: default-SKIP the colors zero-fill
        // (no production shader reads colors_). MC2_STATIC_PROP_COLORS_FILL=1 restores it.
        const unsigned long long _t_cf0 = s_spflushCostSplitEnabled ? __rdtsc() : 0ULL;
        const size_t colOldSize = bucket.colors.size();
        if (s_staticPropColorsFill)
            bucket.colors.resize(colOldSize + (size_t)n * type.vertexCount, 0u);
        if (s_spflushCostSplitEnabled) spflush_cost_split::AddColorZeroFillCycles(__rdtsc() - _t_cf0);
        for (uint32_t local = 0u; local < n; ++local) {
            GpuStaticPropInstance updated = arr[i + local];
            // firstColorOffset is unused by any production shader; keep it accurate
            // when filling, else 0 (running bucket offset only meaningful with fill).
            updated.firstColorOffset = s_staticPropColorsFill
                ? static_cast<uint32_t>(colOldSize + (size_t)local * type.vertexCount)
                : 0u;
            if (s_bucketOrderTrace) { if (!bucket.dbgFirst) bucket.dbgFirst = 1; ++bucket.dbgStatic; }
            bucket.instances.push_back(updated);
        }
        i = j;
    }
}

// 2b Stage 2 (Mechanism B-reinject) -----------------------------------------
uint64_t GpuStaticPropBatcher::persistentStaticGen() { return s_persistentStaticGen; }
void GpuStaticPropBatcher::setPersistentStaticGen(uint64_t g) { s_persistentStaticGen = g; }
uint64_t GpuStaticPropBatcher::persistentStaticTotalCount() {
    uint64_t n = 0; for (auto& kv : s_persistentStaticStore) n += kv.second.size(); return n;
}

// VANISH-PROBE-1: stable-order access to persistent static instance origins.
// Free functions (no header coupling) so code/gamecam.cpp can extern-declare
// them and project a real prop through worldToClipGL at the live camera angle.
uint32_t gos_ProbeStaticInstanceCount() {
    uint32_t n = 0; for (auto& kv : s_persistentStaticStore) n += (uint32_t)kv.second.size();
    return n;
}
bool gos_ProbeStaticInstanceWorld(uint32_t idx, float outXYZ[3]) {
    // unordered_map iteration order is stable between calls within a frame (no
    // mutation), which is all the probe needs. modelMatrix is row-major data
    // uploaded GL_FALSE -> the prop vertex shader reads it column-major, so the
    // instance origin (modelMatrix * vec4(0,0,0,1)) is column 3 = m[12..14].
    for (auto& kv : s_persistentStaticStore) {
        const auto& v = kv.second;
        if (idx < v.size()) {
            const float* m = v[idx].modelMatrix;
            outXYZ[0] = m[12]; outXYZ[1] = m[13]; outXYZ[2] = m[14];
            return true;
        }
        idx -= (uint32_t)v.size();
    }
    return false;
}

void GpuStaticPropBatcher::clearPersistentStatic() {
    // Keep the map + per-type vector capacity; just reset sizes (rebuild refills).
    for (auto& kv : s_persistentStaticStore) kv.second.clear();
    s_persistentStaticGen = 0xFFFFFFFFFFFFFFFFull;  // invalid until the rebuild sets it
}

// Append a contiguous run of recipes (one registry range) into the persistent
// static store, grouped by typeID. Called ONLY on a dirty generation (rare), so
// per-element push_back is fine. firstColorOffset is irrelevant (static carries 0
// colors, no shader reads it) — stored verbatim.
void GpuStaticPropBatcher::appendPersistentStaticRange(const GpuStaticPropInstance* arr,
                                                       uint32_t count) {
    if (!arr || count == 0u) return;
    uint32_t i = 0u;
    while (i < count) {
        const uint32_t typeID = arr[i].typeID;
        if (typeID >= s_types.size()) { ++i; continue; }
        uint32_t j = i;
        while (j < count && arr[j].typeID == typeID) ++j;
        auto& store = s_persistentStaticStore[typeID];
        // NOTE: do NOT reserve(size + n) here — for single-leaf runs that sets
        // capacity to EXACTLY size+1 per call, defeating geometric growth (the 2a
        // reserve-O(N^2) regression). push_back grows geometrically; dirty-only.
        for (uint32_t k = i; k < j; ++k) store.push_back(arr[k]);
        i = j;
    }
}

// Every frame: bulk-inject each persistent static block into its per-type bucket
// (one memcpy per type, NOT per leaf). Dynamic instances (if any) are appended by
// the dynamic submit path separately; static carries 0 colors so colors untouched.
void GpuStaticPropBatcher::reinjectPersistentStatic() {
    // M2a Task 4 DE-MERGE: under the population split, static props are drawn
    // from their own buffer + cmds (the static draw in flush()), so they must NOT
    // be merged into the dynamic bucket (else they double-draw). s_bucketsByType
    // becomes dynamic-only.
    if (staticPopSplitArmed()) return;
    for (auto& kv : s_persistentStaticStore) {
        const auto& block = kv.second;
        if (block.empty()) continue;
        PerTypeBucket& bucket = s_bucketsByType[kv.first];
        bucket.instances.insert(bucket.instances.end(), block.begin(), block.end());  // bulk
        if (s_bucketOrderTrace) { if (!bucket.dbgFirst) bucket.dbgFirst = 1; bucket.dbgStatic += (uint32_t)block.size(); }
    }
}

// Track B -----------------------------------------------------------------

bool GpuStaticPropBatcher::buildRecipeFromShape(
        TG_Shape* shape,
        const Stuff::Matrix4D& shapeToWorld,
        uint32_t highlightARGB,
        uint32_t fogARGB,
        uint32_t flags,
        GpuStaticPropInstance* outRecipe) const {
    if (!shape || !outRecipe) return false;

    TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);
    if (!typeShape) return false;

    auto it = s_typeIndex.find(typeShape);
    if (it == s_typeIndex.end()) return false;

    GpuStaticPropInstance inst{};
    // Matrix4D is a plain row-major Scalar[16] (see stuff/matrix.hpp). Copy
    // as-is; shader uploads the worldToClip uniform with GL_FALSE.
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    inst.typeID           = it->second;
    inst.firstColorOffset = 0u;          // placeholder; submitCachedInstance patches per-frame
    inst.flags            = flags;
    inst.lightDataIndex   = 0xFFFFFFFFu; // sentinel; flush() patches per-frame
    inst.aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    inst.aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    inst.aRGBHighlight[2] = ((highlightARGB >>  0) & 0xFF) / 255.0f;
    inst.aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    inst.fogRGB[0] = ((fogARGB >> 16) & 0xFF) / 255.0f;
    inst.fogRGB[1] = ((fogARGB >>  8) & 0xFF) / 255.0f;
    inst.fogRGB[2] = ((fogARGB >>  0) & 0xFF) / 255.0f;
    inst.fogRGB[3] = ((fogARGB >> 24) & 0xFF) / 255.0f;

    *outRecipe = inst;
    return true;
}

// ---------------------------------------------------------------------------
// C1b free-function accessors (Track C slice C1b).
// Expose immutable per-type geometry for gpu_cull_compute.cpp's indirect buffer
// construction. Valid only after finalizeGeometry().
// ---------------------------------------------------------------------------

uint32_t batcher_getTypeCount() {
    return static_cast<uint32_t>(s_types.size());
}

// [PLACE_TYPE_TEST] DIAG: is this leaf TG_TypeShape* registered (in the finalized
// VBO via s_typeIndex), and/or did it hit the late-register drop (s_failedTypes)?
// Lets the placement site report whether the EXACT shape it draws was primed.
// EDITOR-STATIC-RECIPE-FROM-TYPE-1: resolve a leaf TG_TypeShape* to its typeID
// in the finalized type table. Lets the editor re-point a runtime placement's
// recipe at the canonical (primed, geometry-bearing) TYPE leaf instead of the
// post-finalize CreateFrom instance copy. Returns false if not registered.
bool batcher_typeIdForTypeShape(const void* typeShape, uint32_t* outTypeId) {
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(typeShape);
    if (!ts) return false;
    auto it = s_typeIndex.find(ts);
    if (it == s_typeIndex.end()) return false;
    if (outTypeId) *outTypeId = it->second;
    return true;
}

void batcher_queryTypeShape(const void* typeShape, int* outRegistered, int* outFailed) {
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(typeShape);
    if (outRegistered) *outRegistered = (ts && s_typeIndex.count(ts)) ? 1 : 0;
    if (outFailed) {
        auto it = ts ? s_failedTypes.find(ts) : s_failedTypes.end();
        *outFailed = (it != s_failedTypes.end() && it->second) ? 1 : 0;
    }
}


bool batcher_getTypeDrawInfo(uint32_t  typeID,
                              uint32_t* outIndexCount,
                              uint32_t* outFirstIndex,
                              int32_t*  outBaseVertex,
                              uint32_t* outInstanceCap) {
    if (!s_geometryFinalized) return false;
    if (typeID >= s_types.size()) return false;

    const GpuStaticPropType& type = s_types[typeID];

    // Sum index count across all packets for this type (for correct
    // glMultiDrawElementsIndirect — each bucket covers all packets).
    // For C1b's first-pass implementation we use a single draw command
    // per type covering ALL packets as one contiguous draw. This works
    // when all packets for a type are contiguous in the IBO, which is
    // guaranteed by registerType()'s sequential append.
    uint32_t totalIndexCount = 0;
    uint32_t firstPktFirstIndex = 0;
    int32_t  firstPktBaseVertex = 0;
    for (uint32_t p = 0; p < type.packetCount; ++p) {
        const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
        totalIndexCount += pkt.indexCount;
        if (p == 0) {
            firstPktFirstIndex = pkt.firstIndex;
            firstPktBaseVertex = pkt.baseVertex;
        }
    }

    if (outIndexCount)  *outIndexCount  = totalIndexCount;
    if (outFirstIndex)  *outFirstIndex  = firstPktFirstIndex;
    if (outBaseVertex)  *outBaseVertex  = firstPktBaseVertex;
    // Per-bucket capacity: s_instanceCapacity is the per-frame total;
    // use it as the upper bound for each bucket (conservative — sums to
    // N*s_instanceCapacity for the visibleIds[] buffer, which is large but
    // safe; actual peak is at most s_instanceCapacity total).
    if (outInstanceCap) *outInstanceCap = static_cast<uint32_t>(s_instanceCapacity > 0
                                            ? s_instanceCapacity : INITIAL_INSTANCES_PER_FRAME);
    return true;
}

// ---------------------------------------------------------------------------
// §5.6b accessor bodies (plan v3.8 Step 1.2a — deferred from Step group 1
// because the bodies reference Step 2.1 file-scope statics).
// All return safe sentinels (0 / nullptr / empty / false) before
// finalizeGeometry() runs so a caller-side null-deref or stale-handle bind
// is impossible during init ordering.
// ---------------------------------------------------------------------------

const uint32_t* batcher_getSortedTypeOrder() {
    return (s_geometryFinalized && !s_sortedTypeOrder.empty())
           ? s_sortedTypeOrder.data()
           : nullptr;
}

uint32_t batcher_getAlphaOffCount() { return s_alphaOffCount; }
uint32_t batcher_getAlphaOnCount()  { return s_alphaOnCount;  }

const uint32_t* batcher_getSortedPacketOrder() {
    return (s_geometryFinalized && !s_sortedPacketOrder.empty())
           ? s_sortedPacketOrder.data()
           : nullptr;
}
uint32_t batcher_getSortedPacketCount() {
    return static_cast<uint32_t>(s_sortedPacketOrder.size());
}
uint32_t batcher_getAlphaOffCmdCount() { return s_alphaOffCmdCount; }
uint32_t batcher_getAlphaOnCmdCount()  { return s_alphaOnCmdCount;  }
GLuint   batcher_getCmdToBucketSsbo()  { return s_cmdToBucketSsbo;  }

bool batcher_getPacketDrawInfo(uint32_t globalPacketIdx,
                                uint32_t* outIndexCount,
                                uint32_t* outFirstIndex,
                                int32_t*  outBaseVertex,
                                uint32_t* outOwningTypeID) {
    if (!s_geometryFinalized) return false;
    if (globalPacketIdx >= s_packets.size()) return false;
    const auto& pkt = s_packets[globalPacketIdx];
    if (outIndexCount)   *outIndexCount   = pkt.indexCount;
    if (outFirstIndex)   *outFirstIndex   = pkt.firstIndex;
    if (outBaseVertex)   *outBaseVertex   = pkt.baseVertex;
    if (outOwningTypeID) *outOwningTypeID = pkt.owningTypeID;
    return true;
}

bool batcher_getPacketMaterialFlags(uint32_t globalPacketIdx,
                                     uint32_t* outMaterialFlags) {
    if (!s_geometryFinalized) return false;
    if (globalPacketIdx >= s_packets.size()) return false;
    if (outMaterialFlags) *outMaterialFlags = s_packets[globalPacketIdx].materialFlags;
    return true;
}

uint32_t batcher_getInstanceCap(uint32_t typeID) {
    if (!s_geometryFinalized || typeID >= s_types.size()) return 0;
    return s_types[typeID].instanceCap;
}

// Slice-A cut-off upper-bound oracle: per-type LIVE instance count from the
// last upload (= the frozen draw-pool span the indirect draw renders from).
// s_typeRanges is rebuilt every upload (uploadAllBucketsIfNeeded :4538) with
// instanceCount = bucket.instances.size(); reading it gives an authoritative,
// timing-stable denominator (no per-frame bucket clear race). For frozen static
// props it equals this frame's pool span.
uint32_t batcher_getTypeUploadedInstanceCount(uint32_t typeID) {
    auto it = s_typeRanges.find(typeID);
    if (it == s_typeRanges.end()) return 0u;
    return it->second.instanceCount;
}

// M1 FROZEN-STATIC-CULL-RECORDS: module-scope copy of the per-type global
// instance-pool base (alpha-group prefix-sum over s_sortedTypeOrder), published
// by batcher_prepareBaseInstanceTable(). global_slot(typeID, store_rank) ==
// s_baseInstanceForType[typeID] + store_rank, the exact binding-0 slot the draw
// indexes. Valid only after prepareBaseInstanceTable() runs in global-pool armed
// mode (empty under legacy/disarmed). The golden static cull-record build reads
// it to place each record at its instance-pool slot (record-index == pool-slot).
static std::vector<uint32_t> s_baseInstanceForType;

// ===========================================================================
// M2a POPULATION-SPLIT (gate MC2_STATIC_POP_SPLIT, requires G2). De-merge the
// persistent static instances out of the per-frame dynamic bucket so the
// static cull-record golden has a STABLE per-type base (the merged base shifts
// per-frame when dynamics share a static type's bucket -> the outOfRange 0->71
// defect). Tasks 1+2: static-only base table + own front-packed instance SSBO.
// Default-off => byte-identical (every new path gates on staticPopSplitArmed()).
// ===========================================================================

// Static-only per-type base (s_staticBaseInstanceForType, defined early near
// s_globalInstanceCap): prefix-sum over s_persistentStaticStore[t].size() in the
// SAME sorted+alpha order as s_baseInstanceForType, but counting ONLY persistent-
// static instances (no dynamics). The stable base the golden scatter must use.
// Dirty-gated on s_persistentStaticGen via s_staticBaseBuiltGen.
static void rebuildStaticBaseInstanceTableIfDirty() {
    if (s_staticBaseBuiltGen == s_persistentStaticGen) return;  // dirty-gated
    s_staticBaseInstanceForType.assign(s_types.size(), 0u);
    std::array<uint32_t, 2> groupCursor = {0u, 0u};
    for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
        const uint32_t typeID = s_sortedTypeOrder[i];
        if (typeID >= s_types.size()) continue;
        auto it = s_persistentStaticStore.find(typeID);
        const uint32_t cnt = (it != s_persistentStaticStore.end())
                                 ? static_cast<uint32_t>(it->second.size()) : 0u;
        const uint32_t group = s_types[typeID].alphaClass;
        s_staticBaseInstanceForType[typeID] = groupCursor[group];
        groupCursor[group] += cnt;
    }
    const uint32_t offGroupCount = groupCursor[0];
    for (uint32_t typeID : s_sortedTypeOrder) {
        if (typeID < s_types.size() && s_types[typeID].alphaClass == 1u)
            s_staticBaseInstanceForType[typeID] += offGroupCount;
    }
    s_staticBaseBuiltGen = s_persistentStaticGen;
}

uint32_t batcher_getStaticBaseInstanceForType(uint32_t typeID) {
    return (typeID < s_staticBaseInstanceForType.size())
               ? s_staticBaseInstanceForType[typeID] : 0u;
}

// StaticPopulation instance buffer (Task 2; state s_staticInstanceSsbo/Map/Bytes/
// FillGen defined early near s_globalInstanceCap): persistent-mapped, SINGLE
// region (static is frozen -> no triple-buffer ring). Sized ONCE at the global
// cap so growth never reallocates an immutable buffer mid-flight. Filled
// [0,total) once per dirty in static-base layout.
static bool ensureStaticInstanceCapacity() {
    if (s_staticInstanceSsbo) return s_staticInstanceMap != nullptr;
    const size_t want = static_cast<size_t>(s_globalInstanceCap)
                        * sizeof(GpuStaticPropInstance);
    // TIER2-EXCLUDED: dead-path
    glGenBuffers(1, &s_staticInstanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_staticInstanceSsbo);
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    while (glGetError() != GL_NO_ERROR) {}
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(want), nullptr, flags);
    if (glGetError() != GL_NO_ERROR) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glDeleteBuffers(1, &s_staticInstanceSsbo);
        s_staticInstanceSsbo = 0;
        std::fprintf(stderr, "[STATIC_POP_SPLIT] event=alloc_failed bytes=%zu\n", want);
        return false;
    }
    s_staticInstanceMap = MC2_GL_MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                           static_cast<GLsizeiptr>(want), flags);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    s_staticInstanceBytes = want;
    return s_staticInstanceMap != nullptr;
}

static void fillStaticInstanceBufferIfDirty() {
    if (s_staticInstanceFillGen == s_persistentStaticGen) return;
    rebuildStaticBaseInstanceTableIfDirty();
    size_t total = 0;
    for (auto& kv : s_persistentStaticStore) total += kv.second.size();
    if (total > static_cast<size_t>(s_globalInstanceCap)) {
        std::fprintf(stderr, "[STATIC_POP_SPLIT] event=overflow total=%zu cap=%u\n",
                     total, s_globalInstanceCap);
        return;  // never write past the cap
    }
    if (!ensureStaticInstanceCapacity()) return;
    // Criterion 2: the buffer is overwritten in place — wait for the prior
    // static draw to drain before clobbering it (dirties are rare, so the stall
    // is rare; GPU in-order execution makes one wait cover all earlier frames).
    if (s_staticDrawFence) {
        MC2_GL_ClientWaitSync(s_staticDrawFence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_staticDrawFence);
        s_staticDrawFence = nullptr;
    }
    auto* dst = static_cast<GpuStaticPropInstance*>(s_staticInstanceMap);
    for (auto& kv : s_persistentStaticStore) {
        const uint32_t typeID = kv.first;
        if (typeID >= s_staticBaseInstanceForType.size()) continue;
        const uint32_t base = s_staticBaseInstanceForType[typeID];
        const auto& vec = kv.second;
        for (uint32_t r = 0; r < vec.size(); ++r) dst[base + r] = vec[r];
    }
    s_staticInstanceFillGen = s_persistentStaticGen;
}

GLuint batcher_getStaticInstanceSsbo() { return s_staticInstanceSsbo; }

// Task 3: build the StaticPopulation indirect cmds, one per sorted packet (same
// [alpha-OFF | alpha-ON] order as s_sortedPacketOrder, so the Task-4 draw splits
// at s_alphaOffCmdCount exactly like the dynamic draw). count/firstIndex/
// baseVertex are geometry-static; baseInstance = the type's STABLE static-only
// base; instanceCount = the type's full static population (first-N draw — NOT a
// GPU cull count; visibleIds authority arrives in M2b). A packet whose type has
// 0 static instances gets instanceCount=0 -> harmless no-op sub-draw. Dirty-
// gated on s_persistentStaticGen. Built but NOT drawn until Task 4.
static void rebuildStaticDrawCmdsIfDirty() {
    if (s_staticCmdBuiltGen == s_persistentStaticGen) return;
    rebuildStaticBaseInstanceTableIfDirty();
    const uint32_t pktCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
    s_staticDrawCmds.assign(pktCount, StaticDrawCmd{});
    for (uint32_t i = 0; i < pktCount; ++i) {
        const uint32_t gp = s_sortedPacketOrder[i];
        if (gp >= s_packets.size()) continue;
        const GpuStaticPropPacket& pkt = s_packets[gp];
        const uint32_t tid = pkt.owningTypeID;
        auto it = s_persistentStaticStore.find(tid);
        const uint32_t cnt = (it != s_persistentStaticStore.end())
                                 ? static_cast<uint32_t>(it->second.size()) : 0u;
        StaticDrawCmd& c = s_staticDrawCmds[i];
        c.count         = pkt.indexCount;
        c.instanceCount = cnt;
        c.firstIndex    = pkt.firstIndex;
        c.baseVertex    = pkt.baseVertex;
        c.baseInstance  = batcher_getStaticBaseInstanceForType(tid);
    }
    if (pktCount > 0u) {
        if (!s_staticIndirectCmdBuf) glGenBuffers(1, &s_staticIndirectCmdBuf);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_staticIndirectCmdBuf);
        MC2_GL_BufferData(GL_DRAW_INDIRECT_BUFFER,
                     static_cast<GLsizeiptr>(pktCount * sizeof(StaticDrawCmd)),
                     s_staticDrawCmds.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }
    s_staticCmdBuiltGen = s_persistentStaticGen;

    // Diag (env-gated): dump the static cmd summary so Task 3 can be validated
    // before the draw exists. Sum of instanceCount over OFF/ON groups must equal
    // persistentStaticTotalCount(); non-zero cmd count == sorted packet count.
    static const bool s_cmdDiag = (getenv("MC2_STATIC_POP_SPLIT_CMD_DIAG") != nullptr);
    if (s_cmdDiag) {
        // maxBaseInstance+its instanceCount must stay within the static pool
        // [0,storeTotal); nonZeroCmds = packets whose type has static instances.
        uint32_t nonZero = 0, maxEnd = 0;
        for (const auto& c : s_staticDrawCmds) {
            if (c.instanceCount) ++nonZero;
            const uint32_t end = c.baseInstance + c.instanceCount;
            if (end > maxEnd) maxEnd = end;
        }
        uint64_t storeTotal = 0;
        for (auto& kv : s_persistentStaticStore) storeTotal += kv.second.size();
        std::fprintf(stderr, "[STATIC_POP_SPLIT_CMD] pkts=%u nonZeroCmds=%u offCmds=%u "
                     "maxBaseInst+count=%u storeTotal=%llu (maxEnd<=storeTotal expected)\n",
                     pktCount, nonZero, s_alphaOffCmdCount, maxEnd,
                     (unsigned long long)storeTotal);
        std::fflush(stderr);
    }
}

GLuint batcher_getStaticIndirectCmdBuf() { return s_staticIndirectCmdBuf; }

uint32_t batcher_getBaseInstanceForType(uint32_t typeID) {
    // M2a: under the population split, the golden scatter must use the STABLE
    // static-only base (dynamics no longer inflate it) -> outOfRange -> 0.
    if (staticPopSplitArmed())
        return batcher_getStaticBaseInstanceForType(typeID);
    return (typeID < s_baseInstanceForType.size()) ? s_baseInstanceForType[typeID] : 0u;
}

// True when the per-type base table is valid this frame (global-pool armed and
// prepareBaseInstanceTable has run). The golden build must gate on this.
bool batcher_isBaseInstanceTableReady() {
    return !s_globalPoolLegacy && batcher_isCoalesceArmed()
           && !s_baseInstanceForType.empty();
}

GLuint batcher_getCoalesceInstanceSsbo() { return s_coalesceInstanceSsbo; }
GLuint batcher_getPerDrawSsbo()          { return s_perDrawSsbo;          }
// COMPRESSION-BC7-STATICPROP-2: under the gate the two named handles are 0
// (the build produced per-bucket arrays instead). Fall back to the first
// alpha-OFF / first alpha-ON bucket array so the ImGui GBuffer preview still
// shows a representative array. Gate OFF: returns the original handles.
GLuint batcher_getTexArrayOff() {
    if (s_texArrayOff) return s_texArrayOff;
    for (size_t b = 0; b < s_bucketInfo.size(); ++b)
        if (s_bucketInfo[b].group == 0u) return s_bucketInfo[b].glArray;
    return 0;
}
GLuint batcher_getTexArrayOn() {
    if (s_texArrayOn) return s_texArrayOn;
    for (size_t b = 0; b < s_bucketInfo.size(); ++b)
        if (s_bucketInfo[b].group == 1u) return s_bucketInfo[b].glArray;
    return 0;
}
GLuint batcher_getPermutationSsbo()      { return s_permutationSsbo;      }

size_t batcher_getCoalescePerFrameInstanceBytes() {
    // SSBO-BIND-ALIGN: this is the per-frame RING STRIDE; fr_off_bytes_d =
    // s_coalesceFrameSlot * this is the glBindBufferRange offset. The std430
    // GpuStaticPropInstance stride is 112 bytes, so cap*112 is a multiple of 256
    // (NVIDIA's GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT) only when cap % 16 == 0.
    // Otherwise slots 1+ bind at a misaligned offset -> NVIDIA rejects with
    // GL_INVALID_VALUE -> the instance SSBO never binds -> static-prop trees
    // invisible during gameplay, reappearing only when paused on slot 0 (offset 0,
    // the one aligned slot). AMD tolerated it. Pad the per-frame stride up to the
    // alignment; the allocation (Step 5.9) pads identically so offsets never
    // overrun the ring.
    size_t raw = s_globalPoolLegacy
        ? (s_offGroupTotalBytes + s_onGroupTotalBytes)
        : (static_cast<size_t>(s_globalInstanceCap) * sizeof(GpuStaticPropInstance));
    return (size_t)gpuAlignUp((unsigned long long)raw,
                              (unsigned long long)gpuSsboOffsetAlignment());
}

bool batcher_isCoalesceLayoutReady() { return s_coalesceLayoutReady; }
bool batcher_isCoalesceArmed()       { return s_coalesceArmed;       }

// MaterialGpu-3: look up entry from the per-mission material table.
// Returns false (and leaves *out untouched) when the gate is OFF, the table
// is empty, or index is out of range.
bool batcher_getMaterialGpuEntry(uint32_t index, RenderCore::MaterialGpu* out) {
    if (!s_materialGpuEnabled) return false;
    if (!out) return false;
    if (index >= static_cast<uint32_t>(s_materialGpuTable.size())) return false;
    *out = s_materialGpuTable[index];
    return true;
}

bool batcher_getPacketTexArrayLayer(uint32_t globalPacketIdx, int32_t* out) {
    if (!out) return false;
    *out = -1;
    if (globalPacketIdx >= static_cast<uint32_t>(s_packetTexArrayLayer.size()))
        return false;
    *out = s_packetTexArrayLayer[globalPacketIdx];
    return true;
}

// V-MATERIAL-STATIC-0: read-only inventory accessors.
uint32_t batcher_getStaticPropMaterialInventoryCount() {
    return static_cast<uint32_t>(s_materialInventory.size());
}

bool batcher_getStaticPropMaterialInventoryEntry(
        uint32_t idx, StaticPropMaterialInventoryEntry* out) {
    if (!out) return false;
    if (idx >= static_cast<uint32_t>(s_materialInventory.size())) return false;
    *out = s_materialInventory[idx];
    return true;
}

uint32_t batcher_getDrawSlotCount() {
    return static_cast<uint32_t>(s_sortedPacketOrder.size());
}

bool batcher_getDrawSlotEntry(uint32_t slot, ExtractedStaticPropPacket* out) {
    if (!out) return false;
    if (slot >= static_cast<uint32_t>(s_sortedPacketOrder.size())) return false;

    const uint32_t globalPktIdx = s_sortedPacketOrder[slot];
    if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) return false;

    out->sortedSlot      = slot;
    out->globalPacketIdx = globalPktIdx;
    out->typeId          = s_packets[globalPktIdx].owningTypeID;
    out->pipelineId      = static_cast<uint32_t>(
        slot < static_cast<uint32_t>(s_alphaOffCmdCount)
            ? RenderCore::PipelineId::StaticPropOpaque
            : RenderCore::PipelineId::StaticPropAlphaTest);

    // materialIdx from per-slot sidecar; sentinel if sidecar was not valid at finalize time
    if (s_materialGpuSidecarValid &&
        slot < static_cast<uint32_t>(s_packetMaterialIdx.size())) {
        out->materialIdx = s_packetMaterialIdx[slot];
    } else {
        out->materialIdx = 0xFFFFFFFFu;
    }

    // instanceCount from previous frame's flush() snapshot
    const auto it = s_typeInstanceCountPrevFrame.find(out->typeId);
    out->instanceCount = (it != s_typeInstanceCountPrevFrame.end()) ? it->second : 0u;

    // texArrayLayer from per-packet sidecar (v2.2). Sentinel -1 if not available.
    {
        int32_t layer = -1;
        batcher_getPacketTexArrayLayer(globalPktIdx, &layer);
        out->texArrayLayer = layer;
    }

    return true;
}

void batcher_compareSnapshotPackets(RenderSnapshot* snap) {
    if (!snap) return;

    const uint32_t liveCount = batcher_getDrawSlotCount();
    snap->spCompareSnapshotCount = static_cast<uint32_t>(snap->staticPropPackets.count);
    snap->spCompareLiveCount     = liveCount;

    // Fill live_count even when snapshot data is null (e.g. arena overflow).
    if (!snap->staticPropPackets.data) {
        if (liveCount != 0u) snap->spCountMismatch = 1u;
        return;
    }

    if (snap->staticPropPackets.count != liveCount) {
        snap->spCountMismatch = 1u;
        return;  // per-slot compare is meaningless when counts diverge
    }

    const uint32_t n = liveCount;
    for (uint32_t i = 0u; i < n; ++i) {
        const ExtractedStaticPropPacket& row = snap->staticPropPackets.data[i];

        // sortedSlot must equal i (guaranteed by ok=1 invariant, but verify explicitly)
        if (row.sortedSlot != i) {
            ++snap->spSortedSlotMismatch;
        }

        // globalPacketIdx: three-guard pattern mirrors v6 builder guards.
        // Guard 1: i must be a valid index into s_sortedPacketOrder.
        if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
            ++snap->spGlobalPacketMismatch;
            continue;
        }
        const uint32_t livePktIdx = s_sortedPacketOrder[i];
        // Guard 2: livePktIdx must be a valid index into s_packets.
        if (livePktIdx >= static_cast<uint32_t>(s_packets.size())) {
            ++snap->spGlobalPacketMismatch;
            continue;
        }
        if (row.globalPacketIdx != livePktIdx) {
            ++snap->spGlobalPacketMismatch;
        }

        // pipelineId must use real RenderCore::PipelineId enum values — NOT raw 0/1.
        const uint32_t expectedPipeline = static_cast<uint32_t>(
            i < s_alphaOffCmdCount
                ? RenderCore::PipelineId::StaticPropOpaque
                : RenderCore::PipelineId::StaticPropAlphaTest);
        if (row.pipelineId != expectedPipeline) {
            ++snap->spPipelineMismatch;
        }

        // materialIdx from sidecar (only check when sidecar was valid at finalize time)
        if (s_materialGpuSidecarValid &&
            i < static_cast<uint32_t>(s_packetMaterialIdx.size())) {
            if (row.materialIdx != s_packetMaterialIdx[i]) {
                ++snap->spMaterialIdxMismatch;
            }
        }

        // instanceCount: prev-frame snapshot vs current-frame s_typeRanges.
        // Informational only — different-frame authority by design; NOT in ok gate.
        {
            const auto typeIt = s_typeRanges.find(row.typeId);
            const uint32_t currentInstCount =
                (typeIt != s_typeRanges.end()) ? typeIt->second.instanceCount : 0u;
            if (row.instanceCount != currentInstCount) {
                ++snap->spInstanceCountMismatch;
            }
        }

        // texArrayLayer vs MaterialGpu.albedoTex cross-authority check.
        // Only compare when both materialIdx and texArrayLayer are valid.
        // A missing MaterialGpu entry when materialIdx is valid is itself a mismatch.
        if (row.materialIdx != 0xFFFFFFFFu && row.texArrayLayer != -1) {
            RenderCore::MaterialGpu mat{};
            if (!batcher_getMaterialGpuEntry(row.materialIdx, &mat)) {
                // materialIdx valid but entry missing — structural failure.
                ++snap->spTexLayerMismatch;
            } else if (mat.albedoTex != static_cast<uint32_t>(row.texArrayLayer)) {
                ++snap->spTexLayerMismatch;
            }
        }
    }
}

void batcher_getSnapCullStats(uint32_t* skipped, uint32_t* active, uint32_t* slotMismatch) {
    if (skipped)      *skipped      = s_snapCullSkipped;
    if (active)       *active       = s_snapCullActive;
    if (slotMismatch) *slotMismatch = s_snapCullSlotMismatch;
}

void batcher_getSnapshotBuildStats(uint32_t* attempted, uint32_t* countMismatch,
                                   uint32_t* packetMismatch, uint32_t* metaMismatch,
                                   uint32_t* fallback, uint32_t* retired)
{
    if (attempted)      *attempted      = s_spBuildAttempted;
    if (countMismatch)  *countMismatch  = s_spBuildCountMismatch;
    if (packetMismatch) *packetMismatch = s_spBuildPacketMismatch;
    if (metaMismatch)   *metaMismatch   = s_spBuildMetaMismatch;
    if (fallback)       *fallback       = s_spBuildFallback;
    if (retired)        *retired        = s_spBuildRetired;
}

uint32_t batcher_getPerTypePeakCount(uint32_t typeID) {
    return typeID < s_perTypePeak.size() ? s_perTypePeak[typeID] : 0u;
}

bool batcher_isGlobalPoolLegacy() { return s_globalPoolLegacy; }
uint32_t batcher_getGlobalInstanceCap() { return s_globalInstanceCap; }
uint32_t batcher_getCoalesceFrameSlot() { return s_coalesceFrameSlot; }  // diagnostic

void batcher_getStaticPropOpaqueDebugState(StaticPropOpaqueDebugState* out) {
    if (!out) return;
    out->snapshotDispatchDefault     = s_v6Enabled;
    out->legacyDispatch              = !s_v6Enabled;
    out->materialGpuEnabled          = s_materialGpuEnabled;
    out->materialGpuSample           = s_materialGpuSampleEnabled;
    out->iblShEnabled                = s_iblShEnabled;
    out->iblShStrength               = g_iblShStrength;
    out->iblShSet                    = s_currentShSet ? s_currentShSet->name : "default";
    out->pbrEnabled                  = s_pbrV1Enabled;
    out->pbrStrength                 = g_pbrV1Strength;
    out->pbrRoughnessOverrideEnabled = g_pbrV1RoughnessOverrideEnabled;
    out->pbrRoughnessOverride        = g_pbrV1RoughnessOverrideValue;
    out->debugMaterialMode           = s_staticPropDebugMaterialMode;
    out->spV6DrawCalls               = s_v6FrameDrawsIssued;
    out->spAlphaOffPackets           = s_alphaOffCmdCount;
    out->materialGpuTableSize        = static_cast<uint32_t>(s_materialGpuTable.size());
    out->materialInventorySize       = static_cast<uint32_t>(s_materialInventory.size());
}

// ---------------------------------------------------------------------------
// Type-desc table accessors (v0)
// ---------------------------------------------------------------------------

uint32_t batcher_getStaticPropTypeDescCount() {
    return static_cast<uint32_t>(s_typeDescTable.size());
}

bool batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out) {
    if (!out) return false;
    if (typeId >= static_cast<uint32_t>(s_typeDescTable.size())) return false;
    *out = s_typeDescTable[typeId];
    return true;
}

const RenderCore::StaticPropTypeDesc* batcher_getStaticPropTypeDescTable(uint32_t* outCount) {
    if (!outCount) return nullptr;
    *outCount = static_cast<uint32_t>(s_typeDescTable.size());
    if (s_typeDescTable.empty()) return nullptr;
    return s_typeDescTable.data();
}

void batcher_buildCandidateLog() {
    static const bool gate    = (getenv("MC2_TYPE_TABLE_CAND_LOG")     != nullptr);
    static const bool verbose = (getenv("MC2_TYPE_TABLE_CAND_VERBOSE") != nullptr);
    if (!gate) return;

    if (s_typeDescTable.empty()) {
        std::fprintf(stderr, "[DRAW_CAND v0] table_empty\n");
        return;
    }

    // Compute expected: sum of packetCount over all table entries (zero-packet entries contribute 0).
    uint32_t expected = 0u;
    for (const auto& d : s_typeDescTable) { expected += d.packetCount; }

    static bool s_detailDone = false;
    const bool doDetail = verbose || !s_detailDone;

    uint32_t emitted = 0u;
    uint32_t emptyTypes = 0u;
    uint32_t boundsErrors = 0u;
    for (const RenderCore::StaticPropTypeDesc& desc : s_typeDescTable) {
        if (desc.packetCount == 0u) { ++emptyTypes; continue; }
        for (uint32_t p = 0u; p < desc.packetCount; ++p) {
            const uint32_t pktIdx = desc.firstPacket + p;
            if (pktIdx >= static_cast<uint32_t>(s_packets.size())) {
                if (doDetail) {
                    std::fprintf(stderr,
                        "[DRAW_CAND v0 detail] typeId=%u pkt=%u BOUNDS_OVERFLOW\n",
                        desc.typeId, p);
                }
                ++boundsErrors;
                continue;
            }
            if (doDetail) {
                const GpuStaticPropPacket& pkt = s_packets[pktIdx];
                std::fprintf(stderr,
                    "[DRAW_CAND v0 detail] typeId=%u pkt=%u firstIndex=%u"
                    " indexCount=%u baseVertex=%d alphaClass=%u\n",
                    desc.typeId, p,
                    pkt.firstIndex, pkt.indexCount, pkt.baseVertex,
                    desc.alphaClass);
            }
            ++emitted;
        }
    }
    s_detailDone = true;

    // Summary line emitted every frame.
    // OK gate: emitted==expected (all reachable packets walked), boundsErrors==0.
    // emptyTypes is informational; a type may be registered with no geometry.
    std::fprintf(stderr,
        "[DRAW_CAND v0] emitted=%u expected=%u emptyTypes=%u boundsErrors=%u%s\n",
        emitted, expected, emptyTypes, boundsErrors,
        (emitted == expected && boundsErrors == 0) ? " OK" : " MISMATCH");
}

// Saved slot-16 GL buffer object. Slot 16 is reserved to this feature.
static GLint s_savedSsbo16 = 0;

void batcher_bindBaseInstanceByCmdSsboForPatch() {
    if (s_globalPoolLegacy || !s_baseInstanceByCmdSsbo) return;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, BASE_INSTANCE_SSBO_BINDING, &s_savedSsbo16);
    const GLintptr off = static_cast<GLintptr>(
        s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame);
    gpuBindSsboRange(BASE_INSTANCE_SSBO_BINDING, s_baseInstanceByCmdSsbo,
                     (long long)off, (long long)s_baseInstanceByCmdBytesPerFrame,
                     "sp.baseInstanceByCmd");
}
void batcher_unbindBaseInstanceByCmdSsboForPatch() {
    if (s_globalPoolLegacy || !s_baseInstanceByCmdSsbo) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BASE_INSTANCE_SSBO_BINDING, (GLuint)s_savedSsbo16);
    s_savedSsbo16 = 0;
}

// Env probe for GPU-vs-CPU count diagnostic (Step 4.11).
static const bool s_coalesceGpuVsCpuTrace =
    (getenv("MC2_COALESCE_GPU_VS_CPU_COUNT_TRACE") != nullptr);

void batcher_prepareBaseInstanceTable() {
    // Legacy mode: coalesce ring mirrors legacy ring (s_coalesceFrameSlot = s_frameSlot
    // at the :2644 advance site). Nothing to do here.
    if (s_globalPoolLegacy) return;

    // CRIT-2 v2 fix: advance coalesce ring slot BEFORE any guards so flush()'s
    // memcpy + draw always read a fresh, well-defined slot.
    s_coalesceFrameSlot = (s_coalesceFrameSlot + 1) % RING_FRAMES;

    // Wait on the fence for the slot we just took (from RING_FRAMES frames ago).
    if (s_coalesceFence[s_coalesceFrameSlot]) {
        MC2_GL_ClientWaitSync(s_coalesceFence[s_coalesceFrameSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_coalesceFence[s_coalesceFrameSlot]);
        s_coalesceFence[s_coalesceFrameSlot] = nullptr;
    }

    // Step 4.11 — GPU-vs-CPU count diagnostic (env-gated). Must be after fence
    // wait (prior frame's compute is now complete — fence covered patch dispatch).
    if (s_coalesceGpuVsCpuTrace) {
        static std::vector<uint32_t> s_priorFrameCpuCount;
        const GLuint bucketsBuf = gpu_cull::compute_getBucketCountsBuf();
        if (bucketsBuf && !s_types.empty()) {
            std::vector<uint32_t> bucketCountGpu(s_types.size() + 1, 0u);
            glGetNamedBufferSubData(bucketsBuf, 0,
                static_cast<GLsizeiptr>((s_types.size() + 1) * sizeof(uint32_t)),
                bucketCountGpu.data());
            const size_t cmpN = std::min(s_priorFrameCpuCount.size(), bucketCountGpu.size() - 1);
            for (uint32_t t = 0; t < (uint32_t)cmpN; ++t) {
                if (bucketCountGpu[t] > s_priorFrameCpuCount[t]) {
                    std::fprintf(stderr,
                        "[COALESCE v1] event=gpu_count_exceeds_cpu type=%u gpu=%u cpu=%u\n",
                        t, bucketCountGpu[t], s_priorFrameCpuCount[t]);
                    std::fflush(stderr);
                }
            }
        }
        // Stash this frame's CPU bucket sizes for comparison RING_FRAMES frames from now.
        s_priorFrameCpuCount.assign(s_types.size(), 0u);
        for (auto& kv : s_bucketsByType) {
            if (kv.first < s_priorFrameCpuCount.size())
                s_priorFrameCpuCount[kv.first] = static_cast<uint32_t>(kv.second.instances.size());
        }
    }

    if (!s_baseInstanceByCmdMap) return;
    if (!batcher_isCoalesceArmed()) return;

    // Prefix-sum: compute baseInstance for each type in sorted order.
    std::array<uint32_t, 2> groupCursor = {0u, 0u};
    static std::vector<uint32_t> baseInstanceForType;
    baseInstanceForType.assign(s_types.size(), 0u);
    for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
        const uint32_t typeID = s_sortedTypeOrder[i];
        if (typeID >= s_types.size()) continue;
        auto kvIt = s_bucketsByType.find(typeID);
        if (kvIt == s_bucketsByType.end()) continue;
        const uint32_t group = s_types[typeID].alphaClass;
        baseInstanceForType[typeID] = groupCursor[group];
        groupCursor[group] += static_cast<uint32_t>(kvIt->second.instances.size());
    }
    const uint32_t offGroupCount = groupCursor[0];
    const uint32_t totalCount    = offGroupCount + groupCursor[1];

    if (totalCount > s_globalInstanceCap) {
        std::fprintf(stderr, "[COALESCE v1] event=disarmed reason=global_pool_overflow "
                     "used=%u cap=%u\n", totalCount, s_globalInstanceCap);
        std::fflush(stderr);
        s_coalesceArmed = false;
        return;
    }

    // Offset alpha-ON types by alpha-OFF group total.
    for (uint32_t typeID : s_sortedTypeOrder) {
        if (typeID >= s_types.size()) continue;
        if (s_types[typeID].alphaClass == 1u) {
            baseInstanceForType[typeID] += offGroupCount;
        }
    }

    // M1: publish the finalized per-type base for the golden static cull-record
    // scatter (batcher_getBaseInstanceForType). This is the exact binding-0 slot
    // layout the draw uses, so a record placed at base[t]+rank aligns 1:1 with
    // the instance the draw renders.
    s_baseInstanceForType = baseInstanceForType;

    // M2a POPULATION-SPLIT: rebuild the static-only base table + fill the static
    // instance buffer when the persistent store changed (both dirty-gated on
    // s_persistentStaticGen). The base table must be ready before the golden
    // scatter (buildStaticPrefixGolden) reads it later this frame. Default-off
    // => not called => byte-identical.
    if (staticPopSplitArmed()) {
        rebuildStaticBaseInstanceTableIfDirty();
        fillStaticInstanceBufferIfDirty();
        rebuildStaticDrawCmdsIfDirty();   // Task 3: own indirect cmds (not drawn until Task 4)
    }

    // Write baseInstanceByCmd[c] for each cmd.
    const uint32_t pktCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
    const size_t fr_off = s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame;
    uint32_t* dst = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(s_baseInstanceByCmdMap) + fr_off);
    for (uint32_t i = 0; i < pktCount; ++i) {
        const uint32_t globalPktIdx = s_sortedPacketOrder[i];
        const uint32_t typeID       = s_packets[globalPktIdx].owningTypeID;
        dst[i] = (typeID < baseInstanceForType.size()) ? baseInstanceForType[typeID] : 0u;
    }

    // Stash group byte/count boundaries for flush() memcpy and draw path.
    s_offGroupCountThisFrame  = offGroupCount;
    s_offGroupBytesThisFrame  = offGroupCount * sizeof(GpuStaticPropInstance);
    s_onGroupBytesThisFrame   = (totalCount - offGroupCount) * sizeof(GpuStaticPropInstance);
    s_totalUsedBytesThisFrame = totalCount * sizeof(GpuStaticPropInstance);
}

