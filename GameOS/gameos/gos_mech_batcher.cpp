// GameOS/gameos/gos_mech_batcher.cpp — GPU mech batcher, Slice A.

#include "gos_mech_batcher_internal.h"
#include "mc2fmt.h"


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

#ifdef MC2_VULKAN_ISLAND
// VK-P14 MECH TIER-1 (gate MC2_VK_MECH_TIER1, requires MC2_VK_SHARED_SCENE_DEPTH,
// default-OFF): when armed, the flush() below still BUILDS the instance/bone SSBOs
// and the feed snapshot, but SKIPS its own GL color draws; the caller (txmmgr) runs
// mc2_vk_mech_island_run() to co-write the mechs IN-PLACE into the shared GBuffer.
// Gate OFF -> vkMechGateOn() stays false -> GL flush() byte-identical.
// Resolved on first use, not at static init: the gate registry then sees the environment after any
// launcher_env.json apply, and the session fail-soft flag as it stands when the first flush runs.
bool vkMechGateOn() {
    static int state = -1;
    if (state == -1) {
        // Umbrella member: MC2_VK_SHARED_GBUFFER=1 arms it.
        state = mc2vkgate::umbrellaOn() ? 1 : 0;   // the co-write depth-tests against the shared depth
        // VK-RUNG8 item-9: MC2_VK_MECH_FEED_NATIVE is DEPRECATED / a no-op (the native CPU feed is unconditional
        // under the VK mech island; the GL readback fallback is deleted). The name is still parsed so old
        // command lines do not hit unknown-variable behavior; setting it prints a one-line note.
        if (mc2::gate::get<"MC2_VK_MECH_FEED_NATIVE">())
            mc2::fmt::print_err("[VK_MECH_FEED] MC2_VK_MECH_FEED_NATIVE is deprecated and now a no-op: native CPU feed is always used under the VK mech island (GL readback fallback deleted).\n");
    }
    return state == 1;
}

// VK-P14 MECH feed snapshots (persisted host-visible copies the island memcpy's).
// Populated at flush() end ONLY when vkMechGateOn(); empty otherwise.
std::vector<GpuMechInstance> s_vkMechInstSnapshot;
std::vector<GpuMechBone>     s_vkMechBoneSnapshot;
std::vector<Mc2MechDrawRun>  s_vkMechRunSnapshot;
float                        s_vkMechWorldToClipGL[16] = {0};
float                        s_vkMechObjectDepthBias   = 0.0f;
bool                         s_vkMechFrameReady        = false;

// Exact bytes uploaded to s_sharedVbo (the normals-recomputed `work` copy —
// s_stagingVbo alone is NOT the uploaded bytes when s_mechNormalsMode != 0).
// Refreshed by uploadMechGeometryVbo(); cleared with the staging on map load.
std::vector<uint8_t>         s_vkVboNativeCopy;
// Geometry generation: bumped on every rebuild of the VBO copy AND of s_stagingIbo, so a consumer that keeps its own copy of both (the VK shadow island) re-uploads exactly when either changed.
uint32_t                     s_vkMechGeomGen = 0;
// The geometry generation the run / instance / bone snapshots were built against: their firstIndex ranges index THAT index set.
uint32_t                     s_vkMechSnapshotGeomGen = 0;
#endif

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
bool   s_programLoadTried  = false;
bool   s_programLoadFailed = false;
GLuint s_mechProgram       = 0;
glsl_program* s_mechProgramObj = nullptr;

// MECH-DEBUG-VIEWS-1: ImGui-settable debug mode (mirrors s_staticPropDebugMaterialMode).
// 0 = Final (normal rendering); env MC2_MECH_FRAG_DEBUG wins when set.
int s_mechDebugMode = 0;

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
// Shared diag gate (link-time block-binding log + flush-time binding/matrix
// probe). Default OFF (MC2_MECH_VIEWUNIFORMS_DIAG=1).
const bool s_mechViewUniformsDiag = mc2::gate::get<"MC2_MECH_VIEWUNIFORMS_DIAG">();

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
// MECH-BACK-FILL-1: cool-sky fill for the StandardLit shadow hemisphere.
// Default OFF (0.0). Enable via MC2_MECH_BACK_FILL=<strength> (e.g. 1.5).
float s_mechBackFillStrength = mc2::gate::get<"MC2_MECH_BACK_FILL">();

