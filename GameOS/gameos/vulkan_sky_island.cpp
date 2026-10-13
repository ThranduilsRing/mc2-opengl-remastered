// vulkan_sky_island.cpp -- VK-PARITY-F3: HDRI equirect sky co-write. See header.
// Structure follows vulkan_legacy_shape_island.cpp (shared-GBuffer LOAD pass,
// parity-fenced submit) and vulkan_water_island.cpp ensure_hdri (copy-once import).
#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include <GL/glew.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#include "vulkan_sky_island.h"
#include "vk_shared_gbuffer_gate.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <utility>
#include <functional>
#include <tracy/Tracy.hpp>

namespace {

struct Health { unsigned long attempted = 0, usedVulkan = 0; char fallbackReason[128] = {0}; };
Health g_health;
void setFallback(const char* r) {
    std::strncpy(g_health.fallbackReason, r ? r : "", sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = 0;
}
void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_SKY] ", fmt, ap);
    va_end(ap);
}

bool gateRequested() {
    static int state = -1;
    if (state == -1) state = mc2vkgate::umbrellaOn() ? 1 : 0;
    return state == 1;
}

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default shaders/vulkan, empty reads the default
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog, true);
}

// LOCKSTEP with hdri_sky.frag SkyPC (push constant).
struct SkyPC {
    float invProj[16];
    float rotC0[4], rotC1[4], rotC2[4];
    float skyYaw;
};
static_assert(sizeof(SkyPC) == 116, "SkyPC lockstep");

struct Island {
    bool inited = false, disabled = false;
    int  w = 0, h = 0;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE; uint32_t gfxFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE; VmaAllocator allocator = VK_NULL_HANDLE;
    VkRenderPass rpass = VK_NULL_HANDLE; VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE; VkPipelineLayout playout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    struct VkImageResource* colorRes = nullptr;
    struct VkImageResource* depthRes = nullptr;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkImage hdriImg = VK_NULL_HANDLE; VmaAllocation hdriAlloc = VK_NULL_HANDLE;
    VkImageView hdriView = VK_NULL_HANDLE;
    unsigned int hdriGlTex = 0; bool hdriAttempted = false, hdriReal = false;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer cbuf[2] = {};
};

Island* g_island = nullptr;
bool g_sessionDeclined = false;
int  g_initReachedBuild = 0;

struct Pending { bool valid = false; float invProj[16] = {}; float rot[9] = {}; float yaw = 0.f; unsigned int tex = 0; };
Pending g_pending;

bool make_buffer(Island* s, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VmaAllocation* alloc,
                 std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO; aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) return false;
    mc2vk::nameObjectAt(s->device, *buf, name, i0, i1);
    return true;
}

void destroy_hdri(Island* s) {
    if (s->hdriView) vkDestroyImageView(s->device, s->hdriView, nullptr);
    if (s->hdriImg)  vmaDestroyImage(s->allocator, s->hdriImg, s->hdriAlloc);
    s->hdriView = VK_NULL_HANDLE; s->hdriImg = VK_NULL_HANDLE; s->hdriAlloc = VK_NULL_HANDLE;
}

void destroy_island(Island* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->fb)      vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    destroy_hdri(s);
    if (s->pool)  vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->dsl)   vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->samp)  vkDestroySampler(d, s->samp, nullptr);
    if (s->cpool) vkDestroyCommandPool(d, s->cpool, nullptr);
}

void update_set(Island* s) {
    VkDescriptorImageInfo ii{};
    ii.sampler = s->samp; ii.imageView = s->hdriView; ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = s->set; w.dstBinding = 0;
    w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
}

bool submit_once(Island* s, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(s->device, &fci, nullptr, &f) != VK_SUCCESS) f = VK_NULL_HANDLE;
    bool ok = vkQueueSubmit(s->queue, 1, &si, f) == VK_SUCCESS;
    if (f != VK_NULL_HANDLE) {
        if (ok) ok = vkWaitForFences(s->device, 1, &f, VK_TRUE, 30ull * 1000 * 1000 * 1000) == VK_SUCCESS;
        vkDestroyFence(s->device, f, nullptr);
    } else { vkQueueWaitIdle(s->queue); }
    vkFreeCommandBuffers(s->device, s->cpool, 1, &cb);
    return ok;
}

