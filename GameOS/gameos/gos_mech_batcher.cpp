// GameOS/gameos/gos_mech_batcher.cpp — GPU mech batcher, Slice A.

#include "gos_mech_batcher_internal.h"
#include "mc2fmt.h"


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
// NOTE: raw gos_terrain_indirect_getDispatchMvp16() is phase-private to terrain/
// water passes — mech draw uses gos_GetObjectDrawMVP() (the accessor above).
// Enforced by scripts/check-object-mvp-currency.py.

// MC2_PROP_FIXB_MVP — default ON (=0 reverts, killswitch). Consume the
// dispatch-MVP snapshot when the solid pass is armed (un-armed arm returns the
// live pointer anyway). Sampled once at process start.
bool s_mechFixBMvpEnabled() {
    static const bool on = mc2::gate::get<"MC2_PROP_FIXB_MVP">();
    return on;
}
// RENDER-VIEW-CURRENCY-1: snapshot only when its view epoch matches the current
// view; otherwise the live MVP. Same policy as the static-prop family. Killswitch
// preserved via the arg.
const float* gos_GetMechFixBMVPMat4() {
    return gos_GetObjectDrawMVP(s_mechFixBMvpEnabled());
}

// Default-on flip (2026-05-09): all GPU mech killswitches now default to ON.
// The rows are default-on kill switches (a leading "0" turns one off). Operators
// can opt out per-flag via MC2_GPU_MECH_<FLAG>=0. Pre-flip pattern was a
// presence test (default-off opt-in); post-flip is `default-on, "0" opts out`. Each individual slice's killswitch comments in
// gos_mech_killswitch.h document the inverted semantics.
//
// Why default-on shipped 2026-05-09: full slice campaign accumulated
// substantial empirical soak — tier1 5/5 PASS at full bore, multiple 90s
// mc2_10 Tracy captures clean, mouse-pick + sensor-diamond visual canaries
// passed, combined stack delivers Mech3D.UpdateGeometry mean 71→14.08µs/call
// (-80%). User explicitly signed off on B1's "needs more verification"
// constraint based on this evidence.
// ImGui visibility kill-switch. Always true at startup; toggled by the
// Graphics Options panel "Draw Mechs" checkbox. When false, both the GPU
// batcher submit and the CPU Render(true) fallback are skipped.
bool g_drawMechs = true;

// Slice A: GPU mech batcher. Foundation for the entire stack.
// Opt-out: MC2_GPU_MECHS=0
bool g_useGpuMechs = mc2::gate::get<"MC2_GPU_MECHS">();

// VK-P14 MECH TIER-1: flush() BUILDS the instance, bone and run feeds on the CPU;
// the caller (txmmgr) runs mc2_vk_mech_island_run() to co-write the mechs IN-PLACE into the shared GBuffer.
// MC2_VK_MECH_FEED_NATIVE is DEPRECATED / a no-op (the native CPU feed is unconditional under the VK mech
// island; the GL readback fallback is deleted). The name is still parsed so old command lines do not hit
// unknown-variable behavior; setting it prints a one-line note.
void noteDeprecatedMechFeedNative() {
    static bool done = false;
    if (done) return;
    done = true;
    if (mc2::gate::get<"MC2_VK_MECH_FEED_NATIVE">())
        mc2::fmt::print_err("[VK_MECH_FEED] MC2_VK_MECH_FEED_NATIVE is deprecated and now a no-op: native CPU feed is always used under the VK mech island (GL readback fallback deleted).\n");
}

// VK-P14 MECH feed snapshots (persisted host-visible copies the island memcpy's).
// Populated at flush() end.
std::vector<GpuMechInstance> s_vkMechInstSnapshot;
std::vector<GpuMechBone>     s_vkMechBoneSnapshot;
std::vector<Mc2MechDrawRun>  s_vkMechRunSnapshot;
float                        s_vkMechWorldToClipGL[16] = {0};
float                        s_vkMechObjectDepthBias   = 0.0f;
bool                         s_vkMechFrameReady        = false;
bool                         s_vkMechShadowReady       = false;

// Exact bytes uploaded to s_sharedVbo (the normals-recomputed `work` copy —
// s_stagingVbo alone is NOT the uploaded bytes when s_mechNormalsMode != 0).
// Refreshed by uploadMechGeometryVbo(); cleared with the staging on map load.
std::vector<uint8_t>         s_vkVboNativeCopy;
// Geometry generation: bumped on every rebuild of the VBO copy AND of s_stagingIbo, so a consumer that keeps its own copy of both (the VK shadow island) re-uploads exactly when either changed.
uint32_t                     s_vkMechGeomGen = 0;
// The geometry generation the run / instance / bone snapshots were built against: their firstIndex ranges index THAT index set.
uint32_t                     s_vkMechSnapshotGeomGen = 0;

