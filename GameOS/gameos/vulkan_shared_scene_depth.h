// vulkan_shared_scene_depth.h — VK-TIER1 SHARED SCENE DEPTH substrate.
// =============================================================================
// VK-ONLY (non-exportable) D32_SFLOAT_S8_UINT scene-depth image on the SHARED
// engine Vulkan context. The VK terrain/prop/mech/water islands co-write scene
// depth into this SAME image in place (direct-chain, no GL<->VK handoff). There
// is NO GL import — the engine's GL depth attachment is a plain-GL placeholder
// (gos_postprocess.cpp). This whole shared substrate arms only under the full
// native-present stack; otherwise gos_postprocess declines to pure GL depth.
//
// VK-RUNG8-ITEM5: the former exportable/glInteropView half (GLuint out param,
// vkOnlyNoGlView flag, HasGlView/GetGlView) is deleted. gos_postprocess.cpp (a
// GL TU) still never sees a volk/Vulkan type. Built with the Vulkan renderer.
// =============================================================================

#ifndef MC2_VULKAN_SHARED_SCENE_DEPTH_H
#define MC2_VULKAN_SHARED_SCENE_DEPTH_H



// Opaque owner of the VK-only D32S8 image the islands share. The struct body lives
// in the .cpp so no Vulkan header leaks into GL TUs. gos_postprocess holds a
// pointer to one of these.
struct Mc2SharedSceneDepth;

// Build a VK-only (non-exportable) D32_SFLOAT_S8_UINT image via the SHARED engine
// Vulkan context (ensureEngineVulkanContext). The returned pointer owns
// the lifetime (destroy with mc2SharedSceneDepthDestroy). There is no GL texture:
// the caller allocates its own plain-GL placeholder depth attachment.
//
// FAIL-SOFT: returns nullptr on ANY reason the image cannot be built — shared VK
// context not ready/invalid, allocation failure. *outReason (if non-null) points
// to a short static string. The caller MUST then decline the whole shared scene
// to pure GL; nothing crashes.
Mc2SharedSceneDepth* mc2SharedSceneDepthCreate(int w, int h, const char** outReason);

// Destroy the VK image (safe on nullptr). The caller must have already dropped its
// FBO depth attachment / re-created the FBO.
void mc2SharedSceneDepthDestroy(Mc2SharedSceneDepth* d);

#ifdef MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
// Island-facing accessor (only VK island TUs that include volk define the IMPL
// macro before including this header). Returns the ACTIVE shared scene-depth
// VK-only VkImageResource, or nullptr if none is live (declined / resized away).
// The VK Tier-1 islands co-write a LOAD depth attachment over the SAME image.
struct VkImageResource;
VkImageResource* mc2SharedSceneDepthGetResource();
#endif

#endif // MC2_VULKAN_SHARED_SCENE_DEPTH_H
