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
// Color and extras must agree on per-slot vertex capacity because
// flush()'s glDrawArrays uses ONE `first` argument shared across both
// rings (see static_assert in gos_terrain_patch_stream.cpp). Both rings
// hold 327,680 verts/slot:
//   color  = 327,680 verts × 32 B = 10,485,760 B = 10.0 MB
//   extras = 327,680 verts × 24 B =  7,864,320 B =  7.5 MB
constexpr uint32_t kPatchStreamColorBytesPerSlot  = 327680u * 32u;  // 10.0 MB
constexpr uint32_t kPatchStreamExtrasBytesPerSlot = 327680u * 24u;  //  7.5 MB

struct PatchStreamBucket {
    DWORD    gosHandle;   // resolved gosHandle (tex_resolve already applied)
    uint32_t firstVertex; // slot-relative vertex offset (slotFirstVert added at draw time)
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

    // Emit up to two triangles sharing the same terrain texture handle.
    // tri1Valid/tri2Valid carry the per-triangle pz gate result; a false flag
    // skips that triangle's vertex write without touching the bucket.
    // One bucket lookup when either triangle is valid; zero lookups when both
    // are clipped (early-out before findOrCreateStagingBucket).
    // vColor1/vExtra1 must point to 3 elements each; same for vColor2/vExtra2.
    static void appendQuad(DWORD terrainHandle,
                           const gos_VERTEX*        vColor1,
                           const gos_TERRAIN_EXTRA* vExtra1,
                           bool tri1Valid,
                           const gos_VERTEX*        vColor2,
                           const gos_TERRAIN_EXTRA* vExtra2,
                           bool tri2Valid);

    static bool flush();
    static void beginFrame();

    // Bucket-census instrumentation. Env-gated (MC2_BUCKET_CENSUS=1).
    // Called from txmmgr.cpp Render.TerrainSolid at end of zone, with the
    // count of legacy `masterVertexNodes` that would have drawn this frame
    // under the same eligibility filter as the legacy DRAWSOLID loop.
    // Emits one [BUCKET_CENSUS v1] line per frame plus a 600-frame summary
    // and a final summary at shutdown. No-op if env var unset.
    static void emitCensus(uint32_t legacyEligible);
};
