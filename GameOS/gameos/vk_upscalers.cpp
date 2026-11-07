// Upscaler provider registration, in fallback order (first available wins unless one is preferred by name), and the
// present-side glue that turns engine images into the vendor-neutral per-frame contract.
// Adding a vendor: one provider file implementing RenderCore::IUpscalerProvider (docs/plans/upscaler-providers.md),
// its factory declaration below, and one registerUpscaler line.
#include "vk_upscalers.h"
#include "vk_image_resource.h"
#include "../../RenderCore/UpscalerProvider.h"
#include "../../RenderCore/TemporalInputs.h"
#include <chrono>

namespace mc2vk {
RenderCore::IUpscalerProvider* fsrUpscaler();   // vulkan_fsr_provider.cpp

void registerBuiltinUpscalers() {
    static const bool done = [] {
        RenderCore::registerUpscaler(fsrUpscaler());
        return true;
    }();
    (void)done;
}

namespace {
RenderCore::UpscalerImage wrapImage(const VkImageResource& r) {
    return {reinterpret_cast<uint64_t>(r.image.get()), reinterpret_cast<uint64_t>(r.view.get()),
            static_cast<uint32_t>(r.format), static_cast<uint32_t>(r.usage), r.w, r.h};
}
}  // namespace

RenderCore::IUpscalerProvider* selectUpscaler(RenderCore::IUpscalerProvider*& active, const VkImageResource& scene,
                                              const VkImageResource* depth, bool wanted, uint32_t frame,
                                              RenderCore::UpscalerFrame& f) {
    namespace T = RenderCore::temporal;
    registerBuiltinUpscalers();
    RenderCore::IUpscalerProvider* up = (wanted && depth) ? RenderCore::effectiveUpscaler() : nullptr;
    if (up) {
        f.color = wrapImage(scene);
        f.depth = wrapImage(*depth);
        f.renderWidth = f.displayWidth = scene.w;   // native AA: render == display
        f.renderHeight = f.displayHeight = scene.h;
        T::jitterPixels(frame, up->jitterPhaseCount(f.renderWidth, f.displayWidth), f.jitterPx);
        RenderCore::motionVectorScaleFor(up->caps(), f.renderWidth, f.renderHeight, f.motionVectorScale);
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count()) f.frameDeltaMs = std::chrono::duration<float, std::milli>(now - last).count();
        last = now;
        f.frameIndex = frame;
        if (!up->prepare(f)) up = nullptr;   // failed or unusable this frame: native path, bias back to 0
    }
    if (up != active) {   // present owns the temporal-consumer registration that turns on the scene-feed jitter
        if (active) { active->deactivate(); T::consumerCount().fetch_sub(1); }
        if (up) T::consumerCount().fetch_add(1);
        active = up;
    }
    T::providerPhases().store(up ? up->jitterPhaseCount(f.renderWidth, f.displayWidth) : 0);
    return up;
}

bool dispatchUpscaler(RenderCore::IUpscalerProvider& up, RenderCore::UpscalerFrame& f, VkCommandBuffer cb,
                      VkImage motionVectors, VkImage reactive) {
    const uint32_t sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
    f.commandList = cb;
    f.motionVectors = {reinterpret_cast<uint64_t>(motionVectors), 0, VK_FORMAT_R16G16_SFLOAT, sampled, f.renderWidth, f.renderHeight};
    f.reactive = {reinterpret_cast<uint64_t>(reactive), 0, VK_FORMAT_R8_UNORM, sampled, f.renderWidth, f.renderHeight};
    return up.dispatch(f);
}
}  // namespace mc2vk
