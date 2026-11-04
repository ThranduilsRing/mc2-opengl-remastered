// Native mine bake and cement overlay producer. Texture data stays in the shared GOS cache.
#include <vector>
#include "gos_terrain_indirect_internal.h"
#include "../../mclib/texture_assets.h"
#include "mc2fmt.h"
#include "texture_decode.h"
#include <mdspan>
#include "../../GameAdapters/vegetation_hardsurface.h"
#include "terrain_field_store.h"

namespace {
struct MineVert { float pos[3]; float uv[2]; uint32_t layer; };
std::vector<MineVert> g_mineVerts;
int g_mineVertCount = 0;
bool g_mineVBODirty = true;
bool g_mineTextureArrayReady = false;
int fixtureRow = -1, fixtureCol = -1;
unsigned long fixturePrevious[2]{};
}
namespace gos_terrain_indirect {
void MarkMineDirty() { g_mineVBODirty = true; }
void ResetMineStaticVBO() { g_mineVerts.clear(); g_mineVertCount = 0; g_mineVBODirty = true; fixtureRow = fixtureCol = -1; }
void ResetMineTextureArray() { g_mineTextureArrayReady = false; }
void BuildMineTextureArray() {
    g_mineTextureArrayReady = TerrainQuad::mineTextureHandle != 0xffffffffu &&
                              TerrainQuad::blownTextureHandle != 0xffffffffu;
}
void BuildMineStaticVBO() {
    if (!GameMap || !land) {
        g_mineVertCount = 0;
        g_mineVerts.clear();
        return;
    }

    // Empirically tiny in stock content — start with 64 cell capacity.
    std::vector<MineVert> verts;
    verts.reserve(64 * 6);

    const int W = static_cast<int>(GameMap->width);
    const int H = static_cast<int>(GameMap->height);
    const int TW = W / MAPCELL_DIM;
    const int TH = H / MAPCELL_DIM;
    const float halfCell = Terrain::worldUnitsPerCell * 0.5f;

    for (int tR = 0; tR < TH; ++tR) {
        for (int tC = 0; tC < TW; ++tC) {
            // tileHasMines short-circuits ~97% of tiles per recon-5.
            if (!GameMap->tileHasMines(tR, tC)) continue;
            for (int cR = 0; cR < MAPCELL_DIM; ++cR) {
                for (int cC = 0; cC < MAPCELL_DIM; ++cC) {
                    const int row = tR * MAPCELL_DIM + cR;
                    const int col = tC * MAPCELL_DIM + cC;
                    if (!GameMap->inBounds(row, col)) continue;
                    const unsigned long mine = GameMap->getMine(row, col);
                    if (mine == 0) continue;
                    const uint32_t layer = (mine == 2) ? 1u : 0u;

                    glm::vec3 center;
                    land->tileCellToWorld(tR, tC, cR, cC, center);

                    // Four corners around cell center. tileCellToWorld returns
                    // cell center XY (with half-cell offset baked in per
                    // terrain.h:416-417); we expand to the 4 cell corners.
                    glm::vec3 c00 = center, c10 = center;
                    glm::vec3 c11 = center, c01 = center;
                    c00.x -= halfCell; c00.y += halfCell;  // top-left
                    c10.x += halfCell; c10.y += halfCell;  // top-right
                    c11.x += halfCell; c11.y -= halfCell;  // bottom-right
                    c01.x -= halfCell; c01.y -= halfCell;  // bottom-left
                    c00.z = land->getTerrainElevation(c00);
                    c10.z = land->getTerrainElevation(c10);
                    c11.z = land->getTerrainElevation(c11);
                    c01.z = land->getTerrainElevation(c01);

                    auto pushVert = [&](const glm::vec3& p, float u, float v) {
                        MineVert mv;
                        mv.pos[0] = p.x; mv.pos[1] = p.y; mv.pos[2] = p.z;
                        mv.uv[0]  = u;   mv.uv[1]  = v;
                        mv.layer  = layer;
                        verts.push_back(mv);
                    };
                    pushVert(c00, 0.0f, 0.0f);
                    pushVert(c10, 1.0f, 0.0f);
                    pushVert(c11, 1.0f, 1.0f);
                    pushVert(c00, 0.0f, 0.0f);
                    pushVert(c11, 1.0f, 1.0f);
                    pushVert(c01, 0.0f, 1.0f);
                }
            }
        }
    }

    g_mineVertCount = static_cast<int>(verts.size());
    g_mineVerts = std::move(verts);


}


void RebuildMineStaticVBOIfDirty() {
    BuildMineTextureArray();
    if (!g_mineVBODirty) return;
    BuildMineStaticVBO();
    g_mineVBODirty = false;
}
unsigned int GetMineStaticVBO_GL() { return 0; }
int GetMineVertCount() { return g_mineVertCount; }
unsigned int GetMineTextureArrayGL() { return 0; }
bool IsMineTextureArrayReady() { return g_mineTextureArrayReady; }
bool IsFrameMineArmed() { return IsMineEnabled(); }
bool DrawMineStatic() {
    // The existing FX fixture also covers both mine states through the real dirty bake.
    if (mc2::gate::is<"MC2_FX_FORCE_SPAWN", "9">() && eye && land && GameMap) {
        int row = 0, col = 0;
        land->worldToCell(eye->getPosition(), row, col);
        if ((row != fixtureRow || col != fixtureCol) && GameMap->inBounds(row, col + 1)) {
            if (fixtureRow >= 0 && GameMap->inBounds(fixtureRow, fixtureCol + 1)) {
                GameMap->setMine(fixtureRow, fixtureCol, fixturePrevious[0]);
                GameMap->setMine(fixtureRow, fixtureCol + 1, fixturePrevious[1]);
            }
            fixtureRow = row; fixtureCol = col;
            fixturePrevious[0] = GameMap->getMine(row, col);
            fixturePrevious[1] = GameMap->getMine(row, col + 1);
            GameMap->setMine(row, col, 1);
            GameMap->setMine(row, col + 1, 2);
        }
    }
    RebuildMineStaticVBOIfDirty();
    if (g_mineVerts.empty()) return true;
#ifdef MC2_VULKAN_ISLAND
    if (!mc2_vk_overlay_enabled() || !g_mineTextureArrayReady) return false;
    struct Vertex { float pos[3]; float uv[2]; float fog; uint32_t argb; };
    static_assert(sizeof(Vertex) == 28);
    std::vector<Vertex> vertices;
    std::vector<Mc2VkOverlayDraw> draws;
    for (const auto& mine : g_mineVerts) {
        const auto slot = mine.layer ? TerrainQuad::blownTextureHandle : TerrainQuad::mineTextureHandle;
        const uint32_t handle = tex_resolve(slot);
        if (handle == 0 || handle == 0xffffffffu) return false;
        if (draws.empty() || draws.back().texHandle != handle)
            draws.push_back({handle, static_cast<uint32_t>(vertices.size()), 0});
        ++draws.back().vertCount;
        vertices.push_back({{mine.pos[0], mine.pos[1], mine.pos[2]}, {mine.uv[0], mine.uv[1]}, 1, 0xffffffffu});
    }
    // Dynamic stream keeps the cement bake's independent static-buffer lifetime intact.
    mc2_vk_overlay_submit(2u, vertices.data(), static_cast<uint32_t>(vertices.size()),
                          draws.data(), static_cast<uint32_t>(draws.size()), 0);
    Counters_AddIndirectMineDrawnCells(g_mineVertCount / 6);
    return true;
#else
    return false;
#endif
}
}
void gos_terrain_indirect_MarkMineDirty() { gos_terrain_indirect::MarkMineDirty(); }

