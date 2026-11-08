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


#include <volk.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic_trace.h"
#include "vk_desc_pool_chain.h"
#include "vk_desc_write_cache.h"
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
struct Swapchain { void operator()(VkDevice d, VkSwapchainKHR h) const { infra::release(reinterpret_cast<uint64_t>(h)); vkDestroySwapchainKHR(d, h, nullptr); } };
struct AccelStruct { void operator()(VkDevice d, VkAccelerationStructureKHR h) const { vkDestroyAccelerationStructureKHR(d, h, nullptr); } };
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
// A ray-tracing acceleration structure handle (VK_KHR_acceleration_structure). The backing buffer is an OwnedBuffer next to it (see vk_rt_acceleration.h).
using OwnedAccelStruct = DeviceOwned<VkAccelerationStructureKHR, release::AccelStruct>;
// The surface belongs to the instance, not the device: it must be released before the instance (the engine context destroys that last).
using OwnedSurface = mc2::OwnedHandle<VkSurfaceKHR, VkInstance, release::Surface>;

// Creates through `call(Handle*) -> VkResult` (the `vkCreate*` expression) and hands the new handle to `owner`, releasing what it held. False, with `owner`
// empty, when the call fails: the handle a failed call leaves behind is never adopted.
// The VkResult of the last failed create on this thread (VK_SUCCESS when none since the last take): an island init failure names it
// (vk_island_fail_policy.h).
inline thread_local VkResult t_lastCreateFailure = VK_SUCCESS;
inline VkResult takeLastCreateFailure() {
    const VkResult result = t_lastCreateFailure;
    t_lastCreateFailure = VK_SUCCESS;
    return result;
}

template <class Owner, class Call>
bool create(Owner& owner, VkDevice device, Call&& call) {
    owner.reset();
    typename Owner::HandleType handle{};
    const VkResult result = call(&handle);
    if (result != VK_SUCCESS) t_lastCreateFailure = result;
    if (result != VK_SUCCESS || handle == typename Owner::HandleType{}) return false;
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

// vk-present-instance 6: the one descriptor-set allocate. Logs the VkResult, the pool and the call site on failure (OUT_OF_POOL names the pool that ran dry)
// instead of each site returning a bare false. Use through MC2_VK_ALLOC_SETS so the site is recorded.
inline VkResult allocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out, std::string_view file, int line) {
    const VkResult r = vkAllocateDescriptorSets(device, info, out);
    if (r != VK_SUCCESS) t_lastCreateFailure = r;   // an island init failure names it (vk_island_fail_policy.h)
    bumpDescEpoch();   // a new set may reuse a freed set's handle: every descriptor write-cache key goes stale
    if (r != VK_SUCCESS && mc2_diag::tagRequested("VK_DESC_ALLOC")) {   // quiet-by-default rule: MC2_DIAG_TAGS=VK_DESC_ALLOC prints it
        const std::string base = std::filesystem::path(file).filename().string();
        std::fprintf(stderr, "[VK_DESC_ALLOC] vkAllocateDescriptorSets failed result=%d%s pool=%p sets=%u site=%s:%d\n", static_cast<int>(r),
                     r == VK_ERROR_OUT_OF_POOL_MEMORY ? " (OUT_OF_POOL_MEMORY)" : "", static_cast<void*>(info->descriptorPool), info->descriptorSetCount, base.c_str(), line);
    }
    return r;
}
#define MC2_VK_ALLOC_SETS(dev, info, out) ::mc2vk::allocateDescriptorSets((dev), (info), (out), __FILE__, __LINE__)

