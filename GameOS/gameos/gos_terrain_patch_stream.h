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

// One record per quad. Derived from the color ring's per-slot vertex capacity
// (6 expanded verts per quad). At 192 bytes each, the record SSBO is ~10 MB/slot
// vs ~10 MB (color) + ~7.5 MB (extras). Future: move worldPos/norm/uvData to
// a GPU recipe SSBO (one upload per mission) to reach ~104 bytes/record.
constexpr uint32_t kPatchStreamMaxRecordsPerSlot =
    kPatchStreamColorBytesPerSlot / (sizeof(gos_VERTEX) * 6u);
constexpr uint32_t kPatchStreamRecordBytesPerSlot =
    kPatchStreamMaxRecordsPerSlot * 192u; // sizeof(TerrainQuadRecord) — forward ref

// Compact per-quad record for GPU-side vertex reconstruction (M1).
// TCS reads this SSBO and emits 6 gos_terrain.tesc outputs — two triangles
// matching the TOPRIGHT or BOTTOMLEFT diagonal decomposition.
//
// Layout: std430-compatible (all members at 16-byte-aligned vec4 boundaries).
// 192 bytes/record vs 336 bytes for 6 expanded vertices (43% smaller).
//
// Corner index convention (same for both uvMode variants):
//   corner 0 = vertices[0], UV = (minU, minV)
//   corner 1 = vertices[1], UV = (maxU, minV)
//   corner 2 = vertices[2], UV = (maxU, maxV)
//   corner 3 = vertices[3], UV = (minU, maxV)
//
// Triangle decomposition by uvMode bit:
//   TOPRIGHT  (bit0=0): tri1=corners[0,1,2], tri2=corners[0,2,3]
//   BOTTOMLEFT(bit0=1): tri1=corners[0,1,3], tri2=corners[1,2,3]
struct alignas(16) TerrainQuadRecord {
    // Corner world-space positions (vec4 per corner, w=padding for std430)
    float wx0, wy0, wz0, _wp0;
    float wx1, wy1, wz1, _wp1;
    float wx2, wy2, wz2, _wp2;
    float wx3, wy3, wz3, _wp3;
    // Corner world-space normals (vec4 per corner, w=padding)
    float nx0, ny0, nz0, _np0;
    float nx1, ny1, nz1, _np1;
    float nx2, ny2, nz2, _np2;
    float nx3, ny3, nz3, _np3;
    // UV ranges (same across all verts in quad): minU, minV, maxU, maxV
    float minU, minV, maxU, maxV;
    // Per-frame lighting per corner (ARGB packed, same encoding as gos_VERTEX.argb)
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
    // Per-frame fog + material byte per corner (same encoding as gos_VERTEX.frgb)
    uint32_t fogRGB0, fogRGB1, fogRGB2, fogRGB3;
    // Control
    uint32_t terrainHandle; // raw gosHandle — tex_resolve applied at flush consolidation
    uint32_t flags;         // bit 0: uvMode (0=TOPRIGHT, 1=BOTTOMLEFT)
                            // bit 1: pzTri1Valid, bit 2: pzTri2Valid
    uint32_t _ctrl2, _ctrl3; // padding to 16-byte boundary
    // Total: 4*16 + 4*16 + 16 + 16 + 16 + 16 = 192 bytes
};
static_assert(sizeof(TerrainQuadRecord) == 192, "TerrainQuadRecord must be 192 bytes for std430 alignment");

// --- M1d: Recipe SSBO (cached-per-quad) + Thin Record SSBO (per-frame) ---

// Per-quad cached recipe: world-space corner positions, normals, UV extents.
// Uploaded once on first camera-reveal; stable for a given cached quad.
// Invalidated on mission restart (destroy/init clears s_recipeIndex).
// Layout: std430-compatible (all members at 16-byte-aligned vec4 boundaries).
// 9 vec4s = 144 bytes.
struct alignas(16) TerrainQuadRecipe {
    float wx0, wy0, wz0, _wp0;
    float wx1, wy1, wz1, _wp1;
    float wx2, wy2, wz2, _wp2;
    float wx3, wy3, wz3, _wp3;
    float nx0, ny0, nz0, _np0;
    float nx1, ny1, nz1, _np1;
    float nx2, ny2, nz2, _np2;
    float nx3, ny3, nz3, _np3;
    float minU, minV, maxU, maxV;
};
static_assert(sizeof(TerrainQuadRecipe) == 144,
    "TerrainQuadRecipe must be 144 bytes for std430 alignment");

