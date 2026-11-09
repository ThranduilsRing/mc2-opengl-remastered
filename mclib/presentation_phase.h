#pragma once
// True only while rendering a completed snapshot. Gameplay state is read-only
// in this phase; legacy appearance methods select their private render copy.
extern thread_local bool simPresentationPass;
namespace mc2::sim {
class PresentationScope {
    bool previous_ = simPresentationPass;
public:
    PresentationScope() { simPresentationPass = true; }
    ~PresentationScope() { simPresentationPass = previous_; }
};
// Typed sim read: the legacy node getters pick the sim pose or the draw shape by simPresentationPass. Appearance::simNode*
// forces the sim half for the call, Appearance::drawNode* forces the draw half, so the phase flag of the caller cannot
// change which store an accessor reads.
class NodeReadPhase {
    bool previous_ = simPresentationPass;
public:
    explicit NodeReadPhase(bool draw) { simPresentationPass = draw; }
    ~NodeReadPhase() { simPresentationPass = previous_; }
};
}
