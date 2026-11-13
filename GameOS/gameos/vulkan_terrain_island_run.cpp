#include "Gates/gate_cold.h"
// VK terrain island: the extern C entry points: enabled / ready / prepare, the frame UBO and light-pool staging, and the per-frame run.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"
#include "vulkan_mover_shadow.h"
#include "../../RenderCore/TemporalInputs.h"
#include "hitch_stats.h"
#include "terrain_record_cache.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include <algorithm>
#include <numeric>
#include "../../mclib/battle_bench_wait.h"
#include "vk_island_fail_policy.h"
#include "vk_rt_scene.h"
#include "vk_dynamic_rendering.h"

#include "visual_tuning_profile.h"
#include "mission_environment_gpu.h"
#include "vk_water_planar.h"
#include "water_planar_terrain.h"


// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern uint32_t g_mc2RenderFrame;   // mclib/tgl.cpp
float mc2SceneLightScale();   // mclib/camera.cpp: LOCAL-LIGHTS-4 mission night scale of the prelit ground
static uint32_t g_sceneClearedRenderFrame = 0;   // render frame + 1 of the last shared-scene clear (0 = never)

// The per-frame mission environment (EnvironmentUBO, binding 26 of set[par]): the one producer fills a frame-ring slice and the
// binding is re-pointed at it. A new revision follows, so props, mechs, vegetation and water re-copy it with the rest of the set.
// Call after the slot wait that retires set[par] (the same place as the mover shadow bind).
bool vk_terrain_detail::bind_environment(TerrainIsland* s, uint32_t par) {
    auto& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const mc2vk::FrameAlloc slice = rctx.frameAlloc(sizeof(EnvironmentUBO));
    if (!slice) return false;
    EnvironmentUBO env;
    mc2::environment::fillGpu(visualTuning_environment(), mc2SceneLightScale(), env);
    std::memcpy(slice.ptr, &env, sizeof(env));
    const VkDescriptorBufferInfo info{slice.buffer, slice.offset, sizeof(EnvironmentUBO)};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s->set[par]; w.dstBinding = 26; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &info;
    vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
    mc2vk::noteDescWrites(1, 0);
    s->setRev[par] = mc2vk::nextDescRevision();
    return true;
}

// The visual mode a patch is actually drawn in. Single source for the draw loop and the feed receipt (the receipt used to report
// the requested mode, so a mode-1 patch that fell back showed as 0 or 2 inconsistently with what was drawn).
// TERRAIN-HIRES-NEAR-1: mode 1 (LOD0) draws a 4x-density mesh whose local offsets are in bake (fine) units; uint16 indices cap it
// at 255x255 verts, else it falls back to mode 2. No live bake -> mode 0.
static int32_t resolveVisualMode(const Mc2TerrainTier1Patch& pr, bool bakeLive) {
    int32_t mode = bakeLive ? pr.visualMode : 0;
    if (mode == 1 && (pr.lodStep != 1 || pr.quadCountX * 4 + 1 > 255 || pr.quadCountY * 4 + 1 > 255)) mode = 2;
    return mode;
}

extern "C" int mc2_vk_terrain_island_enabled() {
    return 1;
}

extern "C" int mc2_vk_terrain_all_lods_enabled() {
    return terrainAllLodsRequested() ? 1 : 0;
}

extern "C" int mc2_vk_terrain_skirts_enabled() {
    return terrainSkirtsRequested() ? 1 : 0;
}


// ISLAND-READY predicate: true only when the island is in a state where run() is expected to write this frame's LOD0 patches
// (born, inited, not latched off by a per-frame error, SPIR-V and pipeline present). It does NOT birth the island (prepare does).
extern "C" int mc2_vk_terrain_island_ready() {
    const TerrainIsland* island = parkedIsland();
    if (!island) return 0;                   // not born yet
    if (island->disabled) return 0;          // latched off by a per-frame error
    if (!island->inited) return 0;
    if (!island->vert || !island->frag) return 0; // spv missing
    if (!island->pipe) return 0;                  // pipeline missing
    return 1;
}

// Birth/(re)init the island for the given scene target size, independent of any per-frame feed. Returns true when the parked island
// is live (inited, not disabled). Factored out of run() so the LOD loop can prepare the island before deciding to skip LOD0.
// Init failure follows vk_island_fail_policy.h: not ready yet -> retry next frame; a build failure -> fatal (one transient retry).
bool ensure_island_ready(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const TerrainIsland* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) slotCtx.retireIsland(mc2vk::IslandSlot::Terrain);
    if (!islandSlot()) {
        auto fresh = std::make_unique<TerrainIsland>();
        (void)mc2vk::takeLastCreateFailure();
        if (!init_island(fresh.get(), w, h)) {
            slotCtx.waitIdle("terrain_init_failure");   // objects made before the failure may have been submitted
            fresh.reset();
            mc2vk::onIslandInitFailure(mc2vk::IslandKind::Terrain, g_terrainInitReachedBuild != 0, mc2vk::takeLastCreateFailure(),
                                       g_health.fallbackReason);   // fatal does not return
            return false;
        }
        mc2vk::islandInitFailures(mc2vk::IslandKind::Terrain).reset();
        islandSlot() = std::move(fresh);
    }
    const TerrainIsland* island = parkedIsland();
    return island && island->inited && !island->disabled;
}

// Prepare: the LOD loop calls this at loop-top so the island is born BEFORE the skip decision. Returns 1 iff the island is ready
// to write this frame.
extern "C" int mc2_vk_terrain_island_prepare(int w, int h) {
    return ensure_island_ready(w, h) ? 1 : 0;
}

extern "C" int mc2_vk_terrain_island_prewarm(int w, int h) {
    if (!ensure_island_ready(w, h)) return 0;
    Mc2TerrainTier1Feed feed{};
    if (!mc2_terrain_tier1_get_load_feed(&feed)) return 0;
    auto* s = parkedIsland();
    if (!snapshot_height(s, feed)) return 0;
    import_colormap(s, feed);
    import_cement(s, feed);
    snapshot_cement(s, feed);
    import_shore_mask(s, feed);
    import_materials(s);
    import_concrete_detail(s);
    import_matnormal(s, feed);
    if (feed.visualHeightCpu) snapshot_visual_height(s, feed);
    return 1;
}

