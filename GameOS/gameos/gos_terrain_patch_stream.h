// GameOS/gameos/gos_terrain_patch_stream.h
#pragma once

#include <cstdint>
#include "gameos.hpp"    // for gos_VERTEX, gos_TERRAIN_EXTRA

constexpr uint32_t kPatchStreamRingFrames        = 3;
constexpr uint32_t kPatchStreamMaxBuckets        = 64;
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
