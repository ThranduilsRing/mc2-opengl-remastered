// GameOS/gameos/gos_terrain_mask_dispatch.cpp
//
// Mask-dispatch terrain path: CPU solid/water mask build, SSBO upload and draw entry points.

#include "gos_terrain_mask_dispatch.h"
#include "gos_terrain_indirect.h"
#include "gos_terrain_water_stream.h"
#include "gos_terrain_bridge.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/tex_resolve_table.h"
#include "../../mclib/terrain.h"  // Terrain::waterElevation, Terrain::frameCos
#include "Gates/gate.h"   // typed gate registry

#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GL/glew.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#  pragma intrinsic(_BitScanForward)
static inline int mc2_ctz_u32(uint32_t v) {
    unsigned long idx = 0;
    _BitScanForward(&idx, v);
    return (int)idx;
}
#else
static inline int mc2_ctz_u32(uint32_t v) { return __builtin_ctz(v); }
#endif

// Accessors implemented elsewhere.
extern "C" unsigned int    gos_terrain_indirect_getRecipeSSBO();
extern "C" int             gos_terrain_indirect_getRecipeMapSide();
extern "C" int             gos_terrain_indirect_getRecipeQuadCount();
extern "C" GLuint          gos_terrain_lighting_getOutputSSBO();

using WaterStream::WaterRecipe;

