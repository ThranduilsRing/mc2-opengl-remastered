#pragma once
// LIGHTING-CPU-CUT: per-frame-slot sync of the shared light feed.
//
// A frame slot mirrors the CPU object-light rows [0..count). Every row write in the
// packet takes a fresh generation, sets the packet revision to it and is journaled
// (mclib/vk_object_lights.cpp). A slot that synced at revision R then needs:
//   - nothing, when the revision is still R and no new rows became visible;
//   - only the journaled rows written after R, plus rows newly inside the window,
//     when the journal still reaches back to R;
//   - otherwise the full generation scan (the pre-cut behaviour).
// Header-only, no Vulkan types, so the doctest drives it directly.
#include "../../mclib/vk_object_lights.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2vk {

struct LightFeedSyncState {
    std::vector<uint64_t> generations;  // generation of each row in this slot's copy
    uint64_t syncedRevision = 0;
    size_t   syncedRows = 0;            // rows [0..syncedRows) match syncedRevision
    bool     valid = false;
    void invalidate() { generations.clear(); syncedRevision = 0; syncedRows = 0; valid = false; }
};

struct LightFeedSyncStats {
    uint64_t fastExits = 0, journalSyncs = 0, fullScans = 0, rowsCopied = 0;
};

// copyRows(firstRow, rowCount) copies rows from the CPU packet into the slot.
template <class CopyRows>
inline void SyncLightFeedRows(LightFeedSyncState& s, std::span<const uint64_t> gens,
                              std::span<const mc2::vkLights::RowWrite> journal, uint64_t journalBase,
                              uint64_t revision, CopyRows&& copyRows, LightFeedSyncStats* stats = nullptr)
{
    const size_t count = gens.size();
    if (s.valid && revision == s.syncedRevision && count <= s.syncedRows) {
        if (stats) ++stats->fastExits;
        return;
    }
    if (s.generations.size() < count) s.generations.resize(count, ~0ull);
    uint64_t copied = 0;
    // Copy the dirty runs inside [first, last).
    auto scan = [&](size_t first, size_t last) {
        for (size_t i = first; i < last;) {
            if (s.generations[i] == gens[i]) { ++i; continue; }
            const size_t run = i;
            do { s.generations[i] = gens[i]; ++i; }
            while (i < last && s.generations[i] != gens[i]);
            copyRows(run, i - run);
            copied += i - run;
        }
    };
    if (s.valid && s.syncedRevision >= journalBase && revision >= s.syncedRevision) {
        // Journal entries are in generation order; take the writes after the last sync.
        auto it = std::partition_point(journal.begin(), journal.end(),
                                       [&](const mc2::vkLights::RowWrite& w) { return w.gen <= s.syncedRevision; });
        const size_t tracked = (std::min)(count, s.syncedRows);
        for (; it != journal.end(); ++it) {
            const size_t row = it->row;
            if (row >= tracked || s.generations[row] == gens[row]) continue;
            s.generations[row] = gens[row];
            copyRows(row, 1);
            ++copied;
        }
        scan(tracked, count);  // rows that entered the window since the last sync
        if (stats) ++stats->journalSyncs;
    } else {
        scan(0, count);
        if (stats) ++stats->fullScans;
    }
    if (stats) stats->rowsCopied += copied;
    s.syncedRevision = revision;
    s.syncedRows = count;
    s.valid = true;
}

}  // namespace mc2vk
