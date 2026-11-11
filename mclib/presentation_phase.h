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
}