// Copy-once HDRI import (mirrors vulkan_water_island.cpp ensure_hdri). Called on the GL
// thread BEFORE recording. On failure/absence a 1x1 black dummy is bound (sky = black,
// the same as GL's "no HDRI" baseline).
bool ensure_hdri(Island* s, unsigned int glTex) {
    if (s->hdriAttempted && s->hdriGlTex == glTex) return s->hdriView != VK_NULL_HANDLE;
    s->hdriAttempted = true; s->hdriGlTex = glTex; s->hdriReal = false;
    if (s->hdriView || s->hdriImg) { vkDeviceWaitIdle(s->device); destroy_hdri(s); }

    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    int w = 1, h = 1; uint32_t mips = 1;
    std::vector<std::vector<unsigned char>> levels;
    std::vector<std::pair<int,int>> dims;
    bool haveReal = false;
    if (glTex != 0) {
        GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
        GLint ifmt = 0, compressed = 0, w0 = 0, h0 = 0, maxLevel = 1000;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        if (w0 > 0 && h0 > 0) {
            if (compressed && (ifmt == GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT ||
                               ifmt == GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT)) {
                VkPhysicalDeviceFeatures feats{};
                vkGetPhysicalDeviceFeatures(s->phys, &feats);
                if (feats.textureCompressionBC) {
                    fmt = (ifmt == GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT)
                        ? VK_FORMAT_BC6H_SFLOAT_BLOCK : VK_FORMAT_BC6H_UFLOAT_BLOCK;
                    GLint sz = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &sz);
                    if (sz > 0) {
                        levels.emplace_back((size_t)sz);
                        glGetCompressedTexImage(GL_TEXTURE_2D, 0, levels[0].data());
                        dims.emplace_back(w0, h0);
                        w = w0; h = h0; mips = 1;
                        haveReal = (glGetError() == GL_NO_ERROR);
                    }
                } else vlog("device lacks textureCompressionBC -- black sky.");
            } else if (ifmt == GL_RGBA16F) {
                fmt = VK_FORMAT_R16G16B16A16_SFLOAT; w = w0; h = h0;
                for (int lvl = 0; lvl <= maxLevel; ++lvl) {
                    GLint tw = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, lvl, GL_TEXTURE_WIDTH, &tw);
                    if (tw <= 0) break;
                    const int lw = (w0 >> lvl) > 0 ? (w0 >> lvl) : 1;
                    const int lh = (h0 >> lvl) > 0 ? (h0 >> lvl) : 1;
                    levels.emplace_back((size_t)lw * lh * 8);
                    glGetTexImage(GL_TEXTURE_2D, lvl, GL_RGBA, GL_HALF_FLOAT, levels.back().data());
                    dims.emplace_back(lw, lh);
                    if (lw == 1 && lh == 1) break;
                }
                mips = (uint32_t)levels.size();
                haveReal = (mips > 0) && (glGetError() == GL_NO_ERROR);
            } else vlog("unexpected GL HDRI internal format 0x%x -- black sky.", ifmt);
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    }
    if (!haveReal) {
        fmt = VK_FORMAT_R16G16B16A16_SFLOAT; w = 1; h = 1; mips = 1;
        levels.assign(1, std::vector<unsigned char>(8, 0));
        dims.assign(1, {1, 1});
    }

    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {(uint32_t)w, (uint32_t)h, 1};
        ii.mipLevels = mips; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(s->allocator, &ii, &aci, &s->hdriImg, &s->hdriAlloc, nullptr) != VK_SUCCESS) return false;
        mc2vk::nameObject(s->device, s->hdriImg, "sky.hdri");
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->hdriImg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        if (vkCreateImageView(s->device, &vi, nullptr, &s->hdriView) != VK_SUCCESS) return false;
    }

    std::vector<VkDeviceSize> offs(levels.size());
    VkDeviceSize total = 0;
    for (size_t i = 0; i < levels.size(); ++i) { offs[i] = total; total += (VkDeviceSize)((levels[i].size() + 15) & ~size_t(15)); }
    VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingAlloc, "sky.staging")) return false;
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, stagingAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, staging, stagingAlloc); return false; }
        for (size_t i = 0; i < levels.size(); ++i) std::memcpy((unsigned char*)p + offs[i], levels[i].data(), levels[i].size());
        vmaUnmapMemory(s->allocator, stagingAlloc);
    }
    const bool ok = submit_once(s, [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = s->hdriImg; bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        for (uint32_t i = 0; i < mips; ++i) {
            VkBufferImageCopy c{};
            c.bufferOffset = offs[i];
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            c.imageExtent = {(uint32_t)dims[i].first, (uint32_t)dims[i].second, 1};
            vkCmdCopyBufferToImage(cb, staging, s->hdriImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
    vmaDestroyBuffer(s->allocator, staging, stagingAlloc);
    if (!ok) { vlog("HDRI upload submit failed."); return false; }
    s->hdriReal = haveReal;
    vkDeviceWaitIdle(s->device);   // rare (<=2x per session): descriptor rewrite safety
    update_set(s);
    vlog("HDRI %s import: %dx%d fmt=%d mips=%u glTex=%u (copy-once).", haveReal ? "REAL" : "DUMMY", w, h, (int)fmt, mips, glTex);
    return true;
}

bool init_island(Island* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_initReachedBuild = 0;
    if (!mc2vk::ensureEngineVulkanContext()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->phys = ctx.physicalDevice(); s->device = ctx.device(); s->queue = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily(); s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) { setFallback("device_or_allocator_null"); return false; }
    s->colorRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->depthRes = mc2SharedSceneDepthGetResource();
    if (!s->colorRes || !s->depthRes) { setFallback("shared_gbuffer_not_ready"); return false; }
    g_initReachedBuild = 1;

    {
        VkCommandPoolCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = s->gfxFamily;
        if (vkCreateCommandPool(s->device, &cpi, nullptr, &s->cpool) != VK_SUCCESS) { setFallback("cmdpool_failed"); return false; }
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = s->cpool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cbi, s->cbuf) != VK_SUCCESS) { setFallback("cmdbuf_failed"); return false; }
    }
    {   // sampler: same as the water island's HDRI sampler (linear, clamp, full mip range)
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(s->device, &sci, nullptr, &s->samp) != VK_SUCCESS) { setFallback("sampler_failed"); return false; }
    }
    {   // render pass: LOAD color0 + depth (depth is read-only in use; stored unchanged)
        VkImageResource* colorRes = reinterpret_cast<VkImageResource*>(s->colorRes);
        VkImageResource* depthRes = reinterpret_cast<VkImageResource*>(s->depthRes);
        VkAttachmentDescription att[2]{};
        att[0].format = colorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = depthRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef; sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; deps[1].dstAccessMask = 0;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 2; rpci.pAttachments = att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) { setFallback("renderpass_failed"); return false; }
        VkImageView fbViews[2] = { colorRes->view, depthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 2; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) { setFallback("framebuffer_failed"); return false; }
    }
    {   // descriptor: binding 0 = HDRI sampler
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lci.bindingCount = 1; lci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) { setFallback("dsl_failed"); return false; }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) { setFallback("pool_failed"); return false; }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &s->dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) { setFallback("set_failed"); return false; }
    }
    {   // pipeline
        s->vert = load_spv(s->device, spv_dir() + "hdri_sky.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "hdri_sky.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPC)};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) { setFallback("playout_failed"); return false; }
        VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag; stages[1].pName = "main";
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb; gp.pDynamicState = &dsci;
        gp.layout = s->playout; gp.renderPass = s->rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe) != VK_SUCCESS) { setFallback("pipeline_failed"); return false; }
        mc2vk::nameObject(s->device, s->pipe, "sky.pipe");
    }
    s->inited = true; setFallback("");
    vlog("island initialized %dx%d (HDRI equirect sky -> shared color0; GEQUAL z=0, no depth write).", w, h);
    return true;
}

} // namespace