// SimpleCamera UI preview-render context depth (Mech Bay / Mech Purchase).
// See gos_mech_killswitch.h. Zero during tactical/world rendering.
int g_mechPreviewRenderDepth = 0;

// MECH-KILLSWITCH-LIGHTING-RETIRE-1: u_lightingMode is fed constant 1
// (VS-side lighting always on; default-ON). See gos_mech_killswitch.h.

// MECH-KILLSWITCH-SKIN-RETIRE-1: MC2_GPU_MECH_SKIN retired to constant
// (default-ON, proven no-op — stock + all imports single-bone). u_skinningMode
// fed constant 1; weighted branch kept as forward capacity. See gos_mech_killswitch.h.

// MECH-KILLSWITCH-SHADOW-PAIR-RETIRE-1: MC2_GPU_MECH_SHADOW_SKIP and
// MC2_GPU_MECH_SHADOW_STATE_STRIP retired to constants (both default-ON,
// strict no-ops on the tessellated GPU-mech path). Logic now inline in
// Mech3DAppearance::updateGeometry. See gos_mech_killswitch.h.

// MECH-KILLSWITCH-SENSORSKIP-RETIRE-1: MC2_GPU_MECH_SENSOR_SKIP retired to a
// constant (was default-ON, strict no-op). Skip logic is now inline in
// Mech3DAppearance::updateGeometry. See gos_mech_killswitch.h.

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------
// MECH-VIEWUNIFORMS-BLOCKBINDING-1: mech-specific ViewUniforms gate,
// DEFAULT ON -- kill-switch MC2_MECH_VIEWUNIFORMS=0. Independent of the global
// MC2_VIEW_UNIFORMS. When ON (default), the mech shader is compiled with the
// ViewUniformsBlock (binding=3) consumer and the block binding is enforced
// explicitly after link; the legacy u_worldToClipGL upload is skipped. Setting
// =0 restores the legacy upload, byte-identical to pre-feature.
// This block said "DEFAULT OFF ... opt-in (=1)" until ENV-DROP-CLOSE-1
// (2026-07-30) while the reader four lines below already said DEFAULT-ON, so
// the two halves of the same declaration disagreed.
const bool s_mechViewUniforms = mc2::gate::get<"MC2_MECH_VIEWUNIFORMS">();
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
int s_mechNormalsMode = mc2::gate::get<"MC2_MECH_NORMALS_MODE">();
// Smoothing angle for mode 2 (degrees): faces meeting at a shared vertex blend
// only when within this angle, so a smaller value preserves MORE hard edges
// (e.g. lower it to sharpen the cockpit). Env-tunable for dial-in; default 60.
float s_mechNormalsSmoothDeg = mc2::gate::get<"MC2_MECH_NORMALS_SMOOTH_DEG">();   // row: default 60, clamps to [1, 179]

// MECH-NORMALS-FIX-1: per-node VBO byte ranges recorded during registerTypeLod.
// Each entry is {startByte, endByte} in s_stagingVbo for one node's verts.
// uploadMechGeometryVbo() iterates these to bound recomputeMechNodeNormals
// to a single node (it must not blend normals across node boundaries).
// Cleared at onMapLoad() alongside s_stagingVbo.
std::vector<std::pair<size_t,size_t>> s_nodeVboRanges;

// MECH-AMBIENT-1: hemisphere ambient FILL for mechs. DEFAULT-ON (user-approved
// 2026-05-28 at strength 0.15) with kill-switch MC2_MECH_AMBIENT_V1=0. When off
// the strength uploaded to the shader is 0.0 -> exact no-op (legacy look). Both
// mutable so ImGui can dial them live (batcher_setMechAmbient*). The shader term
// is a per-fragment hemisphere using the world normal; no PBR/material/team-mask
// data involved. Works on legacy and ViewUniforms mech paths (no camera data).
bool s_mechAmbientV1 = mc2::gate::get<"MC2_MECH_AMBIENT_V1">();
float s_mechAmbientStrength = mc2::gate::get<"MC2_MECH_AMBIENT_V1_STRENGTH">();   // row: user-tuned default 0.15, clamps to [0, 2]

