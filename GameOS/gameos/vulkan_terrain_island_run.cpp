// VK terrain island: the extern C entry points: enabled / ready / prepare, the frame UBO and light-pool staging, and the per-frame run.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"
#include "vk_render_pass_timer.h"
#include "mc2fmt.h"

#ifdef MC2_VULKAN_ISLAND


// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_terrain_island_enabled() {
    return terrainTier1Requested() ? 1 : 0;
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
    if (!terrainTier1Requested()) return 0;
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
    if (!terrainTier1Requested()) return false;
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

// VK-PARITY-TERRAIN-2: stage this frame's light pool + grid into slot [par] (same
// per-frame cadence as GL's PackAndDispatch/BuildAndUploadLightGrid uploads). Any
// staging failure degrades to "no pools" (numPoolLights=0), never a hole.
// (Factored out of mc2_vk_terrain_island_run for VK-PARITY-OVERLAY-1 reuse.)
void stage_light_pool(TerrainIsland* s, uint32_t par, const Mc2TerrainTier1Feed& feed,
                             int32_t& vkPoolLights, int32_t& vkGridDim) {
    {
        ZoneScopedN("VKIsl.Terrain.LightPoolStage");
        auto stage = [&](int k, const void* src, VkDeviceSize bytes) -> bool {
            if (!src || bytes == 0) return true;
            if (!ensure_lightpool_capacity(s, par, k, bytes)) return false;
            void* dst = nullptr;
            if (vmaMapMemory(s->allocator, s->lpBuf[par][k].allocation(), &dst) != VK_SUCCESS) return false;
            std::memcpy(dst, src, (size_t)bytes);
            vmaUnmapMemory(s->allocator, s->lpBuf[par][k].allocation());
            vmaFlushAllocation(s->allocator, s->lpBuf[par][k].allocation(), 0, VK_WHOLE_SIZE);
            return true;
        };
        if (vkPoolLights > 0 && feed.poolLights) {
            if (!stage(0, feed.poolLights, (VkDeviceSize)vkPoolLights * 48u)) vkPoolLights = 0;
        } else {
            vkPoolLights = 0;
        }
        if (vkPoolLights > 0 && vkGridDim > 0 && feed.lightGridHeader && feed.lightGridIndices &&
            feed.lightGridHeaderWords >= (uint32_t)vkGridDim * (uint32_t)vkGridDim * 2u) {
            if (!stage(1, feed.lightGridHeader,  (VkDeviceSize)feed.lightGridHeaderWords * 4u) ||
                !stage(2, feed.lightGridIndices, (VkDeviceSize)feed.lightGridIndexWords  * 4u))
                vkGridDim = 0;   // linear loop over the staged pool (exact, slower)
        } else {
            vkGridDim = 0;
        }
    }
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
    u.colormapReady          = s->colormapReady ? 1 : 0;
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
    u.lightingEnabled = feed.lightingEnabled ? 1 : 0;
    u.enableShadows   = (feed.lightingEnabled && feed.shadowsEnabled && s->shadowReady) ? 1 : 0;
    u.shadowSoftness  = feed.shadowSoftness;
    // VK-TERRAIN-DETAIL: normal perturbation. detailReady is only 1 once the GL
    // matNormalArray actually imported (else the FS uses the macro-slope-only
    // normal — still a big win over flat-up, and never a hole).
    u.detailEnabled  = (feed.lightingEnabled && feed.detailEnabled) ? 1 : 0;
    // 0: not imported, 1: imported, 2: imported and carrying the cliff layer (layer 5), which the cliff material samples.
    u.detailReady    = !s->matNormalReady ? 0 : (s->matNormalLayers > 5u ? 2 : 1);
    u.nfhStrength    = feed.nfhStrength;
    u.detailTiling   = feed.detailTiling;
    u.detailStrength = feed.detailStrength;
    // VK-TERRAIN-DYNSHADOW: enable only when the sub-gate is on AND a real GL
    // dynamic map was copied this frame (s->dynShadowReady). Else 0 -> no
    // dynamic shadow term (byte-identical to the pre-dynshadow frame).
    u.enableDynShadows = (feed.lightingEnabled && feed.dynShadowsEnabled && s->dynShadowReady) ? 1 : 0;
    std::memcpy(u.dynamicLightSpaceMatrix, feed.dynamicLightSpaceMatrix,
                sizeof(u.dynamicLightSpaceMatrix));
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
    // VK-TERRAIN-CSM-NEAR-1: enable only when the gate armed AND both the near
    // array + full-map copies succeeded this frame (s->csmArrayReady/
    // csmFullMapReady). Else 0 -> frag falls back to the legacy single-map
    // dynamic-shadow path (u.enableDynShadows above), never a hole.
    u.csmEnabled = (feed.lightingEnabled && feed.csmEnabled
                    && s->csmArrayReady && s->csmFullMapReady) ? 1 : 0;
    u.csmCount     = feed.csmCount;
    u.csmDepthSpan = feed.csmDepthSpan;
    // CSM-BLACKOUT-RECON-1: one-shot debug viz, env-gated (no perf/behavior
    // impact when unset). See MC2_VK_TERRAIN_CSM_DEBUG in mc2vkgate.
    {
        static const int dbgMode = mc2::gate::get<"MC2_VK_TERRAIN_CSM_DEBUG">();
        u.csmDebugMode = dbgMode;
    }
    std::memcpy(u.csmCascadeMatrices, feed.csmCascadeMatrices, sizeof(u.csmCascadeMatrices));
    u.csmCascadeTexelWorld[0] = feed.csmCascadeTexelWorld[0];
    u.csmCascadeTexelWorld[1] = feed.csmCascadeTexelWorld[1];
    u.csmCascadeTexelWorld[2] = feed.csmCascadeTexelWorld[2];
    u.csmCascadeTexelWorld[3] = 0.0f;
    u.csmFullMapTexelWorld = feed.csmFullMapTexelWorld;
    if (mc2_vk_shadow_matrix_diag_enabled() && mc2_vk_shadow_matrix_diag_take_terrain_arm()) {
        // MC2_VK_SHADOW_MATRIX_DIAG: exactly what terrain_tier1.frag samples with (std140 default
        // column-major mat4, no row_major qualifier; raw memcpy of the GL arrays).
        auto pm = [](const char* tag, const float* m, int n) {
            mc2::fmt::print_err("[VK_SHADOW_MATRIX v1] {} = [", mc2::fmt::safe(tag));
            for (int i = 0; i < n; ++i) mc2::fmt::print_err("{}{:.6g}", mc2::fmt::safe(i ? "," : ""), m[i]);
            mc2::fmt::print_err("]\n");
        };
        mc2::fmt::print_err("[VK_SHADOW_MATRIX v1] TERRAIN UBO csmEnabled={:d} csmCount={:d} enableDynShadows={:d} csmDepthSpan={:.6g} csmFullMapTexelWorld={:.6g} csmCascadeTexelWorld={:.6g},{:.6g},{:.6g} feed.csmArraySize={:d} feed.csmFullMapSize={:d} feed.dynShadowSize={:d}\n", mc2::fmt::as_i32((int)u.csmEnabled), mc2::fmt::as_i32((int)u.csmCount), mc2::fmt::as_i32((int)u.enableDynShadows), u.csmDepthSpan, u.csmFullMapTexelWorld, u.csmCascadeTexelWorld[0], u.csmCascadeTexelWorld[1], u.csmCascadeTexelWorld[2], mc2::fmt::as_i32((int)feed.csmArraySize), mc2::fmt::as_i32((int)feed.csmFullMapSize), mc2::fmt::as_i32((int)feed.dynamicShadowSize));
        pm("TERRAIN UBO dynamicLightSpaceMatrix", u.dynamicLightSpaceMatrix, 16);
        for (int i = 0; i < 3; ++i) {
            char tg[64]; mc2::fmt::to_buf(tg, "TERRAIN UBO csmCascadeMatrices[{:d}]", mc2::fmt::as_i32(i));
            pm(tg, u.csmCascadeMatrices + i * 16, 16);
        }
        std::fflush(stderr);
    }
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: mode-2 far-band Z-swap. Per-patch
    // visualMode (0 or 2, from the GL capture site) is only honored when the
    // bake snapshot is live AND the side is sane -- else forced 0 (VS draws
    // undisplaced; fail-soft, never a hole).
    u.visualSide        = feed.visualSide;
    u.visualDisplaceFar = feed.visualDisplaceFar;
    u.visualMode        = (patch.visualMode == 2
                           && s->visualHeightReady && feed.visualSide > 1) ? 2 : 0;
    u.edgeStitch        = (int32_t)patch.edgeStitch;
    u.quadCountX        = patch.quadCountX;
    u.quadCountY        = patch.quadCountY;
    // VK-PARITY-TERRAIN-1: values pulled from the shared GL-side producers.
    u.ambientFloor      = feed.ambientFloor;
    u.ambientDayGate    = feed.ambientDayGate;
    u.missionFlatR      = feed.missionFlatLight[0];
    u.missionFlatG      = feed.missionFlatLight[1];
    u.missionFlatB      = feed.missionFlatLight[2];
    // VK-PARITY-TERRAIN-2: pool/grid/pad-glow knobs from the shared producers.
    u.numPoolLights       = vkPoolLights;
    u.lightGridDim        = vkGridDim;
    u.lightGridMinX       = feed.lightGridMin[0];
    u.lightGridMinY       = feed.lightGridMin[1];
    u.lightGridInvCellX   = feed.lightGridInvCell[0];
    u.lightGridInvCellY   = feed.lightGridInvCell[1];
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
}

