// vk_depth_caster_pipeline.h -- the one depth-only shadow-caster pipeline state shared by the shadow island's casters (static props, alpha-tested props, mechs).
//
// WHAT: instanced triangle-list draws into a depth-only render pass with no colour attachment, LESS forward-Z (the GL shadow maps are forward-Z, cleared to 1.0),
// no culling (GL's shadow pass draws both faces), depth bias enabled and set per pass, viewport and scissor dynamic.
// WHY: every caster kind differs only in its vertex layout, shaders and descriptor layout; the state that must agree with the GL shadow pass lives here once.
#ifndef MC2_VK_DEPTH_CASTER_PIPELINE_H
#define MC2_VK_DEPTH_CASTER_PIPELINE_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "vk_owned.h"

#include <cstdint>
#include <span>

namespace mc2vk {

// One vertex stream (`binding`, its stride and attributes as the caller filled them: the offline layout checker reads those assignments next to the shader name).
// False, with `out` empty, when vkCreateGraphicsPipelines fails (the caller logs the reason it names).
bool createDepthCasterPipeline(OwnedPipeline& out, VkDevice device, VkRenderPass pass, VkPipelineLayout layout, VkShaderModule vert, VkShaderModule frag,
                               const VkVertexInputBindingDescription& binding, std::span<const VkVertexInputAttributeDescription> attributes);

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_DEPTH_CASTER_PIPELINE_H
