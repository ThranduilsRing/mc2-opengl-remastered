// GameAdapters/SkyRenderAdapter.h
//
// Sky rendering firewall bridge.
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md (section 10)
//
// Include discipline (load-bearing):
//   - Header MUST NOT include any engine headers (gos_postprocess.h, GL, RenderWorld).
//   - Header MUST NOT include any game-side headers.
//   - Header is pure interface only — zero #include directives.
//   - The .cpp includes gos_postprocess.h (the only TU that may).
//   - v1 is route-only — no RenderWorld object handle per render contract.

#pragma once

namespace GameAdapters {
namespace Sky {

// True when the HDRI texture loaded successfully and the shader
// compiled. False when MC2_HDRI_SKY=0, asset missing, shader broken,
// or init has not yet been called.
bool isHdriReady();

// Renders the HDRI background as a fullscreen pass into the currently
// bound scene FBO. No-op when isHdriReady() is false.
//
// viewMat, projMat: column-major float[16] from the current camera.
// Only the rotational part of viewMat is used — translation is
// intentionally excluded so the sky does not parallax with the camera.
void renderHdri(const float* viewMat, const float* projMat);

// HDRI-SKY frame fix (MC2_HDRI_SKY_FRAME_FIX ON path): renders the HDRI
// background by reconstructing the world ray directly from the camera's
// WORLD-space basis (raw MC2 frame: x=east, y=north, z=elevation) instead of
// inverting view+proj. No-op when isHdriReady() is false.
//
// camFwd/camRight/camUp: column-vec float[3] world-space camera basis.
// tHX/tHY: tan(halfFOV) horizontal / vertical.
void renderHdriBasis(const float* camFwd, const float* camRight,
                     const float* camUp, float tHX, float tHY);

// HDRI-SKY frame fix (MC2_HDRI_SKY_FRAME_FIX ON path, one-proven-matrix
// approach): renders the HDRI background by UNPROJECTING NDC through the
// inverse of the EXACT matrix the GPU rasterizes terrain with
// (worldToClipGL = kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL).
// Its inverse yields a ray in the raw MC2 world frame (x=east, y=north,
// z=elevation, Z-up) with NO frame/FOV/handedness guessing. No-op when
// isHdriReady() is false.
//
// invVP16: column-major float[16] = inverse(worldToClipGL), uploaded verbatim
// (same convention as renderHdri's invProj) so the shader does
// invWorldToClipGL * vec4(ndc, depth, 1.0).
void renderHdriInvVP(const float* invVP16);

// notify the sky renderer of the mission's theSkyNumber
// (read from [TheSky] SkyNumber in the .fit file).  Swaps the loaded HDRI
// texture to the mood-appropriate asset (IblHdriRegistry).
// Call once at mission load, after mission->theSkyNumber is known.
// No-op when HDRI is disabled or skyNumber is out of range (1-21).
void setSkyNumber(int skyNumber);

}  // namespace Sky
}  // namespace GameAdapters