// VK-PARITY-TERRAIN-2: stage this frame's light pool + grid into slot [par] (same
// per-frame cadence as GL's PackAndDispatch/BuildAndUploadLightGrid uploads). Any
// staging failure degrades to "no pools" (numPoolLights=0), never a hole.
// (Factored out of mc2_vk_terrain_island_run for VK-PARITY-OVERLAY-1 reuse.)
bool stage_light_pool(TerrainIsland* s, const Mc2TerrainTier1Feed& feed,
                             int32_t& vkPoolLights, int32_t& vkGridDim, mc2vk::FrameAlloc (&lp)[3]) {
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (vkPoolLights > 0 && feed.poolLights && mc2::gate::get<"MC2_LIGHT_GRID">()) {
        const char* failure = nullptr;
        if (s->lightLists.stage(ctx, feed.poolLights, static_cast<uint32_t>(vkPoolLights), feed.lightPoolMoverScale, lp, failure)) {
            vkGridDim = mc2vk::LightLists::kDim;
            return true;
        }
        // Local lighting is optional; the accelerator owns retry/backoff and diagnostics.
        vkGridDim = 0;
    }
    {
        ZoneScopedN("VKIsl.Terrain.LightPoolStage");
        auto stage = [&](int k, const void* src, VkDeviceSize bytes) -> bool {
            if (!src || bytes == 0) return true;
            lp[k] = ctx.frameAlloc(bytes);
            if (!lp[k]) return false;
            std::memcpy(lp[k].ptr, src, static_cast<size_t>(bytes));
            return true;
        };
        if (vkPoolLights > 0 && feed.poolLights) {
            if (!stage(0, feed.poolLights, static_cast<VkDeviceSize>(vkPoolLights) * 48u)) vkPoolLights = 0;
        } else {
            vkPoolLights = 0;
        }
        if (vkPoolLights > 0 && vkGridDim > 0 && feed.lightGridHeader && feed.lightGridIndices &&
            feed.lightGridHeaderWords >= static_cast<uint32_t>(vkGridDim) * static_cast<uint32_t>(vkGridDim) * 2u) {
            if (!stage(1, feed.lightGridHeader,  static_cast<VkDeviceSize>(feed.lightGridHeaderWords) * 4u) ||
                !stage(2, feed.lightGridIndices, static_cast<VkDeviceSize>(feed.lightGridIndexWords)  * 4u))
                vkGridDim = 0;   // linear loop over the staged pool (exact, slower)
        } else {
            vkGridDim = 0;
        }
    }
    // Bindings 11-13 are always bound: what this frame did not stage is a small placeholder (the shader reads them only when numPoolLights / gridDim say so).
    for (int k = 0; k < 3; ++k)
        if (!lp[k]) lp[k] = ctx.frameAlloc(256);
    return lp[0] && lp[1] && lp[2];
}

// Fill one TerrainFrameUBO slot from the frame feed + one patch record (shared MVP /
// lighting / shadow knobs; per-patch blockOrigin/lod fields). Factored out of
// mc2_vk_terrain_island_run for VK-PARITY-OVERLAY-1 reuse (the overlay pass fills slot 0).
void fill_frame_ubo(TerrainIsland* s, const Mc2TerrainTier1Feed& feed,
                           const Mc2TerrainTier1Patch& patch,
                           int32_t vkPoolLights, int32_t vkGridDim, TerrainFrameUBO& u) {
    std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
    u.mapSide      = feed.mapSide;
    u.halfMap      = feed.halfMap;
    u.blockOriginX = patch.blockOriginX;
    u.blockOriginY = patch.blockOriginY;
    u.atlasTopLeftX          = feed.atlasTopLeftX;
    u.atlasTopLeftY          = feed.atlasTopLeftY;
    u.atlasOneOverWorldUnits = feed.atlasOneOverWorldUnits;
    u.colormapReady          = s->colormapReady ? (s->colormapBreakup ? 2 : 1) : 0;
    // CEMENT: only enable the FS cement branch when the GL gate is on AND both
    // the VK cement atlas image and the cement-word snapshot are live (fail-soft
    // if only one of them is ready -> colormap base, no hole).
    u.useCement         = (feed.useCement && s->cementReady && s->cementWordsReady) ? 1 : 0;
    u.cementLayerCount  = feed.cementLayerCount;
    u.cementWUPT        = feed.cementWUPT;
    u.useTransitionMask = feed.useTransitionMask;
    u.cementDiagConnect = feed.cementDiagConnect;
    // VK-TERRAIN-LIGHTING: sun dir + static light-space matrix + toggles. The
    // shadow is only sampled when the gate is on AND a real GL map was copied
    // this frame (s->shadowReady) — else enableShadows=0 -> lit-only, no hole.
    u.terrainLightDir[0] = feed.terrainLightDir[0];
    u.terrainLightDir[1] = feed.terrainLightDir[1];
    u.terrainLightDir[2] = feed.terrainLightDir[2];
    u.terrainLightDir[3] = 0.0f;
    std::memcpy(u.staticLightSpaceMatrix, feed.staticLightSpaceMatrix,
                sizeof(u.staticLightSpaceMatrix));
    mc2_vk_shadow_island_get_object_matrix(u.staticLightSpaceMatrix);
    u.lightingEnabled = feed.lightingEnabled ? 1 : 0;
    u.enableShadows   = (feed.lightingEnabled && feed.shadowsEnabled && s->shadowReady) ? 1 : 0;
    u.shadowSoftness  = feed.shadowSoftness;
    // VK-TERRAIN-DETAIL: normal perturbation. detailReady is only 1 once the GL
    // matNormalArray actually imported (else the FS uses the macro-slope-only
    // normal — still a big win over flat-up, and never a hole).
    // 24 EUs: skip POM, triplanar and detail samples rather than sampling dummy normals.
    const bool detail = !mc2::options::lowCost();
    u.detailEnabled  = (detail && feed.lightingEnabled && feed.detailEnabled) ? 1 : 0;
    // 0: not imported, 1: imported, 2: imported and carrying the cliff layer (layer 5), which the cliff material samples.
    u.detailReady    = !s->matNormalReady ? 0 : (s->matNormalLayers > 5u ? 2 : 1);
    u.nfhStrength    = feed.nfhStrength;
    u.concreteDetail = (detail && !mc2::options::current().stockOverlayConcrete && s->concreteDetailReady && feed.lightingEnabled && feed.detailEnabled) ? 1 : 0;
    if (u.concreteDetail && s->cliffRockPresent) u.concreteDetail |= 2;   // TERRAIN-CLIFF-ROCK (bit 2: rock layers 2/3 bound)
    u.concreteDetailStrength = 0.15f;   // normal gain over the cooked 2K sets; tuned in a viewed RTS-zoom montage
    u.detailTiling   = feed.detailTiling;
    u.detailStrength = feed.detailStrength;
    // VK-TERRAIN-LODFADE: camera pos + gate, precondition-chained on detail
    // being enabled (fading an already-off detail term is a no-op).
    u.cameraPosMC2[0] = feed.cameraPosMC2[0];
    u.cameraPosMC2[1] = feed.cameraPosMC2[1];
    u.cameraPosMC2[2] = feed.cameraPosMC2[2];
    u.cameraPosMC2[3] = 0.0f;
    u.lodFadeEnabled  = (u.detailEnabled && feed.lodFadeEnabled) ? 1 : 0;
    // VK-TERRAIN-SKIRT (rung 4): per-patch skirt pull-down depth (0 = this
    // patch's skirt sub-gate was off at capture, or GL reported no skirt for
    // this block -- island draws no skirt strips for it, see the draw loop).
    u.skirtDepth = patch.skirtDepth;
    static const int kCsmDebugMode = std::atoi(mc2::gate::str<"MC2_SHADOW_DEBUG_MODE">().c_str());   // vk-present-instance 8: parsed once, not per patch
    u.csmDebugMode=kCsmDebugMode;
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: mode-2 far-band Z-swap. Per-patch
    // visualMode (0 or 2, from the GL capture site) is only honored when the
    // bake snapshot is live AND the side is sane -- else forced 0 (VS draws
    // undisplaced; fail-soft, never a hole).
    u.visualSide        = feed.visualSide;
    u.visualDisplaceFar = feed.visualDisplaceFar;
    // TERRAIN-HIRES-NEAR-1: frame-level flag for the frag (fine shading normal from the bake).
    u.visualMode        = (s->visualHeightReady && feed.visualSide > 1) ? 2 : 0;
    u.edgeStitch        = static_cast<int32_t>(patch.edgeStitch);
    u.quadCountX        = patch.quadCountX;
    u.quadCountY        = patch.quadCountY;
    // VK-PARITY-TERRAIN-1: values pulled from the shared GL-side producers.
    u.ambientFloor      = feed.ambientFloor;
    u.ambientDayGate    = feed.ambientDayGate;
    {
        // The flat light only tints cement (pads, roads, runways). Keep its brightness but take out 60% of
        // its hue: a low orange sun (mc2_14) read as warm-tinted concrete at night. Grey suns are unchanged.
        const float* f = feed.missionFlatLight;
        const float lum = 0.2126f * f[0] + 0.7152f * f[1] + 0.0722f * f[2];
        u.missionFlatR = lum + 0.4f * (f[0] - lum);
        u.missionFlatG = lum + 0.4f * (f[1] - lum);
        u.missionFlatB = lum + 0.4f * (f[2] - lum);
    }
    // VK-PARITY-TERRAIN-2: pool/grid/pad-glow knobs from the shared producers.
    u.numPoolLights       = vkPoolLights;
    // Low 8 / Potato 4 lights per grid cell (the prop pass uses the same caps); High/Medium walk the whole cell.
    u._lightPad1          = mc2::options::lowCost() ? 1 : 0;
    u._lightPad0          = mc2::options::lowCost() ? (mc2::options::current().baseQuality == 0 ? 4 : 8) : 0;
    u.lightGridDim        = vkGridDim;
    u.lightGridMinX       = feed.lightGridMin[0];
    u.lightGridMinY       = feed.lightGridMin[1];
    u.lightGridInvCellX   = feed.lightGridInvCell[0];
    u.lightGridInvCellY   = feed.lightGridInvCell[1];
    if (vkPoolLights > 0 && mc2::gate::get<"MC2_LIGHT_GRID">()) {
        u.lightGridMinX = s->lightLists.minInv[0]; u.lightGridMinY = s->lightLists.minInv[1];
        u.lightGridInvCellX = s->lightLists.minInv[2]; u.lightGridInvCellY = s->lightLists.minInv[3];
    }
    u.lightPoolSoft       = feed.lightPoolSoft;
    u.lightPoolMoverDim   = feed.lightPoolMoverDim;
    u.lightPoolMoverScale = feed.lightPoolMoverScale;
    u.lightPoolMoverMax   = feed.lightPoolMoverMax;
    u.lightPoolCompress   = feed.lightPoolCompress;
    u.moverContactShadow  = feed.moverContactShadow;
    u.padGlowR            = feed.padGlow[0];
    u.padGlowG            = feed.padGlow[1];
    u.padGlowB            = feed.padGlow[2];
    u.padGlowSpill        = feed.padGlowSpill;
    // VK-PARITY-SHADOW-1: same knob values GL injects as MC2_SHADOW_* defines.
    u.shadowDistFadeWu    = feed.shadowDistFadeWu;
    u.shadowDistFadeMax   = feed.shadowDistFadeMax;
    u.shadowMinSunSin     = feed.shadowMinSunSin;
    u.shadowKnobFlags     = feed.shadowKnobFlags;
    // VK-PARITY-F3: legacy material tint (GL default-ON) from the shared producers.
    std::memcpy(u.tintRock,     feed.tintRock,     sizeof(u.tintRock));
    std::memcpy(u.tintGrass,    feed.tintGrass,    sizeof(u.tintGrass));
    std::memcpy(u.tintDirt,     feed.tintDirt,     sizeof(u.tintDirt));
    std::memcpy(u.tintConcrete, feed.tintConcrete, sizeof(u.tintConcrete));
    std::memcpy(u.tintSnow,     feed.tintSnow,     sizeof(u.tintSnow));
    u.tintStrengthScale    = feed.tintStrengthScale;
    u.snowBrightnessDampen = feed.snowBrightnessDampen;
    u.edgeFeatherStrength  = feed.edgeFeatherStrength;
    u.shorelineOn           = feed.shorelineOn ? 1 : 0;
    u.shorelineHasMask      = s->shoreMaskReady ? 1 : 0;
    u.shorelineWaterElev    = feed.shorelineWaterElev;
    u.shorelineTime         = feed.shorelineTime;
    u.shorelineStrength     = feed.shorelineStrength;
    u.shorelineFoamStrength = feed.shorelineFoamStrength;
    u.shorelineWetRun       = feed.shorelineWetRun;
    u.shorelineFoamRun      = feed.shorelineFoamRun;
    u.shorelineEdgeJitter   = feed.shorelineEdgeJitter;
    u.shorelineBoundsX = feed.shorelineBounds[0]; u.shorelineBoundsY = feed.shorelineBounds[1];
    u.shorelineBoundsW = feed.shorelineBounds[2]; u.shorelineBoundsH = feed.shorelineBounds[3];
    // SHORE-FIELD-1: coastal colormap follows the water's smooth shore field.
    u.shoreFieldOn = 0; // coast-color repair is baked on the CPU
    u.shoreFieldIsSdf = feed.shoreFieldIsSdf;
    u.shoreFieldBoundsX = feed.shoreFieldBounds[0]; u.shoreFieldBoundsY = feed.shoreFieldBounds[1];
    u.shoreFieldBoundsW = feed.shoreFieldBounds[2]; u.shoreFieldBoundsH = feed.shoreFieldBounds[3];
}

