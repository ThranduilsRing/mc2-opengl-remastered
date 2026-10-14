// vulkan_water_island.cpp - Vulkan water island.
// =============================================================================
// Renders the MC2 armed water fast-path mesh (base layer) in Vulkan. Tier-1
// renders in-place into the shared scene GBuffer; the old P9a side-image gate is
// retired and unreachable.
//
// SHAPE (SSAO-island seam + a real geometry draw, per VK-P9a-STEP0 note):
//   - Device: BORROWED from the shared engine VulkanContext
//     (getOrCreateEngineVulkanContext, like engine_present_vk.cpp:330-337). NO
//     second device, NO volkLoadDevice.
//   - Feed (STEP 0 decision — VK-side copy/re-pack, NOT external buffer share):
//       * recipe  = mission-static, copied ONCE at first-run into a VK SSBO from
//         WaterStream::GetRecipes()/GetRecipeCount().
//       * thin    = per-frame, CPU-staged from the caller's thin-record array
//         each frame into a VK SSBO. No fenced GL buffer crosses the seam -> the
//         GL ring-3 fence (id 43) is irrelevant to VK; R2 sync hazard dissolves.
//   - Pipeline (THIS tree's PipelineRegistry WaterArmed row, :227-249):
//       reverse-Z GEQUAL, depth-TEST on, depth-WRITE on, cull NONE, AlphaBlend
//       SRC_ALPHA/ONE_MINUS_SRC_ALPHA, MRT (color0 MainColor + color1 GBuffer1).
//       MVP fudge lives in the VS (clip.z += 0.00025*clip.w), byte-copied.
//   - Output: the VK pass LOADs scene color0+normal+depth from the shared scene
//     GBuffer, alpha-blends water OVER color0, preserves normal, writes depth, and
//     hands the same shared resources back to GL. Forbidden COLOR-op counters and
//     retired depth-IN counters MUST stay 0.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================

#include "vulkan_water_island_internal.h"

#ifdef MC2_VULKAN_ISLAND



namespace vk_water_detail {

WaterIslandHealth g_health;

void setFallback(std::string_view reason) {
    const size_t n = (std::min)(reason.size(), sizeof(g_health.fallbackReason) - 1);
    std::memcpy(g_health.fallbackReason, reason.data(), n);
    g_health.fallbackReason[n] = '\0';
}

InteropForbidden g_forbidden;
unsigned long g_depthInGetTexImage       = 0;  // EXPECTED (scene depth import)
unsigned long g_depthInCopyBufferToImage = 0;  // EXPECTED (staging -> depth image)

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_WATER_ISLAND] ", fmt, ap);
    va_end(ap);
}

// MC2_VK_WATER_TIER1 (rung 6, VK full-ownership ladder): render the armed water
// BASE layer IN-PLACE into the shared scene GBuffer (color0+normal+depth) like
// the terrain/prop/mech Tier-1 islands — no offscreen target, no GL composite,
// no depth readback.
//
// VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): now resolved via the umbrella
// so MC2_VK_SHARED_GBUFFER=1 arms it. Water is BASE scene
// content, not an enhancement -- post-VK-RUNG8-ITEM8 the native VK composite is the sole
// present-image producer and there is no per-layer GL fallback, so an unarmed water layer
// VANISHES (the field bug). This ALSO reconciles a latent inconsistency: gos_postprocess.cpp
// and the post chain already read the water tier as an umbrella member, while this producer
// read it as an explicit "1" -- so under the umbrella the
// post chain believed water was armed while the island declined. Now both agree. The strict
// proof armed WATER_TIER1=1 and passed its parity check.
bool waterTier1Requested() {
    const bool s_on = mc2vkgate::umbrellaOn();   // per-call: VK-FAILSOFT-DISARM-1 (water must return to GL after fail-soft)
    return s_on;
}

// MC2_VK_WATER_PERF — fine-grained per-sub-step microsecond breakdown of the VK
// water interop path (additive diagnostic; zero cost when OFF). Emits a
// [VK_WATER_PERF] line per frame with the readback/stage/depth-in/sem/submit/
// fence-wait/composite split so the ~11ms Tracy zone can be attributed.
bool waterPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_WATER_PERF">();
    }
    return state == 1;
}

// VK-WATER-FENCE-DEFER-1 (cluster-A pilot, ONE island): defer the island's retire
// wait to the parity slot's next vkImageResourceBeginFrame instead of waiting the
// submit fence inline. Default-ON (the per-parity staging pairs make it correct by
// construction); MC2_VK_WATER_FENCE_DEFER=0 is the killswitch restoring the inline
// wait — the staging stays per-parity either way (superset-safe).
bool waterFenceDeferEnabled() {
    static int state = -1;
    if (state == -1)
        state = mc2::gate::get<"MC2_VK_WATER_FENCE_DEFER">() ? 1 : 0;
    return state == 1;
}

