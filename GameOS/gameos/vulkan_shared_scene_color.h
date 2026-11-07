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


// Which scene GBuffer slot a shared-color owner backs (also the island lookup key).
enum Mc2SharedSceneColorSlot {
    MC2_SHARED_SCENE_COLOR0 = 0,   // sceneColorTex_  (MRT0 lit color)
    MC2_SHARED_SCENE_NORMAL = 1,   // sceneNormalTex_ (MRT1 GBuffer1 normal)
    MC2_SHARED_SCENE_OBJECT_ID = 2, // sceneObjectIdTex_ (MRT2 R32UI object id)
    MC2_SHARED_SCENE_OBJECT_MV = 3, // per-object motion vectors (RG16F), written by the mech island, composed by the MV pass
    MC2_SHARED_SCENE_COLOR_SLOT_COUNT = 4
};

// Opaque owner of one VK-only image the islands share. Body lives in the .cpp so no
// Vulkan header leaks into GL TUs.
struct Mc2SharedSceneColor;

// Build a VK-only (non-exportable) image on the SHARED engine Vulkan context.
// Color/normal slots use R16G16B16A16_SFLOAT; object-id uses R32_UINT. There is no
// GL texture — the caller allocates its own plain-GL placeholder attachment. The
// returned pointer owns the lifetime. FAIL-SOFT: returns nullptr on ANY failure;
// *outReason (if non-null) points to a short static string. The caller MUST then
// decline the whole shared scene to pure GL.
Mc2SharedSceneColor* mc2SharedSceneColorCreate(Mc2SharedSceneColorSlot slot,
                                               int w, int h,
                                               const char** outReason);

// Destroy the VK image (safe on nullptr) and clear the island registry entry for
// its slot.
void mc2SharedSceneColorDestroy(Mc2SharedSceneColor* c);

#ifdef MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
// Island-facing accessor (only VK island TUs that include volk define the IMPL macro
// before including this header). Returns the VK-only VkImageResource for a slot, or
// nullptr if that slot has no active shared image (declined / resized away). The
// islands co-write LOAD color attachments over the SAME shared images.
struct VkImageResource;
VkImageResource* mc2SharedSceneColorGetResource(Mc2SharedSceneColorSlot slot);
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_SHARED_SCENE_COLOR_H
