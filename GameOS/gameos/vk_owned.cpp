// vk_owned.cpp -- the VMA-backed owners of vk_owned.h.

#ifdef MC2_VULKAN_ISLAND

#include "vk_owned.h"

#include <vk_mem_alloc.h>

#include "vulkan_context.h"

namespace mc2vk {

bool OwnedBuffer::create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& info, const VmaAllocationCreateInfo& alloc, std::string_view name,
                         int i0, int i1) {
    reset();
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    if (vmaCreateBuffer(allocator, &info, &alloc, &buffer, &allocation, nullptr) != VK_SUCCESS) return false;
    buffer_ = buffer;
    allocation_ = allocation;
    allocator_ = allocator;
    nameObjectAt(device, buffer_, name, i0, i1);
    return true;
}

void OwnedBuffer::reset() noexcept {
    if (buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    allocator_ = nullptr;
}

void* OwnedBuffer::map() const {
    void* p = nullptr;
    return buffer_ != VK_NULL_HANDLE && vmaMapMemory(allocator_, allocation_, &p) == VK_SUCCESS ? p : nullptr;
}

void OwnedBuffer::unmap() const {
    if (buffer_ != VK_NULL_HANDLE) vmaUnmapMemory(allocator_, allocation_);
}

bool OwnedImage::create(VmaAllocator allocator, VkDevice device, const VkImageCreateInfo& info, const VmaAllocationCreateInfo& alloc, std::string_view name,
                        int i0, int i1) {
    reset();
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    if (vmaCreateImage(allocator, &info, &alloc, &image, &allocation, nullptr) != VK_SUCCESS) return false;
    image_ = image;
    allocation_ = allocation;
    allocator_ = allocator;
    nameObjectAt(device, image_, name, i0, i1);
    return true;
}

void OwnedImage::reset() noexcept {
    if (image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, image_, allocation_);
    image_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    allocator_ = nullptr;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
