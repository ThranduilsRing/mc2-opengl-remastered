// GameAdapters/SkyRenderAdapter.cpp
//
// The only TU that may include both the adapter
// header AND the engine-side gos_postprocess.h.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md (section 10)

#include "SkyRenderAdapter.h"

// This .cpp is the ONLY translation unit permitted to include both
// the adapter header AND the engine-side gosPostProcess header.
#include "../GameOS/gameos/gos_postprocess.h"

namespace GameAdapters {
namespace Sky {

bool isHdriReady()
{
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->isHdriReady();
}

void renderHdri(const float* viewMat, const float* projMat)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->isHdriReady()) {
        return;  // no-op: black sky baseline
    }
    pp->renderHdriSkybox(viewMat, projMat);
}

void renderHdriBasis(const float* camFwd, const float* camRight,
                     const float* camUp, float tHX, float tHY)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->isHdriReady()) {
        return;  // no-op: black sky baseline
    }
    pp->renderHdriSkyboxBasis(camFwd, camRight, camUp, tHX, tHY);
}

void renderHdriInvVP(const float* invVP16)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->isHdriReady()) {
        return;  // no-op: black sky baseline
    }
    pp->renderHdriSkyboxInvVP(invVP16);
}

void setSkyNumber(int skyNumber)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;
    pp->setSkyNumber(skyNumber);
}

}  // namespace Sky
}  // namespace GameAdapters
