// tool_vk_gpu -- see tool_vk_gpu.h.
#include "tool_vk_gpu.h"

#include <SDL.h>
#include "backends/imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace toolvk {

namespace {

struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet imgui = VK_NULL_HANDLE;
    int w = 0, h = 0;
    bool repeat = true;
};

struct TargetImpl {
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
};

struct PipelineImpl {
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t samplers = 0;
};

struct Gpu {
    Shell* s = nullptr;
    VkSampler repeatSampler = VK_NULL_HANDLE;
    VkSampler clampSampler = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer uboBuf = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    uint8_t* uboMap = nullptr;
    size_t uboCursor = 0;
    size_t uboAlign = 256;
    VkPhysicalDeviceMemoryProperties memProps{};
    bool bc7 = false;
    uint32_t nextId = 1;
    std::unordered_map<uint32_t, Tex> texs;
    uint32_t whiteId = 0;
};

Gpu g;
constexpr size_t kUboBytes = 1u << 20;
constexpr uint32_t kMaxSets = 4096;

uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& b, VkDeviceMemory& m) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.s->dev, &bi, nullptr, &b) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.s->dev, b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemType(mr.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.s->dev, &ai, nullptr, &m) != VK_SUCCESS) {
        vkDestroyBuffer(g.s->dev, b, nullptr); b = VK_NULL_HANDLE; return false;
    }
    vkBindBufferMemory(g.s->dev, b, m, 0);
    return true;
}

bool makeImage(int w, int h, uint32_t mips, VkFormat fmt, VkImageUsageFlags usage, VkImage& img, VkDeviceMemory& mem) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = mips; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g.s->dev, &ii, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.s->dev, img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.s->dev, &ai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyImage(g.s->dev, img, nullptr); img = VK_NULL_HANDLE; return false;
    }
    vkBindImageMemory(g.s->dev, img, mem, 0);
    return true;
}

VkImageView makeView(VkImage img, VkFormat fmt, uint32_t mips, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {aspect, 0, mips, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(g.s->dev, &vi, nullptr, &v);
    return v;
}

// Records into a fresh one-shot command buffer, submits, waits.
template <class F> bool oneShot(F&& rec) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.s->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g.s->dev, &ai, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    rec(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(g.s->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.s->queue);
    vkFreeCommandBuffers(g.s->dev, g.s->pool, 1, &cb);
    return true;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t baseMip, uint32_t mips,
             VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcA; b.dstAccessMask = dstA; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, 1};
    vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkFormat vkFmt(TexFormat f) {
    switch (f) {
    case TexFormat::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case TexFormat::Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case TexFormat::Bc7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case TexFormat::Bc7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

bool readFile(const std::string& path, std::vector<uint32_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize((n + 3) / 4);
    size_t got = std::fread(out.data(), 1, (size_t)n, f);
    std::fclose(f);
    return got == (size_t)n && n > 0;
}

VkShaderModule loadModule(const std::string& file) {
    std::vector<uint32_t> code;
    const std::string path = shaderDir() + file;
    if (!readFile(path, code)) { std::fprintf(stderr, "[TOOLVK] cannot read %s\n", path.c_str()); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size() * 4; mi.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g.s->dev, &mi, nullptr, &m);
    return m;
}

} // namespace

std::string shaderDir() {
    if (const char* e = std::getenv("MC2_TOOL_SHADER_DIR")) {
        std::string d = e;
        if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
        return d;
    }
    std::string d;
    if (char* b = SDL_GetBasePath()) { d = b; SDL_free(b); }
    return d + "shaders_vk/";
}

bool gpuReady() { return g.s != nullptr; }
bool hasBc7() { return g.bc7; }

bool gpuInit(Shell* s) {
    if (!s || !s->dev) return false;
    g.s = s;
    vkGetPhysicalDeviceMemoryProperties(s->phys, &g.memProps);
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(s->phys, &feats);
    g.bc7 = feats.textureCompressionBC != VK_FALSE;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(s->phys, &pp);
    g.uboAlign = std::max<size_t>(256, (size_t)pp.limits.minUniformBufferOffsetAlignment);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (feats.samplerAnisotropy) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 8.0f; }
    if (vkCreateSampler(s->dev, &si, nullptr, &g.repeatSampler) != VK_SUCCESS) return false;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.anisotropyEnable = VK_FALSE;
    if (vkCreateSampler(s->dev, &si, nullptr, &g.clampSampler) != VK_SUCCESS) return false;

    // Offscreen pass: RGBA8 color (-> shader read) + D32 depth.
    VkAttachmentDescription at[2]{};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM; at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1].format = VK_FORMAT_D32_SFLOAT; at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &cref; sp.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2; rp.pAttachments = at; rp.subpassCount = 1; rp.pSubpasses = &sp; rp.dependencyCount = 2; rp.pDependencies = dep;
    if (vkCreateRenderPass(s->dev, &rp, nullptr, &g.pass) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxSets}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets * 4}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kMaxSets; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(s->dev, &pi, nullptr, &g.pool) != VK_SUCCESS) return false;

    if (!makeBuffer(kUboBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g.uboBuf, g.uboMem)) return false;
    void* mp = nullptr;
    vkMapMemory(s->dev, g.uboMem, 0, kUboBytes, 0, &mp);
    g.uboMap = static_cast<uint8_t*>(mp);

    const uint8_t white[4] = {255, 255, 255, 255};
    g.whiteId = createTexture(1, 1, TexFormat::Rgba8Unorm, white, {}, false, true);
    return g.whiteId != 0;
}

