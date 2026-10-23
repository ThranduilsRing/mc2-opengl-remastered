// vk_owned.h -- move-only owners of the Vulkan objects an island creates.
//
// WHAT: `OwnedImageView`, `OwnedPipeline`, ... own one device-level handle and capture the VkDevice that made it; `OwnedBuffer` and `OwnedImage` own a VMA
// allocation and capture the allocator. The destructor is the only release path (see owned_handle.h). Create through `mc2vk::create(owner, device, call)`
// (the call is the `vkCreate*` expression writing the handle) so the owner holds a handle only when creation succeeded.
// WHY: the islands kept raw handles and a hand-written teardown per island; the create/destroy ledger (.claude/cleanup/vk-ownership-ledger.md) counts the
// manual release code that remains, and a converted island has none. The owners must be destroyed before the device (and allocator) they captured: an
// island is destroyed ahead of the shared context in engine teardown (gameosmain.cpp) and nothing else holds one.
#ifndef MC2_VK_OWNED_H
#define MC2_VK_OWNED_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "owned_handle.h"
#include "vk_render_infra.h"

#include <string_view>

// VMA handles, forward-declared the way vk_shared_light_feed.h does so this header does not pull vk_mem_alloc.h into every island.
struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;
struct VmaAllocationCreateInfo;