//
// STRUCTURAL MIRROR of the PR2c mine static-bake (BuildMineStaticVBO /
// RebuildMineStaticVBOIfDirty / MarkMineDirty / ResetMineStaticVBO /
// IsFrameMineArmed / DrawMineStatic, ~line 3119-3398 above). Same lifetime
// discipline: one GL_STATIC_DRAW buffer kept across missions, CPU state reset
// on mission load + destroy, lazy rebuild-if-dirty on first armed draw (R7
// timing-trap mitigation: do NOT eager-build at primeMissionTerrainCache —
// the overlay tex handles lazy-load in TerrainQuad::setupTextures during the
// first paint cycle, before Render.TerrainOverlaysStatic fires).
//
// What it bakes: the per-quad M2d cement-overlay producer (mclib/quad.cpp
// `if (useOverlayTexture && overlayHandle != 0xffffffff)`). Decal inputs are
// map-stable — corner world XYZ, overlayHandle, uvMode are all derivable from
// the map-immutable Shape-C terrain face cache (MapData::buildTerrainFaceCache)
// + MapData::blocks. The bake reproduces M2d's 4-corner WorldOverlayVert build
// + per-uvMode tri emit UNCONDITIONALLY (no pzTri camera cull — plan's pz-cull
// policy; cement is sparse, GPU clips, mirrors DrawMineStatic's unconditional
// static draw). Vertex argb/fog are forced to 0xffffffff / 1.0: shaders/
// terrain_overlay.frag never reads the Color/FogValue varyings (explicit
// comment at terrain_overlay.frag:48 — it recomputes lighting/fog from WorldPos
// + live uniforms), so a geometry-only bake is lighting-identical to M2d, the
// same way MineVert is pos/uv/layer-only.
// ---------------------------------------------------------------------------

namespace {

// WorldOverlayVert layout MUST byte-match GameOS/include/gameos.hpp's struct
// (28-byte stride) — the bridge VAO (drawDecalStaticBatch / makeOverlayVAO)
// reads attribs 0..3 at offsets 0/12/20/24.
struct DecalVert {
    float        wx, wy, wz;   // offset  0  — MC2 world (x=east, y=north, z=elev)
    float        u, v;         // offset 12
    float        fog;          // offset 20  — forced 1.0 (frag discards it)
    uint32_t     argb;         // offset 24  — forced 0xffffffff (frag discards it)
};

// Per-overlayTexId draw range (mirror gameos_graphics.cpp OverlayBatchEntry_
// and gos_terrain_indirect's mine 6-verts-per-cell grouping idea).
struct DecalDrawRange {
    uint32_t texHandle;
    uint32_t firstVert;
    uint32_t vertCount;
};

int                          g_decalVertCount     = 0;
bool                         g_decalVBODirty      = true;  // first build triggers via Rebuild on first armed draw
std::vector<DecalDrawRange>  g_decalDrawRanges;
#ifdef MC2_VULKAN_ISLAND
// VK-PARITY-OVERLAY-1: CPU copy of the static bake (the GL VBO is write-only here) so the
// VK overlay owner can draw it into the shared GBuffer. Version bumps per rebuild.
std::vector<DecalVert>       g_decalVertsCpu;
uint64_t                     g_decalBakeVersion   = 0;
bool                         g_decalLoadPending   = false;
#endif

// Mirrors quad.cpp's OVERLAY_ELEV_OFFSET (0.15f) + the fixed overlay UV
// constants (oldminU/oldmaxU/oldminV/oldmaxV at quad.cpp:2054-2057). The M2d
// overlay path uses these CONSTANT UVs (not uvData), so they are fully
// map-stable.
constexpr float kOverlayElevOffset = 0.15f;
constexpr float kOldMinU = 0.0078125f;
constexpr float kOldMaxU = 0.9921875f;
constexpr float kOldMinV = 0.0078125f;
constexpr float kOldMaxV = 0.9921875f;

}  // namespace