// Per-frame thin record: recipe index + per-frame lighting, fog, handle, flags.
// 3 uvec4s = 48 bytes.
struct alignas(16) TerrainQuadThinRecord {
    uint32_t recipeIdx;     // index into the recipe SSBO (global, no slot offset)
    uint32_t terrainHandle; // raw gosHandle — tex_resolve applied at flush
    uint32_t flags;         // bit 0: uvMode, bit 1: pzTri1Valid, bit 2: pzTri2Valid
    uint32_t _pad0;
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
    uint32_t fogRGB0,   fogRGB1,   fogRGB2,   fogRGB3;
};
static_assert(sizeof(TerrainQuadThinRecord) == 48,
    "TerrainQuadThinRecord must be 48 bytes for std430 alignment");

// Recipe SSBO: single-buffered, shared across all ring slots.
// Sized for the full terrain grid (120×120 grid ≈ 14 K quads) with 4× headroom.
constexpr uint32_t kPatchStreamMaxRecipesTotal         = 65536u;
constexpr uint32_t kPatchStreamRecipeBytes             =
    kPatchStreamMaxRecipesTotal * 144u;  // 9.2 MB

// Thin-record SSBO: triple-buffered alongside the fat-record SSBO.
// Same per-slot quad capacity as the fat-record path; 48 B vs 192 B.
constexpr uint32_t kPatchStreamMaxThinRecordsPerSlot   = kPatchStreamMaxRecordsPerSlot;
constexpr uint32_t kPatchStreamThinRecordBytesPerSlot  =
    kPatchStreamMaxThinRecordsPerSlot * 48u;

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

    // Returns true when the thin-record GPU path is initialized and active.
    // When true, quad.cpp skips buildTerrainExtraTriple and appendQuad.
    static bool isThinRecordsActive();

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

    // Emit one compact quad record for the GPU reconstruction path (M1).
    // No-op unless MC2_PATCHSTREAM_QUAD_RECORDS=1.
    // Call after appendQuad() at the same call site — both paths must agree on
    // which quads are submitted. The record is written directly into the
    // persistent-mapped record SSBO; no intermediate heap allocation.
    static void appendQuadRecord(const TerrainQuadRecord& rec);

    // Parity: expected vertex count from record flags (sum of valid tris × 3).
    // Compared to s_totalVerts in flush() when MC2_PATCHSTREAM_QUAD_RECORDS=1.
    static void addRecordVertParity(uint32_t n); // n = (pzTri1?3:0)+(pzTri2?3:0)

    // Emit one thin quad record (M1d). No-op unless MC2_PATCHSTREAM_THIN_RECORDS=1.
    // recipe encodes the static geometry (positions, normals, UVs).
    // Per-frame fields are passed inline.
    // Call after appendQuadRecord() at the same quad.cpp call site.
    static void appendThinRecord(DWORD terrainHandle,
                                 const TerrainQuadRecipe& recipe,
                                 uint32_t flags,
                                 uint32_t lightRGB0, uint32_t lightRGB1,
                                 uint32_t lightRGB2, uint32_t lightRGB3,
                                 uint32_t fogRGB0,   uint32_t fogRGB1,
                                 uint32_t fogRGB2,   uint32_t fogRGB3);

    // Parity: expected verts from thin records (same semantics as addRecordVertParity).
    static void addThinRecordVertParity(uint32_t n);

    // Bucket-census instrumentation. Env-gated (MC2_BUCKET_CENSUS=1).
    // Called from txmmgr.cpp Render.TerrainSolid at end of zone, with the
    // count of legacy `masterVertexNodes` that would have drawn this frame
    // under the same eligibility filter as the legacy DRAWSOLID loop.
    // Emits one [BUCKET_CENSUS v1] line per frame plus a 600-frame summary
    // and a final summary at shutdown. No-op if env var unset.
    static void emitCensus(uint32_t legacyEligible);
};
