// Native Vulkan sky feed: camera matrices, sun-sync yaw and the CPU HDRI load generation.
#ifndef MC2_VULKAN_SKY_ISLAND_H
#define MC2_VULKAN_SKY_ISLAND_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  mc2_vk_sky_island_enabled(void);
// invProj: 16 floats column-major; invViewRot: 9 floats column-major (3x3).
void mc2_vk_sky_submit(const float* invProj16, const float* invViewRot9,
                       float skyYaw, uint64_t hdriGeneration);
// Returns 1 if the sky was recorded + submitted this frame.
int  mc2_vk_sky_island_run(int w, int h);
void mc2_vk_sky_island_destroy(void);

#ifdef __cplusplus
}
#endif
#endif
