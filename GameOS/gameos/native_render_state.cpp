#include "native_render_state.h"
#include "gos_postprocess.h"
#include "engine_present_vk.h"
#include "vulkan_context.h"
#include "vk_scene_readback.h"

namespace mc2vk {
bool nativeRenderTargetAvailable(RenderCore::RenderResourceId target) {
#ifdef MC2_VULKAN   // the no-Vulkan unit-test tree links this TU without a device
    using RenderCore::RenderResourceId;
    const auto* pp=getGosPostProcess();
    switch(target) {
        case RenderResourceId::MainColor: return pp && pp->sceneColorReady();
        case RenderResourceId::MainNormal: return pp && pp->sceneNormalReady();
        case RenderResourceId::SceneObjectId: return pp && pp->sceneObjectIdReady();
        case RenderResourceId::MainDepth: {
            int w=0,h=0; return sceneReadbackExtent(w,h,SceneDepth);
        }
        case RenderResourceId::Backbuffer: return getOrCreateEnginePresentVk().valid();
        case RenderResourceId::Unknown: return false;
        default: return RenderCore::getRenderResource(target) != nullptr;
    }
#else
    (void)target;
    return false;
#endif
}
bool nativeRenderHealthy() {
#ifdef MC2_VULKAN
    return engineVulkanContextValid() && getOrCreateEngineVulkanContext().health().validationErrors.load()==0;
#else
    return false;
#endif
}
}