namespace mc2vk {

namespace release {
struct ImageView { void operator()(VkDevice d, VkImageView h) const { vkDestroyImageView(d, h, nullptr); } };
struct Sampler { void operator()(VkDevice d, VkSampler h) const { vkDestroySampler(d, h, nullptr); } };
struct PipelineLayout { void operator()(VkDevice d, VkPipelineLayout h) const { vkDestroyPipelineLayout(d, h, nullptr); } };
struct ShaderModule { void operator()(VkDevice d, VkShaderModule h) const { infra::forgetShader(h); vkDestroyShaderModule(d, h, nullptr); } };
struct DescriptorPool { void operator()(VkDevice d, VkDescriptorPool h) const { vkDestroyDescriptorPool(d, h, nullptr); } };
struct DescriptorSetLayout { void operator()(VkDevice d, VkDescriptorSetLayout h) const { vkDestroyDescriptorSetLayout(d, h, nullptr); } };
struct Framebuffer { void operator()(VkDevice d, VkFramebuffer h) const { vkDestroyFramebuffer(d, h, nullptr); } };
struct RenderPass { void operator()(VkDevice d, VkRenderPass h) const { vkDestroyRenderPass(d, h, nullptr); } };
struct CommandPool { void operator()(VkDevice d, VkCommandPool h) const { vkDestroyCommandPool(d, h, nullptr); } };
struct Fence { void operator()(VkDevice d, VkFence h) const { vkDestroyFence(d, h, nullptr); } };
struct QueryPool { void operator()(VkDevice d, VkQueryPool h) const { vkDestroyQueryPool(d, h, nullptr); } };
struct Semaphore { void operator()(VkDevice d, VkSemaphore h) const { vkDestroySemaphore(d, h, nullptr); } };
struct RawImage { void operator()(VkDevice d, VkImage h) const { vkDestroyImage(d, h, nullptr); } };
struct DeviceMemory { void operator()(VkDevice d, VkDeviceMemory h) const { infra::release(reinterpret_cast<uint64_t>(h)); vkFreeMemory(d, h, nullptr); } };
struct RawBuffer { void operator()(VkDevice d, VkBuffer h) const { vkDestroyBuffer(d, h, nullptr); } };
struct Swapchain { void operator()(VkDevice d, VkSwapchainKHR h) const { vkDestroySwapchainKHR(d, h, nullptr); } };
struct Surface { void operator()(VkInstance i, VkSurfaceKHR h) const { vkDestroySurfaceKHR(i, h, nullptr); } };
}  // namespace release

template <class Handle, class Release> using DeviceOwned = mc2::OwnedHandle<Handle, VkDevice, Release>;
using OwnedImageView = DeviceOwned<VkImageView, release::ImageView>;
using OwnedSampler = DeviceOwned<VkSampler, release::Sampler>;
// A stable owner token survives a reload; get() resolves the current pipeline under the publish lock.
// Old Vulkan handles can then be retired immediately without risking handle reuse or double destruction.
class OwnedPipeline {
public:
    using HandleType=VkPipeline;
    OwnedPipeline()=default;
    OwnedPipeline(VkDevice device,VkPipeline pipeline): device_(device),token_(pipeline ? infra::adoptPipeline(device,pipeline) : 0) {}
    ~OwnedPipeline() { reset(); }
    OwnedPipeline(const OwnedPipeline&)=delete;
    OwnedPipeline& operator=(const OwnedPipeline&)=delete;
    OwnedPipeline(OwnedPipeline&& other) noexcept:device_(other.device_),token_(std::exchange(other.token_,0)) {}
    OwnedPipeline& operator=(OwnedPipeline&& other) noexcept {
        if(this!=&other) { reset(); device_=other.device_; token_=std::exchange(other.token_,0); }
        return *this;
    }
    VkPipeline get() const noexcept { return token_ ? infra::currentPipeline(token_) : VK_NULL_HANDLE; }
    VkDevice context() const noexcept { return device_; }
    explicit operator bool() const noexcept { return token_!=0; }
    void reset() noexcept { if(token_) infra::destroyOwnedPipeline(std::exchange(token_,0)); }
    void markDeferred() { if(token_) infra::deferPipeline(token_); }
private:
    VkDevice device_=VK_NULL_HANDLE;
    uint64_t token_=0;
};
using OwnedPipelineLayout = DeviceOwned<VkPipelineLayout, release::PipelineLayout>;
using OwnedShaderModule = DeviceOwned<VkShaderModule, release::ShaderModule>;
using OwnedDescriptorPool = DeviceOwned<VkDescriptorPool, release::DescriptorPool>;
using OwnedDescriptorSetLayout = DeviceOwned<VkDescriptorSetLayout, release::DescriptorSetLayout>;
using OwnedFramebuffer = DeviceOwned<VkFramebuffer, release::Framebuffer>;
using OwnedRenderPass = DeviceOwned<VkRenderPass, release::RenderPass>;
using OwnedCommandPool = DeviceOwned<VkCommandPool, release::CommandPool>;
using OwnedFence = DeviceOwned<VkFence, release::Fence>;
using OwnedQueryPool = DeviceOwned<VkQueryPool, release::QueryPool>;
using OwnedSemaphore = DeviceOwned<VkSemaphore, release::Semaphore>;
// An image made by vkCreateImage whose memory the owner of the image allocates and binds itself (VMA images use OwnedImage).
using OwnedRawImage = DeviceOwned<VkImage, release::RawImage>;
using OwnedDeviceMemory = DeviceOwned<VkDeviceMemory, release::DeviceMemory>;
// A buffer made by vkCreateBuffer whose memory the owner of the buffer allocates and binds itself (VMA buffers use OwnedBuffer). Declare the memory owner before it.
using OwnedRawBuffer = DeviceOwned<VkBuffer, release::RawBuffer>;
using OwnedSwapchain = DeviceOwned<VkSwapchainKHR, release::Swapchain>;
// The surface belongs to the instance, not the device: it must be released before the instance (the engine context destroys that last).
using OwnedSurface = mc2::OwnedHandle<VkSurfaceKHR, VkInstance, release::Surface>;

// Creates through `call(Handle*) -> VkResult` (the `vkCreate*` expression) and hands the new handle to `owner`, releasing what it held. False, with `owner`
// empty, when the call fails: the handle a failed call leaves behind is never adopted.
template <class Owner, class Call>
bool create(Owner& owner, VkDevice device, Call&& call) {
    owner.reset();
    typename Owner::HandleType handle{};
    if (call(&handle) != VK_SUCCESS || handle == typename Owner::HandleType{}) return false;
    owner = Owner(device, handle);
    return true;
}

// The same, returning the VkResult of the call (VK_ERROR_UNKNOWN when it returned a null handle) for a caller that logs it.
template <class Owner, class Call>
VkResult createChecked(Owner& owner, VkDevice device, Call&& call) {
    VkResult result = VK_ERROR_UNKNOWN;
    create(owner, device, [&](typename Owner::HandleType* handle) { result = call(handle); return result; });
    return result;
}

// A VMA buffer and its allocation. `create` names the buffer for the validation layer (name, optional slot indices; see vulkan_context.h).
class OwnedBuffer {
public:
    OwnedBuffer() = default;
    ~OwnedBuffer() { reset(); }
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;
    OwnedBuffer(OwnedBuffer&& other) noexcept { swap(other); }
    OwnedBuffer& operator=(OwnedBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }

