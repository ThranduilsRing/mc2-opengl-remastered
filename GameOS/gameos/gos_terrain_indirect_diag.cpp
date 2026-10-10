// gos_terrain_indirect: parity printer, 600-frame summary and the recipe-versus-live parity comparison.
// Declared in the class headers; shared names in gos_terrain_indirect_internal.h.
#include "gos_terrain_indirect_internal.h"


// ---------------------------------------------------------------------------
// Parity-printer + 600-frame summary
// ---------------------------------------------------------------------------
namespace {
int       s_parityMismatchesThisFrame = 0;
long long s_paritySummaryFrames       = 0;
long long s_paritySummaryQuads        = 0;
long long s_paritySummaryMismatches   = 0;
}  // namespace

namespace gos_terrain_indirect {

void ParityPrintMismatch(int frame, int quad, const char* layer, int tri,
                         int vert, const char* field,
                         uint32_t legacy, uint32_t fast) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    MC2_DIAGFE(stderr,
            "[TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=%d quad=%d "
            "layer=%s tri=%d vert=%d field=%s legacy=0x%08X fast=0x%08X\n",
            frame, quad, layer ? layer : "?", tri, vert, field ? field : "?",
            legacy, fast);
    fflush(stderr);
}

void ParityFrameTick(int quadsCheckedThisFrame) {
    ++s_paritySummaryFrames;
    s_paritySummaryQuads      += quadsCheckedThisFrame;
    s_paritySummaryMismatches += s_parityMismatchesThisFrame;
    s_parityMismatchesThisFrame = 0;
    if (s_paritySummaryFrames % 600 == 0) {
        // Cost-split columns are appended only when MC2_TERRAIN_COST_SPLIT
        // is set — otherwise the all-zero noise confuses readers.
        const int    csFrames = CostSplit_GetFramesObserved();
        const bool   csOn     = (csFrames > 0);
        const long long csSolidNs   = csOn ? CostSplit_GetSolidNanosTotal()         : 0;
        const long long csDetailNs  = csOn ? CostSplit_GetDetailOverlayNanosTotal() : 0;
        const long long csMineEnqNs = csOn ? CostSplit_GetMineEnqueueNanosTotal()   : 0;
        const long long csMineDrwNs = csOn ? CostSplit_GetMineDrawNanosTotal()      : 0;
        const long long csOverlayNs = csOn ? CostSplit_GetOverlayNanosTotal()       : 0;
        const long long csWvpNs     = csOn ? CostSplit_GetWaterVertProjNanosTotal() : 0;
        const long long csRecipeNs  = csOn ? CostSplit_GetRecipeCacheNanosTotal()   : 0;
        const long long csSetupNs   = csOn ? CostSplit_GetSetupTotalNanosTotal()    : 0;
        const long long csResNs     = csOn ? CostSplit_GetCacheResidentNanosTotal() : 0;
        const long long csSolidPerFrame    = csOn ? csSolidNs   / csFrames : 0;
        const long long csDetailPerFrame   = csOn ? csDetailNs  / csFrames : 0;
        const long long csMineEnqPerFrame  = csOn ? csMineEnqNs / csFrames : 0;
        const long long csMineDrwPerFrame  = csOn ? csMineDrwNs / csFrames : 0;
        const long long csOverlayPerFrame  = csOn ? csOverlayNs / csFrames : 0;
        const long long csWvpPerFrame      = csOn ? csWvpNs     / csFrames : 0;
        const long long csRecipePerFrame   = csOn ? csRecipeNs  / csFrames : 0;
        const long long csSetupPerFrame    = csOn ? csSetupNs   / csFrames : 0;
        const long long csResPerFrame      = csOn ? csResNs     / csFrames : 0;
        if (csOn) {
            MC2_DIAGFE(stderr,
                    "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
                    "quads_checked=%lld total_mismatches=%lld "
                    "legacy_solid_setup_quads=%lld "
                    "indirect_solid_packed_quads=%lld "
                    "legacy_detail_overlay_quads=%lld "
                    "legacy_mine_enqueue_quads=%lld "
                    "legacy_mine_draw_quads=%lld "
                    "indirect_mine_drawn_cells=%lld "
                    "m2c_detail_emit_quads=%lld "
                    "legacy_m2d_overlay_emit_quads=%lld "
                    "indirect_overlay_packed_quads=%lld "
                    "gos_push_overlay_calls=%lld "
                    "solid_branch_ns_per_frame=%lld "
                    "detail_overlay_branch_ns_per_frame=%lld "
                    "mine_enqueue_ns_per_frame=%lld "
                    "mine_draw_ns_per_frame=%lld "
                    "overlay_ns_per_frame=%lld "
                    "water_vert_proj_ns_per_frame=%lld "
                    "recipe_cache_ns_per_frame=%lld "
                    "setup_total_ns_per_frame=%lld "
                    "cache_resident_ns_per_frame=%lld "
                    "shape_c_invisible_quads=%lld "
                    "frames_observed=%d\n",
                    s_paritySummaryFrames,
                    s_paritySummaryQuads,
                    s_paritySummaryMismatches,
                    Counters_GetLegacySolidSetupQuads(),
                    Counters_GetIndirectSolidPackedQuads(),
                    0LL,  // Arc 2: counter retired; field kept for log schema stability (remove in Arc 5)
                    Counters_GetLegacyMineEnqueueQuads(),
                    Counters_GetLegacyMineDrawQuads(),
                    Counters_GetIndirectMineDrawnCells(),
                    Counters_GetM2cDetailEmitQuads(),
                    Counters_GetM2dOverlayEmitQuads(),
                    Counters_GetIndirectOverlayPackedQuads(),
                    Counters_GetGosPushOverlayCalls(),
                    csSolidPerFrame,
                    csDetailPerFrame,
                    csMineEnqPerFrame,
                    csMineDrwPerFrame,
                    csOverlayPerFrame,
                    csWvpPerFrame,
                    csRecipePerFrame,
                    csSetupPerFrame,
                    csResPerFrame,
                    Counters_GetShapeCInvisibleQuads(),
                    csFrames);
        } else {
            MC2_DIAGFE(stderr,
                    "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
                    "quads_checked=%lld total_mismatches=%lld "
                    "legacy_solid_setup_quads=%lld "
                    "indirect_solid_packed_quads=%lld "
                    "legacy_detail_overlay_quads=%lld "
                    "legacy_mine_enqueue_quads=%lld "
                    "legacy_mine_draw_quads=%lld "
                    "indirect_mine_drawn_cells=%lld "
                    "m2c_detail_emit_quads=%lld "
                    "legacy_m2d_overlay_emit_quads=%lld "
                    "indirect_overlay_packed_quads=%lld "
                    "gos_push_overlay_calls=%lld "
                    "shape_c_invisible_quads=%lld\n",
                    s_paritySummaryFrames,
                    s_paritySummaryQuads,
                    s_paritySummaryMismatches,
                    Counters_GetLegacySolidSetupQuads(),
                    Counters_GetIndirectSolidPackedQuads(),
                    0LL,  // Arc 2: counter retired; field kept for log schema stability (remove in Arc 5)
                    Counters_GetLegacyMineEnqueueQuads(),
                    Counters_GetLegacyMineDrawQuads(),
                    Counters_GetIndirectMineDrawnCells(),
                    Counters_GetM2cDetailEmitQuads(),
                    Counters_GetM2dOverlayEmitQuads(),
                    Counters_GetIndirectOverlayPackedQuads(),
                    Counters_GetGosPushOverlayCalls(),
                    Counters_GetShapeCInvisibleQuads());
        }
        fflush(stderr);
    }
}

}  // namespace gos_terrain_indirect

