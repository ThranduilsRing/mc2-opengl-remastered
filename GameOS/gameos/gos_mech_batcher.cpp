// GameOS/gameos/gos_mech_batcher.cpp — GPU mech batcher, Slice A.
#include "gos_mech_batcher.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "vk_feed_dump.h"        // VK-OFFLINE-PROOFS-1: MC2_VK_FEED_DUMP golden dump
#include "gos_materials.h"       // Slice C1: named material profile SSBO (binding 7)
#include "ibl_sh_runtime.h"     // PBR-IBL-MECH-1: g_iblShStrength (shared with static props)
#include "../../RenderCore/IblShCoeffs.h"  // PBR-IBL-MECH-1: SH-L2 coefficients
#include "gos_gpu_sync.h"      // GPU-SYNC-CONTRACT typed barrier helper
#include "render_snapshot.h"  // MECH-EXTRACTION-0: ExtractedMechPacket, RenderSnapshot
#include "../../RenderCore/RenderDebugView.h"  // MECH-DEBUG-VIEWS-1
#include "../../RenderCore/PipelineRegistry.h"  // MECH-PIPELINEDESC-1
#include "pipeline_binder.h"                     // MECH-PIPELINEDESC-1 applyPipeline
#include "../../RenderCore/frame_executor.h"     // APPLY-STATE-MECHOPAQUE-1: ApplyPassId
#include "../../RenderCore/RenderResourceRegistry.h"  // REGISTRY-MATERIAL-SSBO-1: MaterialGpuBuffer
#include "../../RenderCore/GpuBufferOwner.h"  // VK-SHADOW-RING-OWNER-1: ring SSBO owner records
// M2.5: IsObjectIdBufferEnabled() drives the GLSL prefix that gates the
// mech.frag layout(location=2) write. Mirrors the include shipped by M1.5
// at gos_static_prop_batcher.cpp:3. GameOS/ is outside the firewall
// SCOPE_DIRS (scripts/check-include-firewall.sh:22) so this include is
// not policed by the firewall script; reviewer-discipline gate only.
// RenderWorld/RenderWorld.h is the PUBLIC header (no legacy/* reach).
#include "../../RenderWorld/RenderWorld.h"
#include "gos_mech_killswitch.h"
#include "gos_static_prop_batcher.h"  // for STATIC_PROP_RING_FRAMES cross-check
#include "../../mclib/mech_anim_runtime.h"  // 1B-GPU: imported-mech model-delta skinning
#include "gameos.hpp"                 // gos_InvalidateRenderStateCache
#include "utils/shader_builder.h"
#include "gos_postprocess.h"           // getGosPostProcess()->getDynamicLightSpaceMatrix()
#include "gos_terrain_lighting.h"      // POOL-CONSUMERS-1: UploadObjectLightPoolUniforms
#include "view_uniforms_gl.h"          // VK-PARITY-MECH-1: RenderCore::getCurrentView (cameraWorldPos)
#include "../../mclib/txmmgr.h"       // mcTextureManager->get_gosTextureHandle (live resolve)
#include "../../mclib/terrain_depth_bias.h"  // VK-P14 MECH: OBJECT_DEPTH_BIAS feed
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_mech_island.h"       // VK-P14 MECH: island feed contract (Vulkan-free POD)
#endif
#include <GL/glew.h>
#include "gl_state_guard.h"            // GlStateGuard adoption: mc2gl::GlScopedSsboBinding
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>
#include <array>
#include <cmath>
#include <optional>            // GLSTATE-GUARD-ADOPTION-3: deferred-destruct SSBO guards in flush()

// M2.5 (external-review C1): file-scope forward declaration of the
// MLR-side per-mission counter getter, defined in mclib/mech3d.cpp.
// Language-linkage declarations must be at file scope -- not inside a
// function body. Avoids a new header.
extern "C" uint64_t consumeAndResetMlrMechDraws();

// MECH-DECLINE-FIX-1: same shape, same reason -- file-scope forward declaration
// of the light-row freshness counters, defined in mclib/mech3d.cpp. Consumed on
// the always-on per-mission summary in onMapUnload().
extern "C" void consumeAndResetMechLightRowCounters(uint64_t* staleOut,
                                                    uint64_t* oobOut);

// APPLY-STATE-MECHOPAQUE-1: cross-TU apply-state counter bump (defined in
// gos_postprocess.cpp). Bumps the same g_applyStatePasses aggregate used by the
// PostProcess apply-state islands + the TerrainDecal/TerrainOverlay/StaticProp
// top-level applies (single increment per apply, no double-count).
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);

// APPLY-STATE-MECHOPAQUE-1: file-static flag mirroring s_staticPropStateAppliedByExecutor.
// When MC2_FRAMEGRAPH_EXECUTOR is ON, executorApplyMechOpaqueState() pre-applies the
// MechOpaque pipeline (at the flush call site in txmmgr renderLists) and sets this;
// flush()'s body then skips its own applyPipeline (one-shot reset). Gate OFF -> flag
// stays false -> body applies as before -> byte-identical.
static bool s_opaqueStateAppliedByExecutor = false;

// MECH_RING_FRAMES must equal STATIC_PROP_RING_FRAMES so the parity SSBO
// ring and the mech fence ring share the same depth.
static_assert(MECH_RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "MECH_RING_FRAMES must match STATIC_PROP_RING_FRAMES");

// PROP-FIXB-MVP (mech mirror): symmetric-mirror of the terrain Fix-B
// frame-of-reference (see gos_terrain_lod_chunk.cpp ~:33-41 +
// docs/superpowers/specs/2026-05-18-AUTHORITATIVE-zfight-matrix-share-design.md
// §11). When the solid pass is armed the LOD-chunk terrain draws with the
// snapshotted dispatch MVP (frame N-1) while the default mech path uploads the
// LIVE current-frame MVP -> a 1-frame matrix-share lag -> depth shimmer at the
// mech-foot/terrain contact under camera motion. Mirroring the SAME consume
// expression makes the default mech draw agree with terrain depth. The snapshot
// itself is NOT touched; this only CONSUMES the published terrain snapshot.
// Applies ONLY to the default (non-UBO) path — the s_mechViewUniforms UBO path
// is untouched. Same gate (MC2_PROP_FIXB_MVP) as the static-prop sites so the
// whole object family shares one matrix policy. Signatures copied verbatim from
// gos_terrain_lod_chunk.cpp.
#include "gos_object_draw_mvp.h"  // RENDER-VIEW-CURRENCY-1 currency-checked accessor
// NOTE: raw gos_terrain_indirect_getDispatchMvp16() is phase-private to terrain/
// water passes — mech draw uses gos_GetObjectDrawMVP() (the accessor above).
// Enforced by scripts/check-object-mvp-currency.py.

// MC2_PROP_FIXB_MVP — default ON (=0 reverts, killswitch). Consume the
// dispatch-MVP snapshot when the solid pass is armed (un-armed arm returns the
// live pointer anyway). Sampled once at process start.
static bool s_mechFixBMvpEnabled() {
    static const bool on = []() {
        const char* v = std::getenv("MC2_PROP_FIXB_MVP");
        return !(v && std::atoi(v) == 0);  // default ON; only =0 reverts
    }();
    return on;
}
// RENDER-VIEW-CURRENCY-1: snapshot only when its view epoch matches the current
// view; otherwise the live MVP. Same policy as the static-prop family. Killswitch
// preserved via the arg.
static const float* gos_GetMechFixBMVPMat4() {
    return gos_GetObjectDrawMVP(s_mechFixBMvpEnabled());
}

// Default-on flip (2026-05-09): all GPU mech killswitches now default to ON.
// Helper returns true unless the env var is explicitly set to "0". Operators
// can opt out per-flag via MC2_GPU_MECH_<FLAG>=0. Pre-flip pattern was
// `(getenv("X") != nullptr)` (default-off opt-in); post-flip is `default-on,
// "0" opts out`. Each individual slice's killswitch comments in
// gos_mech_killswitch.h document the inverted semantics.
//
// Why default-on shipped 2026-05-09: full slice campaign accumulated
// substantial empirical soak — tier1 5/5 PASS at full bore, multiple 90s
// mc2_10 Tracy captures clean, mouse-pick + sensor-diamond visual canaries
// passed, combined stack delivers Mech3D.UpdateGeometry mean 71→14.08µs/call
// (-80%). User explicitly signed off on B1's "needs more verification"
// constraint based on this evidence.
static bool envFlagDefaultOn(const char* name) {
    const char* v = getenv(name);
    if (v == nullptr) return true;                  // unset → on (new default)
    if (v[0] == '0' && v[1] == '\0') return false;  // exactly "0" → off
    return true;                                     // any other value → on
}

// ImGui visibility kill-switch. Always true at startup; toggled by the
// Graphics Options panel "Draw Mechs" checkbox. When false, both the GPU
// batcher submit and the CPU Render(true) fallback are skipped.
bool g_drawMechs = true;

// Slice A: GPU mech batcher. Foundation for the entire stack.
// Opt-out: MC2_GPU_MECHS=0
bool g_useGpuMechs = envFlagDefaultOn("MC2_GPU_MECHS");

#ifdef MC2_VULKAN_ISLAND
#include "vk_shared_gbuffer_gate.h"  // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
// VK-P14 MECH TIER-1 (gate MC2_VK_MECH_TIER1, requires MC2_VK_SHARED_SCENE_DEPTH,
// default-OFF): when armed, the flush() below still BUILDS the instance/bone SSBOs
// and the feed snapshot, but SKIPS its own GL color draws; the caller (txmmgr) runs
// mc2_vk_mech_island_run() to co-write the mechs IN-PLACE into the shared GBuffer.
// Gate OFF -> s_vkMechGateOn stays false -> GL flush() byte-identical.
static bool s_vkMechGateOn = [] {
    // Umbrella members: MC2_VK_SHARED_GBUFFER=1 arms both; explicit
    // MC2_VK_MECH_TIER1 / MC2_VK_SHARED_SCENE_DEPTH =0/1 override.
    const bool on  = mc2vkgate::subGate("MC2_VK_MECH_TIER1");
    const bool dep = mc2vkgate::subGate("MC2_VK_SHARED_SCENE_DEPTH");
    return on && dep;   // require shared depth (the co-write depth-tests against it)
}();

// VK-P14 MECH feed snapshots (persisted host-visible copies the island memcpy's).
// Populated at flush() end ONLY when s_vkMechGateOn; empty otherwise.
static std::vector<GpuMechInstance> s_vkMechInstSnapshot;
static std::vector<GpuMechBone>     s_vkMechBoneSnapshot;
static std::vector<Mc2MechDrawRun>  s_vkMechRunSnapshot;
static float                        s_vkMechWorldToClipGL[16] = {0};
static float                        s_vkMechObjectDepthBias   = 0.0f;
static bool                         s_vkMechFrameReady        = false;

// VK-MECH-FEED-NATIVE-1 (rung 7): envTriState OPT-IN, default OFF (same new-rung
// pattern as MC2_VK_TERRAIN_SKIRTS / MC2_VK_WATER_TIER1: unset -> OFF regardless
// of the umbrella; explicit 0 forces OFF). When armed, the feed carries the CPU
// byte sources (exact uploaded VBO work blob, s_stagingIbo, mcTextureManager
// lightData_) so the island never reads GL buffer objects back.
// VK-RUNG8 item-9: MC2_VK_MECH_FEED_NATIVE is now DEPRECATED / a no-op. The native
// CPU feed is unconditional whenever the VK mech island is active (s_vkMechGateOn),
// because the GL buffer-readback fallback in the island was deleted. The env name is
// still parsed so old command lines don't hit unknown-var behavior; explicitly setting
// it just prints a one-line deprecation note.
[[maybe_unused]] static const bool s_vkMechFeedNativeDeprecated = [] {
    if (mc2vkgate::envTriState("MC2_VK_MECH_FEED_NATIVE") != -1)
        std::fprintf(stderr, "[VK_MECH_FEED] MC2_VK_MECH_FEED_NATIVE is deprecated "
                             "and now a no-op: native CPU feed is always used under "
                             "the VK mech island (GL readback fallback deleted).\n");
    return true;
}();
// Exact bytes uploaded to s_sharedVbo (the normals-recomputed `work` copy —
// s_stagingVbo alone is NOT the uploaded bytes when s_mechNormalsMode != 0).
// Refreshed by uploadMechGeometryVbo(); cleared with the staging on map load.
static std::vector<uint8_t>         s_vkVboNativeCopy;
#endif

// SimpleCamera UI preview-render context depth (Mech Bay / Mech Purchase).
// See gos_mech_killswitch.h. Zero during tactical/world rendering.
int g_mechPreviewRenderDepth = 0;

// MECH-KILLSWITCH-LIGHTING-RETIRE-1: u_lightingMode is fed constant 1
// (VS-side lighting always on; default-ON). See gos_mech_killswitch.h.

// Slice C1: render-only mech GPU cull. Requires g_useGpuMechs=true.
// Opt-out: MC2_GPU_MECH_CULL=0
bool g_useGpuMechCull = envFlagDefaultOn("MC2_GPU_MECH_CULL");

// MECH-KILLSWITCH-SKIN-RETIRE-1: MC2_GPU_MECH_SKIN retired to constant
// (default-ON, proven no-op — stock + all imports single-bone). u_skinningMode
// fed constant 1; weighted branch kept as forward capacity. See gos_mech_killswitch.h.

// Slice C3-revised: see gos_mech_killswitch.h. Body-only fast transform.
// Opt-out: MC2_GPU_MECH_FAST_TRANSFORM=0
bool g_useGpuMechFastTransform = envFlagDefaultOn("MC2_GPU_MECH_FAST_TRANSFORM");

// Slice C3-shadow: see gos_mech_killswitch.h. Shadow callsite fast transform.
// Opt-out: MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=0
bool g_useGpuMechShadowFastTransform = envFlagDefaultOn("MC2_GPU_MECH_SHADOW_FAST_TRANSFORM");

// MECH-KILLSWITCH-SHADOW-PAIR-RETIRE-1: MC2_GPU_MECH_SHADOW_SKIP and
// MC2_GPU_MECH_SHADOW_STATE_STRIP retired to constants (both default-ON,
// strict no-ops on the tessellated GPU-mech path). Logic now inline in
// Mech3DAppearance::updateGeometry. See gos_mech_killswitch.h.

// Slice D-leaf-skip-v2: see gos_mech_killswitch.h.
// Opt-out: MC2_GPU_MECH_LEAF_SKIP=0
bool g_useGpuMechLeafSkip = envFlagDefaultOn("MC2_GPU_MECH_LEAF_SKIP");

// MECH-KILLSWITCH-SENSORSKIP-RETIRE-1: MC2_GPU_MECH_SENSOR_SKIP retired to a
// constant (was default-ON, strict no-op). Skip logic is now inline in
// Mech3DAppearance::updateGeometry. See gos_mech_killswitch.h.

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------
static bool   s_programLoadTried  = false;
static bool   s_programLoadFailed = false;
static GLuint s_mechProgram       = 0;
static glsl_program* s_mechProgramObj = nullptr;

// MECH-DEBUG-VIEWS-1: ImGui-settable debug mode (mirrors s_staticPropDebugMaterialMode).
// 0 = Final (normal rendering); env MC2_MECH_FRAG_DEBUG wins when set.
static int s_mechDebugMode = 0;

// MECH-VIEWUNIFORMS-BLOCKBINDING-1: mech-specific ViewUniforms gate,
// DEFAULT ON -- kill-switch MC2_MECH_VIEWUNIFORMS=0. Independent of the global
// MC2_VIEW_UNIFORMS. When ON (default), the mech shader is compiled with the
// ViewUniformsBlock (binding=3) consumer and the block binding is enforced
// explicitly after link; the legacy u_worldToClipGL upload is skipped. Setting
// =0 restores the legacy upload, byte-identical to pre-feature.
// This block said "DEFAULT OFF ... opt-in (=1)" until ENV-DROP-CLOSE-1
// (2026-07-30) while the reader four lines below already said DEFAULT-ON, so
// the two halves of the same declaration disagreed.
static const bool s_mechViewUniforms = []() {
    const char* v = std::getenv("MC2_MECH_VIEWUNIFORMS");
    return !(v != nullptr && v[0] == '0');   // DEFAULT-ON; kill-switch =0
}();
// Shared diag gate (link-time block-binding log + flush-time binding/matrix
// probe). Default OFF (MC2_MECH_VIEWUNIFORMS_DIAG=1).
static const bool s_mechViewUniformsDiag = []() {
    const char* v = std::getenv("MC2_MECH_VIEWUNIFORMS_DIAG");
    return v != nullptr && v[0] == '1';
}();

// MECH-NORMALS-FIX-1: gated mech-LOCAL normal recompute, applied at
// registerTypeLod over each node's triangle-soup verts. Does NOT touch the
// shared TGL ASE loader (mclib/tgl.cpp LoadTGShapeFromASE) — which averages
// ASE per-corner normals by vertex index and destroys hard-edge splits (see
// docs/mech-normals-audit.md) but is used by all props/buildings (high blast
// radius). MC2_MECH_NORMALS_MODE: 0=cooked (legacy; kill-switch), 1=geometric
// face normals to all corners (faceted; confirm/debug), 2=angle-threshold
// smoothed (hard-edge preserving) — DEFAULT. Smoothing angle 60deg.
// These are mutable so ImGui controls (via batcher_setMechNormalsMode /
// batcher_setMechNormalsSmoothDeg + batcher_rebuildMechNormals) can dial them
// at runtime without a restart. Env vars still set the startup default.
// DEFAULT-ON: mode 2 (smoothed, hard-edge preserving) — the intended look that
// fixes the corrupted ASE-averaged normals. Kill-switch MC2_MECH_NORMALS_MODE=0
// restores cooked/legacy normals; =1 = faceted. User-approved 2026-05-28.
static int s_mechNormalsMode = []() {
    const char* v = std::getenv("MC2_MECH_NORMALS_MODE");
    return v ? std::atoi(v) : 2;
}();
// Smoothing angle for mode 2 (degrees): faces meeting at a shared vertex blend
// only when within this angle, so a smaller value preserves MORE hard edges
// (e.g. lower it to sharpen the cockpit). Env-tunable for dial-in; default 60.
static float s_mechNormalsSmoothDeg = []() {
    const char* v = std::getenv("MC2_MECH_NORMALS_SMOOTH_DEG");
    float d = v ? (float)std::atof(v) : 60.0f;
    if (d < 1.0f)   d = 1.0f;
    if (d > 179.0f) d = 179.0f;
    return d;
}();

// MECH-NORMALS-FIX-1: per-node VBO byte ranges recorded during registerTypeLod.
// Each entry is {startByte, endByte} in s_stagingVbo for one node's verts.
// uploadMechGeometryVbo() iterates these to bound recomputeMechNodeNormals
// to a single node (it must not blend normals across node boundaries).
// Cleared at onMapLoad() alongside s_stagingVbo.
static std::vector<std::pair<size_t,size_t>> s_nodeVboRanges;

// MECH-AMBIENT-1: hemisphere ambient FILL for mechs. DEFAULT-ON (user-approved
// 2026-05-28 at strength 0.15) with kill-switch MC2_MECH_AMBIENT_V1=0. When off
// the strength uploaded to the shader is 0.0 -> exact no-op (legacy look). Both
// mutable so ImGui can dial them live (batcher_setMechAmbient*). The shader term
// is a per-fragment hemisphere using the world normal; no PBR/material/team-mask
// data involved. Works on legacy and ViewUniforms mech paths (no camera data).
static bool  s_mechAmbientV1 = []() {
    const char* v = std::getenv("MC2_MECH_AMBIENT_V1");
    return !(v != nullptr && v[0] == '0');   // default-ON; kill-switch =0
}();
static float s_mechAmbientStrength = []() {
    const char* v = std::getenv("MC2_MECH_AMBIENT_V1_STRENGTH");
    float d = v ? (float)std::atof(v) : 0.15f;   // user-tuned default
    if (d < 0.0f) d = 0.0f;
    if (d > 2.0f) d = 2.0f;
    return d;
}();

// MECH-SPECULAR-V1: conservative Blinn specular sheen. DEFAULT-OFF (gate
// MC2_MECH_SPECULAR_V1=1 to enable). Only effective when MC2_MECH_VIEWUNIFORMS=1
// (the shader variant with MC2_USE_VIEW_UNIFORMS defined); on all other paths the
// C++ side uploads strength 0 → exact no-op. Per-flush uniforms — no VBO rebuild.
static bool  s_mechSpecularV1 = []() {
    const char* v = std::getenv("MC2_MECH_SPECULAR_V1");
    return !(v != nullptr && v[0] == '0');  // DEFAULT-ON; kill-switch =0
}();
static float s_mechSpecularStrength = []() {
    const char* v = std::getenv("MC2_MECH_SPECULAR_STRENGTH");
    // Default 0.05: at 1.0 the Blinn highlight read as a near-glow on mech
    // armor. 0.05 is a subtle metal glint. MC2_MECH_SPECULAR_STRENGTH overrides.
    // Also profile-backed via the mechSpecularStrength key (visual_tuning.json).
    float d = v ? (float)std::atof(v) : 0.05f;
    if (d < 0.0f) d = 0.0f;
    if (d > 4.0f) d = 4.0f;
    return d;
}();
static float s_mechMetalRoughness = []() {
    const char* v = std::getenv("MC2_MECH_METAL_ROUGHNESS");
    float d = v ? (float)std::atof(v) : 0.85f;
    if (d < 0.04f) d = 0.04f;
    if (d > 1.0f)  d = 1.0f;
    return d;
}();
static float s_mechGlassRoughness = []() {
    const char* v = std::getenv("MC2_MECH_GLASS_ROUGHNESS");
    float d = v ? (float)std::atof(v) : 0.25f;
    if (d < 0.04f) d = 0.04f;
    if (d > 1.0f)  d = 1.0f;
    return d;
}();
static float s_mechGlassLumaThresh = []() {
    const char* v = std::getenv("MC2_MECH_GLASS_LUMA_THRESH");
    float d = v ? (float)std::atof(v) : 0.12f;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return d;
}();
static float s_mechGlassMaxChanThresh = []() {
    const char* v = std::getenv("MC2_MECH_GLASS_MAXCHAN_THRESH");
    float d = v ? (float)std::atof(v) : 0.18f;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return d;
}();
static bool  s_mechSpecDebugMask = false;  // ImGui only; no env var
// MECH-BACK-FILL-1: cool-sky fill for the StandardLit shadow hemisphere.
// Default OFF (0.0). Enable via MC2_MECH_BACK_FILL=<strength> (e.g. 1.5).
static float s_mechBackFillStrength = []() {
    const char* v = std::getenv("MC2_MECH_BACK_FILL");
    return v ? (float)std::atof(v) : 0.0f;
}();

