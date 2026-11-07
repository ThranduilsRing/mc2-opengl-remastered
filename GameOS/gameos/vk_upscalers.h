#pragma once
// Built-in temporal upscaler providers (RenderCore::IUpscalerProvider). Present and the material samplers use only
// the RenderCore registry plus the glue below, which fills the vendor-neutral per-frame contract from engine images.
#include <cstdint>
#include <volk.h>

struct VkImageResource;
namespace RenderCore { class IUpscalerProvider; struct UpscalerFrame; }

namespace mc2vk {
// Registers the built-in providers in fallback order (vk_upscalers.cpp); idempotent. A new vendor adds one provider
// file and one line there.
void registerBuiltinUpscalers();

// Picks the effective provider for a native-AA resolve of scene (wanted = the AA option allows it), fills the
// per-frame contract and prepares it. Moves the temporal-consumer registration (scene jitter) and providerPhases()
// with the active provider. Returns the active provider, or nullptr for the native path.
RenderCore::IUpscalerProvider* selectUpscaler(RenderCore::IUpscalerProvider*& active, const VkImageResource& scene,
                                              const VkImageResource* depth, bool wanted, uint32_t frame,
                                              RenderCore::UpscalerFrame& f);

// Adds the command list and the engine motion-vector / reactive images (render size) to the frame and dispatches.
bool dispatchUpscaler(RenderCore::IUpscalerProvider& up, RenderCore::UpscalerFrame& f, VkCommandBuffer cb,
                      VkImage motionVectors, VkImage reactive);
}  // namespace mc2vk