// GPU-FRONTIER-1: pure command recording for the terrain pass. Mesh lookups, the distance sort and the dynamic offsets are prepared by
// mc2_vk_terrain_island_run on the main thread; this touches only the command buffer and the island timers.
struct TerrainDraw { TerrainPatchConstants patch; const PatchMesh* pm; const SkirtMesh* sm; uint32_t skirtMask; };
struct TerrainCmd { uint32_t indexCount, instanceCount, firstIndex; int32_t baseVertex; uint32_t firstInstance; };
struct TerrainRun { const void* mesh; uint32_t firstRec, count; };
// H8: the island's cached terrain records. `cache` re-resolves only changed patches and bumps its generation only when the ordered
// draws change; recs/cmds/runs are rebuilt on a new generation and each engine slot re-uploads only when its copy is older.
struct TerrainRecordState {
    mc2::terrain::RecordCache<Mc2TerrainTier1Patch, TerrainDraw> cache;
    uint64_t builtGen = UINT64_MAX;
    std::vector<TerrainPatchConstants> recs;           // patches, then skirt edges
    std::vector<TerrainCmd> cmds;                      // same indexing as recs
    std::vector<TerrainRun> patchRuns, skirtRuns;
    uint32_t patchN = 0;
    unsigned long skirtN = 0;
    uint64_t uploadedGen[mc2vk::kMaxFramesInFlight] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
};
// The scene pass: CLEAR+STORE on color0, normal and depth (+ stencil), clear values as the GL frame-entry clear plus clearGBuffer1 sentinel
// (FAR-BAND-BLACK-1: terrain is the first writer of scene depth, so it clears to reverse-Z far instead of loading). The memory barrier is the
// external dependency of the VkRenderPass this replaced (previous frame's attachment writes -> this pass's attachment reads/writes).
static void terrain_begin_scene_pass(mc2vk::VulkanContext& ctx, VkCommandBuffer cb, int w, int h, const float (&clearColor)[4], VkImageView colorView,
                                     VkImageView normalView, VkImageView depthView, VkFormat depthFormat) {
    namespace dr = mc2vk::dynrender;
    VkClearValue clears[3]{};
    for (int k = 0; k < 4; ++k) clears[0].color.float32[k] = clearColor[k];
    clears[1].color.float32[0] = 0.5f; clears[1].color.float32[1] = 0.5f; clears[1].color.float32[2] = 1.0f; clears[1].color.float32[3] = 0.0f;
    clears[2].depthStencil = {0.0f, 0u};
    dr::memoryBarrier(ctx, cb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    dr::Pass p;
    p.area = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    p.colorCount = 2;
    p.color[0] = dr::attachment(colorView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                VK_ATTACHMENT_STORE_OP_STORE, &clears[0]);
    p.color[1] = dr::attachment(normalView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                VK_ATTACHMENT_STORE_OP_STORE, &clears[1]);
    p.setDepth(dr::attachment(depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                              VK_ATTACHMENT_STORE_OP_STORE, &clears[2]), depthFormat);
    dr::begin(ctx, cb, p);
}
static void terrain_begin_scene_pass(TerrainIsland* s, mc2vk::VulkanContext& ctx, VkCommandBuffer cb, int w, int h) {
    terrain_begin_scene_pass(ctx, cb, w, h, g_prefixClearColor, s->colorRes->view.get(), s->normalRes->view.get(), s->depthRes->view.get(),
                             s->depthRes->format);
}

struct TerrainRecPlan {
    bool embedded = false;
    struct Run { VkBuffer vb, ib; uint32_t firstRec, count; };
    mc2vk::VulkanContext* ctx = nullptr;   // const entry-point table only (cmdBeginRendering/cmdEndRendering/cmdPipelineBarrier2)
    VkImageView colorView = VK_NULL_HANDLE, normalView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE; VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipeline pipe = VK_NULL_HANDLE, skirtPipe = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE; uint32_t dynOffs[4]{};
    int w = 0, h = 0; float jx = 0, jy = 0, clearColor[4]{};
    VkBuffer drawBuf = VK_NULL_HANDLE; VkDeviceAddress recBase = 0; VkDeviceSize cmdBase = 0;
    std::vector<Run> patchRuns, skirtRuns;
    mc2vk::LightLists::Pending lightLists;
    VkQueryPool timerPool = VK_NULL_HANDLE; uint32_t timerCount = 0;
};
// Pure vkCmd half of the terrain pass; runs on a Render-pool worker via VulkanContext::submitRecorded.
static bool record_terrain_cmds(const TerrainRecPlan& p, VkCommandBuffer cb) {
    ZoneScopedN("VKIsl.Terrain.Record");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!p.embedded && vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    if (p.lightLists) {
        MC2_VK_GPU_SCOPE(listsScope, cb, "VKIsl.LightLists");
        p.lightLists.recordCmds(cb);
    }
    if (!p.embedded) vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_TerrainChunk);

    // VK-DRAW-PERF-1: reset the query pool (outside the render pass) + stamp pass-start.
    mc2vkts::GpuTimer::cmdReset(cb, p.timerPool, p.timerCount);
    mc2vkts::GpuTimer::cmdWrite(cb, p.timerPool, p.timerCount, 0);

    // FAR-BAND-BLACK-1 FIX: attachment 2 clears to reverse-Z far. The native prefix
    // clear path also clears color0 and normal here, matching the GL frame-entry
    // clear plus clearGBuffer1 sentinel before terrain draws into the shared GBuffer.
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Terrain");
    terrain_begin_scene_pass(*p.ctx, cb, p.w, p.h, p.clearColor, p.colorView, p.normalView, p.depthView, p.depthFormat);

    VkViewport vp{p.jx, p.jy, static_cast<float>(p.w), static_cast<float>(p.h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(p.w), static_cast<uint32_t>(p.h)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipe);

    // VK-DRAW-PERF-1: stamp draws-start (gap 0->1 = setup + clear/load).
    mc2vkts::GpuTimer::cmdWrite(cb, p.timerPool, p.timerCount, 1);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.playout, 0, 1, &p.set, 4, p.dynOffs);

    // One record (TerrainPatchConstants, read through buffer device address by gl_DrawID) and one VkDrawIndexedIndirectCommand per draw; a run of
    // consecutive draws that share a mesh is one vkCmdDrawIndexedIndirect. All patches draw first, then all skirt strips.
    const auto drawRuns = [&](const std::vector<TerrainRecPlan::Run>& runs) {
        for (const TerrainRecPlan::Run& r : runs) {
            const VkDeviceSize vboOff = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &r.vb, &vboOff);
            vkCmdBindIndexBuffer(cb, r.ib, 0, VK_INDEX_TYPE_UINT16);
            for (uint32_t done = 0; done < r.count;) {
                const uint32_t n = std::min<uint32_t>(r.count - done, 65535u);   // multiDrawIndirect guarantees at least 65535 per call
                const uint64_t addr = p.recBase + static_cast<uint64_t>(r.firstRec + done) * sizeof(TerrainPatchConstants);
                vkCmdPushConstants(cb, p.playout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(addr), &addr);
                mc2::battlebench::add(mc2::battlebench::DrawTerrain);
                vkCmdDrawIndexedIndirect(cb, p.drawBuf, p.cmdBase + static_cast<VkDeviceSize>(r.firstRec + done) * sizeof(TerrainCmd), n, sizeof(TerrainCmd));
                done += n;
            }
        }
    };
    {
        MC2_VK_GPU_SCOPE(patchScope, cb, "VKIsl.Terrain.Patches");
        drawRuns(p.patchRuns);
    }
    if (!p.skirtRuns.empty()) {
        MC2_VK_GPU_SCOPE(skirtScope, cb, "VKIsl.Terrain.Skirts");
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.skirtPipe);
        drawRuns(p.skirtRuns);
    }

    mc2vk::dynrender::end(*p.ctx, cb);
    gpuScope.end();
    // VK-DRAW-PERF-1: stamp pass-end (gap 1->2 = all patch/skirt draws).
    mc2vkts::GpuTimer::cmdWrite(cb, p.timerPool, p.timerCount, 2);
    if (!p.embedded) vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_TerrainChunk);
    return p.embedded || vkEndCommandBuffer(cb) == VK_SUCCESS;
}
static bool record_terrain(TerrainIsland* s, mc2vk::VulkanContext& rctx, uint32_t par, int w, int h,
                           const uint32_t (&dynOffs)[4], TerrainRecordState& st,
                           unsigned long& drawnOut, unsigned long& skirtDrawnOut, mc2vk::VulkanContext::RecordFn& recordOut,
                           const mc2::waterplanar::Target* reflected = nullptr) {
    // Prepare half (main): the record cache, the draw-buffer growth and upload, the light-list descriptors and the gpu-timer pool claim. The recording is
    // returned as a callback over copied handles (VulkanContext::submitRecorded): it touches nothing of the island.
    TerrainRecPlan plan;
    plan.lightLists = s->lightLists.claim(rctx, par);
    if (!reflected) { plan.timerCount = s->gpuTimer.count; plan.timerPool = s->gpuTimer.claim(); }
    std::copy_n(g_prefixClearColor, 4, plan.clearColor);
    { float jpx[2]; RenderCore::temporal::sceneJitterPixels(g_mc2RenderFrame, jpx); plan.jx = jpx[0]; plan.jy = jpx[1]; }   // temporal AA: sub-pixel viewport offset, zero with no consumer
    plan.ctx = &rctx; plan.colorView = s->colorRes->view.get(); plan.normalView = s->normalRes->view.get(); plan.depthView = s->depthRes->view.get();
    plan.depthFormat = s->depthRes->format;
    plan.pipe = s->pipe.get(); plan.skirtPipe = s->skirtPipe.get();
    plan.playout = s->playout.get(); plan.set = s->set[par]; plan.w = w; plan.h = h;
    std::copy_n(dynOffs, 4, plan.dynOffs);
    if (reflected) {
        plan.embedded = true;
        plan.colorView = reflected->color; plan.normalView = VK_NULL_HANDLE; plan.depthView = reflected->depth;
        plan.depthFormat = VK_FORMAT_D32_SFLOAT;
        plan.pipe = s->planarPipe.get(); plan.skirtPipe = VK_NULL_HANDLE;
        plan.jx = plan.jy = 0; // spatial reflection, independent of any main-view jitter
        std::fill_n(plan.clearColor, 4, 0.0f); // alpha zero means no reflected geometry (sky fallback)
        plan.timerPool = VK_NULL_HANDLE; plan.timerCount = 0;
    }

    // Opaque patches write depth. Submit the nearest chunks first so the early
    // depth test rejects hidden terrain before running the material shader.
    drawnOut = 0; skirtDrawnOut = 0;
    // One record (TerrainPatchConstants, read through buffer device address by gl_DrawID) and one VkDrawIndexedIndirectCommand per draw. Draws
    // stay in distance order; a run of consecutive draws that share a mesh is one vkCmdDrawIndexedIndirect (the vertex/index buffers are bound
    // per run, the push constant carries the run's first record). All patches draw first, then all skirt strips: they are opaque, depth-tested
    // and write depth, so the order between the two kinds only moves early-Z efficiency.
    // H8: records, commands and runs are rebuilt only when the cached draw sequence changed (a new generation); each engine slot
    // re-uploads only when its copy is older than the cached records.
    if (st.builtGen != st.cache.generation()) {
        const std::vector<TerrainDraw>& draws = st.cache.draws();
        std::vector<const SkirtMesh*> skirtMeshOf;         // per skirt record
        st.recs.clear(); st.cmds.clear(); st.patchRuns.clear(); st.skirtRuns.clear();
        st.recs.reserve(draws.size() * 2); st.cmds.reserve(draws.size() * 2);
        for (const TerrainDraw& d : draws) { st.recs.push_back(d.patch); st.cmds.push_back({d.pm->indexCount, 1, 0, 0, 0}); }
        st.patchN = static_cast<uint32_t>(st.recs.size());
        for (const TerrainDraw& d : draws) {
            if (!d.sm) continue;
            for (int e = 0; e < 4; ++e) {
                if (((d.skirtMask >> e) & 1u) == 0u || d.sm->edgeCount[e] == 0) continue;
                st.recs.push_back(d.patch); st.cmds.push_back({d.sm->edgeCount[e], 1, d.sm->edgeOffset[e], 0, 0});
                skirtMeshOf.push_back(d.sm);
            }
        }
        st.skirtN = static_cast<unsigned long>(skirtMeshOf.size());
        uint32_t i = 0;
        for (const TerrainDraw& d : draws) {
            if (!st.patchRuns.empty() && st.patchRuns.back().mesh == d.pm) ++st.patchRuns.back().count; else st.patchRuns.push_back({d.pm, i, 1});
            ++i;
        }
        for (uint32_t k = 0; k < static_cast<uint32_t>(skirtMeshOf.size()); ++k) {
            if (!st.skirtRuns.empty() && st.skirtRuns.back().mesh == skirtMeshOf[k]) ++st.skirtRuns.back().count;
            else st.skirtRuns.push_back({skirtMeshOf[k], st.patchN + k, 1});
        }
        st.builtGen = st.cache.generation();
    }
    const uint32_t patchN = st.patchN;
    const uint32_t totalN = static_cast<uint32_t>(st.recs.size());
    if (totalN != 0) {
        const VkDeviceSize recBytes = static_cast<VkDeviceSize>(totalN) * sizeof(TerrainPatchConstants);
        const VkDeviceSize need = recBytes + static_cast<VkDeviceSize>(totalN) * sizeof(TerrainCmd);
        if (s->drawBufCap[par] < need || !s->drawBuf[par]) {
            const VkDeviceSize cp = std::max<VkDeviceSize>(need + need / 2, 4096);
            s->drawBuf[par].reset();
            st.uploadedGen[par] = UINT64_MAX;
            if (!make_buffer(s, s->drawBuf[par], cp, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true, "terrain.drawRecs", static_cast<int>(par))) return false;
            s->drawBufCap[par] = cp;
        }
        if (st.uploadedGen[par] != st.builtGen) {
            if (!s->drawBuf[par].upload(st.recs.data(), recBytes, 0) || !s->drawBuf[par].upload(st.cmds.data(), totalN * sizeof(TerrainCmd), recBytes)) return false;
            st.uploadedGen[par] = st.builtGen;
        }
    }
    plan.drawBuf = s->drawBuf[par].get();
    plan.recBase = s->drawBuf[par].deviceAddress();
    plan.cmdBase = static_cast<VkDeviceSize>(totalN) * sizeof(TerrainPatchConstants);
    // One run = one mesh's vertex/index buffers + a record range; the handles are resolved here, so the worker never reads the mesh caches.
    const auto resolveRuns = [](const std::vector<TerrainRun>& runs, bool skirt, std::vector<TerrainRecPlan::Run>& out) {
        out.reserve(runs.size());
        for (const TerrainRun& r : runs) {
            const PatchMesh* pm = skirt ? nullptr : static_cast<const PatchMesh*>(r.mesh);
            const SkirtMesh* sm = skirt ? static_cast<const SkirtMesh*>(r.mesh) : nullptr;
            out.push_back({skirt ? sm->vbo.get() : pm->vbo.get(), skirt ? sm->ibo.get() : pm->ibo.get(), r.firstRec, r.count});
        }
    };
    resolveRuns(st.patchRuns, false, plan.patchRuns);
    resolveRuns(st.skirtRuns, true, plan.skirtRuns);
    drawnOut = patchN;
    skirtDrawnOut = st.skirtN;
    static bool s_logged = false;
    if (!s_logged && patchN != 0) {
        s_logged = true;
        vlog("terrain indirect draws (first frame): patches=%u skirtEdges=%lu (was %lu vkCmdDrawIndexed) -> %zu patch runs + %zu skirt runs.",
             patchN, skirtDrawnOut, patchN + skirtDrawnOut, st.patchRuns.size(), st.skirtRuns.size());
    }
    recordOut = [plan = std::move(plan)](VkCommandBuffer cb) -> bool { return record_terrain_cmds(plan, cb); };
    return true;
}