// PBR-TUNE-1: StandardLit GGX gate + material influence knobs.
// Mutable so ImGui can dial live (batcher_setStandardLitEnabled / batcher_setPbr*).
// Env-seeded defaults; no VBO rebuild needed (per-flush uniforms).
static int   s_standardLitEnabled = []() {
    const char* v = std::getenv("MC2_STANDARD_LIT_V1");
    return (v != nullptr && v[0] == '1');   // TEMPORARY DEFAULT-OFF (PBR mech rendering disabled); opt-in =1
}();
static float s_pbrMetallicInfluence = []() {
    const char* v = std::getenv("MC2_PBR_METALLIC_INFLUENCE");
    if (v) return (float)std::atof(v);
    const char* mat = std::getenv("MC2_MECH_SURFACE_MATERIAL");
    return (mat && strcmp(mat, "painted_subtle") == 0) ? 0.03f : 0.15f;
}();
static float s_pbrRoughnessMin = []() {
    const char* v = std::getenv("MC2_PBR_ROUGHNESS_MIN");
    if (v) return (float)std::atof(v);
    const char* mat = std::getenv("MC2_MECH_SURFACE_MATERIAL");
    return (mat && strcmp(mat, "painted_subtle") == 0) ? 0.65f : 0.45f;
}();
static float s_pbrRoughnessMax = []() {
    const char* v = std::getenv("MC2_PBR_ROUGHNESS_MAX");
    if (v) return (float)std::atof(v);
    const char* mat = std::getenv("MC2_MECH_SURFACE_MATERIAL");
    return (mat && strcmp(mat, "painted_subtle") == 0) ? 0.90f : 0.90f;
}();
static float s_pbrAmbientSpecularStrength = []() {
    const char* v = std::getenv("MC2_PBR_AMBIENT_SPECULAR");
    return v ? (float)std::atof(v) : 0.25f;
}();

// PBR-LAYERED-1: PaintedMetal003 paint layer + triplanar. All mutable via ImGui.
static uint32_t s_mechPaintSurfaceMaterialIdx = 0u;  // resolved lazily alongside s_mechSurfaceMaterialIdx
static float s_pbrWearStrength = []() {
    const char* v = std::getenv("MC2_PBR_WEAR_STRENGTH");
    if (v) return (float)std::atof(v);
    const char* mat = std::getenv("MC2_MECH_SURFACE_MATERIAL");
    return (mat && strcmp(mat, "painted_subtle") == 0) ? 0.0f : 1.0f;
}();
static int   s_pbrTriplanar = []() {
    const char* v = std::getenv("MC2_PBR_TRIPLANAR");
    return (v && v[0] == '1') ? 1 : 0;  // DEFAULT-OFF; enable with =1
}();
static float s_pbrTriplanarScale = []() {
    const char* v = std::getenv("MC2_PBR_TRIPLANAR_SCALE");
    return v ? (float)std::atof(v) : 0.2f;
}();
// PBR-IBL-MECH-1: SH-L2 image-based ambient for mechs.
// Shares g_iblShStrength with static props (same slider). Default-ON; =0 disables.
static const bool s_mechIblShEnabled = []() {
    const char* v = std::getenv("MC2_MECH_IBL_SH");
    return !(v != nullptr && v[0] == '0');
}();

// Cached uniform locations (set at program link time).
static GLint s_loc_u_instanceBase    = -1;
static GLint s_loc_u_materialFlags   = -1;
static GLint s_loc_terrainMVP     = -1;
static GLint s_loc_u_mvp             = -1;
static GLint s_loc_u_tex             = -1;
static GLint s_loc_u_fogValue        = -1;
static GLint s_loc_u_debugMode       = -1;
static GLint s_loc_u_mechAmbientV1Strength = -1;  // MECH-AMBIENT-1
static GLint s_loc_u_lightingMode    = -1;
static GLint s_loc_u_skinningMode    = -1;
static GLint s_loc_u_sunAxisMc2Fix   = -1;  // SUN-AXIS-FIX-1
// MECH-SPECULAR-V1: cached locations (all -1 on non-viewuniforms variants).
static GLint s_loc_u_mechSpecularV1Strength  = -1;
static GLint s_loc_u_mechMetalRoughness      = -1;
static GLint s_loc_u_mechGlassRoughness      = -1;
static GLint s_loc_u_mechGlassLumaThresh     = -1;
static GLint s_loc_u_mechGlassMaxChanThresh  = -1;
static GLint s_loc_u_mechSpecDebugMask       = -1;
static GLint s_loc_u_mechBackFillStrength    = -1;
// BT2018-MECH-MATERIAL-TUNING-1: live ImGui-tunable imported-mech albedo knobs
// (external linkage; bound by the Graphics Options "BT2018 Imported Skin" sliders).
// Applied imported-mech-only in mech.frag (renderFlags bit 3); stock untouched.
//
// Defaults are GAMMA-NEUTRAL (1.0) + a mild brightness lift (1.1), NOT full sRGB
// decode. Strict pow(2.2) is "technically sRGB-correct" but MC2's legacy Blinn mech
// path is not a linear/PBR pipeline (no energy model / tone-map / env fill), so
// linearizing only the albedo crushes dark BT skins while nothing downstream
// compensates. User-dialed visual result: gamma 1.0 + scale 1.1 reads right against
// stock for both a light (marauder) and a dark (blackwidow) reference mech.
float g_importedMechGamma       = 1.0f;
float g_importedMechAlbedoScale = 1.1f;
float g_importedMechAoStrength  = 0.5f;   // AO-1: imported-mech AO multiply strength
float g_importedMechNormalStrength = 1.0f; // NORMALS-1: imported-mech normal-map strength

// VK-PARITY-MECH-1: shared producer of the default mech shading knobs (see header).
void gpuMechShadingParams(GpuMechShadingParams* o) {
    if (!o) return;
    o->ambientStrength     = s_mechAmbientV1 ? s_mechAmbientStrength : 0.0f;
    o->specularStrength    = (s_mechSpecularV1 && s_mechViewUniforms)
                                 ? s_mechSpecularStrength : 0.0f;
    o->metalRoughness      = s_mechMetalRoughness;
    o->glassRoughness      = s_mechGlassRoughness;
    o->glassLumaThresh     = s_mechGlassLumaThresh;
    o->glassMaxChanThresh  = s_mechGlassMaxChanThresh;
    o->specDebugMask       = s_mechSpecDebugMask ? 1 : 0;
    o->importedGamma       = g_importedMechGamma;
    o->importedAlbedoScale = g_importedMechAlbedoScale;
    o->aoStrength          = g_importedMechAoStrength;
    o->normalStrength      = g_importedMechNormalStrength;
}

static GLint s_loc_u_importedGamma           = -1;
static GLint s_loc_u_importedAlbedoScale     = -1;
static GLint s_loc_u_mechAoTex               = -1;   // AO-1: AO sampler (unit 6)
static GLint s_loc_u_aoStrength              = -1;
static GLint s_loc_u_mechNormalTex           = -1;   // NORMALS-1: normal sampler (unit 7)
static GLint s_loc_u_normalStrength          = -1;
// Slice C1: StandardLit toggle. Returns -1 until mech.frag declares the uniform;
// guarded with >= 0 at upload so no crash when shader lacks it.
static GLint s_loc_u_standardLitEnabled      = -1;
// Slice C2: PBR detail material samplers + tile scale.
static GLint s_loc_u_pbrNormalTex            = -1;
static GLint s_loc_u_pbrOrmTex               = -1;
static GLint s_loc_u_pbrTileScale            = -1;
// PBR-TUNE-1: material influence knobs (metallic scale, roughness clamp, ambient specular).
static GLint s_loc_u_pbrMetallicInfluence      = -1;
static GLint s_loc_u_pbrRoughnessMin           = -1;
static GLint s_loc_u_pbrRoughnessMax           = -1;
static GLint s_loc_u_pbrAmbientSpecularStrength = -1;
// PBR-LAYERED-1: paint layer + triplanar.
static GLint s_loc_u_pbrPaintNormalTex  = -1;
static GLint s_loc_u_pbrPaintOrmTex     = -1;
static GLint s_loc_u_pbrWearStrength    = -1;
static GLint s_loc_u_pbrTriplanar       = -1;
static GLint s_loc_u_pbrTriplanarScale  = -1;
// PBR-IBL-MECH-1: SH-L2 ambient.
static GLint s_loc_u_iblSh             = -1;
static GLint s_loc_u_iblShStrength     = -1;

// Geometry (immutable after finalizeGeometry).
static GLuint s_sharedVao = 0;
static GLuint s_sharedVbo = 0;
static GLuint s_sharedIbo = 0;
static GLuint s_sampler   = 0;    // session-lifetime; GL_REPEAT / LINEAR
static bool   s_geometryFinalized = false;
// VPL-#11 (SP-logistics late roster): set by registerTypeLod when a type
// registers AFTER finalizeGeometry (campaign .fit resume spawns the
// player force-group post-Mission::init-finalize). finalizePending()
// consumes it to rebuild the shared VBO/IBO from the (retained) staging.
static bool   s_pendingLateTypes = false;
static uint32_t s_lateStagedCount = 0;  // late (type,lod) regs since last finalizePending

// Staging buffers. RETAINED for the whole mission (NOT freed at
// finalizeGeometry) so finalizePending() can append a late-registered
// type and rebuild the immutable shared VBO/IBO from the full
// original+late staging (VPL-#11 SP-logistics fix; the alternative of
// re-deriving geometry from TG type shapes is fragile). Cleared only at
// onMapLoad(). Memory cost is MB-scale and intentionally not optimized.
static std::vector<uint8_t>   s_stagingVbo;
static std::vector<uint32_t>  s_stagingIbo;

// Type registration.
static std::vector<GpuMechTypeLodRecord> s_typeLodRecords;
static std::vector<GpuMechPacket>        s_packets;

// Key: (Mech3DAppearanceType*, lod) -> s_typeLodRecords index
struct TypeLodKey {
    const Mech3DAppearanceType* type;
    int lod;
    bool operator==(const TypeLodKey& o) const { return type == o.type && lod == o.lod; }
};
struct TypeLodKeyHash {
    size_t operator()(const TypeLodKey& k) const {
        return std::hash<const void*>()(k.type) ^ (std::hash<int>()(k.lod) * 2654435761u);
    }
};
static std::unordered_map<TypeLodKey, uint32_t, TypeLodKeyHash> s_typeLodIndex;

// Per-frame ring SSBOs.
static GLuint   s_instanceSsbo    = 0;
static GLuint   s_boneSsbo        = 0;
// VK-SHADOW-RING-OWNER-1: parallel GpuBufferOwner identity records for the two
// per-frame ring SSBOs (logical id + lifetime + debug name + handle). The raw GLuint
// globals above are UNCHANGED so every gen/bind/map/delete site keeps its exact GL
// args/order; the owner record's backendHandle is written straight from the freshly
// gen'd handle (pattern (b): `owner.backendHandle = s_xxxSsbo` right after glGenBuffers,
// mirrors view_uniforms). Registered at create, invalidated on destroy (grow-realloc +
// onMapUnload). Lifetime Persistent: persistent-mapped, realloc-on-grow, freed on unload.
static RenderCore::GpuBufferOwner s_instanceSsboOwner{
    RenderCore::RenderResourceId::MechInstanceRingSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "MechInstanceRingSsbo",
    0u};
static RenderCore::GpuBufferOwner s_boneSsboOwner{
    RenderCore::RenderResourceId::MechBoneRingSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "MechBoneRingSsbo",
    0u};
static void*    s_instanceMap     = nullptr;
static void*    s_boneMap         = nullptr;
static size_t   s_instanceCapacity = 0;  // per ring slot (in GpuMechInstance units)
static size_t   s_boneCapacity     = 0;  // per ring slot (in GpuMechBone units)
static uint32_t s_frameSlot        = 0;
static GLsync   s_fence[MECH_RING_FRAMES] = {};

static constexpr size_t kInitialInstancesPerFrame = 512;
static constexpr size_t kInitialBonesPerFrame     = 8192;

// Per-frame pending submit list.
struct PendingSubmit {
    GpuMechSubmitDesc        desc;
    std::vector<GpuMechBone> bones;          // staged from listOfShapes[i].shapeToWorld
    std::vector<uint32_t>    packetTexHandles; // per-packet live gosHandle captured at submit
    uint32_t                 typeLodIdx;
};
static std::vector<PendingSubmit> s_pendingSubmits;

// MECH-EXTRACTION-0: L2 handoff / persist buffer.
//
// Cross-phase lifetime — this MUST remain a std::vector (or equivalent heap storage).
// It CANNOT be moved to the RenderSnapshot FrameArena.
//
// Write path: flush() (inside DoGameLogic()) fills s_mechExtractPersist and then
//   immediately calls s_pendingSubmits.clear().  At that point the FrameArena for
//   the current frame has not been allocated yet.
//
// Read path: ExtractRenderSnapshot() (after DoGameLogic() returns) calls
//   batcher_getMechPendingCount/Entry, copies into snap.mechPackets which IS
//   FrameArena-backed, then calls batcher_compareMechSnapshot().
//
// The FrameArena is reset at the top of ExtractRenderSnapshot() — after
// DoGameLogic() exits — so any pointer written during flush() into the arena
// would be use-after-reset by the time the read path runs.
// Keep the batcher-side vector as the authoritative handoff buffer; the
// FrameArena copy in snap.mechPackets is the consumer-side view.
static std::vector<ExtractedMechPacket> s_mechExtractPersist;

// Per-frame draw-call snapshot persisted at the END of flush() for use by
// flushShadow() on the NEXT frame.  flushShadow() runs before this frame's
// flush(), so it reads last frame's already-fenced SSBO slot (s_frameSlot)
// together with the drawCalls that were built for that same slot.
struct ShadowDrawEntry {
    uint32_t globalPacketIdx;
    uint32_t instanceBase;
    uint32_t instanceCount;
};
static std::vector<ShadowDrawEntry> s_lastDrawCalls;
static size_t                       s_lastTotalInstances = 0;
static size_t                       s_lastTotalBones     = 0;

// V1A: per-frame submit count latched at flush() entry (before any
// early-return guards clear s_pendingSubmits). Counts GpuMechBatcher
// submits only — does NOT include MLR fallback draws or alive count.
// queryVisibility() reads this for mechs_visible in VisibilityResult.
static uint64_t                     s_lastFlushSubmitCount = 0;

// File-scope counters written by flushShadow() and read by Task 6 probe.
static int s_shadowTypesDrawn = 0;
static int s_shadowInstDrawn  = 0;

// Counters.
static bool     s_mechBatcherTrace     = false;
static bool     s_mechBatcherTraceInit = false;
static bool     s_mechLightTrace       = false;
static bool     s_mechLightTraceInit   = false;
static uint32_t s_lightCacheFullFrames = 0;  // monotonic; emitted on first overflow per frame
static uint32_t s_eligibleActorsThisFrame = 0;
// MECH-LIGHTROW-FRESHNESS-1: bound is (int)GpuMechFallbackReason::StaleLightRow + 1.
// Sized from the enum rather than a bare literal so a future append cannot
// silently write past the end -- the previous bare 5 was exactly that hazard.
static constexpr int kGpuMechFallbackReasonCount =
    static_cast<int>(GpuMechFallbackReason::StaleLightRow) + 1;
static uint32_t s_fallbacksThisFrame[kGpuMechFallbackReasonCount] = {};  // indexed by GpuMechFallbackReason

// M2.5 (Q4): always-on per-mission counter of per-instance fills whose
// objectIdRaw was non-zero. Incremented in flush()'s per-instance loop;
// emitted on the per-mission [MECHBATCHER v1] event=mech_id_summary
// line from onMapUnload(); reset in onMapLoad() and on emit.
//
// Per adversarial M1: the per-frame intermediate (s_gpuMechIdWritesThisFrame)
// was dropped -- it had NO consumer (only the per-mission counter is read
// by the emit). Direct per-mission accumulation removes three would-be
// reset sites (onMapLoad, flush early-out, flush normal end at line 1365)
// from M2.5's edit surface.
//
// Counter is ALWAYS-ON (NOT env-gated). When env-OFF the handle bits are
// still written into inst.objectIdRaw (Q3 unconditional CPU fill), so
// the counter tracks writer volume regardless of env state.
static uint64_t s_gpuMechIdWritesThisMission = 0;
static uint64_t s_allowedLateRegEvents    = 0;
static uint64_t s_disallowedLateRegEvents = 0;
static bool     s_lastFailWasLateReg      = false;

// [SPOTLIGHT_REAL_TRACE v1] (E) Stage 0 / T0.2 baseline. Two-channel evidence:
//   (a) event=type_spotlight_node — emitted from registerTypeLod when a
//       SpotLight_-prefixed TypeNode is skipped during recipe build. Counts
//       distinct (typeLod, nodeIdx) tuples. Per-process monotonic; not gated
//       on per-frame cadence.
//   (b) event=mech_spotlight_draw — emitted from submitActor for actors whose
//       mechType carries >= 1 spotlight TypeNode (seen at recipe build).
//       Per-frame counter, 600-frame summary alongside mono.
// Map keyed by Mech3DAppearanceType* (the desc.mechType field on submitActor).
// Value is the number of SpotLight_-prefixed TypeNodes seen during ANY LOD's
// recipe build (max across LODs, since each instance only renders at one LOD).
static const bool s_spotlightRealTrace =
    (getenv("MC2_SPOTLIGHT_REAL_TRACE") != nullptr);
static std::unordered_map<const Mech3DAppearanceType*, uint32_t>
    s_spotlightRealTypeNodeCount;
static uint64_t s_spotlightReal_typeNodeEvents = 0;  // monotonic (a)
static uint64_t s_spotlightReal_drawWindow     = 0;  // 600-frame window (b)
static uint64_t s_spotlightReal_drawMono       = 0;  // monotonic (b)
static uint64_t s_spotlightReal_drawFrameCount = 0;  // frames flushed since process start
static bool     s_spotlightReal_firstHitTypeNode = false;
static bool     s_spotlightReal_firstHitDraw     = false;

// ---------------------------------------------------------------------------
// MaterialGpu Mech-1 substrate
// MC2_MATERIAL_GPU defaults ON. Set MC2_MATERIAL_GPU=0 to disable.
// ---------------------------------------------------------------------------
#include "../../RenderCore/MaterialGpu.h"
#include "../../RenderCore/GpuBufferOwner.h"
static const bool s_mechMaterialGpuEnabled = []() {
    const char* v = getenv("MC2_MATERIAL_GPU");
    return v == nullptr || (v[0] != '0');
}();
static std::vector<RenderCore::MaterialGpu>        s_mechMaterialTable;
static std::unordered_map<uint32_t, uint32_t>      s_mechHandleToMaterialIdx;
static std::vector<uint32_t>                       s_mechDrawMaterialIdx;
// MECH-MATERIAL-SSBO-OWNER-1: per-actor MaterialGpu table SSBO (binding 2),
// narrowed behind a GpuBufferOwner identity record (logical id + lifetime +
// debug name + GLuint value). GL calls happen at the same sites with the same
// args/order/flags/slot; the raw handle is reached only via owner.backendHandle.
// Lifetime Persistent: survives across frames, torn down on map load/unload.
static RenderCore::GpuBufferOwner                  s_mechMaterialSsbo{
    RenderCore::RenderResourceId::MaterialGpuBuffer,
    RenderCore::RenderResourceLifetime::Persistent,
    "MaterialGpuBuffer",
    0u};
static uint32_t                                    s_mechMaterialGpuFrameCount = 0;

// Slice C1: named profile index for binding-7 SSBO (gos_materials).
// Resolved lazily at first flush() (after gos_materials::init() runs).
// 0 = default/passthrough (env unset or none/0). Non-zero = named profile active;
// overrides per-instance materialIdx only when selected so the existing per-actor
// albedo table (binding 2) is undisturbed when MC2_MECH_SURFACE_MATERIAL is unset.
// Protected by s_mechSurfaceMaterialIdxResolved to call getProfileIndex() only once.
static uint32_t s_mechSurfaceMaterialIdx         = 0u;
static bool     s_mechSurfaceMaterialIdxResolved = false;

