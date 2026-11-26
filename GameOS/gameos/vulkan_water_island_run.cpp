// VK water island: mc2_vk_water_island_run: the per-frame feed staging, draw and fence handling.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vulkan_water_island_internal.h"
#include "load_scope.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "vk_one_shot.h"
#include "vk_dynamic_rendering.h"
#include "vk_barrier_batch_vk.h"
#include <cmath>
#include <type_traits>
#include "mc2fmt.h"
#include "utils/vec.h"   // existing Windows min/max macro cleanup
#include "vulkan_shadow_island_live.h"   // the bake mask
#include "vulkan_mover_shadow.h"
#include "gos_terrain_lod_chunk.h"   // gos_WaterShoreMode
#include "vulkan_terrain_island.h"
#include "terrain_field_store.h"
void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap);
#include "mc2_diag_verbose.h"
#include "../../mclib/battle_bench_wait.h"
#include "vk_island_fail_policy.h"
#include "../../RenderCore/water_reactive.h"
#include "../../mclib/timing.h"
#include "vk_water_planar.h"
#include "Gates/render_options.h"
#include "water_planar_view.h"


const float* gos_GetObjectFixBMVPMat4();
int gos_GetWaterReflectionQuality();
int gos_GetWaterFsDebugMode();
void gos_SetWaterFsDebugMode(int);
bool gos_WaterProfileRequested();
void gos_RequestWaterProfile(bool);

