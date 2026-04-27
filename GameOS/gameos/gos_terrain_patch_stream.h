// GameOS/gameos/gos_terrain_patch_stream.h
#pragma once

#include <cstdint>
#include "gameos.hpp"    // for gos_VERTEX, gos_TERRAIN_EXTRA

constexpr uint32_t kPatchStreamRingFrames        = 3;
// Raw terrainHandle values from quad.cpp callsites are NOT canonicalized —
// each unique colormap tile creates a bucket. Empirical observation on
// mc2_01 standard zoom: ~64+ distinct handles per frame. The earlier
// audit-derived estimate of 5–15 buckets was the post-mcTextureManager
// node count, not the raw callsite count. Cap is sized for headroom over
// realistic worst-case mission load (Wolfman + Magic content); each
// bucket carries ~4K verts of pre-reserved std::vector capacity (see
// init() pre-reserve loop), so total CPU staging RAM at full capacity
// is ~512 * 4K * (32 + 24) bytes = ~115 MB — same total budget as the
// pre-fix 64 × 32K config, just more granular.
constexpr uint32_t kPatchStreamMaxBuckets        = 512;
constexpr uint32_t kPatchStreamColorBytesPerSlot  = 10u * 1024 * 1024;
constexpr uint32_t kPatchStreamExtrasBytesPerSlot = 8u  * 1024 * 1024;

struct PatchStreamBucket {
    DWORD textureIndex;  // terrain colormap handle, resolved at draw time
    uint32_t firstVertex;
    uint32_t vertexCount;
};

class TerrainPatchStream {
public:
    static bool init();
    static void destroy();
    static bool isReady();
    static bool isOverflowed();

    static void appendTriangle(DWORD textureIndex,
                               const gos_VERTEX* vColor,
                               const gos_TERRAIN_EXTRA* vExtra);

    static bool flush();
    static void beginFrame();
};
