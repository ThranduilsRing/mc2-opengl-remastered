// CPU terrain recipe/record layouts shared by native terrain and water producers.
#pragma once
#include <cstdint>
#include <cstddef>

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

// Per-frame thin record: recipe index + per-frame lighting + per-corner clip.
// 96 bytes (Fix B 2026-05-14: per-corner clip-space positions added so the
// thin VS skips terrainMVP projection entirely — eliminates the temporal MVP
// misalignment root cause of the raster-triangle bug at its source).
// LOCKSTEP across four declarations per memory/cpp_glsl_ubo_struct_lockstep.md:
//   - this C++ struct
//   - shaders/gpu_driven_terrain_solid.comp (writer)
//   - shaders/gos_terrain_thin.vert         (VS reader)
//   - shaders/gos_terrain.frag              (frag mirror — declared for std430 stride only, not read)
// The clipPos[16] flat-float form (with alignas(16) on the field) matches
// std430 vec4[4] (16 B per element, contiguous, 16-aligned). glm::vec4 is
// not in scope at this header so we use the flat form + offset asserts.
struct alignas(16) TerrainQuadThinRecord {
    uint32_t recipeIdx;        // index into the recipe SSBO (global, no slot offset)
    uint32_t terrainHandle;    // raw gosHandle — tex_resolve applied at flush
    uint32_t flags;            // bit 0: uvMode, bit 1: pzTri1Valid, bit 2: pzTri2Valid
    uint32_t cementWord;       // cement layer index + valid bit (bit 31)
                               //   (was _pad0; renamed 2026-05-14 to match writer compute shader)
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
    alignas(16) float clipPos[16];  // 4 corners x vec4(x,y,z,w) in clip space
                                    //   ordering: clipPos[c*4 + {0,1,2,3}] = corner c (x,y,z,w)
                                    //   written by writer at compute/CPU time; VS reads via
                                    //   `vec4 clip = clipArr[cornerIdx]` where cornerIdx 0..3.
};
static_assert(sizeof(TerrainQuadThinRecord) == 96,
    "TerrainQuadThinRecord must be 96 bytes for std430 alignment");
static_assert(offsetof(TerrainQuadThinRecord, clipPos) == 32,
    "clipPos must start at offset 32 (after control + lightRGB)");