namespace gos_terrain_indirect {

// Public forwarder: calls the anonymous-namespace BuildCementCatalogAtlas() from
// editor code (e.g. EditorData::refreshTerrainAfterEdit) that needs to rebuild the
// cement atlas after overlay painting creates new texture slots not present at
// mission-load atlas-build time. Editor uses only; game path goes through BuildDenseRecipe.
void RebuildCementAtlas() {
    BuildCementCatalogAtlas();
}

// Deferred cement-atlas rebuild (mirror MarkDecalDirty/RebuildDecalStaticVBOIfDirty
// debounce discipline). RebuildCementAtlas() does a glGetTexImage readback, which
// breaks in-flight GL draw state if called MID-FRAME (the editor used to call it
// straight from refreshTerrainAfterEdit and that reset normal/detail tiling
// map-wide). So instead a cement-affecting brush stroke MARKS the atlas dirty here,
// and RebuildCementAtlasIfDirty() fires the actual rebuild at FRAME-START — before
// any terrain/GL draws — where the readback is safe. BuildCementCatalogAtlas()
// itself save/restores GL_ACTIVE_TEXTURE + GL_TEXTURE_BINDING_2D + pack/unpack
// alignment (gos_terrain_indirect.cpp:1231-1273) and only touches GL_TEXTURE0, so
// the rebuild does not intrinsically disturb normal/detail bindings.
static bool g_cementAtlasDirty = false;

void MarkCementAtlasDirty() {
    g_cementAtlasDirty = true;
}

void RebuildCementAtlasIfDirty() {
    if (!g_cementAtlasDirty) return;
    g_cementAtlasDirty = false;
    RebuildCementAtlas();
}

// Mirror MarkMineDirty — idempotent; multiple cement mutations between paints
// debounce to one rebuild via the dirty flag.
void MarkDecalDirty() {
    InvalidateHardSurfaceField();
    g_decalVBODirty = true;
}

// Clear the mission-owned CPU decal bake; the native consumer owns its GPU copy.
void ResetDecalStaticVBO() {
    InvalidateHardSurfaceField();
    g_decalVertCount = 0;
    g_decalVBODirty  = true;
    g_decalDrawRanges.clear();
#ifdef MC2_VULKAN_ISLAND
    g_decalLoadPending = false;
    g_decalVertsCpu.clear();
#endif
    if (IsTraceEnabled()) {
        mc2::fmt::print_out("[TERRAIN_OVERLAY v1] event=decal_cpu_reset\n");
        fflush(stdout);
    }
}

// Mirror BuildMineStaticVBO. Iterates the map-immutable Shape-C terrain face
// cache exactly as MapData::buildTerrainFaceCache does (mapdata.cpp:262-309):
// same tile range, same 4-corner block layout, same worldQuadUVMode formula.
bool CementWordSuppressesDecal(const MapData::WorldQuadTerrainCacheEntry* e,
                                Overlays overlayType, long tileR, long tileC, bool overlayMetadata = true) {
    const long mapSide = Terrain::realVerticesMapSide;
    const auto blocks = Terrain::mapData->getBlocks();
            if (e->isCement() && e->isAlpha() && overlayType == INVALID_OVERLAY) {
                // MISSING-RUNWAY-1 (mc2_10 field report 2026-07-18): the blanket
                // skip here assumed every isCement&&isAlpha&&no-&Overlays tile is
                // a true CONCRETE transition the frag cement-word edge system
                // redraws. WRONG on mc2_10: its roads + the airbase runway are
                // authored as cement-ALPHA transition tile art over SNOW terrain
                // (type 68) with NO &Overlays id — no corner vertex has a cement
                // terrain type, so PopulateRecipeCementWords bakes NO word
                // (maskId needs >=1 cement corner) and the frag draws nothing;
                // skipping the legacy decal too left the runway/roads INVISIBLE
                // (tile census: e.g. tex=771 n=93, tex=774 n=80, all ov=-1).
                // Skip ONLY when the cement-word system actually owns the tile —
                // same corner predicate PopulateRecipeCementWords uses (>=1
                // corner terrainType mapping to a cement catalog layer AND a
                // valid mask). Otherwise fall through and bake the decal (the
                // tile texture IS the road/runway art). If the cement layer map
                // is not ready yet, keep the old skip (conservative; the words
                // bake before the first decal build in the mission-load order).
                // Killswitch MC2_MISSING_RUNWAY_FIX=0 -> old blanket skip.
                static const bool s_runwayFix = true;
                bool fragOwnsTile = true;
                if (s_runwayFix && g_cementLayerMapReady && blocks) {
                    auto cemCorner = [&](long r, long c2) -> bool {
                        const size_t vi = static_cast<size_t>(r) * static_cast<size_t>(mapSide) + static_cast<size_t>(c2);
                        const DWORD  vt = blocks[vi].terrainType;
                        return vt < static_cast<DWORD>(MC_MAX_TERRAIN_TXMS)
                               && g_cementLayerIndexBySlot[vt] != 0xFFFFu;
                    };
                    const bool c0 = cemCorner(tileR,     tileC);
                    const bool c1 = cemCorner(tileR,     tileC + 1);
                    const bool c2 = cemCorner(tileR + 1, tileC + 1);
                    const bool c3 = cemCorner(tileR + 1, tileC);
                    fragOwnsTile = (c0 || c1 || c2 || c3)
                                   && deriveMaskIdFromCorners(c0, c1, c2, c3) >= 0;
                    // PAD-EDGE-ROAD-1: road-adjacent pad-edge transition tiles
                    // revert to the stock decal (PopulateRecipeCementWords
                    // bakes NO cement word for them — complementary predicate).
                    if (fragOwnsTile && PadEdgeRoadFixEnabled()
                        && TileRoadAdjacent8(tileR, tileC, mapSide, blocks, overlayMetadata))
                        fragOwnsTile = false;
                }
                return fragOwnsTile;
            }
    return false;
}

void BuildDecalStaticVBOFromInputs(bool overlayMetadata) {
    g_decalVertCount = 0;
    g_decalDrawRanges.clear();

    if (!Terrain::mapData) return;
    PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    if (!blocks) return;

    const long mapSide  = Terrain::realVerticesMapSide;
    const long half     = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;
    const long cacheW   = mapSide - 1;   // == worldQuadCacheWidth() (mapdata.cpp:107)
    if (cacheW <= 0) return;

    std::vector<DecalVert> verts;
    verts.reserve(64 * 6);
    // The second plane shares the cement atlas and buffer: valid | adopt | art layer.
    // Keep the first plane intact so transparent road texels retain their cement base.
    auto* fields = gos_TerrainFieldStore();
    const size_t plane = size_t(mapSide) * size_t(mapSide);
    std::vector<uint32_t> surfaceWords(2 * plane, 0u);
    if (fields && fields->cementWords().size() >= plane)
        std::copy_n(fields->cementWords().begin(), plane, surfaceWords.begin());
    int cookedRoads = 0;

    // MISSING-RUNWAY-1 tile census (MC2_TERRAIN_TILE_CENSUS=1, silent by
    // default): one summary line per distinct (terrainType, texTileIdx,
    // cement, alpha, overlayId) combo with count + world bbox. Answers "what
    // IS the runway in this map's data" without knowing its coords up front.
    {
        static const bool s_tileCensus = false;
        if (s_tileCensus) {
            struct Agg { long n; float minX, maxX, minY, maxY; };
            std::map<unsigned long long, Agg> agg;
            for (long tR = 0; tR < cacheW; ++tR) {
                for (long tC = 0; tC < cacheW; ++tC) {
                    const MapData::WorldQuadTerrainCacheEntry* ce =
                        Terrain::mapData->getTerrainFaceCacheEntry(tR, tC);
                    const long terrType = Terrain::mapData->getTerrain(tR, tC);
                    const long idx = tC + tR * mapSide;
                    const DWORD texData = blocks[idx].textureData;
                    Overlays ovT = INVALID_OVERLAY; DWORD ovO = 0;
                    Terrain::mapData->getOverlay(tR, tC, ovT, ovO);
                    const int cem = (ce && ce->isCement()) ? 1 : 0;
                    const int alp = (ce && ce->isAlpha())  ? 1 : 0;
                    unsigned long long key =
                        (static_cast<unsigned long long>(static_cast<unsigned>(terrType)) << 40) |
                        (static_cast<unsigned long long>((texData & 0xffffu)) << 8) |
                        (static_cast<unsigned long long>((static_cast<int>(ovT) + 1)) << 2) |
                        (static_cast<unsigned>(cem) << 1) | static_cast<unsigned>(alp);
                    const float wx = float(tC - half) * wupv;
                    const float wy = float(half - tR) * wupv;
                    auto it = agg.find(key);
                    if (it == agg.end())
                        agg[key] = { 1, wx, wx, wy, wy };
                    else {
                        Agg& a = it->second; ++a.n;
                        if (wx < a.minX) a.minX = wx; if (wx > a.maxX) a.maxX = wx;
                        if (wy < a.minY) a.minY = wy; if (wy > a.maxY) a.maxY = wy;
                    }
                }
            }
            for (auto& kv : agg) {
                const unsigned long long k = kv.first;
                mc2::fmt::print_out("[TILE_CENSUS v1] terr={:d} tex={:d} ov={:d} cement={:d} alpha={:d} n={:d} bbox=({:.0f},{:.0f})-({:.0f},{:.0f})\n", mc2::fmt::as_i32(static_cast<long>((k >> 40))), mc2::fmt::as_u32(static_cast<unsigned long>(((k >> 8) & 0xffffu))), mc2::fmt::as_i32(static_cast<int>(((k >> 2) & 0x3f)) - 1), mc2::fmt::as_u64((k >> 1) & 1ull), mc2::fmt::as_u64(k & 1ull), mc2::fmt::as_i32(kv.second.n), kv.second.minX, kv.second.minY, kv.second.maxX, kv.second.maxY);
            }
            fflush(stdout);
        }
    }

    // First collect (texId -> tri verts) so draws can be grouped per texId
    // (mirror OverlayBatch_'s verts + draws split / pushToOverlayBatch_).
    // overlay tile count is empirically tiny — a flat scan + run-grouping is
    // fine (cement sparse, matches DrawMineStatic's tileHasMines short-circuit
    // in spirit).
    struct PendingTri { uint32_t texId; DecalVert v[3]; };
    std::vector<PendingTri> pending;
    int cementSuppressed = 0, sidecarSuppressed = 0, unresolved = 0;

    for (long tileR = 0; tileR < cacheW; ++tileR) {
        for (long tileC = 0; tileC < cacheW; ++tileC) {
            const MapData::WorldQuadTerrainCacheEntry* e =
                Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC);
            if (!e || !e->isValid()) continue;
            // M2d gate equivalent: useOverlayTexture && overlayHandle !=
            // 0xffffffff. buildTerrainFaceCache only sets overlayHandle for
            // the alpha-cement case (mapdata.cpp:296-302); pure-cement and
            // non-cement leave it 0xffffffff. Use the handle directly so this
            // matches the live M2d predicate byte-for-byte.
            const DWORD overlayHandle = e->overlayHandle;
            // MISSING-RUNWAY-1 census probe (MC2_TERRAIN_INDIRECT_TRACE, silent
            // by default): dump EVERY tile that carries an &Overlays id with its
            // face-cache classification, so tiles the bake DROPS (overlayHandle
            // 0xffffffff: opaque runway/road centers = isCement&&!isAlpha case 3,
            // or non-cement colormap case 1) are visible with world coords.
            if (IsTraceEnabled()) {
                Overlays ovT = INVALID_OVERLAY; DWORD ovO = 0;
                Terrain::mapData->getOverlay(tileR, tileC, ovT, ovO);
                if (ovT != INVALID_OVERLAY) {
                    mc2::fmt::print_out("[TERRAIN_OVERLAY v1] event=overlay_census tile=({:d},{:d}) world=({:.0f},{:.0f}) type={:d} off={:d} cement={:d} alpha={:d} ovHandle=0x{:08x} terrHandle=0x{:08x}\n", mc2::fmt::as_i32(tileR), mc2::fmt::as_i32(tileC), static_cast<double>(((tileC - half) * wupv)), static_cast<double>(((half - tileR) * wupv)), mc2::fmt::as_i32(static_cast<int>(ovT)), mc2::fmt::as_u32(static_cast<unsigned long>(ovO)), mc2::fmt::as_i32(e->isCement() ? 1 : 0), mc2::fmt::as_i32(e->isAlpha() ? 1 : 0), mc2::fmt::as_u32(static_cast<unsigned long>(overlayHandle)), mc2::fmt::as_u32(static_cast<unsigned long>(e->terrainHandle)));
                    fflush(stdout);
                }
            }
            if (overlayHandle == 0xffffffffu) continue;

            // CEMENT-HARD-EDGE-1 (Slice 2, default-on; re-apply of reverted 322ef8b4):
            // Cement transition edges are now drawn analytically in the chunk frag
            // (terrain_lod_chunk.frag, neighbor-derived hard mask). Skip the legacy
            // gray cement-transition decal so it does not double-draw / seam over the
            // new frag edge.
            //
            // CEMENT-ROAD-SPLIT-1: the earlier "isCement()&&isAlpha() is concrete-only"
            // assumption was WRONG. isCement() is a MISNOMER for "uses the cement/
            // transition texture machinery": terrtxm.cpp:1493 sets CEMENT_FLAG on EVERY
            // createTransition — roads/runways/bridges included (see the explicit comment
            // terrtxm.cpp:1501-1504 "If we are a road ... ALPHA is TRUE"). So &Overlays
            // road/runway/bridge tiles ALSO satisfy isCement()&&isAlpha() and were being
            // wrongly skipped, making dirt/regular roads vanish. The frag cement-word
            // edge system only ever covers cement TERRAIN-TYPE tiles (10, 13-20), never
            // the &Overlays road system, so skip the legacy decal ONLY for a true
            // concrete transition: isCement()&&isAlpha() AND no road overlay on the tile.
            Overlays decalOverlayType = INVALID_OVERLAY;
            DWORD    decalOverlayOffset = 0;
            if (overlayMetadata)
                Terrain::mapData->getOverlay(tileR, tileC, decalOverlayType, decalOverlayOffset);
            if (CementWordSuppressesDecal(e, decalOverlayType, tileR, tileC, overlayMetadata)) { ++cementSuppressed; continue; }

            const DWORD overlayTexId = tex_resolve(overlayHandle);
            if (overlayTexId == 0) { ++unresolved; continue; }

            const DWORD slot = Terrain::mapData->getTexture(tileR, tileC) & 0xffffu;
            const uint16_t artLayer = slot < MC_MAX_TERRAIN_TXMS && g_cementAlphaLayerMapReady
                ? g_cementAlphaLayerIndexBySlot[slot] : 0xffffu;
            if (artLayer != 0xffffu && !gos_TerrainLodChunk_OverlaySidecarLoaded()) {
                static const bool adopt = mc2::gate::get<"MC2_OVERLAY_CEMENT_ADOPT">();
                surfaceWords[plane + size_t(tileR) * mapSide + tileC] =
                    0x80000000u | (adopt ? 0x00010000u : 0u) | artLayer;
                ++cookedRoads;
                continue;
            }

            // OVERLAY-TILE-HIRES-1 (gated, default OFF => kOld* constants used
            // verbatim). Mirror quad.cpp's per-overlay half-texel inset: a
            // resynced hi-res overlay tile sampled with the 64px inset (0.5/64)
            // crops its outer texel ring and breaks road continuity at seams.
            // physical edge = logical * uvScale (disk 4x-upscale TGA convention).
            float tileMinU = kOldMinU, tileMaxU = kOldMaxU;
            float tileMinV = kOldMinV, tileMaxV = kOldMaxV;
            if (MC2_OverlayTileHiresSize() > 0)
            {
                DWORD ovLogicalW = 0, ovLogicalH = 0;
                if (mc2::textures::logicalSize(overlayHandle, ovLogicalW, ovLogicalH))
                {
                    const DWORD ovEdge = ovLogicalW * mc2::textures::uvScale(overlayHandle);
                    if (ovEdge > 0)
                    {
                        tileMinU = tileMinV = 0.5f / static_cast<float>(ovEdge);
                        tileMaxU = tileMaxV = 1.0f - 0.5f / static_cast<float>(ovEdge);
                    }
                }
            }

            // 4 corners — mirror buildTerrainFaceCache (mapdata.cpp:266-275)
            // and fillWorldCacheVertex (mapdata.cpp:125-132). Quad
            // vertices[0..3] == cache worldVertices[0..3] (verified vs
            // MapData::makeLists v0=v(x,y) v1=v(x+1,y) v2=v(x+1,y+1)
            // v3=v(x,y+1)).
            const long idx0 = tileC + tileR * mapSide;
            PostcompVertexPtr p0 = &blocks[idx0];
            PostcompVertexPtr p1 = p0 + 1;
            PostcompVertexPtr p2 = p0 + mapSide + 1;
            PostcompVertexPtr p3 = p0 + mapSide;

            auto cornerXY = [&](long tR, long tC, float& wx, float& wy) {
                wx = float(tC - half) * wupv;     // fillWorldCacheVertex vx
                wy = float(half - tR) * wupv;     // fillWorldCacheVertex vy
            };

            DecalVert c[4];
            cornerXY(tileR,     tileC,     c[0].wx, c[0].wy);
            cornerXY(tileR,     tileC + 1, c[1].wx, c[1].wy);
            cornerXY(tileR + 1, tileC + 1, c[2].wx, c[2].wy);
            cornerXY(tileR + 1, tileC,     c[3].wx, c[3].wy);
            c[0].wz = p0->elevation + kOverlayElevOffset;  // M2d quad.cpp:2406
            c[1].wz = p1->elevation + kOverlayElevOffset;
            c[2].wz = p2->elevation + kOverlayElevOffset;
            c[3].wz = p3->elevation + kOverlayElevOffset;

            // TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: when the authored OVERLAY_V2
            // sidecar covers this tile it already draws the runway/road/cement
            // here (composited in the LOD-chunk frag). Baking the legacy decal
            // too draws it TWICE (visible double-draw at e.g. the mc2_01 runway).
            // Skip the legacy decal for tiles FULLY inside the sidecar's world
            // bounds so cement/runway draws once. Byte-identical when OVERLAY_V2
            // is off (sidecar not loaded -> guard false, no tile skipped) and
            // for tiles OUTSIDE the bounds (fall through -> baked normally, so
            // no gaps where the sidecar doesn't reach). Bounds-rect v0 (per the
            // decal recon): assumes the sidecar authors the overlay content
            // within its bounds; alpha-accurate per-tile suppression is a
            // follow-up. Env MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS=0 reverts.
            {
                static const bool s_decalSuppress = mc2::gate::get<"MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS">();
                if (s_decalSuppress && gos_TerrainLodChunk_OverlaySidecarLoaded()) {
                    const float* ob = gos_TerrainLodChunk_OverlayBounds();
                    const float bMinX = ob[0];
                    const float bMaxY = ob[1];
                    const float bMaxX = ob[0] + ob[2];
                    const float bMinY = ob[1] - ob[3];
                    bool allInside = true;
                    for (int k = 0; k < 4; ++k) {
                        if (c[k].wx < bMinX || c[k].wx > bMaxX ||
                            c[k].wy < bMinY || c[k].wy > bMaxY) { allInside = false; break; }
                    }
                    if (allInside) { ++sidecarSuppressed; continue; }   // sidecar owns this tile's overlay
                }
            }

            // [TERRAIN_OVERLAY v1] decal_corner_probe — env-gated
            // (MC2_TERRAIN_INDIRECT_TRACE), first few cement tiles only,
            // SILENT by default. Retained dormant diagnostic (demote-not-
            // delete per the debug-instrumentation rule): prints the bake's
            // per-corner world coords + the Terrain globals it derived them
            // from + an ELEV_IDENTICAL flag. The raster-sheet bug it was
            // built for was root-caused (unconditional all-map draw vs the
            // non-clip-safe terrain_overlay.vert) and FIXED 2026-05-17 (the
            // px.z in [0,1) guard) — the bake coords were proven correct, so
            // this stays as a coord-sanity check for any future bake-source
            // change, not an active investigation. Background:
            // memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md
            if (IsTraceEnabled()) {
                static int s_probeCount = 0;
                if (s_probeCount < 6) {
                    ++s_probeCount;
                    const bool elevIdentical =
                        (p0->elevation == p1->elevation) &&
                        (p1->elevation == p2->elevation) &&
                        (p2->elevation == p3->elevation);
                    mc2::fmt::print_out("[TERRAIN_OVERLAY v1] event=decal_corner_probe tileR={:d} tileC={:d} mapSide={:d} half={:d} wupv={:.6f} elev_identical={:d} c0=({:.3f},{:.3f},{:.5f}) c1=({:.3f},{:.3f},{:.5f}) c2=({:.3f},{:.3f},{:.5f}) c3=({:.3f},{:.3f},{:.5f}) elev_raw=({:.5f},{:.5f},{:.5f},{:.5f})\n", mc2::fmt::as_i32(tileR), mc2::fmt::as_i32(tileC), mc2::fmt::as_i32(static_cast<long>(mapSide)), mc2::fmt::as_i32(static_cast<long>(half)), static_cast<double>(wupv), mc2::fmt::as_i32(elevIdentical ? 1 : 0), c[0].wx, c[0].wy, c[0].wz, c[1].wx, c[1].wy, c[1].wz, c[2].wx, c[2].wy, c[2].wz, c[3].wx, c[3].wy, c[3].wz, static_cast<double>(p0->elevation), static_cast<double>(p1->elevation), static_cast<double>(p2->elevation), static_cast<double>(p3->elevation));
                    fflush(stdout);
                }
            }
            // ROAD-PBR-ABANDON-1: the per-tile PBR material-id bake
            // (ROAD-PBR-ASPHALT-1/GRAVEL-1, d7c0ff2d) is retired — stock form
            // restored: every tile takes the legacy tgl road-tile path.
            for (int k = 0; k < 4; ++k) {
                c[k].fog  = 1.0f;          // frag discards FogValue (terrain_overlay.frag:48)
                c[k].argb = 0xffffffffu;   // frag discards Color    (terrain_overlay.frag:48)
            }

            // uvMode: mirror worldQuadUVMode (mapdata.cpp:115-118). The cache's
            // uvData was resolved with this; M2d's diagonal/UV choice keys off
            // the live quad uvMode, which equals worldQuadUVMode(absRow,absCol)
            // (MapData::makeLists parity == absolute-tile parity).
            const long uvMode =
                ((tileR & 1) == (tileC & 1)) ? BOTTOMRIGHT : BOTTOMLEFT;

            auto emit = [&](const DecalVert& a, float au, float av,
                            const DecalVert& b, float bu, float bv,
                            const DecalVert& d, float du, float dv) {
                PendingTri t;
                t.texId = static_cast<uint32_t>(overlayTexId);
                t.v[0] = a; t.v[0].u = au; t.v[0].v = av;
                t.v[1] = b; t.v[1].u = bu; t.v[1].v = bv;
                t.v[2] = d; t.v[2].u = du; t.v[2].v = dv;
                pending.push_back(t);
            };

            // EXACT reproduction of M2d's per-uvMode tri emit
            // (quad.cpp:2412-2443), but UNCONDITIONAL (no pzTri1/pzTri2).
            // tileMin/Max == kOld* constants unless OVERLAY-TILE-HIRES is on.
            if (uvMode == BOTTOMLEFT) {
                emit(c[0], tileMinU, tileMinV,
                     c[1], tileMaxU, tileMinV,
                     c[3], tileMinU, tileMaxV);
                emit(c[1], tileMaxU, tileMinV,
                     c[2], tileMaxU, tileMaxV,
                     c[3], tileMinU, tileMaxV);
            } else {
                emit(c[0], tileMinU, tileMinV,
                     c[1], tileMaxU, tileMinV,
                     c[2], tileMaxU, tileMaxV);
                emit(c[0], tileMinU, tileMinV,
                     c[2], tileMaxU, tileMaxV,
                     c[3], tileMinU, tileMaxV);
            }
        }
    }

