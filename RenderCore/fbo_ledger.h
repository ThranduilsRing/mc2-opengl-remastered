#pragma once
// FRAME-GRAPH-FBO-LEDGER-1 — map live framebuffer GLuints to LOGICAL render targets.
//
// An executor must own render targets as logical resources (MainColor,
// Backbuffer, ...), not anonymous GLuints. Today every FBO is an anonymous GLuint in
// gos_postprocess and the RenderResourceRegistry glName is never populated. This ledger
// closes that: FBO owners REGISTER their GLuint->RenderResourceId at creation, the
// per-pass FBO guard SAMPLES GL_DRAW_FRAMEBUFFER_BINDING and RESOLVES it back to a logical
// target, and compares against the pass's declared target.
//
// The registry is runtime-populated (GLuints are assigned by glGenFramebuffers, not known
// at compile time), so it is a tiny header-only singleton. The resolve/compare logic and
// the per-pass target table are pure and offline-testable (tests/unit/test_frame_graph.cpp).
// Like the ambient guard, an unregistered/unknown FBO is SKIPPED (no false positive), and
// the guard ships measure-first: declare targets only where the bound FBO at the sample
// seam is certain, expand as the probe proves more.
#include "RenderPassContract.h"
#include "RenderResourceRegistry.h"

namespace RenderCore { namespace framegraph {

class FboLedger {
    static constexpr int kMax = 16;
    struct Entry { unsigned glName; RenderResourceId target; };
    Entry e_[kMax];
    int   n_ = 0;
public:
    void reset() { n_ = 0; }                       // tests
    void registerFbo(unsigned glName, RenderResourceId target) {
        for (int i = 0; i < n_; ++i)
            if (e_[i].glName == glName) { e_[i].target = target; return; }
        if (n_ < kMax) e_[n_++] = { glName, target };
    }
    RenderResourceId resolve(unsigned glName) const {
        if (glName == 0u) return RenderResourceId::Backbuffer;   // default framebuffer
        for (int i = 0; i < n_; ++i)
            if (e_[i].glName == glName) return e_[i].target;
        return RenderResourceId::Unknown;                        // unregistered -> skipped
    }
};

// One process-wide ledger (inline -> single instance across TUs).
inline FboLedger& fboLedger() { static FboLedger g; return g; }

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
    // fboLedger().resolve(0) == Backbuffer. First ledger row using Backbuffer.
    { RenderPassId::UI,               RenderResourceId::Backbuffer },
};
static constexpr int kPassFboTargetCount =
    sizeof(kPassFboTarget) / sizeof(kPassFboTarget[0]);

inline RenderResourceId declaredFboTarget(RenderPassId id) {
    for (int i = 0; i < kPassFboTargetCount; ++i)
        if (kPassFboTarget[i].id == id) return kPassFboTarget[i].target;
    return RenderResourceId::Unknown;   // pass declares no FBO target -> guard skips it
}

}} // namespace RenderCore::framegraph
