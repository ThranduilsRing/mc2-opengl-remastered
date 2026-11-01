#include "Gates/gate_cold.h"
// VK terrain island: the extern C entry points: enabled / ready / prepare, the frame UBO and light-pool staging, and the per-frame run.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"
#include "vulkan_mover_shadow.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include <algorithm>
#include <numeric>
#include "../../mclib/battle_bench_sink.h"

#ifdef MC2_VULKAN_ISLAND


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

extern "C" int mc2_vk_terrain_visualdisplace_far_enabled() {
    return terrainVisualDisplaceFarRequested() ? 1 : 0;
}

// FAIL-SOFT ISLAND-READY predicate. True only when the gate is on AND the island is in a
// state where run() is expected to co-write this frame's LOD0 patches. The GL LOD-chunk
// loop gates its SPECULATIVE LOD0-skip on this: if the island can NOT render (SPV/pipeline
// load failed, init failed, device/allocator missing, or a prior transient error latched
// s->disabled), the GL path keeps drawing LOD0 so there is NEVER a terrain hole.
//
// This does NOT birth the island (that happens in run() on the render thread with GL
// context current); if the island is not yet inited it is NOT ready this frame (GL draws
// LOD0), and the first successful run() flips it ready for subsequent frames — a one-frame
// GL-draw-then-VK-takeover with no hole.
extern "C" int mc2_vk_terrain_island_ready() {
    const TerrainIsland* island = parkedIsland();
    if (!island) return 0;                   // not born yet -> GL owns LOD0 this frame
    if (island->disabled) return 0;          // latched off by a prior fatal (spv/pipeline/device)
    if (!island->inited) return 0;
    if (!island->vert || !island->frag) return 0; // spv missing
    if (!island->pipe) return 0;                  // pipeline missing
    return 1;
}

// Birth/(re)init the island for the given scene target size, independent of any
// per-frame feed. Returns true when the parked island is live (inited, not disabled). Must run
// on the GL/render thread (init touches the shared VK context / GL interop). This is
// FACTORED OUT of run() so the GL loop can PREPARE the island BEFORE deciding to skip
// LOD0 — without this the skip would gate on ready, ready would gate on a born island,
// and the island would only be born from a non-empty feed produced by the skip
// (chicken-egg deadlock -> island never births -> LOD0 always GL-drawn).
bool ensure_island_ready(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    // Item-2 boot-level native-or-decline: a prior PERMANENT init failure declined this
    // island for the whole session -> GL owns terrain, no per-frame re-init churn.
    if (g_terrainSessionDeclined) return false;
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const TerrainIsland* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) slotCtx.retireIsland(mc2vk::IslandSlot::Terrain);
    if (!islandSlot()) {
        auto fresh = std::make_unique<TerrainIsland>();
        if (!init_island(fresh.get(), w, h)) {
            const bool permanent = (g_terrainInitReachedBuild != 0);
            slotCtx.waitIdle("terrain_init_failure");   // objects made before the failure may have been submitted
            fresh.reset();
            if (permanent) {
                // Build-phase failure won't recover this session: latch decline + ONE
                // receipt line (vs. logging + re-init'ing every frame). GL renders
                // terrain from frame 0.
                g_terrainSessionDeclined = true;
                vlog("[VK_TERRAIN_ISLAND] boot-level decline: permanent init failure "
                     "(%s) -- GL owns terrain for this session (no per-frame retry).",
                     g_health.fallbackReason);
            } else {
                // Transient readiness gate (VK context / shared GBuffer not born yet):
                // keep item-9 fail-hard-with-retry -- GL draws this frame, retry next.
                // Log once (boot warmup spans a few frames) to avoid per-frame spam.
                static bool s_transientLogged = false;
                if (!s_transientLogged) {
                    vlog("init not ready (%s) — GL fallback (will retry).", g_health.fallbackReason);
                    s_transientLogged = true;
                }
            }
            return false;
        }
        islandSlot() = std::move(fresh);
    }
    const TerrainIsland* island = parkedIsland();
    return island && island->inited && !island->disabled;
}