namespace gos_terrain_indirect {

//
// Walks the live quadList, looks up the dense recipe by top-left vertexNum,
// and byte-compares recipe input fields against the legacy-equivalent values
// derived directly from the live quad vertex pointers.
//
// Skip set (mirrors water_ssbo_pattern.md):
//   - null pointer guards on q.vertices[0..3] and ->pVertex
//   - blank-vertex skip: any vertexNum < 0
//   - recipe coverage gate: RecipeForVertexNum returns nullptr → skip
//
// Compared fields: corner positions (wx*/wy*/wz*), corner normals (nx*/ny*/nz*),
// UV extents (minU/minV/maxU/maxV), _wp0 terrainType-pack bits.
// NOT compared: _wp1.._wp3, _np0.._np3 (always 0.f padding),
//               post-projection data (per water_ssbo_pattern.md — sub-ULP drift).
//
int ParityCompareRecipeFrame() {
    if (!IsParityCheckEnabled()) return 0;
    if (!g_recipeReady || g_denseRecipes.empty()) return 0;
    if (!land) return 0;

    const long total            = land->getNumQuads();
    const TerrainQuadPtr quads  = land->getQuadList();
    if (!quads || total <= 0) return 0;

    static long long s_parityFrameIdx = 0;
    const long long frame = ++s_parityFrameIdx;

    const long  mapSide  = Terrain::realVerticesMapSide;
    const long  halfSide = Terrain::halfVerticesMapSide;
    const float wupv     = Terrain::worldUnitsPerVertex;

    int quadsChecked = 0;

    for (long qi = 0; qi < total; ++qi) {
        const TerrainQuad& q = quads[qi];

        if (!q.vertices[0] || !q.vertices[1] || !q.vertices[2] || !q.vertices[3]) continue;
        if (!q.vertices[0]->pVertex || !q.vertices[1]->pVertex ||
            !q.vertices[2]->pVertex || !q.vertices[3]->pVertex) continue;

        if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;

        const int32_t vn0 = (int32_t)q.vertices[0]->vertexNum;

        const TerrainQuadRecipe* rec = RecipeForVertexNum(vn0);
        if (!rec) continue;

        ++quadsChecked;

        const long  mx = vn0 % mapSide;
        const long  my = vn0 / mapSide;
        const float e_wx0 = float(mx     - halfSide) * wupv;
        const float e_wy0 = float(halfSide - my    ) * wupv;
        const float e_wx1 = float(mx + 1 - halfSide) * wupv;
        const float e_wy1 = e_wy0;
        const float e_wx2 = e_wx1;
        const float e_wy2 = float(halfSide - (my + 1)) * wupv;
        const float e_wx3 = e_wx0;
        const float e_wy3 = e_wy2;

        const float e_wz0 = q.vertices[0]->pVertex->elevation;
        const float e_wz1 = q.vertices[1]->pVertex->elevation;
        const float e_wz2 = q.vertices[2]->pVertex->elevation;
        const float e_wz3 = q.vertices[3]->pVertex->elevation;

        const float e_nx0 = q.vertices[0]->pVertex->vertexNormal.x;
        const float e_ny0 = q.vertices[0]->pVertex->vertexNormal.y;
        const float e_nz0 = q.vertices[0]->pVertex->vertexNormal.z;
        const float e_nx1 = q.vertices[1]->pVertex->vertexNormal.x;
        const float e_ny1 = q.vertices[1]->pVertex->vertexNormal.y;
        const float e_nz1 = q.vertices[1]->pVertex->vertexNormal.z;
        const float e_nx2 = q.vertices[2]->pVertex->vertexNormal.x;
        const float e_ny2 = q.vertices[2]->pVertex->vertexNormal.y;
        const float e_nz2 = q.vertices[2]->pVertex->vertexNormal.z;
        const float e_nx3 = q.vertices[3]->pVertex->vertexNormal.x;
        const float e_ny3 = q.vertices[3]->pVertex->vertexNormal.y;
        const float e_nz3 = q.vertices[3]->pVertex->vertexNormal.z;

        // Expected UV extents: mirror buildRecipeSlot exactly — read from the
        // terrain face cache entry (NOT from q.uvData, which is only set by the
        // Shape C hot-path and stays zero for quads that bypass it).
        // This comparison is apples-to-apples: recipe was built from the cache;
        // parity derives the expected value from the same cache.
        float e_minU = 0.5f / TERRAIN_TXM_SIZE;
        float e_maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
        float e_minV = e_minU;
        float e_maxV = e_maxU;
        if (Terrain::terrainTextures2 && Terrain::mapData) {
            const MapData::WorldQuadTerrainCacheEntry* entry =
                Terrain::mapData->getTerrainFaceCacheEntry(my, mx);
            if (entry && entry->isValid()) {
                const bool isCement  = entry->isCement();
                const bool noOverlay = (entry->overlayHandle == 0xffffffffu);
                if (!(noOverlay && isCement)) {
                    e_minU = entry->uvData.minU;
                    e_minV = entry->uvData.minV;
                    e_maxU = entry->uvData.maxU;
                    e_maxV = entry->uvData.maxV;
                }
            }
        }

        const uint32_t e_m0 = terrainTypeToMaterialLocal(q.vertices[0]->pVertex->terrainType);
        const uint32_t e_m1 = terrainTypeToMaterialLocal(q.vertices[1]->pVertex->terrainType);
        const uint32_t e_m2 = terrainTypeToMaterialLocal(q.vertices[2]->pVertex->terrainType);
        const uint32_t e_m3 = terrainTypeToMaterialLocal(q.vertices[3]->pVertex->terrainType);
        const uint32_t e_tpacked = e_m0 | (e_m1 << 8) | (e_m2 << 16) | (e_m3 << 24);

        uint32_t g_tpacked = 0;
        memcpy(&g_tpacked, &rec->_wp0, 4);

        // Helper: bit-cast float to uint32 for exact mismatch comparison
        // (avoids NaN != NaN false positives and keeps hex output informative).
#define FCMP(fname, got_f, exp_f) \
        do { \
            uint32_t _g = 0, _e = 0; \
            float _gf = (got_f), _ef = (exp_f); \
            memcpy(&_g, &_gf, 4); memcpy(&_e, &_ef, 4); \
            if (_g != _e) { \
                ParityPrintMismatch((int)frame, (int)qi, "recipe", 0, 0, fname, _e, _g); \
            } \
        } while(0)

        FCMP("wx0", rec->wx0, e_wx0);
        FCMP("wy0", rec->wy0, e_wy0);
        FCMP("wz0", rec->wz0, e_wz0);
        FCMP("wx1", rec->wx1, e_wx1);
        FCMP("wy1", rec->wy1, e_wy1);
        FCMP("wz1", rec->wz1, e_wz1);
        FCMP("wx2", rec->wx2, e_wx2);
        FCMP("wy2", rec->wy2, e_wy2);
        FCMP("wz2", rec->wz2, e_wz2);
        FCMP("wx3", rec->wx3, e_wx3);
        FCMP("wy3", rec->wy3, e_wy3);
        FCMP("wz3", rec->wz3, e_wz3);

        FCMP("nx0", rec->nx0, e_nx0);
        FCMP("ny0", rec->ny0, e_ny0);
        FCMP("nz0", rec->nz0, e_nz0);
        FCMP("nx1", rec->nx1, e_nx1);
        FCMP("ny1", rec->ny1, e_ny1);
        FCMP("nz1", rec->nz1, e_nz1);
        FCMP("nx2", rec->nx2, e_nx2);
        FCMP("ny2", rec->ny2, e_ny2);
        FCMP("nz2", rec->nz2, e_nz2);
        FCMP("nx3", rec->nx3, e_nx3);
        FCMP("ny3", rec->ny3, e_ny3);
        FCMP("nz3", rec->nz3, e_nz3);

        FCMP("minU", rec->minU, e_minU);
        FCMP("minV", rec->minV, e_minV);
        FCMP("maxU", rec->maxU, e_maxU);
        FCMP("maxV", rec->maxV, e_maxV);

        if (g_tpacked != e_tpacked) {
            ParityPrintMismatch((int)frame, (int)qi, "recipe", 0, 0,
                                "_wp0", e_tpacked, g_tpacked);
        }

#undef FCMP
    }

    return quadsChecked;
}

}  // namespace gos_terrain_indirect  // Stage 2 block
