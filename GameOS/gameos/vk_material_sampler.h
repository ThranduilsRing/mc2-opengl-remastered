#pragma once
// Shared material-sampler build and FSR/AA mip-bias refresh used by the Vulkan islands (definitions in vk_render_infra.cpp).
#include <functional>
#include <initializer_list>
#include "vk_owned.h"
#include "vk_render_infra.h"

namespace mc2vk::infra {
// Build a trilinear material sampler at the current effective bias (recorded in usedBias).
bool makeMaterialSampler(VkDevice device, OwnedSampler& samp, VkSamplerAddressMode address, float& usedBias);
struct SamplerRebuild { OwnedSampler* samp; std::function<bool()> make; };
// When the effective resolve state changed since usedBias: retire each live sampler (freed after its in-flight frames) and rebuild it.
// True = rebuilt, the owner rewrites its descriptor sets.
bool refreshSamplers(float& usedBias, std::initializer_list<SamplerRebuild> items);
}  // namespace mc2vk::infra