namespace gos_terrain_mask_dispatch {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static bool    s_masterOn  = false;  // MC2_TERRAIN_MASK_DISPATCH != "0" (and set)
static bool    s_initDone  = false;  // Init() succeeded
static bool    s_readyThisFrame = false;

// Worst-case mask size: 120×120 map = 14,400 quads, ceil(14400/32) = 450 uint32s
static constexpr int32_t kMaxMapSide     = 120;
static constexpr int32_t kMaxQuads       = kMaxMapSide * kMaxMapSide;
static constexpr int32_t kMaskWords      = (kMaxQuads + 31) / 32;  // 450

// CPU shadow buffers (reset each frame, rebuilt by BuildAndUploadMasksForFrame)
static std::vector<uint32_t> s_solidMask;
static std::vector<uint32_t> s_waterMask;

// GL SSBO names (allocated once, reused across missions)
static GLuint s_solidMaskSSBO = 0;
static GLuint s_waterMaskSSBO = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool MasterEnabled() {
    // Cached at Init() time; re-read here for the IsMaskDispatchEnabled() path
    // which may be called before Init() (safe: returns false if not init'd).
    return mc2::gate::get<"MC2_TERRAIN_MASK_DISPATCH">();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Init(int32_t mapSide) {
    s_masterOn = MasterEnabled();

    if (!s_masterOn) return true;  // not an error; feature is just off

    if (mapSide > kMaxMapSide) {
        fprintf(stderr,
            "[MASK_DISPATCH v1] event=init_fail reason=oversized_map mapSide=%d max=%d\n",
            (int)mapSide, (int)kMaxMapSide);
        fflush(stderr);
        s_initDone = false;
        return false;
    }

    if (s_initDone) return true;

    // Allocate CPU shadow buffers at worst-case size
    s_solidMask.assign(kMaskWords, 0u);
    s_waterMask.assign(kMaskWords, 0u);

    // Allocate GL SSBOs at worst-case size (never reallocated per mission)
    const GLsizeiptr maskBytes = (GLsizeiptr)(kMaskWords * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_solidMaskSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_solidMaskSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, maskBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_waterMaskSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_waterMaskSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, maskBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    s_initDone = true;
    return true;
}

void Reset() {
    if (s_solidMask.size()) std::fill(s_solidMask.begin(), s_solidMask.end(), 0u);
    if (s_waterMask.size()) std::fill(s_waterMask.begin(), s_waterMask.end(), 0u);
    s_readyThisFrame = false;
}

// ---------------------------------------------------------------------------
// Per-frame predicates
// ---------------------------------------------------------------------------

bool IsMaskDispatchEnabled() {
    return MasterEnabled()
        && gos_terrain_indirect::IsDenseRecipeReady()
        && s_initDone;
}

bool IsMaskDispatchReady() {
    return s_readyThisFrame;
}

void BeginFrame() {
    s_readyThisFrame = false;
}

bool IsFrameMaskSolidArmed() {
    if (!s_readyThisFrame) return false;
    return mc2::gate::get<"MC2_TERRAIN_MASK_DISPATCH_SOLID">();
}

bool IsFrameMaskWaterArmed() {
    if (!s_readyThisFrame) return false;
    return mc2::gate::get<"MC2_TERRAIN_MASK_DISPATCH_WATER">();
}

// ---------------------------------------------------------------------------
// Mask build — fused single pass over quadList
// ---------------------------------------------------------------------------

void BuildAndUploadMasksForFrame(const TerrainQuadPtr quadList, long numQuads) {
    if (!s_initDone || !s_masterOn) return;

    // Clear CPU shadow buffers
    std::fill(s_solidMask.begin(), s_solidMask.end(), 0u);
    std::fill(s_waterMask.begin(), s_waterMask.end(), 0u);

    for (long qi = 0; qi < numQuads; ++qi) {
        const TerrainQuad& q = quadList[qi];

        // (1) Pointer guards (same predicate as the indirect terrain path)
        if (!q.vertices[0] || !q.vertices[1] ||
            !q.vertices[2] || !q.vertices[3]) continue;

        // (2) Map-edge blank-vertex skip (same predicate as the indirect path)
        const int32_t vn0 = q.vertices[0]->vertexNum;
        if (vn0 < 0 ||
            q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 ||
            q.vertices[3]->vertexNum < 0) continue;

        // Bounds check: vn0 must fit in the mask array
        if (vn0 >= kMaxQuads) continue;

        // (3) No CPU pz cull: the GPU mask shader and raster clip reject
        // off-screen quads, so the CPU mask is conservatively inclusive.
        const bool pzVisible = true;

        // --- SOLID mask ---
        // (4) Recipe coverage gate — mirrors :1420
        if (pzVisible) {
            const TerrainQuadRecipe* rec =
                gos_terrain_indirect::RecipeForVertexNum(vn0);
            if (rec) {
                // (5) terrainHandle skip — CRITICAL: mirrors :1425-1428
                const uint32_t th = static_cast<uint32_t>(
                    tex_resolve(static_cast<DWORD>(q.terrainHandle)));
                if (th != 0 && th != 0xffffffffu) {
                    // Bit index = vn0 (corner-0 vertexNum = mapY*W+mapX).
                    // The mask shader must read by vertexNum, NOT quadList slot.
                    s_solidMask[vn0 >> 5] |= (1u << (vn0 & 31));
                }
            }
        }

        // --- Water mask ---
        // Only water-bearing quads with a valid waterHandle
        if (pzVisible) {
            const WaterRecipe* wrec =
                WaterStream::RecipeForVertexNum(vn0);
            if (wrec) {
                if (q.waterHandle != 0xffffffffu) {
                    s_waterMask[vn0 >> 5] |= (1u << (vn0 & 31));
                }
            }
        }
    }

    // Upload both masks
    const GLsizeiptr maskBytes = (GLsizeiptr)(kMaskWords * sizeof(uint32_t));

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_solidMaskSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, maskBytes, s_solidMask.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_waterMaskSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, maskBytes, s_waterMask.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    s_readyThisFrame = true;
}

// ---------------------------------------------------------------------------
// Draw entry points
// ---------------------------------------------------------------------------

bool DrawMaskSolid() {
    if (!s_readyThisFrame) return false;

    // Per-bucket killswitch (allows armed-water-only configurations)
    if (!mc2::gate::get<"MC2_TERRAIN_MASK_DISPATCH_SOLID">()) return false;

    const uint32_t solidSSBO  = s_solidMaskSSBO;
    const uint32_t recipeSSBO = (uint32_t)gos_terrain_indirect_getRecipeSSBO();
    const uint32_t lightSSBO  = (uint32_t)gos_terrain_lighting_getOutputSSBO();
    const int      quadCount  = gos_terrain_indirect_getRecipeQuadCount();
    const int      mapSide    = gos_terrain_indirect_getRecipeMapSide();

    if (solidSSBO == 0 || recipeSSBO == 0 || quadCount <= 0 || mapSide <= 0)
        return false;

    const bool ok = gos_terrain_bridge_drawMaskSolid(
        solidSSBO, recipeSSBO, lightSSBO, quadCount, mapSide);

    return ok;
}

bool DrawMaskWater() {
    if (!s_readyThisFrame) return false;

    // Per-bucket killswitch (allows armed-solid-only configurations)
    if (!mc2::gate::get<"MC2_TERRAIN_MASK_DISPATCH_WATER">()) return false;

    const uint32_t waterMaskSSBO = s_waterMaskSSBO;
    const uint32_t recipeSSBO    = (uint32_t)WaterStream::EnsureRecipeBufferUploaded();
    const uint32_t lightSSBO     = (uint32_t)gos_terrain_lighting_getOutputSSBO();
    const int      recipeCount   = (int)WaterStream::GetRecipeCount();

    if (waterMaskSSBO == 0 || recipeSSBO == 0 || recipeCount <= 0) return false;

    const float wElev = Terrain::waterElevation;
    const float fCos  = Terrain::frameCos;

    const bool ok = gos_terrain_bridge_drawMaskWater(
        waterMaskSSBO, recipeSSBO, lightSSBO, recipeCount, wElev, fCos);

    return ok;
}

uint32_t GetSolidMaskSSBO() {
    return (uint32_t)s_solidMaskSSBO;
}

uint32_t GetWaterMaskSSBO() {
    return (uint32_t)s_waterMaskSSBO;
}

} // namespace gos_terrain_mask_dispatch