// MC2_VK_WATER_DIRECT_CHAIN -- deletion-readiness proof gate. When the prior
// terrain/prop/mech owners have kept the shared GBuffer Vulkan-owned, water can
// render in-place without another GL/VK semaphore round trip.
// Water co-write is unconditionally direct-chain-native (the GL<->VK
// semaphore-handoff arm and the shared-scene GL-import half are deleted).
bool waterDirectChainRequested() {
    return true;
}

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default shaders/vulkan, empty reads the default
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}



std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Water);
}
WaterIsland* parkedIsland() { return static_cast<WaterIsland*>(islandSlot().get()); }

bool make_buffer(WaterIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage,
                 std::string_view name, int i0, int i1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("buffer creation failed (%.*s).", static_cast<int>(name.size()), name.data()); return false;
    }
    return true;
}

// Write the 4-binding descriptor set (0=vsUbo, 1=fsUbo, 5=recipe, 6=thin).
// Called at init (after the set is allocated) and whenever a buffer is (re)allocated.
// No-op until the descriptor SET exists — ensure_thin_capacity / recipe realloc can
// run during init BEFORE vkAllocateDescriptorSets, and writing to a NULL set is a
// validation error + crash (dstSet == VK_NULL_HANDLE). The post-alloc write_descriptors
// in init_island covers the initial bind; later reallocs re-write with a valid set.
void write_descriptors(WaterIsland* s) {
    // VK-WATER-FENCE-DEFER-1: write BOTH parity sets (set[i] binds buffers[i]).
    // Callers (re)allocating any bound buffer are vkDeviceWaitIdle-guarded, so no
    // in-flight command buffer can be using either set during the update.
    for (int i = 0; i < 2; ++i) {
        if (s->set[i] == VK_NULL_HANDLE) continue;   // set not allocated yet (early init path)
        VkDescriptorBufferInfo vsInfo{ s->vsUbo[i].get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo fsInfo{ s->fsUbo[i].get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo recInfo{ s->recipeBuf.get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo thinInfo{ s->thinBuf[i].get(), 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[4]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set[i];
        w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &vsInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set[i];
        w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &fsInfo;
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = s->set[i];
        w[2].dstBinding = 5; w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &recInfo;
        w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = s->set[i];
        w[3].dstBinding = 6; w[3].descriptorCount = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &thinInfo;
        // Binding 2 (HDRI equirect) is written separately by ensure_hdri() once the
        // image exists — it may not yet at init time (write of a NULL view is invalid).
        VkDescriptorImageInfo hdriInfo{};
        VkWriteDescriptorSet w4{};
        if (s->hdriView && s->hdriSampler) {
            hdriInfo = { s->hdriSampler.get(), s->hdriView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            w4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w4.dstSet = s->set[i];
            w4.dstBinding = 2; w4.descriptorCount = 1;
            w4.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w4.pImageInfo = &hdriInfo;
            vkUpdateDescriptorSets(s->device, 1, &w4, 0, nullptr);
        }
        vkUpdateDescriptorSets(s->device, 4, w, 0, nullptr);
        // VK-PARITY-WATER-SHORE-1: bindings 3/4 (shore height / mask), once imported.
        if (s->shHView && s->shMView && s->hdriSampler) {
            VkDescriptorImageInfo shInfo[2] = {
                { s->hdriSampler.get(), s->shHView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { s->hdriSampler.get(), s->shMView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet ws[2]{};
            for (int k = 0; k < 2; ++k) {
                ws[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; ws[k].dstSet = s->set[i];
                ws[k].dstBinding = 3 + k; ws[k].descriptorCount = 1;
                ws[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                ws[k].pImageInfo = &shInfo[k];
            }
            vkUpdateDescriptorSets(s->device, 2, ws, 0, nullptr);
        }
    }
}

// Grow slot `par`'s thin SSBO to at least `bytes`; rewrite descriptors on realloc.
bool ensure_thin_capacity(WaterIsland* s, uint32_t par, VkDeviceSize bytes) {
    if (s->thinBuf[par] && bytes <= s->thinCapBytes[par]) return true;
    if (s->thinBuf[par]) {
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("water_thin_grow");   // simple + safe for a realloc (rare, grows once)
        s->thinBuf[par].reset();
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096; // headroom
    if (!make_buffer(s, s->thinBuf[par], cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "water.thinBuf", static_cast<int>(par)))
        return false;
    s->thinCapBytes[par] = cap;
    write_descriptors(s);
    return true;
}

// RUNG-6.5: one-shot transient command submit (import-time only, fence-waited).
bool submit_once(WaterIsland* s, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cai, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool ok = (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS);
    if (ok) { record(cb); ok = (vkEndCommandBuffer(cb) == VK_SUCCESS); }
    if (ok) {
        VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        mc2vk::OwnedFence fence;
        ok = mc2vk::create(fence, s->device, [&](VkFence* h) { return vkCreateFence(s->device, &fci, nullptr, h); });
        if (ok) {
            VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            const VkFence f = fence.get();
            ok = (vkQueueSubmit(s->queue, 1, &si, f) == VK_SUCCESS) &&
                 (vkWaitForFences(s->device, 1, &f, VK_TRUE, 5ull*1000*1000*1000) == VK_SUCCESS);
        }
    }
    vkFreeCommandBuffers(s->device, s->cpool.get(), 1, &cb);
    return ok;
}

}  // namespace vk_water_detail

// ===========================================================================
// PUBLIC ENTRY POINTS (consumed under #ifdef MC2_VULKAN_ISLAND from
// gameos_graphics.cpp:renderWaterFastPath + debug_state_dump.cpp).
// ===========================================================================

// Health accessor for debug_state_dump.cpp.
extern "C" void mc2_render_backend_water_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* thinRecords, unsigned long* recipeCount,
    unsigned long* forbiddenCopyBufToImg, unsigned long* forbiddenCopyImgToBuf,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenTexSubImage,
    unsigned long* depthInGetTexImage, unsigned long* depthInCopyBufToImg,
    double* copyColorInUs, double* copyDepthInUs, double* drawUs, double* colorOutUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (validationErrors) *validationErrors = g_health.validationErrors;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (interopActive)    *interopActive    = g_health.interopActive;
    if (vk2glSemActive)   *vk2glSemActive   = g_health.vk2glSemActive;
    if (usedInterop)      *usedInterop      = g_health.usedInterop;
    if (thinRecords)      *thinRecords      = g_health.thinRecords;
    if (recipeCount)      *recipeCount      = g_health.recipeCount;
    if (forbiddenCopyBufToImg) *forbiddenCopyBufToImg = g_forbidden.vkCmdCopyBufferToImage;
    if (forbiddenCopyImgToBuf) *forbiddenCopyImgToBuf = g_forbidden.vkCmdCopyImageToBuffer;
    if (forbiddenGetTexImage)  *forbiddenGetTexImage  = g_forbidden.glGetTexImage;
    if (forbiddenTexSubImage)  *forbiddenTexSubImage  = g_forbidden.glTexSubImage2D;
    if (depthInGetTexImage)    *depthInGetTexImage    = g_depthInGetTexImage;
    if (depthInCopyBufToImg)   *depthInCopyBufToImg   = g_depthInCopyBufferToImage;
    if (copyColorInUs)    *copyColorInUs    = g_health.copyColorInUs;
    if (copyDepthInUs)    *copyDepthInUs    = g_health.copyDepthInUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (colorOutUs)       *colorOutUs       = g_health.colorOutUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

// True when the Tier-1 gate requests the VK water region (the caller then SKIPS the GL
// MDI draw and calls mc2_vk_water_island_run instead — mutually exclusive).
extern "C" int mc2_vk_water_island_enabled() {
    return waterTier1Requested() ? 1 : 0;
}

// Teardown (called from gosPostProcess::destroy or renderer shutdown).
extern "C" void mc2_vk_water_island_destroy() {
    vlog("[VK_WATER_TIER1] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d validation_errors=%lu thin_records=%lu recipe_count=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu forbidden_copyBufToImg=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_texSubImage=%lu depthin_getTexImage=%lu depthin_copyBufToImg=%lu "
         "copy_color_in_us=%.1f copy_depth_in_us=%.1f draw_us=%.1f color_out_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.validationErrors, g_health.thinRecords, g_health.recipeCount,
         g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyBufferToImage, g_forbidden.vkCmdCopyImageToBuffer,
         g_forbidden.glGetTexImage, g_forbidden.glTexSubImage2D,
         g_depthInGetTexImage, g_depthInCopyBufferToImage,
         g_health.copyColorInUs, g_health.copyDepthInUs, g_health.drawUs, g_health.colorOutUs, g_health.totalUs);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Water);
}

#endif // MC2_VULKAN_ISLAND
