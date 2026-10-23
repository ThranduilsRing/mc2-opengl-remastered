// Native scene-target ownership. Islands, presentation and explicit readbacks
// share these images; no parallel texture or framebuffer allocations exist.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"
#include "vulkan_impact_decals.h"
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_context.h"
#endif
#include <cstdlib>

bool gosPostProcess::sceneColorReady() const {
#ifdef MC2_VULKAN_ISLAND
    return sharedSceneColor_ != nullptr;
#else
    return false;
#endif
}
bool gosPostProcess::sceneNormalReady() const {
#ifdef MC2_VULKAN_ISLAND
    return sharedSceneNormal_ != nullptr;
#else
    return false;
#endif
}
bool gosPostProcess::sceneObjectIdReady() const {
#ifdef MC2_VULKAN_ISLAND
    return sharedSceneObjectId_ != nullptr;
#else
    return false;
#endif
}

void gosPostProcess::createSceneTargets(int w, int h) {
    width_ = w > 0 ? w : 1;
    height_ = h > 0 ? h : 1;
#ifdef MC2_VULKAN_ISLAND
    const char* reason = "unknown";
    auto require = [&](bool ready, const char* slot) {
        if (ready) return;
        mc2::fmt::print_err("[SCENE_TARGETS] native allocation failed slot={} reason={}\n",slot,reason ? reason : "unknown");
        std::abort();
    };
    sharedSceneColor_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_COLOR0,width_,height_,&reason);
    require(sharedSceneColor_ != nullptr,"color");
    sharedSceneDepth_ = mc2SharedSceneDepthCreate(width_,height_,&reason);
    require(sharedSceneDepth_ != nullptr,"depth");
    sharedSceneNormal_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_NORMAL,width_,height_,&reason);
    require(sharedSceneNormal_ != nullptr,"normal");
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        sharedSceneObjectId_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_OBJECT_ID,width_,height_,&reason);
        require(sharedSceneObjectId_ != nullptr,"objectId");
    }
    using namespace RenderCore;
    auto describe = [&](RenderResourceId id,RenderResourceFormat format,const char* name,uint32_t bytes,bool ready) {
        RenderResourceDesc d;
        d.id=id; d.kind=RenderResourceKind::Texture2D; d.lifetime=RenderResourceLifetime::External;
        d.format=format; d.debugName=name; d.width=ready ? uint32_t(width_) : 0; d.height=ready ? uint32_t(height_) : 0;
        d.sizeBytes=ready ? uint64_t(width_)*uint64_t(height_)*bytes : 0; d.valid=ready;
        registerOrUpdateRenderResource(d);
    };
    describe(RenderResourceId::MainColor,RenderResourceFormat::RGBA16F,"MainColor",8,sceneColorReady());
    describe(RenderResourceId::MainDepth,RenderResourceFormat::Depth32F,"MainDepth",8,sharedSceneDepth_ != nullptr);
    describe(RenderResourceId::MainNormal,RenderResourceFormat::RGBA16F,"MainNormal",8,sceneNormalReady());
    describe(RenderResourceId::SceneObjectId,RenderResourceFormat::Unknown,"SceneObjectId",4,sceneObjectIdReady());
    mc2::fmt::print_err("[SCENE_TARGETS] backend=Vulkan extent={:d}x{:d} object_id={:d}\n",
        mc2::fmt::as_i32(width_),mc2::fmt::as_i32(height_),mc2::fmt::as_i32(sceneObjectIdReady() ? 1 : 0));
#else
    mc2::fmt::print_err("[SCENE_TARGETS] Vulkan support is required\n");
    std::abort();
#endif
}

void gosPostProcess::destroySceneTargets() {
#ifdef MC2_VULKAN_ISLAND
    // Menu frames also sample these images, before any lazy post-effect exists.
    if (mc2vk::engineVulkanContextValid())
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("scene_targets");
    // Drop consumers before the images they borrow.
    destroyVkPostHzbNative();
    destroyVkPostFogNative();
    destroyVkPostScreenFxNative();
    mc2_vk_impact_decals_destroy();
    mc2SharedSceneColorDestroy(sharedSceneObjectId_); sharedSceneObjectId_=nullptr;
    mc2SharedSceneColorDestroy(sharedSceneNormal_); sharedSceneNormal_=nullptr;
    mc2SharedSceneColorDestroy(sharedSceneColor_); sharedSceneColor_=nullptr;
    mc2SharedSceneDepthDestroy(sharedSceneDepth_); sharedSceneDepth_=nullptr;
#endif
    using RenderCore::RenderResourceId;
    for (const auto id : {RenderResourceId::MainColor,RenderResourceId::MainDepth,RenderResourceId::MainNormal,RenderResourceId::SceneObjectId}) {
        RenderCore::RenderResourceDesc d; d.id=id;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

void gosPostProcess::resize(int w,int h) {
    if (w<=0 || h<=0 || (w==width_ && h==height_)) return;
    destroySceneTargets();
    createSceneTargets(w,h);
}

void gosPostProcess::destroy() {
    if (!initialized_) return;
    destroySceneTargets();
    hdriCpu_.reset(); hdriReady_=false; hdriEnabled_=false;
    s_postProcess=nullptr;
    initialized_=false;
}