static int run_terrain(int w, int h, TerrainIsland* supplied = nullptr,
                       const Mc2TerrainTier1Feed* reflectedFeed = nullptr, mc2::waterplanar::Target* target = nullptr,
                       mc2vk::VulkanContext::RecordFn* reflectedRecord = nullptr) {
    ZoneScopedN("VK-terrain-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    mc2_hitch2::Scope hitchTotal("terrain.island");
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = terrainPerfGateRequested();
    double us_fence_wait = 0.0;   // deferred retire (beginFrame wait) OR inline wait under the killswitch

    // Pull this frame's LOD0 patch list + shared params from the GL terrain path.
    Mc2TerrainTier1Feed feed{};
    bool gotFeed;
    { ZoneScopedN("VKIsl.Terrain.GetFrameFeed"); mc2_hitch2::Scope hs("terrain.feed");
      if (reflectedFeed) { feed = *reflectedFeed; gotFeed = true; }
      else gotFeed = mc2_terrain_tier1_get_frame_feed(&feed); }
    if (!gotFeed) { setFallback("no_terrain_feed"); return 0; }
    if (!target && mc2vk::rt::scene::demanded()) {   // sample the current surface only when a ray consumer needs it
        mc2vk::rt::scene::TerrainSource rt;
        rt.mapSide = feed.mapSide;
        rt.halfMap = feed.halfMap;
        rt.heights = feed.heightCpu;
        rt.fine = feed.visualSide > 0 ? feed.visualHeightCpu : nullptr;
        rt.visualSide = feed.visualSide;
        rt.displaceFar = feed.visualDisplaceFar;
        rt.heightVersion = feed.heightVersion;
        rt.visualVersion = feed.visualHeightVersion;
        mc2vk::rt::scene::noteTerrain(rt);
    }
    if (!target && (!feed.patches || feed.patchCount == 0)) { setFallback("no_lod0_patches"); return 0; }
    if (feed.mapSide <= 0) { setFallback("bad_mapside"); return 0; }

    bool islandReady;
    { ZoneScopedN("VKIsl.Terrain.EnsureReady"); mc2_hitch2::Scope hs("terrain.ensure_ready"); islandReady = supplied || ensure_island_ready(w, h); }
    if (!islandReady) return 0;
    TerrainIsland* s = supplied ? supplied : parkedIsland();
    if (!s || s->disabled || !s->inited) return 0;

    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    const clk::time_point tStage0 = clk::now();

    // Snapshot the height field (only re-copies when the feed version changes).
    { ZoneScopedN("VK-terrain-interop-height");   // VK-FLIP-FIELD-REPORT-1: GL->VK height copy
    if (!snapshot_height(s, feed)) { setFallback("height_snapshot_failed"); return 0; } }

    // VK-FLIP-FIELD-REPORT-1: atlas + per-frame shadow-map interop copies grouped in
    // one coarse zone (colormap/cement copy-once; static copy-once-per-generation since
    // VK-SHADOW-COPY-ONCE-1; dyn shadow copy-per-frame).
    // VK-TERRAIN-STAGING-PERF-1: per-step sub-timers inside the copies zone, only
    // sampled when MC2_VK_TERRAIN_COPY_STATS is set (same gate as the summary line).
    {
    ZoneScopedN("VK-terrain-interop-copies");
    // COLORMAP-BASE: copy-once import of the GL colormap atlas (no-op once imported;
    // no-op while the atlas isn't built -> FS falls back to olive). Must run while the
    // GL context is current (this entry point is called on the GL/render thread).
    import_colormap(s, feed);

    // CEMENT/ROAD: copy-once import of the GL cement atlas + map-load snapshot of the
    // per-vertex cement-word SSBO. Both no-op once imported / while not ready. Runs on
    // the GL/render thread (GL context current) — same as the colormap/height copies.
    import_cement(s, feed);
    snapshot_cement(s, feed);
    import_shore_mask(s, feed);
    import_materials(s);
    import_concrete_detail(s);
    import_matnormal(s, feed);

    // VK-TERRAIN-VISUALDISPLACE-FAR-1: version-gated mission-load snapshot of the 4x
    // visual-height bake (no-op unless the sub-gate armed the feed fields AND the bake
    // version changed). Fail-soft: not-ready -> UBO visualMode forced 0 (undisplaced).
    if (feed.visualHeightCpu)
        snapshot_visual_height(s, feed);

    bind_static_shadow(s,feed);
    const auto mask=s->staticVisibilityExternalView;
    if (mask!=s->staticVisibilityBoundView) {
        s->staticVisibilityBoundView=mask;
        write_descriptors(s);
    }
    // LOCAL-LIGHTS-2: the local-light shadow atlas appears once per session (binding 19).
    if (const auto atlas=reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_local_atlas_view()); atlas!=s->localShadowBoundView) {
        s->localShadowBoundView=atlas;
        write_descriptors(s);
    }

    } // texture imports and shadow-mask binding

    // CPU staging cost so far (copies + snapshots); the per-parity UBO fill below is
    // added after the beginFrame wait so stageUs never absorbs the deferred retire.
    const double usStagePre = usSince(tStage0);

    // Per-frame data goes into the engine frame ring BEFORE beginFrame (a ring failure after it could strand the fence; the ring slot belongs to the
    // frame, not to the parity, so staging here races nothing): the per-patch dynamic UBO and the light pool + grid spans.
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const uint32_t patchCount = feed.patchCount;
    const mc2vk::FrameAlloc uboSpan = rctx.frameAlloc(sizeof(TerrainFrameUBO));
    int32_t vkPoolLights = feed.numPoolLights;
    int32_t vkGridDim    = feed.lightGridDim;
    mc2vk::FrameAlloc lpSpans[3];
    if (!uboSpan || !stage_light_pool(s, feed, vkPoolLights, vkGridDim, lpSpans)) { setFallback("frame_ring_alloc_failed"); return 0; }

    // VK-TERRAIN-FENCE-DEFER-1 (water-pilot template): begin the shared-slot frame
    // BEFORE the per-frame UBO staging. The slot-fence wait inside beginFrame IS the
    // deferred retire of the submit that last used this parity slot, and it protects
    // everything indexed [par] below: the dynamic UBO, the descriptor set, and the
    // command buffer.
    const clk::time_point tFence0 = clk::now();
    // GPU-FRONTIER-1: engine frame slot; the wait covers the previous terrain run AND the previous overlay at this slot (both draw through set[par],
    // which is rewritten below). The scene clear and the overlay keep their own tickets.
    const uint32_t par = rctx.frameContext().slot;
    // Both submits signal the graphics timeline: the later ticket covers the earlier one, including repeated calls in this frame.
    const auto slotTicket = mc2vk::GpuTicket::both(s->submitted[par], s->ovlSubmitted[par]);
    { ZoneScopedN("VKIsl.Terrain.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);   // VK-ISLAND-TRACY-1: deferred retire of the slot
      if (!vk_island_util::waitStrike(s->waitStrikes, s->disabled,
              rctx.wait(slotTicket, 5000000000ull, "terrain submission slot"), "VK_TERRAIN_ISLAND")) {
          setFallback("submission_slot_timeout"); vlog("[VK_TERRAIN_ISLAND] LOUD: submission slot wait timed out (5 s); skipping this frame, will retry next frame."); return 0;
      } }
    us_fence_wait = usSince(tFence0);
    refresh_biased_samplers(s);
    s->setBoundFrame[par] = rctx.frameContext().frame;
    bool moverBound;
    { ZoneScopedN("VKIsl.Terrain.BindMoverShadow"); mc2_hitch2::Scope hs("terrain.bind_mover_shadow"); moverBound = mc2vk::bindMoverShadowDescriptors(s->device,s->set[par],&s->moverStamp[par]); }
    if (!moverBound) { setFallback("mover_receiver_unavailable"); s->disabled=true; return 0; }
    if (!bind_environment(s,par)) { setFallback("environment_slice_unavailable"); s->disabled=true; return 0; }
    const std::array<VkImageView,7> views{s->colormapTexture.view(),s->cementTexture.view(),s->shoreMaskTexture.view(),s->matNormalTexture.view(),s->materialTexture.view(),s->macroGradientTexture.view(),s->staticVisibilityDummy.view()};
    if (s->descriptorsDirty[par] || s->boundTextures[par]!=views) {
        ZoneScopedN("VKIsl.Terrain.WriteDescriptors"); mc2_hitch2::Scope hs("terrain.write_descriptors");
        write_descriptors(s,static_cast<int>(par)); s->boundTextures[par]=views;
    }

    const clk::time_point tUboFill0 = clk::now();

    // Common frame data is staged once; only vertex patch constants vary per draw.
    TerrainFrameUBO common{};
    fill_frame_ubo(s, feed, Mc2TerrainTier1Patch{}, vkPoolLights, vkGridDim, common);
    std::memcpy(uboSpan.ptr, &common, sizeof(common));
    sync_ring_bindings(s, par, uboSpan, lpSpans);
    if (mc2_terrain_feed_observation_enabled()) {
        // Preserve the observation boundary's expanded legacy feed for exact A/B.
        std::vector<TerrainFrameUBO> expanded(patchCount, common);
        for (uint32_t i = 0; i < patchCount; ++i) {
            const auto& p = feed.patches[i];
            auto& u = expanded[i];
            u.blockOriginX = p.blockOriginX; u.blockOriginY = p.blockOriginY;
            u.quadCountX = p.quadCountX; u.quadCountY = p.quadCountY;
            u.edgeStitch = static_cast<int32_t>(p.edgeStitch);
            u.visualMode = resolveVisualMode(p, s->visualHeightReady && feed.visualSide > 1);
            u.skirtDepth = p.skirtDepth;
        }
        const uint64_t imported[] = {s->heightVersionCopied, s->cementVersionCopied, s->visualHeightVersionCopied,
            s->colormapVersionImported, s->cementVersionImported, s->shoreMaskVersionImported,
            s->materialSurfaceVersion, s->matNormalVersionImported};
        mc2_terrain_feed_dump_consumed(&feed, g_mc2RenderFrame, w, h, {reinterpret_cast<const uint8_t*>(expanded.data()),
            expanded.size() * sizeof(TerrainFrameUBO)}, vkPoolLights, vkGridDim, imported);
    }
    g_health.stageUs = usStagePre + usSince(tUboFill0);
    g_health.patchRecords = patchCount;

    // ---- Item-4: terrain is the native first owner — no GL->VK semaphore handoff.
    // BeginFrame on color0 only supplies the cb parity + submit fence (CPU ordering);
    // normal/depth need no per-frame sync object (this pass CLEARs and writes them,
    // downstream native islands chain Vulkan-owned). The old hybrid arm
    // (BeginFrame x3 + glInteropSignalToVk x3 + vk2gl signal sems + glInteropWaitFromVk)
    // was deleted. VK-TERRAIN-FENCE-DEFER-1: beginFrame is HOISTED above the UBO fill.
    const clk::time_point tRender0 = clk::now();
    ZoneNamedN(zTerrRec, "VK-terrain-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to fn return)
    // TERRAIN-HIRES-NEAR-DIAG: diagonal-fitted mode-1 meshes are keyed to one bake; a rebake (rare) drops them.
    const bool diagLive = feed.visualHeightCpu && feed.visualSide > 1 && s->visualHeightReady
        && static_cast<uint64_t>(feed.visualHeightBytes) >= static_cast<uint64_t>(feed.visualSide) * feed.visualSide * sizeof(float);
    if (diagLive && s->diagPatchVersion != feed.visualHeightVersion) {
        if (!s->diagPatchCache.empty()) { mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_diag_patch_rebake"); s->diagPatchCache.clear(); }
        s->diagPatchVersion = feed.visualHeightVersion;
    }
    // H8: cached per-patch records. A patch is re-resolved only when its feed record changed or the external key below changed
    // (the inputs resolveVisualMode and the mesh lookups read besides the patch itself); the order is rebuilt only when the camera
    // or a patch changed. (distance, index) is a strict total order, so the result equals a full rebuild.
    if (!s->recordCache) s->recordCache = std::make_shared<TerrainRecordState>();
    TerrainRecordState& rec = *static_cast<TerrainRecordState*>(s->recordCache.get());
    const bool visualReady = s->visualHeightReady && feed.visualSide > 1;
    const uint64_t cacheKey = (visualReady ? 1ull : 0ull) | (diagLive ? 2ull : 0ull)
        | (diagLive ? (static_cast<uint64_t>(feed.visualHeightVersion) << 2) : 0ull);
    {
        ZoneScopedN("VKIsl.Terrain.RecordCache");
        rec.cache.update(std::span<const Mc2TerrainTier1Patch>(feed.patches, patchCount), cacheKey, feed.cameraPosMC2[0], feed.cameraPosMC2[2],
            [&](const Mc2TerrainTier1Patch& pr, TerrainDraw& d) {
                // TERRAIN-HIRES-NEAR-1: mode 1 (LOD0) draws a 4x-density mesh whose local offsets are
                // in bake (fine) units; uint16 indices cap it at 255x255 verts, else fall back to mode 2.
                const int32_t mode = resolveVisualMode(pr, visualReady);
                const int32_t meshQX = mode == 1 ? pr.quadCountX * 4 : pr.quadCountX;
                const int32_t meshQY = mode == 1 ? pr.quadCountY * 4 : pr.quadCountY;
                const PatchMesh* pm = (mode == 1 && diagLive)
                    ? getOrBuildPatch(s, meshQX, meshQY, pr.lodStep, feed.visualHeightCpu, feed.visualSide, pr.blockOriginX, pr.blockOriginY)
                    : getOrBuildPatch(s, meshQX, meshQY, pr.lodStep);
                if (!pm || pm->indexCount == 0) return false;
                d = TerrainDraw{};
                d.patch = TerrainPatchConstants{pr.blockOriginX, pr.blockOriginY, pr.quadCountX, pr.quadCountY,
                    static_cast<int32_t>(pr.edgeStitch), mode, pr.skirtDepth, static_cast<int32_t>(pr.morph)};
                d.pm = pm;
                if (pr.skirtDepth > 0.0f && pr.skirtEdgeMask != 0u) { d.sm = getOrBuildSkirtMesh(s, meshQX, meshQY, pr.lodStep); d.skirtMask = pr.skirtEdgeMask; }
                return true;
            },
            [&](const Mc2TerrainTier1Patch& p) {
                const float x = (p.blockOriginX + p.quadCountX * 0.5f) * 128.0f - feed.halfMap + feed.cameraPosMC2[0];
                const float y = feed.halfMap - (p.blockOriginY + p.quadCountY * 0.5f) * 128.0f - feed.cameraPosMC2[2];
                return x * x + y * y;
            });
    }
    const uint32_t dynOffs[4] = {static_cast<uint32_t>(uboSpan.offset), static_cast<uint32_t>(lpSpans[0].offset),
                                 static_cast<uint32_t>(lpSpans[1].offset), static_cast<uint32_t>(lpSpans[2].offset)};
    if (!target) g_sceneClearedRenderFrame = g_mc2RenderFrame + 1u;   // main-view clear only
    unsigned long drawn = 0, skirtDrawn = 0;
    mc2vk::VulkanContext::RecordFn record;
    if (!record_terrain(s, rctx, par, w, h, dynOffs, rec, drawn, skirtDrawn, record, target)) { setFallback("record_failed"); s->disabled = true; return 0; }
    if (target) {
        target->ground = s->groundFrame;
        *reflectedRecord = std::move(record);
        return 1;
    }

    // Item-4: no GL<->VK semaphores. The recording runs at the next flush on a worker with the frame's other recorded islands; done() runs in reserved
    // order. Downstream native islands (prop/mech/water/post) chain Vulkan-owned resources in the same batch order.
    auto done = [s](bool ok) {
        if (ok) return;
        vk_render_pass_timer::Cancel(gos_render_pass_timer::Pass_TerrainChunk);
        setFallback("record_failed"); s->disabled = true;
    };
    { ZoneScopedN("VKIsl.Terrain.Submit");   // the reservation in the deferred submit batch
      if (rctx.submitRecorded(std::move(record), std::move(done), nullptr) != VK_SUCCESS) return 0; }   // done(false) already disabled the island
    s->submitted[par] = rctx.tracker().now();

    // VK-TERRAIN-FENCE-DEFER-1: the inline retire wait is GONE — the submit's fence
    // (the shared parity-slot fence) is waited at the slot's next
    // vkImageResourceBeginFrame, i.e. the deferred retire that beginFrame above the
    // UBO fill already timed into us_fence_wait. Everything this frame's submit uses
    // is per-parity ([par]), write-once, or rare-write device-idle-guarded, so
    // nothing is rewritten before that wait. GPU-side ordering against downstream
    // islands/post/composite never depended on this CPU wait (same-queue submission
    // order + their command buffers' entry barriers).
    g_health.drawUs = usSince(tRender0);
    // VK-DRAW-PERF-1: resolve() is non-blocking (ring of pools, read >= 2 frames late, no WAIT_BIT).
    s->gpuTimer.resolve();

    // HARD assert: no COLOR-side readback fired on the co-write path (the height
    // snapshot uses glGetBufferSubData on an SSBO — that is NOT a color/tex readback
    // and does not touch these counters).
    const bool readbackFired =
        (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
    if (readbackFired) {
        vlog("FATAL: a color readback fired on the co-write path — failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = drawn + skirtDrawn;  // VK-TERRAIN-SKIRT: include skirt edge-strip draws
    ++g_health.usedVulkan;
    g_health.totalUs = usSince(tTotal0);
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first TIER-1 in-place terrain GBuffer co-write: lod0Patches=%u drawn=%lu skirtDrawn=%lu "
             "(color(MRT0)+normal(MRT1 a=1.0)+depth co-written; no composite).",
             patchCount, drawn, skirtDrawn);
        s_firstOk = true;
    }
    if (perf) {
        // [VK_TERRAIN_PERF] — water-style per-frame us breakdown (MC2_VK_TERRAIN_PERF).
        // record_submit_us = record + submit (+ inline fence wait under the killswitch);
        // fence_wait_us = the deferred retire (beginFrame wait) or the inline wait.
        mc2::fmt::print_err("[VK_TERRAIN_PERF] tier1=1 patches={:d} stage_us={:.1f} record_submit_us={:.1f} fence_wait_us={:.1f} island_total_us={:.1f}\n",
            mc2::fmt::as_u32(patchCount), g_health.stageUs, g_health.drawUs, us_fence_wait, g_health.totalUs);
        std::fflush(stderr);
    }
    return 1;
}

extern "C" int mc2_vk_terrain_island_run(int w, int h) { return run_terrain(w, h); }

namespace mc2::waterplanar {
struct PlanarTerrain : TerrainIsland { TerrainView view; };
bool prepareTerrain(std::unique_ptr<mc2vk::IslandState>& state, Target& target, mc2vk::VulkanContext::RecordFn& record) {
    if (!state) {
        auto fresh = std::make_unique<PlanarTerrain>();
        if (!init_island(fresh.get(), target.w, target.h)) return false;
        state = std::move(fresh);
    }
    auto* s = static_cast<PlanarTerrain*>(state.get());
    if (!prepareTerrainView(s->view, target.mainWorldToClip, target.water)) return false;
    auto feed = s->view.feed();
    feed.shorelineWaterElev = target.water;
    return run_terrain(target.w, target.h, s, &feed, &target, &record) != 0;
}
}

// The shared scene's own clear. The terrain pass is the normal clear (CLEAR loadOps), but it does not run when the frame has no terrain (menu,
// loading, mech bay) or when it declines before recording, and every later island LOADs: without this the composite would show the previous frame.
// `g_sceneClearedRenderFrame` holds the render frame (plus one) of the last clear, so a frame the terrain pass already cleared is not cleared twice.
// Callers: the frame start when the last frame had no terrain, and the point right after the terrain flush (before any prop / mech draw).
extern "C" int mc2_vk_scene_clear(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (g_sceneClearedRenderFrame == g_mc2RenderFrame + 1u) return 1;
    if (!ensure_island_ready(w, h)) return 0;
    TerrainIsland* s = parkedIsland();
    if (!s || s->disabled || !s->inited) return 0;

    mc2vk::VulkanContext& cctx = mc2vk::getOrCreateEngineVulkanContext();
    const uint32_t par = cctx.frameContext().slot;
    if (!vk_island_util::waitStrike(s->waitStrikes, s->disabled, cctx.wait(s->clearSubmitted[par], 5000000000ull, "scene clear slot"), "VK_TERRAIN_ISLAND")) { setFallback("clear_slot_timeout"); vlog("[VK_TERRAIN_ISLAND] LOUD: scene clear slot wait timed out (5 s); skipping this frame, will retry next frame."); return 0; }
    VkCommandBuffer cb = s->cbuf[mc2vk::kMaxFramesInFlight + par];
    vkResetCommandPool(s->device, s->slotPool[mc2vk::kMaxFramesInFlight + par].get(), 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("clear_record_failed"); s->disabled = true; return 0; }

    // OBJECT-ID-1: the terrain island is the first owner of the shared scene images, so it zeroes the object-id image
    // (0 = invalid handle = background) once per frame; the prop island then writes real handles over it (LOAD pass).
    if (const VkImageResource* idRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_OBJECT_ID); idRes && idRes->image) {
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = idRes->image.get();
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        VkClearColorValue zero{}; // uint32 zeros
        vkCmdClearColorImage(cb, idRes->image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &ib.subresourceRange);
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    }

    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.SceneClear");
    terrain_begin_scene_pass(s, cctx, cb, w, h);
    mc2vk::dynrender::end(cctx, cb);
    gpuScope.end();
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("clear_record_failed"); s->disabled = true; return 0; }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("clear_submit_failed"); s->disabled = true; return 0; }
    s->clearSubmitted[par] = cctx.tracker().now();
    g_sceneClearedRenderFrame = g_mc2RenderFrame + 1u;
    return 1;
}

extern "C" void mc2_render_backend_terrain_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* attempted, unsigned long* usedVulkan, const char** fallbackReason,
    unsigned long* vkDraws, int* interopActive, int* vk2glSemActive,
    unsigned long* patchRecords, unsigned long* heightSnapshots,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenReadPixels,
    double* stageUs, double* drawUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (interopActive)    *interopActive    = g_health.interopActive;
    if (vk2glSemActive)   *vk2glSemActive   = g_health.vk2glSemActive;
    if (patchRecords)     *patchRecords     = g_health.patchRecords;
    if (heightSnapshots)  *heightSnapshots  = g_health.heightSnapshots;
    if (forbiddenGetTexImage) *forbiddenGetTexImage = g_forbidden.glGetTexImage;
    if (forbiddenReadPixels)  *forbiddenReadPixels  = g_forbidden.glReadPixels;
    if (stageUs)          *stageUs          = g_health.stageUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

