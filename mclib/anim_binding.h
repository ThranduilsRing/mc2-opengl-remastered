#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// A new token on load also distinguishes animations recreated at the same address.
inline std::uint64_t nextAnimationBindingId()
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// Owned by one destination shape, never by the shared animation. Only the most
// recently used clip is retained; steady-state ticks do no name comparisons.
class AnimBindingCache
{
    std::uint64_t animation_ = 0;
    std::uint64_t revision_ = 0;
    long nodeCount_ = -1;
    std::vector<long> indices_;
public:
    void invalidate() { animation_ = 0; ++revision_; }
    std::uint64_t revision() const { return revision_; }

    template<class Matches>
    const std::vector<long>& bind(std::uint64_t animation, long tracks, long nodes, Matches matches)
    {
        if (animation_ == animation && nodeCount_ == nodes
            && indices_.size() == static_cast<std::size_t>(tracks))
            return indices_;
        indices_.assign(tracks, -1);
        for (long track = 0; track < tracks; ++track)
            for (long node = 0; node < nodes; ++node)
                if (matches(track, node)) { indices_[track] = node; break; }
        animation_ = animation;
        nodeCount_ = nodes;
        return indices_;
    }
};