// ---------------------------------------------------------------------------
// Shader load
// ---------------------------------------------------------------------------
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    // M2.5: GLSL preprocessor does NOT inherit C++ build flags
    // (memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
    // Build the prefix as a std::string and append the
    // MC2_OBJECT_ID_BUFFER macro definition when the env gate is on,
    // mirroring gos_static_prop_batcher.cpp:510-521.
    std::string mechPrefix = "#version 430\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        mechPrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }
    // MECH-VIEWUNIFORMS-1: gated opt-in only (default OFF). Pulls the binding=3
    // ViewUniformsBlock into mech.vert via its #ifdef. The layout(binding=3)
    // qualifier is honored at link (verified below under diag), so no explicit
    // glUniformBlockBinding is needed.
    if (s_mechViewUniforms) {
        mechPrefix += "#define MC2_USE_VIEW_UNIFORMS 1\n";
    }
    // BT2018-MECH-MATERIAL-GAMMA-1: enable the imported-mech sRGB->linear albedo
    // correction. mech.vert forwards renderFlags bit 3; mech.frag linearizes albedo
    // for imported mechs only. Always defined — stock mechs have the bit unset, so
    // the runtime path is byte-identical for them (the varying is just always 0).
    mechPrefix += "#define MC2_IMPORTED_MECH_MATERIAL 1\n";

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", mechPrefix.c_str());

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        MC2_DIAGFE(stderr,
            "[MECHBATCHER v1] event=shader_fail — GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;

    // MECH-PIPELINEDESC-1: register the linked mech program into PipelineRegistry
    // so flush() can drive fixed-function state via applyPipeline(MechOpaque)
    // instead of hand-set GL calls. Mirrors gos_static_prop_batcher.cpp's
    // bindProgram-at-link. Must run before any mech draw (flush() asserts
    // geometry finalized, which calls loadProgramsIfNeeded first).
    RenderCore::bindProgram(RenderCore::PipelineId::MechOpaque, s_mechProgram);

    // SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: record the logical shader-variant
    // identity (canonical define-set key) into the pipeline-key path. Derived from
    // mechPrefix, so it is IDENTICAL whether the mech stages loaded as GLSL or
    // SPIR-V — the SPIR-V artifact selection lives per-stage below this bound
    // program and cannot fork pipeline-key accounting.
    {
        std::string pkVariant = std::string("mech|") +
            glsl_program::shaderDefineKey(mechPrefix.c_str());
        RenderCore::recordPipelineVariantKey(RenderCore::PipelineId::MechOpaque,
                                             pkVariant.c_str());
        // VERTEXLAYOUT-AUTHORITY-1: the MechOpaque PipelineKey is (shaderVariant +
        // vertexLayout). Record the fixed GpuMechVertex 48B VAO identity
        // (gos_mech_batcher.cpp:1376-1382). Pure metadata; no GL change.
        RenderCore::recordPipelineVertexLayout(RenderCore::PipelineId::MechOpaque,
                                               RenderCore::VertexLayoutId::MechGpuVertex48B);
        fprintf(stderr, "[PIPELINE_VARIANT] pipeline=MechOpaque glProgram=%u key=%s vertexLayout=%s\n",
                s_mechProgram, pkVariant.c_str(),
                RenderCore::vertexLayoutName(RenderCore::VertexLayoutId::MechGpuVertex48B));
        fflush(stderr);
    }

    // Slice B1 (2026-05-09): MAJOR-3 from adversarial review.
    // shaders/include/lighting.hglsl declares LightsData[64] (~113 KB
    // std140). GL spec mandates only 16 KB minimum support; many
    // older drivers / iGPUs cap at 64 KB. If the host GPU can't fit
    // the UBO, force g_useGpuMechLighting off so the shader's
    // u_lightingMode=0 branch (Slice A flat-white) runs and the
    // app doesn't fail compilation or read garbage.
    // [LIGHTSSBO v1] RF4: LightsData is no longer a UBO — it is an
    // unbounded std430 SSBO (binding 20). The old GL_MAX_UNIFORM_BLOCK_SIZE
    // gate (which forced g_useGpuMechLighting=false on GPUs whose max UBO
    // block < ~113KB) is now SPURIOUS: SSBO storage is bounded by
    // GL_MAX_SHADER_STORAGE_BLOCK_SIZE (typically >=128MB, never the
    // constraint) and there is no fixed 64-entry window. Disabling mech
    // lighting on the UBO basis would defeat the ceiling-removal this
    // conversion delivers. Gate removed.
    // Diagnostic gated behind MC2_LIGHTSSBO_TRACE (demote-not-delete;
    // matches gameos_graphics.cpp s_lightSsboTrace). Default-off so it
    // does not pollute frame-time captures.
    if (std::getenv("MC2_LIGHTSSBO_TRACE") != nullptr) {
        GLint maxSsbo = 0;
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSsbo);
        std::fprintf(stderr,
            "[LIGHTSSBO v1] event=mech_ssbo_check max_ssbo_block=%d (no 64-slot cap)\n",
            maxSsbo);
    }

    auto loc = [&](const char* name) {
        return glGetUniformLocation(s_mechProgram, name);
    };
    s_loc_u_instanceBase    = loc("u_instanceBase");
    s_loc_u_materialFlags   = loc("u_materialFlags");
    s_loc_terrainMVP     = loc("u_worldToClipGL");
    s_loc_u_mvp             = loc("u_mvp");
    s_loc_u_tex             = loc("u_tex");
    s_loc_u_fogValue        = loc("u_fogValue");
    s_loc_u_debugMode       = loc("u_debugMode");
    s_loc_u_mechAmbientV1Strength = loc("u_mechAmbientV1Strength");  // MECH-AMBIENT-1
    s_loc_u_lightingMode    = loc("u_lightingMode");
    s_loc_u_skinningMode    = loc("u_skinningMode");
    s_loc_u_sunAxisMc2Fix   = loc("u_sunAxisMc2Fix");   // SUN-AXIS-FIX-1
    // MECH-SPECULAR-V1: all return -1 on non-viewuniforms variants; guarded at upload.
    s_loc_u_mechSpecularV1Strength  = loc("u_mechSpecularV1Strength");
    s_loc_u_mechMetalRoughness      = loc("u_mechMetalRoughness");
    s_loc_u_mechGlassRoughness      = loc("u_mechGlassRoughness");
    s_loc_u_mechGlassLumaThresh     = loc("u_mechGlassLumaThresh");
    s_loc_u_mechGlassMaxChanThresh  = loc("u_mechGlassMaxChanThresh");
    s_loc_u_mechSpecDebugMask       = loc("u_mechSpecDebugMask");
    s_loc_u_mechBackFillStrength    = loc("u_mechBackFillStrength");
    // BT2018-MECH-MATERIAL-GAMMA-1/TUNING-1: imported-mech albedo knobs (locations
    // exist only when MC2_IMPORTED_MECH_MATERIAL is defined; guarded >= 0 at upload).
    s_loc_u_importedGamma           = loc("u_importedGamma");
    s_loc_u_importedAlbedoScale     = loc("u_importedAlbedoScale");
    s_loc_u_mechAoTex               = loc("u_mechAoTex");    // AO-1
    s_loc_u_aoStrength              = loc("u_aoStrength");
    s_loc_u_mechNormalTex           = loc("u_mechNormalTex"); // NORMALS-1
    s_loc_u_normalStrength          = loc("u_normalStrength");
    // Slice C1: StandardLit toggle (default 0 = passthrough; shader may not declare it yet).
    s_loc_u_standardLitEnabled      = loc("u_standardLitEnabled");
    // Slice C2: PBR detail surface samplers + tile scale.
    s_loc_u_pbrNormalTex            = loc("u_pbrNormalTex");
    s_loc_u_pbrOrmTex               = loc("u_pbrOrmTex");
    s_loc_u_pbrTileScale            = loc("u_pbrTileScale");
    // PBR-TUNE-1: material influence knobs.
    s_loc_u_pbrMetallicInfluence       = loc("u_pbrMetallicInfluence");
    s_loc_u_pbrRoughnessMin            = loc("u_pbrRoughnessMin");
    s_loc_u_pbrRoughnessMax            = loc("u_pbrRoughnessMax");
    s_loc_u_pbrAmbientSpecularStrength = loc("u_pbrAmbientSpecularStrength");
    // PBR-LAYERED-1: paint layer + triplanar.
    s_loc_u_pbrPaintNormalTex  = loc("u_pbrPaintNormalTex");
    s_loc_u_pbrPaintOrmTex     = loc("u_pbrPaintOrmTex");
    s_loc_u_pbrWearStrength    = loc("u_pbrWearStrength");
    s_loc_u_pbrTriplanar       = loc("u_pbrTriplanar");
    s_loc_u_pbrTriplanarScale  = loc("u_pbrTriplanarScale");
    // PBR-IBL-MECH-1: SH-L2 ambient (shared coeffs with static props).
    s_loc_u_iblSh              = loc("u_iblSh");
    s_loc_u_iblShStrength      = loc("u_iblShStrength");

    // MECH-VIEWUNIFORMS-1: on the gated path, verify the ViewUniformsBlock is
    // present and bound to point 3. The GLSL layout(binding=3) qualifier is
    // honored at link (GL 4.2+ core; same as static_prop), so NO explicit
    // glUniformBlockBinding is required — confirmed by the diag below reporting
    // binding=3 with no intervention. Read-only verification, logged under
    // MC2_MECH_VIEWUNIFORMS_DIAG. (The original "block missing / bound to 0"
    // symptom was a deploy gap: the edited shader was never copied to the
    // deploy dir — see docs/mech-viewuniforms-source-dump.md.)
    if (s_mechViewUniforms && s_mechViewUniformsDiag) {
        GLuint vuBlockIdx = glGetUniformBlockIndex(s_mechProgram, "ViewUniformsBlock");
        GLint vuBind = -1;
        if (vuBlockIdx != GL_INVALID_INDEX)
            glGetActiveUniformBlockiv(s_mechProgram, vuBlockIdx,
                                      GL_UNIFORM_BLOCK_BINDING, &vuBind);
        std::fprintf(stderr,
            "[MECH_VU_DIAG v1] event=block_verify prog=%u vu_block_idx=%u "
            "binding=%d (expect 3) loc_u_worldToClipGL=%d\n",
            s_mechProgram, vuBlockIdx, vuBind, s_loc_terrainMVP);
    }

    MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=shader_ok prog=%u\n", s_mechProgram);
}

// ---------------------------------------------------------------------------
// Ring SSBO management
// ---------------------------------------------------------------------------
static void ensureRingCapacity(size_t neededInstances, size_t neededBones) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededBones     > s_boneCapacity;
    if (!needGrow) return;

    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    // VK-SHADOW-RING-OWNER-1: invalidate owner record + registry slot on destroy.
    if (s_instanceSsbo) {
        glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr;
        s_instanceSsboOwner.backendHandle = 0;
        RenderCore::RenderResourceDesc invalid; invalid.id = RenderCore::RenderResourceId::MechInstanceRingSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_boneSsbo) {
        glDeleteBuffers(1, &s_boneSsbo); s_boneSsbo = 0; s_boneMap = nullptr;
        s_boneSsboOwner.backendHandle = 0;
        RenderCore::RenderResourceDesc invalid; invalid.id = RenderCore::RenderResourceId::MechBoneRingSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }

    s_instanceCapacity = std::max(neededInstances,
        s_instanceCapacity ? s_instanceCapacity * 2 : kInitialInstancesPerFrame);
    s_boneCapacity = std::max(neededBones,
        s_boneCapacity ? s_boneCapacity * 2 : kInitialBonesPerFrame);

    // SSBO-BIND-ALIGN: the per-slot bind offset is slot * capacity * sizeof. It
    // MUST be a multiple of GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (256 on
    // NVIDIA) or slots 1+ bind at a misaligned offset -> NVIDIA rejects with
    // GL_INVALID_VALUE -> the instance/bone SSBO never binds -> the VS reads
    // garbage -> mechs invisible/garbled (AMD tolerated it). Rounding capacity up
    // to a multiple of the alignment (in ELEMENTS) makes capacity*sizeof a multiple
    // of the alignment, so every ring-slot offset (and the alloc + CPU write
    // pointer, which also derive from capacity) is aligned. Element-count rounding
    // avoids the lossy byte/sizeof division when sizeof does not divide 256.
    {
        const unsigned long long a = (unsigned long long)gpuSsboOffsetAlignment();
        s_instanceCapacity = (size_t)gpuAlignUp((unsigned long long)s_instanceCapacity, a);
        s_boneCapacity     = (size_t)gpuAlignUp((unsigned long long)s_boneCapacity,     a);
    }

    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    // VK-SHADOW-RING-OWNER-1: now OWNED (GpuBufferOwner) — the old TIER2-EXCLUDED:ring
    // tag is removed (a buffer is owned XOR excluded). Same GL args/order as before.
    glGenBuffers(1, &s_instanceSsbo);
    s_instanceSsboOwner.backendHandle = s_instanceSsbo;  // mirror handle into owner
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)),
        nullptr, flags);
    s_instanceMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)), flags);
    {
        // VK-SHADOW-RING-OWNER-1: register the slot (observe-only metadata; never read by the draw path).
        RenderCore::RenderResourceDesc d;
        d.id        = s_instanceSsboOwner.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_instanceSsboOwner.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_instanceSsboOwner.debugName;
        d.glName    = s_instanceSsbo;
        d.sizeBytes = static_cast<uint64_t>(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance));
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // VK-SHADOW-RING-OWNER-1: now OWNED (GpuBufferOwner) — TIER2-EXCLUDED:ring removed.
    glGenBuffers(1, &s_boneSsbo);
    s_boneSsboOwner.backendHandle = s_boneSsbo;  // mirror handle into owner
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_boneSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)),
        nullptr, flags);
    s_boneMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)), flags);
    {
        RenderCore::RenderResourceDesc d;
        d.id        = s_boneSsboOwner.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_boneSsboOwner.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_boneSsboOwner.debugName;
        d.glName    = s_boneSsbo;
        d.sizeBytes = static_cast<uint64_t>(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone));
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_boneMap) {
        MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=persistent_map_fail\n");
    }
    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=ring_alloc instances=%zu bones=%zu\n",
        s_instanceCapacity, s_boneCapacity);
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
GpuMechBatcher& GpuMechBatcher::instance() {
    static GpuMechBatcher batcher;
    return batcher;
}

void GpuMechBatcher::onMapLoad() {
    // M2.5 (Q4): per-mission writer counter; emitted on onMapUnload.
    s_gpuMechIdWritesThisMission = 0;
    s_typeLodRecords.clear();
    s_packets.clear();
    s_typeLodIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
#ifdef MC2_VULKAN_ISLAND
    s_vkVboNativeCopy.clear();   // VK-MECH-FEED-NATIVE-1: geometry rebuilt per map
#endif
    s_nodeVboRanges.clear();   // MECH-NORMALS-FIX-1: per-node ranges are per-mission
    s_geometryFinalized = false;
    // S2.12: DO NOT reset s_programLoadTried / s_programLoadFailed here.
    // The GL program created by loadProgramsIfNeeded() is process-global
    // state cached in shader_builder's s_programs map under the name "mech".
    // Resetting these flags causes loadProgramsIfNeeded() to call
    // glsl_program::makeProgram("mech", ...) again on the second map load,
    // which the shader_builder cache rejects with
    // "Program with this name (mech) already exists" — disabling the GPU
    // mech path on every load after the first. Program creation parameters
    // (prefix derived from RenderWorld::IsObjectIdBufferEnabled() etc.) do
    // not change per-mission, so a one-shot first-load is correct.
    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
    // Clear persisted shadow state so flushShadow() finds no stale entries
    // that index into the now-cleared s_packets.  Order-independent vs the
    // !s_instanceSsbo guard in flushShadow().
    s_lastDrawCalls.clear();
    s_lastTotalInstances = 0;
    s_lastTotalBones     = 0;
    // Mech-1: reset MaterialGpu SSBO and tables on map load
    if (s_mechMaterialSsbo.backendHandle != 0) {
        GLuint local = static_cast<GLuint>(s_mechMaterialSsbo.backendHandle);
        glDeleteBuffers(1, &local);
        s_mechMaterialSsbo.backendHandle = 0;

        // REGISTRY-MATERIAL-SSBO-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::MaterialGpuBuffer;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    s_mechMaterialTable.clear();
    s_mechHandleToMaterialIdx.clear();
    s_mechDrawMaterialIdx.clear();
    s_mechMaterialGpuFrameCount = 0;
    MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=map_load\n");
}

void GpuMechBatcher::onMapUnload() {
    // M2.5 (Q6 amendment 2): consume the MLR-side per-mission counter
    // and emit on its own [MECHBATCHER v1] event=mlr_mech_summary line.
    // The split-line shape is OFFICIAL (external-review M1): the two
    // counters live in different TUs and MUST emit on two adjacent
    // lines (mlr first, then gpu_mech_id). Do not collapse to one line.
    // consumeAndResetMlrMechDraws is forward-declared at file scope
    // (external-review C1) -- declaration is NOT inside this function body.
    const uint64_t mlrDraws = consumeAndResetMlrMechDraws();
    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=%llu\n",
        (unsigned long long)mlrDraws);

    // M2.5 (Q4 + Q6 amendment 2): always-on per-mission writer summary.
    // Surfaces gpu_mech_id_writes to the M2.6 readiness decision rule.
    // Always-on (NOT env-gated): M2.6 needs this signal regardless of
    // MC2_OBJECT_ID_BUFFER state to size the MLR-vs-GPU split.
    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=%llu\n",
        (unsigned long long)s_gpuMechIdWritesThisMission);
    s_gpuMechIdWritesThisMission = 0;

    // MECH-DECLINE-FIX-1: always-on per-mission light-row freshness summary.
    // These counters previously existed ONLY inside their own once-per-mech-type
    // stderr line in mech3d.cpp, so a count that started or grew after that first
    // line was unobservable and "check the counter on your first launch" was not
    // executable. This is the always-on per-mission line, adjacent to the two
    // above, that a supervised run actually sees.
    // stale=0 means the freshness gate is INERT (the expected steady-state
    // result -- reset/stamp/check share one g_mc2FrameCounter epoch). Non-zero
    // means Mech3DAppearance::update() is not running at render cadence for some
    // actor, and the repair is to re-stamp at update() time. oob MUST read zero;
    // it is an invariant assertion, and a non-zero value means a clamped index.
    {
        uint64_t staleRows = 0, oobRows = 0;
        consumeAndResetMechLightRowCounters(&staleRows, &oobRows);
        MC2_DIAGFE(stderr,
            "[MECHBATCHER v1] event=stale_light_row_summary stale=%llu oob=%llu\n",
            (unsigned long long)staleRows, (unsigned long long)oobRows);
    }
    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_sharedVao)    { glDeleteVertexArrays(1, &s_sharedVao);    s_sharedVao    = 0; }
    if (s_sharedVbo)    { glDeleteBuffers(1, &s_sharedVbo);         s_sharedVbo    = 0; }
    if (s_sharedIbo)    { glDeleteBuffers(1, &s_sharedIbo);         s_sharedIbo    = 0; }
    if (s_sampler)      { glDeleteSamplers(1, &s_sampler);          s_sampler      = 0; }
    // VK-SHADOW-RING-OWNER-1: invalidate owner record + registry slot on destroy.
    if (s_instanceSsbo) {
        glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr;
        s_instanceSsboOwner.backendHandle = 0;
        RenderCore::RenderResourceDesc invalid; invalid.id = RenderCore::RenderResourceId::MechInstanceRingSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_boneSsbo) {
        glDeleteBuffers(1, &s_boneSsbo); s_boneSsbo = 0; s_boneMap = nullptr;
        s_boneSsboOwner.backendHandle = 0;
        RenderCore::RenderResourceDesc invalid; invalid.id = RenderCore::RenderResourceId::MechBoneRingSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    s_instanceCapacity  = 0;
    s_boneCapacity      = 0;
    s_geometryFinalized = false;
    s_pendingSubmits.clear();
    s_mechExtractPersist.clear();  // MECH-EXTRACTION-0
    // Mech-1: teardown MaterialGpu SSBO and tables
    if (s_mechMaterialSsbo.backendHandle != 0) {
        GLuint local = static_cast<GLuint>(s_mechMaterialSsbo.backendHandle);
        glDeleteBuffers(1, &local);
        s_mechMaterialSsbo.backendHandle = 0;

        // REGISTRY-MATERIAL-SSBO-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::MaterialGpuBuffer;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    s_mechMaterialTable.clear();
    s_mechHandleToMaterialIdx.clear();
    s_mechDrawMaterialIdx.clear();
    // Slice C1: release named material profile SSBO (binding 7).
    // shutdown() resets s_initialized so init() can be called again on next map load.
    // Also reset the lazy-resolve guard so the profile index is re-queried next mission.
    gos_materials::shutdown();
    s_mechSurfaceMaterialIdx         = 0u;
    s_mechPaintSurfaceMaterialIdx    = 0u;
    s_mechSurfaceMaterialIdxResolved = false;
    MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=map_unload\n");
}

bool GpuMechBatcher::wasLastFailureLateRegistration() const { return s_lastFailWasLateReg; }
uint64_t GpuMechBatcher::getAllowedLateRegEventCount()      { return s_allowedLateRegEvents; }
uint64_t GpuMechBatcher::getDisallowedLateRegEventCount()  { return s_disallowedLateRegEvents; }
bool GpuMechBatcher::isFinalized() const                    { return s_geometryFinalized; }

void GpuMechBatcher::recordEligibleActor()                         { ++s_eligibleActorsThisFrame; }
void GpuMechBatcher::recordCpuFallback(GpuMechFallbackReason r)   { ++s_fallbacksThisFrame[(int)r]; }
void GpuMechBatcher::flushShadow() {
    // Depth-only draw of the previous frame's mech instances into the dynamic
    // shadow FBO.  Called from Task-5's gos_BeginDynamicShadowPass region,
    // BEFORE this frame's flush().
    //
    // Ring-slot reasoning (verified against flush() line numbers):
    //   flush():913  s_frameSlot = (s_frameSlot+1) % MECH_RING_FRAMES  -- advance
    //   flush():920  SSBO written to new s_frameSlot                    -- write
    //   flush():1190 s_fence[s_frameSlot] = glFenceSync(...)            -- fence
    // flushShadow() runs before this frame's flush(), so s_frameSlot still
    // holds the PREVIOUS frame's post-advance value.  The previous frame's
    // data lives at s_frameSlot and was already fenced by the previous
    // flush():1097.  Read slot = s_frameSlot.  No advance / write / fence here.
    //
    // Bucket-reuse: Option B (persisted drawCalls).  flush()'s local DrawCall
    // vector is entangled with the ring write inside the same function, so we
    // persist it to s_lastDrawCalls / s_lastTotalInstances / s_lastTotalBones
    // at the end of flush() and consume those statics here.

    // DEFAULT ON. Kill-switch: MC2_SHADOW_ENABLE=0.
    static const bool s_shadowEnabled = !(getenv("MC2_SHADOW_ENABLE") != nullptr &&
                                          getenv("MC2_SHADOW_ENABLE")[0] == '0');
    if (!s_shadowEnabled) return;

    // Geometry-readiness guard, mirroring the color flush() path (:867).
    // The new txmmgr shadow region calls this from frame ~1 and across
    // mid-mission finalizePending() rebuilds; without this, a not-yet-
    // finalized or rebuilt s_packets is indexed by stale persisted
    // s_lastDrawCalls entries and faults in the draw below.
    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed) return;
    if (s_lastDrawCalls.empty()) return;
    if (!s_instanceSsbo || !s_boneSsbo) return;

    // Resolve shadow_mech program via the global registry (Task 1 registered
    // it under this key via makeProgram("shadow_mech", ...)).
    auto pit = glsl_program::s_programs.find("shadow_mech");
    if (pit == glsl_program::s_programs.end() || !pit->second || !pit->second->shp_)
        return;
    const GLuint shadowProg = pit->second->shp_;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Save/restore the GL state this pass perturbs, mirroring flush()'s
    // bracket (:1036-1043 / :1209-1218). Leaking program/VAO/element-
    // buffer/SSBO bindings 0+1 here poisons gpu_cull::compute_dispatch()
    // and the indirect flush() that run after -> invisible mechs+props.
    // All early returns above precede any GL mutation.
    GLint prevProgram = 0, prevVao = 0, prevElemBuf = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    // GlStateGuard adoption: SSBO slots 0/1 restore-previous, replacing the
    // hand-rolled prevSsbo0/prevSsbo1 save (was here) + restore (was at the end
    // of this function). SHADER_STORAGE_BUFFER base bindings are global context
    // state — independent of the VAO/program/element-buffer restored manually
    // below — so the scope-exit restore order is immaterial. flushShadow() has
    // no gos_InvalidateRenderStateCache() call, so function-scope guards are
    // correct here (cf. gl_state_guard.h slice-2 scoping note).
    mc2gl::GlScopedSsboBinding guardSsbo0(0);
    mc2gl::GlScopedSsboBinding guardSsbo1(1);

    glUseProgram(shadowProg);

    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

    const GLint smLoc   = glGetUniformLocation(shadowProg, "u_skinningMode");
    const GLint baseLoc = glGetUniformLocation(shadowProg, "u_instanceBase");

    if (smLoc >= 0)
        glUniform1i(smLoc, 1);   // SKIN-RETIRE-1: weighted skinning always on (no-op for single-bone data)

    glBindVertexArray(s_sharedVao);
    // Same root cause as GpuStaticPropBatcher::flushShadow: do not rely on
    // s_sharedVao carrying the IBO (other GPU paths clobber its VAO-resident
    // GL_ELEMENT_ARRAY_BUFFER binding); bind s_sharedIbo explicitly so the
    // indexed draw never treats firstIndex*4 as a client pointer.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Read the already-fenced previous-frame SSBO slot.
    const uint32_t slot = s_frameSlot;
    gpuBindSsboRange(0, s_instanceSsbo,
        (long long)(slot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (long long)(s_lastTotalInstances * sizeof(GpuMechInstance)),
        "mech.instance");
    gpuBindSsboRange(1, s_boneSsbo,
        (long long)(slot * s_boneCapacity * sizeof(GpuMechBone)),
        (long long)(s_lastTotalBones * sizeof(GpuMechBone)),
        "mech.bone");

    // GPU-SYNC-CONTRACT: same coherent instance/bone SSBOs feed the shadow draws;
    // order the CPU writes before these reads too (see flush() Step 7).
    gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                   "mech_instance_bone_shadow");

    int typesDrawn = 0, instDrawn = 0;
    for (const ShadowDrawEntry& dc : s_lastDrawCalls) {
        if (baseLoc >= 0)
            glUniform1i(baseLoc, (int)dc.instanceBase);

        // Skip (not break) a truly-stale index so one bad entry post-rebuild
        // does not drop the remaining (valid) draws -> missing parts.
        if (dc.globalPacketIdx >= s_packets.size()) continue;
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];
        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES,
            (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount,
            pkt.baseVertex);

        ++typesDrawn;
        instDrawn += (int)dc.instanceCount;
    }

    s_shadowTypesDrawn = typesDrawn;
    s_shadowInstDrawn  = instDrawn;

    // Restore exactly what we changed (mirrors flush()'s restore bracket)
    // so the cull dispatch + indirect flush() that follow are not poisoned.
    // ORDER MATTERS: restore VAO before element-buffer so glBindBuffer(ELEM)
    // writes into prevVao's state, not s_sharedVao's. Wrong order leaves
    // s_sharedVao.elemBuf=0 and flush() faults on the next indexed draw.
    // (SSBO slots 0/1 are restored by guardSsbo0/guardSsbo1 at scope exit.)
    glBindVertexArray((GLuint)prevVao);                          // VAO first
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf); // then elem
    glUseProgram((GLuint)prevProgram);
}