extern "C" int mc2_vk_terrain_island_run(int w, int h) {
    ZoneScopedN("VK-terrain-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    if (!terrainTier1Requested()) return 0;
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
    const clk::time_point tCopies0 = clk::now();
    // VK-TERRAIN-STAGING-PERF-1: per-step sub-timers inside the copies zone, only
    // sampled when MC2_VK_TERRAIN_COPY_STATS is set (same gate as the summary line).
    static const bool s_copyStatsOn = mc2::gate::get<"MC2_VK_TERRAIN_COPY_STATS">();
    double subStaticUs = 0.0, subDynUs = 0.0, subCsmUs = 0.0;
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

    // VK-TERRAIN-VISUALDISPLACE-FAR-1: version-gated mission-load snapshot of the 4x
    // visual-height bake (no-op unless the sub-gate armed the feed fields AND the bake
    // version changed). Fail-soft: not-ready -> UBO visualMode forced 0 (undisplaced).
    if (feed.visualHeightCpu)
        snapshot_visual_height(s, feed);

    // Binding 5 is the VK shadow island's static sun map of this frame's generation, or no static shadow (lit, never a hole).
    {
        const clk::time_point t0 = clk::now();
        bind_static_shadow(s, feed);
        if (s_copyStatsOn) subStaticUs = usSince(t0);
    }

    // Binding 7 is the VK shadow island's single dynamic map this frame, or no dynamic shadow at all.
    {
        const clk::time_point t0 = clk::now();
        copy_dynamic_shadow(s, feed);
        if (s_copyStatsOn) subDynUs = usSince(t0);
    }

    // VK-TERRAIN-CSM-NEAR-1: per-frame copy of the GL near-cascade array + full-map
    // cascade. No-op (csmArrayReady/csmFullMapReady=false) when the gate is off or GL
    // CSM isn't actually built this frame -> UBO csmEnabled forced 0 below -> frag falls
    // back to the legacy single-map path (fail-soft, never a hole).
    {
        const clk::time_point t0 = clk::now();
        copy_csm_shadows(s, feed);
        if (s_copyStatsOn) subCsmUs = usSince(t0);
    }

    // The shadow island's views are stable across frames, but bindings 5, 7, 8 and 9 are rewritten (write_descriptors) on the frame a binding switches
    // between the island's view and this island's dummy: the descriptor set otherwise still names whatever was last written.
    {
        const VkImageView wantStaticView = s->shadowExternalView != VK_NULL_HANDLE ? s->shadowExternalView : s->shadowView.get();
        const VkImageView wantView = s->dynShadowDirect ? s->dynShadowExternalView : s->dynShadowView.get();
        const VkImageView wantCsmArrayView = s->csmArrayDirect ? s->csmArrayExternalView : s->csmArrayView.get();
        const VkImageView wantCsmFullMapView = s->csmFullMapDirect ? s->csmFullMapExternalView : s->csmFullMapView.get();
        if (wantStaticView != s->shadowBoundView || wantView != s->dynShadowBoundView || wantCsmArrayView != s->csmArrayBoundView
            || wantCsmFullMapView != s->csmFullMapBoundView) {
            // VK-TERRAIN-FENCE-DEFER-1: a prior terrain submit may still be in flight
            // using these sets under the deferred retire (the old inline wait made
            // this rewrite safe for free). Bound-view switches are RARE (first frame
            // of a direct-consume switch / warmup transitions) — device-idle first.
            mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_bound_view_switch");
            write_descriptors(s);
            s->shadowBoundView = wantStaticView;
            s->dynShadowBoundView = wantView;
            s->csmArrayBoundView = wantCsmArrayView;
            s->csmFullMapBoundView = wantCsmFullMapView;
        }
    }

    // VK-TERRAIN-DETAIL: copy-once import of the GL material detail-normal 2D_ARRAY. No-op
    // once imported / while the gate is off (feed.glMatNormalArrayTex==0). Fail-soft: leaves
    // the flat-normal dummy bound (matNormalReady=false) -> FS uses macro-slope-only normal.
    import_matnormal(s, feed);
    }   // VK-FLIP-FIELD-REPORT-1: close VK-terrain-interop-copies zone

    // VK-SHADOW-COPY-ONCE-1 evidence: optional per-frame ms log for the interop-copies
    // zone, gated the same way as MC2_MECH_BATCHER_STATS (env-presence, off by default,
    // zero cost when unset). Lets a smoke run produce before/after timing without a
    // Tracy capture tool in the loop.
    {
        if (s_copyStatsOn) {
            const double ms = usSince(tCopies0) / 1000.0;
            mc2::fmt::print_err("[VK_TERRAIN_COPY_STATS] copies_ms={:.4f} static_us={:.1f} dyn_us={:.1f} csm_us={:.1f} staticGen={:d} shadowReady={:d} dynReady={:d} dynDirect={:d} csmArrayReady={:d} csmArrayDirect={:d} csmFullMapReady={:d} csmFullMapDirect={:d}\n", ms, subStaticUs, subDynUs, subCsmUs, mc2::fmt::as_u64((unsigned long long)feed.staticShadowGeneration), mc2::fmt::as_i32(s->shadowReady ? 1 : 0), mc2::fmt::as_i32(s->dynShadowReady ? 1 : 0), mc2::fmt::as_i32(s->dynShadowDirect ? 1 : 0), mc2::fmt::as_i32(s->csmArrayReady ? 1 : 0), mc2::fmt::as_i32(s->csmArrayDirect ? 1 : 0), mc2::fmt::as_i32(s->csmFullMapReady ? 1 : 0), mc2::fmt::as_i32(s->csmFullMapDirect ? 1 : 0));
        }
    }

    // CPU staging cost so far (copies + snapshots); the per-parity UBO fill below is
    // added after the beginFrame wait so stageUs never absorbs the deferred retire.
    const double usStagePre = usSince(tStage0);

    // VK-TERRAIN-FENCE-DEFER-1 (water-pilot template): begin the shared-slot frame
    // BEFORE the per-frame UBO staging. The slot-fence wait inside beginFrame IS the
    // deferred retire of the submit that last used this parity slot, and it protects
    // everything indexed [par] below: the dynamic UBO, the descriptor set, and the
    // command buffer. The inline post-submit wait this replaces is kept only under
    // the MC2_VK_TERRAIN_FENCE_DEFER=0 killswitch.
    const clk::time_point tFence0 = clk::now();
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Terrain.FenceWait");   // VK-ISLAND-TRACY-1: deferred retire of the parity slot
      fsC = vkImageResourceBeginFrame(s->colorRes); }
    us_fence_wait = usSince(tFence0);
    const uint32_t par = fsC.parity & 1u;
    const clk::time_point tUboFill0 = clk::now();

    // Fill one dynamic-UBO slot per LOD0 patch (shared MVP/mapSide/halfMap + per-patch
    // blockOrigin). quadCount comes from the patch record (drives grid rebuild).
    const uint32_t patchCount = feed.patchCount;
    const VkDeviceSize needUbo = (VkDeviceSize)patchCount * sizeof(TerrainFrameUBO);
    if (!ensure_ubo_capacity(s, par, needUbo)) { s->disabled = true; return 0; }

    // VK-PARITY-TERRAIN-2: stage this frame's light pool + grid into slot [par] (same
    // per-frame cadence as GL's PackAndDispatch/BuildAndUploadLightGrid uploads). Any
    // staging failure degrades to "no pools" (numPoolLights=0), never a hole.
    int32_t vkPoolLights = feed.numPoolLights;
    int32_t vkGridDim    = feed.lightGridDim;
    stage_light_pool(s, par, feed, vkPoolLights, vkGridDim);
    {
        ZoneScopedN("VKIsl.Terrain.UboFill");   // VK-ISLAND-TRACY-1: per-patch dynamic-UBO fill
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->ubo[par].allocation(), &p) == VK_SUCCESS) {
            uint8_t* slots = static_cast<uint8_t*>(p);   // memcpy, not a typed store: the mapped offset is not guaranteed to match the struct's alignment
            for (uint32_t i = 0; i < patchCount; ++i) {
                TerrainFrameUBO u{};
                fill_frame_ubo(s, feed, feed.patches[i], vkPoolLights, vkGridDim, u);
                std::memcpy(slots + (size_t)i * sizeof(TerrainFrameUBO), &u, sizeof(TerrainFrameUBO));
            }
            vmaUnmapMemory(s->allocator, s->ubo[par].allocation());
        }
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
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_TerrainChunk);
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_TerrainChunk);

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
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());

    // VK-DRAW-PERF-1: stamp draws-start (gap 0->1 = setup + clear/load).
    s->gpuTimer.write(cb, 1);

    unsigned long drawn = 0;
    unsigned long skirtDrawn = 0;  // VK-TERRAIN-SKIRT (rung 4): skirt edge-strip draw count
    for (uint32_t i = 0; i < patchCount; ++i) {
        const Mc2TerrainTier1Patch& pr = feed.patches[i];
        const PatchMesh* pm = getOrBuildPatch(s, pr.quadCountX, pr.quadCountY, pr.lodStep);
        if (!pm || pm->indexCount == 0) continue;
        const uint32_t dynOff = i * (uint32_t)sizeof(TerrainFrameUBO);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[par], 1, &dynOff);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
        VkDeviceSize vboOff = 0;
        { const VkBuffer vb = pm->vbo.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vb, &vboOff); }
        vkCmdBindIndexBuffer(cb, pm->ibo.get(), 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, pm->indexCount, 1, 0, 0, 0);
        ++drawn;

        // VK-TERRAIN-SKIRT (rung 4): draw this patch's skirt strips, gated per-block
        // exactly like GL (skirtDepth>0 AND per-edge mask bit set,
        // gos_terrain_lod_chunk.cpp:1736,1753-1758). pr.skirtDepth/skirtEdgeMask are
        // 0 whenever the GL-side skirts sub-gate wasn't armed at capture -- no-op then.
        if (pr.skirtDepth > 0.0f && pr.skirtEdgeMask != 0u) {
            const SkirtMesh* sm = getOrBuildSkirtMesh(s, pr.quadCountX, pr.quadCountY, pr.lodStep);
            if (sm) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->skirtPipe.get());
                VkDeviceSize svboOff = 0;
                { const VkBuffer vb = sm->vbo.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vb, &svboOff); }
                vkCmdBindIndexBuffer(cb, sm->ibo.get(), 0, VK_INDEX_TYPE_UINT16);
                for (int e = 0; e < 4; ++e) {
                    if (((pr.skirtEdgeMask >> e) & 1u) == 0u) continue;
                    if (sm->edgeCount[e] == 0) continue;
                    vkCmdDrawIndexed(cb, sm->edgeCount[e], 1, sm->edgeOffset[e], 0, 0);
                    ++skirtDrawn;
                }
            }
        }
    }

    vkCmdEndRenderPass(cb);
    // VK-DRAW-PERF-1: stamp pass-end (gap 1->2 = all patch/skirt draws).
    s->gpuTimer.write(cb, 2);
    vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_TerrainChunk);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // Item-4 submit: wait/signal NO GL<->VK semaphores. The fence orders CPU access;
    // downstream native islands (prop/mech/water/post) chain Vulkan-owned resources.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Terrain.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
      if (mc2vk::submitGraphics(si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
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
        ZoneScopedN("VKIsl.Terrain.InlineFenceWait");   // VK-ISLAND-TRACY-1: killswitch-only inline retire
        const clk::time_point tInline0 = clk::now();
        if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
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
