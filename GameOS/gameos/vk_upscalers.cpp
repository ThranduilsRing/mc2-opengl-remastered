// Upscaler provider registration, in fallback order (first available wins unless one is preferred by name), and the
// present-side glue that turns engine images into the vendor-neutral per-frame contract.
// Adding a vendor: one provider file implementing RenderCore::IUpscalerProvider (docs/plans/upscaler-providers.md),
// its factory declaration below, and one registerUpscaler line.
#include <cmath>
#include "vk_upscalers.h"
#include "vk_image_resource.h"
#include "../../RenderCore/UpscalerProvider.h"
#include "../../RenderCore/TemporalInputs.h"
#include "mc2fmt.h"
#include "diagnostic_trace.h"
#include <algorithm>
#include <chrono>

namespace mc2vk {
#ifdef _WIN32
RenderCore::IUpscalerProvider* fsrUpscaler();   // vulkan_fsr_provider.cpp
#endif

void registerBuiltinUpscalers() {
    static const bool done = [] {
#ifdef _WIN32
        RenderCore::registerUpscaler(fsrUpscaler());
#endif
        return true;
    }();
    (void)done;
}

namespace {
RenderCore::UpscalerImage wrapImage(const VkImageResource& r) {
    return {reinterpret_cast<uint64_t>(r.image.get()), reinterpret_cast<uint64_t>(r.view.get()),
            static_cast<uint32_t>(r.format), static_cast<uint32_t>(r.usage), r.w, r.h};
}
// One [VK_PRESENT] line per change of provider, extents or a meaningful projection change (the dynamic near plane
// moves constantly, so near/far only count past 2x and FOV past 0.5 degrees), so captures record the provider inputs.
void logInputsOnChange(const RenderCore::IUpscalerProvider& up, const RenderCore::UpscalerFrame& f) {
    static const RenderCore::IUpscalerProvider* lastUp = nullptr;
    static uint32_t ext[4]{};
    static float proj[3]{};
    static bool flags[3]{};
    const uint32_t nowExt[4]{f.renderWidth, f.renderHeight, f.displayWidth, f.displayHeight};
    const bool nowFlags[3]{f.depthReversed, f.depthInfinite, f.perspective};
    const auto ratioFar = [](float a, float b) { return a > 0.f && b > 0.f ? (a > b ? a / b : b / a) > 2.f : a != b; };
    const bool changed = lastUp != &up || !std::equal(nowExt, nowExt + 4, ext) || !std::equal(nowFlags, nowFlags + 3, flags) ||
                         ratioFar(f.cameraNear, proj[0]) || ratioFar(f.cameraFar, proj[1]) ||
                         std::fabs(f.cameraFovY - proj[2]) * 57.29578f > 0.5f;
    if (!changed) return;
    lastUp = &up;
    std::copy(nowExt, nowExt + 4, ext);
    std::copy(nowFlags, nowFlags + 3, flags);
    proj[0] = f.cameraNear; proj[1] = f.cameraFar; proj[2] = f.cameraFovY;
    if (mc2_diag::tagRequested("VK_PRESENT"))
        mc2::fmt::print_err("[VK_PRESENT] upscaler_inputs upscaler={} render={}x{} display={}x{} near={:.1f} far={:.1f} "
                            "fov_y_deg={:.2f} reverse_z={} infinite_far={} perspective={}\n", up.name(), f.renderWidth,
                            f.renderHeight, f.displayWidth, f.displayHeight, f.cameraNear, f.cameraFar,
                            f.cameraFovY * 57.29578f, f.depthReversed ? 1 : 0, f.depthInfinite ? 1 : 0, f.perspective ? 1 : 0);
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
        const T::Projection p = T::projection().load(std::memory_order_relaxed);
        f.cameraNear = p.nearZ; f.cameraFar = p.farZ; f.cameraFovY = p.fovY;
        f.depthReversed = p.reverseZ; f.depthInfinite = p.infiniteFar; f.perspective = p.perspective;
        logInputsOnChange(*up, f);
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
    // Unit coverage is fully reactive: also discard temporal locks there. The HUD is already drawn in the present tail.
    f.transparency = f.reactive;
    return up.dispatch(f);
}
}  // namespace mc2vk
