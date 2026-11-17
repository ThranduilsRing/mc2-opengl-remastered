#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_set>

// A failed immutable chain is as expensive to rescan as a valid one. Unresolved
// registrations must remain retryable; the owning batcher clears this per map.
class PropLodEvaluation {
public:
    static bool resolved(std::span<const uint32_t> ids) {
        return std::ranges::find(ids, UINT32_MAX) == ids.end();
    }
    bool containsAll(std::span<const uint32_t> base) const {
        return !base.empty() && std::ranges::all_of(base, [this](uint32_t id) { return evaluated_.contains(id); });
    }
    void remember(std::span<const uint32_t> base, bool registrationsResolved) {
        if (registrationsResolved && resolved(base)) evaluated_.insert(base.begin(), base.end());
    }
    void clear() { evaluated_.clear(); }
private:
    std::unordered_set<uint32_t> evaluated_;
};
