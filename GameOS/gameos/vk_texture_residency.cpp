#ifdef MC2_VULKAN_ISLAND
#include "vk_texture_residency.h"
#include "vulkan_context.h"
#include "vk_one_shot.h"
#include "texture_decode.h"
#include "utils/mc2log.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace mc2vk {
bool hasUiImage(RenderCore::TextureHandle handle) { return getOrCreateEngineVulkanContext().textures().contains(handle); }
void attachUiImage(RenderCore::TextureHandle handle, std::shared_ptr<const RenderCore::KtxImage> image) {
    getOrCreateEngineVulkanContext().textures().attach(handle,std::move(image));
}

ResidentTextureRef::~ResidentTextureRef() { if (owner_) owner_->release(handle_); }
ResidentTextureRef::ResidentTextureRef(ResidentTextureRef&& o) noexcept
    : owner_(std::exchange(o.owner_, nullptr)), handle_(o.handle_) {}
ResidentTextureRef& ResidentTextureRef::operator=(ResidentTextureRef&& o) noexcept {
    if (this != &o) { if (owner_) owner_->release(handle_); owner_ = std::exchange(o.owner_, nullptr); handle_ = o.handle_; }
    return *this;
}
TextureResidency::TextureResidency(VulkanContext& ctx, RenderCore::TextureService& service) : ctx_(ctx), service_(service) {
    service_.onRetired=[this](RenderCore::TextureHandle h) { std::lock_guard lock(retiredMutex_); retired_.push_back(h); };
}
TextureResidency::~TextureResidency() { service_.onRetired={}; }
void TextureResidency::attach(RenderCore::TextureHandle handle, std::shared_ptr<const RenderCore::KtxImage> image, bool useMips) {
    if (!service_.isLive(handle) || residents_.contains(handle.raw())) return;
    residents_.emplace(handle.raw(),Resident{{},0,useMips});
    if (image) pending_.push_back({handle,std::move(image)});
}

bool TextureResidency::init() {
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(ctx_.physicalDevice(), &props);
    const auto& l = props.limits;
    capacity_ = std::min({4096u, l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorSampledImages,
                         l.maxDescriptorSetSamplers, l.maxDescriptorSetSampledImages, l.maxPerStageResources});
    if (capacity_ < 2) return false;
    table_ = std::make_unique<RenderCore::TextureDescriptorTable>(capacity_);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; sci.maxLod = VK_LOD_CLAMP_NONE;
    if (!mc2vk::create(sampler_, ctx_.device(), [&](VkSampler* s) { return vkCreateSampler(ctx_.device(), &sci, nullptr, s); })) return false;
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = capacity_; binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=1; li.pBindings=&binding;
    if (!mc2vk::create(layout_, ctx_.device(), [&](VkDescriptorSetLayout* s) { return vkCreateDescriptorSetLayout(ctx_.device(), &li, nullptr, s); })) return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, capacity_ * 2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=2; pi.poolSizeCount=1; pi.pPoolSizes=&size;
    if (!mc2vk::create(pool_, ctx_.device(), [&](VkDescriptorPool* p) { return vkCreateDescriptorPool(ctx_.device(), &pi, nullptr, p); })) return false;
    VkDescriptorSetLayout layouts[] = {layout_.get(),layout_.get()}; VkDescriptorSet sets[2]{};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool=pool_.get(); ai.descriptorSetCount=2; ai.pSetLayouts=layouts;
    if (vkAllocateDescriptorSets(ctx_.device(),&ai,sets)!=VK_SUCCESS) return false;
    for (int i=0;i<2;++i) flights_[i].set=sets[i];
    RenderCore::KtxImage image; image.width=image.height=2; image.mipCount=1; image.vkFormat=37; image.mipByteOffsets={0};
    image.pixels={255,0,255,255, 0,0,0,255, 0,0,0,255, 255,0,255,255};
    placeholder_=upload(image); if (!placeholder_) return false;
    table_->placeholder(placeholder_);
    MC2_LOG(Vk, Info, "[VK_TEXTURE_SERVICE v1] capacity={} flights=2 placeholder=0 legacy_boundary=P9", capacity_);
    return true;
}

