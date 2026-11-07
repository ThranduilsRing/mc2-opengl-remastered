#include "vk_texture_residency.h"
#include "hitch_stats.h"
#include "present_thread.h"
#include "vk_texture_pending.h"
#include <tracy/Tracy.hpp>
#include "RenderCore/GpuResidencyPolicy.h"
#include "Gates/render_options.h"
#include "Gates/gate_cold.h"
#include "vulkan_context.h"
#include "vk_barrier_batch_vk.h"
#include "engine_present_vk.h"
#include "vk_one_shot.h"
#include "texture_decode.h"
#include <vk_mem_alloc.h>
#include "utils/mc2log.h"
#include "diagnostic_trace.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <chrono>
#include <cstdio>
#include "mc2fmt.h"

namespace mc2vk {
bool uiTextureServiceReady() { return engineVulkanContextValid() && getOrCreateEnginePresentVk().valid(); }
bool hasUiImage(RenderCore::TextureHandle handle) { return engineVulkanContextValid() && getOrCreateEngineVulkanContext().textures().contains(handle); }
void invalidateUiImage(RenderCore::TextureHandle handle) {
    if (engineVulkanContextValid()) getOrCreateEngineVulkanContext().textures().invalidate(handle);
}
void TextureResidency::invalidate(RenderCore::TextureHandle handle) {
    table_->erase(handle);
    auto it=residents_.find(handle.raw());
    if (it!=residents_.end()) { auto& [raw,resident]=*it; if (resident.image) ctx_.retire(std::move(resident.image)); residents_.erase(it); }
}
void requestUiImage(RenderCore::TextureHandle handle, std::function<std::shared_ptr<const RenderCore::KtxImage>()> decode) {
    getOrCreateEngineVulkanContext().textures().requestDecoded(handle,std::move(decode));
}
void TextureResidency::requestDecoded(RenderCore::TextureHandle handle, std::function<std::shared_ptr<const RenderCore::KtxImage>()> decode) {
    if (!service_.isLive(handle) || residents_.contains(handle.raw())) return;
    auto [it,inserted]=residents_.emplace(handle.raw(),Resident{{},0,false});
    auto& [raw,resident]=*it;
    resident.highPriority=true; resident.contentVersion=service_.contentVersion(handle);
    worker_.request(handle,std::move(decode),resident.contentVersion);
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
    auto& e=legacyUi_[gosHandle | (static_cast<uint64_t>(displaySpace | (mipmaps<<1))<<32)];
    settlePendingUiTexture(e,[this](auto h) { return service_.isLive(h); },
                           [this](auto h) { return contains(h); },[this](auto h) { return ready(h); });
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
    // The slot table is residency admission accounting only. No pipeline binds a bindless texture set (every island writes its own
    // per-draw descriptors from view()), so no VkDescriptorSet mirrors the table: the per-flight 4096-entry rewrite it used to get on
    // every residency revision is gone. A future bindless consumer creates one set with UPDATE_AFTER_BIND | PARTIALLY_BOUND.
    table_ = std::make_unique<RenderCore::TextureDescriptorTable>(capacity_);
    RenderCore::KtxImage image; image.width=image.height=2; image.mipCount=1; image.vkFormat=37; image.mipByteOffsets={0};
    // Async residency is normal during screen changes. Keep the checkerboard
    // diagnostic opt-in; players see a blank image until the upload publishes.
    if (false)
        image.pixels={255,0,255,255, 0,0,0,255, 0,0,0,255, 255,0,255,255};
    else
        image.pixels.assign(16,0);
    placeholder_=upload(image); if (!placeholder_) return false;
    RenderCore::KtxImage flat; flat.width=flat.height=1; flat.vkFormat=37; flat.mipCount=1; flat.mipByteOffsets={0};
    flat.pixels={128,128,255,255}; arrayPlaceholder_=upload(flat,false,nullptr,1,true);
    if (!arrayPlaceholder_) return false;
    table_->placeholder(placeholder_);
    if (mc2_diag::tagRequested("VK_TEXTURE_SERVICE"))
        MC2_LOG(Vk, Info, "[VK_TEXTURE_SERVICE v1] capacity={} placeholder=0 legacy_boundary=P9", capacity_);
    return true;
}

std::shared_ptr<TextureResidency::Image> TextureResidency::upload(const RenderCore::KtxImage& image, bool useMips, Upload* async, uint32_t layers, bool arrayView, bool vegetation) {
    ZoneScopedN("TextureResidency.Upload");
    ZoneValue(image.pixels.size());
    const bool rgba=image.vkFormat==37 || image.vkFormat==43;
    const bool floating=image.vkFormat==97 || image.vkFormat==100 || image.vkFormat==VK_FORMAT_R32G32_SFLOAT;
    const size_t stride=(image.vkFormat==97 || image.vkFormat==VK_FORMAT_R32G32_SFLOAT) ? 8u : 4u;
    const bool bc1=image.vkFormat>=131 && image.vkFormat<=134;
    const bool blocks=bc1 || image.vkFormat==137 || image.vkFormat==138 || image.vkFormat==141 || (image.vkFormat>=143 && image.vkFormat<=146);
    if ((!rgba && !floating && !blocks) || image.isCompressed!=blocks || (blocks && image.blockSizeBytes!=(bc1?8u:16u))) return {};
    const uint32_t w=image.width,h=image.height;
    if (!layers || layers>4096 || (!arrayView && layers!=1)) return {};
    if (!w || !h || w>RenderCore::kKtxMaxEdge || h>RenderCore::kKtxMaxEdge || image.mipCount<1 ||
        image.mipCount>15 || image.mipByteOffsets.size()!=static_cast<size_t>(image.mipCount)) return {};
    uint32_t fullChain=1;
    for (uint32_t edge=std::max(w,h);edge>1;edge>>=1) ++fullChain;
    if (static_cast<uint32_t>(image.mipCount)>fullChain) return {};
    VkPhysicalDeviceProperties deviceProps{}; vkGetPhysicalDeviceProperties(ctx_.physicalDevice(),&deviceProps);
    if (layers>deviceProps.limits.maxImageArrayLayers) {
        mc2::fmt::print_err("[VK_RESIDENCY v1] refused=texture layers={:d} limit={:d} fallback=placeholder\n",mc2::fmt::as_u32(layers),mc2::fmt::as_u32(deviceProps.limits.maxImageArrayLayers));
        return {};
    }
    VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(ctx_.physicalDevice(), VkFormat(image.vkFormat), &properties);
    constexpr auto need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & need)!=need) {
        // A device without this BC format (BC6H has no RGBA8 form): decode the chain on the CPU and upload RGBA8 instead.
        if (!blocks || layers!=1 || (image.vkFormat>=143 && image.vkFormat<=144)) return {};
        RenderCore::KtxImage decoded;
        decoded.alpha=image.alpha; decoded.width=image.width; decoded.height=image.height; decoded.isSrgb=image.isSrgb;
        decoded.mipCount=image.mipCount; decoded.vkFormat=image.isSrgb ? 43u : 37u;
        for (int level=0;level<image.mipCount;++level) {
            std::vector<uint8_t> rgba;
            if (!RenderCore::ktxDecodeBlocksToRgba8(image,level,rgba)) return {};
            decoded.mipByteOffsets.push_back(decoded.pixels.size());
            decoded.pixels.insert(decoded.pixels.end(),rgba.begin(),rgba.end());
        }
        std::fprintf(stderr,"[VK_RESIDENCY v1] bc_unsupported vkFormat=%u fallback=rgba8 %ux%u\n",image.vkFormat,w,h);
        return upload(decoded,useMips,async,layers,arrayView,vegetation);
    }
    std::vector<VkBufferImageCopy> copies;
    size_t expected=0;
    for (int level=0;level<image.mipCount;++level) {
        const uint32_t mw=std::max(1u,w>>level),mh=std::max(1u,h>>level);
        const size_t bytes=(image.isCompressed ? static_cast<size_t>((mw+3)/4)*((mh+3)/4)*image.blockSizeBytes : static_cast<size_t>(mw)*mh*stride)*layers;
        if (image.mipByteOffsets[level]!=expected || !bytes || expected>image.pixels.size() || bytes>image.pixels.size()-expected) return {};
        VkBufferImageCopy copy{}; copy.bufferOffset=expected; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,static_cast<uint32_t>(level),0,layers}; copy.imageExtent={mw,mh,1};
        copies.push_back(copy); expected+=bytes;
    }
    if (expected!=image.pixels.size()) return {};
    // Admit upload overlap as well as the resident image. Drop authored top mips before refusing;
    // arrays retain their layer count and all copies use the reduced base extent.
    const auto policy=RenderCore::gpuResidencyPolicy(mc2::options::current().shadowQuality);
    const auto maxEdge=std::min(floating?policy.terrainEdge:policy.textureEdge,deviceProps.limits.maxImageDimension2D);
    if (w>maxEdge || h>maxEdge || static_cast<uint64_t>(expected)*2>policy.textureUpload || !infra::admit(static_cast<uint64_t>(expected)*2 + 128*1024)) {
        if (image.mipCount>1) {
            RenderCore::KtxImage reduced=image;
            const size_t skip=image.mipByteOffsets[1];
            reduced.width=std::max(1u,w/2);
            reduced.height=std::max(1u,h/2); --reduced.mipCount;
            reduced.pixels.erase(reduced.pixels.begin(),reduced.pixels.begin()+skip);
            reduced.mipByteOffsets.erase(reduced.mipByteOffsets.begin());
            for(auto& offset:reduced.mipByteOffsets) offset-=skip;
            mc2::fmt::print_err("[VK_RESIDENCY v1] refused=texture extent={:d}x{:d} fallback=mip_{:d}x{:d}\n",mc2::fmt::as_u32(w),mc2::fmt::as_u32(h),mc2::fmt::as_u32(reduced.width),mc2::fmt::as_u32(reduced.height));
            return upload(reduced,useMips,async,layers,arrayView,vegetation);
        }
        if(!blocks && (w>1 || h>1)) {
            RenderCore::KtxImage reduced=image;
            reduced.width=std::max(1u,w/2);
            reduced.height=std::max(1u,h/2);
            const size_t layerBytes=static_cast<size_t>(reduced.width)*reduced.height*stride;
            reduced.pixels.resize(layerBytes*layers);
            for(uint32_t layer=0;layer<layers;++layer) {
                if(rgba) {
                    std::vector<uint8_t> src(image.pixels.begin()+static_cast<size_t>(layer)*w*h*stride,image.pixels.begin()+static_cast<size_t>(layer+1)*w*h*stride);
                    const auto down=mc2tex::boxDownsampleRgba8(src,w,h);
                    std::memcpy(reduced.pixels.data()+layer*layerBytes,down.data(),layerBytes);
                } else {
                    // Float data keeps its native format; nearest reduction preserves encoded gradients/heights.
                    for(uint32_t y=0;y<uint32_t(reduced.height);++y) for(uint32_t x=0;x<uint32_t(reduced.width);++x)
                        std::memcpy(reduced.pixels.data()+layer*layerBytes+(static_cast<size_t>(y)*reduced.width+x)*stride,
                            image.pixels.data()+(static_cast<size_t>(layer)*w*h+static_cast<size_t>(y*2)*w+x*2)*stride,stride);
                }
            }
            mc2::fmt::print_err("[VK_RESIDENCY v1] refused=texture extent={:d}x{:d} fallback=resample_{:d}x{:d}\n",mc2::fmt::as_u32(w),mc2::fmt::as_u32(h),mc2::fmt::as_u32(reduced.width),mc2::fmt::as_u32(reduced.height));
            return upload(reduced,useMips,async,layers,arrayView,vegetation);
        }
        mc2::fmt::print_err("[VK_RESIDENCY v1] refused=texture extent={:d}x{:d} bytes={:d} fallback=placeholder\n",mc2::fmt::as_u32(w),mc2::fmt::as_u32(h),mc2::fmt::as_size(expected));
        return {};
    }
    OwnedBuffer staging;
    { ZoneScopedN("TextureResidency.Stage");
      if (!staging.createFilled(ctx_.allocator(),ctx_.device(),image.pixels.data(),image.pixels.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,vegetation ? "veg.stage.atlas" : "texture.service.staging")) return {}; }
    auto resource=std::make_shared<Image>();
    { ZoneScopedN("TextureResidency.CreateImage"); if (arrayView) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VkFormat(image.vkFormat);
        ci.extent={w,h,1}; ci.mipLevels=static_cast<uint32_t>(image.mipCount); ci.arrayLayers=layers; ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{}; aci.usage=VMA_MEMORY_USAGE_AUTO;
        if (!resource->image.create(ctx_.allocator(),ctx_.device(),ci,aci,vegetation ? "veg.atlas" : "texture.service.array")) return {};
    } else if (!resource->image.createSampled2D(ctx_.allocator(),ctx_.device(),VkFormat(image.vkFormat),w,h,image.mipCount,0,"texture.service.image")) return {}; }
    const auto record=[this,buffer=staging.get(),target=resource->image.get(),levels=static_cast<uint32_t>(image.mipCount),layers,copies](VkCommandBuffer cb){
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,layers};
        mc2vk::sync::cmdImageBarrier(ctx_,cb,target,range,
            VK_IMAGE_LAYOUT_UNDEFINED,VK_PIPELINE_STAGE_2_NONE,0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cb,buffer,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,static_cast<uint32_t>(copies.size()),copies.data());
        mc2vk::sync::cmdImageBarrier(ctx_,cb,target,range,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=resource->image.get();vi.viewType=arrayView ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;vi.format=VkFormat(image.vkFormat);
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,useMips ? static_cast<uint32_t>(image.mipCount) : 1u,0,layers};
    if (!mc2vk::create(resource->view,ctx_.device(),[&](VkImageView* v){return vkCreateImageView(ctx_.device(),&vi,nullptr,v);})) return {};
    // This context creates only the graphics family. Copy and shader barriers therefore stay on the same queue;
    // no unsupported transfer family or ownership handoff is assumed.
    if (async) {
        ZoneScopedN("TextureResidency.SubmitUpload");
        ctx_.uploads().record(record);
        async->staging=std::move(staging); async->image=resource;
    } else if (!submitOneShot(ctx_,record,"texture placeholder initialization")) return {};
    return resource;
}

