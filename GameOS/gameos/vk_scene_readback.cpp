#include "vk_scene_readback.h"
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_context.h"
#include "vk_barrier_batch_vk.h"
#include "vk_image_resource.h"
#include "vk_one_shot.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#include "vulkan_shared_scene_depth.h"
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>
#include <glm/gtc/packing.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mc2vk {
namespace {
using Images = std::array<VkImageResource*, 3>;
constexpr uint32_t bits[3] = {SceneColor, SceneDepth, SceneObjectId};
constexpr VkFormat formats[3] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R32_UINT};
VkImageLayout attachment(unsigned i) {
    return i == 1 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
VkImageAspectFlags aspects(unsigned i) {
    return i == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}
VkPipelineStageFlags stages(unsigned i) {
    return (i == 1 ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                   : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
}
VkAccessFlags accesses(unsigned i) {
    return (i == 1 ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                   : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) | VK_ACCESS_SHADER_READ_BIT;
}
Images activeImages() {
    return {mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0), mc2SharedSceneDepthGetResource(),
            mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_OBJECT_ID)};
}
bool extent(const Images& images, uint32_t channels, int& width, int& height) {
    width = height = 0;
    if (!channels || (channels & ~SceneAll)) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (!(channels & bits[i])) continue;
        const auto* image = images[i];
        if (!image || !image->image || (image->format != formats[i] && !(i == 0 && image->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32)) || !image->w || !image->h ||
            !(image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || image->w > INT32_MAX || image->h > INT32_MAX) return false;
        if (width && (unsigned(width) != image->w || unsigned(height) != image->h)) return false;
        width = int(image->w); height = int(image->h);
    }
    return width > 0 && height > 0;
}
void imageBarrier(VkCommandBuffer cb, const VkImageResource& image, unsigned i, bool toTransfer) {
    const VkImageSubresourceRange range{aspects(i), 0, 1, 0, 1};
    const VkPipelineStageFlags2 attStages = stages(i);
    const VkAccessFlags2 attAccess = accesses(i);
    auto& ctx = getOrCreateEngineVulkanContext();
    if (toTransfer)
        sync::cmdImageBarrier(ctx, cb, image.image.get(), range, attachment(i), attStages, attAccess,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    else
        sync::cmdImageBarrier(ctx, cb, image.image.get(), range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, attachment(i), attStages, attAccess);
}
bool readImages(const Images& images, int x, int y, int w, int h, uint32_t channels, ScenePixel* out, size_t count,
                const ScenePoint* points = nullptr, size_t sampleCount = 0) {
    int sourceW = 0, sourceH = 0;
    if (!out || !extent(images, channels, sourceW, sourceH) || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        w > sourceW || h > sourceH || x > sourceW-w || y > sourceH-h) return false;
    if (points) {
        if (!sampleCount || sampleCount > 4096) return false;
        for (size_t p=0;p<sampleCount;++p)
            if (points[p].x<0 || points[p].y<0 || points[p].x>=sourceW || points[p].y>=sourceH) return false;
    }
    const size_t pixels = points ? sampleCount : size_t(w)*size_t(h);
    const bool packedColor = images[0] && images[0]->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    const uint32_t strides[3] = {packedColor ? 4u : 8u, 4, 4};
    if (pixels > count || pixels > 1024*1024) return false;
    auto& ctx = getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return false;
    size_t bytes = 0;
    std::array<size_t, 3> offsets{};
    for (unsigned i = 0; i < 3; ++i) if (channels & bits[i]) {
        offsets[i] = bytes; bytes += pixels*strides[i];
    }
    OwnedBuffer staging;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = bytes; buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo host{}; host.usage = VMA_MEMORY_USAGE_AUTO;
    host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!staging.create(ctx.allocator(), ctx.device(), buffer, host, "scene.explicitReadback")) return false;
    if (!submitOneShot(ctx, [&](VkCommandBuffer cb) {
        for (unsigned i = 0; i < 3; ++i) if (channels & bits[i]) {
            imageBarrier(cb, *images[i], i, true);
            std::vector<VkBufferImageCopy> copies(points ? sampleCount : 1);
            for(size_t p=0;p<copies.size();++p) {
                auto& copy=copies[p]; copy.bufferOffset = offsets[i] + (points ? p*strides[i] : 0);
                copy.imageSubresource = {VkImageAspectFlags(i == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
                copy.imageOffset = points ? VkOffset3D{points[p].x,points[p].y,0} : VkOffset3D{x,y,0};
                copy.imageExtent = points ? VkExtent3D{1,1,1} : VkExtent3D{uint32_t(w),uint32_t(h),1};
            }
            vkCmdCopyImageToBuffer(cb, images[i]->image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.get(), uint32_t(copies.size()), copies.data());
            imageBarrier(cb, *images[i], i, false);
        }
        sync::BarrierBatch batch;
        sync::BufferBarrierRec br;
        br.buffer = reinterpret_cast<uint64_t>(staging.get());
        br.srcStages = VK_PIPELINE_STAGE_2_COPY_BIT; br.srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        br.dstStages = VK_PIPELINE_STAGE_2_HOST_BIT; br.dstAccess = VK_ACCESS_2_HOST_READ_BIT;
        batch.buffer(br);
        sync::flush(getOrCreateEngineVulkanContext(), cb, batch);
    }, "scene explicit pick/probe readback")) return false;
    const auto* data = static_cast<const uint8_t*>(staging.map());
    if (!data) return false;
    const bool valid = vmaInvalidateAllocation(ctx.allocator(), staging.allocation(), 0, VK_WHOLE_SIZE) == VK_SUCCESS;
    if (valid) for (size_t p = 0; p < pixels; ++p) {
        out[p] = {};
        if (channels & SceneColor) {
            glm::vec4 color;
            if (packedColor) {
                uint32_t packed; std::memcpy(&packed, data+offsets[0]+4*p, 4);
                color = glm::vec4(glm::unpackF2x11_1x10(packed), 1.0f);
            } else {
                uint64_t packed; std::memcpy(&packed, data+offsets[0]+8*p, 8);
                color = glm::unpackHalf4x16(packed);
            }
            for (unsigned c = 0; c < 4; ++c)
                out[p].rgba[c] = std::isnan(color[c]) ? 0 : uint8_t(std::lround(std::clamp(color[c], 0.0f, 1.0f)*255.0f));
        }
        if (channels & SceneDepth) std::memcpy(&out[p].depth, data+offsets[1]+4*p, 4);
        if (channels & SceneObjectId) std::memcpy(&out[p].objectId, data+offsets[2]+4*p, 4);
    }
    staging.unmap();
    return valid;
}
}
bool sceneReadbackExtent(int& width, int& height, uint32_t channels) {
    return extent(activeImages(), channels, width, height);
}
bool readScenePixels(int x, int y, int w, int h, uint32_t channels, ScenePixel* out, size_t count) {
    return readImages(activeImages(), x, y, w, h, channels, out, count);
}
bool readSceneSamples(const ScenePoint* points, size_t count, uint32_t channels, ScenePixel* out) {
    return points && readImages(activeImages(),0,0,1,1,channels,out,count,points,count);
}
bool sceneReadbackSelfTest() {
    auto& ctx = getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return false;
    std::array<VkImageResource, 3> owned;
    struct Cleanup {
        std::array<VkImageResource,3>& images;
        VkDevice device;
        ~Cleanup() { for(auto& image:images) vkImageResourceDestroy(&image,device); }
    } cleanup{owned,ctx.device()};
    Images images{&owned[0], &owned[1], &owned[2]};
    for (unsigned i = 0; i < 3; ++i)
        if (!vkImageResourceCreate(images[i], ctx.device(), ctx.physicalDevice(), 2, 3, formats[i],
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (i == 1 ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))) return false;
    std::array<uint8_t, 96> data{};
    for (unsigned p = 0; p < 6; ++p) {
        const uint64_t color = glm::packHalf4x16(glm::vec4(float(p)/4.0f, 0.5f, 0.25f, 1.0f));
        const float depth = float(p)/8.0f; const uint32_t id = 0xA50000u+p;
        std::memcpy(data.data()+8*p, &color, 8); std::memcpy(data.data()+48+4*p, &depth, 4); std::memcpy(data.data()+72+4*p, &id, 4);
    }
    OwnedBuffer upload;
    if (!upload.createFilled(ctx.allocator(), ctx.device(), data.data(), data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "scene.readbackTestUpload")) return false;
    if (!submitOneShot(ctx, [&](VkCommandBuffer cb) {
        const size_t offsets[3] = {0,48,72};
        for (unsigned i = 0; i < 3; ++i) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[i]->image.get(); barrier.subresourceRange = {aspects(i),0,1,0,1};
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferImageCopy copy{}; copy.bufferOffset=offsets[i];
            copy.imageSubresource={VkImageAspectFlags(i==1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),0,0,1}; copy.imageExtent={2,3,1};
            vkCmdCopyBufferToImage(cb,upload.get(),images[i]->image.get(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=accesses(i);
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=attachment(i);
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,stages(i),0,0,nullptr,0,nullptr,1,&barrier);
        }
    },"scene readback test upload")) return false;
    auto correct=[](const ScenePixel& pixel, unsigned p) {
        const unsigned r[6]={0,64,128,191,255,255};
        return pixel.rgba[0]==r[p] && pixel.rgba[1]==128 && pixel.rgba[2]==64 && pixel.rgba[3]==255 &&
               pixel.depth==float(p)/8.0f && pixel.objectId==0xA50000u+p;
    };
    std::array<ScenePixel,6> pixels{};
    if (!readImages(images,0,0,2,3,SceneAll,pixels.data(),pixels.size())) return false;
    for(unsigned p=0;p<6;++p) if (!correct(pixels[p],p)) return false;
    if (!readImages(images,1,1,1,2,SceneAll,pixels.data(),pixels.size()) || !correct(pixels[0],3) || !correct(pixels[1],5) ||
        readImages(images,-1,0,1,1,SceneAll,pixels.data(),pixels.size()) ||
        readImages(images,0,0,2,3,SceneAll,pixels.data(),5)) return false;
    const ScenePoint points[2]={{1,2},{0,0}};
    if (!readImages(images,0,0,1,1,SceneAll,pixels.data(),pixels.size(),points,2) || !correct(pixels[0],5) || !correct(pixels[1],0) ||
        !readImages(images,0,0,1,1,SceneObjectId,pixels.data(),pixels.size(),points,2) ||
        pixels[0].objectId!=0xA50005u || pixels[1].objectId!=0xA50000u || pixels[0].depth!=0 || pixels[0].rgba[3]!=0) return false;
    std::fprintf(stderr,"[VK_SCENE_READBACK] selftest samples=12 channels=3 bounds=pass result=pass\n");
    return true;
}
}
#else
namespace mc2vk {
bool sceneReadbackExtent(int& w,int& h,uint32_t) { w=h=0; return false; }
bool readScenePixels(int,int,int,int,uint32_t,ScenePixel*,size_t) { return false; }
bool readSceneSamples(const ScenePoint*,size_t,uint32_t,ScenePixel*) { return false; }
bool sceneReadbackSelfTest() { return false; }
}
#endif
