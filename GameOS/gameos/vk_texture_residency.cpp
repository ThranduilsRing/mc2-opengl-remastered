#ifdef MC2_VULKAN_ISLAND
#include "vk_texture_residency.h"
#include "RenderCore/GpuResidencyPolicy.h"
#include "Gates/render_options.h"
#include "vulkan_context.h"
#include "engine_present_vk.h"
#include "vk_one_shot.h"
#include "texture_decode.h"
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>
#include "utils/mc2log.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <chrono>
#include <cstdio>

namespace mc2vk {
bool uiTextureServiceReady() { return engineVulkanContextValid() && getOrCreateEnginePresentVk().valid(); }
bool hasUiImage(RenderCore::TextureHandle handle) { return engineVulkanContextValid() && getOrCreateEngineVulkanContext().textures().contains(handle); }
void invalidateUiImage(RenderCore::TextureHandle handle) {
    if (engineVulkanContextValid()) getOrCreateEngineVulkanContext().textures().invalidate(handle);
}
void TextureResidency::invalidate(RenderCore::TextureHandle handle) {
    table_->erase(handle);
    auto it=residents_.find(handle.raw());
    if (it!=residents_.end()) { if (it->second.image) ctx_.retire(std::move(it->second.image)); residents_.erase(it); }
}
void requestUiImage(RenderCore::TextureHandle handle, std::function<std::shared_ptr<const RenderCore::KtxImage>()> decode) {
    getOrCreateEngineVulkanContext().textures().requestDecoded(handle,std::move(decode));
}
void TextureResidency::requestDecoded(RenderCore::TextureHandle handle, std::function<std::shared_ptr<const RenderCore::KtxImage>()> decode) {
    if (!service_.isLive(handle) || residents_.contains(handle.raw())) return;
    auto [it,inserted]=residents_.emplace(handle.raw(),Resident{{},0,false});
    it->second.highPriority=true; it->second.contentVersion=service_.contentVersion(handle);
    worker_.request(handle,std::move(decode),it->second.contentVersion);
}
void attachUiImage(RenderCore::TextureHandle handle, std::shared_ptr<const RenderCore::KtxImage> image) {
    getOrCreateEngineVulkanContext().textures().attach(handle,std::move(image));
}

VkImageView ResidentTextureRef::view() const { return owner_ ? owner_->view(handle_,arrayView_) : VK_NULL_HANDLE; }
ResidentTextureRef::~ResidentTextureRef() { if (owner_) owner_->release(handle_); }
ResidentTextureRef::ResidentTextureRef(ResidentTextureRef&& o) noexcept
    : owner_(std::exchange(o.owner_, nullptr)), handle_(o.handle_), arrayView_(o.arrayView_) {}
ResidentTextureRef& ResidentTextureRef::operator=(ResidentTextureRef&& o) noexcept {
    if (this != &o) { if (owner_) owner_->release(handle_); owner_ = std::exchange(o.owner_, nullptr); handle_ = o.handle_; arrayView_=o.arrayView_; }
    return *this;
}
TextureResidency::TextureResidency(VulkanContext& ctx, RenderCore::TextureService& service) : ctx_(ctx), service_(service) {
    service_.onRetired=[this](RenderCore::TextureHandle h) { std::lock_guard lock(retiredMutex_); retired_.push_back(h); };
}
TextureResidency::~TextureResidency() { service_.onRetired={}; }
void TextureResidency::attach(RenderCore::TextureHandle handle, std::shared_ptr<const RenderCore::KtxImage> image, bool useMips) {
    if (!service_.isLive(handle) || residents_.contains(handle.raw())) return;
    residents_.emplace(handle.raw(),Resident{{},0,useMips});
    residents_.at(handle.raw()).contentVersion=service_.contentVersion(handle);
    if (image) pending_.push_back({handle,std::move(image),service_.contentVersion(handle)});
}

ResidentTextureRef TextureResidency::createRenderTarget(uint32_t width, uint32_t height) {
    if (table_->full(uploading_.size())) return {};
    auto image=std::make_shared<Image>();
    if (!image->image.createSampled2D(ctx_.allocator(),ctx_.device(),VK_FORMAT_R8G8B8A8_UNORM,width,height,1,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,"ui.preview")) return {};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image->image.get();
    vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_R8G8B8A8_UNORM; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if (!mc2vk::create(image->view,ctx_.device(),[&](VkImageView* v){return vkCreateImageView(ctx_.device(),&vi,nullptr,v);})) return {};
    RenderCore::TextureKey key; key.source="ui-preview:"+std::to_string(++targetSerial_);
    auto handle=service_.acquire(key,RenderCore::kNoTextureNode,{width,height,800,600});
    if (!handle.isValid()) return {};
    const auto slot=table_->insert(handle,image);
    residents_.emplace(handle.raw(),Resident{std::move(image),slot,false});
    return ResidentTextureRef(*this,handle);
}
uint64_t TextureResidency::legacyUiId(uint32_t gosHandle, uint32_t width, uint32_t height, VkFormat format,
                                     std::shared_ptr<const std::vector<uint8_t>> bytes, bool displaySpace, bool mipmaps) {
    if (!bytes) return 0;
    auto& e=legacyUi_[gosHandle | (uint64_t(displaySpace | (mipmaps<<1))<<32)];
    if (e.pending && !service_.isLive(e.pending.get())) e.pending={};
    if (e.texture && !service_.isLive(e.texture.get())) e.texture={};
    if (e.pending && ready(e.pending.get())) e.texture=std::move(e.pending);
    // Dynamic pages keep the last completed frame. Replacing a pending frame every tick could cancel every upload before publication.
    if (!e.pending && (e.source.lock()!=bytes || !e.texture)) {
        if (displaySpace && format==VK_FORMAT_BC7_SRGB_BLOCK) format=VK_FORMAT_BC7_UNORM_BLOCK;
        e.pending=acquirePixels(width,height,format,bytes->data(),bytes->size(),mipmaps); e.source=bytes;
    }
    return RenderCore::uiTextureId(e.texture ? e.texture.get() : e.pending.get());
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
    RenderCore::KtxImage flat; flat.width=flat.height=1; flat.vkFormat=37; flat.mipCount=1; flat.mipByteOffsets={0};
    flat.pixels={128,128,255,255}; arrayPlaceholder_=upload(flat,false,nullptr,1,true);
    if (!arrayPlaceholder_) return false;
    table_->placeholder(placeholder_);
    MC2_LOG(Vk, Debug, "[VK_TEXTURE_SERVICE v1] capacity={} flights=2 placeholder=0 legacy_boundary=P9", capacity_);
    return true;
}

std::shared_ptr<TextureResidency::Image> TextureResidency::upload(const RenderCore::KtxImage& image, bool useMips, Upload* async, uint32_t layers, bool arrayView) {
    const bool rgba=image.vkFormat==37 || image.vkFormat==43;
    const bool floating=image.vkFormat==97 || image.vkFormat==100 || image.vkFormat==VK_FORMAT_R32G32_SFLOAT;
    const size_t stride=(image.vkFormat==97 || image.vkFormat==VK_FORMAT_R32G32_SFLOAT) ? 8u : 4u;
    const bool bc1=image.vkFormat>=131 && image.vkFormat<=134;
    const bool blocks=bc1 || image.vkFormat==137 || image.vkFormat==138 || image.vkFormat==141 || (image.vkFormat>=143 && image.vkFormat<=146);
    if ((!rgba && !floating && !blocks) || image.isCompressed!=blocks || (blocks && image.blockSizeBytes!=(bc1?8u:16u))) return {};
    const uint32_t w=image.width,h=image.height;
    if (!layers || layers>4096 || (!arrayView && layers!=1)) return {};
    if (!w || !h || w>RenderCore::kKtxMaxEdge || h>RenderCore::kKtxMaxEdge || image.mipCount<1 ||
        image.mipCount>15 || image.mipByteOffsets.size()!=size_t(image.mipCount)) return {};
    uint32_t fullChain=1;
    for (uint32_t edge=std::max(w,h);edge>1;edge>>=1) ++fullChain;
    if (uint32_t(image.mipCount)>fullChain) return {};
    VkPhysicalDeviceProperties deviceProps{}; vkGetPhysicalDeviceProperties(ctx_.physicalDevice(),&deviceProps);
    if (layers>deviceProps.limits.maxImageArrayLayers) {
        std::fprintf(stderr,"[VK_RESIDENCY v1] refused=texture layers=%u limit=%u fallback=placeholder\n",layers,deviceProps.limits.maxImageArrayLayers);
        return {};
    }
    VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(ctx_.physicalDevice(), VkFormat(image.vkFormat), &properties);
    constexpr auto need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & need)!=need) return {};
    std::vector<VkBufferImageCopy> copies;
    size_t expected=0;
    for (int level=0;level<image.mipCount;++level) {
        const uint32_t mw=std::max(1u,w>>level),mh=std::max(1u,h>>level);
        const size_t bytes=(image.isCompressed ? size_t((mw+3)/4)*((mh+3)/4)*image.blockSizeBytes : size_t(mw)*mh*stride)*layers;
        if (image.mipByteOffsets[level]!=expected || !bytes || expected>image.pixels.size() || bytes>image.pixels.size()-expected) return {};
        VkBufferImageCopy copy{}; copy.bufferOffset=expected; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,uint32_t(level),0,layers}; copy.imageExtent={mw,mh,1};
        copies.push_back(copy); expected+=bytes;
    }
    if (expected!=image.pixels.size()) return {};
    // Admit upload overlap as well as the resident image. Drop authored top mips before refusing;
    // arrays retain their layer count and all copies use the reduced base extent.
    const auto policy=RenderCore::gpuResidencyPolicy(mc2::options::current().shadowQuality);
    const auto maxEdge=std::min(floating?policy.terrainEdge:policy.textureEdge,deviceProps.limits.maxImageDimension2D);
    if (w>maxEdge || h>maxEdge || uint64_t(expected)*2>policy.textureUpload || !infra::admit(uint64_t(expected)*2 + 128*1024)) {
        if (image.mipCount>1) {
            RenderCore::KtxImage reduced=image;
            const size_t skip=image.mipByteOffsets[1];
            reduced.width=std::max(1u,w/2);
            reduced.height=std::max(1u,h/2); --reduced.mipCount;
            reduced.pixels.erase(reduced.pixels.begin(),reduced.pixels.begin()+skip);
            reduced.mipByteOffsets.erase(reduced.mipByteOffsets.begin());
            for(auto& offset:reduced.mipByteOffsets) offset-=skip;
            std::fprintf(stderr,"[VK_RESIDENCY v1] refused=texture extent=%ux%u fallback=mip_%ux%u\n",w,h,reduced.width,reduced.height);
            return upload(reduced,useMips,async,layers,arrayView);
        }
        if(!blocks && (w>1 || h>1)) {
            RenderCore::KtxImage reduced=image;
            reduced.width=std::max(1u,w/2);
            reduced.height=std::max(1u,h/2);
            const size_t layerBytes=size_t(reduced.width)*reduced.height*stride;
            reduced.pixels.resize(layerBytes*layers);
            for(uint32_t layer=0;layer<layers;++layer) {
                if(rgba) {
                    std::vector<uint8_t> src(image.pixels.begin()+size_t(layer)*w*h*stride,image.pixels.begin()+size_t(layer+1)*w*h*stride);
                    const auto down=mc2tex::boxDownsampleRgba8(src,w,h);
                    std::memcpy(reduced.pixels.data()+layer*layerBytes,down.data(),layerBytes);
                } else {
                    // Float data keeps its native format; nearest reduction preserves encoded gradients/heights.
                    for(uint32_t y=0;y<uint32_t(reduced.height);++y) for(uint32_t x=0;x<uint32_t(reduced.width);++x)
                        std::memcpy(reduced.pixels.data()+layer*layerBytes+(size_t(y)*reduced.width+x)*stride,
                            image.pixels.data()+(size_t(layer)*w*h+size_t(y*2)*w+x*2)*stride,stride);
                }
            }
            std::fprintf(stderr,"[VK_RESIDENCY v1] refused=texture extent=%ux%u fallback=resample_%ux%u\n",w,h,reduced.width,reduced.height);
            return upload(reduced,useMips,async,layers,arrayView);
        }
        std::fprintf(stderr,"[VK_RESIDENCY v1] refused=texture extent=%ux%u bytes=%zu fallback=placeholder\n",w,h,expected);
        return {};
    }
    OwnedBuffer staging;
    if (!staging.createFilled(ctx_.allocator(),ctx_.device(),image.pixels.data(),image.pixels.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,"texture.service.staging")) return {};
    auto resource=std::make_shared<Image>();
    if (arrayView) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VkFormat(image.vkFormat);
        ci.extent={w,h,1}; ci.mipLevels=uint32_t(image.mipCount); ci.arrayLayers=layers; ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{}; aci.usage=VMA_MEMORY_USAGE_AUTO;
        if (!resource->image.create(ctx_.allocator(),ctx_.device(),ci,aci,"texture.service.array")) return {};
    } else if (!resource->image.createSampled2D(ctx_.allocator(),ctx_.device(),VkFormat(image.vkFormat),w,h,image.mipCount,0,"texture.service.image")) return {};
    const auto record=[&](VkCommandBuffer cb){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=resource->image.get(); b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,uint32_t(image.mipCount),0,layers};
        b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        vkCmdCopyBufferToImage(cb,staging.get(),resource->image.get(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=resource->image.get();vi.viewType=arrayView ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;vi.format=VkFormat(image.vkFormat);
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,useMips ? uint32_t(image.mipCount) : 1u,0,layers};
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

ResidentTextureRef TextureResidency::acquire(const RenderCore::TextureKey& key, const RenderCore::KtxImage& image, RenderCore::TextureDimensions dimensions, uint32_t layers, bool arrayView) {
    auto handle=service_.acquire(key,RenderCore::kNoTextureNode,dimensions);
    if (!handle.isValid()) return {};
    if (residents_.contains(handle.raw())) return ResidentTextureRef(*this,handle,arrayView);
    residents_.emplace(handle.raw(),Resident{{},0,mc2tex::wantsMipmaps(key.hints),layers,arrayView});
    residents_.at(handle.raw()).contentVersion=service_.contentVersion(handle);
    pending_.push_back({handle,std::make_shared<RenderCore::KtxImage>(image),service_.contentVersion(handle)});
    return ResidentTextureRef(*this,handle,arrayView);
}
ResidentTextureRef TextureResidency::requestKtx(const RenderCore::TextureKey& key, std::vector<uint8_t> bytes,
                                               RenderCore::TextureDimensions dimensions) {
    auto handle=service_.acquire(key,RenderCore::kNoTextureNode,dimensions);
    if (!handle.isValid()) return {};
    if (!residents_.contains(handle.raw())) {
        residents_.emplace(handle.raw(),Resident{{},0,mc2tex::wantsMipmaps(key.hints)});
        residents_.at(handle.raw()).contentVersion=service_.contentVersion(handle);
        worker_.request(handle,std::move(bytes),service_.contentVersion(handle));
    }
    return ResidentTextureRef(*this,handle);
}
void TextureResidency::pump() {
    service_.collectSourceRetirements();
    std::vector<RenderCore::TextureHandle> retired;
    { std::lock_guard lock(retiredMutex_); retired.swap(retired_); }
    for (auto handle : retired) {
        table_->erase(handle);
        auto it=residents_.find(handle.raw());
        if (it!=residents_.end()) { if (it->second.image) ctx_.retire(std::move(it->second.image)); residents_.erase(it); }
    }
    for (auto& result : worker_.take()) {
        auto it=residents_.find(result.handle.raw());
        if (!result.image || !service_.isLive(result.handle) || it==residents_.end() || result.contentVersion!=it->second.contentVersion) continue;
        Pending pending{result.handle,std::move(result.image),result.contentVersion};
        if (it->second.highPriority) pending_.push_front(std::move(pending));
        else pending_.push_back(std::move(pending));
    }
    // Queue order makes completion a prefix: idle resident images are never scanned.
    RenderCore::publishTextureUploads(service_,*table_,uploading_,residents_,budget_,
                                     [this](GpuTicket ticket) { return ctx_.isComplete(ticket); });
    while (!pending_.empty()) {
        auto& p=pending_.front();
        auto it=residents_.find(p.handle.raw());
        if (it==residents_.end() || !service_.isLive(p.handle) || it->second.contentVersion!=p.contentVersion) { pending_.pop_front(); continue; }
        const size_t bytes=p.image->pixels.size();
        if (table_->full(uploading_.size()) || !budget_.reserve(bytes)) break;
        Upload upload; upload.handle=p.handle; upload.bytes=bytes; upload.contentVersion=p.contentVersion;
        if (this->upload(*p.image,it->second.useMips,&upload,it->second.layers,it->second.arrayView)) uploading_.push_back(std::move(upload));
        else budget_.complete(bytes);
        pending_.pop_front();
    }
}
bool TextureResidency::ready(RenderCore::TextureHandle handle) const {
    auto it=residents_.find(handle.raw());
    return it!=residents_.end() && it->second.image && service_.isLive(handle);
}
void TextureResidency::finishLoading() {
    const auto start=std::chrono::steady_clock::now();
    worker_.waitIdle();
    size_t previous=~size_t(0);
    unsigned batches=0;
    for (;;) {
        pump();
        const size_t remaining=pending_.size()+uploading_.size();
        if (!remaining || uploading_.empty()) break;
        // A full descriptor table retains placeholders and the pending queue;
        // never spin indefinitely trying to publish into a full table.
        if (remaining>=previous) break;
        previous=remaining;
        if (!ctx_.wait(uploading_.back().ticket,5000000000ull,"mission_texture_uploads")) break;
        ++batches;
    }
    // Keep the progress UI's descriptor snapshot intact. Normal frame-boundary
    // publication picks up the new table revision after its consumers retire.
    const auto readyCount=std::count_if(residents_.begin(),residents_.end(),[](const auto& entry) { const auto& [handle,resident]=entry; return bool(resident.image); });
    std::fprintf(stderr,"[VK_LOAD_TEXTURES v1] ready=%zu batches=%u blocked=%zu ms=%.3f\n",
        size_t(readyCount),batches,pending_.size()+uploading_.size(),
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
}
ResidentTextureRef TextureResidency::acquireLegacy(uint32_t width,uint32_t height,VkFormat format,const void* bytes,size_t count) {
    return acquirePixels(width,height,format,bytes,count,false);
}
ResidentTextureRef TextureResidency::acquirePixels(uint32_t width,uint32_t height,VkFormat format,const void* bytes,size_t count,
                                                   bool mipmaps,uint32_t layers,bool arrayView) {
    if (!bytes || !width || !height || width>RenderCore::kKtxMaxEdge || height>RenderCore::kKtxMaxEdge || !layers || layers>4096) return {};
    const bool compressed=format>=131 && format<=146;
    const size_t expected=(compressed ? size_t((width+3)/4)*((height+3)/4)*(format<=134?8u:16u) : size_t(width)*height*((format==VK_FORMAT_R16G16B16A16_SFLOAT || format==VK_FORMAT_R32G32_SFLOAT) ? 8u : 4u))*layers;
    if (count!=expected) return {};
    RenderCore::TextureKey key;
    key.source=RenderCore::makeTextureSourceId("legacy-cpu:P9",std::to_string(width)+"x"+std::to_string(height)+
        ":"+std::to_string(layers)+(arrayView ? ":array" : ":2d"),RenderCore::textureContentHash(static_cast<const uint8_t*>(bytes),count));
    key.format=uint32_t(format); key.hints=mipmaps ? gosHint_MipmapFilter0 : 0;
    RenderCore::KtxImage image; image.width=int(width); image.height=int(height); image.vkFormat=format; image.mipCount=1; image.mipByteOffsets={0};
    image.isCompressed=compressed; image.blockSizeBytes=(format>=131 && format<=134)?8:compressed?16:0;
    image.pixels.assign(static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+count);
    if (mipmaps && !compressed && (format==VK_FORMAT_R8G8B8A8_UNORM || format==VK_FORMAT_R8G8B8A8_SRGB)) {
        std::vector<uint8_t> current=image.pixels;
        uint32_t w=width,h=height;
        while (w>1 || h>1) {
            std::vector<uint8_t> next;
            const size_t layerBytes=size_t(w)*h*4;
            for (uint32_t layer=0;layer<layers;++layer) {
                std::vector<uint8_t> level(current.begin()+layer*layerBytes,current.begin()+(layer+1)*layerBytes);
                auto down=mc2tex::boxDownsampleRgba8(level,w,h);
                if (down.empty()) return {};
                next.insert(next.end(),down.begin(),down.end());
            }
            image.mipByteOffsets.push_back(image.pixels.size()); image.pixels.insert(image.pixels.end(),next.begin(),next.end()); ++image.mipCount;
            current=std::move(next); w=std::max(1u,w/2);h=std::max(1u,h/2);
        }
    }
    auto ref=acquire(key,image,{width,height,width,height},layers,arrayView);
    if (ref) { auto& r=residents_.at(ref.get().raw()); r.layers=layers; r.arrayView=arrayView; }
    return ref;
}
ResidentTextureRef TextureResidency::retain(RenderCore::TextureHandle handle) {
    return residents_.contains(handle.raw()) && service_.retain(handle) ? ResidentTextureRef(*this,handle,residents_.at(handle.raw()).arrayView) : ResidentTextureRef{};
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
VkImageView TextureResidency::view(RenderCore::TextureHandle handle, bool arrayView) const {
    auto it=residents_.find(handle.raw()); return it!=residents_.end() && it->second.image && service_.isLive(handle) ? it->second.image->view.get() : (arrayView || (it!=residents_.end() && it->second.arrayView) ? arrayPlaceholder_->view.get() : placeholder_->view.get());
}
std::shared_ptr<const void> TextureResidency::imageOwner(RenderCore::TextureHandle handle) const {
    auto it=residents_.find(handle.raw());
    return it!=residents_.end() && it->second.image && service_.isLive(handle) ? it->second.image : placeholder_;
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