// ---------------------------------------------------------------------------
// Registration (Task 4)
// ---------------------------------------------------------------------------

// MECH-NORMALS-FIX-1: recompute normals for one node's triangle-soup vertices
// in-place, mech-locally, over the byte range [startByte, endByte). The verts
// are a triangle list (corners 3k,3k+1,3k+2 form triangle k), all in mech model
// space — same space as the cooked normal the shader already consumes, so no
// axis swap here. mode 1 = geometric face normal to all 3 corners (faceted).
// mode 2 = exact-position-grouped, angle-threshold accumulation that preserves
// hard edges (faces meeting at > smoothDeg do not blend). Grouping is exact on
// source position bits (shared verts have bit-identical TG positions) and stays
// within this node (separate rigid mech pieces are never blended). Face normals
// are oriented to the cooked-normal hemisphere to stay winding-agnostic.
// endByte: exclusive end of this node's data in vbo; use vbo.size() for
// "rest of buffer". Added for per-node bounding (MECH-NORMALS-FIX-1 rebuild).
static void recomputeMechNodeNormals(std::vector<uint8_t>& vbo,
                                     size_t startByte, size_t endByte,
                                     int mode, float smoothDeg) {
    if (mode != 1 && mode != 2) return;
    const size_t stride = sizeof(GpuMechVertex);
    if (startByte >= endByte || startByte >= vbo.size()) return;
    const size_t clampedEnd = (endByte < vbo.size()) ? endByte : vbo.size();
    const size_t n = (clampedEnd - startByte) / stride;   // corners
    if (n < 3) return;
    GpuMechVertex* V = reinterpret_cast<GpuMechVertex*>(vbo.data() + startByte);
    const size_t triCount = n / 3;

    auto dot3 = [](const float* a, const float* b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    };

    // Per-triangle area-weighted geometric normal (un-normalized cross), plus a
    // unit copy for the angle test. Oriented to agree with the cooked-normal
    // hemisphere (sum of the 3 corners' cooked normals) to handle winding.
    std::vector<std::array<float,3>> faceRaw(triCount), faceUnit(triCount);
    for (size_t k = 0; k < triCount; ++k) {
        const float* p0 = V[3*k+0].position;
        const float* p1 = V[3*k+1].position;
        const float* p2 = V[3*k+2].position;
        float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
        float fr[3] = { e1[1]*e2[2]-e1[2]*e2[1],
                        e1[2]*e2[0]-e1[0]*e2[2],
                        e1[0]*e2[1]-e1[1]*e2[0] };
        float ref[3] = {
            V[3*k+0].normal[0] + V[3*k+1].normal[0] + V[3*k+2].normal[0],
            V[3*k+0].normal[1] + V[3*k+1].normal[1] + V[3*k+2].normal[1],
            V[3*k+0].normal[2] + V[3*k+1].normal[2] + V[3*k+2].normal[2] };
        if (dot3(fr, ref) < 0.0f) { fr[0]=-fr[0]; fr[1]=-fr[1]; fr[2]=-fr[2]; }
        faceRaw[k] = { fr[0], fr[1], fr[2] };
        float m = std::sqrt(dot3(fr, fr));
        float inv = (m > 1e-12f) ? 1.0f/m : 0.0f;
        faceUnit[k] = { fr[0]*inv, fr[1]*inv, fr[2]*inv };
    }

    if (mode == 1) {
        for (size_t k = 0; k < triCount; ++k)
            for (int c = 0; c < 3; ++c) {
                V[3*k+c].normal[0] = faceUnit[k][0];
                V[3*k+c].normal[1] = faceUnit[k][1];
                V[3*k+c].normal[2] = faceUnit[k][2];
            }
        return;
    }

    // mode 2: group corners by exact source position (3 float-bit key), then for
    // each corner accumulate area-weighted face normals of same-position corners
    // whose face is within smoothDeg of this corner's face (hard-edge preserving).
    const float cosT = std::cos(smoothDeg * 3.14159265358979f / 180.0f);
    std::map<std::array<uint32_t,3>, std::vector<uint32_t>> groups;
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        std::array<uint32_t,3> kkey{};
        std::memcpy(kkey.data(), V[i].position, 12);
        groups[kkey].push_back(i);
    }
    std::vector<std::array<float,3>> out(n);
    for (auto& kv : groups) {
        const std::vector<uint32_t>& g = kv.second;
        for (uint32_t i : g) {
            const size_t ti = i / 3;
            float acc[3] = {0,0,0};
            for (uint32_t j : g) {
                const size_t tj = j / 3;
                if (dot3(faceUnit[ti].data(), faceUnit[tj].data()) >= cosT) {
                    acc[0] += faceRaw[tj][0];
                    acc[1] += faceRaw[tj][1];
                    acc[2] += faceRaw[tj][2];
                }
            }
            float m = std::sqrt(dot3(acc, acc));
            if (m > 1e-12f) { out[i] = { acc[0]/m, acc[1]/m, acc[2]/m }; }
            else            { out[i] = faceUnit[ti]; }  // degenerate fallback
        }
    }
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        V[i].normal[0] = out[i][0];
        V[i].normal[1] = out[i][1];
        V[i].normal[2] = out[i][2];
    }
}

void GpuMechBatcher::registerTypeLod(const Mech3DAppearanceType* mechType, int lod) {
    if (!mechType) return;
    const TypeLodKey key{mechType, lod};
    if (s_typeLodIndex.count(key)) return;  // idempotent
    static const bool s_mechRestoreTrace = (getenv("MC2_MECH_RESTORE_TRACE") != nullptr);
    if (s_geometryFinalized) {
        // Late registration (post-finalizeGeometry). DO NOT drop: stage
        // the type into the retained s_stagingVbo/Ibo (fall through to
        // the body below) and mark pending so finalizePending() rebuilds
        // the shared buffers. Caller MUST invoke finalizePending() once
        // its late-spawn batch completes (VPL-#11: logistics.cpp SP
        // force-group loop). Until then submitActor still fast-rejects
        // this type (s_typeLodIndex has the entry but the GL buffers
        // predate it) -- harmless because no frame renders between the
        // late registers and the finalizePending() call on that path.
        MC2_DIAGFE(stderr,
            "[MECHBATCHER v1] event=late_register type=%p lod=%d (staged_pending)\n",
            (void*)mechType, lod);
        if (s_mechRestoreTrace)
            std::fprintf(stderr,
                "[MECHRESTORE v1] event=register type=%p lod=%d result=staged_pending finalized=1\n",
                (void*)mechType, lod);
        s_pendingLateTypes = true;
        ++s_lateStagedCount;
        // fall through -- stage like a normal pre-finalize registration
    }

    TG_TypeMultiShape* typeMulti = mechType->mechShape[lod];
    if (!typeMulti) return;

    const int numNodes = typeMulti->GetNumShapes();
    if (numNodes == 0 || numNodes > 255) {
        if (numNodes > 255) {
            MC2_DIAGFE(stderr,
                "[MECHBATCHER v1] event=u8_bone_overflow type=%p lod=%d numNodes=%d\n",
                (void*)mechType, lod, numNodes);
        }
        return;
    }

    // BT2018-SKEL-GPU-PALETTE-PLACEMENT-1: an imported animated mech is ONE merged
    // node but skins over its full BT skeleton. Use its joint count as numBones and
    // its per-vertex bone index for packing. Stock mechs (importJointCount==0) are
    // byte-unchanged.
    const unsigned char* importBoneIdx = nullptr;
    int importNumVerts = 0;
    const int importJointCount =
        mc2mechanim::ImportedGpuTypeInfo((const void*)typeMulti, &importBoneIdx, &importNumVerts);
    const bool importGpu = (importJointCount > 0 && importJointCount <= 255);

    static const bool s_nodeTrace = (getenv("MC2_MECH_NODE_TRACE") != nullptr);
    if (s_nodeTrace) {
        std::fprintf(stderr, "[MECHREG v1] event=register type=%p lod=%d numBones=%d\n",
                     (void*)mechType, lod, numNodes);
    }

    const uint32_t typeLodIdx = (uint32_t)s_typeLodRecords.size();
    s_typeLodIndex[key] = typeLodIdx;
    if (s_mechRestoreTrace)
        std::fprintf(stderr,
            "[MECHRESTORE v1] event=register type=%p lod=%d result=registered finalized=0 numBones=%d\n",
            (void*)mechType, lod, numNodes);

    GpuMechTypeLodRecord rec{};
    rec.firstBoneIndex = 0;
    rec.numBones       = importGpu ? (uint32_t)importJointCount : (uint32_t)numNodes;
    rec.importedGpuType = importGpu ? (const void*)typeMulti : nullptr;
    if (importGpu)
        std::fprintf(stderr, "[MECH_IMPORT_GPU] registerTypeLod lod=%d numBones %d->%d verts=%d\n",
                     lod, numNodes, importJointCount, importNumVerts);
    rec.firstPacket    = (uint32_t)s_packets.size();
    rec.packetCount    = 0;
    rec.vertexCount    = 0;
    rec.sourceNode0    = nullptr;

    for (int nodeIdx = 0; nodeIdx < numNodes; ++nodeIdx) {
        TG_TypeNodePtr tnode = typeMulti->GetTypeNode(nodeIdx);
        if (!tnode || tnode->GetNodeType() != SHAPE_NODE) continue;
        TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(tnode);
        if (nodeIdx == 0) rec.sourceNode0 = typeShape;

        // Skip spotlight leaves: TG_Shape::isSpotlight is set on the
        // per-instance shape from "SpotLight_*" node name prefix at
        // tgl.cpp:259/475. Post-T3.1 ((E) SpotLight_ -> real illumination),
        // mech spotlight child shapes are emitted as real TG_Light
        // registrations via Mech3DAppearance::updateGeometry (T1.6/T1.7,
        // commit fceb304). Skip the geometry emission here — the cone is
        // no longer drawn, only the light contribution lives downstream.
        // See docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md.
        const char* nodeName = tnode->getNodeId();
        if (nodeName && S_strnicmp(nodeName, "SpotLight_", 10) == 0) {
            if (s_nodeTrace) {
                std::fprintf(stderr,
                    "[MECHREG v1] event=skip_spotlight type=%p lod=%d nodeIdx=%d name=%s\n",
                    (void*)mechType, lod, nodeIdx, nodeName);
            }
            // [SPOTLIGHT_REAL_TRACE v1] T0.2 (a) — record SpotLight_ TypeNode
            // observed at recipe build. Tracks max across LODs since each
            // instance only renders at one LOD; the running submitActor counter
            // (b) below cares about whether the actor has ANY spotlight nodes,
            // and lazy-registration in T1.6 walks the live mechShape regardless
            // of LOD layout.
            ++s_spotlightReal_typeNodeEvents;
            const uint32_t newCount = s_spotlightRealTypeNodeCount[mechType] + 1;
            s_spotlightRealTypeNodeCount[mechType] = newCount;
            if (!s_spotlightReal_firstHitTypeNode) {
                s_spotlightReal_firstHitTypeNode = true;
                MC2_DIAGFE(stderr,
                    "[SPOTLIGHT_REAL_TRACE v1] event=first_hit site=mech_recipe_build "
                    "type=%p lod=%d nodeIdx=%d name=%s\n",
                    (const void*)mechType, lod, nodeIdx, nodeName);
                std::fflush(stderr);
            }
            if (s_spotlightRealTrace) {
                MC2_DIAGFE(stderr,
                    "[SPOTLIGHT_REAL_TRACE v1] event=type_spotlight_node "
                    "type=%p lod=%d nodeIdx=%d name=%s type_node_count=%u\n",
                    (const void*)mechType, lod, nodeIdx, nodeName, newCount);
            }
            continue;
        }

        if (!typeShape->numTypeTriangles || !typeShape->listOfTypeTriangles ||
            !typeShape->listOfTypeVertices) continue;

        const int32_t baseVertex = (int32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex));

        // Group triangles by localTextureHandle (same as static prop batcher).
        const uint32_t numTris = typeShape->numTypeTriangles;
        uint32_t runStart = 0;
        while (runStart < numTris) {
            const DWORD runTexSlot =
                typeShape->listOfTypeTriangles[runStart].localTextureHandle;
            uint32_t runEnd = runStart;
            while (runEnd < numTris &&
                   typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTexSlot)
                ++runEnd;

            const uint32_t packetFirstIndex = (uint32_t)s_stagingIbo.size();

            for (uint32_t t = runStart; t < runEnd; ++t) {
                const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];
                const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
                const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

                // NORMALS-1: per-face UV-gradient tangent in object/local space (same frame
                // as src.normal — the vert shader transforms both identically). Encoded
                // octahedral into tangentOct for imported-GPU types ONLY; stock types keep
                // the zero-fill below (VBO byte-identical). Degenerate UV (|det|~0) or zero-
                // length tangent -> leave zero, which the frag treats as "no tangent" and
                // falls back to v_normal. No NaN can escape (all divides are guarded).
                int16_t faceTanOct[2] = { 0, 0 };
                if (importGpu) {
                    const TG_TypeVertex& p0 = typeShape->listOfTypeVertices[tri.Vertices[0]];
                    const TG_TypeVertex& p1 = typeShape->listOfTypeVertices[tri.Vertices[1]];
                    const TG_TypeVertex& p2 = typeShape->listOfTypeVertices[tri.Vertices[2]];
                    const float e1x = p1.position.x - p0.position.x;
                    const float e1y = p1.position.y - p0.position.y;
                    const float e1z = p1.position.z - p0.position.z;
                    const float e2x = p2.position.x - p0.position.x;
                    const float e2y = p2.position.y - p0.position.y;
                    const float e2z = p2.position.z - p0.position.z;
                    const float du1 = cornerU[1] - cornerU[0];
                    const float dv1 = cornerV[1] - cornerV[0];
                    const float du2 = cornerU[2] - cornerU[0];
                    const float dv2 = cornerV[2] - cornerV[0];
                    const float det = du1 * dv2 - du2 * dv1;
                    if (std::fabs(det) > 1e-8f) {
                        const float r = 1.0f / det;
                        float tx = r * (dv2 * e1x - dv1 * e2x);
                        float ty = r * (dv2 * e1y - dv1 * e2y);
                        float tz = r * (dv2 * e1z - dv1 * e2z);
                        const float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
                        if (tl > 1e-8f) {
                            tx /= tl; ty /= tl; tz /= tl;
                            // octahedral encode: unit vec3 -> vec2 in [-1,1]
                            const float invL1 = 1.0f / (std::fabs(tx) + std::fabs(ty) + std::fabs(tz));
                            float ox = tx * invL1;
                            float oy = ty * invL1;
                            if (tz < 0.0f) {
                                const float oxn = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
                                const float oyn = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
                                ox = oxn; oy = oyn;
                            }
                            auto packS = [](float v) -> int16_t {
                                float s = v * 32767.0f;
                                if (s >  32767.0f) s =  32767.0f;
                                if (s < -32767.0f) s = -32767.0f;
                                return (int16_t)std::lrintf(s);
                            };
                            faceTanOct[0] = packS(ox);
                            faceTanOct[1] = packS(oy);
                        }
                    }
                }

                for (int c = 0; c < 3; ++c) {
                    const TG_TypeVertex& src =
                        typeShape->listOfTypeVertices[tri.Vertices[c]];

                    GpuMechVertex vert{};
                    std::memcpy(vert.position, &src.position.x, 12);
                    std::memcpy(vert.normal,   &src.normal.x,   12);
                    vert.uv[0] = cornerU[c];
                    vert.uv[1] = cornerV[c];
                    // boneIndices: .x = nodeIdx (rigid, Slice A), .yzw = 0.
                    // 1B-GPU: imported mech uses its per-vertex BT bone index (one bone
                    // per vertex) into the imported joint palette instead of nodeIdx.
                    uint8_t boneSel = (uint8_t)(nodeIdx & 0xFF);
                    if (importGpu && importBoneIdx) {
                        const uint32_t gv = tri.Vertices[c];
                        if ((int)gv < importNumVerts) boneSel = importBoneIdx[gv];
                    }
                    vert.boneIndices[0] = boneSel;
                    vert.boneIndices[1] = 0;
                    vert.boneIndices[2] = 0;
                    vert.boneIndices[3] = 0;
                    // boneWeights: .x = 255 (= 1.0 normalized), .yzw = 0
                    vert.boneWeights[0] = 255;
                    vert.boneWeights[1] = 0;
                    vert.boneWeights[2] = 0;
                    vert.boneWeights[3] = 0;
                    // tangentOct: NORMALS-1 face tangent for imported types; zero for stock
                    // (no .tglgpu sidecar) -> frag falls back to v_normal.
                    vert.tangentOct[0] = faceTanOct[0];
                    vert.tangentOct[1] = faceTanOct[1];
                    vert.aRGBLight = src.aRGBLight;

                    s_stagingVbo.insert(s_stagingVbo.end(),
                        reinterpret_cast<uint8_t*>(&vert),
                        reinterpret_cast<uint8_t*>(&vert) + sizeof(GpuMechVertex));

                    // Write index LOCAL to this packet's baseVertex.
                    // glDrawElementsInstancedBaseVertex adds pkt.baseVertex at draw time,
                    // so IBO must contain (globalVertex - baseVertex), not globalVertex.
                    const uint32_t localIdx =
                        (uint32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex) - 1u)
                        - (uint32_t)baseVertex;
                    s_stagingIbo.push_back(localIdx);
                    ++rec.vertexCount;
                }
            }

            // Derive ALPHA_TEST_BIT from the texture slot's textureAlpha flag.
            uint32_t matFlags = 0;
            if (runTexSlot < (DWORD)typeShape->numTextures &&
                typeShape->listOfTextures[runTexSlot].textureAlpha) {
                matFlags = 1u;  // ALPHA_TEST_BIT (matches mech.frag ALPHA_TEST_BIT constant)
            }

            GpuMechPacket pkt{};
            pkt.firstIndex          = packetFirstIndex;
            pkt.indexCount          = (runEnd - runStart) * 3;
            pkt.baseVertex          = baseVertex;
            pkt.textureSlot         = (uint32_t)runTexSlot;
            pkt.materialFlags       = matFlags;
            pkt.owningTypeLodRecord = typeLodIdx;
            pkt.nodeLocalIndex      = (uint32_t)nodeIdx;
            pkt.owningTypeShape     = typeShape;
            s_packets.push_back(pkt);
            ++rec.packetCount;

            runStart = runEnd;
        }

        // MECH-NORMALS-FIX-1: record this node's byte range in s_stagingVbo
        // so uploadMechGeometryVbo() can apply recomputeMechNodeNormals per-node
        // on a transient copy (preserving s_stagingVbo as pristine cooked data).
        // Registration no longer mutates normals; the recompute runs at upload time.
        {
            const size_t nodeStart = (size_t)baseVertex * sizeof(GpuMechVertex);
            const size_t nodeEnd   = s_stagingVbo.size();
            s_nodeVboRanges.push_back({nodeStart, nodeEnd});
        }
    }

    s_typeLodRecords.push_back(rec);
}

// MECH-NORMALS-FIX-1: Create (or recreate) the immutable VBO from s_stagingVbo,
// applying the current s_mechNormalsMode/s_mechNormalsSmoothDeg on a transient
// copy so s_stagingVbo stays pristine (cooked normals). Also sets up all 7 vertex
// attribute pointers on the already-bound VAO (s_sharedVao must be bound by caller).
//
// VBO recreation:
//   - If s_sharedVbo already exists (rebuild path): delete it first (glBufferStorage
//     buffers are immutable — they CANNOT be resized or re-uploaded in place).
//   - Allocate a new s_sharedVbo with glGenBuffers + glBufferStorage.
//   - Re-run glVertexAttribPointer for all 7 attribs, which bind to the currently
//     bound GL_ARRAY_BUFFER. The element-array binding (s_sharedIbo) is VAO state
//     and is NOT disturbed by rebinding GL_ARRAY_BUFFER or by glVertexAttribPointer
//     calls — the VAO already has s_sharedIbo bound from finalizeGeometry, and the
//     standard guarantees that GL_ELEMENT_ARRAY_BUFFER VAO state is only changed by
//     explicit glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...) while a VAO is bound.
//     Therefore the element binding is preserved across VBO replacement and is safe
//     to skip re-binding here.
//
// Called from: finalizeGeometry() (initial) and batcher_rebuildMechNormals() (live).
static void uploadMechGeometryVbo() {
    // Build transient work copy of the staging VBO (s_stagingVbo = pristine cooked).
    // mode 0 means no recompute — work is byte-identical to cooked.
    std::vector<uint8_t> work = s_stagingVbo;

    if (s_mechNormalsMode != 0) {
        // Apply recompute per-node using the recorded per-node byte ranges.
        // Each range is [start, end) in work; recomputeMechNodeNormals modifies
        // work in place over that range only (bounded by the endByte param added
        // in MECH-NORMALS-FIX-1 so normals from adjacent nodes are never blended).
        for (const auto& rng : s_nodeVboRanges) {
            recomputeMechNodeNormals(work, rng.first, rng.second,
                                     s_mechNormalsMode, s_mechNormalsSmoothDeg);
        }
    }

    // Delete the old VBO if this is a rebuild (immutable buffer must be re-created).
    if (s_sharedVbo != 0) {
        glDeleteBuffers(1, &s_sharedVbo);
        s_sharedVbo = 0;
    }

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    (GLsizeiptr)work.size(),
                    work.data(), 0);  // immutable, GPU-only