    // Group pending tris into contiguous per-texId draw ranges (mirror
    // pushToOverlayBatch_'s run-coalescing: consecutive same-texHandle tris
    // extend the last range).
    for (const PendingTri& t : pending) {
        if (!g_decalDrawRanges.empty() &&
            g_decalDrawRanges.back().texHandle == t.texId) {
            g_decalDrawRanges.back().vertCount += 3;
        } else {
            DecalDrawRange r;
            r.texHandle = t.texId;
            r.firstVert = static_cast<uint32_t>(verts.size());
            r.vertCount = 3;
            g_decalDrawRanges.push_back(r);
        }
        verts.push_back(t.v[0]);
        verts.push_back(t.v[1]);
        verts.push_back(t.v[2]);
    }

    g_decalVertCount = static_cast<int>(verts.size());
    if (fields && fields->cementWords() != surfaceWords)
        fields->setCementWords(surfaceWords.data(), static_cast<int>(surfaceWords.size()));
    MC2_DIAGF("[TERRAIN_ROAD_COOK] tiles=%d fallback_vertices=%d unresolved=%d\n",
        cookedRoads, g_decalVertCount, unresolved);
    g_decalVBODirty = unresolved != 0;
#ifdef MC2_VULKAN_ISLAND
    if (mc2_vk_overlay_enabled()) {
        const bool identical = g_decalVertsCpu.size() == verts.size() &&
            (verts.empty() || std::memcmp(g_decalVertsCpu.data(), verts.data(), verts.size() * sizeof(DecalVert)) == 0);
        if (g_decalLoadPending) {
            mc2::fmt::print_out("[OVERLAY_PRESTAGE] validate identical={} old_verts={} verts={} cement_skip={} sidecar_skip={} unresolved={}\n",
                identical, g_decalVertsCpu.size(), verts.size(), cementSuppressed, sidecarSuppressed, unresolved);
            g_decalLoadPending = false;
        }
        if (!identical) { g_decalVertsCpu = verts; ++g_decalBakeVersion; }
    }
#endif

}