// One island's chained descriptor pools (vk_desc_pool_chain.h): every pool of the chain is made from the create info given to init(). allocate() fills in
// the pool; a dry or fragmented pool is replaced by the next one of the chain (or a fresh one) and the allocation retried once, else the caller gets the
// actual VkResult and keeps its own failure path. reset() resets every pool (the caller idles the GPU first and restores its dummy sets).
class DescPoolChain {
public:
    VkResult init(VkDevice device, const VkDescriptorPoolCreateInfo& info) {
        device_ = device; flags_ = info.flags; maxSets_ = info.maxSets;
        sizes_.assign(info.pPoolSizes, info.pPoolSizes + info.poolSizeCount);
        chain_.clear();
        OwnedDescriptorPool first;
        const VkResult r = createPool(&first);
        if (r == VK_SUCCESS) chain_.adopt(std::move(first));
        return r;
    }
    VkResult allocate(const VkDescriptorSetAllocateInfo& info, VkDescriptorSet* out, std::string_view file, int line) {
        const auto alloc = [&](OwnedDescriptorPool& pool) {
            VkDescriptorSetAllocateInfo ai = info;
            ai.descriptorPool = pool.get();
            return static_cast<int32_t>(allocateDescriptorSets(device_, &ai, out, file, line));
        };
        return static_cast<VkResult>(chain_.allocate(alloc, [&](OwnedDescriptorPool* pool) { return static_cast<int32_t>(createPool(pool)); }));
    }
    void reset() {
        chain_.reset([&](OwnedDescriptorPool& pool) { vkResetDescriptorPool(device_, pool.get(), 0); });
        bumpDescEpoch();   // every set handle of the chain may come back from the next allocation
    }
    // [VK_DESC_POOL v1] (MC2_DIAG_TAGS=VK_DESC_POOL): the chain length at mission unload; stable across reloads unless a mission needs more.
    void logChain(const char* island) const {
        if (mc2_diag::tagRequested("VK_DESC_POOL"))
            std::fprintf(stderr, "[VK_DESC_POOL v1] island=%s pools=%zu sets_per_pool=%u recoveries=%llu\n", island, chain_.length(), maxSets_,
                         static_cast<unsigned long long>(chain_.retries()));
    }
    size_t length() const { return chain_.length(); }
    explicit operator bool() const { return !chain_.empty(); }

private:
    VkResult createPool(OwnedDescriptorPool* pool) {
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = flags_; pci.maxSets = maxSets_;
        pci.poolSizeCount = static_cast<uint32_t>(sizes_.size()); pci.pPoolSizes = sizes_.data();
        return createChecked(*pool, device_, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(device_, &pci, nullptr, h); });
    }
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPoolCreateFlags flags_ = 0;
    uint32_t maxSets_ = 0;
    std::vector<VkDescriptorPoolSize> sizes_;
    DescPoolChainCore<OwnedDescriptorPool> chain_;
};
#define MC2_VK_ALLOC_SETS_CHAIN(chain, info, out) (chain).allocate((info), (out), __FILE__, __LINE__)

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

    // `minAlignment` (0 = the driver's own requirement) places the allocation, and so the device address, on a multiple of it through
    // vmaCreateBufferWithAlignment. A buffer whose device address a consumer needs aligned beyond what the usage flags imply (RT instance arrays 16,
    // AS storage 256, build scratch minAccelerationStructureScratchOffsetAlignment) must pass it: the driver may report 4 for a host buffer and VMA
    // then packs it right behind an odd-sized neighbour.
    bool create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& info, const VmaAllocationCreateInfo& alloc, std::string_view name,
                int i0 = -1, int i1 = -1, VkDeviceSize minAlignment = 0);
    // A host-visible buffer the CPU writes in random order (a per-frame feed, a copy of CPU geometry).
    bool createHostWritable(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name, int i0 = -1, int i1 = -1,
                            VkDeviceSize minAlignment = 0);
    // A buffer the CPU writes sequentially and the GPU reads every frame (UBO/SSBO feeds): prefers device-local host-visible memory
    // (ReBAR) so the GPU does not read across PCIe; falls back to host memory where the device has no such heap. Never read back.
    bool createGpuRead(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name, int i0 = -1, int i1 = -1);
    // A host-writable buffer holding a copy of `bytes` of CPU data (copy-once geometry). On any failure the buffer is left empty,
    // so a caller that retries later never holds a half-filled buffer.
    bool createFilled(VmaAllocator allocator, VkDevice device, const void* src, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name,
                      VkDeviceSize minAlignment = 0);
    void reset() noexcept;

    VkBuffer get() const noexcept { return buffer_; }
    VmaAllocation allocation() const noexcept { return allocation_; }
    // The buffer's device address (buffer device address is a required device feature). Queried once in `create` and cached; 0 unless the buffer
    // was created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    VkDeviceAddress deviceAddress() const noexcept { return address_; }
    void markDeferred() { if(allocation_) infra::defer(reinterpret_cast<uint64_t>(allocation_)); }
    explicit operator bool() const noexcept { return buffer_ != VK_NULL_HANDLE; }

    // Host mapping of a host-visible buffer for one write; nullptr when the map fails.
    void* map() const;
    void unmap() const;
    // Copies `bytes` to `offset` and flushes the written range, for memory that is not coherent. False when the buffer is empty or the map fails.
    bool upload(const void* src, VkDeviceSize bytes, VkDeviceSize offset = 0) const;
    // A device-local buffer, filled by a transfer (copy-once data).
    bool createDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name, int i0 = -1, int i1 = -1,
                           VkDeviceSize minAlignment = 0);
    // Copies `bytes` from `offset` to `dst` after invalidating the range, for host-visible memory the GPU wrote. False when the buffer is empty or the map fails.
    bool read(void* dst, VkDeviceSize bytes, VkDeviceSize offset = 0) const;

private:
    void swap(OwnedBuffer& other) noexcept {
        VkBuffer b = buffer_; buffer_ = other.buffer_; other.buffer_ = b;
        VmaAllocation a = allocation_; allocation_ = other.allocation_; other.allocation_ = a;
        VmaAllocator al = allocator_; allocator_ = other.allocator_; other.allocator_ = al;
        VkDeviceAddress ad = address_; address_ = other.address_; other.address_ = ad;
    }
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VmaAllocator allocator_ = nullptr;
    VkDeviceAddress address_ = 0;
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

#endif  // MC2_VK_OWNED_H
