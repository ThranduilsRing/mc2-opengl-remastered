#pragma once
// ---------------------------------------------------------------------------
// GPU-direct terrain arming predicate — the single source of truth shared by
// the game and the Mission Editor.
//
// These are pure, dependency-free functions so the editor↔game agreement they
// encode can be unit-tested without a GL context.
// gos_terrain_indirect.cpp calls them from CollectUniqueNodeIds() and
// ComputePreflight() so the render path and any test exercise identical logic.
//
// Background (the bug these prevent regressing): every terrain quad bakes a
// "node id" into recipe._wp2:
//   * 0            -> no terrain for this quad (map edge / invalid) — skip.
//   * 0xFFFFFFFF   -> colormap sentinel: sample the merged colormap atlas
//                     directly by world position (no per-tile handle LUT entry).
//   * (0, maxTex)  -> a real tile texture handle (cement/overlay quads) that
//                     needs a handle-LUT entry resolved per frame.
// A freshly generated flat editor map is ALL colormap quads (every _wp2 =
// 0xFFFFFFFF) with zero tile-handle quads, so its unique-node-id set is empty.
// The GPU terrain path must still arm for it (the colormap atlas renders it);
// gating only on "non-empty node id set" left such a map black.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace gos_terrain_arm {

// True iff a baked terrain node id (_wp2) is a real tile texture handle, i.e.
// it belongs in the per-frame terrain handle LUT. Excludes 0 (no terrain) and
// the 0xFFFFFFFF colormap sentinel (and anything >= maxTextures).
inline bool IsTileHandle(uint32_t nodeId, uint32_t maxTextures) {
    return nodeId != 0u && nodeId < maxTextures;
}

// The predicate that decides whether the GPU-direct terrain dispatch arms.
// A map uses the GPU path when terrain-solid is enabled AND either:
//   - it has at least one tile-handle quad (cement/overlay) needing the LUT, OR
//   - a colormap atlas exists — colormap quads sample it directly, so the map
//     renders even with an empty handle LUT.
// A legacy non-colormap map has neither signal and falls back to the CPU path
// (unchanged behaviour — no regression).
inline bool ShouldArmGpuTerrain(bool solidEnabled,
                                bool hasTileNodeIds,
                                bool hasColormapAtlas) {
    return solidEnabled && (hasTileNodeIds || hasColormapAtlas);
}

} // namespace gos_terrain_arm