#ifdef MC2_VULKAN_ISLAND
    // VK-RUNG8 item-9 (native feed now MANDATORY): persist the EXACT uploaded bytes
    // for the native geometry feed whenever the VK mech island is active. The old
    // GL buffer-readback fallback in the island was deleted, so this copy is the
    // island's only geometry source. Copy-once per map / normals rebuild; the copy
    // exists only under the VK backend (s_vkMechGateOn), never in the pure-GL product.
    if (s_vkMechGateOn)
        s_vkVboNativeCopy = work;
#endif

    // Vertex attribute setup — 48-byte GpuMechVertex, 7 attributes.
    // glVertexAttribPointer binds each attrib to the currently bound GL_ARRAY_BUFFER
    // (s_sharedVbo just bound above). The VAO's GL_ELEMENT_ARRAY_BUFFER binding
    // (s_sharedIbo) is a separate piece of VAO state and is not disturbed here.
    auto enableF = [](GLuint loc, GLint sz, GLenum type, GLboolean norm,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, sz, type, norm, stride, (void*)offset);
    };
    auto enableI = [](GLuint loc, GLint sz, GLenum type,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribIPointer(loc, sz, type, stride, (void*)offset);
    };

    const GLsizei S = (GLsizei)sizeof(GpuMechVertex);
    enableF(0, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, position));
    enableF(1, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, normal));
    enableF(2, 2, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, uv));
    enableI(3, 4, GL_UNSIGNED_BYTE,            S, offsetof(GpuMechVertex, boneIndices));
    enableF(4, 4, GL_UNSIGNED_BYTE,  GL_TRUE,  S, offsetof(GpuMechVertex, boneWeights));
    enableF(5, 2, GL_SHORT,          GL_TRUE,  S, offsetof(GpuMechVertex, tangentOct));
    enableI(6, 1, GL_UNSIGNED_INT,             S, offsetof(GpuMechVertex, aRGBLight));
}

void GpuMechBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;
    loadProgramsIfNeeded();
    // Slice C1: init named material profile SSBO (binding 7) once per process.
    // Idempotent; requires GL context (already active at finalizeGeometry call site).
    gos_materials::init();

    // Bail cleanly if shader failed: geometry upload skipped.
    // submit() fast-rejects on s_geometryFinalized==false.
    if (s_programLoadFailed) {
        MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=finalize_skip reason=shader_fail\n");
        return;
    }

    if (s_stagingVbo.empty()) {
        MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=finalize_empty — no types registered\n");
        s_geometryFinalized = true;
        return;
    }

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    // VBO creation + vertex attrib setup delegated to uploadMechGeometryVbo()
    // so batcher_rebuildMechNormals() can re-run just the VBO part without
    // recreating the VAO, IBO, or sampler.
    uploadMechGeometryVbo();

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    (GLsizeiptr)(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(), 0);

    glBindVertexArray(0);

    // Session-lifetime sampler: GL_REPEAT / GL_LINEAR.
    // GL_LINEAR (not GL_LINEAR_MIPMAP_LINEAR) because mech textures may not
    // have mipmaps generated — sampling a mipmap chain that doesn't exist
    // is undefined behavior on AMD, often black. Slice A+ can revisit if
    // mech textures get mipmaps from the upscaler pipeline.
    // TEX-CLASS: per-pass-rebind -- mech-skin sampler object (bound per draw)
    glGenSamplers(1, &s_sampler);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Staging RETAINED (not cleared/shrunk) so finalizePending() can
    // append a late type and rebuild from the full original+late
    // staging. See s_stagingVbo declaration. Cleared only at onMapLoad().

    s_geometryFinalized = true;
    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=finalize_ok types=%zu packets=%zu\n",
        s_typeLodRecords.size(), s_packets.size());
}

// VPL-#11 SP-logistics fix. Rebuild the immutable shared VBO/IBO/VAO so
// types registered AFTER a prior finalizeGeometry() (campaign .fit
// resume spawns the player force-group post-Mission::init-finalize, via
// logistics.cpp addMover) become drawable. No-op unless a late type was
// staged. SAFE because: (a) no frame renders between the late registers
// and this call on the SP path (adversarial-review grep-closed:
// GpuMechBatcher::flush only via renderLists from the frame loop, after
// logistics.cpp mission->update()); (b) glBufferStorage buffers are
// immutable so they MUST be deleted before recreate; (c) s_typeLodIndex
// /s_typeLodRecords/s_packets are append-only and preserved here, so the
// ~pre-finalize types keep stable indices and the retained staging holds
// their original-offset vertices + the appended late ones.
void GpuMechBatcher::finalizePending() {
    if (!s_pendingLateTypes) return;

    // VPL-#11 shadow-regression hardening (2026-05-16): finalizePending
    // runs mid-mission-setup; save EVERY GL binding it perturbs and
    // restore on exit so it is provably state-neutral to the rest of the
    // frame (vulkan_prep_explicit_device_discipline). The mech advisor
    // ruled GL-state OUT as the half-map-shadow cause (left-bound buffer
    // is overwritten before first render), but this removes it as a
    // variable definitively and is correct discipline regardless.
    GLint prevVao = 0, prevArr = 0, prevElem = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,        &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,        &prevArr);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&prevElem);

    // Drain ring fences before deleting GL objects (defensive; mirrors
    // onMapUnload). No draw can be in flight here per (a), so this is
    // belt-and-suspenders, not load-bearing.
    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    // Delete the immutable shared geometry objects finalizeGeometry()
    // glGen'd; it will recreate all four from the retained staging.
    if (s_sharedVao) { glDeleteVertexArrays(1, &s_sharedVao); s_sharedVao = 0; }
    if (s_sharedVbo) { glDeleteBuffers(1, &s_sharedVbo);      s_sharedVbo = 0; }
    if (s_sharedIbo) { glDeleteBuffers(1, &s_sharedIbo);      s_sharedIbo = 0; }
    if (s_sampler)   { glDeleteSamplers(1, &s_sampler);       s_sampler   = 0; }

    const uint32_t lateAdded = s_lateStagedCount;
    s_lateStagedCount   = 0;
    s_pendingLateTypes  = false;
    s_geometryFinalized = false;   // re-open the guard so finalizeGeometry runs
    finalizeGeometry();            // rebuilds VBO/IBO/VAO from full retained staging

    // The rebuild above repopulates s_packets with new indices/order, so
    // the persisted shadow draw list (pre-rebuild globalPacketIdx values)
    // is now stale. Clear it (mirrors onMapLoad); the next flush() will
    // repopulate it consistently with the rebuilt s_packets. flushShadow()
    // early-returns on empty s_lastDrawCalls until then.
    s_lastDrawCalls.clear();
    s_lastTotalInstances = 0;
    s_lastTotalBones     = 0;

    // Restore the bindings finalizeGeometry left dirty (it ends on
    // glBindVertexArray(0) with GL_ARRAY_BUFFER still = the new s_sharedVbo).
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER,         (GLuint)prevArr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElem);

    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=finalize_pending types=%zu lateAdded=%u rebuilt=%d\n",
        s_typeLodRecords.size(), lateAdded, s_geometryFinalized ? 1 : 0);
    if (getenv("MC2_MECH_RESTORE_TRACE") != nullptr)
        std::fprintf(stderr,
            "[MECHRESTORE v1] event=finalize_pending types=%zu lateAdded=%u rebuilt=%d\n",
            s_typeLodRecords.size(), lateAdded, s_geometryFinalized ? 1 : 0);
}

// ---------------------------------------------------------------------------
// submitActor (Task 6) — bone staging + texture capture
// ---------------------------------------------------------------------------
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc& desc) {
    s_lastFailWasLateReg = false;

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed) return false;
    if (!desc.mechShape || !desc.mechType) return false;

    const TypeLodKey key{desc.mechType, desc.currentLOD};
    auto it = s_typeLodIndex.find(key);
    if (it == s_typeLodIndex.end()) {
        s_lastFailWasLateReg = true;
        ++s_disallowedLateRegEvents;
        return false;
    }
    const uint32_t typeLodIdx = it->second;
    const GpuMechTypeLodRecord& rec = s_typeLodRecords[typeLodIdx];

    PendingSubmit ps;
    ps.desc       = desc;
    ps.typeLodIdx = typeLodIdx;
    ps.bones.reserve(rec.numBones);

    // Stage bone matrices from live shapeToWorld (set by TransformMultiShape).
    // listOfShapes[i].shapeToWorld is a Stuff::LinearMatrix4D with entries[12]
    // stored column-major: entries[(col<<2)+row], 3 explicit cols + implicit col3=[0,0,0,1].
    // Row k extraction: [entries[k], entries[4+k], entries[8+k], w] where w=1 for row3 only.
    // BT2018-SKEL-GPU-PALETTE-PLACEMENT-1: imported animated mech sources its bone
    // palette from boneT_i = shapeToWorld_root · model_delta_i. shapeToWorld_root is
    // the single merged node's live transform (placement, set by _HierarchyOnly);
    // model_delta_i (mc2mechanim, refreshed each frame) maps the Y-up rest VBO vertex
    // to the Z-up animated MODEL pose. M_sw (row-major) = entries[0..11] + (0,0,0,1).
    if (rec.importedGpuType) {
        const float* md = nullptr;
        // Per-actor palette + lift, keyed by this actor's instance shape (mechShape).
        const int mdCount = mc2mechanim::ImportedGpuModelDelta((const void*)desc.mechShape, &md);
        const float lift = mc2mechanim::ImportedGpuLift((const void*)desc.mechShape);
        const int   liftAxis = mc2mechanim::ImportedGpuLiftAxis();   // 0/1/2 = Stuff.x/y/z
        const int   liftIdx = 3 + 4 * (liftAxis & 3);                // F[3]/F[7]/F[11]
        float Msw[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (desc.mechShape->GetNumShapes() > 0) {
            const float* e = (const float*)desc.mechShape->listOfShapes[0].shapeToWorld.entries;
            for (int k = 0; k < 12; ++k) Msw[k] = e[k];
        }
        for (int b = 0; b < mdCount && b < (int)rec.numBones && md; ++b) {
            const float* D = md + (size_t)b * 16;
            float F[16];                                   // F = Msw * D (row-major)
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    F[r*4+c] = Msw[r*4+0]*D[0*4+c] + Msw[r*4+1]*D[1*4+c]
                             + Msw[r*4+2]*D[2*4+c] + Msw[r*4+3]*D[3*4+c];
            F[liftIdx] += lift;                            // foot-ground lift (world-up axis tunable)
            GpuMechBone bone;                              // pack columns of F
            bone.row0[0]=F[0]; bone.row0[1]=F[4]; bone.row0[2]=F[ 8]; bone.row0[3]=0.0f;
            bone.row1[0]=F[1]; bone.row1[1]=F[5]; bone.row1[2]=F[ 9]; bone.row1[3]=0.0f;
            bone.row2[0]=F[2]; bone.row2[1]=F[6]; bone.row2[2]=F[10]; bone.row2[3]=0.0f;
            bone.row3[0]=F[3]; bone.row3[1]=F[7]; bone.row3[2]=F[11]; bone.row3[3]=1.0f;
            ps.bones.push_back(bone);
        }
    } else {
        const int numShapes = desc.mechShape->GetNumShapes();
        for (int i = 0; i < numShapes && i < (int)rec.numBones; ++i) {
            const TG_ShapeRec& sr = desc.mechShape->listOfShapes[i];
            const float* e = (const float*)sr.shapeToWorld.entries;
            GpuMechBone bone;
            bone.row0[0]=e[0]; bone.row0[1]=e[4]; bone.row0[2]=e[ 8]; bone.row0[3]=0.0f;
            bone.row1[0]=e[1]; bone.row1[1]=e[5]; bone.row1[2]=e[ 9]; bone.row1[3]=0.0f;
            bone.row2[0]=e[2]; bone.row2[1]=e[6]; bone.row2[2]=e[10]; bone.row2[3]=0.0f;
            bone.row3[0]=e[3]; bone.row3[1]=e[7]; bone.row3[2]=e[11]; bone.row3[3]=1.0f;
            ps.bones.push_back(bone);
        }
    }
    while ((int)ps.bones.size() < (int)rec.numBones) {
        GpuMechBone id{};
        id.row0[0]=1.f; id.row1[1]=1.f; id.row2[2]=1.f; id.row3[3]=1.f;
        ps.bones.push_back(id);
    }

    // Capture live per-actor texture handle for each packet.
    //
    // SLOT 0 is per-actor (paint scheme / team color). TG_TypeShape::listOfTextures
    // is a shared type-level cache mutated by TransformMultiShape — by render time
    // it reflects the LAST actor through, not the current one. Use desc.slot0TexHandle
    // (the raw gos handle passed by the caller) directly for slot 0.
    //
    // SLOTS 1+ are type-stable; reading from owningTypeShape is correct.
    ps.packetTexHandles.resize(rec.packetCount, 0);
    for (uint32_t p = 0; p < rec.packetCount; ++p) {
        const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
        if (pkt.textureSlot == 0) {
            ps.packetTexHandles[p] = desc.slot0TexHandle;
        } else if (pkt.owningTypeShape && pkt.owningTypeShape->listOfTextures &&
                   pkt.textureSlot < (uint32_t)pkt.owningTypeShape->numTextures) {
            ps.packetTexHandles[p] =
                pkt.owningTypeShape->listOfTextures[pkt.textureSlot].gosTextureHandle;
        }
    }

    s_pendingSubmits.push_back(std::move(ps));

    // [SPOTLIGHT_REAL_TRACE v1] T0.2 (b) — per-instance counter. Fires once
    // per submitActor when the actor's mechType has at least one SpotLight_
    // TypeNode (recorded at registerTypeLod time). Per-frame increment is
    // unconditional; 600-frame summary is env-gated in flush() below.
    if (s_spotlightRealTypeNodeCount.find(desc.mechType)
            != s_spotlightRealTypeNodeCount.end()) {
        ++s_spotlightReal_drawWindow;
        ++s_spotlightReal_drawMono;
        if (!s_spotlightReal_firstHitDraw) {
            s_spotlightReal_firstHitDraw = true;
            MC2_DIAGFE(stderr,
                "[SPOTLIGHT_REAL_TRACE v1] event=first_hit site=mech_submit "
                "type=%p lod=%d\n",
                (const void*)desc.mechType, desc.currentLOD);
            std::fflush(stderr);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// flush (Task 7) — bucket-sorted compaction + draw loop
// ---------------------------------------------------------------------------
// APPLY-STATE-MECHOPAQUE-1: executor-driven pre-apply of the MechOpaque pipeline.
// Called from the flush call site in txmmgr renderLists() when MC2_FRAMEGRAPH_EXECUTOR
// is ON and a kTopLevelStateDesc row exists. Applies ONLY the pipeline (program +
// fixed-function depth/blend/cull); FBO/MRT/objectId(loc2)/viewport are inherited from
// the preceding passes and are NOT touched here (kTopLevelStateDesc records them as
// Unknown/Inherit). MVP is a view-UBO uniform (binding=3), not pipeline state -> pin-safe.
// Idempotent: flush()'s body makes the identical call when the flag is false.
void GpuMechBatcher::executorApplyMechOpaqueState() {
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::MechOpaque),
        "MechOpaque");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::MechOpaque);
    s_opaqueStateAppliedByExecutor = true;
}

