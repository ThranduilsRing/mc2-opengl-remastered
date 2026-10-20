// VK water island: mc2_vk_water_island_run: the per-frame feed staging, draw and fence handling.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vulkan_water_island_internal.h"
#include "vk_render_pass_timer.h"
#include "mc2fmt.h"
#include "mc2_diag_verbose.h"

#ifdef MC2_VULKAN_ISLAND


// Per-frame entry. Inputs (all from the GL side, at water-render time):
//   sceneColorTex/sceneDepthTex — GL scene MainColor + depth textures (w x h).
//   mvp16 — the 16 raw floats GL uploads (glUniformMatrix4fv GL_FALSE) for u_worldToClipGL.
//   nativeInputsIn — the per-frame scalars of the native Vulkan cull/pack (WaterStream::VkNativeInputs); the thin records are produced on the GPU.
//   recipes/recipeCount — mission-static recipe array (64B each; uploaded once).
//   vsP/fsP — the packed uniform params (LOCKSTEP with water_geom.{vert,frag}).
// Returns 1 if the VK water pass rendered this frame; 0 = declined (no water this frame: the GL water draw is gone).
extern "C" int mc2_vk_water_island_run(
    int w, int h,
    const void* recipes, unsigned int recipeCount,
    const void* vsParams, const void* fsParams,
    uint64_t hdriGeneration,
    const void* nativeInputsIn) {
    if (!nativeInputsIn || recipeCount == 0 || w <= 0 || h <= 0) return 0;
    ZoneScopedN("VKIsl.Water.Run");   // VK-ISLAND-TRACY-1 per-frame island total (CPU)

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = waterPerfGateRequested();
    // Fine-grained sub-step timers (only meaningful when `perf`; cheap to always fill).
    double us_stage = 0.0;      // recipe/thin/UBO memcpy staging (steps 1-3)
    double us_depth_readback=0.0; // glGetTexImage(DEPTH) GL->CPU stall (step 5)
    double us_depth_stage = 0.0;  // memcpy depth scratch -> VK staging (step 5)
    double us_record = 0.0;     // command-buffer record (step 7)
    double us_submit = 0.0;     // vkQueueSubmit (step 8)
    double us_sem_wait = 0.0;   // glInteropWaitFromVk (step 8)
    double us_fence_wait = 0.0; // vkWaitForFences inline stall (step 8)
    double us_composite = 0.0;  // GL composite draw (step 9)

    // Lazy init / rebuild on size change.
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const WaterIsland* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) slotCtx.retireIsland(mc2vk::IslandSlot::Water);
    if (!islandSlot()) {
        auto fresh = std::make_unique<WaterIsland>();
        if (!init_island(fresh.get(), w, h)) {
            vlog("init failed (%s) — no water this frame.", g_health.fallbackReason);
            slotCtx.waitIdle("water_init_failure");   // objects made before the failure may have been submitted
            return 0;
        }
        islandSlot() = std::move(fresh);
    }
    WaterIsland* s = parkedIsland();
    // VK-OWNERSHIP-GAP-FIX (item-4 correctness): direct-chain sets useInterop=false
    // (init:640) but the old guard '|| !s->useInterop' bailed here, so under the strict
    // umbrella water returned 0 and GL silently drew water into the VK-owned GBuffer
    // (used_vulkan=0). Enter the native path when direct-chain OR hybrid interop is armed.
    if (s->disabled || !s->inited) return 0;  // VK-RUNG8-ITEM5: direct-chain-native only

    // RUNG-6.5: COPY-ONCE HDRI import (or 1x1 dummy) — the FS statically uses
    // binding 2, so it must be valid before the first draw. Re-runs only if the
    // GL HDRI texture id changes (e.g. late HDRI load: first frames import the
    // dummy at glTex=0, then the real texture once ready). Safe to update the
    // descriptor here: ensure_hdri device-idles before its descriptor update
    // (VK-WATER-FENCE-DEFER-1 — the old "previous frame's fence was waited
    // inline" guarantee is gone under the deferred retire).
    if (!ensure_hdri(s, hdriGeneration)) {
        setFallback("hdri_binding_setup_failed"); s->disabled = true; return 0;
    }

    // VK-PARITY-WATER-SHORE-1: shore height/mask copy-once import (binding 3/4).
    if (!ensure_shore(s)) {
        setFallback("shore_binding_setup_failed"); s->disabled = true; return 0;
    }

    // Snapshot forbidden COLOR counters (prove none fire on the interop path).
    const unsigned long f0a = g_forbidden.vkCmdCopyBufferToImage;
    const unsigned long f0b = g_forbidden.vkCmdCopyImageToBuffer;
    const unsigned long f0c = g_forbidden.glGetTexImage;
    const unsigned long f0d = g_forbidden.glTexSubImage2D;

    // VK-RUNG8-ITEM5: direct-chain-native only (the trailing non-tier1 path is dead).
    if (!s->tier1) return 0;

    // VK-WATER-FENCE-DEFER-1 (cluster-A pilot): begin the shared-slot frame BEFORE
    // staging. The slot-fence wait inside beginFrame IS the deferred retire of the
    // submit that last used this parity slot (normally an earlier island / last
    // frame — near-free by now), and it protects everything indexed [par] below:
    // staging buffers, descriptor set, command buffer. The inline post-submit wait
    // this replaces is kept only under the MC2_VK_WATER_FENCE_DEFER=0 killswitch.
    // The parameter UBOs go into the engine frame ring BEFORE beginFrame (a ring failure after it could strand the fence); the ring slot belongs to the
    // frame, not to the parity, so staging them here races nothing.
    const VkDeviceSize fsUboOff = mc2vk::alignUp(sizeof(WaterVsParams), slotCtx.frameAlignment());
    const mc2vk::FrameAlloc uboSpan = slotCtx.frameAlloc(fsUboOff + sizeof(WaterFsParams));
    if (!uboSpan) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }

    const clk::time_point tFence0 = clk::now();
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Water.FenceWait");   // VK-ISLAND-TRACY-1: deferred retire of the parity slot
      fsC = vkImageResourceBeginFrame(s->sColorRes); }
    us_fence_wait = usSince(tFence0);
    const uint32_t par = fsC.parity & 1u;

    const clk::time_point tStage0 = clk::now();
    const WaterStream::VkNativeInputs* nat = nullptr;   // STEP 3.4P: set in the feed block when the native producers run this frame
    {
    ZoneScopedN("VKIsl.Water.Feed");   // VK-ISLAND-TRACY-1: recipe/thin/UBO staging (steps 1-3)
    // ---- 1) Upload recipe SSBO once (mission-static), into device-local memory (the cull compute reads it every frame). ----
    if (!s->recipeUploaded) {
        if (!upload_recipes_device(s, recipes, recipeCount)) { s->disabled = true; return 0; }
        s->recipeUploaded = true;
        g_health.recipeCount = recipeCount;
    }

    // STEP 3.4P: bring the native producers up once the recipes and the first lighting snapshot exist.
    if (!s->native && !s->nativeFailed) {
        gos_terrain_lighting::VkProducerSnapshot snap0{};
        if (gos_terrain_lighting::GetVkProducerSnapshot(snap0) && snap0.numVertices > 0) {
            slotCtx.waitIdle("water_native_init");
            auto np = std::make_unique<mc2vk::NativeProducers>();
            if (np->init(slotCtx, snap0.numVertices, recipeCount, s->recipeBuf.get())) {
                s->native = std::move(np);
                s->thinNative[0] = s->thinNative[1] = true;
                write_descriptors(s);   // binding 6 -> the GPU-produced thin buffers
            } else {
                s->nativeFailed = true;
                vlog("native producers unavailable - no water (the GL cull/pack readback feed is gone).");
            }
        }
    }
    nat = (s->native && s->native->ready()) ? static_cast<const WaterStream::VkNativeInputs*>(nativeInputsIn) : nullptr;
    if (!nat) {   // nothing will submit: give the slot's fence back
        if (!vkImageResourceCancelFrame(s->sColorRes, fsC)) { setFallback("cancel_frame_fence_failed"); s->disabled = true; }
        return 0;
    }

    // Binding 6 of slot `par` must name the GPU-produced thin buffer (the island starts on the placeholder thinBuf until the native producers exist).
    if (!s->thinNative[par]) {
        s->thinNative[par] = true;
        write_descriptors(s, static_cast<int>(par));
    }
    g_health.thinRecords = 0;   // GPU-produced: the count lives in the draw args

    const std::array<VkImageView,3> views{s->hdriTexture.view(),s->shHTexture.view(),s->shMTexture.view()};
    if (s->imageDescriptorsDirty[par] || s->boundTextures[par]!=views) {
        write_descriptors(s,int(par)); s->boundTextures[par]=views;
    }
    // ---- 3) Stage the parameter UBOs into the frame-ring span and point slot `par` at its buffer. ----
    {
        uint8_t* const base = static_cast<uint8_t*>(uboSpan.ptr);
        WaterVsParams vsLocal;
        std::memcpy(&vsLocal, vsParams, sizeof(WaterVsParams));
        WaterFsParams fsLocal;
        std::memcpy(&fsLocal, fsParams, sizeof(WaterFsParams));
        // Shore mode 3: flat water plane in the VS only when the hi-res height sidecar
        // is really imported (mirrors GL u_shoreFlat = mode==3 && shTex != 0).
        const bool shoreSdf = static_cast<int>(fsLocal.f4[0] + 0.5f) == 3 &&
                              s->shHReal && s->shHSdf;
        vsLocal.p3[1] = shoreSdf ? 1.0f : 0.0f;
        std::memcpy(base, &vsLocal, sizeof(WaterVsParams));
        // RUNG-6.5 fail-soft: if the real HDRI bytes aren't imported (dummy
        // bound), force u_waterHdriLod (f2.w, offset 60) to -1 so the FS
        // takes the SH-L2 fallback -- same semantics as GL !isHdriReady().
        if (!s->hdriRealImported) fsLocal.f2[3] = -1.0f;
        // VK-PARITY-WATER-SHORE-1: GL sets u_hasShoreHeight = (mode==2 && tex)
        // and u_hasShoreMask = (mode>=1 && tex); the island owns the textures.
        {
            const int shMode = static_cast<int>(fsLocal.f4[0] + 0.5f);
            fsLocal.f4[1] = (shMode >= 2 && shoreSdf) ? 1.0f : 0.0f;
            fsLocal.f4[2] = (shMode >= 1 && s->shMReal) ? 1.0f : 0.0f;
        }
        std::memcpy(base + fsUboOff, &fsLocal, sizeof(WaterFsParams));
        if (s->ringBound[par] != uboSpan.buffer) {
            mc2vk::writeDynamicBinding(s->device, s->set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, uboSpan.buffer, sizeof(WaterVsParams));
            mc2vk::writeDynamicBinding(s->device, s->set[par], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, uboSpan.buffer, sizeof(WaterFsParams));
            s->ringBound[par] = uboSpan.buffer;
        }
    }
    }   // VK-ISLAND-TRACY-1: close VKIsl.Water.Feed

    // ============================ RUNG 6 TIER-1 PATH ============================
    // In-place co-write into the SHARED scene GBuffer. No color/depth import, no
    // composite. Frame-sync protocol COPIED from vulkan_terrain_island.cpp:2495-2603,
    // with the beginFrame hoisted above staging (VK-WATER-FENCE-DEFER-1).
    {
        us_stage = usSince(tStage0);
        g_health.copyColorInUs = 0.0;
        g_health.copyDepthInUs = 0.0;

        VkCommandBuffer cb = s->cbuf[par];

        const clk::time_point tRender0 = clk::now();
        // Declared ahead of the record scope: the bracket opened inside it must be cancelled by any return up to the submit.
        vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_Water);
        {
        ZoneScopedN("VKIsl.Water.Record");   // VK-ISLAND-TRACY-1: command-buffer record
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
        vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_Water);

        // STEP 3.4P: the native producers (terrain lighting, cull/pack, draw args) are recorded ahead of the render pass in this command buffer.
        {
            gos_terrain_lighting::VkProducerSnapshot snap{};
            gos_terrain_lighting::GetVkProducerSnapshot(snap);   // none = zero window: the cull reads the previous lighting
            mc2vk::WaterCullFrame wf{};
            std::memcpy(wf.mvp, nat->mvp, sizeof(wf.mvp));
            wf.frameCos = nat->frameCos; wf.waterElevation = nat->waterElevation; wf.shoreExt = nat->shoreExt;
            wf.mapSide = nat->mapSide; wf.biasLegacy = nat->biasLegacy;
            if (!s->native->record(cb, par, snap, wf, false)) {
                // record() fails only before it records a command: close the begun buffer so it is never left open, then fail soft.
                vkEndCommandBuffer(cb);
                setFallback("native_record_failed"); s->disabled = true; return 0;
            }
        }

        // All attachments LOAD -> clear values ignored (array must still cover them).
        VkClearValue clears[3]{};
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
        rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rbi.clearValueCount = 3; rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
        const uint32_t uboOffsets[2] = {static_cast<uint32_t>(uboSpan.offset), static_cast<uint32_t>(uboSpan.offset + fsUboOff)};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[par], 2, uboOffsets);
        vkCmdDrawIndirect(cb, s->native->drawArgs(par), 0, 1, 16);
        vkCmdEndRenderPass(cb);
        vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_Water);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
        }   // VK-ISLAND-TRACY-1: close VKIsl.Water.Record
        us_record = usSince(tRender0);

        // VK-RUNG8-ITEM5: direct-chain-native submit — waits/signals NO GL<->VK
        // semaphores (shared GBuffer stays Vulkan-owned); color fence orders CPU.
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr; si.pWaitDstStageMask = nullptr;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
        const clk::time_point tSubmit0 = clk::now();
        { ZoneScopedN("VKIsl.Water.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
          if (mc2vk::submitGraphics(si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
        timerGuard.submitted();
        us_submit = usSince(tSubmit0);

        ++s->nativeFrames;

        us_sem_wait = 0.0;

        // VK-WATER-FENCE-DEFER-1: the inline retire wait is GONE — the submit's
        // fence (the shared parity-slot fence) is waited at the slot's next
        // vkImageResourceBeginFrame, i.e. the deferred retire that beginFrame at
        // the top of this function already timed into us_fence_wait. Everything
        // this frame's submit uses is per-parity ([par]) or static, so nothing is
        // rewritten before that wait. GPU-side ordering against the downstream
        // post/composite passes never depended on this CPU wait (their command
        // buffers' entry barriers cover earlier submission-order work).
        // Killswitch MC2_VK_WATER_FENCE_DEFER=0 restores the inline wait.
        if (!waterFenceDeferEnabled()) {
            ZoneScopedN("VKIsl.Water.InlineFenceWait");   // VK-ISLAND-TRACY-1: killswitch-only inline retire
            const clk::time_point tInline0 = clk::now();
            if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
                setFallback("fence_timeout"); s->disabled = true; return 0;
            }
            us_fence_wait = usSince(tInline0);
        }
        g_health.drawUs = usSince(tRender0);
        g_health.colorOutUs = 0.0;   // no composite in tier1

        // HARD assert: no COLOR-side readback/copy fired (tier1 must be zero-copy on
        // color AND depth — there is no expected depth-in either).
        const bool tier1ReadbackFired =
            (g_forbidden.vkCmdCopyBufferToImage != f0a) || (g_forbidden.vkCmdCopyImageToBuffer != f0b) ||
            (g_forbidden.glGetTexImage != f0c) || (g_forbidden.glTexSubImage2D != f0d);
        if (tier1ReadbackFired) {
            vlog("FATAL: a COLOR-side readback/copy fired on the tier1 co-write path — "
                 "zero-copy invariant violated; failing frame + disabling.");
            setFallback("interop_did_readback"); s->disabled = true; return 0;
        }

        g_health.vkDraws = 1;
        ++g_health.usedVulkan;
        // VK-RUNG8-ITEM5: never interop (direct-chain-native only).
        g_health.totalUs = usSince(tTotal0);
        static bool s_tier1FirstOk = false;
        if (!s_tier1FirstOk) {
            vlog("first TIER-1 in-place water GBuffer co-write: recipes=%u "
                 "(color0 premult-over blend, normal untouched (writemask 0, GL a=0 parity), "
                 "shared depth GEQUAL test+write; no composite, no depth readback).",
                 recipeCount);
            s_tier1FirstOk = true;
        }
        if (perf) {
            mc2::fmt::print_err("[VK_WATER_PERF] tier1=1 recipes={:d} stage_us={:.1f} record_us={:.1f} submit_us={:.1f} sem_wait_us={:.1f} fence_wait_us={:.1f} island_total_us={:.1f}\n", mc2::fmt::as_u32(recipeCount), us_stage, us_record, us_submit, us_sem_wait, us_fence_wait, g_health.totalUs);
            std::fflush(stderr);
        }
        return 1;
    }
    return 0;
}

#endif // MC2_VULKAN_ISLAND
