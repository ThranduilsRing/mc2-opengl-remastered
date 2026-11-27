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

#include <bit>
#include "vulkan_water_island_internal.h"
#include "vk_one_shot.h"   // STEP 3.4P: recipe upload
#include "hitch_stats.h"

void gos_SetWaterFsDebugMode(int);




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

// MC2_VK_WATER_PERF — fine-grained per-sub-step microsecond breakdown of the VK
// water interop path (additive diagnostic; zero cost when OFF). Emits a
// [VK_WATER_PERF] line per frame with the readback/stage/depth-in/sem/submit/
// fence-wait/composite split so the ~11ms Tracy zone can be attributed.
bool waterPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = false;
    }
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

std::string spv_dir() { return vk_island_util::spvDir(); }

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
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;   // per-frame GPU-read feed: ReBAR device-local where available
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("buffer creation failed (%.*s).", static_cast<int>(name.size()), name.data()); return false;
    }
    return true;
}

// Write the descriptor set (0=vsUbo, 1=fsUbo: frame-ring dynamic bindings; 5=recipe, 6=thin).
// Called at init (after the set is allocated) and whenever a buffer is (re)allocated.
// No-op until the descriptor SET exists — ensure_thin_capacity / recipe realloc can
// run during init BEFORE vkAllocateDescriptorSets, and writing to a NULL set is a
// validation error + crash (dstSet == VK_NULL_HANDLE). The post-alloc write_descriptors
// in init_island covers the initial bind; later reallocs re-write with a valid set.
void write_descriptors(WaterIsland* s, int only) {
    if (only<0) { for (auto& dirty : s->imageDescriptorsDirty) dirty = true; return; }
    // Resource changes mark both slots dirty; each slot is written only after its fence.
    for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
        if (i!=only) continue;
        s->imageDescriptorsDirty[i]=false;
        if (s->set[i] == VK_NULL_HANDLE) continue;   // set not allocated yet (early init path)
        // Bindings 0 and 1 (the parameter UBOs) at the frame ring's current buffer; the slot re-points them at its next frame (ringBound cleared).
        if (const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer()) {
            mc2vk::writeDynamicBinding(s->device, s->set[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, kWaterVsUboSize);
            mc2vk::writeDynamicBinding(s->device, s->set[i], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, sizeof(WaterFsParams));
        }
        s->ringBound[i] = VK_NULL_HANDLE;
        VkDescriptorBufferInfo recInfo{ s->recipeBuf.get(), 0, VK_WHOLE_SIZE };
        // STEP 3.4P: binding 6 is the GPU-produced thin buffer only for the slot whose last feed was the native one (thinNative[i]);
        // a frame that takes the readback feed (opt-out, null MVP) rebinds its slot to thinBuf first (vulkan_water_island_run.cpp).
        VkDescriptorBufferInfo thinInfo{ (s->thinNative[i] && s->native && s->native->ready()) ? s->native->thin(static_cast<uint32_t>(i)) : s->thinBuf[i].get(), 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set[i];
        w[0].dstBinding = 5; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &recInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set[i];
        w[1].dstBinding = 6; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &thinInfo;
        // Binding 2 (HDRI equirect) is written separately by ensure_hdri() once the
        // image exists — it may not yet at init time (write of a NULL view is invalid).
        VkDescriptorImageInfo hdriInfo{};
        VkWriteDescriptorSet w4{};
        mc2_vk_sky_sample_info(&hdriInfo);
        if (hdriInfo.imageView && s->hdriSampler) {
            hdriInfo.sampler = s->hdriSampler.get();
            w4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w4.dstSet = s->set[i];
            w4.dstBinding = 2; w4.descriptorCount = 1;
            w4.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w4.pImageInfo = &hdriInfo;
            vkUpdateDescriptorSets(s->device, 1, &w4, 0, nullptr);
        }
        vkUpdateDescriptorSets(s->device, 2, w, 0, nullptr);
        if (s->fftSurface[i]) {
            const VkDescriptorBufferInfo fft{s->fftSurface[i].get(),0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet=s->set[i]; write.dstBinding=19; write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&fft;
            vkUpdateDescriptorSets(s->device,1,&write,0,nullptr);
        }
        if (s->foamTexture && s->hdriSampler) {
            const VkDescriptorImageInfo foam{s->hdriSampler.get(), s->foamTexture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = s->set[i]; write.dstBinding = 18; write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &foam;
            vkUpdateDescriptorSets(s->device, 1, &write, 0, nullptr);
        }
        const VkDescriptorBufferInfo surfaceInfo{s->shoreSurface ? s->shoreSurface.get() : s->recipeBuf.get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet surfaceWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        surfaceWrite.dstSet = s->set[i]; surfaceWrite.dstBinding = 14;
        surfaceWrite.descriptorCount = 1; surfaceWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        surfaceWrite.pBufferInfo = &surfaceInfo;
        if (surfaceInfo.buffer) vkUpdateDescriptorSets(s->device, 1, &surfaceWrite, 0, nullptr);
        if (s->reflectionView && s->hdriSampler) {
            const VkDescriptorImageInfo images[]{
                {s->hdriSampler.get(),s->reflectionView.get(),VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE,s->reflectionView.get(),VK_IMAGE_LAYOUT_GENERAL},
                {s->hdriSampler.get(),s->reflectionView.get(),VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[3]{};
            for (int k=0;k<3;++k) {
                writes[k].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[k].dstSet=s->set[i];
                writes[k].dstBinding=k==0 ? 8 : k==1 ? 11 : 9; writes[k].descriptorCount=1; writes[k].pImageInfo=&images[k];
                writes[k].descriptorType=k==1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            vkUpdateDescriptorSets(s->device,3,writes,0,nullptr);
        }
        // VK-PARITY-WATER-SHORE-1: bindings 3/4 (shore height / mask), once imported.
        if (s->shHTexture && s->shMTexture && s->hdriSampler) {
            VkDescriptorImageInfo shInfo[2] = {
                { s->hdriSampler.get(), s->shHTexture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { s->hdriSampler.get(), s->shMTexture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet ws[2]{};
            for (int k = 0; k < 2; ++k) {
                ws[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; ws[k].dstSet = s->set[i];
                ws[k].dstBinding = 3 + k; ws[k].descriptorCount = 1;
                ws[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                ws[k].pImageInfo = &shInfo[k];
            }
            vkUpdateDescriptorSets(s->device, 2, ws, 0, nullptr);
        }
        // Binding 7: the bake mask once the bake is ready (the linear, mipped, clamped sampler suits it); until then any valid 2D view, which the
        // shader does not read (F.f7.z = 0).
        const VkImageView maskOrStandIn = s->maskView != VK_NULL_HANDLE ? s->maskView : (s->hdriTexture ? s->hdriTexture.view() : VK_NULL_HANDLE);
        if (maskOrStandIn != VK_NULL_HANDLE && s->hdriSampler) {
            const VkDescriptorImageInfo maskInfo{ s->hdriSampler.get(), maskOrStandIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet wm{};
            wm.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm.dstSet = s->set[i];
            wm.dstBinding = 7; wm.descriptorCount = 1;
            wm.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wm.pImageInfo = &maskInfo;
            vkUpdateDescriptorSets(s->device, 1, &wm, 0, nullptr);
        }
    }
}

// Grow slot `par`'s thin SSBO to at least `bytes`; rewrite descriptors on realloc.
bool ensure_thin_capacity(WaterIsland* s, uint32_t par, VkDeviceSize bytes) {
    if (s->thinBuf[par] && bytes <= s->thinCapBytes[par]) return true;
    // Power-of-two capacity; the old buffer retires on the open-frame ticket (the slot's in-flight submit may still read it).
    if (s->thinBuf[par]) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->thinBuf[par]), s->thinCapBytes[par]);
    const VkDeviceSize cap = std::bit_ceil(static_cast<uint64_t>(bytes + bytes / 2 + 4096));
    if (!make_buffer(s, s->thinBuf[par], cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "water.thinBuf", static_cast<int>(par)))
        return false;
    s->thinCapBytes[par] = cap;
    write_descriptors(s);
    return true;
}

// STEP 3.4P: the mission-static recipes (64 B each) copied once into a device-local storage buffer (staging + one-shot copy). The native cull reads it every
// frame and the water vertex stage reads it for the visible records, so it must not live in host memory.
bool upload_recipes_device(WaterIsland* s, const void* recipes, uint32_t recipeCount) {
    const VkDeviceSize need = static_cast<VkDeviceSize>(recipeCount) * 64;
    if (!recipes || need == 0) return false;
    mc2vk::OwnedBuffer staging;
    if (!staging.createFilled(s->allocator, s->device, recipes, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "water.recipeStage")) {
        vlog("recipe staging failed."); return false;
    }
    if (s->recipeBuf) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->recipeBuf), s->recipeBytes);   // in-flight frames may still read the old table
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = need; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!s->recipeBuf.create(s->allocator, s->device, bci, aci, "water.recipeBuf")) { vlog("recipe device buffer failed."); return false; }
    s->recipeBytes = need;
    const VkBuffer src = staging.get(), dst = s->recipeBuf.get();
    if (!mc2vk::submitOneShot(mc2vk::getOrCreateEngineVulkanContext(), [&](VkCommandBuffer cb) {
            VkBufferCopy c{0, 0, need};
            vkCmdCopyBuffer(cb, src, dst, 1, &c);
        }, "water_recipe_upload")) {
        vlog("recipe copy failed."); return false;
    }
    write_descriptors(s);
    return true;
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
    return 1;
}

// Teardown (called from gosPostProcess::destroy or renderer shutdown).
extern "C" void mc2_vk_water_island_destroy() {
    if (const WaterIsland* parked = parkedIsland(); parked && parked->profileIssued < 2400)
        gos_SetWaterFsDebugMode(0);
    vlog("[VK_WATER_TIER1] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d validation_errors=%lu thin_records=%lu recipe_count=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu forbidden_copyBufToImg=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_texSubImage=%lu depthin_getTexImage=%lu depthin_copyBufToImg=%lu "
         "copy_color_in_us=%.1f copy_depth_in_us=%.1f draw_us=%.1f color_out_us=%.1f total_us=%.1f "
         "water_visible=%lu water_eligible_bound=%lu",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.validationErrors, g_health.thinRecords, g_health.recipeCount,
         g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyBufferToImage, g_forbidden.vkCmdCopyImageToBuffer,
         g_forbidden.glGetTexImage, g_forbidden.glTexSubImage2D,
         g_depthInGetTexImage, g_depthInCopyBufferToImage,
         g_health.copyColorInUs, g_health.copyDepthInUs, g_health.drawUs, g_health.colorOutUs, g_health.totalUs,
         g_health.waterVisible, g_health.waterEligibleBound);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Water);
}