// MECH-SPECULAR-V1: conservative Blinn specular sheen. DEFAULT-OFF (gate
// MC2_MECH_SPECULAR_V1=1 to enable). Only effective when MC2_MECH_VIEWUNIFORMS=1
// (the shader variant with MC2_USE_VIEW_UNIFORMS defined); on all other paths the
// C++ side uploads strength 0 → exact no-op. Per-flush uniforms — no VBO rebuild.
bool s_mechSpecularV1 = mc2::gate::get<"MC2_MECH_SPECULAR_V1">();
// Default 0.05: at 1.0 the Blinn highlight read as a near-glow on mech armor; 0.05 is a subtle metal glint.
// MC2_MECH_SPECULAR_STRENGTH overrides (row clamps to [0, 4]); also profile-backed via the mechSpecularStrength
// key (visual_tuning.json).
float s_mechSpecularStrength = mc2::gate::get<"MC2_MECH_SPECULAR_STRENGTH">();
// Rows carry the defaults (0.85 / 0.25 / 0.12 / 0.18) and the clamps ([0.04, 1] roughness, [0, 1] thresholds).
float s_mechMetalRoughness = mc2::gate::get<"MC2_MECH_METAL_ROUGHNESS">();
float s_mechGlassRoughness = mc2::gate::get<"MC2_MECH_GLASS_ROUGHNESS">();
float s_mechGlassLumaThresh = mc2::gate::get<"MC2_MECH_GLASS_LUMA_THRESH">();
float s_mechGlassMaxChanThresh = mc2::gate::get<"MC2_MECH_GLASS_MAXCHAN_THRESH">();
bool  s_mechSpecDebugMask = false;  // ImGui only; no env var

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

// Geometry (immutable after finalizeGeometry).
bool   s_geometryFinalized = false;
// VPL-#11 (SP-logistics late roster): set by registerTypeLod when a type
// registers AFTER finalizeGeometry (campaign .fit resume spawns the
// player force-group post-Mission::init-finalize). finalizePending()
// consumes it to rebuild the shared VBO/IBO from the (retained) staging.
bool   s_pendingLateTypes = false;
uint32_t s_lateStagedCount = 0;  // late (type,lod) regs since last finalizePending

// Staging buffers. RETAINED for the whole mission (NOT freed at
// finalizeGeometry) so finalizePending() can append a late-registered
// type and rebuild the immutable shared VBO/IBO from the full
// original+late staging (VPL-#11 SP-logistics fix; the alternative of
// re-deriving geometry from TG type shapes is fragile). Cleared only at
// onMapLoad(). Memory cost is MB-scale and intentionally not optimized.
std::vector<uint8_t>   s_stagingVbo;
std::vector<uint32_t>  s_stagingIbo;

// Type registration.
std::vector<GpuMechTypeLodRecord> s_typeLodRecords;
std::vector<GpuMechPacket>        s_packets;
std::unordered_map<TypeLodKey, uint32_t, TypeLodKeyHash> s_typeLodIndex;

std::vector<PendingSubmit> s_pendingSubmits;

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
std::vector<ExtractedMechPacket> s_mechExtractPersist;
// V1A: per-frame submit count latched at flush() entry (before any
// early-return guards clear s_pendingSubmits). Counts GpuMechBatcher
// submits only — does NOT include MLR fallback draws or alive count.
// queryVisibility() reads this for mechs_visible in VisibilityResult.
uint64_t                     s_lastFlushSubmitCount = 0;

// Per-frame mech shadow caster counts (the previous-frame run list the VK shadow cascades draw); read by the state dump.
int s_shadowTypesDrawn = 0;
int s_shadowInstDrawn  = 0;

// Counters.
bool     s_mechBatcherTrace     = false;
bool     s_mechBatcherTraceInit = false;
bool     s_mechLightTrace       = false;
bool     s_mechLightTraceInit   = false;
uint32_t s_lightCacheFullFrames = 0;  // monotonic; emitted on first overflow per frame
uint32_t s_eligibleActorsThisFrame = 0;
uint32_t s_fallbacksThisFrame[kGpuMechFallbackReasonCount] = {};  // indexed by GpuMechFallbackReason

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
uint64_t s_gpuMechIdWritesThisMission = 0;
uint64_t s_allowedLateRegEvents    = 0;
uint64_t s_disallowedLateRegEvents = 0;
bool     s_lastFailWasLateReg      = false;

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
const bool s_mechSpotlightRealTrace = mc2::gate::get<"MC2_SPOTLIGHT_REAL_TRACE">();
std::unordered_map<const Mech3DAppearanceType*, uint32_t>
    s_spotlightRealTypeNodeCount;