void BuildDecalStaticVBO() { BuildDecalStaticVBOFromInputs(true); }

// Mirror RebuildMineStaticVBOIfDirty (no texture-array first-build step —
// cement overlay uses the existing per-mission terrain texture nodes via
// tex_resolve, not a dedicated 2-layer array).
void RebuildDecalStaticVBOIfDirty() {
    if (!g_decalVBODirty) return;
    if (!Terrain::mapData || !Terrain::mapData->getBlocks() || !g_recipeReady) return;
    BuildDecalStaticVBO();
}



// ---------------------------------------------------------------------------
// Vegetation coverage follows the terrain cement words plus decals that the
// bake actually submits. Solid tiles use a sentinel; transitions use R8 blocks.
// ---------------------------------------------------------------------------
namespace {
constexpr int kHsRes = 32;                       // texels per tile side (4 wu at 128 wu/tile)
struct HardSurfaceField {
    bool                 built = false;
    long                 side = 0;               // tiles per side (cacheW)
    long                 half = 0;
    float                wupv = 128.0f;
    std::vector<int32_t> blockOf;                // tile -> block index or -1
    std::vector<uint8_t> texels;                 // blocks * kHsRes * kHsRes
    int                  tilesAlpha = 0, tilesPure = 0, tilesNoImage = 0, tilesRoadSkipped = 0;
};
HardSurfaceField g_hs;
uint64_t g_hsVersion = 1;
// Cement mutations that named their tile (a destroyed bridge) rebuild just that tile and tell the materials consumer which
// rects to recomposite; everything else (and an overflowing list) is a full invalidation.
constexpr size_t kMaxDirtyTiles = 64;
std::vector<uint32_t> g_hsPending;   // marked, not yet rebuilt in g_hs (row * side + col)
std::vector<uint32_t> g_hsDirty;     // rebuilt/changed since the consumer last took them
bool g_hsDirtyAll = true;

inline uint8_t hsTexel(long ix, long iy) {
    if (ix < 0 || iy < 0) return 0;
    const long tc = ix / kHsRes, tr = iy / kHsRes;
    if (tc >= g_hs.side || tr >= g_hs.side) return 0;
    const int32_t b = g_hs.blockOf[static_cast<size_t>(tr) * static_cast<size_t>(g_hs.side) + static_cast<size_t>(tc)];
    if (b == -2) return 255; // uniform solid coverage needs no texel block
    if (b < 0) return 0;
    return g_hs.texels[(static_cast<size_t>(b) * kHsRes + static_cast<size_t>(iy % kHsRes)) * kHsRes + static_cast<size_t>(ix % kHsRes)];
}

// One tile of the hard-surface field (also the incremental rebuild unit after a cement mutation).
void BuildHardSurfaceTile(long tr, long tc) {
    const long mapSide = Terrain::realVerticesMapSide;
    const long cacheW  = g_hs.side;
    const auto* fields = gos_TerrainFieldStore();
    const auto wordAt = [&](int row, int col) -> uint32_t {
        if (!fields || row < 0 || col < 0 || row >= mapSide || col >= mapSide) return 0;
        const size_t index = size_t(row) * size_t(mapSide) + size_t(col);
        return index < fields->cementWords().size() ? fields->cementWords()[index] : 0;
    };
    const MapData::WorldQuadTerrainCacheEntry* e = Terrain::mapData->getTerrainFaceCacheEntry(tr, tc);
    if (!e || !e->isValid() || (!e->isCement() && !wordAt(tr, tc))) return;
    Overlays ovT = INVALID_OVERLAY; DWORD ovO = 0;
    Terrain::mapData->getOverlay(tr, tc, ovT, ovO);
    const bool softOverlay = ovT == ROUGH || ovT == DIRT_ROAD;
    if (softOverlay) ++g_hs.tilesRoadSkipped;

    const bool alphaTile = e->isAlpha() && e->overlayHandle != 0xffffffffu;
    std::shared_ptr<const RenderCore::KtxImage> img;
    if (alphaTile && e->overlayHandle < static_cast<DWORD>(mc2::textures::kTextureAssetLimit))
        img = mc2::textures::image(e->overlayHandle);
    const bool imgOk = img && img->vkFormat == 37 && img->width > 0 && img->height > 0 &&
                       img->pixels.size() >= static_cast<size_t>(img->width) * static_cast<size_t>(img->height) * 4u;

    const int32_t b = static_cast<int32_t>(g_hs.texels.size() / (static_cast<size_t>(kHsRes) * kHsRes));
    g_hs.texels.resize(g_hs.texels.size() + static_cast<size_t>(kHsRes) * kHsRes, 0);
    uint8_t* blk = &g_hs.texels[static_cast<size_t>(b) * kHsRes * kHsRes];
    const std::mdspan<uint8_t, std::extents<size_t, kHsRes, kHsRes>> coverage(blk);
    if (!alphaTile || !imgOk) {
        // Pure cement (class 3), or an alpha tile whose source pixels are
        // unavailable: full coverage (conservative -- no vegetation).
        memset(blk, 255, static_cast<size_t>(kHsRes) * kHsRes);
        if (alphaTile) ++g_hs.tilesNoImage; else ++g_hs.tilesPure;
    } else {
        ++g_hs.tilesAlpha;
        // The bake maps u along +x (tileC) and v along +row (south) over
        // [kOldMin, kOldMax]; sample the same way (box-filtered per texel).
        const int W = img->width, H = img->height;
        const float u0 = kOldMinU, u1 = kOldMaxU, v0 = kOldMinV, v1 = kOldMaxV;
        for (int r = 0; r < kHsRes; ++r) {
            for (int c = 0; c < kHsRes; ++c) {
                const float ua = u0 + (u1 - u0) * (static_cast<float>(c)     / kHsRes);
                const float ub = u0 + (u1 - u0) * (static_cast<float>(c + 1) / kHsRes);
                const float va = v0 + (v1 - v0) * (static_cast<float>(r)     / kHsRes);
                const float vb = v0 + (v1 - v0) * (static_cast<float>(r + 1) / kHsRes);
                const int x0 = std::clamp(static_cast<int>(ua * W), 0, W - 1);
                const int x1 = std::clamp(static_cast<int>(ub * W), x0, W - 1);
                const int y0 = std::clamp(static_cast<int>(va * H), 0, H - 1);
                const int y1 = std::clamp(static_cast<int>(vb * H), y0, H - 1);
                unsigned sum = 0, n = 0;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        sum += img->pixels[(static_cast<size_t>(y) * W + x) * 4u + 3u];
                        ++n;
                    }
                coverage[r, c] = static_cast<uint8_t>(n ? (sum + n / 2) / n : 0);
            }
        }
    }
    const bool overlayDrawn = alphaTile && !softOverlay && !CementWordSuppressesDecal(e, ovT, tr, tc);
    const uint32_t word = wordAt(tr, tc);
    for (int row = 0; row < kHsRes; ++row) {
        for (int col = 0; col < kHsRes; ++col) {
            const float u = (float(col) + 0.5f) / kHsRes;
            const float v = (float(row) + 0.5f) / kHsRes;
            float atlasAlpha = coverage[row, col] / 255.0f;
            if ((word & 0x00800000u) && fields) {
                const auto& atlas = fields->cementAtlas();
                const int grid = g_cementAtlasGridSide;
                if (grid > 0 && atlas.width > 0 && atlas.height > 0 && !atlas.texels.empty()) {
                    const int layer = int(word & 0xffffu);
                    const int x = std::min(int((float(layer % grid) + u) * atlas.width / grid), atlas.width - 1);
                    const int y = std::min(int((float(layer / grid) + v) * atlas.height / grid), atlas.height - 1);
                    atlasAlpha = atlas.texels[(size_t(y) * atlas.width + x) * 4 + 3] / 255.0f;
                }
            }
            const float cement = mc2::vegetation::cementWordCoverage(int(tr), int(tc), u, v, atlasAlpha, wordAt);
            coverage[row, col] = uint8_t(255 * mc2::vegetation::renderedHardSurfaceCoverage(
                cement, coverage[row, col] / 255.0f, overlayDrawn) + 0.5f);
        }
    }
    const bool solid = std::all_of(blk, blk + kHsRes * kHsRes, [](uint8_t value) { return value == 255; });
    if (solid) g_hs.texels.resize(size_t(b) * kHsRes * kHsRes);
    g_hs.blockOf[static_cast<size_t>(tr) * static_cast<size_t>(cacheW) + static_cast<size_t>(tc)] = solid ? -2 : b;
}