void GpuMechBatcher::flush() {
    finalizePending();

    // V1A: latch BEFORE any early-return guard clears s_pendingSubmits.
    // Counts GpuMechBatcher submits only (not MLR fallback, not alive count).
    s_lastFlushSubmitCount = static_cast<uint64_t>(s_pendingSubmits.size());

    // [SPOTLIGHT_REAL_TRACE v1] T0.2 (b) — periodic summary at 600-frame
    // cadence. Frame counter advances every flush() regardless of env or
    // pending submits, so the cadence stays stable across missions. Window
    // counter resets after emit; mono is monotonic.
    ++s_spotlightReal_drawFrameCount;
    const bool spotlightRealPeriodic =
        (s_spotlightReal_drawFrameCount % 600 == 0 && s_spotlightReal_drawFrameCount > 0);
    if (s_spotlightRealTrace && spotlightRealPeriodic) {
        MC2_DIAGFE(stderr,
            "[SPOTLIGHT_REAL_TRACE v1] event=summary site=mech_submit "
            "frames=%llu window_mech_spotlight_draws=%llu mono_mech_spotlight_draws=%llu "
            "type_node_events_total=%llu distinct_types_with_spotlights=%zu\n",
            (unsigned long long)s_spotlightReal_drawFrameCount,
            (unsigned long long)s_spotlightReal_drawWindow,
            (unsigned long long)s_spotlightReal_drawMono,
            (unsigned long long)s_spotlightReal_typeNodeEvents,
            s_spotlightRealTypeNodeCount.size());
        std::fflush(stderr);
    }
    if (spotlightRealPeriodic) {
        s_spotlightReal_drawWindow = 0;
    }

    if (!s_mechBatcherTraceInit) {
        s_mechBatcherTrace     = (getenv("MC2_MECH_BATCHER_STATS") != nullptr);
        s_mechBatcherTraceInit = true;
    }
    if (!s_mechLightTraceInit) {
        s_mechLightTrace     = (getenv("MC2_MECH_LIGHT_TRACE") != nullptr);
        s_mechLightTraceInit = true;
    }
    if (s_mechLightTrace) {
        // [LIGHTSSBO v1] RF4: LightsData is an unbounded SSBO now — there
        // is NO 64-slot cap, so the old event=cache_full overflow alarm
        // would false-fire on exactly the dense missions this conversion
        // enables. Repurposed to a pure maxIdx observation (no cap, no
        // disable). s_lightCacheFullFrames retired.
        uint32_t maxIdx = 0;
        for (const auto& ps : s_pendingSubmits) {
            if (ps.desc.lightDataIndex > maxIdx) maxIdx = ps.desc.lightDataIndex;
        }
        std::fprintf(stderr,
            "[LIGHTSSBO v1] event=mech_lightidx_max submitted=%zu maxIdx=%u (unbounded SSBO)\n",
            s_pendingSubmits.size(), maxIdx);
    }

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed ||
        s_pendingSubmits.empty()) {
        s_pendingSubmits.clear();
        s_eligibleActorsThisFrame = 0;
        std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
        // MECH-EXTRACTION-0: persist buffer is retained for frames with no pending submits;
        // it will be cleared in onMapUnload() or repopulated in the normal flush() path.
        return;
    }

    // Slice C1: resolve s_mechSurfaceMaterialIdx once after gos_materials::init() has run.
    // init() is called from finalizeGeometry() which always precedes the first flush().
    if (!s_mechSurfaceMaterialIdxResolved) {
        s_mechSurfaceMaterialIdxResolved = true;
        const char* v = std::getenv("MC2_MECH_SURFACE_MATERIAL");
        if (v && v[0] != '\0' && strcmp(v, "none") != 0 && strcmp(v, "0") != 0)
            s_mechSurfaceMaterialIdx = gos_materials::getProfileIndex(v);
        // else stays 0 (default passthrough)
        if (s_mechSurfaceMaterialIdx != 0u)
            s_mechPaintSurfaceMaterialIdx = gos_materials::getProfileIndex("paintedmetal003");
    }

    // Step 1: Count total bones (one block per actor).
    size_t totalBones = 0;
    for (const auto& ps : s_pendingSubmits) totalBones += ps.bones.size();

    // Step 2: Build draw buckets.
    // Key: (typeLodIdx, globalPacketIdx, texHandle, materialFlags).
    // Each actor × packet produces one entry in the matching bucket.
    // Different per-actor paint schemes for the same packet -> different buckets.
    struct BucketKey {
        uint32_t typeLodIdx;
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        bool operator<(const BucketKey& o) const {
            if (typeLodIdx      != o.typeLodIdx)      return typeLodIdx      < o.typeLodIdx;
            if (globalPacketIdx != o.globalPacketIdx) return globalPacketIdx < o.globalPacketIdx;
            if (texHandle       != o.texHandle)       return texHandle       < o.texHandle;
            return materialFlags < o.materialFlags;
        }
    };

    std::map<BucketKey, std::vector<uint32_t>> buckets;  // key -> [submitIdx list]

    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        const PendingSubmit& ps = s_pendingSubmits[si];
        const GpuMechTypeLodRecord& rec = s_typeLodRecords[ps.typeLodIdx];
        for (uint32_t p = 0; p < rec.packetCount; ++p) {
            const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
            BucketKey key;
            key.typeLodIdx       = ps.typeLodIdx;
            key.globalPacketIdx  = rec.firstPacket + p;
            key.texHandle        = ps.packetTexHandles[p];
            key.materialFlags    = pkt.materialFlags;
            buckets[key].push_back(si);
        }
    }

    size_t totalInstances = 0;
    for (const auto& kv : buckets) totalInstances += kv.second.size();

    ensureRingCapacity(totalInstances, totalBones);
    if (!s_instanceMap || !s_boneMap) {
        s_pendingSubmits.clear();
        return;
    }

    // Step 3: Advance ring slot and wait for oldest fence.
    s_frameSlot = (s_frameSlot + 1) % MECH_RING_FRAMES;
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    GpuMechInstance* instDst = (GpuMechInstance*)s_instanceMap + s_frameSlot * s_instanceCapacity;
    GpuMechBone*     boneDst = (GpuMechBone*)    s_boneMap     + s_frameSlot * s_boneCapacity;

    // Step 4: Write bone SSBO once per actor; record each actor's boneBase offset.
    std::vector<uint32_t> actorBoneBase(s_pendingSubmits.size());
    uint32_t boneHead = 0;
    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        actorBoneBase[si] = boneHead;
        for (const auto& b : s_pendingSubmits[si].bones)
            boneDst[boneHead++] = b;
    }

    // Step 2.5: Build mech MaterialGpu table (same bucket iteration order as Step 5).
    // s_mechDrawMaterialIdx is per-draw (bucket-iteration order) — rebuilt every flush.
    // s_mechMaterialTable / s_mechHandleToMaterialIdx PERSIST within a mission
    // (texHandle->idx is stable; reset by onMapUnload). Upload only when the table grows.
    s_mechDrawMaterialIdx.clear();
    if (s_mechMaterialGpuEnabled) {
        if (s_mechMaterialTable.empty())
            s_mechMaterialTable.push_back(RenderCore::MaterialGpu{}); // index 0 = sentinel/not-assigned
        bool tableDirty = false;
        for (const auto& kv : buckets) {
            const uint32_t texHandle = kv.first.texHandle;
            uint32_t mIdx;
            auto it = s_mechHandleToMaterialIdx.find(texHandle);
            if (it != s_mechHandleToMaterialIdx.end()) {
                mIdx = it->second;
            } else {
                mIdx = static_cast<uint32_t>(s_mechMaterialTable.size());
                RenderCore::MaterialGpu entry{};
                entry.albedoTex = texHandle;
                s_mechMaterialTable.push_back(entry);
                s_mechHandleToMaterialIdx[texHandle] = mIdx;
                tableDirty = true;
            }
            s_mechDrawMaterialIdx.push_back(mIdx);
        }
        // Upload only when the table grew (or buffer not yet created).
        if (s_mechMaterialSsbo.backendHandle == 0 || tableDirty) {
            const size_t byteSize = s_mechMaterialTable.size() * sizeof(RenderCore::MaterialGpu);
            if (s_mechMaterialSsbo.backendHandle == 0) {
                GLuint local = 0;
                glGenBuffers(1, &local);
                s_mechMaterialSsbo.backendHandle = static_cast<uint32_t>(local);
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_mechMaterialSsbo.backendHandle));
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(byteSize),
                         s_mechMaterialTable.data(), GL_DYNAMIC_DRAW);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            // REGISTRY-MATERIAL-SSBO-1: register the mech material SSBO (observe-only
            // metadata; never read by the draw path). Registered here (not at init)
            // because both the live GL handle and the byte size are only known once
            // the material table has been sized and uploaded.
            {
                RenderCore::RenderResourceDesc d;
                d.id        = RenderCore::RenderResourceId::MaterialGpuBuffer;
                d.kind      = RenderCore::RenderResourceKind::Buffer;
                d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // persistent material table (not a pass output)
                d.format    = RenderCore::RenderResourceFormat::BufferRaw;
                d.debugName = "MaterialGpuBuffer";
                d.glName    = static_cast<GLuint>(s_mechMaterialSsbo.backendHandle);
                d.sizeBytes = static_cast<uint64_t>(byteSize);
                d.valid     = true;
                RenderCore::registerOrUpdateRenderResource(d);
            }
        }
    }
    // Throttled table_link log (first flush + every 600th)
    ++s_mechMaterialGpuFrameCount;
    if (s_mechMaterialGpuFrameCount % 600 == 1) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "[MECH_MATERIAL_GPU v1] event=table_link materials=%u\n",
            static_cast<uint32_t>(s_mechMaterialTable.size()));
        MC2_DIAG_FPUTS(buf, stderr);
    }

    // Step 5: Write instance SSBO in bucket order; collect draw calls.
    struct DrawCall {
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        uint32_t instanceBase;
        uint32_t instanceCount;
        uint32_t aoTexHandle;   // AO-1: AO mcTextureManager slot (0=none; per-type, so
                                // consistent within a bucket). Bound to unit 6 at draw.
        uint32_t normalTexHandle; // NORMALS-1: normal-map slot (0=none; per-type).
                                  // Bound to unit 7 at draw.
    };
    std::vector<DrawCall> drawCalls;
    drawCalls.reserve(buckets.size());

    auto unpack = [](uint32_t argb, float out[4]) {
        out[0] = ((argb >> 16) & 0xFF) / 255.f;  // r
        out[1] = ((argb >>  8) & 0xFF) / 255.f;  // g
        out[2] = ((argb >>  0) & 0xFF) / 255.f;  // b
        out[3] = ((argb >> 24) & 0xFF) / 255.f;  // a
    };

    uint32_t instHead = 0;
    uint32_t dcIdx    = 0;
    for (const auto& kv : buckets) {
        const BucketKey& key              = kv.first;
        const std::vector<uint32_t>& subs = kv.second;

        DrawCall dc;
        dc.globalPacketIdx = key.globalPacketIdx;
        dc.texHandle       = key.texHandle;
        dc.materialFlags   = key.materialFlags;
        dc.instanceBase    = instHead;
        dc.instanceCount   = (uint32_t)subs.size();
        // AO-1: AO is per-type, so all submits in this bucket share it; read from the first.
        dc.aoTexHandle     = subs.empty() ? 0u : s_pendingSubmits[subs[0]].desc.slot6TexHandle;
        // NORMALS-1: normal map is per-type too; same bucket-share rule.
        dc.normalTexHandle = subs.empty() ? 0u : s_pendingSubmits[subs[0]].desc.slot7TexHandle;

        for (uint32_t si : subs) {
            const PendingSubmit& ps   = s_pendingSubmits[si];
            const GpuMechSubmitDesc& d = ps.desc;
            GpuMechInstance inst{};
            inst.typeLodRecordIndex = ps.typeLodIdx;
            inst.baseBoneOffset     = actorBoneBase[si];
            inst.lightDataIndex     = d.lightDataIndex;
            inst.renderFlags        = d.renderFlags;
            // BT2018-MECH-MATERIAL-GAMMA-1: tag imported BT mechs (renderFlags bit 3)
            // so the frag linearizes their sRGB-authored albedo. Stock types have
            // importedGpuType == nullptr, so the bit stays clear (no-op for them).
            if (ps.typeLodIdx < s_typeLodRecords.size() &&
                s_typeLodRecords[ps.typeLodIdx].importedGpuType)
                inst.renderFlags |= 0x8u;
            unpack(d.highlightARGB, inst.aRGBHighlight);
            unpack(d.fogARGB,       inst.fogRGB);
            // M2.5 (Q3 unconditional): carry the RenderWorld handle through
            // to the SSBO. Env-OFF: GLSL macro gates out the FS write, so
            // the value is never read by the GPU.
            inst.objectIdRaw        = d.objectIdRaw;
            // M2.5 (Q4): count non-zero writes for per-mission observability.
            // Direct per-mission accumulation (no per-frame intermediate;
            // per adversarial M1 the per-frame counter had no consumer).
            if (d.objectIdRaw != 0u) {
                ++s_gpuMechIdWritesThisMission;
            }
            // Mech-1: per-instance materialIdx from parallel s_mechDrawMaterialIdx table
            inst.materialIdx = (s_mechMaterialGpuEnabled && dcIdx < s_mechDrawMaterialIdx.size())
                ? s_mechDrawMaterialIdx[dcIdx]
                : 0u;
            // Slice C1: when a named surface material profile is selected via
            // MC2_MECH_SURFACE_MATERIAL, override materialIdx with the binding-7
            // profile index. 0 = env unset/none → preserve existing albedo-table value.
            if (s_mechSurfaceMaterialIdx != 0u)
                inst.materialIdx = s_mechSurfaceMaterialIdx;
            // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: per-mech visual state into the
            // SSBO record. No shader reads these in Slice 1.
            inst.visualDamage01     = d.damage01;
            inst.visualFlags        = d.visualFlags;
            instDst[instHead++]     = inst;
        }
        drawCalls.push_back(dc);
        ++dcIdx;
    }

    // Task 5: Compare validation — verify albedoTex == texHandle per draw call
    if (s_mechMaterialGpuEnabled) {
        int mismatches = 0;
        for (uint32_t i = 0; i < (uint32_t)drawCalls.size(); ++i) {
            const uint32_t mIdx = (i < (uint32_t)s_mechDrawMaterialIdx.size()) ? s_mechDrawMaterialIdx[i] : 0u;
            if (mIdx == 0u || mIdx >= (uint32_t)s_mechMaterialTable.size()) continue;
            const uint32_t albedo   = s_mechMaterialTable[mIdx].albedoTex;
            const uint32_t expected = drawCalls[i].texHandle;
            if (albedo != expected) {
                if (mismatches < 10) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                        "[MECH_MATERIAL_GPU v1] MISMATCH dc=%u materialIdx=%u albedo=%u expected=%u\n",
                        i, mIdx, albedo, expected);
                    MC2_DIAG_FPUTS(buf, stderr);
                }
                ++mismatches;
            }
        }
        if (s_mechMaterialGpuFrameCount % 600 == 1) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "[MECH_MATERIAL_GPU v1] event=compare frame=%u mechs=%u mismatches=%d\n",
                s_mechMaterialGpuFrameCount,
                static_cast<uint32_t>(drawCalls.size()),
                mismatches);
            MC2_DIAG_FPUTS(buf, stderr);
        }
    }

    // Mech-1: save binding 2 before Step 6 mutates it.
    // GLSTATE-GUARD-ADOPTION-3 (glstate-guard-epilogue-recon-1.md Site 1 — "the
    // one genuinely clean, behavior-preserving win"): restore-previous SSBO
    // binding via GlScopedSsboBinding, replacing the hand-rolled prevSsbo2
    // save (was here) + restore (was after the draw loop). The recon requires
    // this guard to close BEFORE the function's trailing
    // gos_InvalidateRenderStateCache() call, which is earlier than this
    // function's natural end — so it is wrapped in std::optional and
    // .reset() explicitly at the original manual-restore point instead of a
    // ~430-line lexical block (avoids re-scoping every local declared in
    // between). Render-state (program/VAO/buffers/samplers/depth/blend/cull)
    // stays fully manual per the recon's recommendation — only SSBO 0/1/2
    // convert in this slice.
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo2;
    guardSsbo2.emplace(2);

    // Step 6: Bind SSBOs (whole per-frame slices; shader indexes via u_instanceBase).
    gpuBindSsboRange(0, s_instanceSsbo,
        (long long)(s_frameSlot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (long long)(totalInstances * sizeof(GpuMechInstance)),
        "mech.instance.shadow");
    gpuBindSsboRange(1, s_boneSsbo,
        (long long)(s_frameSlot * s_boneCapacity * sizeof(GpuMechBone)),
        (long long)(totalBones * sizeof(GpuMechBone)),
        "mech.bone.shadow");
    // Mech-1: bind MaterialGpu table SSBO at binding 2
    if (s_mechMaterialGpuEnabled && s_mechMaterialSsbo.backendHandle != 0) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, static_cast<GLuint>(s_mechMaterialSsbo.backendHandle));
    }
    // Slice C1: bind named material profile SSBO at binding 7.
    // No-op when MC2_MATERIAL_GPU=0, init() not called, or no textured profiles.
    gos_materials::bindMaterialTable();

    // Save prior GL state. The mech flush bypasses applyRenderStates'
    // tracked slot set, so the engine's render-state cache becomes
    // out-of-sync after we mutate state directly. We save EVERYTHING the
    // static_prop batcher saves (mirror its pattern), restore at end, and
    // call gos_InvalidateRenderStateCache() to force the next
    // applyRenderStates to re-issue. Skipping invalidate has been observed
    // to make subsequent draws (water, mechs themselves on later frames)
    // disappear after a few frames as the cache thinks GL is in state X
    // while reality is state Y.
    GLint     prevDepthFunc;   glGetIntegerv(GL_DEPTH_FUNC,      &prevDepthFunc);
    GLboolean prevDepthTest;   glGetBooleanv(GL_DEPTH_TEST,      &prevDepthTest);
    GLboolean prevDepthMask;   glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevBlend;       glGetBooleanv(GL_BLEND,            &prevBlend);
    GLboolean prevCull;        glGetBooleanv(GL_CULL_FACE,        &prevCull);
    GLint     prevCullMode;    glGetIntegerv(GL_CULL_FACE_MODE,  &prevCullMode);
    GLint     prevProgram;     glGetIntegerv(GL_CURRENT_PROGRAM,  &prevProgram);
    GLint     prevVao;         glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLint     prevArrayBuf;    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    GLint     prevElemBuf;     glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    GLint     prevActiveTex;   glGetIntegerv(GL_ACTIVE_TEXTURE,   &prevActiveTex);
    GLint     prevSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &prevSampler);
    // GLSTATE-GUARD-ADOPTION-3: SSBO 0/1 restore-previous, same pattern as
    // guardSsbo2 above (captured here, AFTER Step 6 already rebound slots 0/1
    // to s_instanceSsbo/s_boneSsbo — matching the exact pre-conversion capture
    // point/semantics unchanged, not "fixed").
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo0;
    guardSsbo0.emplace(0);
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo1;
    guardSsbo1.emplace(1);
    glActiveTexture(GL_TEXTURE0);
    GLint     prevTexUnit0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit0);
    // Slice C2 + PBR-LAYERED-1: save PBR detail sampler units 1-4.
    glActiveTexture(GL_TEXTURE1);
    GLint prevTexUnit1 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit1);
    GLint prevSampler1 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &prevSampler1);
    glActiveTexture(GL_TEXTURE2);
    GLint prevTexUnit2 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit2);
    GLint prevSampler2 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 2, &prevSampler2);
    glActiveTexture(GL_TEXTURE3);
    GLint prevTexUnit3 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit3);
    GLint prevSampler3 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &prevSampler3);
    glActiveTexture(GL_TEXTURE4);
    GLint prevTexUnit4 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit4);
    GLint prevSampler4 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 4, &prevSampler4);
    // AO-1: save unit 6 (imported-mech AO). Paranoid leak guard — restored below.
    glActiveTexture(GL_TEXTURE6);
    GLint prevTexUnit6 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit6);
    GLint prevSampler6 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 6, &prevSampler6);
    // NORMALS-1: save unit 7 (imported-mech normal map). Same leak guard.
    glActiveTexture(GL_TEXTURE7);
    GLint prevTexUnit7 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit7);
    GLint prevSampler7 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 7, &prevSampler7);
    glActiveTexture(GL_TEXTURE0);
    glActiveTexture(GL_TEXTURE0);

    // MECH-PIPELINEDESC-1: program + fixed-function state (depth test+write+func
    // GEQUAL, blend Opaque, cull Back) from the registered MechOpaque PipelineDesc,
    // replacing the prior hand-set glUseProgram/glEnable/glDepthFunc/... block.
    // applyPipeline also does glUseProgram(desc.glProgramName) (= s_mechProgram via
    // bindProgram at link). Sampler + VAO/IBO stay manual (not PipelineDesc fields).
    // APPLY-STATE-MECHOPAQUE-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Gate OFF -> flag false ->
    // body applies -> byte-identical. flushShadow() (separate Shadow pass) is untouched.
    if (!s_opaqueStateAppliedByExecutor) {
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::MechOpaque));
    }
    s_opaqueStateAppliedByExecutor = false;

    glBindVertexArray(s_sharedVao);
    // Explicit IBO rebind: mirrors flushShadow() pattern. GL_ELEMENT_ARRAY_BUFFER
    // is VAO state; s_sharedVao's slot can be left at 0 by a prior flushShadow()
    // restore-order bug. Belt-and-suspenders self-heal.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Bind our REPEAT/LINEAR sampler. Per
    // memory/sampler_state_inheritance_in_fast_paths.md: when a prior pass
    // (e.g. patch_stream) leaves a CLAMP_TO_EDGE sampler bound on unit 0,
    // mech UVs that fall outside [0,1] (legitimate for tiled mech body
    // textures) all clamp to the texture edge — which for many mech
    // textures is a black border, producing the all-black mech symptom.
    // Sampler-object state OVERRIDES the texture object's glTexParameter
    // values, so setting them on the texture object alone is insufficient.
    // Identified via debug=7 (hardcoded UV (0.5, 0.5) showed yellow paint
    // while debug=2 with v_uv showed black).
    glBindSampler(0, s_sampler);

    // Slice C2: bind PBR surface-detail textures to units 1 and 2.
    // s_mechSurfaceMaterialIdx 0 → both getters return 0 → unbind (safe; shader
    // only samples these when u_standardLitEnabled != 0, which requires a non-zero profile).
    {
        static const float s_pbrTileScale = []() {
            const char* v = std::getenv("MC2_PBR_TILE_SCALE");
            if (v) return (float)std::atof(v);
            const char* mat = std::getenv("MC2_MECH_SURFACE_MATERIAL");
            return (mat && strcmp(mat, "painted_subtle") == 0) ? 2.0f : 4.0f;
        }();
        const GLuint pbrNormal = gos_materials::getProfileNormalTex(s_mechSurfaceMaterialIdx);
        const GLuint pbrOrm    = gos_materials::getProfileOrmTex(s_mechSurfaceMaterialIdx);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, pbrNormal);
        glBindSampler(1, s_sampler);   // REPEAT/LINEAR same as unit 0
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, pbrOrm);
        glBindSampler(2, s_sampler);
        glActiveTexture(GL_TEXTURE0);
        if (s_loc_u_pbrNormalTex >= 0) glUniform1i(s_loc_u_pbrNormalTex, 1);
        if (s_loc_u_pbrOrmTex    >= 0) glUniform1i(s_loc_u_pbrOrmTex,    2);
        if (s_loc_u_pbrTileScale >= 0) glUniform1f(s_loc_u_pbrTileScale, s_pbrTileScale);
    }
    // PBR-LAYERED-1: bind PaintedMetal003 paint layer to units 3 (normal) and 4 (ORM).
    // Falls back to Metal061B textures when paint profile not registered (idx=0, id=0).
    {
        GLuint pbrPaintNormal = gos_materials::getProfileNormalTex(s_mechPaintSurfaceMaterialIdx);
        if (pbrPaintNormal == 0u) pbrPaintNormal = gos_materials::getProfileNormalTex(s_mechSurfaceMaterialIdx);
        GLuint pbrPaintOrm = gos_materials::getProfileOrmTex(s_mechPaintSurfaceMaterialIdx);
        if (pbrPaintOrm == 0u) pbrPaintOrm = gos_materials::getProfileOrmTex(s_mechSurfaceMaterialIdx);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, pbrPaintNormal);
        glBindSampler(3, s_sampler);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, pbrPaintOrm);
        glBindSampler(4, s_sampler);
        glActiveTexture(GL_TEXTURE0);
        if (s_loc_u_pbrPaintNormalTex >= 0) glUniform1i(s_loc_u_pbrPaintNormalTex, 3);
        if (s_loc_u_pbrPaintOrmTex    >= 0) glUniform1i(s_loc_u_pbrPaintOrmTex,    4);
    }

    // VK-PARITY-MECH-1: default shading knobs from the shared producer (the VK
    // mech island's frame feed reads the same function). Values unchanged.
    GpuMechShadingParams shp{};
    gpuMechShadingParams(&shp);

    // Static uniforms.
    glUniform1i(s_loc_u_tex,      0);
    glUniform1f(s_loc_u_fogValue, 1.0f);
    // BT2018-MECH-MATERIAL-GAMMA-1/TUNING-1: imported-mech albedo knobs (live ImGui).
    if (s_loc_u_importedGamma       >= 0) glUniform1f(s_loc_u_importedGamma,       shp.importedGamma);
    if (s_loc_u_importedAlbedoScale >= 0) glUniform1f(s_loc_u_importedAlbedoScale, shp.importedAlbedoScale);
    if (s_loc_u_mechAoTex           >= 0) glUniform1i(s_loc_u_mechAoTex,           6);  // AO-1: unit 6
    if (s_loc_u_aoStrength          >= 0) glUniform1f(s_loc_u_aoStrength,          shp.aoStrength);
    if (s_loc_u_mechNormalTex       >= 0) glUniform1i(s_loc_u_mechNormalTex,       7);  // NORMALS-1: unit 7
    if (s_loc_u_normalStrength      >= 0) glUniform1f(s_loc_u_normalStrength,      shp.normalStrength);
    // Slice B1: lighting mode 0 = Slice A flat-white passthrough,
    // 1 = calc_light() per-vertex. Set per-flush from killswitch.
    if (s_loc_u_lightingMode >= 0)
        glUniform1i(s_loc_u_lightingMode, 1);   // LIGHTING-RETIRE-1: calc_light always on
    // Slice C2: skinning mode 0 = rigid per-bone (Slice A), 1 = weighted
    // multi-bone blend. Stock data is byte-identical across modes.
    if (s_loc_u_skinningMode >= 0)
        glUniform1i(s_loc_u_skinningMode, 1);   // SKIN-RETIRE-1: weighted skinning always on
    // SUN-AXIS-FIX-1: hand calc_light's DIFFUSE directional branches the same
    // Stuff->MC2 swap mech.vert:279 already applies for SPECULAR, so mech
    // diffuse stops dotting an MC2-frame normal against a Stuff-frame sun.
    // Boot-cached (Debug Instrumentation Rule). Default OFF -> uniform 0 ->
    // shipped expression bit-for-bit. See RendererFeatureRegistry.h.
    if (s_loc_u_sunAxisMc2Fix >= 0) {
        // Canonical DEFAULT-OFF OPT-IN idiom `(v && v[0] && v[0] != '0')`.
        // Written in exactly this form on purpose: scripts/check-gate-default-
        // agreement.py classifies a Feature gate's default FROM ITS READER, and
        // an idiom it cannot parse is a FAILURE, not a pass. The first draft
        // here used an early-return plus `? 0 : 1` and came back UNCLASSIFIABLE.
        static const bool s_sunAxisMc2Fix = [](){
            const char* v = getenv("MC2_SUN_AXIS_FIX");
            return (v && v[0] && v[0] != '0');
        }();
        glUniform1i(s_loc_u_sunAxisMc2Fix, s_sunAxisMc2Fix ? 1 : 0);
    }
    if (s_mechBatcherTrace) {
        static int s_uniDiagPrinted = 0;
        if (s_uniDiagPrinted < 2) {
            ++s_uniDiagPrinted;
            GLint utexVal = -99;
            if (s_loc_u_tex >= 0)
                glGetUniformiv(s_mechProgram, s_loc_u_tex, &utexVal);
            MC2_DIAGFE(stderr,
                "[MECHBATCHER v1] event=uni_probe loc_u_tex=%d u_tex_val=%d s_loc_terrainMVP=%d s_loc_u_mvp=%d\n",
                s_loc_u_tex, utexVal, s_loc_terrainMVP, s_loc_u_mvp);
        }
    }
    {
        // MC2_MECH_FRAG_DEBUG=N: legacy env path; wins over ImGui s_mechDebugMode.
        // 0=normal, 1=magenta, 2=texOnly, 3=lightOnly, 4=normal-as-color.
        // When env is unset and s_mechDebugMode=0: dbgMode=0 → byte-identical.
        const char* dbg = std::getenv("MC2_MECH_FRAG_DEBUG");
        const int dbgMode = dbg ? std::atoi(dbg) : s_mechDebugMode;
        if (s_loc_u_debugMode >= 0)
            glUniform1i(s_loc_u_debugMode, dbgMode);
    }
    // MECH-AMBIENT-1: hemisphere ambient fill strength. Uploads 0.0 when the
    // gate is OFF -> exact no-op (byte-identical default path).
    if (s_loc_u_mechAmbientV1Strength >= 0)
        glUniform1f(s_loc_u_mechAmbientV1Strength, shp.ambientStrength);
    // MECH-SPECULAR-V1: upload all 6 specular uniforms. Strength is 0.0 when
    // the gate is off -> exact no-op. The roughness/threshold uniforms are
    // uploaded unconditionally; they are inert when strength is 0. All 6 locs
    // are -1 on non-viewuniforms variants (glUniform* with loc -1 is a no-op
    // per GL spec), so the guards are redundant but kept for clarity.
    if (s_loc_u_mechSpecularV1Strength >= 0)
        glUniform1f(s_loc_u_mechSpecularV1Strength, shp.specularStrength);
    if (s_loc_u_mechMetalRoughness >= 0)
        glUniform1f(s_loc_u_mechMetalRoughness, shp.metalRoughness);
    if (s_loc_u_mechGlassRoughness >= 0)
        glUniform1f(s_loc_u_mechGlassRoughness, shp.glassRoughness);
    if (s_loc_u_mechGlassLumaThresh >= 0)
        glUniform1f(s_loc_u_mechGlassLumaThresh, shp.glassLumaThresh);
    if (s_loc_u_mechGlassMaxChanThresh >= 0)
        glUniform1f(s_loc_u_mechGlassMaxChanThresh, shp.glassMaxChanThresh);
    if (s_loc_u_mechSpecDebugMask >= 0)
        glUniform1i(s_loc_u_mechSpecDebugMask, shp.specDebugMask);
    if (s_loc_u_mechBackFillStrength >= 0)
        glUniform1f(s_loc_u_mechBackFillStrength, s_mechBackFillStrength);
    // Slice C1: StandardLit GGX gate (mutable via ImGui / batcher_setStandardLitEnabled).
    if (s_loc_u_standardLitEnabled >= 0)
        glUniform1i(s_loc_u_standardLitEnabled, s_standardLitEnabled);

    // PBR-TUNE-1: metallic influence, roughness clamp, ambient specular fill.
    // Mutable via ImGui / batcher_setPbr* accessors; env-seeded defaults.
    if (s_loc_u_pbrMetallicInfluence      >= 0) glUniform1f(s_loc_u_pbrMetallicInfluence,       s_pbrMetallicInfluence);
    if (s_loc_u_pbrRoughnessMin           >= 0) glUniform1f(s_loc_u_pbrRoughnessMin,            s_pbrRoughnessMin);
    if (s_loc_u_pbrRoughnessMax           >= 0) glUniform1f(s_loc_u_pbrRoughnessMax,            s_pbrRoughnessMax);
    if (s_loc_u_pbrAmbientSpecularStrength >= 0) glUniform1f(s_loc_u_pbrAmbientSpecularStrength, s_pbrAmbientSpecularStrength);
    // PBR-LAYERED-1: wear blend + triplanar.
    if (s_loc_u_pbrWearStrength   >= 0) glUniform1f(s_loc_u_pbrWearStrength,   s_pbrWearStrength);
    if (s_loc_u_pbrTriplanar      >= 0) glUniform1i(s_loc_u_pbrTriplanar,      s_pbrTriplanar ? 1 : 0);
    if (s_loc_u_pbrTriplanarScale >= 0) glUniform1f(s_loc_u_pbrTriplanarScale, s_pbrTriplanarScale);
    // PBR-IBL-MECH-1: SH-L2 ambient — same coefficients as static props.
    if (s_loc_u_iblSh         >= 0)
        glUniform3fv(s_loc_u_iblSh, 9, &RenderCore::kIblShCoeffs[0][0]);
    if (s_loc_u_iblShStrength >= 0)
        glUniform1f(s_loc_u_iblShStrength, s_mechIblShEnabled ? g_iblShStrength : 0.0f);

    // POOL-CONSUMERS-1 (batch-6 item 6): bind + upload the night light-pool
    // SSBO/uniforms so movers receive pool light (their own headlights,
    // building pools). u_numPoolLights=0 when gated -> byte-identical.
    gos_terrain_lighting::UploadObjectLightPoolUniforms(s_mechProgram);

    // Projection uniforms — match static_prop batcher and the
    // terrain_overlay.vert convention: terrainMVP = CPU-composed
    // axisSwap * worldToClip, row-major, uploaded with GL_FALSE.
    // Plan template said "TG_Shape::s_worldToClip with GL_TRUE" — that's
    // wrong; it skips the axis swap, mech ends up off-screen. Caught
    // 2026-05-08 by operator visual smoke (mechs invisible after PREC fix
    // re-enabled the GPU path).
    // terrainMVP fetched unconditionally so the DIAG probe below can compare
    // it against the binding-3 UBO even on the gated ViewUniforms path.
    const float* terrainMVP = gos_GetTerrainMVPMat4();
    // MECH-VIEWUNIFORMS-BLOCKBINDING-1: on the gated path the ViewUniformsBlock
    // UBO (binding=3) supplies u_worldToClipGL, so s_loc_terrainMVP is -1 (the
    // legacy uniform is #ifndef'd out) and this upload would be a no-op anyway;
    // skip it explicitly. Default path (gate OFF) uploads the legacy uniform
    // exactly as before.
    // PROP-FIXB-MVP: the default (non-UBO) path uploads the Fix-B-mirrored MVP
    // so the mech foot/terrain contact shares the terrain dispatch snapshot when
    // armed (gate ON). Gate OFF -> gos_GetMechFixBMVPMat4() returns the live MVP,
    // byte-identical to before. `terrainMVP` (live) is left untouched above so
    // the MECH_VU_DIAG probe keeps comparing the LIVE MVP against the binding-3
    // UBO. nullptr-safety: helper never returns null unless live is null too, so
    // the original `&& terrainMVP` guard still covers the null case.
    if (!s_mechViewUniforms && s_loc_terrainMVP >= 0 && terrainMVP)
        glUniformMatrix4fv(s_loc_terrainMVP, 1, GL_FALSE, gos_GetMechFixBMVPMat4());
    const float* mm = gos_GetProj2ScreenMat4();
    if (s_loc_u_mvp >= 0 && mm)
        glUniformMatrix4fv(s_loc_u_mvp, 1, GL_TRUE, mm);

    // MECH-VIEWUNIFORMS-BINDING-DIAG-1: gated read-only probe
    // (MC2_MECH_VIEWUNIFORMS_DIAG=1, default OFF). Diagnoses the reverted
    // MECH-VIEWUNIFORMS-1-PRE failure (mechs vanished on the UBO path). Recon
    // proved binding=3 is uploaded once per frame at gamecam top and is never
    // clobbered, so this confirms empirically AT MECH FLUSH TIME: (a) is the
    // ViewUniforms UBO still bound at binding point 3, and (b) does its
    // worldToClipGL match what the mech path actually uploads
    // (gos_GetTerrainMVPMat4). If max_diff != 0 the once-per-frame UBO capture
    // is stale/wrong for the mech pass; if binding3==0 the bind is dead here.
    // Pure read-back: no render effect. Rate-limited. Saves+restores the
    // generic GL_UNIFORM_BUFFER target binding.
    {
        if (s_mechViewUniformsDiag) {
            static int s_vuDiagFrame = 0;
            ++s_vuDiagFrame;
            if (s_vuDiagFrame <= 5 || (s_vuDiagFrame % 300) == 0) {
                GLint b3 = 0;
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 3, &b3);
                float ubo[16] = {0.0f};
                float maxDiff = -1.0f;
                if (b3 != 0) {
                    GLint prevBound = 0;
                    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &prevBound);
                    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)b3);
                    glGetBufferSubData(GL_UNIFORM_BUFFER, 0,
                                       (GLsizeiptr)sizeof(ubo), ubo);
                    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)prevBound);
                    if (terrainMVP) {
                        maxDiff = 0.0f;
                        for (int i = 0; i < 16; ++i) {
                            float d = ubo[i] - terrainMVP[i];
                            if (d < 0.0f) d = -d;
                            if (d > maxDiff) maxDiff = d;
                        }
                    }
                }
                std::fprintf(stderr,
                    "[MECH_VU_DIAG v1] frame=%d submits=%llu binding3_ubo=%d "
                    "max_diff_vs_terrainMVP=%.6f ubo_row0=[%.3f %.3f %.3f %.3f] "
                    "mvp_row0=[%.3f %.3f %.3f %.3f]\n",
                    s_vuDiagFrame,
                    (unsigned long long)s_lastFlushSubmitCount, b3, maxDiff,
                    ubo[0], ubo[1], ubo[2], ubo[3],
                    terrainMVP ? terrainMVP[0] : 0.0f,
                    terrainMVP ? terrainMVP[1] : 0.0f,
                    terrainMVP ? terrainMVP[2] : 0.0f,
                    terrainMVP ? terrainMVP[3] : 0.0f);
            }
        }
    }

    // GPU-SYNC-CONTRACT: the instance + bone SSBOs were just written through a
    // persistent GL_MAP_COHERENT_BIT mapping (Steps 4-5). Order those CPU writes
    // BEFORE the instanced draws below read them as SSBO. Without this, NVIDIA may
    // draw from stale/zero instance+bone data -> mechs garbled/invisible; AMD
    // tolerated it. (Default-on GPU mech path -- a sibling of the static-prop tree
    // bug; routed through the typed helper.)
    gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                   "mech_instance_bone");

    // Step 7: Issue one draw call per bucket.
    uint32_t drawnCalls = 0;
    static int s_texDiagPrinted = 0;