uint64_t s_spotlightReal_typeNodeEvents = 0;  // monotonic (a)
uint64_t s_spotlightReal_drawWindow     = 0;  // 600-frame window (b)
uint64_t s_spotlightReal_drawMono       = 0;  // monotonic (b)
uint64_t s_spotlightReal_drawFrameCount = 0;  // frames flushed since process start
bool     s_spotlightReal_firstHitTypeNode = false;
bool     s_spotlightReal_firstHitDraw     = false;

// ---------------------------------------------------------------------------
// MaterialGpu Mech-1 substrate
// MC2_MATERIAL_GPU defaults ON. Set MC2_MATERIAL_GPU=0 to disable.
// ---------------------------------------------------------------------------
const bool s_mechMaterialGpuEnabled = mc2::gate::get<"MC2_MATERIAL_GPU">();
std::vector<RenderCore::MaterialGpu>        s_mechMaterialTable;
std::unordered_map<uint32_t, uint32_t>      s_mechHandleToMaterialIdx;
std::vector<uint32_t>                       s_mechDrawMaterialIdx;
uint32_t                                    s_mechMaterialGpuFrameCount = 0;

// Slice C1: named profile index for binding-7 SSBO (gos_materials).
// Resolved lazily at first flush() (after gos_materials::init() runs).
// 0 = default/passthrough (env unset or none/0). Non-zero = named profile active;
// overrides per-instance materialIdx only when selected so the existing per-actor
// albedo table (binding 2) is undisturbed when MC2_MECH_SURFACE_MATERIAL is unset.
// Protected by s_mechSurfaceMaterialIdxResolved to call getProfileIndex() only once.
uint32_t s_mechSurfaceMaterialIdx         = 0u;
bool     s_mechSurfaceMaterialIdxResolved = false;

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
    s_vkVboNativeCopy.clear();   // VK-MECH-FEED-NATIVE-1: geometry rebuilt per map
    s_nodeVboRanges.clear();   // MECH-NORMALS-FIX-1: per-node ranges are per-mission
    s_geometryFinalized = false;
    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
    // The feed snapshots index the geometry just cleared: drop them with it.
    s_vkMechInstSnapshot.clear();
    s_vkMechBoneSnapshot.clear();
    s_vkMechRunSnapshot.clear();
    s_vkMechFrameReady = false;
    s_vkMechShadowReady = false;
    // Mech-1: reset the MaterialGpu tables on map load
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
        static_cast<unsigned long long>(mlrDraws));

    // M2.5 (Q4 + Q6 amendment 2): always-on per-mission writer summary.
    // Surfaces gpu_mech_id_writes to the M2.6 readiness decision rule.
    // Always-on (NOT env-gated): M2.6 needs this signal regardless of
    // MC2_OBJECT_ID_BUFFER state to size the MLR-vs-GPU split.
    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=%llu\n",
        static_cast<unsigned long long>(s_gpuMechIdWritesThisMission));
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
            static_cast<unsigned long long>(staleRows), static_cast<unsigned long long>(oobRows));
    }
    s_geometryFinalized = false;
    s_pendingSubmits.clear();
    s_mechExtractPersist.clear();  // MECH-EXTRACTION-0
    s_vkMechInstSnapshot.clear();
    s_vkMechBoneSnapshot.clear();
    s_vkMechRunSnapshot.clear();
    s_vkMechFrameReady = false;
    s_vkMechShadowReady = false;
    s_mechMaterialTable.clear();
    s_mechHandleToMaterialIdx.clear();
    s_mechDrawMaterialIdx.clear();
    // Slice C1: release the named material profile table (binding 7).
    // shutdown() resets s_initialized so init() can be called again on next map load.
    // Also reset the lazy-resolve guard so the profile index is re-queried next mission.
    gos_materials::shutdown();
    s_mechSurfaceMaterialIdx         = 0u;
    s_mechSurfaceMaterialIdxResolved = false;
    MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=map_unload\n");
}

bool GpuMechBatcher::wasLastFailureLateRegistration() const { return s_lastFailWasLateReg; }
uint64_t GpuMechBatcher::getAllowedLateRegEventCount()      { return s_allowedLateRegEvents; }
uint64_t GpuMechBatcher::getDisallowedLateRegEventCount()  { return s_disallowedLateRegEvents; }
bool GpuMechBatcher::isFinalized() const                    { return s_geometryFinalized; }

void GpuMechBatcher::recordEligibleActor()                         { ++s_eligibleActorsThisFrame; }
void GpuMechBatcher::recordCpuFallback(GpuMechFallbackReason r)   { ++s_fallbacksThisFrame[static_cast<int>(r)]; }