std::shared_ptr<TextureResidency::Image> TextureResidency::upload(const RenderCore::KtxImage& image, bool useMips, Upload* async) {
    const bool rgba=image.vkFormat==37 || image.vkFormat==43;
    const bool bc1=image.vkFormat>=131 && image.vkFormat<=134;
    const bool blocks=bc1 || image.vkFormat==137 || image.vkFormat==138 || image.vkFormat==141 || (image.vkFormat>=143 && image.vkFormat<=146);
    if ((!rgba && !blocks) || image.isCompressed!=blocks || (blocks && image.blockSizeBytes!=(bc1?8u:16u))) return {};
    const uint32_t w=image.width,h=image.height;
    if (!w || !h || w>RenderCore::kKtxMaxEdge || h>RenderCore::kKtxMaxEdge || image.mipCount<1 ||
        image.mipCount>15 || image.mipByteOffsets.size()!=size_t(image.mipCount)) return {};
    uint32_t fullChain=1;
    for (uint32_t edge=std::max(w,h);edge>1;edge>>=1) ++fullChain;
    if (uint32_t(image.mipCount)>fullChain) return {};
    VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(ctx_.physicalDevice(), VkFormat(image.vkFormat), &properties);
    constexpr auto need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & need)!=need) return {};
    std::vector<VkBufferImageCopy> copies;
    size_t expected=0;
    for (int level=0;level<image.mipCount;++level) {
        const uint32_t mw=std::max(1u,w>>level),mh=std::max(1u,h>>level);
        const size_t bytes=image.isCompressed ? size_t((mw+3)/4)*((mh+3)/4)*image.blockSizeBytes : size_t(mw)*mh*4;
        if (image.mipByteOffsets[level]!=expected || !bytes || expected>image.pixels.size() || bytes>image.pixels.size()-expected) return {};
        VkBufferImageCopy copy{}; copy.bufferOffset=expected; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,uint32_t(level),0,1}; copy.imageExtent={mw,mh,1};
        copies.push_back(copy); expected+=bytes;
    }
    if (expected!=image.pixels.size()) return {};
    OwnedBuffer staging;
    if (!staging.createFilled(ctx_.allocator(),ctx_.device(),image.pixels.data(),image.pixels.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,"texture.service.staging")) return {};
    auto resource=std::make_shared<Image>();
    if (!resource->image.createSampled2D(ctx_.allocator(),ctx_.device(),VkFormat(image.vkFormat),w,h,image.mipCount,0,"texture.service.image")) return {};
    const auto record=[&](VkCommandBuffer cb){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=resource->image.get(); b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,uint32_t(image.mipCount),0,1};
        b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        vkCmdCopyBufferToImage(cb,staging.get(),resource->image.get(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=resource->image.get();vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VkFormat(image.vkFormat);
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,useMips ? uint32_t(image.mipCount) : 1u,0,1};
    if (!mc2vk::create(resource->view,ctx_.device(),[&](VkImageView* v){return vkCreateImageView(ctx_.device(),&vi,nullptr,v);})) return {};
    // This context creates only the graphics family. Copy and shader barriers therefore stay on the same queue;
    // no unsupported transfer family or ownership handoff is assumed.
    if (async) {
        VkCommandBuffer cb=VK_NULL_HANDLE;
        if (!beginTransientCommandBuffer(ctx_,async->pool,cb)) return {};
        record(cb);
        if (vkEndCommandBuffer(cb)!=VK_SUCCESS) return {};
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
        uint64_t value=0;
        if (ctx_.submit(si,VK_NULL_HANDLE,&value)!=VK_SUCCESS) return {};
        async->ticket={value,0}; async->staging=std::move(staging); async->image=resource;
    } else if (!submitOneShot(ctx_,record,"texture placeholder initialization")) return {};
    return resource;
}

