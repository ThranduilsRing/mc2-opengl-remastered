// Native HDRI sky submission; the terrain pass draws it behind its depth.
#include "gos_postprocess_internal.h"

// MC2_HDRI_SKY_AZ_OFFSET is a manual sun-sync trim in DEGREES; every sky path wants it in radians.
static float hdriAzOffsetRadians()
{
    return static_cast<float>(static_cast<double>(mc2::gate::get<"MC2_HDRI_SKY_AZ_OFFSET">()) * 3.14159265358979323846 / 180.0);
}

void gosPostProcess::renderHdriSkybox(const float* viewMat, const float* projMat)
{
    if (!hdriReady_ || !hdriCpu_ || !viewMat || !projMat) return;
    // Compute the inverse projection. Convert float arrays to mat4 structs.
    // mat4 constructor takes column-major order; projMat is already column-major.
    mat4 projMat4;
    memcpy(&projMat4.elem[0][0], projMat, 16 * sizeof(float));
    mat4 invProj = inverseMat4(projMat4);
    float invProjArray[16];
    memcpy(invProjArray, &invProj.elem[0][0], 16 * sizeof(float));

    // Extract upper 3x3 of column-major viewMat and transpose
    // (transpose-of-rotation = inverse-of-rotation). Translation is
    // intentionally excluded so the sky does not parallax with the camera.
    float invViewRot[9] = {
        viewMat[0], viewMat[4], viewMat[8],
        viewMat[1], viewMat[5], viewMat[9],
        viewMat[2], viewMat[6], viewMat[10]
    };

    if (!mc2_vk_sky_island_enabled()) return;
    float yaw = hdriAzOffsetRadians();
    if (hdriBakedSunValid_) {
        float lx = 0.0f, ly = 0.0f, lz = 0.0f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        yaw += hdriBakedSunAz_ - atan2f(ly, -lx);
    }
    skyYaw_ = yaw;
    mc2_vk_sky_submit(invProjArray, invViewRot, yaw, getHdriGeneration());
}
