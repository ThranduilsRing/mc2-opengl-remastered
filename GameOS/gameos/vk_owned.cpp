// vk_owned.cpp -- the VMA-backed owners of vk_owned.h.

#ifdef MC2_VULKAN_ISLAND

#include "vk_owned.h"

#include <cstdio>
#include <cstring>

#include <vk_mem_alloc.h>

#include "Gates/gate.h"
#include "Gates/gate_cold.h"

#include "hitch_stats.h"
#include "vulkan_context.h"

namespace mc2vk {

bool OwnedBuffer::create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& info, const VmaAllocationCreateInfo& alloc, std::string_view name,
                         int i0, int i1) {
    reset();
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkBuffer probe=VK_NULL_HANDLE;
    if(vkCreateBuffer(device,&info,nullptr,&probe)!=VK_SUCCESS) return false;
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,probe,&requirements);
    vkDestroyBuffer(device,probe,nullptr);
    infra::Admission admission(requirements.size,name);
    if(!admission) return false;
    VmaAllocationInfo placed{};
    if (vmaCreateBuffer(allocator, &info, &alloc, &buffer, &allocation, &placed) != VK_SUCCESS) return false;
    if (mc2::gate::get<"MC2_DIAG_VERBOSE">()) {   // where the buffer landed (audit S2): DL = DEVICE_LOCAL, HV = HOST_VISIBLE, HC = HOST_CACHED
        VkMemoryPropertyFlags pf = 0;
        vmaGetMemoryTypeProperties(allocator, placed.memoryType, &pf);
        std::fprintf(stderr, "[VK_MEM v1] name=%.*s i0=%d i1=%d bytes=%llu memtype=%u %s%s%s\n", static_cast<int>(name.size()), name.data(), i0, i1,
                     static_cast<unsigned long long>(info.size), placed.memoryType, (pf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DL " : "",
                     (pf & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HV " : "", (pf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HC" : "");
    }
    buffer_ = buffer;
    allocation_ = allocation;
    allocator_ = allocator;
    address_ = 0;
    if (info.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        dai.buffer = buffer_;
        address_ = vkGetBufferDeviceAddress(device, &dai);
    }
    admission.commit(reinterpret_cast<uint64_t>(allocation), name);
    nameObjectAt(device, buffer_, name, i0, i1);
    return true;
}

bool OwnedBuffer::createHostWritable(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name, int i0, int i1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    return create(allocator, device, bci, aci, name, i0, i1);
}

bool OwnedBuffer::createFilled(VmaAllocator allocator, VkDevice device, const void* src, VkDeviceSize bytes, VkBufferUsageFlags usage,
                               std::string_view name) {
    if (!src || bytes == 0 || !createHostWritable(allocator, device, bytes, usage, name) || !upload(src, bytes)) {
        reset();
        return false;
    }
    if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) mc2_hitch2::noteStaged(bytes);   // createFilled with TRANSFER_SRC is a staging copy; the frame ring and other persistent buffers are not
    return true;
}

void OwnedBuffer::reset() noexcept {
    if (allocation_) infra::release(reinterpret_cast<uint64_t>(allocation_));
    if (buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    allocator_ = nullptr;
    address_ = 0;
}

void* OwnedBuffer::map() const {
    void* p = nullptr;
    return buffer_ != VK_NULL_HANDLE && vmaMapMemory(allocator_, allocation_, &p) == VK_SUCCESS ? p : nullptr;
}

void OwnedBuffer::unmap() const {
    if (buffer_ != VK_NULL_HANDLE) vmaUnmapMemory(allocator_, allocation_);
}

bool OwnedBuffer::upload(const void* src, VkDeviceSize bytes, VkDeviceSize offset) const {
    void* dst = map();
    if (!dst) return false;
    std::memcpy(static_cast<uint8_t*>(dst) + offset, src, static_cast<size_t>(bytes));
    vmaFlushAllocation(allocator_, allocation_, offset, bytes);
    unmap();
    return true;
}

bool OwnedBuffer::createDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name, int i0, int i1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return create(allocator, device, bci, aci, name, i0, i1);
}

bool OwnedBuffer::read(void* dst, VkDeviceSize bytes, VkDeviceSize offset) const {
    const void* src = map();
    if (!src) return false;
    vmaInvalidateAllocation(allocator_, allocation_, offset, bytes);
    std::memcpy(dst, static_cast<const uint8_t*>(src) + offset, static_cast<size_t>(bytes));
    unmap();
    return true;
}

bool OwnedImage::createSampled2D(VmaAllocator allocator, VkDevice device, VkFormat format, uint32_t width, uint32_t height,
                                 uint32_t mipLevels, VkImageUsageFlags extraUsage, std::string_view name) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = mipLevels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extraUsage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    return create(allocator, device, ici, aci, name);
}

bool OwnedImage::create(VmaAllocator allocator, VkDevice device, const VkImageCreateInfo& info, const VmaAllocationCreateInfo& alloc, std::string_view name,
                        int i0, int i1) {
    reset();
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    if(!infra::imageSupported(info,name)) return false;
    VkImage probe=VK_NULL_HANDLE;
    if(vkCreateImage(device,&info,nullptr,&probe)!=VK_SUCCESS) return false;
    VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device,probe,&requirements);
    vkDestroyImage(device,probe,nullptr);
    infra::Admission admission(requirements.size,name);
    if(!admission) return false;
    if (vmaCreateImage(allocator, &info, &alloc, &image, &allocation, nullptr) != VK_SUCCESS) return false;
    image_ = image;
    allocation_ = allocation;
    allocator_ = allocator;
    admission.commit(reinterpret_cast<uint64_t>(allocation), name);
    nameObjectAt(device, image_, name, i0, i1);
    return true;
}

void OwnedImage::reset() noexcept {
    if (allocation_) infra::release(reinterpret_cast<uint64_t>(allocation_));
    if (image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, image_, allocation_);
    image_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    allocator_ = nullptr;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