void BuildHardSurfaceField() {
    g_hs = HardSurfaceField{};
    g_hsPending.clear();
    g_hs.built = true;
    if (!Terrain::mapData) return;
    const long mapSide = Terrain::realVerticesMapSide;
    const long cacheW  = mapSide - 1;
    if (cacheW <= 0) return;
    g_hs.side = cacheW;
    g_hs.half = Terrain::halfVerticesMapSide;
    g_hs.wupv = Terrain::worldUnitsPerVertex;
    g_hs.blockOf.assign(static_cast<size_t>(cacheW) * static_cast<size_t>(cacheW), -1);

    for (long tr = 0; tr < cacheW; ++tr)
        for (long tc = 0; tc < cacheW; ++tc) BuildHardSurfaceTile(tr, tc);
    MC2_DIAGF("[VEG_HARDSURFACE v1] event=field_built tiles_alpha=%d tiles_pure=%d tiles_alpha_noimage=%d tiles_road_skipped=%d blocks=%zu\n",
              g_hs.tilesAlpha, g_hs.tilesPure, g_hs.tilesNoImage, g_hs.tilesRoadSkipped,
              g_hs.texels.size() / (static_cast<size_t>(kHsRes) * kHsRes));
}

// Pure bilinear of a texel fetcher; split out so the unit test can drive it.
}  // namespace

