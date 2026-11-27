#include "../../mclib/camera.h"
#include "vulkan_mover_shadow.h"
#include "shadow_quality.h"
#include "visual_tuning_profile.h"
#include "vulkan_context.h"
#include "vk_barrier_batch_vk.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "vk_one_shot.h"
#include "vulkan_mech_island.h"
#include "terrain_field_store.h"
#include "diagnostic_trace.h"
#include <tracy/Tracy.hpp>
#include "../../RenderCore/gpu_abi/GpuMechInstance.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cmath>
#include <cstring>

extern void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap);
extern void gos_GetTerrainCameraPos(float* x, float* y, float* z);
float mc2NightLightFactor();   // mclib/camera.cpp: 0 day .. 1 full night
float mc2MoverShadowNightFade(float* darkness, float* moonLum, float* ambientLum);   // mclib/camera.cpp

namespace mc2vk {
using namespace mc2shadow;

namespace {
// Night fade of every directional sun shadow: none by day, a faint moon shadow (15% of the day
// darkening) at full night; evening missions keep a soft low-sun shadow in between.
float nightShadowFade() { return 0.85f * std::clamp(mc2NightLightFactor(), 0.0f, 1.0f); }
// Mover (mech/prop) shadows fade by the moon's share of the light, not the static shadows' flat 0.85.
float moverShadowFade() { float d,m,a; return mc2MoverShadowNightFade(&d,&m,&a); }
}

bool MoverShadowState::init(VulkanContext& ctx, bool unionPath) {
    device_ = ctx.device();
    frame_.flags[3]=unionPath?1:0;
    auto makeFrame = [&](OwnedBuffer& buffer) {
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        b.size = sizeof(MoverShadowFrame); b.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo a{}; a.usage = VMA_MEMORY_USAGE_AUTO;
        a.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        return buffer.create(ctx.allocator(), device_, b, a, "shadow.moverFrame") && buffer.upload(&frame_, sizeof(frame_));
    };
    if (!makeFrame(inactive_)) return false;
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_NEAREST; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    s.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    // Receivers read blocker depth and perform their own filtered comparisons.
    s.compareEnable = VK_FALSE;
    if (!create(sampler_, device_, [&](auto* h) { return vkCreateSampler(device_, &s, nullptr, h); })) return false;
    VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    i.imageType = VK_IMAGE_TYPE_2D; i.format = VK_FORMAT_D16_UNORM; i.extent = {1,1,1};
    i.mipLevels = i.arrayLayers = 1; i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL;
    i.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo a{}; a.usage = VMA_MEMORY_USAGE_AUTO;
    if (!dummy_.create(ctx.allocator(), device_, i, a, "shadow.moverDummy")) return false;
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = dummy_.get(); v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = VK_FORMAT_D16_UNORM;
    v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    if (!create(dummyView_, device_, [&](auto* h) { return vkCreateImageView(device_, &v, nullptr, h); })) return false;
    return submitOneShot(ctx, [&](VkCommandBuffer cb) {
        sync::cmdImageBarrier(ctx, cb, dummy_.get(), v.subresourceRange,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue clear{1.0f,0};
        vkCmdClearDepthStencilImage(cb,dummy_.get(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&v.subresourceRange);
        sync::cmdImageBarrier(ctx, cb, dummy_.get(), v.subresourceRange,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }, "mover shadow placeholder");
}

void MoverShadowState::clearGeometry() {
    bounds_.clear(); mechGeneration_ = ~0u; heightVersion_ = ~0ull; extent_ = 0; radiusBand_ = 0;
}

bool MoverShadowState::cacheGeometry(const Mc2ShadowGeomFeed& feed, const void* vertices, const void* indices) {
    if (!vertices || !indices || feed.vertexStride < 12) return false;
    bounds_.assign(feed.typeCount, Bounds{{1e30f,1e30f,1e30f},{-1e30f,-1e30f,-1e30f},false});
    const auto* vb = static_cast<const uint8_t*>(vertices);
    const auto* ib = static_cast<const uint32_t*>(indices);
    const uint64_t nv = feed.vboBytes / feed.vertexStride, ni = feed.iboBytes / sizeof(uint32_t);
    for (uint32_t t=0;t<feed.typeCount;++t) {
        auto& bounds = bounds_[t];
        for (uint32_t k=0;k<feed.typePacketCount[t];++k) {
            const uint32_t pk = feed.typeFirstPacket[t]+k;
            if (pk >= feed.packetCount) break;
            const auto& packet = feed.packets[pk];
            for (uint64_t index=packet.firstIndex; index<static_cast<uint64_t>(packet.firstIndex)+packet.indexCount && index<ni; ++index) {
                const uint64_t vertex = static_cast<uint64_t>(ib[index])+packet.baseVertex;
                if (vertex >= nv) continue;
                float p[3]; std::memcpy(p,vb+vertex*feed.vertexStride,sizeof(p));
                bounds.lo.x=(std::min)(bounds.lo.x,p[0]); bounds.hi.x=(std::max)(bounds.hi.x,p[0]);
                bounds.lo.y=(std::min)(bounds.lo.y,p[1]); bounds.hi.y=(std::max)(bounds.hi.y,p[1]);
                bounds.lo.z=(std::min)(bounds.lo.z,p[2]); bounds.hi.z=(std::max)(bounds.hi.z,p[2]);
                bounds.valid = true;
            }
        }
    }
    return true;
}

MoverSphere MoverShadowState::propSphere(const GpuStaticPropInstance& instance, const Bounds& b) const {
    const Vec3 c{(b.lo.x+b.hi.x)*0.5f,(b.lo.y+b.hi.y)*0.5f,(b.lo.z+b.hi.z)*0.5f};
    const Vec3 e{(b.hi.x-b.lo.x)*0.5f,(b.hi.y-b.lo.y)*0.5f,(b.hi.z-b.lo.z)*0.5f};
    const float* m=instance.modelMatrix;
    const Vec3 center{-(c.x*m[0]+c.y*m[1]+c.z*m[2]+m[3]),
        c.x*m[8]+c.y*m[9]+c.z*m[10]+m[11], c.x*m[4]+c.y*m[5]+c.z*m[6]+m[7]};
    const Vec3 extent{std::fabs(m[0])*e.x+std::fabs(m[1])*e.y+std::fabs(m[2])*e.z,
        std::fabs(m[4])*e.x+std::fabs(m[5])*e.y+std::fabs(m[6])*e.z,
        std::fabs(m[8])*e.x+std::fabs(m[9])*e.y+std::fabs(m[10])*e.z};
    return {center,std::sqrt(dot(extent,extent))};
}

void MoverShadowState::updateGround() {
    if (const auto* store=gos_TerrainFieldStore(); store && store->heightVersion()!=heightVersion_) {
        heightVersion_=store->heightVersion();
        ground_=store->height().empty()?0:*std::min_element(store->height().begin(),store->height().end());
        heightMax_=store->height().empty()?0:*std::max_element(store->height().begin(),store->height().end());
        radiusBand_=0;
    }
}

void MoverShadowState::appendMechs(const float cameraRows[16], Vec3 sun, float ground) {
    (void)cameraRows; (void)sun; (void)ground;
    for (const auto& a : mech_.actors) if (a.inView) spheres_.push_back(a.sphere);
}

void MoverShadowState::refreshMechActors(uint64_t epoch, const float cameraRows[16], Vec3 sun) {
    updateGround();
    mech_.begin(epoch); actorCount_ = 0;
    const float ground = ground_;
    const ViewPlanes view=makeViewPlanes(cameraRows);
    Mc2MechShadowFeed feed{};
    if (!mc2_mech_island_get_shadow_feed(&feed) || !feed.instances64 || !feed.bones64 || !feed.vboCpu || feed.vertexStride < 12) return;
    if (mechGeneration_ != feed.geomGen) {
        mechRadius_ = 0;
        const auto* vertices=static_cast<const uint8_t*>(feed.vboCpu);
        for (uint64_t i=0;i<feed.vboBytes/feed.vertexStride;++i) {
            float p[3]; std::memcpy(p,vertices+i*feed.vertexStride,sizeof(p));
            mechRadius_=(std::max)(mechRadius_,std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]));
        }
        mechGeneration_=feed.geomGen;
    }
    const auto* instances=static_cast<const GpuMechInstance*>(feed.instances64);
    const auto* bones=static_cast<const float*>(feed.bones64);
    boneOffsets_.clear();
    for (uint32_t i=0;i<feed.instanceCount;++i)
        if (instances[i].baseBoneOffset < feed.boneCount) boneOffsets_.push_back(instances[i].baseBoneOffset);
    std::sort(boneOffsets_.begin(),boneOffsets_.end());
    boneOffsets_.erase(std::unique(boneOffsets_.begin(),boneOffsets_.end()),boneOffsets_.end());
    actorCount_=static_cast<uint32_t>(boneOffsets_.size());
    for (size_t i=0;i<boneOffsets_.size();++i) {
        const uint32_t begin=boneOffsets_[i], end=i+1<boneOffsets_.size()?boneOffsets_[i+1]:feed.boneCount;
        Vec3 lo{1e30f,1e30f,1e30f}, hi{-1e30f,-1e30f,-1e30f};
        float scale=0;
        for (uint32_t k=begin;k<end;++k) {
            const float* b=bones+static_cast<size_t>(k)*16;
            const Vec3 p{-b[12],b[14],b[13]};
            lo.x=(std::min)(lo.x,p.x); hi.x=(std::max)(hi.x,p.x);
            lo.y=(std::min)(lo.y,p.y); hi.y=(std::max)(hi.y,p.y);
            lo.z=(std::min)(lo.z,p.z); hi.z=(std::max)(hi.z,p.z);
            float norm=0;
            for (int row=0;row<3;++row) for (int col=0;col<3;++col) norm+=b[row*4+col]*b[row*4+col];
            scale=(std::max)(scale,std::sqrt(norm));
        }
        const Vec3 extent{(hi.x-lo.x)*0.5f,(hi.y-lo.y)*0.5f,(hi.z-lo.z)*0.5f};
        const MoverSphere sphere{{(lo.x+hi.x)*0.5f,(lo.y+hi.y)*0.5f,(lo.z+hi.z)*0.5f},std::sqrt(dot(extent,extent))+mechRadius_*scale};
        mech_.actors.push_back({sphere, sweptShadowInView(sphere,sun,ground,view)});
    }
    auto& instActor_ = mech_.instActor;
    instActor_.resize(feed.instanceCount, -1);
    for (uint32_t i=0;i<feed.instanceCount;++i) {
        const uint32_t off=instances[i].baseBoneOffset;
        if (off >= feed.boneCount) continue;
        const auto it=std::lower_bound(boneOffsets_.begin(),boneOffsets_.end(),off);
        if (it!=boneOffsets_.end() && *it==off) instActor_[i]=static_cast<int32_t>(it-boneOffsets_.begin());
    }
}

bool MoverShadowState::prepare(uint64_t epoch, int mapSide, const float cameraRows[16], Vec3 sun,
                               std::span<const GpuStaticPropInstance> props, bool maskReady) {
    updateGround();
    if (mech_.epoch != epoch) refreshMechActors(epoch, cameraRows, sun);   // the table stays valid for the whole epoch (the sun pass reads it after this)
    props_.clear(); spheres_.clear();
    const ViewPlanes view=makeViewPlanes(cameraRows);
    // These are live rigid casters, including turrets and demoted static owners
    // absent from the bake. Every leaf whose swept shadow meets the view stays
    // in the live map, independently of nearby mechs and baked-mask readiness.
    uint32_t casters=0;
    for (const auto& prop:props) {
        if (prop.typeID >= bounds_.size() || !bounds_[prop.typeID].valid) continue;
        const auto sphere=propSphere(prop,bounds_[prop.typeID]);
        if (!sweptShadowInView(sphere,sun,ground_,view)) continue;
        ++casters;
        props_.push_back(prop); spheres_.push_back(sphere);
    }
    appendMechs(cameraRows,sun,ground_);
    frame_={};
    frame_.flags[0]=spheres_.empty()?0:1; frame_.flags[1]=shadowQuality().level==0?0:1; frame_.flags[2]=shadowQuality().level; frame_.flags[3]=1;
    frame_.sun[0]=sun.x; frame_.sun[1]=sun.y; frame_.sun[2]=sun.z;
    const float sharp=visualTuning_moverShadowSharpness();
    frame_.sun[3]=std::tan(moverSunAngularRadiusDeg(sharp) * 0.01745329252f);
    frame_.light[3]=sharp;
    frame_.light[0]=nightShadowFade(); frame_.light[2]=moverShadowFade();
    frame_.light[1]=static_cast<float>(mc2::gate::get<"MC2_SHADOW_MOVER_SAMPLES">());
    int side=0; float halfMap=0; gos_GetTerrainChunkGridInfo(&side,&halfMap);
    frame_.mask[0]=-halfMap; frame_.mask[1]=halfMap;
    frame_.mask[2]=1.0f/((std::max)(side-1,1)*kWorldUnitsPerVertex); frame_.mask[3]=maskReady?1.0f:0.0f;
    if (!spheres_.empty()) {
        const auto basis=lightBasis(sun);
        const auto focus = eye ? eye->getPosition() : glm::vec3(0.0f);
        auto region=moverGroundFootprint(cameraRows,ground_,halfMap,{focus.x,focus.y,focus.z});
        region.radius=moverRadiusBand(region.radius,radiusBand_);
        // A map-sized caster bound is conservative for every mission object;
        // never refit depth to the current visible/animated caster set.
        const float casterRadius=halfMap;
        std::array<MoverSphere,8> depthBounds;
        for (int c=0;c<8;++c) depthBounds[c]={{(c&1)?halfMap:-halfMap,
            (c&2)?halfMap:-halfMap,(c&4)?heightMax_+casterRadius:ground_},casterRadius};
        const Window window=fitMoverWindow(depthBounds,basis,ground_,mapSide,region);
        extent_=window.texelWorld*mapSide;
        windowMatrix(basis,window,frame_.matrix);
        frame_.params[0]=window.texelWorld; frame_.params[1]=1.0f/(window.dMax-window.dMin);
        frame_.params[2]=(std::max)(0.2f,window.texelWorld*0.35f); frame_.params[3]=1.0f/mapSide;
        if (frame_.flags[1]) buildMoverCoverage(frame_,spheres_,basis,window);
    }
    auto& ctx=getOrCreateEngineVulkanContext();
    active_=ctx.frameAlloc(sizeof(frame_));
    if (!active_) return false;
    std::memcpy(active_.ptr,&frame_,sizeof(frame_)); // the engine ring is host coherent
    activeFrame_=ctx.frameContext().frame;
    if (activeFrame_%30==0 && mc2_diag::tagRequested("MOVER_SHADOW")) {   // MC2_DIAG_TAGS=MOVER_SHADOW
        Vec3 camera{};
        gos_GetTerrainCameraPos(&camera.x,&camera.y,&camera.z); // render space (west, up, north)
        const MoverSphere caster=mech_.actors.empty()?MoverSphere{}:mech_.actors.front().sphere;
        const Vec3 delta{-caster.center.x-camera.x,caster.center.z-camera.y,caster.center.y-camera.z};
        mc2::fmt::print_err("[MOVER_SHADOW] frame={} mask_ready={} props_total={} casters_total={} movers_in_view={} texel_wu={:.3f} sharp={:.1f} extent_wu={:.3f} caster={:.3f},{:.3f},{:.3f} radius_wu={:.3f} eye_distance_wu={:.3f} view_yaw_deg={:.3f}\n",
            activeFrame_,maskReady?1:0,props.size(),casters,
            std::count_if(mech_.actors.begin(),mech_.actors.end(),[](const mc2shadow::MechActorTable::Actor& a){ return a.inView; }),
            frame_.params[0],sharp,extent_,caster.center.x,caster.center.y,caster.center.z,caster.radius,
            std::sqrt(dot(delta,delta)),std::atan2(cameraRows[1],cameraRows[0])*57.295779513f);
    }
    return true;
}

void reportDescWrites() {
    auto& c=descWriteCounts();
    const uint64_t issued=c.issued.exchange(0,std::memory_order_relaxed), skipped=c.skipped.exchange(0,std::memory_order_relaxed);
    TracyPlot("VK.DescWrites.Issued",static_cast<int64_t>(issued));
    TracyPlot("VK.DescWrites.Skipped",static_cast<int64_t>(skipped));
    static uint64_t sumIssued=0, sumSkipped=0; static uint32_t frames=0;
    sumIssued+=issued; sumSkipped+=skipped;
    if (++frames<600) return;
    if (mc2_diag::tagRequested("VK_DESC_WRITES"))   // MC2_DIAG_TAGS=VK_DESC_WRITES; uncached = issued+skipped
        mc2::fmt::print_err("[VK_DESC_WRITES v1] frames={} issued_per_frame={:.1f} skipped_per_frame={:.1f}\n",
            frames, static_cast<double>(sumIssued) / frames, static_cast<double>(sumSkipped) / frames);
    sumIssued=sumSkipped=0; frames=0;
}

bool MoverShadowState::bind(VkDevice device, VkDescriptorSet set, VkImageView depth, bool ready, DescWriteStamp* stamp) const {
    if (!inactive_ || !dummyView_ || !sampler_) return false;
    ready=ready && active_ && activeFrame_==getOrCreateEngineVulkanContext().frameContext().frame;
    const VkDescriptorImageInfo image{sampler_.get(),ready?depth:dummyView_.get(),ready?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo buffer{ready?active_.buffer:inactive_.get(),ready?active_.offset:0,sizeof(MoverShadowFrame)};
    if (!ready) {
        // No movers this frame, but the static sun shadow still takes the night fade.
        auto& ctx=getOrCreateEngineVulkanContext();
        if (auto slice=ctx.frameAlloc(sizeof(MoverShadowFrame))) {
            MoverShadowFrame idle{};
            idle.flags[3]=frame_.flags[3];
            idle.light[0]=nightShadowFade(); idle.light[2]=moverShadowFade();
            std::memcpy(slice.ptr,&idle,sizeof(idle));
            buffer={slice.buffer,slice.offset,sizeof(MoverShadowFrame)};
        }
    }
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet=set; writes[0].dstBinding=24;
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].descriptorCount=1; writes[0].pImageInfo=&image;
    writes[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet=set; writes[1].dstBinding=25;
    writes[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].descriptorCount=1; writes[1].pBufferInfo=&buffer;
    // Binding 24 is keyed per destination set on what it would hold (live depth vs dummy is the view+layout pair).
    const bool write24=!stamp || stamp->needsWrite({descKeyWord(set),descKeyWord(image.imageView),descKeyWord(image.sampler),descKeyWord(image.imageLayout)});
    vkUpdateDescriptorSets(device,write24?2u:1u,write24?writes:writes+1,0,nullptr);
    noteDescWrites(write24?2u:1u,write24?0u:1u);
    return true;
}
}
