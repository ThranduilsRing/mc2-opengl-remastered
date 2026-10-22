// Shared construction and barrier helpers for the static shadow bake.
#pragma once

#include "vulkan_shadow_bake.h"
#include "../../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "vk_island_util.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cstring>
#include <string_view>

namespace mc2vk::shadow_bake_detail {

constexpr VkFormat kWindowFmt = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kObjectFmt = VK_FORMAT_D16_UNORM;
constexpr VkFormat kMaskFmt = VK_FORMAT_BC4_UNORM_BLOCK;   // the product (P4: half the R8 footprint)
constexpr VkFormat kWorkFmt = VK_FORMAT_R8_UNORM;          // a tile's resolved levels and the coarse top image, before BC4
constexpr uint32_t kTileLevels = 6;                        // levels 0..5 of a tile (512..16) or a sub-tile (128..4) are encoded where they lie
constexpr uint32_t kTopLevel = 5;                          // the coarse top image is mask level 5; the levels below it are re-derived from it
constexpr uint32_t kSubTilesPerFrame = 2;                  // the dirty-update budget: about 0.12 ms of GPU each on the 7900 XTX (0.3 ms target)
constexpr uint64_t kDirtyIdleFrames = 1800;               // the dirty path's transients go after this long without a batch (about 30 s)
constexpr int kDirtyGrowTexels = 24;                       // the blurred fade (16) and the 3x3 resolve (1) read this far past a change
constexpr uint32_t kSliceSubTile = kTileLevels;            // encoder parameter slices: 0..5 tile levels, 6..11 sub-tile levels, then the top
constexpr int kObjectMapSize = 4096;              // High preset (D4); presets arrive with P7
constexpr float kMaskTargetTexelWorld = 1.5f;     // High preset target (D1)
constexpr int kMaskCapTexels = 12288;
constexpr VkDeviceSize kSlice = 256;              // >= any device's minUniformBufferOffsetAlignment
constexpr uint32_t kQueriesPerTile = 5;
// Waiting this many frames for a late alpha-ON array, the bake goes ahead with those casters opaque (a solid canopy shadow) and says so.
constexpr uint32_t kMaxAlphaWaitFrames = 600;

// LOCKSTEP with shadow_prop.vert / shadow_terrain_static.vert ShadowFrameUBO (std140).
struct CasterUBO {
    float lightSpace[16];
    int32_t instBase;
    int32_t pad[3];
};
static_assert(sizeof(CasterUBO) == 80, "ShadowFrameUBO std140 size");

static_assert(sizeof(ShadowBakeTileParams) == 176, "BakeTile std140 size (shadow_bake_common.glsl)");

inline void blog(std::string_view what) { mc2::fmt::print_err("[SHADOW_BAKE] {}\n", what); }

inline VkImageMemoryBarrier imageBarrier(VkImage image, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mips, VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, baseMip, mips, 0, 1};
    return b;
}

// A depth-only pass: cleared, stored, left read-only for sampling. The dependencies order it after earlier reads of the image (compute or fragment) and
// its writes before later reads.
inline bool makeDepthPass(OwnedRenderPass& out, VkDevice device, VkFormat format) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.pDepthStencilAttachment = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = 0;   // write-after-read: an execution dependency is enough
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1; rci.pAttachments = &a;
    rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = 2; rci.pDependencies = deps;
    return create(out, device, [&](auto* h) { return vkCreateRenderPass(device, &rci, nullptr, h); });
}

inline bool makeImage(OwnedImage& image, OwnedImageView& view, VmaAllocator allocator, VkDevice device, VkFormat format, uint32_t side, uint32_t mips,
               VkImageUsageFlags usage, VkImageAspectFlags aspect, std::string_view name) {
    view.reset();
    image.reset();
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {side, side, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!image.create(allocator, device, ici, aci, name)) return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image.get();
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, mips, 0, 1};
    return create(view, device, [&](auto* h) { return vkCreateImageView(device, &vci, nullptr, h); });
}

inline bool makeFramebuffer(OwnedFramebuffer& fb, VkDevice device, VkRenderPass pass, VkImageView view, uint32_t side) {
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass; fci.attachmentCount = 1; fci.pAttachments = &view;
    fci.width = side; fci.height = side; fci.layers = 1;
    return create(fb, device, [&](auto* h) { return vkCreateFramebuffer(device, &fci, nullptr, h); });
}

inline bool makeHostBuffer(OwnedBuffer& buf, VmaAllocator allocator, VkDevice device, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name) {
    buf.reset();
    return buf.createHostWritable(allocator, device, (std::max<VkDeviceSize>)(bytes, 256), usage, name);
}

// World-space corners of a type's local box under an instance's transform: Stuff = (pos, 1) * M (row vector, M column-major), MC2 = (-x, z, y).
inline void instanceBox(const float m[16], const float mn[3], const float mx[3], mc2shadow::Vec3& lo, mc2shadow::Vec3& hi) {
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float v[3] = {(c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2]};
        mc2shadow::Vec3 stuff;
        float* s = &stuff.x;
        for (int j = 0; j < 3; ++j) s[j] = v[0] * m[4 * j + 0] + v[1] * m[4 * j + 1] + v[2] * m[4 * j + 2] + m[4 * j + 3];
        const mc2shadow::Vec3 w = mc2ToRender(stuff);
        lo = {(std::min)(lo.x, w.x), (std::min)(lo.y, w.y), (std::min)(lo.z, w.z)};
        hi = {(std::max)(hi.x, w.x), (std::max)(hi.y, w.y), (std::max)(hi.z, w.z)};
    }
}

// Instances sorted by type, appended to `out` as runs of one type each (firstInstance counts from `base`).
template <class Run>
void appendRuns(std::vector<Run>& out, const std::vector<uint32_t>& typeIds, uint32_t base) {
    for (uint32_t i = 0; i < typeIds.size();) {
        uint32_t j = i;
        while (j < typeIds.size() && typeIds[j] == typeIds[i]) ++j;
        out.push_back({typeIds[i], base + i, j - i});
        i = j;
    }
}

}  // namespace mc2vk::shadow_bake_detail
