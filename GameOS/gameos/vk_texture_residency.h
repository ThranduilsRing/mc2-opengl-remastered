#pragma once
#ifdef MC2_VULKAN_ISLAND
#include "vk_owned.h"
#include "vk_completion_tracker.h"
#include "../../RenderCore/TextureService.h"
#include "../../RenderCore/TextureStream.h"
#include <deque>
#include <array>
#include <span>

namespace mc2vk {
class VulkanContext;
class TextureResidency;
class ResidentTextureRef {
public:
    ResidentTextureRef() = default;
    ~ResidentTextureRef();
    ResidentTextureRef(const ResidentTextureRef&) = delete;
    ResidentTextureRef& operator=(const ResidentTextureRef&) = delete;
    ResidentTextureRef(ResidentTextureRef&& o) noexcept;
    ResidentTextureRef& operator=(ResidentTextureRef&& o) noexcept;
    RenderCore::TextureHandle get() const { return owner_ ? handle_ : RenderCore::TextureHandle::invalid(); }
    explicit operator bool() const { return owner_ != nullptr; }
private:
    friend class TextureResidency;
    ResidentTextureRef(TextureResidency& owner, RenderCore::TextureHandle handle) : owner_(&owner), handle_(handle) {}
    TextureResidency* owner_ = nullptr;
    RenderCore::TextureHandle handle_;
};

// Vulkan residency of the existing session TextureService. Context-owned, destroyed after islands and before VMA.
// Only Vulkan images enter here. Legacy CPU/gos producers use acquireLegacy until P9 removes it after P4-P8 migrate their consumers.
class TextureResidency {
public:
    TextureResidency(VulkanContext& ctx, RenderCore::TextureService& service);
    bool init();
    ResidentTextureRef acquire(const RenderCore::TextureKey& key, const RenderCore::KtxImage& image,
                               RenderCore::TextureDimensions dimensions);
    // VFS resolution/extraction happens before this call. The worker never touches shared archives.
    ResidentTextureRef requestKtx(const RenderCore::TextureKey& key, std::vector<uint8_t> bytes,
                                  RenderCore::TextureDimensions dimensions);
    void pump();
    bool ready(RenderCore::TextureHandle handle) const;
    ResidentTextureRef acquireLegacy(uint32_t width, uint32_t height, VkFormat format, const void* bytes, size_t count);
    ResidentTextureRef retain(RenderCore::TextureHandle handle);
    void release(RenderCore::TextureHandle handle);
    VkImageView view(RenderCore::TextureHandle handle) const;
    // Call only in the open engine frame. An immutable per-flight table is written after its previous ticket completes.
    VkDescriptorSet descriptors();
    uint32_t descriptorIndex(RenderCore::TextureHandle handle) const;
    VkDescriptorSetLayout layout() const { return layout_.get(); }
    uint32_t capacity() const { return capacity_; }
    void sealFrame();
private:
    struct Image { OwnedImage image; OwnedImageView view; };
    struct Resident { std::shared_ptr<Image> image; uint32_t descriptor = 0; bool useMips = true; };
    struct Pending { RenderCore::TextureHandle handle; std::shared_ptr<const RenderCore::KtxImage> image; };
    struct Upload {
        RenderCore::TextureHandle handle;
        std::shared_ptr<Image> image;
        OwnedBuffer staging;
        OwnedCommandPool pool;
        GpuTicket ticket;
        size_t bytes = 0;
    };
    struct Flight {
        VkDescriptorSet set = VK_NULL_HANDLE;
        GpuTicket ticket;
        uint64_t frame = ~uint64_t(0);
        uint64_t revision = ~uint64_t(0);
        std::vector<RenderCore::TextureDescriptorTable::Slot> snapshot;
    };
    std::shared_ptr<Image> upload(const RenderCore::KtxImage& image, bool useMips = true, Upload* async = nullptr);
    VulkanContext& ctx_;
    RenderCore::TextureService& service_;
    uint32_t capacity_ = 0;
    OwnedSampler sampler_;
    OwnedDescriptorSetLayout layout_;
    OwnedDescriptorPool pool_;
    std::unique_ptr<RenderCore::TextureDescriptorTable> table_;
    std::shared_ptr<Image> placeholder_;
    std::unordered_map<uint32_t, Resident> residents_;
    std::array<Flight, 2> flights_;
    RenderCore::TextureStagingBudget budget_;
    std::deque<Pending> pending_;
    std::deque<Upload> uploading_;
    RenderCore::TextureDecodeWorker worker_;

};
} // namespace mc2vk
#endif
