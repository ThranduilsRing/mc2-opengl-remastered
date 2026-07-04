// vulkan_shared_scene_color.h — VK-P11 TIER-1 SHARED SCENE COLOR/NORMAL substrate.
// =============================================================================
// Sibling of vulkan_shared_scene_depth: swaps the engine's scene HDR color
// (sceneColorTex_) AND GBuffer1 normal (sceneNormalTex_) textures so each is
// backed by a VK-BUILT exportable R16G16B16A16_SFLOAT VkImageResource imported
// into GL as GL_RGBA16F. The GL scene renders terrain into these attachments
// exactly as before; the VK-P11 Tier-1 prop island then imports the SAME images
// as LOAD (preserve GL terrain) color attachments 0+1 and co-writes props
// in-place under the shared depth test — no side-image, no composite.
//
// Gate MC2_VK_PROP_COLOR_TIER1 (checked at the postprocess seam) + requires
// MC2_VK_SHARED_SCENE_DEPTH. Fail-soft: any interop failure falls through to the
// plain GL glTexImage2D alloc so the engine never crashes (gate-ON with no
// interop == the GL path). Rebuilt on resize (destroyFBOs tears these down too).
//
// Two faces:
//   1. GL-facing (this whole header without MC2_VULKAN_ISLAND-only volk): opaque
//      owner + create/destroy + GLuint out — usable from gos_postprocess.cpp
//      (a pure-GL TU that must NOT see a Vulkan type).
//   2. Island-facing (volk-gated section): accessors that hand the underlying
//      exportable VkImageResource + its GlInteropView to the VK prop island so it
//      can build LOAD color attachments over the SAME shared images.
// =============================================================================

#ifndef MC2_VULKAN_SHARED_SCENE_COLOR_H
#define MC2_VULKAN_SHARED_SCENE_COLOR_H

#ifdef MC2_VULKAN_ISLAND

#include <GL/glew.h>

// Which scene GBuffer slot a shared-color owner backs (also the island lookup key).
enum Mc2SharedSceneColorSlot {
    MC2_SHARED_SCENE_COLOR0 = 0,   // sceneColorTex_  (MRT0 lit color)
    MC2_SHARED_SCENE_NORMAL = 1,   // sceneNormalTex_ (MRT1 GBuffer1 normal)
    MC2_SHARED_SCENE_COLOR_SLOT_COUNT = 2
};

// Opaque owner of one exportable RGBA16F VK image + its GL interop view. Body lives
// in the .cpp so no Vulkan header leaks into GL TUs.
struct Mc2SharedSceneColor;

// Build a VK exportable R16G16B16A16_SFLOAT image on the SHARED engine Vulkan
// context and import it into GL as GL_RGBA16F. On success *outGlTex is the imported
// GL texture (GL_TEXTURE_2D, GL_RGBA16F) the caller substitutes for
// sceneColorTex_/sceneNormalTex_ at its alloc+attach site, and the returned pointer
// owns the lifetime. `slot` records which GBuffer slot this backs so the VK prop
// island can look it up. FAIL-SOFT: returns nullptr (and leaves *outGlTex untouched)
// on ANY failure; *outReason (if non-null) points to a short static string.
Mc2SharedSceneColor* mc2SharedSceneColorCreate(Mc2SharedSceneColorSlot slot,
                                               int w, int h, GLuint* outGlTex,
                                               const char** outReason);

// Destroy the GL interop view + VK image (safe on nullptr) and clear the island
// registry entry for its slot. The GL texture name from create() becomes invalid.
void mc2SharedSceneColorDestroy(Mc2SharedSceneColor* c);

#ifdef MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
// Island-facing accessors (only the VK prop island TU, which includes volk, defines
// the IMPL macro before including this header). Return the exportable VkImageResource
// + its GlInteropView for a slot, or nullptr if that slot has no active shared image
// this frame (interop unavailable / gate off / resized-away). The island uses these
// to build LOAD color attachments over the SAME shared images GL renders terrain to.
struct VkImageResource;
struct GlInteropView;
VkImageResource* mc2SharedSceneColorGetResource(Mc2SharedSceneColorSlot slot);
GlInteropView*   mc2SharedSceneColorGetGlView(Mc2SharedSceneColorSlot slot);
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_SHARED_SCENE_COLOR_H