ResidentTextureRef TextureResidency::acquire(const RenderCore::TextureKey& key, const RenderCore::KtxImage& image, RenderCore::TextureDimensions dimensions, uint32_t layers, bool arrayView) {
    auto handle=service_.acquire(key,RenderCore::kNoTextureNode,dimensions);
    if (!handle.isValid()) return {};
    if (residents_.contains(handle.raw())) return ResidentTextureRef(*this,handle,arrayView);
    residents_.emplace(handle.raw(),Resident{{},0,mc2tex::wantsMipmaps(key.hints),layers,arrayView});
    residents_.at(handle.raw()).contentVersion=service_.contentVersion(handle);
    residents_.at(handle.raw()).vegetation=key.source.starts_with("vegetation:P2|");
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
    ZoneScopedN("TextureResidency.Pump");
    const auto started=std::chrono::steady_clock::now();
    if (!present_thread::onWorker()) {
        ZoneScopedN("TextureResidency.CollectSources"); service_.collectSourceRetirements();
    }
    std::vector<RenderCore::TextureHandle> retired;
    { std::lock_guard lock(retiredMutex_); retired.swap(retired_); }
    { ZoneScopedN("TextureResidency.RetireImages"); for (auto handle : retired) {
        table_->erase(handle);
        auto it=residents_.find(handle.raw());
        if (it!=residents_.end()) { auto& [raw,resident]=*it; if (resident.image) ctx_.retire(std::move(resident.image)); residents_.erase(it); }
    } }
    { ZoneScopedN("TextureResidency.TakeDecoded"); for (auto& result : worker_.take()) {
        auto it=residents_.find(result.handle.raw());
        if (!result.image || !service_.isLive(result.handle) || it==residents_.end()) continue;
        const auto& [raw,resident]=*it;
        if (result.contentVersion!=resident.contentVersion) continue;
        Pending pending{result.handle,std::move(result.image),result.contentVersion};
        if (resident.highPriority) pending_.push_front(std::move(pending));
        else pending_.push_back(std::move(pending));
    } }
    // Queue order makes completion a prefix: idle resident images are never scanned.
    { ZoneScopedN("TextureResidency.Publish");
      RenderCore::publishTextureUploads(service_,*table_,uploading_,residents_,budget_,
                                       [this](GpuTicket ticket) { return ctx_.isComplete(ticket); }); }
    const size_t firstNew = uploading_.size();
    std::deque<Pending> batch;
    pumpPendingTextureUploads(pending_,uploading_,batch,budget_,[this](const Pending& p) {
        auto it=residents_.find(p.handle.raw());
        if (it==residents_.end() || !service_.isLive(p.handle)) return false;
        const auto& [raw,resident]=*it;
        return resident.contentVersion==p.contentVersion;
    },[this,firstNew,started] {
        // A screen can request many images at once. Spread staging across
        // frames instead of draining the decoded queue on its first render.
        // Always allow one image so large textures still make progress.
        return table_->full(uploading_.size()) ||
            (uploading_.size()>firstNew && std::chrono::steady_clock::now()-started>std::chrono::milliseconds(2));
    },[this](const Pending& p, Upload& next) {
        const auto& r=residents_.at(p.handle.raw());
        return static_cast<bool>(upload(*p.image,r.useMips,&next,r.layers,r.arrayView,r.vegetation));
    },[this](const Pending& p) {
        // A missing resident is terminal failure, distinct from a queued upload.
        // Retained handles may reacquire, and dynamic pages may submit newer pixels.
        residents_.erase(p.handle.raw());
        mc2::fmt::print_err("[VK_RESIDENCY v1] dropped=upload handle={:d} attempts={:d} fallback=placeholder\n",mc2::fmt::as_u32(p.handle.raw()),mc2::fmt::as_u32(p.attempts));
    });
    if (uploading_.size() > firstNew) {
        ZoneScopedN("TextureResidency.SubmitBatch");
        mc2_hitch2::Scope hitchResidency("residency.submit");
        // One command buffer and submit for the ready batch. Each image and
        // staging owner stays queued until this shared completion ticket retires.
        const GpuTicket ticket = ctx_.flushUploads();
        RenderCore::finishTextureUploadBatch(uploading_,firstNew,pending_,batch,budget_,ticket);
    }
}
bool TextureResidency::ready(RenderCore::TextureHandle handle) const {
    auto it=residents_.find(handle.raw());
    if (it==residents_.end()) return false;
    const auto& [raw,resident]=*it;
    return resident.image && service_.isLive(handle);
}
uint64_t TextureResidency::residentBytes() const {
    uint64_t total=0;
    for (const auto& [handle,resident] : residents_) {
        if (!resident.image) continue;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(ctx_.allocator(),resident.image->image.allocation(),&info);
        total+=info.size;
    }
    return total;
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
    const auto readyCount=std::count_if(residents_.begin(),residents_.end(),[](const auto& entry) { const auto& [handle,resident]=entry; return static_cast<bool>(resident.image); });
    if (mc2::gate::get<"MC2_DIAG_VERBOSE">()) std::fprintf(stderr,"[VK_LOAD_TEXTURES v1] ready=%zu batches=%u blocked=%zu ms=%.3f\n",
        static_cast<size_t>(readyCount),batches,pending_.size()+uploading_.size(),
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
}
ResidentTextureRef TextureResidency::acquireLegacy(uint32_t width,uint32_t height,VkFormat format,const void* bytes,size_t count) {
    return acquirePixels(width,height,format,bytes,count,false);
}
ResidentTextureRef TextureResidency::acquirePixels(uint32_t width,uint32_t height,VkFormat format,const void* bytes,size_t count,
                                                   bool mipmaps,uint32_t layers,bool arrayView) {
    if (!bytes || !width || !height || width>RenderCore::kKtxMaxEdge || height>RenderCore::kKtxMaxEdge || !layers || layers>4096) return {};
    const bool compressed=format>=131 && format<=146;
    const size_t expected=(compressed ? static_cast<size_t>((width+3)/4)*((height+3)/4)*(format<=134?8u:16u) : static_cast<size_t>(width)*height*((format==VK_FORMAT_R16G16B16A16_SFLOAT || format==VK_FORMAT_R32G32_SFLOAT) ? 8u : 4u))*layers;
    if (count!=expected) return {};
    RenderCore::TextureKey key;
    key.source=RenderCore::makeTextureSourceId("legacy-cpu:P9",std::to_string(width)+"x"+std::to_string(height)+
        ":"+std::to_string(layers)+(arrayView ? ":array" : ":2d"),RenderCore::textureContentHash(static_cast<const uint8_t*>(bytes),count));
    key.format=static_cast<uint32_t>(format); key.hints=mipmaps ? gosHint_MipmapFilter0 : 0;
    RenderCore::KtxImage image; image.width=static_cast<int>(width); image.height=static_cast<int>(height); image.vkFormat=format; image.mipCount=1; image.mipByteOffsets={0};
    image.isCompressed=compressed; image.blockSizeBytes=(format>=131 && format<=134)?8:compressed?16:0;
    image.pixels.assign(static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+count);
    if (mipmaps && !compressed && (format==VK_FORMAT_R8G8B8A8_UNORM || format==VK_FORMAT_R8G8B8A8_SRGB)) {
        std::vector<uint8_t> current=image.pixels;
        uint32_t w=width,h=height;
        while (w>1 || h>1) {
            std::vector<uint8_t> next;
            const size_t layerBytes=static_cast<size_t>(w)*h*4;
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
            auto& [raw,resident]=*it;
            if (resident.image) ctx_.retire(std::move(resident.image));
            residents_.erase(it);
        }
    }
}
VkImageView TextureResidency::view(RenderCore::TextureHandle handle, bool arrayView) const {
    const auto it=residents_.find(handle.raw());
    const Resident* resident=nullptr;
    if (it!=residents_.end()) { const auto& [raw,found]=*it; resident=&found; }
    if (resident && resident->image && service_.isLive(handle)) return resident->image->view.get();
    return (arrayView || (resident && resident->arrayView)) ? arrayPlaceholder_->view.get() : placeholder_->view.get();
}
std::shared_ptr<const void> TextureResidency::imageOwner(RenderCore::TextureHandle handle) const {
    auto it=residents_.find(handle.raw());
    if (it==residents_.end()) return placeholder_;
    const auto& [raw,resident]=*it;
    return resident.image && service_.isLive(handle) ? resident.image : placeholder_;
}
} // namespace mc2vk
