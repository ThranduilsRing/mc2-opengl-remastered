#pragma once
#include <cstdint>

namespace GosVegetation {

    struct Instance {
        float x, y, z;       // terrain-chunk space: x=east_centered, y=north_centered, z=elevation
        float yaw;            // rotation about Z (elevation) axis (radians)
        float scale;          // uniform scale in WU
        uint32_t atlasFrame;  // 0..7
        float seed;           // per-instance random [0,1] for dither stability
        // blockIdx removed — computed per-vertex in shader from world position
    };
    static_assert(sizeof(Instance) == 28, "GosVegetation::Instance must be 28 bytes");

    // VEG-FLUSH-REASON-1: per-frame stats written by flush().
    // Exposed for smoke contract assertion (VEG-SMOKE-FLOOR-1).
    struct VegFrameStats {
        int         instance_count;  // total instances in SSBO (s_instanceCount)
        int         draw_calls;      // draw calls issued this frame
        const char* flush_reason;    // reason string set in flush() — static literal
    };

    void init();
    void shutdown();
    void setAtlasPath(const char* path);
    // Most instances the placement may produce: the requested cap, the storage-buffer limit and what fits the residency preset + live GPU headroom,
    // so a big map places only what will be resident instead of placing everything and thinning it at upload. mapSpan bounds the region count.
    uint32_t populationCapacity(uint32_t requested, float mapSpan);
    void uploadInstances(const Instance* instances, uint32_t count);

    // Per-block visibility: 1=draw, 0=cull.  Derived from Terrain::s_blockMeta[i].lodLevel.
    // Call once per frame before flush().  blockCount = s_terrainChunkSide^2.
    void setBlockVisibility(const uint32_t* visible, uint32_t blockCount);

    // Draws all uploaded instances.
    // lightDir: world-space light direction (x,y,z,w) - terrainLightDir convention.
    // time: elapsed seconds.
    // camChunkX/Y/Z: camera position in terrain-chunk space (for wind LOD fade).
    // mapHalfWU: worldUnitsMapSide * 0.5 (for blockIdx computation in shader).
    // blockSideWU: worldUnitsBlockSide (2560.0f).
    // chunkSide: s_terrainChunkSide (block grid dimension).
    void flush(float lightDirX, float lightDirY, float lightDirZ, float lightDirW,
               float time,
               float camChunkX, float camChunkY, float camChunkZ,
               float mapHalfWU, float blockSideWU, int chunkSide);

    // VEG-FLUSH-REASON-1: returns stats from the most recent flush() call.
    // Safe to call at any time; returns zeroes / "no_call" before first flush().
    [[nodiscard]] VegFrameStats getFrameStats();

    // VEG-SMOKE-FLOOR-1: emit VEG_SUMMARY line to stderr for smoke log parsing.
    // Called by the engine at mission end / shutdown when MC2_VEGETATION_CARDS=1.
    void emitSummary();

    [[nodiscard]] bool isEnabled();       // checks MC2_VEGETATION_CARDS env var
    [[nodiscard]] uint32_t instanceCount();

} // namespace GosVegetation
