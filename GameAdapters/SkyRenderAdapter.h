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

// True when the HDRI source loaded. False when MC2_HDRI_SKY=0, asset missing,
// or init has not yet been called.
bool isHdriReady();

// Queues the native HDRI background. No-op when isHdriReady() is false.
//
// viewMat, projMat: column-major float[16] from the current camera.
// Only the rotational part of viewMat is used — translation is
// intentionally excluded so the sky does not parallax with the camera.
void renderHdri(const float* viewMat, const float* projMat);

// notify the sky renderer of the mission's theSkyNumber
// (read from [TheSky] SkyNumber in the .fit file).  Swaps the loaded HDRI
// texture to the mood-appropriate asset (IblHdriRegistry).
// Call once at mission load, after mission->theSkyNumber is known.
// No-op when HDRI is disabled or skyNumber is out of range (1-21).
void setSkyNumber(int skyNumber);

}  // namespace Sky
}  // namespace GameAdapters