bool vk_water_detail::record_reflection(WaterIsland* s, VkCommandBuffer cb, bool enabled, uint32_t par, const uint32_t (&offsets)[2]) {
    auto barrier=[&](VkImage image,VkImageAspectFlags aspect,VkImageLayout from,VkImageLayout to,
                     VkPipelineStageFlags src,VkAccessFlags read,VkPipelineStageFlags dst,VkAccessFlags write) {
        // Legacy stage/access bits equal the low 32 bits of the sync2 masks.
        mc2vk::sync::cmdImageBarrier(mc2vk::getOrCreateEngineVulkanContext(),cb,image,VkImageSubresourceRange{aspect,0,1,0,1},
            from,VkPipelineStageFlags2(src),VkAccessFlags2(read),to,VkPipelineStageFlags2(dst),VkAccessFlags2(write));
    };
    // Keep the statically used water sampler valid even in Off/Sky-only mode.
    if (!s->reflectionInitialized) {
        barrier(s->reflectionImage.get(),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        s->reflectionInitialized=true;
    }
    if (!enabled) return true;
    MC2_VK_GPU_SCOPE(reflectionScope,cb,"VKIsl.Water.Reflection");
    // Discard the previous mask after dilation has finished reading it.
    barrier(s->coverageImage.get(),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    // Coverage uses the same displaced water vertices, shoreline discard and main-view depth test.
    auto& ctx=mc2vk::engineVulkanContextNoWait();
    namespace dr=mc2vk::dynrender;
    {
    MC2_VK_GPU_SCOPE(coverageScope,cb,"VKIsl.Water.Reflection.Coverage");
    dr::Pass coverage;
    coverage.area={{0,0},{static_cast<uint32_t>(s->w),static_cast<uint32_t>(s->h)}};
    coverage.colorCount=2;
    const VkClearValue zero{};
    coverage.color[0]=dr::attachment(s->coverageView.get(),VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,&zero);
    coverage.color[1]=dr::attachment(VK_NULL_HANDLE,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE);
    coverage.setDepth(dr::attachment(s->sDepthRes->view.get(),VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE),s->sceneDepthFmt);
    dr::memoryBarrier(ctx,cb,VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    dr::begin(ctx,cb,coverage);
    mc2vk::infra::setSceneViewport(cb,static_cast<uint32_t>(s->w),static_cast<uint32_t>(s->h));
    mc2vk::infra::bind(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->maskCoverage.get());
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->playout.get(),0,1,&s->set[par],2,offsets);
    vkCmdDrawIndirect(cb,s->native->drawArgs(par),0,1,16);
    dr::end(ctx,cb);
    }
    {
    MC2_VK_GPU_SCOPE(maskScope,cb,"VKIsl.Water.Reflection.Mask");
    barrier(s->coverageImage.get(),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    dr::memoryBarrier(ctx,cb,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT);
    mc2vk::infra::bind(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->maskCompute.get());
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->maskLayout.get(),0,1,&s->maskSet,0,nullptr);
    for (int32_t pass=0; pass<4; ++pass) {
        const int32_t mode = pass | ((s->reflectionDivisor-1) << 2);
        vkCmdPushConstants(cb,s->maskLayout.get(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(mode),&mode);
        const int32_t width = pass==3 ? s->w : (s->w+7)/8;
        const int32_t height = pass==3 ? s->h : (s->h+7)/8;
        vkCmdDispatch(cb,(width+15)/16,(height+15)/16,1);
        dr::memoryBarrier(ctx,cb,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,
            pass==3 ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            pass==3 ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
    }
    }
    {
    MC2_VK_GPU_SCOPE(depthCopyScope,cb,"VKIsl.Water.Reflection.DepthCopy");
    barrier(s->planarDepthImage.get(),VK_IMAGE_ASPECT_DEPTH_BIT,
        s->planarDepthInitialized ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        s->planarDepthInitialized ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
    copy.imageExtent={static_cast<uint32_t>(s->w),static_cast<uint32_t>(s->h),1};
    vkCmdCopyBufferToImage(cb,s->maskDepth.get(),s->planarDepthImage.get(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier(s->planarDepthImage.get(),VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    barrier(s->reflectionImage.get(),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    s->planarDepthInitialized = true;
    if (!s->planarTerrainRecord || !s->planarTerrainRecord(cb)) return false;
    if (s->planarPropsRecord && !s->planarPropsRecord(cb)) return false;
    barrier(s->reflectionImage.get(),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    return true;
}

extern float g_waterGlintWarm;   // visual_tuning_profile.cpp
extern float g_waterWaveBreakup; // visual_tuning_profile.cpp

namespace {
bool prepareNative(WaterIsland* s, mc2vk::VulkanContext& ctx) {
    if (s->native && s->native->ready()) return true;
    if (s->nativeFailed) return false;
    const auto capacity = WaterStream::GetNativeCapacity();
    if (!capacity) return false;
    // GetNativeCapacity adds 4096 records of slack after counting eligible quads.
    // A dry map skips the reflection pass, including its attachment transitions.
    s->mapHasWater = capacity->eligibleBound > 4096u;
    if (s->mapHasWater && !s->planarDepthImage && !init_reflection(s)) return false;
    mc2vk::gpuprof::rearmWaterReflectionCost();   // WATER-REFLECT-TIER: one cost line per mission
    ZoneScopedN("VKIsl.Water.PrepareNative");
    if (!s->recipeUploaded) {
        if (!upload_recipes_device(s, WaterStream::GetRecipes(), WaterStream::GetRecipeCount())) return false;
        s->recipeUploaded = true;
        g_health.recipeCount = WaterStream::GetRecipeCount();
    }
    ctx.waitIdle("water_native_init");
    auto np = s->native ? std::move(s->native) : std::make_unique<mc2vk::NativeProducers>();
    (void)mc2vk::takeLastCreateFailure();
    if (!np->init(ctx, capacity->vertices, WaterStream::GetRecipeCount(), s->recipeBuf.get(), capacity->eligibleBound)) {
        // The native cull/pack producers are the only water feed: a build failure here is a water init failure.
        mc2vk::onIslandInitFailure(mc2vk::IslandKind::Water, true, mc2vk::takeLastCreateFailure(), "native_producers_init_failed");
        return false;   // fatal does not return; Retry tries again next frame
    }
    s->native = std::move(np);
    // The first drawn slot still waits for a lighting snapshot, so prewarm
    // never exposes the newly allocated, unwritten lighting output.
    for (bool& thinNative : s->thinNative) thinNative = false;
    write_descriptors(s);
    return true;
}
}

// Per-frame entry. Inputs (all from the GL side, at water-render time):
//   sceneColorTex/sceneDepthTex — GL scene MainColor + depth textures (w x h).
//   mvp16 — the 16 raw floats GL uploads (glUniformMatrix4fv GL_FALSE) for u_worldToClipGL.
//   nativeInputsIn — the per-frame scalars of the native Vulkan cull/pack (WaterStream::VkNativeInputs); the thin records are produced on the GPU.
//   recipes/recipeCount — mission-static recipe array (64B each; uploaded once).
//   vsP/fsP — the packed uniform params (LOCKSTEP with water_geom.{vert,frag}).
// Returns 1 if the VK water pass rendered this frame; 0 = declined (no water this frame: the GL water draw is gone).
extern "C" int mc2_vk_water_island_prewarm(int w,int h) {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    if(!ctx.valid() || w<=0 || h<=0) return 0;
    auto& slot=ctx.islandSlot(mc2vk::IslandSlot::Water);
    if(!slot) {
        MC2_LOAD_SCOPE("water.init");
        auto fresh=std::make_unique<WaterIsland>();
        if(!init_island(fresh.get(),w,h)) { ctx.waitIdle("water_prewarm_failure"); return 0; }
        slot=std::move(fresh);
    }
    auto* s=parkedIsland();
    if (![&] { MC2_LOAD_SCOPE("water.hdriShore"); return ensure_hdri(s, mc2vk::hdriCpuGeneration()) && ensure_shore(s); }()) return 0;
    if(!s->native) s->native=std::make_unique<mc2vk::NativeProducers>();
    if (![&] { MC2_LOAD_SCOPE("water.pipelines"); return s->native->prewarmPipelines(ctx); }()) return 0;
    if (![&] { MC2_LOAD_SCOPE("water.native"); return prepareNative(s, ctx); }()) return 0;
    if (![&] { MC2_LOAD_SCOPE("water.planarProps"); return mc2::waterplanar::prewarmProps(s->planarProps, w, h); }()) return 0;
    // The live planar terrain island is otherwise created inside the first drawn frame; its lazy creation stays the fallback.
    if (s->mapHasWater && reflect_live_active()) {
        MC2_LOAD_SCOPE("water.planarTerrain"); (void)mc2::waterplanar::prewarmTerrain(s->planarTerrain, w, h);
    }
    MC2_LOAD_SCOPE("water.warmDispatch");
    // Exercise cull/args now as well as creating their pipelines: the first
    // gameplay dispatch must not pay driver first-use work or buffer clears.
    // This off-screen cull is never drawn; the real feed overwrites both slots.
    ZoneScopedN("VKIsl.Water.WarmDispatch");
    const auto capacity = WaterStream::GetNativeCapacity();
    if (!capacity) return 0;
    // The first feed's visibility test builds the shore-field wet tables; do it here, with the field mode the feed will use.
    if (gos_WaterShoreMode() == 3 && s->shHSourceReal && s->shHSdf) WaterStream::PrewarmShoreFieldCoverage();
    gos_terrain_lighting::VkProducerSnapshot light{};
    light.numVertices = capacity->vertices;
    mc2vk::WaterCullFrame water{};
    water.mapSide = static_cast<uint32_t>(std::sqrt(static_cast<double>(capacity->vertices)));
    water.mvp[0] = water.mvp[5] = water.mvp[10] = water.mvp[15] = 1.0f;
    water.mvp[12] = 1e9f;
    bool recorded = true;
    const bool submitted = mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) {
        for (uint32_t par = 0; par < mc2vk::kMaxFramesInFlight; ++par)
            recorded = s->native->record(cb, par, light, water, false) && recorded;
    }, "water_load_dispatch");
    return submitted && recorded ? 1 : 0;
}

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
    if (const WaterIsland* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) {
        if (parked->profileIssued < 2400) gos_SetWaterFsDebugMode(0);
        slotCtx.retireIsland(mc2vk::IslandSlot::Water);
    }
    if (!islandSlot()) {
        auto fresh = std::make_unique<WaterIsland>();
        (void)mc2vk::takeLastCreateFailure();
        if (!init_island(fresh.get(), w, h)) {
            slotCtx.waitIdle("water_init_failure");   // objects made before the failure may have been submitted
            mc2vk::onIslandInitFailure(mc2vk::IslandKind::Water, g_waterInitReachedBuild != 0,
                                       mc2vk::takeLastCreateFailure(), g_health.fallbackReason);
            return 0;
        }
        mc2vk::islandInitFailures(mc2vk::IslandKind::Water).reset();
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
    bool hdriOk;
    { ZoneScopedN("VKIsl.Water.EnsureHdri"); hdriOk = ensure_hdri(s, hdriGeneration); }
    if (!hdriOk) {
        setFallback("hdri_binding_setup_failed"); s->disabled = true; return 0;
    }

    // VK-PARITY-WATER-SHORE-1: shore height/mask copy-once import (binding 3/4).
    bool shoreOk;
    { ZoneScopedN("VKIsl.Water.EnsureShore"); shoreOk = ensure_shore(s); }
    if (!shoreOk) {
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
    // staging buffers, descriptor set, command buffer.
    // The parameter UBOs go into the engine frame ring BEFORE beginFrame (a ring failure after it could strand the fence); the ring slot belongs to the
    // frame, not to the parity, so staging them here races nothing.
    const VkDeviceSize fsUboOff = mc2vk::alignUp(kWaterVsUboSize, slotCtx.frameAlignment());
    const VkDeviceSize reactiveOff = mc2vk::alignUp(fsUboOff + sizeof(WaterFsParams), slotCtx.frameAlignment());
    const mc2vk::FrameAlloc uboSpan = slotCtx.frameAlloc(reactiveOff + sizeof(mc2::waterreactive::GpuFrame));
    if (!uboSpan) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }

    const clk::time_point tFence0 = clk::now();
    // GPU-FRONTIER-1: engine frame slot + this island's own previous submit (see the sky island); no shared scene-color fence ring.
    const uint32_t par = slotCtx.frameContext().slot;
    { ZoneScopedN("VKIsl.Water.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);   // VK-ISLAND-TRACY-1: deferred retire of the slot
      if (!vk_island_util::waitStrike(s->waitStrikes, s->disabled, slotCtx.wait(s->submitted[par], 5000000000ull, "water submission slot"), "VK_WATER")) { setFallback("submission_slot_timeout"); vlog("LOUD: submission slot wait timed out (5 s); skipping this frame, will retry next frame."); return 0; } }
    us_fence_wait = usSince(tFence0);
    // Read only after this slot's previous water submission has retired. Queries
    // measure fragment invocations (including helpers/discard), not visible pixels.
    if (s->profilePending[par] && s->profileStats && s->profileTimes && s->profileSamples) {
        const int mode = s->profilePending[par] - 1;
        uint64_t fragments = 0, samples = 0, ticks[2]{};
        const auto flags = VK_QUERY_RESULT_64_BIT;
        if (vkGetQueryPoolResults(s->device, s->profileSamples.get(), par, 1, sizeof(samples), &samples, sizeof(uint64_t), flags) != VK_SUCCESS ||
            vkGetQueryPoolResults(s->device, s->profileStats.get(), par, 1, sizeof(fragments), &fragments, sizeof(uint64_t), flags) != VK_SUCCESS ||
            vkGetQueryPoolResults(s->device, s->profileTimes.get(), 2 * par, 2, sizeof(ticks), ticks, sizeof(uint64_t), flags) != VK_SUCCESS) {
            vlog("WATER_PROFILE FAIL query results mode=%d", mode);
            gos_RequestWaterProfile(false); s->profileIssued = 2400;
            gos_SetWaterFsDebugMode(0);
        } else {
            s->profileFragments[mode] += fragments;
            s->profilePassed[mode] += samples;
            s->profileTicks[mode] += ticks[1] - ticks[0];
            if (++s->profileCount[mode] == 300) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(s->phys, &props);
                Mc2TerrainTier1Feed feed{};
                mc2_terrain_tier1_get_frame_feed(&feed);
                vlog("WATER_PROFILE mode=%d frames=300 gpu_ms=%.6f fragment_invocations=%.3f samples_passed=%.3f visible_quads=%u visualDisplaceFar=%.6f", mode,
                     static_cast<double>(s->profileTicks[mode]) * props.limits.timestampPeriod / 300.0e6,
                     static_cast<double>(s->profileFragments[mode]) / 300.0,
                     static_cast<double>(s->profilePassed[mode]) / 300.0,
                     s->native ? s->native->lastVisible() : 0u, feed.visualDisplaceFar);
            }
        }
        s->profilePending[par] = 0;
    }
    bool profilePending = false;
    for (int pending : s->profilePending) profilePending |= pending != 0;
    if (gos_WaterProfileRequested() && s->profileIssued == 2400 && !profilePending) {
        gos_RequestWaterProfile(false);
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(s->phys, &features);
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; qi.queryCount = mc2vk::kMaxFramesInFlight;
        qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        bool ready = features.pipelineStatisticsQuery && features.occlusionQueryPrecise;
        if (ready && !s->profileStats)
            ready = mc2vk::create(s->profileStats, s->device, [&](auto* h) { return vkCreateQueryPool(s->device, &qi, nullptr, h); });
        qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.pipelineStatistics = 0;
        if (ready && !s->profileSamples)
            ready = mc2vk::create(s->profileSamples, s->device, [&](auto* h) { return vkCreateQueryPool(s->device, &qi, nullptr, h); });
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount *= 2; qi.pipelineStatistics = 0;
        if (ready && !s->profileTimes)
            ready = mc2vk::create(s->profileTimes, s->device, [&](auto* h) { return vkCreateQueryPool(s->device, &qi, nullptr, h); });
        if (ready) {
            s->profileIssued = 0;
            for (int mode = 0; mode < 8; ++mode) {
                s->profileCount[mode] = 0; s->profileFragments[mode] = 0; s->profileTicks[mode] = 0; s->profilePassed[mode] = 0;
            }
        } else {
            gos_SetWaterFsDebugMode(0);
            vlog("WATER_PROFILE FAIL fragment statistics or precise occlusion unavailable");
        }
    }
    const int profileMode = s->profileIssued < 2400 ? static_cast<int>(s->profileIssued / 300) : -1;
    if (profileMode >= 0) gos_SetWaterFsDebugMode(profileMode);
    const auto ground = mc2_vk_ground_lighting_frame();
    if (!ground) return 0;
    s->groundLighting.copy(s->device, par, *ground);
    if (!mc2vk::bindMoverShadowDescriptors(s->device,s->set[par],&s->moverStamp[par])) { setFallback("mover_receiver_unavailable"); s->disabled=true; return 0; }

    const clk::time_point tStage0 = clk::now();
    const WaterStream::VkNativeInputs* nat = nullptr;   // STEP 3.4P: set in the feed block when the native producers run this frame
    bool reflectThisFrame=false;
    bool waterInView=true;
    {
    ZoneScopedN("VKIsl.Water.Feed");   // VK-ISLAND-TRACY-1: recipe/thin/UBO staging (steps 1-3)
    // ---- 1) Upload recipe SSBO once (mission-static), into device-local memory (the cull compute reads it every frame). ----
    if (!s->recipeUploaded) {
        if (!upload_recipes_device(s, recipes, recipeCount)) { s->disabled = true; return 0; }
        s->recipeUploaded = true;
        g_health.recipeCount = recipeCount;
    }

    // Fallback for callers whose mission did not run the load prewarm.
    if ((!s->native || !s->native->ready()) && !s->nativeFailed) {
        gos_terrain_lighting::VkProducerSnapshot snap0{};
        if (gos_terrain_lighting::GetVkProducerSnapshot(snap0) && snap0.numVertices > 0)
            prepareNative(s, slotCtx);
    }
    nat = (s->native && s->native->ready()) ? static_cast<const WaterStream::VkNativeInputs*>(nativeInputsIn) : nullptr;
    bool anyThinNative = false;
    for (bool thinNative : s->thinNative) anyThinNative |= thinNative;
    if (nat && !anyThinNative) {
        gos_terrain_lighting::VkProducerSnapshot first{};
        if (!gos_terrain_lighting::GetVkProducerSnapshot(first) || !first.numVertices) nat = nullptr;
    }
    if (!nat) return 0;   // nothing will submit
    // Mission reload can destroy the previous bake's view. Refresh the borrowed
    // mask before ANY descriptor rewrite, including the first native thin bind.
    s->maskView = (mc2_vk_shadow_island_bake_ready() && s->hdriSampler)
                      ? reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_bake_mask_view()) : VK_NULL_HANDLE;
    // The shore rebake rewrites every descriptor, so it runs after the mask refresh.
    bool shoreSurfOk;
    { ZoneScopedN("VKIsl.Water.ShoreSurface"); shoreSurfOk = ensure_shore_surface(s); }
    if (!shoreSurfOk) { setFallback("shore_surface_bake_failed"); s->disabled = true; return 0; }
    if (!prepare_shore_fft(s,par,uboSpan.buffer)) {
        setFallback("shore_fft_prepare_failed"); s->disabled=true; return 0;
    }

    // Binding 6 of slot `par` must name the GPU-produced thin buffer (the island starts on the placeholder thinBuf until the native producers exist).
    if (!s->thinNative[par]) {
        s->thinNative[par] = true;
        write_descriptors(s, static_cast<int>(par));
    }
    g_health.thinRecords = 0;   // GPU-produced: the count lives in the draw args

    // Live tier switch (Low/Medium <-> High): swap the reflection colour image before this frame's descriptors are written,
    // so the frame never samples a retired bake or a stale-size image.
    if (s->mapHasWater && s->reflectionFull != reflect_live_active() && !init_reflection(s)) { setFallback("reflection_init_failed"); s->disabled = true; return 0; }
    refresh_hdri_sampler(s);
    const std::array<VkImageView,4> views{s->hdriTexture.view(),s->shHTexture.view(),s->shMTexture.view(),s->maskView};
    if (s->imageDescriptorsDirty[par] || s->boundTextures[par]!=views) {
        write_descriptors(s,static_cast<int>(par)); s->boundTextures[par]=views;
    }
    // ---- 3) Stage the parameter UBOs into the frame-ring span and point slot `par` at its buffer. ----
    {
        uint8_t* const base = static_cast<uint8_t*>(uboSpan.ptr);
        WaterVsParams vsLocal;
        std::memcpy(&vsLocal, vsParams, sizeof(WaterVsParams));
        WaterFsParams fsLocal;
        std::memcpy(&fsLocal, fsParams, sizeof(WaterFsParams));
        fsLocal.debug[0] = static_cast<float>(gos_GetWaterFsDebugMode());
        fsLocal.f9[3] = ::g_waterWaveBreakup;    // F.f9.w: waterWaveBreakup key (projAspect slot, unread by water_geom.frag)
        fsLocal.debug[3] = ::g_waterGlintWarm;  // F.debug.w: waterGlintWarm key (slot unused elsewhere)
        // Shore mode 3: flat water plane in the VS only when the hi-res height sidecar
        // is really imported (mirrors GL u_shoreFlat = mode==3 && shTex != 0).
        const bool shoreSdf = static_cast<int>(fsLocal.f4[0] + 0.5f) == 3 &&
                              s->shHReal && s->shHSdf;
        float bounds[4]{};
        // Conservative superset of the GPU cull: false means it admits no water triangle. Also bounds the live reflection walk.
        waterInView = s->mapHasWater && WaterStream::AnyWaterChunkInFrustum(*nat);
        reflectThisFrame=waterInView && gos_GetWaterReflectionQuality()==2 && mc2::options::current().reflectionQuality>0
            && !reflect_bake_active() && WaterStream::NativeWaterInFrustum(*nat,shoreSdf,bounds);
        const auto pixels=mc2::waterplanar::paddedPixels(bounds,w,h);
        s->reflectionScissor={{pixels[0],pixels[1]},{static_cast<uint32_t>(pixels[2]-pixels[0]),static_cast<uint32_t>(pixels[3]-pixels[1])}};
        const auto* fields = gos_TerrainFieldStore();
        vsLocal.p3[1] = (shoreSdf || (fields && fields->visualSide() > 1)) ? 1.0f : 0.0f;
        vsLocal.p3[2] = fields ? static_cast<float>(fields->visualSide()) : 0.0f;
        vsLocal.p3[3] = static_cast<float>(mc2::options::current().shorelineLook);
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
        // The bake mask: its world span (the terrain grid, as the terrain shader maps it) and whether binding 7 holds it this frame.
        {
            int side = 0;
            float halfMap = 0.0f;
            gos_GetTerrainChunkGridInfo(&side, &halfMap);
            fsLocal.f7[1] = 128.0f * static_cast<float>(side > 1 ? side - 1 : 1);
            fsLocal.f7[2] = (s->maskView != VK_NULL_HANDLE) ? 1.0f : 0.0f;
            fsLocal.f7[3] = halfMap;
        }

        const float* objectCamera = gos_GetObjectFixBMVPMat4();
        float camera[33]{};
        std::memcpy(camera, nat->mvp, 16*sizeof(float));
        if (objectCamera) std::memcpy(camera+16, objectCamera, 16*sizeof(float));
        camera[32] = nat->waterElevation;
        static const bool hold = mc2::gate::get<"MC2_WATER_REFLECTION_HOLD">();
        const bool still = objectCamera && std::memcmp(camera,s->reflectionCamera,sizeof(camera))==0;
        const int divisor = 1;   // only High renders the live pass (full resolution); Low and Medium use the bake
        fsLocal.debug[1] = mc2::options::current().waterDetail == 0 ? 1.0f : 0.0f;
        // f3.w: 0/1 retain current static/animated wash; 2/3/4 select the review candidates.
        if (mc2::options::current().shorelineLook != 0)
            fsLocal.f3[3] = static_cast<float>(1 + mc2::options::current().shorelineLook);
        // No cheap bake/content epoch: bound stale light, scene and leaf sway to four frames.
        const bool reuse = hold && still && s->reflectionValid && s->reflectionDivisor==divisor && s->nativeFrames%4!=0;
        std::memcpy(s->reflectionCamera,camera,sizeof(camera));
        s->reflectionValid = reflectThisFrame && !reflect_bake_active();   // a frame served by the bake leaves no live image to reuse
        s->reflectionDivisor = divisor;
        fsLocal.DEEP_COLOR[3] = reflectThisFrame ? static_cast<float>(divisor) : 0.0f;
        reflectThisFrame = reflectThisFrame && !reuse;
        std::memcpy(base + fsUboOff, &fsLocal, sizeof(WaterFsParams));
        // Frame-owned host writes, after this descriptor slot's fence; no readback or extra submit.
        // The snapshot is written straight into the frame-ring slot (no 139 KB stack frame, no copy). GpuFrame is trivially copyable.
        static_assert(std::is_trivially_copyable_v<mc2::waterreactive::GpuFrame>);
        auto& reactive = *reinterpret_cast<mc2::waterreactive::GpuFrame*>(base + reactiveOff);
        mc2::waterreactive::buffer().snapshot(scenarioTime, reactive);
        {
            static auto lastLog = clk::now() - std::chrono::seconds(2);
            static const bool logStats = mc2_diag::tagRequested("WATER_REACTIVE");   // MC2_DIAG_TAGS=WATER_REACTIVE
            if (logStats && usSince(lastLog) >= 1.0e6) {
                lastLog = clk::now();
                const auto st = mc2::waterreactive::buffer().takeStats();
                uint32_t litTiles = 0, tileCount = 0;
                for (uint32_t c : reactive.counts) litTiles += c ? 1u : 0u;
                float maxRadius = 0.0f;
                const auto& p0 = reactive.stamps[0].positionRadiusAge;
                const uint32_t live = mc2::waterreactive::buffer().size();
                if (live) {
                    tileCount = reactive.counts[mc2::waterreactive::tile(
                        static_cast<int>(std::floor(p0[0] / mc2::waterreactive::kTileWidth)),
                        static_cast<int>(std::floor(p0[1] / mc2::waterreactive::kTileWidth)))];
                    for (uint32_t i = 0; i < live; ++i)
                        maxRadius = std::max(maxRadius, reactive.stamps[i].positionRadiusAge[2]);
                }
                if (mc2_diag::tagRequested("WATER_REACTIVE")) mc2::fmt::print_err("[WATER_REACTIVE v1] stamps={:d} litTiles={:d} submits={:d} wet={:d} vis={:d} wetVisible={:d} emitted={:d} maxRadius={:.1f} stamp0=({:.1f},{:.1f}) age0={:.2f} tileUnderStamp0={:d} firstVxy=({:.1f},{:.1f})\n",
                    mc2::fmt::as_u32(live), mc2::fmt::as_u32(litTiles),
                    mc2::fmt::as_u32(st.submits), mc2::fmt::as_u32(st.wet), mc2::fmt::as_u32(st.vis), mc2::fmt::as_u32(st.wetVisible), mc2::fmt::as_u32(st.emitted), maxRadius,
                    p0[0], p0[1], p0[3], mc2::fmt::as_u32(tileCount), st.vx, st.vy);
            }
        }
        VkDescriptorBufferInfo reactiveInfo{uboSpan.buffer, uboSpan.offset + reactiveOff, sizeof(reactive)};
        VkWriteDescriptorSet reactiveWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        reactiveWrite.dstSet = s->set[par]; reactiveWrite.dstBinding = 13;
        reactiveWrite.descriptorCount = 1; reactiveWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        reactiveWrite.pBufferInfo = &reactiveInfo;
        vkUpdateDescriptorSets(s->device, 1, &reactiveWrite, 0, nullptr);
        if (s->ringBound[par] != uboSpan.buffer) {
            mc2vk::writeDynamicBinding(s->device, s->set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, uboSpan.buffer, kWaterVsUboSize);
            mc2vk::writeDynamicBinding(s->device, s->set[par], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, uboSpan.buffer, sizeof(WaterFsParams));
            s->ringBound[par] = uboSpan.buffer;
        }
    }
    }   // VK-ISLAND-TRACY-1: close VKIsl.Water.Feed

    bool wavesOk;
    { ZoneScopedN("VKIsl.Water.WavePackets"); wavesOk = prepare_wave_packets(s, par); }
    if (!wavesOk) { setFallback("wave_packet_bake_failed"); return 0; }
    bool bakeOk;
    { ZoneScopedN("VKIsl.Water.ReflectBake"); bakeOk = prepare_reflect_bake(s, nat->waterElevation); }
    if (!bakeOk) setFallback("reflection_bake_failed");
    if (reflect_bake_active()) reflectThisFrame = false;   // the bake replaces the live pass below High
    bind_reflect_bake(s, par, reinterpret_cast<WaterFsParams*>(static_cast<uint8_t*>(uboSpan.ptr) + fsUboOff));
    // Nothing the cull could admit is in view: skip the producers, the scene pass and the submit. The one-time bakes above still ran.
    if (!waterInView && gos_GetWaterFsDebugMode() == 0 && profileMode < 0) {
        s->reflectionValid = false;
        // No water this frame: the per-frame counters must not keep the last water frame's values (usedVulkan is a cumulative frame count and stays).
        g_health.waterVisible = 0; g_health.waterEligibleBound = 0; g_health.vkDraws = 0;
        return 0;
    }
    s->planarTerrainRecord = {}; s->planarPropsRecord = {};
    if (reflectThisFrame) {
        mc2::waterplanar::Target target;
        target.w = w; target.h = h; target.water = nat->waterElevation; target.mainWorldToClip = nat->mvp;
        target.color = s->reflectionView.get(); target.depth = s->planarDepthView.get();
        target.scissor = s->reflectionScissor;
        target.divisor = s->reflectionDivisor;
        target.scissor.offset.x /= target.divisor; target.scissor.offset.y /= target.divisor;
        target.scissor.extent.width = (s->reflectionScissor.offset.x + s->reflectionScissor.extent.width + target.divisor - 1) / target.divisor - target.scissor.offset.x;
        target.scissor.extent.height = (s->reflectionScissor.offset.y + s->reflectionScissor.extent.height + target.divisor - 1) / target.divisor - target.scissor.offset.y;
        target.scissor.extent.width = std::min(target.scissor.extent.width, static_cast<uint32_t>((w+target.divisor-1)/target.divisor-target.scissor.offset.x));
        target.scissor.extent.height = std::min(target.scissor.extent.height, static_cast<uint32_t>((h+target.divisor-1)/target.divisor-target.scissor.offset.y));
        if (![&] {
                ZoneScopedN("Water.PlanarTerrainPrepare");
                return mc2::waterplanar::prepareTerrain(s->planarTerrain, target, s->planarTerrainRecord);
            }() || ![&] {
                ZoneScopedN("Water.PlanarPropsPrepare");
                return mc2::waterplanar::prepareProps(s->planarProps, target, s->planarPropsRecord);
            }()) {
            setFallback("planar_view_prepare_failed");
            reflectThisFrame = false;
            s->reflectionValid = false;   // no image was rendered: a held frame must not sample it
            s->planarTerrainRecord = {}; s->planarPropsRecord = {};
            // Keep water visible with sky only, and rebuild failed owners on
            // the next frame instead of permanently disabling the water pass.
            slotCtx.retire(std::move(s->planarTerrain));
            slotCtx.retire(std::move(s->planarProps));
            reinterpret_cast<WaterFsParams*>(static_cast<uint8_t*>(uboSpan.ptr) + fsUboOff)->DEEP_COLOR[3] = 0.0f;
        }
    }

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
        vkResetCommandPool(s->device, s->cpool[par].get(), 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
        vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_Water);
        if (mc2::options::current().shorelineLook == 2)
            record_shore_fft(s,cb,par,static_cast<uint32_t>(uboSpan.offset+fsUboOff));

        // STEP 3.4P: the native producers (terrain lighting, cull/pack, draw args) are recorded ahead of the render pass in this command buffer.
        {
            gos_terrain_lighting::VkProducerSnapshot snap{};
            gos_terrain_lighting::GetVkProducerSnapshot(snap);   // none = zero window: the cull reads the previous lighting
            mc2vk::WaterCullFrame wf{};
            std::memcpy(wf.mvp, nat->mvp, sizeof(wf.mvp));
            wf.frameCos = nat->frameCos; wf.waterElevation = nat->waterElevation; wf.shoreExt = nat->shoreExt;
            wf.mapSide = nat->mapSide; wf.biasLegacy = nat->biasLegacy;
            wf.waterVertices = mc2::options::current().shorelineLook == 2 ? 384 : 6;
            // D0 must include field-dry recipes: scene depth alone defines submersion.
            if (gos_GetWaterFsDebugMode() == 10) { wf.shoreExt = 1.0e9f; wf.frameCos = 0.0f; }
            if (reflectThisFrame) {
                mc2vk::gpuprof::noteWaterReflection(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            }
            bool nativeRecOk;
            { ZoneScopedN("VKIsl.Water.NativeRecord"); nativeRecOk = s->native->record(cb, par, snap, wf, false); }
            if (!nativeRecOk) {
                // record() fails only before it records a command: close the begun buffer so it is never left open, then fail soft.
                vkEndCommandBuffer(cb);
                setFallback("native_record_failed"); s->disabled = true; return 0;
            }
            g_health.waterVisible = s->native->lastVisible();
            g_health.waterEligibleBound = s->native->eligibleBound();
        }

        const uint32_t uboOffsets[2] = {static_cast<uint32_t>(uboSpan.offset), static_cast<uint32_t>(uboSpan.offset + fsUboOff)};
        const auto maskGroundSet=s->groundLighting.set(par);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->playout.get(),1,1,&maskGroundSet,4,ground->offsets.data());
        bool reflRecOk;
        { ZoneScopedN("VKIsl.Water.RecordReflection"); reflRecOk = record_reflection(s,cb,reflectThisFrame,par,uboOffsets); }
        if (!reflRecOk) {
            vkEndCommandBuffer(cb); setFallback("planar_record_failed"); s->disabled = true; return 0;
        }
        if (profileMode >= 0) {
            vkCmdResetQueryPool(cb, s->profileSamples.get(), par, 1);
            vkCmdResetQueryPool(cb, s->profileStats.get(), par, 1);
            vkCmdResetQueryPool(cb, s->profileTimes.get(), 2 * par, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s->profileTimes.get(), 2 * par);
        }
        // All attachments LOAD: no clear values.
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Water");
        static constexpr const char* modeZones[] = {"VKIsl.Water.Mode0", "VKIsl.Water.Mode1", "VKIsl.Water.Mode2",
            "VKIsl.Water.Mode3", "VKIsl.Water.Mode4", "VKIsl.Water.Mode5", "VKIsl.Water.Mode6", "VKIsl.Water.Mode7"};
        mc2vk::gpuprof::Scope modeScope(profileMode >= 0 ? cb : VK_NULL_HANDLE,
            profileMode >= 0 ? modeZones[profileMode] : "VKIsl.Water.Idle", __FILE__, __LINE__);
        mc2vk::VulkanContext& dctx = mc2vk::engineVulkanContextNoWait();
        mc2vk::dynrender::beginSceneLoad(dctx, cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h), s->sColorRes->view.get(), s->sNormalRes->view.get(), s->sDepthRes->view.get(), s->sceneDepthFmt);
        mc2vk::infra::setSceneViewport(cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        // Match the bound terrain UBO: only its GPU-ready fine surface makes discard dead.
        const bool earlyWater = ground->visualWaterEligible;
        static int lastEarlyWater = -1;
        if (lastEarlyWater != static_cast<int>(earlyWater)) {
            vlog("WATER_MODULE selected=%s uploaded_visual_eligible=%d", earlyWater ? "early" : "standard", static_cast<int>(earlyWater));
            lastEarlyWater = static_cast<int>(earlyWater);
        }
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, earlyWater ? s->pipeEarly.get() : s->pipe.get());
        if (profileMode >= 0) {
            vkCmdBeginQuery(cb, s->profileStats.get(), par, 0);
            vkCmdBeginQuery(cb, s->profileSamples.get(), par, VK_QUERY_CONTROL_PRECISE_BIT);
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[par], 2, uboOffsets);
        const auto groundSet = s->groundLighting.set(par);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 1, 1, &groundSet, 4, ground->offsets.data());
        if (gos_GetWaterFsDebugMode() == 10 || gos_GetWaterFsDebugMode() == 11) {
            for (const auto& pipeline : s->debugWater) {
                mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.get());
                vkCmdDrawIndirect(cb, s->native->drawArgs(par), 0, 1, 16);
            }
        } else {
            if (mc2::options::current().shorelineLook == 1) {
                mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->absorption.get());
                vkCmdDrawIndirect(cb, s->native->drawArgs(par), 0, 1, 16);
                mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, earlyWater ? s->pipeEarly.get() : s->pipe.get());
            }
            mc2::battlebench::add(mc2::battlebench::DrawWater), vkCmdDrawIndirect(cb, s->native->drawArgs(par), 0, 1, 16);
        }
        if (profileMode >= 0) {
            vkCmdEndQuery(cb, s->profileSamples.get(), par);
            vkCmdEndQuery(cb, s->profileStats.get(), par);
        }
        mc2vk::dynrender::end(dctx, cb);
        modeScope.end();
        gpuScope.end();
        if (profileMode >= 0)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->profileTimes.get(), 2 * par + 1);
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
          if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
        s->submitted[par] = slotCtx.tracker().now();
        if (profileMode >= 0) {
            s->profilePending[par] = profileMode + 1;
            if (++s->profileIssued == 2400) gos_SetWaterFsDebugMode(0);
        }
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

