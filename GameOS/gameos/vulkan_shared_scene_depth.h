// vulkan_shared_scene_depth.h — VK-TIER1 SHARED SCENE DEPTH substrate (step 2).
// =============================================================================
// GL-ONLY consumer slice: swap the engine's scene-depth texture (sceneDepthTex_)
// so it is backed by a VK-BUILT exportable D32_SFLOAT_S8_UINT VkImageResource,
// imported into GL as a GL_DEPTH24_STENCIL8 texture (depth+stencil aspect usable
// for BOTH sampling AND FBO depth-stencil attachment). The P5-T0 + depth-test
// spikes proved this substitution is transparent to the 42 GL depth-sample
// consumers, the glCopyImageSubData copy path, and the stencil glTextureView.
//
// This slice produces ONLY the shared backing image. There is NO VK color/depth
// consumer yet (that is the P11 Tier-1 slice). Its whole purpose is to isolate
// the attach-swap risk: gate MC2_VK_SHARED_SCENE_DEPTH ON substitutes the shared
// image; gate OFF (or interop unavailable) is byte-identical to the plain GL
// GL_DEPTH24_STENCIL8 texture.
//
// The interface intentionally exposes ONLY a GLuint (the imported GL texture) +
// opaque lifetime handles, so gos_postprocess.cpp (a GL translation unit) never
// sees a volk/Vulkan type. The whole TU compiles to nothing unless
// MC2_VULKAN_ISLAND is defined (same discipline as the other island TUs).
// =============================================================================

#ifndef MC2_VULKAN_SHARED_SCENE_DEPTH_H
#define MC2_VULKAN_SHARED_SCENE_DEPTH_H

#ifdef MC2_VULKAN_ISLAND

#include <GL/glew.h>

// Opaque owner of the VK exportable D32S8 image + its GL interop view. The struct
// body lives in the .cpp so no Vulkan header leaks into GL TUs. gos_postprocess
// holds a pointer to one of these under #ifdef MC2_VULKAN_ISLAND.
struct Mc2SharedSceneDepth;

// Build a VK exportable D32_SFLOAT_S8_UINT image via the SHARED engine Vulkan
// context (ensureEngineVulkanContextForInterop) and import it into GL as a
// GL_DEPTH24_STENCIL8 texture. On success *outGlTex is the imported GL texture
// name (a GL_TEXTURE_2D, GL_DEPTH24_STENCIL8) the caller substitutes for
// sceneDepthTex_ at its alloc+attach site, and the returned pointer owns the
// lifetime (destroy with mc2SharedSceneDepthDestroy).
//
// FAIL-SOFT: returns nullptr (and leaves *outGlTex untouched) on ANY reason the
// shared image cannot be built — shared VK context not ready/invalid, image not
// exportable, GL import/LUID failure, GL error. *outReason (if non-null) points
// to a short static string describing the fallback cause. The caller MUST then
// fall back to the plain GL GL_DEPTH24_STENCIL8 alloc; nothing crashes.
Mc2SharedSceneDepth* mc2SharedSceneDepthCreate(int w, int h, GLuint* outGlTex,
                                               const char** outReason);

// Destroy the GL interop view + the VK image (safe on nullptr). The GL texture
// name returned by create() becomes invalid; the caller must have already
// dropped its sceneDepthTex_ reference / re-created the FBO.
void mc2SharedSceneDepthDestroy(Mc2SharedSceneDepth* d);

#ifdef MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
// Island-facing accessors (only VK island TUs that include volk define the IMPL
// macro before including this header). Return the ACTIVE shared scene-depth
// exportable VkImageResource + its GlInteropView, or nullptr if none is live (gate
// OFF / interop fell back / resized away). The VK-P11 Tier-1 prop island uses these
// to build a LOAD depth attachment over the SAME shared depth GL authored.
struct VkImageResource;
struct GlInteropView;
VkImageResource* mc2SharedSceneDepthGetResource();
GlInteropView*   mc2SharedSceneDepthGetGlView();
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_SHARED_SCENE_DEPTH_H
