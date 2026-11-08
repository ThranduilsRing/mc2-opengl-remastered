#include "Gates/gate_cold.h"
// VK terrain island: the extern C entry points: enabled / ready / prepare, the frame UBO and light-pool staging, and the per-frame run.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"
#include "vulkan_mover_shadow.h"
#include "../../RenderCore/TemporalInputs.h"
#include "hitch_stats.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include <algorithm>
#include <numeric>
#include "../../mclib/battle_bench_wait.h"
#include "vk_island_fail_policy.h"



// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern uint32_t g_mc2RenderFrame;   // mclib/tgl.cpp
float mc2SceneLightScale();   // mclib/camera.cpp: LOCAL-LIGHTS-4 mission night scale of the prelit ground
static uint32_t g_sceneClearedRenderFrame = 0;   // render frame + 1 of the last shared-scene clear (0 = never)

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
    u.cementGridSide    = feed.cementGridSide;
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
    u.concreteDetail = (detail && s->concreteDetailReady && feed.lightingEnabled && feed.detailEnabled) ? 1 : 0;
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
    u.sceneLightScale   = mc2SceneLightScale();   // LOCAL-LIGHTS-4 (mclib/camera.cpp mission profile)
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
    u.cementAtlasSeamFix   = feed.cementAtlasSeamFix ? 1 : 0;
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
static bool record_terrain(TerrainIsland* s, mc2vk::VulkanContext& rctx, VkCommandBuffer cb, uint32_t par, int w, int h,
                           const uint32_t (&dynOffs)[4], const std::vector<TerrainDraw>& draws,
                           unsigned long& drawnOut, unsigned long& skirtDrawnOut) {
    vkResetCommandPool(s->device, s->slotPool[par].get(), 0);   // cb is s->cbuf[par]
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    {
        MC2_VK_GPU_SCOPE(listsScope, cb, "VKIsl.LightLists");
        s->lightLists.record(rctx, cb, par);
    }
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_TerrainChunk);

    // VK-DRAW-PERF-1: reset the query pool (outside the render pass) + stamp pass-start.
    s->gpuTimer.reset(cb);
    s->gpuTimer.write(cb, 0);

    // FAR-BAND-BLACK-1 FIX: attachment 2 clears to reverse-Z far. The native prefix
    // clear path also clears color0 and normal here, matching the GL frame-entry
    // clear plus clearGBuffer1 sentinel before terrain draws into the shared GBuffer.
    VkClearValue clears[3]{};
    clears[0].color.float32[0] = g_prefixClearColor[0];
    clears[0].color.float32[1] = g_prefixClearColor[1];
    clears[0].color.float32[2] = g_prefixClearColor[2];
    clears[0].color.float32[3] = g_prefixClearColor[3];
    clears[1].color.float32[0] = 0.5f;
    clears[1].color.float32[1] = 0.5f;
    clears[1].color.float32[2] = 1.0f;
    clears[1].color.float32[3] = 0.0f;
    clears[2].depthStencil = {0.0f, 0u};

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Terrain");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    float jpx[2];
    RenderCore::temporal::sceneJitterPixels(g_mc2RenderFrame, jpx);   // temporal AA: sub-pixel viewport offset, zero with no consumer
    VkViewport vp{jpx[0], jpx[1], static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());

    // VK-DRAW-PERF-1: stamp draws-start (gap 0->1 = setup + clear/load).
    s->gpuTimer.write(cb, 1);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[par], 4, dynOffs);

    // Opaque patches write depth. Submit the nearest chunks first so the early
    // depth test rejects hidden terrain before running the material shader.
    drawnOut = 0; skirtDrawnOut = 0;
    // One record (TerrainPatchConstants, read through buffer device address by gl_DrawID) and one VkDrawIndexedIndirectCommand per draw. Draws
    // stay in distance order; a run of consecutive draws that share a mesh is one vkCmdDrawIndexedIndirect (the vertex/index buffers are bound
    // per run, the push constant carries the run's first record). All patches draw first, then all skirt strips: they are opaque, depth-tested
    // and write depth, so the order between the two kinds only moves early-Z efficiency.
    struct Cmd { uint32_t indexCount, instanceCount, firstIndex; int32_t baseVertex; uint32_t firstInstance; };
    struct Run { const void* mesh; uint32_t firstRec, count; };
    std::vector<TerrainPatchConstants> recs;           // patches, then skirt edges
    std::vector<Cmd> cmds;                             // same indexing as recs
    std::vector<const SkirtMesh*> skirtMeshOf;         // per skirt record
    recs.reserve(draws.size() * 2); cmds.reserve(draws.size() * 2);
    for (const TerrainDraw& d : draws) { recs.push_back(d.patch); cmds.push_back({d.pm->indexCount, 1, 0, 0, 0}); }
    const uint32_t patchN = static_cast<uint32_t>(recs.size());
    for (const TerrainDraw& d : draws) {
        if (!d.sm) continue;
        for (int e = 0; e < 4; ++e) {
            if (((d.skirtMask >> e) & 1u) == 0u || d.sm->edgeCount[e] == 0) continue;
            recs.push_back(d.patch); cmds.push_back({d.sm->edgeCount[e], 1, d.sm->edgeOffset[e], 0, 0});
            skirtMeshOf.push_back(d.sm);
        }
    }
    const uint32_t totalN = static_cast<uint32_t>(recs.size());
    if (totalN != 0) {
        const VkDeviceSize recBytes = static_cast<VkDeviceSize>(totalN) * sizeof(TerrainPatchConstants);
        const VkDeviceSize need = recBytes + static_cast<VkDeviceSize>(totalN) * sizeof(Cmd);
        if (s->drawBufCap[par] < need || !s->drawBuf[par]) {
            const VkDeviceSize cp = std::max<VkDeviceSize>(need + need / 2, 4096);
            s->drawBuf[par].reset();
            if (!make_buffer(s, s->drawBuf[par], cp, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true, "terrain.drawRecs", static_cast<int>(par))) return false;
            s->drawBufCap[par] = cp;
        }
        if (!s->drawBuf[par].upload(recs.data(), recBytes, 0) || !s->drawBuf[par].upload(cmds.data(), totalN * sizeof(Cmd), recBytes)) return false;
    }
    const VkBuffer drawBuf = s->drawBuf[par].get();
    const VkDeviceAddress recBase = s->drawBuf[par].deviceAddress();
    const VkDeviceSize cmdBase = static_cast<VkDeviceSize>(totalN) * sizeof(TerrainPatchConstants);
    auto drawRuns = [&](const std::vector<Run>& runs, bool skirt) {
        for (const Run& r : runs) {
            const PatchMesh* pm = skirt ? nullptr : static_cast<const PatchMesh*>(r.mesh);
            const SkirtMesh* sm = skirt ? static_cast<const SkirtMesh*>(r.mesh) : nullptr;
            const VkBuffer vb = skirt ? sm->vbo.get() : pm->vbo.get();
            const VkBuffer ib = skirt ? sm->ibo.get() : pm->ibo.get();
            const VkDeviceSize vboOff = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &vb, &vboOff);
            vkCmdBindIndexBuffer(cb, ib, 0, VK_INDEX_TYPE_UINT16);
            for (uint32_t done = 0; done < r.count;) {
                const uint32_t n = std::min<uint32_t>(r.count - done, 65535u);   // multiDrawIndirect guarantees at least 65535 per call
                const uint64_t addr = recBase + static_cast<uint64_t>(r.firstRec + done) * sizeof(TerrainPatchConstants);
                vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(addr), &addr);
                mc2::battlebench::add(mc2::battlebench::DrawTerrain);
                vkCmdDrawIndexedIndirect(cb, drawBuf, cmdBase + static_cast<VkDeviceSize>(r.firstRec + done) * sizeof(Cmd), n, sizeof(Cmd));
                done += n;
            }
        }
    };
    {
        std::vector<Run> runs;
        uint32_t i = 0;
        for (const TerrainDraw& d : draws) {
            if (!runs.empty() && runs.back().mesh == d.pm) ++runs.back().count; else runs.push_back({d.pm, i, 1});
            ++i;
        }
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
        drawRuns(runs, false);
        drawnOut = patchN;
        const size_t patchRuns = runs.size();
        runs.clear();
        for (uint32_t k = 0; k < static_cast<uint32_t>(skirtMeshOf.size()); ++k) {
            if (!runs.empty() && runs.back().mesh == skirtMeshOf[k]) ++runs.back().count; else runs.push_back({skirtMeshOf[k], patchN + k, 1});
        }
        if (!runs.empty()) mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->skirtPipe.get());
        drawRuns(runs, true);
        skirtDrawnOut = skirtMeshOf.size();
        static bool s_logged = false;
        if (!s_logged && patchN != 0) {
            s_logged = true;
            vlog("terrain indirect draws (first frame): patches=%u skirtEdges=%lu (was %lu vkCmdDrawIndexed) -> %zu patch runs + %zu skirt runs.",
                 patchN, skirtDrawnOut, patchN + skirtDrawnOut, patchRuns, runs.size());
        }
    }

    vkCmdEndRenderPass(cb);
    gpuScope.end();
    // VK-DRAW-PERF-1: stamp pass-end (gap 1->2 = all patch/skirt draws).
    s->gpuTimer.write(cb, 2);
    vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_TerrainChunk);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
    return true;
}