    bool create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& info, const VmaAllocationCreateInfo& alloc, std::string_view name,
                int i0 = -1, int i1 = -1);
    // A host-visible buffer the CPU writes in random order (a per-frame feed, a copy of CPU geometry).
    bool createHostWritable(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name, int i0 = -1, int i1 = -1);
    // A host-writable buffer holding a copy of `bytes` of CPU data (copy-once geometry). On any failure the buffer is left empty,
    // so a caller that retries later never holds a half-filled buffer.
    bool createFilled(VmaAllocator allocator, VkDevice device, const void* src, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name);
    void reset() noexcept;

    VkBuffer get() const noexcept { return buffer_; }
    VmaAllocation allocation() const noexcept { return allocation_; }
    void markDeferred() { if(allocation_) infra::defer(reinterpret_cast<uint64_t>(allocation_)); }
    explicit operator bool() const noexcept { return buffer_ != VK_NULL_HANDLE; }

    // Host mapping of a host-visible buffer for one write; nullptr when the map fails.
    void* map() const;
    void unmap() const;
    // Copies `bytes` to `offset` and flushes the written range, for memory that is not coherent. False when the buffer is empty or the map fails.
    bool upload(const void* src, VkDeviceSize bytes, VkDeviceSize offset = 0) const;
    // A device-local buffer, filled by a transfer (copy-once data).
    bool createDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name, int i0 = -1, int i1 = -1);
    // Copies `bytes` from `offset` to `dst` after invalidating the range, for host-visible memory the GPU wrote. False when the buffer is empty or the map fails.
    bool read(void* dst, VkDeviceSize bytes, VkDeviceSize offset = 0) const;

private:
    void swap(OwnedBuffer& other) noexcept {
        VkBuffer b = buffer_; buffer_ = other.buffer_; other.buffer_ = b;
        VmaAllocation a = allocation_; allocation_ = other.allocation_; other.allocation_ = a;
        VmaAllocator al = allocator_; allocator_ = other.allocator_; other.allocator_ = al;
    }
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VmaAllocator allocator_ = nullptr;
};

// A VMA image and its allocation.
class OwnedImage {
public:
    OwnedImage() = default;
    ~OwnedImage() { reset(); }
    OwnedImage(const OwnedImage&) = delete;
    OwnedImage& operator=(const OwnedImage&) = delete;
    OwnedImage(OwnedImage&& other) noexcept { swap(other); }
    OwnedImage& operator=(OwnedImage&& other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }

    bool create(VmaAllocator allocator, VkDevice device, const VkImageCreateInfo& info, const VmaAllocationCreateInfo& alloc, std::string_view name,
                int i0 = -1, int i1 = -1);
    // A device-local, optimally tiled 2D texture that is filled by transfer and then sampled (`extraUsage` adds e.g. TRANSFER_SRC
    // for blit-generated mips).
    bool createSampled2D(VmaAllocator allocator, VkDevice device, VkFormat format, uint32_t width, uint32_t height, uint32_t mipLevels,
                         VkImageUsageFlags extraUsage, std::string_view name);
    void reset() noexcept;

    VkImage get() const noexcept { return image_; }
    VmaAllocation allocation() const noexcept { return allocation_; }
    void markDeferred() { if(allocation_) infra::defer(reinterpret_cast<uint64_t>(allocation_)); }
    explicit operator bool() const noexcept { return image_ != VK_NULL_HANDLE; }

private:
    void swap(OwnedImage& other) noexcept {
        VkImage i = image_; image_ = other.image_; other.image_ = i;
        VmaAllocation a = allocation_; allocation_ = other.allocation_; other.allocation_ = a;
        VmaAllocator al = allocator_; allocator_ = other.allocator_; other.allocator_ = al;
    }
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VmaAllocator allocator_ = nullptr;
};

// The image format for texels a gos texture handle reports (gos_GetTextureTexels: RGBA8, or BC7 blocks with their colour space
// kept, so the image samples what GL samples).
inline VkFormat texelFormat(uint32_t vkFormat) {
    switch (vkFormat) {
        case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
        case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
        default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_OWNED_H
