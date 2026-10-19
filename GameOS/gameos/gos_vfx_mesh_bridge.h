// CPU mesh-cache ABI for native Vulkan effect geometry.
#pragma once

#include <cstdint>

extern "C" {

// One mesh upload payload — only consumed by the bridge on a cache MISS for
// meshId. On a cache HIT, all pointers/counts are ignored (mesh already in
// the CPU cache). Vertices are MODEL-SPACE Point3D (x,y,z floats, 3 per vertex packed
// contiguously); uvs are 2 floats per vertex; indices are 16-bit, triangle
// list. vertexCount == number of (x,y,z) triples == number of (u,v) pairs.
struct GosVfxMeshUpload {
    uint64_t       meshId;        // stable key (MLRShape* cast to uint64)
    const float*   positions;     // vertexCount * 3 floats (model space)
    const float*   uvs;           // vertexCount * 2 floats (may be null -> 0,0)
    const uint16_t* indices;      // indexCount * uint16 (triangle list)
    uint32_t       vertexCount;
    uint32_t       indexCount;
};

// One draw instance. meshId must match a prior/accompanying upload. The bridge
// composes worldpos = (modelVert * scale) transformed by modelToWorld on the
// CPU; modelToWorld is the legacy shapeToWorld LinearMatrix4D laid
// out row-major as 16 floats (3x4 affine padded to 4x4; row 3 = 0,0,0,1).
struct GosVfxMeshInstance {
    uint64_t meshId;
    float    modelToWorld[16];    // column-major 4x4 (GL native, column-vector)
    float    scale;               // uniform model scale
    float    rgba[4];             // per-instance paint color (0..1)
    uint32_t gosTexHandle;        // GOS texture handle (already resolved by mclib), 0 = untextured white
    int      blendMode;           // 0 = alpha (SRC_ALPHA,ONE_MINUS_SRC_ALPHA), 1 = additive (SRC_ALPHA,ONE)
};

// Per-frame flush. uploads[] supplies geometry for any meshId not yet cached;
// instances[] are drawn in submission order (effect-phase order — no global
// translucency re-sort). No-op when count==0. Vulkan owns the upload ring,
// descriptors and pipelines. Safe to call every frame; uploads are skipped
// on cache hits.
void gos_vfx_mesh_flush(const GosVfxMeshUpload*   uploads,
                        unsigned int              numUploads,
                        const GosVfxMeshInstance* instances,
                        unsigned int              numInstances);

// Runtime witness, called only by MC2_FX_FORCE_SPAWN=9. Draws alpha and additive
// untextured triangles through the same mesh cache and transform as gosFX shapes.
void gos_vfx_mesh_fixture(const float origin[3]);

}  // extern "C"