// FAIL-SOFT prepare: the GL loop calls this at loop-top (GL context current) so the
// island is born BEFORE the skip decision. Returns 1 iff the island is ready to co-write
// this frame -> the GL loop then commits the LOD0 skip. 0 -> GL draws LOD0 (no hole).
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
        if (s->lightLists.stage(ctx, feed.poolLights, uint32_t(vkPoolLights), feed.lightPoolMoverScale, lp, failure)) {
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
    u.csmDebugMode=std::atoi(mc2::gate::str<"MC2_SHADOW_DEBUG_MODE">().c_str());
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: mode-2 far-band Z-swap. Per-patch
    // visualMode (0 or 2, from the GL capture site) is only honored when the
    // bake snapshot is live AND the side is sane -- else forced 0 (VS draws
    // undisplaced; fail-soft, never a hole).
    u.visualSide        = feed.visualSide;
    u.visualDisplaceFar = feed.visualDisplaceFar;
    u.visualMode        = (patch.visualMode == 2
                           && s->visualHeightReady && feed.visualSide > 1) ? 2 : 0;
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
    vkResetCommandBuffer(cb, 0);
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

    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
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
    for (const TerrainDraw& d : draws) {
        vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(d.patch), &d.patch);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
        VkDeviceSize vboOff = 0;
        { const VkBuffer vb = d.pm->vbo.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vb, &vboOff); }
        vkCmdBindIndexBuffer(cb, d.pm->ibo.get(), 0, VK_INDEX_TYPE_UINT16);
        mc2::battlebench::add(mc2::battlebench::DrawTerrain), vkCmdDrawIndexed(cb, d.pm->indexCount, 1, 0, 0, 0);
        ++drawnOut;
        if (d.sm) {
            mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->skirtPipe.get());
            VkDeviceSize svboOff = 0;
            { const VkBuffer vb = d.sm->vbo.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vb, &svboOff); }
            vkCmdBindIndexBuffer(cb, d.sm->ibo.get(), 0, VK_INDEX_TYPE_UINT16);
            for (int e = 0; e < 4; ++e) {
                if (((d.skirtMask >> e) & 1u) == 0u) continue;
                if (d.sm->edgeCount[e] == 0) continue;
                mc2::battlebench::add(mc2::battlebench::DrawTerrain), vkCmdDrawIndexed(cb, d.sm->edgeCount[e], 1, d.sm->edgeOffset[e], 0, 0);
                ++skirtDrawnOut;
            }
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
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = terrainPerfGateRequested();
    double us_fence_wait = 0.0;   // deferred retire (beginFrame wait) OR inline wait under the killswitch

    // Pull this frame's LOD0 patch list + shared params from the GL terrain path.
    Mc2TerrainTier1Feed feed{};
    if (!mc2_terrain_tier1_get_frame_feed(&feed)) { setFallback("no_terrain_feed"); return 0; }
    if (!feed.patches || feed.patchCount == 0) { setFallback("no_lod0_patches"); return 0; }
    if (feed.mapSide <= 0) { setFallback("bad_mapside"); return 0; }

    if (!ensure_island_ready(w, h)) return 0;
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
    if (!mc2vk::bindMoverShadowDescriptors(s->device,s->set[par])) { setFallback("mover_receiver_unavailable"); s->disabled=true; return 0; }
    const std::array<VkImageView,7> views{s->colormapTexture.view(),s->cementTexture.view(),s->shoreMaskTexture.view(),s->matNormalTexture.view(),s->materialTexture.view(),s->macroGradientTexture.view(),s->staticVisibilityDummy.view()};
    if (s->descriptorsDirty[par] || s->boundTextures[par]!=views) {
        write_descriptors(s,int(par)); s->boundTextures[par]=views;
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
    std::vector<uint32_t> drawOrder(patchCount);
    std::iota(drawOrder.begin(), drawOrder.end(), 0u);
    const auto distanceSquared = [&](uint32_t i) {
        const auto& p = feed.patches[i];
        const float x = (p.blockOriginX + p.quadCountX * 0.5f) * 128.0f - feed.halfMap + feed.cameraPosMC2[0];
        const float y = feed.halfMap - (p.blockOriginY + p.quadCountY * 0.5f) * 128.0f - feed.cameraPosMC2[2];
        return x * x + y * y;
    };
    std::sort(drawOrder.begin(), drawOrder.end(), [&](uint32_t a, uint32_t b) {
        const float da = distanceSquared(a), db = distanceSquared(b);
        return da < db || (da == db && a < b);
    });

    std::vector<TerrainDraw> draws;
    draws.reserve(patchCount);
    for (uint32_t i : drawOrder) {
        const Mc2TerrainTier1Patch& pr = feed.patches[i];
        const PatchMesh* pm = getOrBuildPatch(s, pr.quadCountX, pr.quadCountY, pr.lodStep);
        if (!pm || pm->indexCount == 0) continue;
        TerrainDraw d{};
        d.patch = TerrainPatchConstants{pr.blockOriginX, pr.blockOriginY, pr.quadCountX, pr.quadCountY,
            static_cast<int32_t>(pr.edgeStitch),
            (pr.visualMode == 2 && s->visualHeightReady && feed.visualSide > 1) ? 2 : 0, pr.skirtDepth};
        d.pm = pm;
        if (pr.skirtDepth > 0.0f && pr.skirtEdgeMask != 0u) { d.sm = getOrBuildSkirtMesh(s, pr.quadCountX, pr.quadCountY, pr.lodStep); d.skirtMask = pr.skirtEdgeMask; }
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
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("clear_record_failed"); s->disabled = true; return 0; }

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

#endif // MC2_VULKAN_ISLAND