static void FlushHardSurfaceTiles() {
    if (!g_hs.built) return;
    for (const uint32_t t : g_hsPending) {
        g_hs.blockOf[t] = -1;
        BuildHardSurfaceTile(long(t / uint32_t(g_hs.side)), long(t % uint32_t(g_hs.side)));
    }
    g_hsPending.clear();
}

float HardSurfaceCoverage(float wx, float wy) {
    if (!g_hs.built) BuildHardSurfaceField();
    FlushHardSurfaceTiles();
    if (g_hs.blockOf.empty()) return 0.0f;
    // Continuous texel coords; texel centres sit at +0.5.
    const float X = (wx / g_hs.wupv + static_cast<float>(g_hs.half)) * kHsRes - 0.5f;
    const float Y = (static_cast<float>(g_hs.half) - wy / g_hs.wupv) * kHsRes - 0.5f;
    const float fxl = std::floor(X), fyl = std::floor(Y);
    const float fx = X - fxl, fy = Y - fyl;
    const long ix = static_cast<long>(fxl), iy = static_cast<long>(fyl);
    const float a = static_cast<float>(hsTexel(ix, iy))     * (1.0f - fx) + static_cast<float>(hsTexel(ix + 1, iy))     * fx;
    const float b = static_cast<float>(hsTexel(ix, iy + 1)) * (1.0f - fx) + static_cast<float>(hsTexel(ix + 1, iy + 1)) * fx;
    return (a * (1.0f - fy) + b * fy) * (1.0f / 255.0f);
}

void InvalidateHardSurfaceField() {
    g_hs = HardSurfaceField{};
    g_hsPending.clear(); g_hsDirty.clear(); g_hsDirtyAll = true;
    ++g_hsVersion;
}

