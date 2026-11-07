// Upscaler provider registration, in fallback order (first available wins unless one is preferred by name).
// Adding a vendor: one provider file implementing RenderCore::IUpscalerProvider (docs/plans/upscaler-providers.md),
// its factory declaration below, and one registerUpscaler line.
#include "vk_upscalers.h"
#include "../../RenderCore/UpscalerProvider.h"

namespace mc2vk {
RenderCore::IUpscalerProvider* fsrUpscaler();   // vulkan_fsr_provider.cpp

void registerBuiltinUpscalers() {
    static const bool done = [] {
        RenderCore::registerUpscaler(fsrUpscaler());
        return true;
    }();
    (void)done;
}
}  // namespace mc2vk