#ifdef MC2_VULKAN_ISLAND
    // VK-P14 MECH TIER-1: when armed, capture each run (with its LIVE slot-0 paint
    // texture resolved this frame) for the island and SKIP the GL draw below.
    if (s_vkMechGateOn) s_vkMechRunSnapshot.clear();
#endif
    for (const DrawCall& dc : drawCalls) {
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];

        if (s_loc_u_instanceBase >= 0)
            glUniform1i(s_loc_u_instanceBase, (int)dc.instanceBase);
        if (s_loc_u_materialFlags >= 0)
            glUniform1i(s_loc_u_materialFlags, (int)dc.materialFlags);

        // dc.texHandle is the mcTextureManager slot index, NOT a gos
        // handle (TG_TinyTexture::gosTextureHandle is a misnamed slot
        // index per memory/mc2_texture_handle_is_live.md). Resolve to the
        // live gos handle THIS FRAME, then to GL texture id.
        const DWORD liveGosHandle = (dc.texHandle != 0xFFFFFFFFu && mcTextureManager)
            ? mcTextureManager->get_gosTextureHandle(dc.texHandle)
            : 0u;
        const uint32_t glTexId = gos_GetGLTextureId((uint32_t)liveGosHandle);
#ifdef MC2_VULKAN_ISLAND
        if (s_vkMechGateOn) {
            Mc2MechDrawRun run{};
            run.firstIndex    = pkt.firstIndex;
            run.indexCount    = pkt.indexCount;
            run.baseVertex    = pkt.baseVertex;
            run.instanceBase  = dc.instanceBase;
            run.instanceCount = dc.instanceCount;
            run.materialFlags = dc.materialFlags;
            run.slot0GlTex    = glTexId;   // LIVE per-actor paint scheme this frame
            // VK-PARITY-MECH-1: per-type AO / normal maps (same resolve as units 6/7 below).
            {
                const DWORD aoGos = (dc.aoTexHandle != 0u && dc.aoTexHandle != 0xFFFFFFFFu && mcTextureManager)
                    ? mcTextureManager->get_gosTextureHandle(dc.aoTexHandle) : 0u;
                run.aoGlTex = aoGos ? gos_GetGLTextureId((uint32_t)aoGos) : 0u;
                const DWORD nrmGos = (dc.normalTexHandle != 0u && dc.normalTexHandle != 0xFFFFFFFFu && mcTextureManager)
                    ? mcTextureManager->get_gosTextureHandle(dc.normalTexHandle) : 0u;
                run.normalGlTex = nrmGos ? gos_GetGLTextureId((uint32_t)nrmGos) : 0u;
            }
            s_vkMechRunSnapshot.push_back(run);
        }
#endif
        if (s_mechBatcherTrace && s_texDiagPrinted < 16) {
            ++s_texDiagPrinted;
            // Probe what the bound texture object actually contains.
            GLint tw = 0, th = 0, tfmt = 0, tMin = 0, tMag = 0;
            GLint sR = 0, sG = 0, sB = 0, sA = 0;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &th);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &tfmt);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &tMin);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &tMag);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &sR);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &sG);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &sB);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &sA);
            // Probe the actually-effective sampler state (sampler-object
            // overrides texture-object params silently per brainstorm #1).
            GLint boundSampler = -1;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &boundSampler);
            GLint sampMin = -1, sampMag = -1, sampWrapS = -1;
            if (boundSampler != 0) {
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MIN_FILTER, &sampMin);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MAG_FILTER, &sampMag);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_WRAP_S,     &sampWrapS);
            }
            MC2_DIAGFE(stderr,
                "[MECHBATCHER v1] event=tex_probe glTex=%u w=%d h=%d fmt=0x%x texMin=0x%x texMag=0x%x boundSampler=%d sampMin=0x%x sampMag=0x%x sampWrapS=0x%x\n",
                glTexId, tw, th, tfmt, tMin, tMag, boundSampler, sampMin, sampMag, sampWrapS);
            // Pixel-readback diagnostic — gated on its own env var
            // (NOT MC2_MECH_BATCHER_STATS) because glGetTexImage is a
            // synchronous GPU stall that pollutes any perf measurement.
            // Only enable when actively diagnosing texture-content bugs.
            static const bool s_pixelReadback = (getenv("MC2_MECH_TEX_READBACK") != nullptr);
            if (s_pixelReadback && tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                std::vector<uint8_t> pixels((size_t)tw * th * 3, 0);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                int nonZero = 0;
                for (uint8_t b : pixels) if (b != 0) { nonZero = 1; break; }
                MC2_DIAGFE(stderr,
                    "[MECHBATCHER v1] event=tex_pixels glTex=%u bytes=%zu nonZero=%d first15="
                    "%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n",
                    glTexId, pixels.size(), nonZero,
                    pixels[ 0], pixels[ 1], pixels[ 2],
                    pixels[ 3], pixels[ 4], pixels[ 5],
                    pixels[ 6], pixels[ 7], pixels[ 8],
                    pixels[ 9], pixels[10], pixels[11],
                    pixels[12], pixels[13], pixels[14]);
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, glTexId);
        // AO-1: bind this type's AO to unit 6. dc.aoTexHandle resolves slot -> live gos
        // -> GL id; 0 -> bind 0 (the shader's HAS_AO bit is OFF for AO-less types, so an
        // unbound unit 6 is never sampled -> no black). glBindSampler(6,0) clears any
        // stale sampler-object; mech.frag uses textureLod(...,0) so no mip strict-fail.
        {
            const DWORD aoGos = (dc.aoTexHandle != 0u && dc.aoTexHandle != 0xFFFFFFFFu && mcTextureManager)
                ? mcTextureManager->get_gosTextureHandle(dc.aoTexHandle) : 0u;
            const uint32_t aoGlId = aoGos ? gos_GetGLTextureId((uint32_t)aoGos) : 0u;
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, aoGlId);
            glBindSampler(6, 0);
            glActiveTexture(GL_TEXTURE0);
        }
        // NORMALS-1: bind this type's normal map to unit 7 (same contract as AO unit 6).
        // HAS_NORMAL bit is OFF for normal-less types, so an unbound unit 7 is never
        // sampled. textureLod(...,0) in the frag avoids AMD mip strict-fail.
        {
            const DWORD nrmGos = (dc.normalTexHandle != 0u && dc.normalTexHandle != 0xFFFFFFFFu && mcTextureManager)
                ? mcTextureManager->get_gosTextureHandle(dc.normalTexHandle) : 0u;
            const uint32_t nrmGlId = nrmGos ? gos_GetGLTextureId((uint32_t)nrmGos) : 0u;
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, nrmGlId);
            glBindSampler(7, 0);
            glActiveTexture(GL_TEXTURE0);
        }
        // The actual texture-black fix in Slice A is mech.frag's
        // textureLod(u_tex, v_uv, 0.0) — see memory/amd_auto_lod_strict_fail.md.
        // Sampler-state inheritance is defended by the glBindSampler(0,
        // s_sampler) above (REPEAT/LINEAR), which OVERRIDES texture-object
        // params anyway. So no per-texture glTexParameteri here. Keeping the
        // texture object's persistent state untouched avoids leaking mech-
        // specific filter/wrap onto a gosTexture handle that another renderer
        // may want to sample with auto-LOD or CLAMP later.

#ifdef MC2_VULKAN_ISLAND
        // VK-P14 MECH TIER-1: gate ON -> the island co-writes these mechs into the
        // shared GBuffer instead; skip the GL color draw (SSBOs + feed still built).
        if (!s_vkMechGateOn)
#endif
        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES,
            (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount,
            pkt.baseVertex);

        ++drawnCalls;
    }

    // Restore prior state in reverse order of save.
    // GLSTATE-GUARD-ADOPTION-3: SSBO 0/1/2 restore via guard .reset() (runs the
    // dtor immediately, same glBindBufferBase(slot, prev) it replaces) — MUST
    // fire before gos_InvalidateRenderStateCache() below (recon requirement);
    // relative order between slots is immaterial (independent binding points).
    guardSsbo0.reset();
    guardSsbo1.reset();
    guardSsbo2.reset();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit0);
    // Slice C2 + PBR-LAYERED-1: restore PBR detail sampler units 1-4.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit1);
    glBindSampler(1, (GLuint)prevSampler1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit2);
    glBindSampler(2, (GLuint)prevSampler2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit3);
    glBindSampler(3, (GLuint)prevSampler3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit4);
    glBindSampler(4, (GLuint)prevSampler4);
    // AO-1: restore unit 6.
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit6);
    glBindSampler(6, (GLuint)prevSampler6);
    // NORMALS-1: restore unit 7.
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit7);
    glBindSampler(7, (GLuint)prevSampler7);
    glActiveTexture((GLenum)prevActiveTex);
    glBindSampler(0, (GLuint)prevSampler);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(prevDepthMask);
    glDepthFunc((GLenum)prevDepthFunc);
    if (prevCull)  { glEnable(GL_CULL_FACE); glCullFace((GLenum)prevCullMode); }
    else             glDisable(GL_CULL_FACE);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // RENDER_STATES v1: even with the explicit save/restore above, the
    // engine's applyRenderStates cache mirrors a separate (slot,value)
    // table — it does NOT re-read GL state. Without invalidate, a
    // subsequent applyRenderStates call early-outs on matching cached
    // values while the actual texture binding / sampler / depth bits we
    // touched go unrechecked. Mirrors gos_static_prop_batcher.cpp:1791.
    gos_InvalidateRenderStateCache();

    // Per-reason stats output (MC2_MECH_BATCHER_STATS=1).
    if (s_mechBatcherTrace) {
        static const char* const kFallbackNames[] = {
            "UnregisteredType", "U8BoneOverflow", "RingOverflow",
            "TglGpuUnsupported", "ShaderInitFailure",
            "StaleLightRow"   // MECH-LIGHTROW-FRESHNESS-1
        };
        static_assert(sizeof(kFallbackNames) / sizeof(kFallbackNames[0])
                          == kGpuMechFallbackReasonCount,
                      "kFallbackNames must cover every GpuMechFallbackReason");
        uint32_t fallbackTotal = 0;
        for (int i = 0; i < kGpuMechFallbackReasonCount; ++i)
            fallbackTotal += s_fallbacksThisFrame[i];
        extern uint32_t g_mc2FrameCounter;
        MC2_DIAGFE(stderr,
            "[MECHBATCHER v1] event=summary frame=%u eligible=%u submitted=%zu "
            "buckets=%zu draw_calls=%u instances=%zu fallback_total=%u\n",
            g_mc2FrameCounter, s_eligibleActorsThisFrame, s_pendingSubmits.size(),
            buckets.size(), drawnCalls, totalInstances, fallbackTotal);
        for (int i = 0; i < kGpuMechFallbackReasonCount; ++i) {
            if (s_fallbacksThisFrame[i] > 0) {
                MC2_DIAGFE(stderr,
                    "[MECHBATCHER v1] event=fallback reason=%s count=%u\n",
                    kFallbackNames[i], s_fallbacksThisFrame[i]);
            }
        }
    }

    // Persist this frame's draw-call list and SSBO sizes for flushShadow()
    // to consume on the NEXT frame (Option B: persisted drawCalls).
    s_lastDrawCalls.clear();
    s_lastDrawCalls.reserve(drawCalls.size());
    for (const DrawCall& dc : drawCalls) {
        ShadowDrawEntry e;
        e.globalPacketIdx = dc.globalPacketIdx;
        e.instanceBase    = dc.instanceBase;
        e.instanceCount   = dc.instanceCount;
        s_lastDrawCalls.push_back(e);
    }
    s_lastTotalInstances = totalInstances;
    s_lastTotalBones     = totalBones;

#ifdef MC2_VULKAN_ISLAND
    // VK-P14 MECH TIER-1: snapshot this frame's instance + bone palette (host-visible
    // copies out of the mapped ring slot) + the world->clip / depth-bias the GL VS
    // uses, so the island can memcpy them into VK SSBOs without touching a fenced GL
    // buffer. Runs only when the gate is armed (empty otherwise -> feed reports 0).
    if (s_vkMechGateOn) {
        s_vkMechInstSnapshot.assign(instDst, instDst + totalInstances);
        s_vkMechBoneSnapshot.assign(boneDst, boneDst + totalBones);
        // gos_GetMechFixBMVPMat4() is the row-major (GL_FALSE) kAxisSwap*worldToClip
        // the GL mech VS uploads as u_worldToClipGL (see the s_loc_terrainMVP upload).
        const float* mvp = gos_GetMechFixBMVPMat4();
        if (mvp) std::memcpy(s_vkMechWorldToClipGL, mvp, sizeof(s_vkMechWorldToClipGL));
        s_vkMechObjectDepthBias = mc2depth::OBJECT_DEPTH_BIAS;   // mclib/terrain_depth_bias.h
        s_vkMechFrameReady = (totalInstances > 0 && !s_vkMechRunSnapshot.empty());
    } else {
        s_vkMechFrameReady = false;
    }
#endif

    // MECH-EXTRACTION-0: persist per-actor facts for batcher_getMechPendingCount/Entry.
    // Must run before s_pendingSubmits.clear() so ExtractRenderSnapshot can read this frame's data.
    static const bool s_mechExtractEnabled = [] {
        const char* v = std::getenv("MC2_SNAPSHOT_MECH_EXTRACT");
        return v && v[0] == '1';
    }();
    if (s_mechExtractEnabled) {
        s_mechExtractPersist.clear();
        s_mechExtractPersist.reserve(s_pendingSubmits.size());
        for (uint32_t i = 0u; i < static_cast<uint32_t>(s_pendingSubmits.size()); ++i) {
            const PendingSubmit& ps = s_pendingSubmits[i];
            ExtractedMechPacket pkt{};
            pkt.objectIdRaw = ps.desc.objectIdRaw;
            pkt.instanceIdx = i;
            // MECH-EXTRACTION-1: wire real materialIdx from the handle map populated
            // in Step 2.5 earlier this flush(). Sentinel when disabled or handle absent.
            if (s_mechMaterialGpuEnabled) {
                auto it = s_mechHandleToMaterialIdx.find(ps.desc.slot0TexHandle);
                pkt.materialIdx = (it != s_mechHandleToMaterialIdx.end())
                    ? it->second : 0xFFFFFFFFu;
            } else {
                pkt.materialIdx = 0xFFFFFFFFu;
            }
            pkt.texHandle   = ps.desc.slot0TexHandle;
            pkt.typeLodIdx  = ps.typeLodIdx;
            pkt.renderFlags = ps.desc.renderFlags;
            // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: visual state for the debug dump.
            pkt.heat01      = ps.desc.heat01;
            pkt.damage01    = ps.desc.damage01;
            pkt.visualFlags = ps.desc.visualFlags;
            s_mechExtractPersist.push_back(pkt);
        }
    }

    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
}

