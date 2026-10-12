// vulkan_sky_island.h -- VK-PARITY-F3: VK owner of the HDRI equirect sky.
// =============================================================================
// WHY. GL draws the HDRI skybox (gosPostProcess::renderHdriSkybox) into the GL
// scene FBO before terrain. Under the VK umbrella the VK terrain island is the
// native first owner and CLEARs shared color0 to a flat sky-ish grey-blue
// (0.55,0.62,0.72); the GL sky draw lands in a discarded buffer, so VK showed a
// flat cool grey-blue sky where GL shows the warm beige HDRI (measured F2/F3).
//
// WHAT. renderHdriSkybox, when this island is enabled, skips its GL draw and
// submits the camera matrices + sun-sync yaw + the GL HDRI texture id here
// (mc2_vk_sky_submit). The terrain-island call site then runs mc2_vk_sky_island_run
// right after the terrain pass: a fullscreen triangle at reverse-Z far (z=0) with a
// GEQUAL depth test, no depth write, color0 only, so it fills exactly the pixels
// terrain left untouched -- the same visible result as GL's sky-first ordering.
// The HDRI is imported copy-once from the GL texture (BC6H bytes or RGBA16F mip
// chain), like the water island does.
//
// GATE: no env of its own; armed exactly when the VK terrain island is the native
// first owner (mc2vkgate::umbrellaOn). Gate off -> GL unchanged.
// =============================================================================
#ifndef MC2_VULKAN_SKY_ISLAND_H
#define MC2_VULKAN_SKY_ISLAND_H

#ifdef MC2_VULKAN_ISLAND
#ifdef __cplusplus
extern "C" {
#endif

int  mc2_vk_sky_island_enabled(void);
// invProj: 16 floats column-major; invViewRot: 9 floats column-major (3x3).
void mc2_vk_sky_submit(const float* invProj16, const float* invViewRot9,
                       float skyYaw, unsigned int hdriGlTex);
// Returns 1 if the sky was recorded + submitted this frame.
int  mc2_vk_sky_island_run(int w, int h);
void mc2_vk_sky_island_destroy(void);

#ifdef __cplusplus
}
#endif
#endif // MC2_VULKAN_ISLAND
#endif
