#pragma once
#include <cstdint>
#include <span>

namespace static_prop {
// Frozen stores contain the whole registry; ordinary stores contain only the live set.
template<class Store>
bool prepareVisibleStore(Store& store, std::span<const uint32_t> live, bool frozen,
                         uint64_t generation, uint64_t& previousSignature)
{
    bool changed = false;
    if (!frozen) {
        uint64_t signature = 1469598103934665603ull;
        for (uint32_t range : live) { signature ^= range; signature *= 1099511628211ull; }
        changed = signature != previousSignature;
        previousSignature = signature;
    }
    const bool dirty = store.persistentStaticGen() != generation || changed;
    if (dirty) store.clearPersistentStatic();
    return dirty;
}
} // namespace static_prop