ResidentTextureRef TextureResidency::acquire(const RenderCore::TextureKey& key, const RenderCore::KtxImage& image, RenderCore::TextureDimensions dimensions) {
    auto handle=service_.acquire(key,RenderCore::kNoTextureNode,dimensions);
    if (!handle.isValid()) return {};
    if (residents_.contains(handle.raw())) return ResidentTextureRef(*this,handle);
    residents_.emplace(handle.raw(),Resident{{},0,mc2tex::wantsMipmaps(key.hints)});
    pending_.push_back({handle,std::make_shared<RenderCore::KtxImage>(image)});
    return ResidentTextureRef(*this,handle);
}
ResidentTextureRef TextureResidency::requestKtx(const RenderCore::TextureKey& key, std::vector<uint8_t> bytes,
                                               RenderCore::TextureDimensions dimensions) {
    auto handle=service_.acquire(key,RenderCore::kNoTextureNode,dimensions);
    if (!handle.isValid()) return {};
    if (!residents_.contains(handle.raw())) {
        residents_.emplace(handle.raw(),Resident{{},0,mc2tex::wantsMipmaps(key.hints)});
        worker_.request(handle,std::move(bytes));
    }
    return ResidentTextureRef(*this,handle);
}
void TextureResidency::pump() {
    std::vector<RenderCore::TextureHandle> retired;
    { std::lock_guard lock(retiredMutex_); retired.swap(retired_); }
    for (auto handle : retired) {
        table_->erase(handle);
        auto it=residents_.find(handle.raw());
        if (it!=residents_.end()) { if (it->second.image) ctx_.retire(std::move(it->second.image)); residents_.erase(it); }
    }
    for (auto& result : worker_.take())
        if (result.image && service_.isLive(result.handle)) pending_.push_back({result.handle,std::move(result.image)});
    // Queue order makes completion a prefix: idle resident images are never scanned.
    while (!uploading_.empty() && ctx_.isComplete(uploading_.front().ticket)) {
        auto& upload=uploading_.front();
        auto it=residents_.find(upload.handle.raw());
        if (it!=residents_.end() && service_.isLive(upload.handle)) {
            const auto slot=table_->insert(upload.handle,upload.image);
            if (slot) { it->second.image=std::move(upload.image); it->second.descriptor=slot; }
        }
        budget_.complete(upload.bytes); uploading_.pop_front();
    }
    while (!pending_.empty()) {
        auto& p=pending_.front();
        auto it=residents_.find(p.handle.raw());
        if (it==residents_.end() || !service_.isLive(p.handle)) { pending_.pop_front(); continue; }
        const size_t bytes=p.image->pixels.size();
        if (bytes>budget_.limit) {
            MC2_LOG(Vk, Warn, "[VK_TEXTURE_SERVICE] staging request exceeds budget: {} bytes",bytes);
            pending_.pop_front(); continue;
        }
        if (table_->full() || !budget_.reserve(bytes)) break;
        Upload upload; upload.handle=p.handle; upload.bytes=bytes;
        if (this->upload(*p.image,it->second.useMips,&upload)) uploading_.push_back(std::move(upload));
        else budget_.complete(bytes);
        pending_.pop_front();
    }
}
bool TextureResidency::ready(RenderCore::TextureHandle handle) const {
    auto it=residents_.find(handle.raw());
    return it!=residents_.end() && it->second.image && service_.isLive(handle);
}
ResidentTextureRef TextureResidency::acquireLegacy(uint32_t width,uint32_t height,VkFormat format,const void* bytes,size_t count) {
    if (!bytes || !width || !height || width>RenderCore::kKtxMaxEdge || height>RenderCore::kKtxMaxEdge) return {};
    const bool compressed=format>=131 && format<=146;
    const size_t expected=compressed ? size_t((width+3)/4)*((height+3)/4)*(format<=134?8u:16u) : size_t(width)*height*4;
    if (count!=expected) return {};
    RenderCore::TextureKey key;
    key.source=RenderCore::makeTextureSourceId("legacy-cpu:P9",std::to_string(width)+"x"+std::to_string(height),
        RenderCore::textureContentHash(static_cast<const uint8_t*>(bytes),count)); key.format=uint32_t(format);
    RenderCore::KtxImage image; image.width=width;image.height=height;image.vkFormat=format;image.mipCount=1;image.mipByteOffsets={0};
    image.isCompressed=(format>=131 && format<=146);image.blockSizeBytes=(format>=131 && format<=134)?8:image.isCompressed?16:0;
    image.pixels.assign(static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+count);
    return acquire(key,image,{width,height,width,height});
}
ResidentTextureRef TextureResidency::retain(RenderCore::TextureHandle handle) {
    return residents_.contains(handle.raw()) && service_.retain(handle) ? ResidentTextureRef(*this,handle) : ResidentTextureRef{};
}
void TextureResidency::release(RenderCore::TextureHandle handle) {
    if (!service_.release(handle)) return;
    if (!service_.isLive(handle) || service_.users(handle)==0) {
        table_->erase(handle);
        auto it=residents_.find(handle.raw());
        if (it!=residents_.end()) {
            // Compatibility descriptors can read this view outside the bindless snapshots. The open-frame ticket covers those draws too.
            if (it->second.image) ctx_.retire(std::move(it->second.image));
            residents_.erase(it);
        }
    }
}
VkImageView TextureResidency::view(RenderCore::TextureHandle handle) const {
    auto it=residents_.find(handle.raw()); return it!=residents_.end() && it->second.image && service_.isLive(handle) ? it->second.image->view.get() : placeholder_->view.get();
}
VkDescriptorSet TextureResidency::descriptors() {
    auto& flight=flights_[ctx_.frameContext().slot];
    if (!ctx_.tracker().frameOpen()) return VK_NULL_HANDLE;
    if (flight.frame==ctx_.tracker().frameNo()) return flight.set;
    if (flight.revision == table_->revision()) { flight.frame=ctx_.tracker().frameNo(); return flight.set; }
    if (!ctx_.wait(flight.ticket,5000000000ull,"texture descriptor flight")) return VK_NULL_HANDLE;
    flight.snapshot=table_->snapshot();
    std::vector<VkDescriptorImageInfo> infos(capacity_,{sampler_.get(),placeholder_->view.get(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    for (size_t i=1;i<flight.snapshot.size();++i)
        if (flight.snapshot[i].owner) infos[i].imageView=std::static_pointer_cast<Image>(flight.snapshot[i].owner)->view.get();
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=flight.set;write.dstBinding=0;write.descriptorCount=capacity_;
    write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=infos.data();
    vkUpdateDescriptorSets(ctx_.device(),1,&write,0,nullptr);
    flight.revision=table_->revision();flight.frame=ctx_.tracker().frameNo();return flight.set;
}
uint32_t TextureResidency::descriptorIndex(RenderCore::TextureHandle handle) const {
    const auto& f=flights_[ctx_.frameContext().slot];
    if (f.frame!=ctx_.tracker().frameNo() || !service_.isLive(handle)) return 0;
    const uint32_t idx=table_->index(handle);
    return idx<f.snapshot.size() && f.snapshot[idx].handle==handle ? idx : 0;
}
void TextureResidency::sealFrame() {
    auto& f=flights_[ctx_.frameContext().slot];
    if (f.frame==ctx_.tracker().frameNo()) f.ticket=ctx_.tracker().now();
}
} // namespace mc2vk
#endif