void gpuShutdown() {
    if (!g.s) return;
    vkDeviceWaitIdle(g.s->dev);
    std::vector<uint32_t> ids;
    for (auto& kv : g.texs) ids.push_back(kv.first);
    for (uint32_t id : ids) destroyTexture(id);
    if (g.uboMap) vkUnmapMemory(g.s->dev, g.uboMem);
    if (g.uboBuf) vkDestroyBuffer(g.s->dev, g.uboBuf, nullptr);
    if (g.uboMem) vkFreeMemory(g.s->dev, g.uboMem, nullptr);
    if (g.pool) vkDestroyDescriptorPool(g.s->dev, g.pool, nullptr);
    if (g.pass) vkDestroyRenderPass(g.s->dev, g.pass, nullptr);
    if (g.repeatSampler) vkDestroySampler(g.s->dev, g.repeatSampler, nullptr);
    if (g.clampSampler) vkDestroySampler(g.s->dev, g.clampSampler, nullptr);
    g = Gpu{};
}

// ---------------------------------------------------------------- textures
uint32_t createTexture(int w, int h, TexFormat fmt, const void* data, const std::vector<MipLevel>& levels, bool genMips, bool repeat) {
    if (!g.s || w < 1 || h < 1 || !data) return 0;
    const bool bc7 = fmt == TexFormat::Bc7Unorm || fmt == TexFormat::Bc7Srgb;
    if (bc7 && !g.bc7) { std::fprintf(stderr, "[TOOLVK] BC7 textures need textureCompressionBC\n"); return 0; }
    if (genMips && bc7) genMips = false;

    std::vector<MipLevel> lv = levels;
    size_t totalBytes = 0;
    if (lv.empty()) lv.push_back({0, (size_t)w * h * 4});
    for (auto& l : lv) totalBytes = std::max(totalBytes, l.offset + l.size);

    uint32_t mips = (uint32_t)lv.size();
    if (genMips && lv.size() == 1) {
        uint32_t n = 1;
        for (int m = std::max(w, h); m > 1; m >>= 1) ++n;
        mips = n;
    }

    const VkFormat vf = vkFmt(fmt);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (genMips && mips > 1) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    Tex t;
    t.w = w; t.h = h; t.repeat = repeat;
    if (!makeImage(w, h, mips, vf, usage, t.image, t.mem)) { std::fprintf(stderr, "[TOOLVK] image alloc failed\n"); return 0; }

    VkBuffer stage = VK_NULL_HANDLE; VkDeviceMemory stageMem = VK_NULL_HANDLE;
    if (!makeBuffer(totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stage, stageMem)) {
        vkDestroyImage(g.s->dev, t.image, nullptr); vkFreeMemory(g.s->dev, t.mem, nullptr); return 0;
    }
    void* mp = nullptr;
    vkMapMemory(g.s->dev, stageMem, 0, totalBytes, 0, &mp);
    std::memcpy(mp, data, totalBytes);
    vkUnmapMemory(g.s->dev, stageMem);

    const uint32_t uploaded = (uint32_t)lv.size();
    oneShot([&](VkCommandBuffer cb) {
        barrier(cb, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, mips, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (uint32_t i = 0; i < uploaded; ++i) {
            VkBufferImageCopy c{};
            c.bufferOffset = lv[i].offset;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            c.imageExtent = {(uint32_t)std::max(1, w >> i), (uint32_t)std::max(1, h >> i), 1};
            vkCmdCopyBufferToImage(cb, stage, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        if (mips > uploaded) {   // generated chain: blit each level from the previous one
            for (uint32_t i = 1; i < mips; ++i) {
                barrier(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i - 1, 1,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageBlit b{};
                b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
                b.srcOffsets[1] = {std::max(1, w >> (i - 1)), std::max(1, h >> (i - 1)), 1};
                b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
                b.dstOffsets[1] = {std::max(1, w >> i), std::max(1, h >> i), 1};
                vkCmdBlitImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            }
            barrier(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mips - 1, 1,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            barrier(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, mips - 1,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        } else {
            barrier(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, mips,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    });
    vkDestroyBuffer(g.s->dev, stage, nullptr);
    vkFreeMemory(g.s->dev, stageMem, nullptr);

    t.view = makeView(t.image, vf, mips, VK_IMAGE_ASPECT_COLOR_BIT);
    const uint32_t id = g.nextId++;
    g.texs[id] = t;
    return id;
}

void destroyTexture(uint32_t id) {
    auto it = g.texs.find(id);
    if (!g.s || it == g.texs.end()) return;
    vkDeviceWaitIdle(g.s->dev);
    Tex& t = it->second;
    if (t.imgui) ImGui_ImplVulkan_RemoveTexture(t.imgui);
    if (t.view) vkDestroyImageView(g.s->dev, t.view, nullptr);
    if (t.image) vkDestroyImage(g.s->dev, t.image, nullptr);
    if (t.mem) vkFreeMemory(g.s->dev, t.mem, nullptr);
    g.texs.erase(it);
}

unsigned long long imguiTex(uint32_t id) {
    auto it = g.texs.find(id);
    if (it == g.texs.end()) return 0;
    if (!it->second.imgui)
        it->second.imgui = ImGui_ImplVulkan_AddTexture(g.clampSampler, it->second.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return (unsigned long long)(uintptr_t)it->second.imgui;
}

// ---------------------------------------------------------------- targets
bool createTarget(Target& t, int w, int h) {
    destroyTarget(t);
    if (!g.s || w < 1 || h < 1) return false;
    auto* impl = new TargetImpl;
    Tex ct;
    ct.w = w; ct.h = h; ct.repeat = false;
    if (!makeImage(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, ct.image, ct.mem) ||
        !makeImage(w, h, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, impl->depth, impl->depthMem)) {
        delete impl; return false;
    }
    ct.view = makeView(ct.image, VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    impl->depthView = makeView(impl->depth, VK_FORMAT_D32_SFLOAT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView views[2] = {ct.view, impl->depthView};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.pass; fi.attachmentCount = 2; fi.pAttachments = views; fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1;
    if (vkCreateFramebuffer(g.s->dev, &fi, nullptr, &impl->fb) != VK_SUCCESS) { delete impl; return false; }
    const uint32_t id = g.nextId++;
    g.texs[id] = ct;
    t.colorTex = id; t.w = w; t.h = h; t.impl = impl;
    // First use: the render pass declares initialLayout UNDEFINED, so the layout ImGui is told about is only valid after the first pass.
    return true;
}

void destroyTarget(Target& t) {
    if (!t.impl) { t = Target{}; return; }
    vkDeviceWaitIdle(g.s->dev);
    auto* impl = static_cast<TargetImpl*>(t.impl);
    if (impl->fb) vkDestroyFramebuffer(g.s->dev, impl->fb, nullptr);
    if (impl->depthView) vkDestroyImageView(g.s->dev, impl->depthView, nullptr);
    if (impl->depth) vkDestroyImage(g.s->dev, impl->depth, nullptr);
    if (impl->depthMem) vkFreeMemory(g.s->dev, impl->depthMem, nullptr);
    delete impl;
    destroyTexture(t.colorTex);
    t = Target{};
}

// ---------------------------------------------------------------- buffers
bool createBuffer(Buffer& b, const void* data, size_t bytes, bool index) {
    destroyBuffer(b);
    if (!g.s || bytes == 0) return false;
    VkBuffer vb = VK_NULL_HANDLE; VkDeviceMemory vm = VK_NULL_HANDLE;
    if (!makeBuffer(bytes, index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vb, vm)) return false;
    void* mp = nullptr;
    vkMapMemory(g.s->dev, vm, 0, bytes, 0, &mp);
    std::memcpy(mp, data, bytes);
    vkUnmapMemory(g.s->dev, vm);
    b.buf = (unsigned long long)(uintptr_t)vb; b.mem = (unsigned long long)(uintptr_t)vm; b.size = bytes;
    return true;
}

void destroyBuffer(Buffer& b) {
    if (g.s && b.buf) {
        vkDeviceWaitIdle(g.s->dev);
        vkDestroyBuffer(g.s->dev, (VkBuffer)(uintptr_t)b.buf, nullptr);
        vkFreeMemory(g.s->dev, (VkDeviceMemory)(uintptr_t)b.mem, nullptr);
    }
    b = Buffer{};
}

// ---------------------------------------------------------------- pipelines
bool createPipeline(Pipeline& p, const PipelineDesc& d) {
    destroyPipeline(p);
    if (!g.s) return false;
    VkShaderModule vs = loadModule(d.vertSpv), fs = loadModule(d.fragSpv);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(g.s->dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(g.s->dev, fs, nullptr);
        return false;
    }
    auto* impl = new PipelineImpl;
    impl->samplers = d.samplers;

    std::vector<VkDescriptorSetLayoutBinding> bind;
    VkDescriptorSetLayoutBinding ub{};
    ub.binding = 0; ub.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ub.descriptorCount = 1;
    ub.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bind.push_back(ub);
    for (uint32_t i = 0; i < d.samplers; ++i) {
        VkDescriptorSetLayoutBinding sb{};
        sb.binding = 1 + i; sb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sb.descriptorCount = 1;
        sb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bind.push_back(sb);
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = (uint32_t)bind.size(); dli.pBindings = bind.data();
    vkCreateDescriptorSetLayout(g.s->dev, &dli, nullptr, &impl->dsl);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &impl->dsl;
    vkCreatePipelineLayout(g.s->dev, &pli, nullptr, &impl->layout);

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    VkVertexInputBindingDescription vb{0, d.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> va;
    for (auto& a : d.attrs) va.push_back({a.location, 0, (VkFormat)a.format, a.offset});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = (uint32_t)va.size(); vi.pVertexAttributeDescriptions = va.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;   // with the flipped viewport this matches GL's CCW-front
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy;
    gi.layout = impl->layout; gi.renderPass = g.pass; gi.subpass = 0;
    const VkResult r = vkCreateGraphicsPipelines(g.s->dev, VK_NULL_HANDLE, 1, &gi, nullptr, &impl->pipe);
    vkDestroyShaderModule(g.s->dev, vs, nullptr);
    vkDestroyShaderModule(g.s->dev, fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[TOOLVK] pipeline %s failed (%d)\n", d.fragSpv.c_str(), (int)r);
        p.impl = impl; destroyPipeline(p); return false;
    }
    p.impl = impl;
    return true;
}

void destroyPipeline(Pipeline& p) {
    if (!p.impl) return;
    auto* impl = static_cast<PipelineImpl*>(p.impl);
    if (g.s) {
        vkDeviceWaitIdle(g.s->dev);
        if (impl->pipe) vkDestroyPipeline(g.s->dev, impl->pipe, nullptr);
        if (impl->layout) vkDestroyPipelineLayout(g.s->dev, impl->layout, nullptr);
        if (impl->dsl) vkDestroyDescriptorSetLayout(g.s->dev, impl->dsl, nullptr);
    }
    delete impl;
    p.impl = nullptr;
}

// ---------------------------------------------------------------- pass
bool Pass::begin(Target& t, const float clear[4]) {
    if (!g.s || !t.impl || open_) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.s->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g.s->dev, &ai, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    vkResetDescriptorPool(g.s->dev, g.pool, 0);
    g.uboCursor = 0;

    VkClearValue cv[2]{};
    std::memcpy(cv[0].color.float32, clear, 16);
    cv[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.pass; rb.framebuffer = static_cast<TargetImpl*>(t.impl)->fb;
    rb.renderArea = {{0, 0}, {(uint32_t)t.w, (uint32_t)t.h}};
    rb.clearValueCount = 2; rb.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    // Negative height (VK_KHR_maintenance1, core 1.1): GL-style Y-up clip space renders upright.
    VkViewport vp{0.0f, (float)t.h, (float)t.w, -(float)t.h, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {(uint32_t)t.w, (uint32_t)t.h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cb_ = cb;
    open_ = true;
    cur_ = nullptr;
    return true;
}

void Pass::bind(Pipeline& p) {
    if (!open_ || !p.impl) return;
    vkCmdBindPipeline(static_cast<VkCommandBuffer>(cb_), VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<PipelineImpl*>(p.impl)->pipe);
    cur_ = &p;
}

void Pass::draw(const void* ubo, size_t uboBytes, std::initializer_list<uint32_t> texIds, const Buffer& vb, const Buffer& ib, uint32_t indexCount) {
    if (!open_ || !cur_ || !vb.buf) return;
    auto* pl = static_cast<PipelineImpl*>(cur_->impl);
    auto cb = static_cast<VkCommandBuffer>(cb_);
    const size_t off = (g.uboCursor + g.uboAlign - 1) / g.uboAlign * g.uboAlign;
    if (off + uboBytes > kUboBytes) return;   // out of ring: drop the draw
    std::memcpy(g.uboMap + off, ubo, uboBytes);
    g.uboCursor = off + uboBytes;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &pl->dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g.s->dev, &ai, &set) != VK_SUCCESS) return;
    VkDescriptorBufferInfo bufInfo{g.uboBuf, (VkDeviceSize)off, (VkDeviceSize)uboBytes};
    std::vector<VkWriteDescriptorSet> ws;
    std::vector<VkDescriptorImageInfo> ii(pl->samplers);
    VkWriteDescriptorSet w0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w0.dstSet = set; w0.dstBinding = 0; w0.descriptorCount = 1; w0.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w0.pBufferInfo = &bufInfo;
    ws.push_back(w0);
    uint32_t i = 0;
    for (uint32_t id : texIds) {
        if (i >= pl->samplers) break;
        auto it = g.texs.find(id);
        if (it == g.texs.end()) it = g.texs.find(g.whiteId);
        ii[i] = {g.repeatSampler, it->second.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = 1 + i; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii[i];
        ws.push_back(w);
        ++i;
    }
    for (; i < pl->samplers; ++i) {   // unnamed slots read the white texel
        auto& wt = g.texs[g.whiteId];
        ii[i] = {g.repeatSampler, wt.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = 1 + i; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii[i];
        ws.push_back(w);
    }
    vkUpdateDescriptorSets(g.s->dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl->layout, 0, 1, &set, 0, nullptr);
    VkBuffer vbuf = (VkBuffer)(uintptr_t)vb.buf;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &zero);
    if (ib.buf) {
        vkCmdBindIndexBuffer(cb, (VkBuffer)(uintptr_t)ib.buf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, indexCount, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cb, indexCount, 1, 0, 0);
    }
}

void Pass::end() {
    if (!open_) return;
    auto cb = static_cast<VkCommandBuffer>(cb_);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(g.s->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.s->queue);
    vkFreeCommandBuffers(g.s->dev, g.s->pool, 1, &cb);
    open_ = false; cb_ = nullptr; cur_ = nullptr;
}

void perspectiveVk(float fovyRad, float aspect, float zn, float zf, float m[16]) {
    const float f = 1.0f / std::tan(fovyRad * 0.5f);
    for (int i = 0; i < 16; ++i) m[i] = 0;
    m[0] = f / aspect; m[5] = f;
    m[10] = zf / (zn - zf); m[11] = -1.0f;
    m[14] = (zf * zn) / (zn - zf);
}

} // namespace toolvk