// ---------------------------------------------------------------------------
// MECH-EXTRACTION-0: snapshot API (gate: MC2_SNAPSHOT_MECH_EXTRACT=1)
// ---------------------------------------------------------------------------

uint32_t batcher_getMechPendingCount() {
    return static_cast<uint32_t>(s_mechExtractPersist.size());
}

bool batcher_getMechPendingEntry(uint32_t idx, ExtractedMechPacket* out) {
    if (!out || idx >= static_cast<uint32_t>(s_mechExtractPersist.size()))
        return false;
    *out = s_mechExtractPersist[idx];
    out->instanceIdx = idx;
    return true;
}

// V1A: per-frame mech submit count, latched at flush() entry.
// GpuMechBatcher submits only. Does NOT include MLR fallback draws or alive count.
uint64_t batcher_getLastFlushSubmitCount() {
    return s_lastFlushSubmitCount;
}

#ifdef MC2_VULKAN_ISLAND
// ---------------------------------------------------------------------------
// VK-P14 MECH TIER-1: island feed accessors (Vulkan-free; the island never
// includes this batcher). Geometry is copy-once; the per-frame feed points at
// the persistent snapshots built at flush() end. Return 0 if not ready.
// ---------------------------------------------------------------------------
// VK-MECH-MATERIAL-LIGHTING-1: GL-owner accessors for the calc_light + fog feed.
extern "C" void gos_LightDataSsbo_GetInterop(unsigned int* glName, unsigned long long* bytes);
extern "C" void mc2GetSceneFogColorRGB(float out[3]);

extern "C" uint32_t mc2_mech_vertex_stride(void) { return (uint32_t)sizeof(GpuMechVertex); }

extern "C" int mc2_mech_island_get_geometry_feed(Mc2MechGeomFeed* out) {
    if (!out) return 0;
    if (!s_vkMechGateOn || !s_geometryFinalized || s_sharedVbo == 0 || s_sharedIbo == 0)
        return 0;
    // Byte sizes from the staged CPU geometry (uploaded verbatim to the GL buffers).
    const uint64_t vboBytes = (uint64_t)s_stagingVbo.size();
    const uint64_t iboBytes = (uint64_t)s_stagingIbo.size() * sizeof(uint32_t);
    if (vboBytes == 0 || iboBytes == 0) return 0;
    out->glVbo        = s_sharedVbo;
    out->glIbo        = s_sharedIbo;
    out->vboBytes     = vboBytes;
    out->iboBytes     = iboBytes;
    out->vertexStride = (uint32_t)sizeof(GpuMechVertex);   // 48
    // VK-RUNG8 item-9: CPU byte sources are now the ONLY mech geometry feed (the GL
    // buffer-readback fallback was deleted from the island). Populate whenever the VK
    // mech island is active (we are already past the s_vkMechGateOn guard above). A
    // size mismatch means a normals rebuild raced this accessor -> leave vboCpu null;
    // the island declines this frame and retries next frame (no GL readback rescue).
    out->vboCpu = nullptr;
    out->iboCpu = nullptr;
    if ((uint64_t)s_vkVboNativeCopy.size() == vboBytes)
        out->vboCpu = s_vkVboNativeCopy.data();
    out->iboCpu = s_stagingIbo.data();   // uploaded verbatim to s_sharedIbo
    return 1;
}

extern "C" int mc2_mech_island_get_frame_feed(Mc2MechFrameFeed* out) {
    if (!out) return 0;
    if (!s_vkMechGateOn || !s_vkMechFrameReady) return 0;
    if (s_vkMechInstSnapshot.empty() || s_vkMechRunSnapshot.empty()) return 0;
    out->instances64   = s_vkMechInstSnapshot.data();
    out->instanceCount = (uint32_t)s_vkMechInstSnapshot.size();
    out->bones64       = s_vkMechBoneSnapshot.data();
    out->boneCount     = (uint32_t)s_vkMechBoneSnapshot.size();
    out->runs          = s_vkMechRunSnapshot.data();
    out->runCount      = (uint32_t)s_vkMechRunSnapshot.size();
    // VK-FLIP-FIELD-BUG4-1: use the LIVE camera MVP, not the flush-time snapshot.
    // s_vkMechWorldToClipGL is captured only inside GpuMechBatcher::flush() (batcher
    // submit path). On frames where the batcher skips its flush (mech instance data
    // unchanged — units idle), that snapshot is STALE, but a camera PAN still changes
    // the view/proj. The island then draws mechs with last frame's MVP -> mechs FREEZE
    // in screen space while the terrain (which re-projects live) pans beneath them.
    // This accessor runs in the GL context (see gos_LightDataSsbo_GetInterop below), so
    // gos_GetMechFixBMVPMat4() returns the current-frame MVP here. Instances may be one
    // frame stale (imperceptible when idle); the PROJECTION is always current -> no
    // screen-space stick. Falls back to the snapshot if the live accessor returns null.
    if (const float* liveMvp = gos_GetMechFixBMVPMat4())
        std::memcpy(out->worldToClipGL, liveMvp, sizeof(out->worldToClipGL));
    else
        std::memcpy(out->worldToClipGL, s_vkMechWorldToClipGL, sizeof(out->worldToClipGL));
    out->objectDepthBias = s_vkMechObjectDepthBias;
    // VK-MECH-MATERIAL-LIGHTING-1: LightsData SSBO name/size + fog color for the
    // island's calc_light + fog port. Both from GL owners (this runs in GL context).
    out->lightDataGlSsbo = 0; out->lightDataBytes = 0;
    { unsigned int nm = 0; unsigned long long by = 0;
      gos_LightDataSsbo_GetInterop(&nm, &by);
      out->lightDataGlSsbo = nm; out->lightDataBytes = by; }
    out->fogColorRGB[0] = out->fogColorRGB[1] = out->fogColorRGB[2] = 0.0f;
    mc2GetSceneFogColorRGB(out->fogColorRGB);
    // VK-RUNG8 item-9: the game's CPU LightsData array (the byte source of the GL
    // SSBO upload, txmmgr renderLists) is now the ONLY per-frame lighting feed. The
    // island's GL SSBO-readback fallback was deleted. lightDataGlSsbo/Bytes above are
    // still exposed for the verification-only FEED-AUDIT byte-compare.
    out->lightDataCpu = nullptr;
    out->lightDataCpuBytes = 0;
    if (mcTextureManager) {
        out->lightDataCpu      = mcTextureManager->getLightDataPtr();
        out->lightDataCpuBytes = mcTextureManager->getLightDataBytes();
    }
    // VK-PARITY-MECH-1: default mech FS shading inputs from the GL producers.
    {
        const RenderCore::ViewUniforms& vu = RenderCore::getCurrentView().viewUniforms;
        for (int k = 0; k < 4; ++k) out->cameraWorldPos[k] = vu.cameraWorldPos[k];
        GpuMechShadingParams shp{};
        gpuMechShadingParams(&shp);
        out->ambientStrength     = shp.ambientStrength;
        out->specularStrength    = shp.specularStrength;
        out->metalRoughness      = shp.metalRoughness;
        out->glassRoughness      = shp.glassRoughness;
        out->glassLumaThresh     = shp.glassLumaThresh;
        out->glassMaxChanThresh  = shp.glassMaxChanThresh;
        out->specDebugMask       = shp.specDebugMask;
        out->importedGamma       = shp.importedGamma;
        out->importedAlbedoScale = shp.importedAlbedoScale;
        out->aoStrength          = shp.aoStrength;
        out->normalStrength      = shp.normalStrength;
        gos_terrain_lighting::ObjectLightPoolParams pp{};
        gos_terrain_lighting::GetObjectLightPoolParams(&pp);
        out->numPoolLights  = pp.pool ? pp.count : 0;
        out->poolLights48   = pp.pool;
        out->poolSoft       = pp.soft;
        out->poolMoverDim   = pp.moverDim;
        out->poolMoverScale = pp.moverScale;
        out->poolMoverMax   = pp.moverMax;
        out->poolCompress   = pp.compress;
        out->poolObjScale   = pp.objScale;
    }

    // VK-OFFLINE-PROOFS-1: golden feed dump. Fires once per process (see
    // vk_feed_dump.h). No-op unless MC2_VK_FEED_DUMP is set.
    if (mc2vkfeeddump::enabled() && mc2vkfeeddump::readyToDump() && !mc2vkfeeddump::latch("mech")) {
        if (FILE* f = mc2vkfeeddump::openAppend(/*truncateFirst=*/false)) {
            std::fprintf(f, "{\"island\":\"mech\",\"instanceCount\":%u,\"boneCount\":%u,"
                "\"runCount\":%u,\"objectDepthBias\":%.6f,\"instances\":[",
                out->instanceCount, out->boneCount, out->runCount, out->objectDepthBias);
            const GpuMechInstance* insts =
                reinterpret_cast<const GpuMechInstance*>(out->instances64);
            const GpuMechBone* bones =
                reinterpret_cast<const GpuMechBone*>(out->bones64);
            for (uint32_t i = 0; i < out->instanceCount; ++i) {
                const GpuMechInstance& mi = insts[i];
                float rootX = 0.f, rootY = 0.f, rootZ = 0.f;
                if (bones && mi.baseBoneOffset < out->boneCount) {
                    const GpuMechBone& b0 = bones[mi.baseBoneOffset];
                    rootX = b0.row0[3]; rootY = b0.row1[3]; rootZ = b0.row2[3];
                }
                std::fprintf(f, "%s{\"typeLodRecordIndex\":%u,\"baseBoneOffset\":%u,"
                    "\"objectIdRaw\":%u,\"materialIdx\":%u,\"rootBoneX\":%.4f,"
                    "\"rootBoneY\":%.4f,\"rootBoneZ\":%.4f}",
                    i ? "," : "", mi.typeLodRecordIndex, mi.baseBoneOffset,
                    mi.objectIdRaw, mi.materialIdx, rootX, rootY, rootZ);
            }
            std::fprintf(f, "]}\n");
            std::fclose(f);
        }
        mc2vkfeeddump::latch("mech") = true;
    }
    return 1;
}
#endif // MC2_VULKAN_ISLAND

void batcher_compareMechSnapshot(RenderSnapshot* snap) {
    if (!snap) return;
    const uint32_t liveCount  = static_cast<uint32_t>(s_mechExtractPersist.size());
    const uint32_t snapCount  = static_cast<uint32_t>(snap->mechPackets.size());
    if (snapCount != liveCount)
        snap->mechCountMismatch = 1u;
    const uint32_t compareCount = snapCount < liveCount ? snapCount : liveCount;
    for (uint32_t i = 0u; i < compareCount; ++i) {
        const ExtractedMechPacket& row = snap->mechPackets[i];
        const ExtractedMechPacket& ref = s_mechExtractPersist[i];
        if (row.typeLodIdx  != ref.typeLodIdx)  ++snap->mechHandleMismatch;
        if (row.objectIdRaw != ref.objectIdRaw) ++snap->mechObjectIdMismatch;
        if (row.texHandle   != ref.texHandle)   ++snap->mechTexHandleMismatch;
        // MECH-EXTRACTION-1: independent live materialIdx from s_mechHandleToMaterialIdx
        // (still valid post-flush; cleared at START of next flush). Stronger than
        // snapshot-vs-persist self-consistency — map is the same authority the SSBO
        // upload used, so a divergence here means the persist capture was wrong.
        {
            auto it = s_mechHandleToMaterialIdx.find(row.texHandle);
            const uint32_t liveMat = (s_mechMaterialGpuEnabled &&
                                      it != s_mechHandleToMaterialIdx.end())
                ? it->second : 0xFFFFFFFFu;
            if (row.materialIdx != liveMat)
                ++snap->mechMaterialIdxMismatch;
        }
    }
}

// MECH-SPINE-1: read-only free-function accessors for the Object Inspector's
// Mech Snapshot panel. Mirrors the TERRAIN-SPINE-0 pattern (program-id
// accessors in gameos_graphics.cpp). Each returns 0 / nullptr when the
// underlying state is not yet initialized. Read-only — no GL state touched,
// no draw-path mutation.
extern "C" uint32_t gos_getMechProgramId() {
    return (uint32_t)s_mechProgram;
}
extern "C" uint32_t gos_getMechShadowProgramId() {
    auto pit = glsl_program::s_programs.find("shadow_mech");
    if (pit == glsl_program::s_programs.end() || !pit->second)
        return 0u;
    return (uint32_t)pit->second->shp_;
}
// SHADOW-SPINE-0: read-only accessors for mech shadow caster counts written
// at the end of flushShadow(). Returns last-frame values; not reset.
extern "C" uint32_t gos_getMechShadowTypesDrawn() {
    return (uint32_t)s_shadowTypesDrawn;
}
extern "C" uint32_t gos_getMechShadowInstDrawn() {
    return (uint32_t)s_shadowInstDrawn;
}
extern "C" const char* gos_getMechTextureNameByNodeIdx(uint32_t nodeIdx) {
    if (nodeIdx == 0xFFFFFFFFu || !mcTextureManager) return nullptr;
    return mcTextureManager->getTextureName((DWORD)nodeIdx);
}

// MECH-DEBUG-VIEWS-1: RenderDebugView <-> mech.frag u_debugMode mapping.
// Shader branch numbers: 0=Final, 1=magenta, 2=texOnly(Albedo),
// 3=lightOnly(LightingOnly), 4=normal-as-color(Normal), 5=UV, 6=alpha, 7..9=diag.
// Mirror of StaticPropViewToShaderMode/StaticPropShaderModeToView in
// gos_static_prop_batcher.cpp; co-located here to stay with the flush site
// and the s_mechDebugMode state it drives.
int MechViewToFragDebugMode(RenderDebugView v) {
    switch (v) {
        case RenderDebugView::Final:        return 0;
        case RenderDebugView::Albedo:       return 2;
        case RenderDebugView::Normal:       return 4;
        case RenderDebugView::LightingOnly: return 3;
        default:                            return 0;  // unsupported -> Final
    }
}

RenderDebugView MechFragDebugModeToView(int m) {
    switch (m) {
        case 0: return RenderDebugView::Final;
        case 2: return RenderDebugView::Albedo;
        case 4: return RenderDebugView::Normal;
        case 3: return RenderDebugView::LightingOnly;
        default: return RenderDebugView::Final;
    }
}

extern "C" void batcher_setMechDebugMode(int shaderMode) {
    if (shaderMode < 0) shaderMode = 0;
    s_mechDebugMode = shaderMode;
}

extern "C" int batcher_getMechDebugMode() {
    return s_mechDebugMode;
}

// MECH-NORMALS-FIX-1: runtime getter/setter API for the normals mode and
// smoothing angle. These allow ImGui controls to dial settings without a restart.
// batcher_rebuildMechNormals() applies the current settings by re-uploading the
// VBO from the pristine s_stagingVbo (mode 0 = byte-identical to cooked).

extern "C" void batcher_setMechNormalsMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_mechNormalsMode = mode;
}

extern "C" int batcher_getMechNormalsMode() {
    return s_mechNormalsMode;
}

extern "C" void batcher_setMechNormalsSmoothDeg(float deg) {
    if (deg < 1.0f)   deg = 1.0f;
    if (deg > 179.0f) deg = 179.0f;
    s_mechNormalsSmoothDeg = deg;
}

extern "C" float batcher_getMechNormalsSmoothDeg() {
    return s_mechNormalsSmoothDeg;
}

// MECH-AMBIENT-1: runtime API for the gated hemisphere ambient fill. No VBO
// rebuild needed — strength is a per-flush uniform, so changes take effect the
// next frame. enabled=false uploads 0.0 (no-op).
extern "C" void batcher_setMechAmbientEnabled(int on) { s_mechAmbientV1 = (on != 0); }
extern "C" int  batcher_getMechAmbientEnabled()       { return s_mechAmbientV1 ? 1 : 0; }
extern "C" void batcher_setMechAmbientStrength(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 2.0f) s = 2.0f;
    s_mechAmbientStrength = s;
}
extern "C" float batcher_getMechAmbientStrength() { return s_mechAmbientStrength; }

// MECH-SPECULAR-V1: runtime API for the gated Blinn specular sheen. All per-flush
// uniforms — no VBO rebuild. Changes take effect the next frame. When the
// viewuniforms variant is not active (s_mechViewUniforms==false) the C++ flush
// uploads strength 0 regardless of the gate, so the setter is safe to call but
// the visual has no effect until MC2_MECH_VIEWUNIFORMS=1 is also set.
extern "C" void  batcher_setMechSpecularEnabled(int on)      { s_mechSpecularV1 = (on != 0); }
extern "C" int   batcher_getMechSpecularEnabled()            { return s_mechSpecularV1 ? 1 : 0; }
extern "C" void  batcher_setMechSpecularStrength(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 4.0f) s = 4.0f;
    s_mechSpecularStrength = s;
}
extern "C" float batcher_getMechSpecularStrength()           { return s_mechSpecularStrength; }
extern "C" void  batcher_setMechMetalRoughness(float r) {
    if (r < 0.04f) r = 0.04f;
    if (r > 1.0f)  r = 1.0f;
    s_mechMetalRoughness = r;
}
extern "C" float batcher_getMechMetalRoughness()             { return s_mechMetalRoughness; }
// BT2018-MECH-MATERIAL-GAMMA-1/TUNING-1: imported-mech albedo knobs (imported only).
extern "C" void  batcher_setImportedMechGamma(float g) {
    if (g < 1.0f) g = 1.0f; if (g > 2.4f) g = 2.4f; g_importedMechGamma = g;
}
extern "C" float batcher_getImportedMechGamma()              { return g_importedMechGamma; }
extern "C" void  batcher_setImportedMechAlbedoScale(float s) {
    if (s < 0.25f) s = 0.25f; if (s > 2.5f) s = 2.5f; g_importedMechAlbedoScale = s;
}
extern "C" float batcher_getImportedMechAlbedoScale()        { return g_importedMechAlbedoScale; }
extern "C" void  batcher_setImportedMechAoStrength(float s) {
    if (s < 0.0f) s = 0.0f; if (s > 1.0f) s = 1.0f; g_importedMechAoStrength = s;
}
extern "C" float batcher_getImportedMechAoStrength()         { return g_importedMechAoStrength; }
extern "C" void  batcher_setImportedMechNormalStrength(float s) {
    if (s < 0.0f) s = 0.0f; if (s > 1.0f) s = 1.0f; g_importedMechNormalStrength = s;
}
extern "C" float batcher_getImportedMechNormalStrength()     { return g_importedMechNormalStrength; }
extern "C" void  batcher_setMechGlassRoughness(float r) {
    if (r < 0.04f) r = 0.04f;
    if (r > 1.0f)  r = 1.0f;
    s_mechGlassRoughness = r;
}
extern "C" float batcher_getMechGlassRoughness()             { return s_mechGlassRoughness; }
extern "C" void  batcher_setMechGlassLumaThresh(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    s_mechGlassLumaThresh = t;
}
extern "C" float batcher_getMechGlassLumaThresh()            { return s_mechGlassLumaThresh; }
extern "C" void  batcher_setMechGlassMaxChanThresh(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    s_mechGlassMaxChanThresh = t;
}
extern "C" float batcher_getMechGlassMaxChanThresh()         { return s_mechGlassMaxChanThresh; }
extern "C" void  batcher_setMechSpecDebugMask(int on)        { s_mechSpecDebugMask = (on != 0); }
extern "C" int   batcher_getMechSpecDebugMask()              { return s_mechSpecDebugMask ? 1 : 0; }
extern "C" float batcher_getMechBackFillStrength()           { return s_mechBackFillStrength; }
extern "C" void  batcher_setMechBackFillStrength(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 4.0f) v = 4.0f;
    s_mechBackFillStrength = v;
}

// PBR-TUNE-1: StandardLit GGX toggle + material influence knobs.
// All per-flush uniforms — no VBO rebuild needed.
extern "C" void  batcher_setStandardLitEnabled(int on)  { s_standardLitEnabled = (on != 0) ? 1 : 0; }
extern "C" int   batcher_getStandardLitEnabled()        { return s_standardLitEnabled; }
extern "C" void  batcher_setPbrMetallicInfluence(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    s_pbrMetallicInfluence = v;
}
extern "C" float batcher_getPbrMetallicInfluence()      { return s_pbrMetallicInfluence; }
extern "C" void  batcher_setPbrRoughnessMin(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    s_pbrRoughnessMin = v;
}
extern "C" float batcher_getPbrRoughnessMin()           { return s_pbrRoughnessMin; }
extern "C" void  batcher_setPbrRoughnessMax(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    s_pbrRoughnessMax = v;
}
extern "C" float batcher_getPbrRoughnessMax()           { return s_pbrRoughnessMax; }
extern "C" void  batcher_setPbrAmbientSpecularStrength(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 2.0f) v = 2.0f;
    s_pbrAmbientSpecularStrength = v;
}
extern "C" float batcher_getPbrAmbientSpecularStrength() { return s_pbrAmbientSpecularStrength; }

// PBR-LAYERED-1: wear blend + triplanar knobs.
extern "C" void  batcher_setPbrWearStrength(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 4.0f) v = 4.0f;
    s_pbrWearStrength = v;
}
extern "C" float batcher_getPbrWearStrength()        { return s_pbrWearStrength; }
extern "C" void  batcher_setPbrTriplanar(int on)     { s_pbrTriplanar = (on != 0) ? 1 : 0; }
extern "C" int   batcher_getPbrTriplanar()           { return s_pbrTriplanar; }
extern "C" void  batcher_setPbrTriplanarScale(float v) {
    if (v < 0.001f) v = 0.001f; if (v > 10.0f) v = 10.0f;
    s_pbrTriplanarScale = v;
}
extern "C" float batcher_getPbrTriplanarScale()      { return s_pbrTriplanarScale; }

// batcher_rebuildMechNormals: re-upload the mech geometry VBO with the current
// s_mechNormalsMode and s_mechNormalsSmoothDeg. No-op if geometry is not yet
// finalized. Saves and restores the current VAO and ARRAY_BUFFER bindings so
// it is safe to call from an ImGui callback between frames.
extern "C" void batcher_rebuildMechNormals() {
    if (!s_geometryFinalized || s_sharedVao == 0) return;

    // Save current bindings that uploadMechGeometryVbo() will perturb.
    GLint prevVao = 0, prevArr = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArr);

    // uploadMechGeometryVbo() expects the VAO to be bound (attrib pointers are
    // VAO state). It also preserves the VAO's existing element-array binding
    // (s_sharedIbo) — see the comment in uploadMechGeometryVbo().
    glBindVertexArray(s_sharedVao);
    uploadMechGeometryVbo();
    glBindVertexArray(0);

    // Restore bindings so nothing upstream is surprised.
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArr);

    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=normals_rebuild mode=%d smoothDeg=%.1f nodes=%zu vbo_bytes=%zu\n",
        s_mechNormalsMode, s_mechNormalsSmoothDeg, s_nodeVboRanges.size(), s_stagingVbo.size());
}
