// Counted navigation value packets. Asset packets retain their native format.
#pragma once
#include "save_sections.h"

namespace mc2::save {
// Derived state is deliberately restored by the existing load/search paths:
// pending move requests: MOVE_init/MovePathManager construction and warrior
// load clear them; subsequent orders request fresh paths. PathFindMap worker
// scratch: MOVE_init and MOVE_configurePathFindMaps allocate both contexts.
// MapCell mover bits: clear then place restored movers; tileMineCount: rescan.
// areas_doors/doors_links: reconstruct views over decoded info/link pools;
// areas_cellsCovered: zero on load. Legacy32bitPtr slots remain inert values.
// ownerWID/teamID/open: save resets area defaults; restored objects apply state.
// parent/fromAreaIndex/flags/g/hPrime/fPrime: save clears; calcPath reinitializes.
// pathExistsTable: allocate and clear; offMapAreas/numOffMapAreas: rescan areas.
// openHeap: reserve after load, clear per search; startCell/goalCell/goalArea/
// goalSector/useClosedAreas/moverTeamID: set by each search. closes/opens/
// calcedPathCost: fresh GlobalMap defaults. blank/hover: set per movement level.
// doorBuildList/specialAreas: construction-only scratch, fresh map starts empty.
// gate and mover callbacks: rebound by Mission::loadMovement after map restore.
struct NavigationFormat {
    bool cells = false;
    bool areas = false;
    bool doors = false;
    bool links = false;
};

// The already-validated map dimensions/counts bound decoding, including grids
// larger than Archive's ordinary 65536-element sequence limit.
template<class T, std::size_t N> Bytes encodeNavigationRecord(std::span<T, N> values, std::uint32_t tag) {
    Archive a;
    std::uint32_t version = 1, count = static_cast<std::uint32_t>(values.size());
    if (values.size() > maxFileBytes / sizeof(T)) return {};
    a(tag, version, count);
    for (auto& value : values) a(value);
    return a.good() ? a.bytes() : Bytes{};
}

template<class T, std::size_t N> bool decodeNavigationRecord(std::span<const std::uint8_t> bytes,
                                             std::span<T, N> values, std::uint32_t expected) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0, count = 0;
    a(tag, version, count);
    if (!a.good() || tag != expected || version != 1 || count != values.size() ||
        !a.allocate(count, sizeof(T))) return false;
    std::vector<T> staged(count);
    for (auto& value : staged) a(value);
    if (!a.finish()) return false;
    std::copy(staged.begin(), staged.end(), values.begin());
    return true;
}
}
