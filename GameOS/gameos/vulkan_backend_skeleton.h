// Declaration of the composite-present test scaffold driver, defined in
// vulkan_swapchain_present.cpp. Compiles to nothing unless MC2_VULKAN is defined.

#ifndef MC2_VULKAN_BACKEND_SKELETON_H
#define MC2_VULKAN_BACKEND_SKELETON_H

#ifdef MC2_VULKAN

// Presents `frames` frames (0 -> default 16) to the persistent mc2vk::VulkanPresentObject's own
// separate Vulkan window, with a resize at the midpoint. The game window is untouched. Fail-soft.
// Optional out-params receive the present-frame / resize / validation-error tallies.
bool mc2_vulkan_composite_present_scaffold(const char* spvDir, unsigned int frames,
                                           unsigned int* presentFramesOut,
                                           unsigned int* resizeCountOut,
                                           unsigned int* validationErrorsOut);

#endif // MC2_VULKAN

#endif // MC2_VULKAN_BACKEND_SKELETON_H
