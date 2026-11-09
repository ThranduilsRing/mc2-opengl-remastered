#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc2 {

// Render-extract residency: terrain objects whose last render visit settled in the static registry, so the
// next extract skips them (no extractRenderView / renderView / markVisibleChecked). Bookkeeping only: the
// caller says which residents are still valid and what dropping one means (the registry clear).
// An epoch key (height epoch, light environment) change drops every resident.
class StaticResidentSet {
    std::vector<int32_t> recipeOf_;   // per object handle; -1 = not resident
    std::vector<long> handles_;       // residents, for the per-frame validation (may hold dropped ones until the next validate)
    std::vector<unsigned char> listed_;   // handle has an entry in handles_
    std::size_t live_ = 0;
    uint64_t epochKey_ = 0;
public:
    void reset(std::size_t handleCount) {
        recipeOf_.assign(handleCount, -1);
        listed_.assign(handleCount, 0);
        live_ = 0;
        handles_.clear();
        epochKey_ = 0;
    }
    bool contains(long handle) const {
        return handle >= 0 && static_cast<std::size_t>(handle) < recipeOf_.size() && recipeOf_[handle] >= 0;
    }
    int32_t recipe(long handle) const { return contains(handle) ? recipeOf_[handle] : -1; }
    std::size_t size() const { return live_; }
    void drop(long handle) {
        if (contains(handle)) { recipeOf_[handle] = -1; --live_; }
    }
    bool add(long handle, int32_t recipe) {
        if (recipe < 0 || handle < 0 || static_cast<std::size_t>(handle) >= recipeOf_.size()) return false;
        if (recipeOf_[handle] < 0) ++live_;
        if (!listed_[handle]) { listed_[handle] = 1; handles_.push_back(handle); }
        recipeOf_[handle] = recipe;
        return true;
    }
    // Keeps a resident only while the epoch is unchanged and valid(handle, recipe) holds; drop(handle, recipe)
    // runs for every other one.
    template <class Valid, class Drop>
    void validate(uint64_t epochKey, Valid&& valid, Drop&& drop) {
        const bool epochChanged = epochKey != epochKey_;
        epochKey_ = epochKey;
        std::size_t kept = 0;
        for (long handle : handles_) {
            const int32_t recipe = recipeOf_[handle];
            if (recipe < 0) { listed_[handle] = 0; continue; }   // dropped since the last pass
            if (!epochChanged && valid(handle, recipe)) { handles_[kept++] = handle; continue; }
            drop(handle, recipe);
            recipeOf_[handle] = -1;
            listed_[handle] = 0;
            --live_;
        }
        handles_.resize(kept);
    }
};

} // namespace mc2
