// vulkan_shared_static_shadow.h -- VK-backed static shadow-map substrate.
//
// GL still renders the static shadow pass. This optional substrate only swaps the
// static shadow texture allocation to a VK exportable D32_SFLOAT image imported
// into GL as GL_DEPTH_COMPONENT32F, so later native screen-shadow code can sample
// the same image without a GL copy/readback.

#ifndef MC2_VULKAN_SHARED_STATIC_SHADOW_H
#define MC2_VULKAN_SHARED_STATIC_SHADOW_H

#ifdef MC2_VULKAN_ISLAND

#include <GL/glew.h>

struct Mc2SharedStaticShadow;

Mc2SharedStaticShadow* mc2SharedStaticShadowCreate(int w, int h, GLuint* outGlTex,
                                                   const char** outReason);
void mc2SharedStaticShadowDestroy(Mc2SharedStaticShadow* s);

#ifdef MC2_VULKAN_SHARED_STATIC_SHADOW_IMPL_ISLAND
struct VkImageResource;
struct GlInteropView;
VkImageResource* mc2SharedStaticShadowGetResource();
GlInteropView*   mc2SharedStaticShadowGetGlView();
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_SHARED_STATIC_SHADOW_H