extern "C" int mc2_vk_terrain_island_run(int w, int h) {
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
    { ZoneScopedN("VKIsl.Terrain.GetFrameFeed"); mc2_hitch2::Scope hs("terrain.feed"); gotFeed = mc2_terrain_tier1_get_frame_feed(&feed); }
    if (!gotFeed) { setFallback("no_terrain_feed"); return 0; }
    if (!feed.patches || feed.patchCount == 0) { setFallback("no_lod0_patches"); return 0; }
    if (feed.mapSide <= 0) { setFallback("bad_mapside"); return 0; }

    bool islandReady;
    { ZoneScopedN("VKIsl.Terrain.EnsureReady"); mc2_hitch2::Scope hs("terrain.ensure_ready"); islandReady = ensure_island_ready(w, h); }
    if (!islandReady) return 0;
    TerrainIsland* s = parkedIsland();
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
    // command buffer. The inline post-submit wait this replaces is kept only under
    // the MC2_VK_TERRAIN_FENCE_DEFER=0 killswitch.
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
    s->setBoundFrame[par] = rctx.frameContext().frame;
    bool moverBound;
    { ZoneScopedN("VKIsl.Terrain.BindMoverShadow"); mc2_hitch2::Scope hs("terrain.bind_mover_shadow"); moverBound = mc2vk::bindMoverShadowDescriptors(s->device,s->set[par],&s->moverStamp[par]); }
    if (!moverBound) { setFallback("mover_receiver_unavailable"); s->disabled=true; return 0; }
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
            u.visualMode = (p.visualMode == 2 && s->visualHeightReady && feed.visualSide > 1) ? 2 : 0;
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
    VkCommandBuffer cb = s->cbuf[par];

    const clk::time_point tRender0 = clk::now();
    ZoneNamedN(zTerrRec, "VK-terrain-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to fn return)
    // Distance keys are computed once per patch (not twice per comparison); (distance, index) is a strict total order, so the result is unchanged.
    static std::vector<std::pair<float, uint32_t>> drawOrder;
    drawOrder.clear();
    drawOrder.reserve(patchCount);
    for (uint32_t i = 0; i < patchCount; ++i) {
        const auto& p = feed.patches[i];
        const float x = (p.blockOriginX + p.quadCountX * 0.5f) * 128.0f - feed.halfMap + feed.cameraPosMC2[0];
        const float y = feed.halfMap - (p.blockOriginY + p.quadCountY * 0.5f) * 128.0f - feed.cameraPosMC2[2];
        drawOrder.emplace_back(x * x + y * y, i);
    }
    std::sort(drawOrder.begin(), drawOrder.end());

    // TERRAIN-HIRES-NEAR-DIAG: diagonal-fitted mode-1 meshes are keyed to one bake; a rebake (rare) drops them.
    const bool diagLive = feed.visualHeightCpu && feed.visualSide > 1 && s->visualHeightReady
        && static_cast<uint64_t>(feed.visualHeightBytes) >= static_cast<uint64_t>(feed.visualSide) * feed.visualSide * sizeof(float);
    if (diagLive && s->diagPatchVersion != feed.visualHeightVersion) {
        if (!s->diagPatchCache.empty()) { mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_diag_patch_rebake"); s->diagPatchCache.clear(); }
        s->diagPatchVersion = feed.visualHeightVersion;
    }
    std::vector<TerrainDraw> draws;
    draws.reserve(patchCount);
    for (const auto& [dist, i] : drawOrder) {
        (void)dist;
        const Mc2TerrainTier1Patch& pr = feed.patches[i];
        // TERRAIN-HIRES-NEAR-1: mode 1 (LOD0) draws a 4x-density mesh whose local offsets are
        // in bake (fine) units; uint16 indices cap it at 255x255 verts, else fall back to mode 2.
        const bool bakeLive = s->visualHeightReady && feed.visualSide > 1;
        int32_t mode = bakeLive ? pr.visualMode : 0;
        if (mode == 1 && (pr.lodStep != 1 || pr.quadCountX * 4 + 1 > 255 || pr.quadCountY * 4 + 1 > 255)) mode = 2;
        const int32_t meshQX = mode == 1 ? pr.quadCountX * 4 : pr.quadCountX;
        const int32_t meshQY = mode == 1 ? pr.quadCountY * 4 : pr.quadCountY;
        const PatchMesh* pm = (mode == 1 && diagLive)
            ? getOrBuildPatch(s, meshQX, meshQY, pr.lodStep, feed.visualHeightCpu, feed.visualSide, pr.blockOriginX, pr.blockOriginY)
            : getOrBuildPatch(s, meshQX, meshQY, pr.lodStep);
        if (!pm || pm->indexCount == 0) continue;
        TerrainDraw d{};
        d.patch = TerrainPatchConstants{pr.blockOriginX, pr.blockOriginY, pr.quadCountX, pr.quadCountY,
            static_cast<int32_t>(pr.edgeStitch), mode, pr.skirtDepth};
        d.pm = pm;
        if (pr.skirtDepth > 0.0f && pr.skirtEdgeMask != 0u) { d.sm = getOrBuildSkirtMesh(s, meshQX, meshQY, pr.lodStep); d.skirtMask = pr.skirtEdgeMask; }
        draws.push_back(d);
    }
    const uint32_t dynOffs[4] = {static_cast<uint32_t>(uboSpan.offset), static_cast<uint32_t>(lpSpans[0].offset),
                                 static_cast<uint32_t>(lpSpans[1].offset), static_cast<uint32_t>(lpSpans[2].offset)};
    g_sceneClearedRenderFrame = g_mc2RenderFrame + 1u;   // the recorded pass CLEARs color0 / normal / depth (see mc2_vk_scene_clear)
    unsigned long drawn = 0, skirtDrawn = 0;
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_TerrainChunk);
    if (!record_terrain(s, rctx, cb, par, w, h, dynOffs, draws, drawn, skirtDrawn)) { setFallback("record_failed"); s->disabled = true; return 0; }

    // Item-4 submit: wait/signal NO GL<->VK semaphores. The fence orders CPU access;
    // downstream native islands (prop/mech/water/post) chain Vulkan-owned resources.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Terrain.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
      if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    s->submitted[par] = rctx.tracker().now();
    timerGuard.submitted();

    // VK-TERRAIN-FENCE-DEFER-1: the inline retire wait is GONE — the submit's fence
    // (the shared parity-slot fence) is waited at the slot's next
    // vkImageResourceBeginFrame, i.e. the deferred retire that beginFrame above the
    // UBO fill already timed into us_fence_wait. Everything this frame's submit uses
    // is per-parity ([par]), write-once, or rare-write device-idle-guarded, so
    // nothing is rewritten before that wait. GPU-side ordering against downstream
    // islands/post/composite never depended on this CPU wait (same-queue submission
    // order + their command buffers' entry barriers).
    // Killswitch MC2_VK_TERRAIN_FENCE_DEFER=0 restores the inline wait.
    if (!terrainFenceDeferEnabled()) {
        ZoneScopedN("VKIsl.Terrain.InlineFenceWait"); mc2::battlebench::WaitScope gpuWait2(mc2::battlebench::WaitGpuMainNs);   // VK-ISLAND-TRACY-1: killswitch-only inline retire
        const clk::time_point tInline0 = clk::now();
        if (!rctx.wait(s->submitted[par], 5000000000ull, "terrain inline retire")) {
            setFallback("fence_timeout"); s->disabled = true; return 0;
        }
        us_fence_wait = usSince(tInline0);
    }
    g_health.drawUs = usSince(tRender0);
    // VK-DRAW-PERF-1 note under the deferred retire: resolve() uses
    // VK_QUERY_RESULT_WAIT_BIT, so with MC2_VK_GPU_TIMESTAMPS armed it blocks until
    // this frame's pass completes (diagnostic-only stall; gate is default-OFF and
    // resolve() is a no-op without it).
    if (s->gpuTimer.supported) rctx.poll();   // resolve() waits on the query results: the deferred batch holding this cb must be on the queue first
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
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        VkClearColorValue zero{}; // uint32 zeros
        vkCmdClearColorImage(cb, idRes->image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &ib.subresourceRange);
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    }

    VkClearValue clears[3]{};
    for (int k = 0; k < 4; ++k) clears[0].color.float32[k] = g_prefixClearColor[k];
    clears[1].color.float32[0] = 0.5f; clears[1].color.float32[1] = 0.5f; clears[1].color.float32[2] = 1.0f; clears[1].color.float32[3] = 0.0f;
    clears[2].depthStencil = {0.0f, 0u};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.SceneClear");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cb);
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

