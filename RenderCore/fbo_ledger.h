#pragma once
// Logical pass targets. Native executor observations already carry resource IDs;
// the retired GL name-to-resource ledger is no longer needed.
#include "RenderPassContract.h"
#include "RenderResourceRegistry.h"

namespace RenderCore { namespace framegraph {

// True iff the pass rendered into the WRONG logical target. Unknown on either side is
// skipped (undeclared pass target, or an unregistered/irrelevant FBO) -> no false positive.
inline bool fboMismatch(RenderResourceId declared, RenderResourceId actual) {
    if (declared == RenderResourceId::Unknown) return false;
    if (actual   == RenderResourceId::Unknown) return false;
    return declared != actual;
}

// Per-pass expected logical target. SPARSE + measure-first: only passes whose bound FBO
// at the noteRenderPass seam is CERTAIN (the scene passes render into sceneFBO_ = MainColor).
// PostProcess is intentionally absent (its FBO at the sample seam is timing-uncertain --
// the probe will tell us before we declare it, the same discipline that caught blend).
struct PassFboTarget { RenderPassId id; RenderResourceId target; };
static constexpr PassFboTarget kPassFboTarget[] = {
    { RenderPassId::MechOpaque,       RenderResourceId::MainColor },
    // WATER-SAME-ORDER-VALIDATE-1: water fast path renders into the scene FBO (MainColor).
    { RenderPassId::Water,            RenderResourceId::MainColor },
    { RenderPassId::StaticPropOpaque, RenderResourceId::MainColor },
    { RenderPassId::Terrain,          RenderResourceId::MainColor },
    { RenderPassId::TerrainOverlay,   RenderResourceId::MainColor },
    { RenderPassId::TerrainDecal,     RenderResourceId::MainColor },
    // VFX-FBO-ONLY-VALIDATE-1: particle/VFX flush renders into the scene HDR FBO (MainColor),
    // bound across the whole flush window at gamecam:594 (every frame, incl. empty-particle frames).
    { RenderPassId::VFX,              RenderResourceId::MainColor },
    // UI-SAME-ORDER-VALIDATE-1: HUD/UI batch flush renders into the DEFAULT framebuffer
    // (post-composite). pp->endScene() binds FBO 0 before flushHUDBatch (gameos_graphics:7485-7487);
    // The native UI target is the backbuffer.
    { RenderPassId::UI,               RenderResourceId::Backbuffer },
};
static constexpr int kPassFboTargetCount =
    sizeof(kPassFboTarget) / sizeof(kPassFboTarget[0]);

inline RenderResourceId declaredFboTarget(RenderPassId id) {
    for (auto i : kPassFboTarget)
        if (i.id == id) return i.target;
    return RenderResourceId::Unknown;   // pass declares no FBO target -> guard skips it
}

}} // namespace RenderCore::framegraph
