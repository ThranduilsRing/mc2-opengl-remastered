#pragma once

namespace mc2::pick {

struct Rank {
    int priority = 10;
    float depth = 0.0f;
    long handle = 0;
};

// Equivalent to stable depth/handle sorting followed by strict priority selection.
inline bool precedes(const Rank& candidate, const Rank& best) {
    if (candidate.priority != best.priority) return candidate.priority < best.priority;
    if (candidate.depth != best.depth) return candidate.depth < best.depth;
    return candidate.handle < best.handle;
}

template<class Target, class Clear, class Pick>
void refreshHover(bool guiCovered, Target*& target, Clear clear, Pick pick) {
    clear();
    target = guiCovered ? nullptr : pick();
}

} // namespace mc2::pick