extern "C" int mc2_vk_sky_island_enabled(void) { return (gateRequested() && !g_sessionDeclined) ? 1 : 0; }

extern "C" void mc2_vk_sky_submit(const float* invProj16, const float* invViewRot9, float skyYaw, unsigned int hdriGlTex) {
    if (!invProj16 || !invViewRot9) return;
    std::memcpy(g_pending.invProj, invProj16, sizeof(g_pending.invProj));
    std::memcpy(g_pending.rot, invViewRot9, sizeof(g_pending.rot));
    g_pending.yaw = skyYaw; g_pending.tex = hdriGlTex; g_pending.valid = true;
}

extern "C" int mc2_vk_sky_island_run(int w, int h) {
    ZoneScopedN("VK-sky-island");
    Pending p = g_pending;
    g_pending.valid = false;   // consume: a frame with no sky submit draws no sky
    if (!gateRequested() || g_sessionDeclined || !p.valid || w <= 0 || h <= 0) return 0;

    if (g_island && (g_island->w != w || g_island->h != h)) { destroy_island(g_island); delete g_island; g_island = nullptr; }
    if (!g_island) {
        g_island = new Island();
        if (!init_island(g_island, w, h)) {
            const bool permanent = (g_initReachedBuild != 0);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            if (permanent) { g_sessionDeclined = true; vlog("permanent init failure (%s) -- flat clear sky.", g_health.fallbackReason); }
            return 0;
        }
    }
    Island* s = g_island;
    if (s->disabled || !s->inited) return 0;
    if (!ensure_hdri(s, p.tex) || s->hdriView == VK_NULL_HANDLE) { setFallback("hdri_import_failed"); return 0; }

    VkImageResource* colorRes = reinterpret_cast<VkImageResource*>(s->colorRes);
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Sky.FenceWait"); fsC = vkImageResourceBeginFrame(colorRes); }
    const uint32_t par = fsC.parity & 1u;

    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    SkyPC pc{};
    std::memcpy(pc.invProj, p.invProj, sizeof(pc.invProj));
    for (int i = 0; i < 3; ++i) { pc.rotC0[i] = p.rot[i]; pc.rotC1[i] = p.rot[3 + i]; pc.rotC2[i] = p.rot[6 + i]; }
    pc.skyYaw = p.yaw;
    vkCmdPushConstants(cb, s->playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Sky.Submit");
      if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    ++g_health.usedVulkan;
    static bool s_firstOk = false;
    if (!s_firstOk) { vlog("first HDRI sky co-write (hdriReal=%d).", s->hdriReal ? 1 : 0); s_firstOk = true; }
    return 1;
}

extern "C" void mc2_vk_sky_island_destroy(void) {
    vlog("used_vulkan=%lu fallback_reason=\"%s\"", g_health.usedVulkan, g_health.fallbackReason);
    if (!g_island) return;
    destroy_island(g_island); delete g_island; g_island = nullptr;
}

#endif // MC2_VULKAN_ISLAND