void MarkDecalDirtyTile(long row, long col) {
    if (g_hsDirtyAll || !g_hs.built || row < 0 || col < 0 || row >= g_hs.side || col >= g_hs.side || g_hsDirty.size() >= kMaxDirtyTiles) {
        MarkDecalDirty();
        return;
    }
    const uint32_t t = uint32_t(row * g_hs.side + col);
    g_hsPending.push_back(t); g_hsDirty.push_back(t);
    ++g_hsVersion;
    g_decalVBODirty = true;
}

// What changed in the hard-surface field since the materials consumer last looked: everything, or world rects
// {x0, y0, x1, y1}, each a changed tile padded by one tile (the coverage lookup is bilinear). Consumes the list.
HardSurfaceDirty TakeHardSurfaceDirty() {
    HardSurfaceDirty d;
    d.all = g_hsDirtyAll || g_hsDirty.size() > d.rects.size();
    if (!d.all) {
        for (const uint32_t t : g_hsDirty) {
            const float col = float(t % uint32_t(g_hs.side)), row = float(t / uint32_t(g_hs.side)), half = float(g_hs.half);
            d.rects[d.count++] = {(col - 1.0f - half) * g_hs.wupv, (half - (row + 2.0f)) * g_hs.wupv,
                                  (col + 2.0f - half) * g_hs.wupv, (half - (row - 1.0f)) * g_hs.wupv};
        }
    }
    g_hsDirtyAll = false;
    g_hsDirty.clear();
    return d;
}
uint64_t HardSurfaceVersion() { return g_hsVersion; }

void HardSurfaceStats(int* alpha, int* pure, int* noImage) {
    if (!g_hs.built) BuildHardSurfaceField();
    if (alpha) *alpha = g_hs.tilesAlpha;
    if (pure) *pure = g_hs.tilesPure;
    if (noImage) *noImage = g_hs.tilesNoImage;
}

int          GetDecalVertCount()    { return g_decalVertCount; }

// Mirror DrawMineStatic. Lazy rebuild-if-dirty on first armed draw, single
// bridge dispatch, NO clear (static buffer persists). Returns true on a
// successful zero-emit frame (mission has no cement overlay) — the M2d gate-
// off is still the point.
bool DrawDecalStatic() {
    RebuildDecalStaticVBOIfDirty();
    if (g_decalVertCount <= 0 || g_decalDrawRanges.empty()) {
        return true;  // no cement overlay this mission — successful no-op
    }
    static_assert(sizeof(DecalDrawRange) == sizeof(GosDecalStaticDraw),
                  "DecalDrawRange must layout-match GosDecalStaticDraw");
#ifdef MC2_VULKAN_ISLAND
    // VK-PARITY-OVERLAY-1: under the VK umbrella the GL scene FBO is discarded, so hand the
    // static bake to the VK overlay owner (drawn by mc2_vk_overlay_run after Render.Decals)
    // INSTEAD of the GL draw. Gate off => falls through to the unchanged GL bridge call.
    if (mc2_vk_overlay_enabled() && !g_decalVertsCpu.empty() &&
        g_decalVertsCpu.size() == static_cast<size_t>(g_decalVertCount)) {
        static_assert(sizeof(DecalDrawRange) == sizeof(Mc2VkOverlayDraw),
                      "DecalDrawRange must layout-match Mc2VkOverlayDraw");
        static_assert(sizeof(DecalVert) == 28, "DecalVert must be the 28B WorldOverlayVert");
        mc2_vk_overlay_submit(0u, g_decalVertsCpu.data(), static_cast<uint32_t>(g_decalVertsCpu.size()),
                              reinterpret_cast<const Mc2VkOverlayDraw*>(g_decalDrawRanges.data()),
                              static_cast<uint32_t>(g_decalDrawRanges.size()), g_decalBakeVersion);
        Counters_AddDecalStaticTrisDrawn(static_cast<long long>((g_decalVertCount / 3)));
        return true;
    }
#endif
    return false;

}

}  // namespace gos_terrain_indirect

#ifdef MC2_VULKAN_ISLAND
extern "C" int mc2_terrain_overlay_get_load_stream(const void** verts, uint32_t* vertCount,
    const Mc2VkOverlayDraw** draws, uint32_t* drawCount, uint64_t* version) {
    using namespace gos_terrain_indirect;
    if (!Terrain::mapData || !Terrain::mapData->getBlocks() || !g_recipeReady || !mc2_vk_overlay_enabled()) return 0;
    // Mission::update calls TerrainTextures::update before the first draw. That
    // purges overlays/numOverlays: getOverlay then returns INVALID_OVERLAY and
    // road adjacency changes (mc2_01: 450 load vertices versus 228 at draw).
    // Supply the post-purge classification without purging the live load metadata.
    BuildDecalStaticVBOFromInputs(false);
    if (g_decalVBODirty) return 0;
    // Validate the explicit snapshot at first draw; mutations remain retryable.
    g_decalLoadPending = true;
    g_decalVBODirty = true;
    *verts = g_decalVertsCpu.data(); *vertCount = static_cast<uint32_t>(g_decalVertsCpu.size());
    *draws = reinterpret_cast<const Mc2VkOverlayDraw*>(g_decalDrawRanges.data());
    *drawCount = static_cast<uint32_t>(g_decalDrawRanges.size()); *version = g_decalBakeVersion;
    mc2::fmt::print_out("[OVERLAY_PRESTAGE] load verts={} draws={} cement_ready={} sidecar={}\n",
        *vertCount, *drawCount, g_cementLayerMapReady, gos_TerrainLodChunk_OverlaySidecarLoaded());
    return 1;
}
#endif

// C-linkage forwarder for the cement-mutation invalidation sites (code/
// bldng.cpp bridge-destroy + mclib/terrain.cpp Terrain::setOverlay). Mirrors
// gos_terrain_indirect_MarkMineDirty: forward-declared `extern void
// gos_terrain_indirect_MarkDecalDirty();` at the call sites so widely-included
// headers do not need gos_terrain_indirect.h. Idempotent — chain mutations
// debounce to one rebuild/paint via the dirty flag.
void gos_terrain_indirect_MarkDecalDirty() {
    gos_terrain_indirect::MarkDecalDirty();
}

// VEG-CEMENT-1 C-linkage forwarders (VegetationAdapter avoids gos_terrain_indirect.h).
uint64_t gos_terrain_indirect_HardSurfaceVersion() {
    return gos_terrain_indirect::HardSurfaceVersion();
}
void gos_terrain_indirect_MarkDecalDirtyTile(long row, long col) {
    gos_terrain_indirect::MarkDecalDirtyTile(row, col);
}
float gos_terrain_indirect_HardSurfaceCoverage(float wx, float wy) {
    return gos_terrain_indirect::HardSurfaceCoverage(wx, wy);
}
void gos_terrain_indirect_HardSurfaceStats(int* alpha, int* pure, int* noImage) {
    gos_terrain_indirect::HardSurfaceStats(alpha, pure, noImage);
}
