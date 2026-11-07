// Native scene-target ownership. Islands, presentation and explicit readbacks
// share these images; no parallel texture or framebuffer allocations exist.
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "mc2_czstring.h"
#include "gos_postprocess_internal.h"
#include "Gates/render_options.h"
#include "mc2fmt.h"
#include "vulkan_impact_decals.h"
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_context.h"
#include "vk_image_resource.h"
#endif
#include <cstdlib>

namespace {
int sceneSide(int pixels) {
    return (std::max)(1, static_cast<int>(std::lround(pixels * mc2::gate::get<"MC2_RENDER_SCALE">())));
}
}

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
    outputWidth_ = w > 0 ? w : 1;
    outputHeight_ = h > 0 ? h : 1;
    width_ = sceneSide(outputWidth_);
    height_ = sceneSide(outputHeight_);
#ifdef MC2_VULKAN_ISLAND
    const char* reason = "unknown";
    auto require = [&](bool ready, mc2::czstring slot) {
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
    sharedSceneObjectMv_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_OBJECT_MV,width_,height_,&reason);
    require(sharedSceneObjectMv_ != nullptr,"objectMotion");
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        sharedSceneObjectId_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_OBJECT_ID,width_,height_,&reason);
        require(sharedSceneObjectId_ != nullptr,"objectId");
    }
    using namespace RenderCore;
    auto describe = [&](RenderResourceId id,RenderResourceFormat format,mc2::czstring name,uint32_t bytes,bool ready) {
        RenderResourceDesc d;
        d.id=id; d.kind=RenderResourceKind::Texture2D; d.lifetime=RenderResourceLifetime::External;
        d.format=format; d.debugName=name; d.width=ready ? static_cast<uint32_t>(width_) : 0; d.height=ready ? static_cast<uint32_t>(height_) : 0;
        d.sizeBytes=ready ? static_cast<uint64_t>(width_)*static_cast<uint64_t>(height_)*bytes : 0; d.valid=ready;
        registerOrUpdateRenderResource(d);
    };
    const bool packedColor = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0)->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    describe(RenderResourceId::MainColor,packedColor ? RenderResourceFormat::R11G11B10F : RenderResourceFormat::RGBA16F,
             "MainColor",packedColor ? 4 : 8,sceneColorReady());
    describe(RenderResourceId::MainDepth,RenderResourceFormat::Depth32F,"MainDepth",8,sharedSceneDepth_ != nullptr);
    describe(RenderResourceId::MainNormal,mc2::options::lowCost() ? RenderResourceFormat::RGBA8 : RenderResourceFormat::RGBA16F,
             "MainNormal",mc2::options::lowCost() ? 4 : 8,sceneNormalReady());
    describe(RenderResourceId::SceneObjectId,RenderResourceFormat::Unknown,"SceneObjectId",4,sceneObjectIdReady());
    mc2::fmt::print_err("[SCENE_TARGETS] backend=Vulkan extent={:d}x{:d} object_id={:d} output={:d}x{:d}\n",
        mc2::fmt::as_i32(width_),mc2::fmt::as_i32(height_),mc2::fmt::as_i32(sceneObjectIdReady() ? 1 : 0),
        mc2::fmt::as_i32(outputWidth_),mc2::fmt::as_i32(outputHeight_));
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
    mc2SharedSceneColorDestroy(sharedSceneObjectMv_); sharedSceneObjectMv_=nullptr;
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
    if (w<=0 || h<=0 || (w==outputWidth_ && h==outputHeight_)) return;
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