// PBR-TUNE-1: StandardLit GGX gate + material influence knobs.
// Mutable so ImGui can dial live (batcher_setStandardLitEnabled / batcher_setPbr*).
// Env-seeded defaults; no VBO rebuild needed (per-flush uniforms).
int s_standardLitEnabled = mc2::gate::get<"MC2_STANDARD_LIT_V1">();
float s_pbrMetallicInfluence = mc2::gate::isSet<"MC2_PBR_METALLIC_INFLUENCE">()
    ? mc2::gate::get<"MC2_PBR_METALLIC_INFLUENCE">()
    : (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">() ? 0.03f : 0.15f);
float s_pbrRoughnessMin = mc2::gate::isSet<"MC2_PBR_ROUGHNESS_MIN">()
    ? mc2::gate::get<"MC2_PBR_ROUGHNESS_MIN">()
    : (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">() ? 0.65f : 0.45f);
float s_pbrRoughnessMax = mc2::gate::isSet<"MC2_PBR_ROUGHNESS_MAX">()
    ? mc2::gate::get<"MC2_PBR_ROUGHNESS_MAX">()
    : (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">() ? 0.90f : 0.90f);
float s_pbrAmbientSpecularStrength = mc2::gate::get<"MC2_PBR_AMBIENT_SPECULAR">();

// PBR-LAYERED-1: PaintedMetal003 paint layer + triplanar. All mutable via ImGui.
uint32_t s_mechPaintSurfaceMaterialIdx = 0u;  // resolved lazily alongside s_mechSurfaceMaterialIdx
float s_pbrWearStrength = mc2::gate::isSet<"MC2_PBR_WEAR_STRENGTH">()
    ? mc2::gate::get<"MC2_PBR_WEAR_STRENGTH">()
    : (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">() ? 0.0f : 1.0f);
int s_pbrTriplanar = mc2::gate::get<"MC2_PBR_TRIPLANAR">();
float s_pbrTriplanarScale = mc2::gate::get<"MC2_PBR_TRIPLANAR_SCALE">();
// PBR-IBL-MECH-1: SH-L2 image-based ambient for mechs.
// Shares g_iblShStrength with static props (same slider). Default-ON; =0 disables.
const bool s_mechIblShEnabled = mc2::gate::get<"MC2_MECH_IBL_SH">();

// Cached uniform locations (set at program link time).
GLint s_loc_u_instanceBase    = -1;
GLint s_loc_u_materialFlags   = -1;
GLint s_loc_terrainMVP     = -1;
GLint s_loc_u_mvp             = -1;
GLint s_loc_u_tex             = -1;
GLint s_loc_u_fogValue        = -1;
GLint s_loc_u_debugMode       = -1;
GLint s_loc_u_mechAmbientV1Strength = -1;  // MECH-AMBIENT-1
GLint s_loc_u_lightingMode    = -1;
GLint s_loc_u_skinningMode    = -1;
GLint s_loc_u_sunAxisMc2Fix   = -1;  // SUN-AXIS-FIX-1
// MECH-SPECULAR-V1: cached locations (all -1 on non-viewuniforms variants).
GLint s_loc_u_mechSpecularV1Strength  = -1;
GLint s_loc_u_mechMetalRoughness      = -1;
GLint s_loc_u_mechGlassRoughness      = -1;
GLint s_loc_u_mechGlassLumaThresh     = -1;
GLint s_loc_u_mechGlassMaxChanThresh  = -1;
GLint s_loc_u_mechSpecDebugMask       = -1;
GLint s_loc_u_mechBackFillStrength    = -1;
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

GLint s_loc_u_importedGamma           = -1;
GLint s_loc_u_importedAlbedoScale     = -1;
GLint s_loc_u_mechAoTex               = -1;   // AO-1: AO sampler (unit 6)
GLint s_loc_u_aoStrength              = -1;
GLint s_loc_u_mechNormalTex           = -1;   // NORMALS-1: normal sampler (unit 7)
GLint s_loc_u_normalStrength          = -1;
// Slice C1: StandardLit toggle. Returns -1 until mech.frag declares the uniform;
// guarded with >= 0 at upload so no crash when shader lacks it.
GLint s_loc_u_standardLitEnabled      = -1;
// Slice C2: PBR detail material samplers + tile scale.
GLint s_loc_u_pbrNormalTex            = -1;
GLint s_loc_u_pbrOrmTex               = -1;
GLint s_loc_u_pbrTileScale            = -1;
// PBR-TUNE-1: material influence knobs (metallic scale, roughness clamp, ambient specular).
GLint s_loc_u_pbrMetallicInfluence      = -1;
GLint s_loc_u_pbrRoughnessMin           = -1;
GLint s_loc_u_pbrRoughnessMax           = -1;
GLint s_loc_u_pbrAmbientSpecularStrength = -1;
// PBR-LAYERED-1: paint layer + triplanar.
GLint s_loc_u_pbrPaintNormalTex  = -1;
GLint s_loc_u_pbrPaintOrmTex     = -1;
GLint s_loc_u_pbrWearStrength    = -1;
GLint s_loc_u_pbrTriplanar       = -1;
GLint s_loc_u_pbrTriplanarScale  = -1;
// PBR-IBL-MECH-1: SH-L2 ambient.
GLint s_loc_u_iblSh             = -1;
GLint s_loc_u_iblShStrength     = -1;

// Geometry (immutable after finalizeGeometry).
GLuint s_sharedVao = 0;
GLuint s_sharedVbo = 0;
GLuint s_sharedIbo = 0;
GLuint s_sampler   = 0;    // session-lifetime; GL_REPEAT / LINEAR
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

// Per-frame ring SSBOs.
GLuint   s_instanceSsbo    = 0;
GLuint   s_boneSsbo        = 0;
// VK-SHADOW-RING-OWNER-1: parallel GpuBufferOwner identity records for the two
// per-frame ring SSBOs (logical id + lifetime + debug name + handle). The raw GLuint
// globals above are UNCHANGED so every gen/bind/map/delete site keeps its exact GL
// args/order; the owner record's backendHandle is written straight from the freshly
// gen'd handle (pattern (b): `owner.backendHandle = s_xxxSsbo` right after glGenBuffers,
// mirrors view_uniforms). Registered at create, invalidated on destroy (grow-realloc +
// onMapUnload). Lifetime Persistent: persistent-mapped, realloc-on-grow, freed on unload.
RenderCore::GpuBufferOwner s_instanceSsboOwner{
    RenderCore::RenderResourceId::MechInstanceRingSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "MechInstanceRingSsbo",
    0u};
RenderCore::GpuBufferOwner s_boneSsboOwner{
    RenderCore::RenderResourceId::MechBoneRingSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "MechBoneRingSsbo",
    0u};
void*    s_instanceMap     = nullptr;
void*    s_boneMap         = nullptr;
size_t   s_instanceCapacity = 0;  // per ring slot (in GpuMechInstance units)
size_t   s_boneCapacity     = 0;  // per ring slot (in GpuMechBone units)
uint32_t s_frameSlot        = 0;
GLsync   s_fence[MECH_RING_FRAMES] = {};

static constexpr size_t kInitialInstancesPerFrame = 512;
static constexpr size_t kInitialBonesPerFrame     = 8192;
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
std::vector<ShadowDrawEntry> s_lastDrawCalls;
size_t                       s_lastTotalInstances = 0;
size_t                       s_lastTotalBones     = 0;

// V1A: per-frame submit count latched at flush() entry (before any
// early-return guards clear s_pendingSubmits). Counts GpuMechBatcher
// submits only — does NOT include MLR fallback draws or alive count.
// queryVisibility() reads this for mechs_visible in VisibilityResult.
uint64_t                     s_lastFlushSubmitCount = 0;

// File-scope counters written by flushShadow() and read by Task 6 probe.
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
// MECH-MATERIAL-SSBO-OWNER-1: per-actor MaterialGpu table SSBO (binding 2),
// narrowed behind a GpuBufferOwner identity record (logical id + lifetime +
// debug name + GLuint value). GL calls happen at the same sites with the same
// args/order/flags/slot; the raw handle is reached only via owner.backendHandle.
// Lifetime Persistent: survives across frames, torn down on map load/unload.
RenderCore::GpuBufferOwner                  s_mechMaterialSsbo{
    RenderCore::RenderResourceId::MaterialGpuBuffer,
    RenderCore::RenderResourceLifetime::Persistent,
    "MaterialGpuBuffer",
    0u};
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
// Shader load
// ---------------------------------------------------------------------------
void loadProgramsIfNeeded() {
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
        mc2::fmt::print_err("[PIPELINE_VARIANT] pipeline=MechOpaque glProgram={:d} key={} vertexLayout={}\n", mc2::fmt::as_u32(s_mechProgram), pkVariant.c_str(), mc2::fmt::safe(RenderCore::vertexLayoutName(RenderCore::VertexLayoutId::MechGpuVertex48B)));
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
    if (mc2::gate::get<"MC2_LIGHTSSBO_TRACE">()) {
        GLint maxSsbo = 0;
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSsbo);
        mc2::fmt::print_err("[LIGHTSSBO v1] event=mech_ssbo_check max_ssbo_block={:d} (no 64-slot cap)\n", mc2::fmt::as_i32(maxSsbo));
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
        mc2::fmt::print_err("[MECH_VU_DIAG v1] event=block_verify prog={:d} vu_block_idx={:d} binding={:d} (expect 3) loc_u_worldToClipGL={:d}\n", mc2::fmt::as_u32(s_mechProgram), mc2::fmt::as_u32(vuBlockIdx), mc2::fmt::as_i32(vuBind), mc2::fmt::as_i32(s_loc_terrainMVP));
    }

    MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=shader_ok prog=%u\n", s_mechProgram);
}

// ---------------------------------------------------------------------------
// Ring SSBO management
// ---------------------------------------------------------------------------
void ensureRingCapacity(size_t neededInstances, size_t neededBones) {
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
    static const bool s_shadowEnabled = mc2::gate::get<"MC2_SHADOW_ENABLE">();
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
