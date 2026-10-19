// gos_terrain_indirect: mine static VBO and texture array, decal static VBO and their dirty tracking.
// Declared in the class headers; shared names in gos_terrain_indirect_internal.h.
#include "gos_terrain_indirect_internal.h"
#include "mc2fmt.h"


// ---------------------------------------------------------------------------
// PR2c Stage 1c — mine static-bake infrastructure.
//
// Builds a per-mission mine VBO + 2-layer mine/blown texture-array. Both are
// populated lazily on first dirty event (i.e., first MissionMap::setMine
// call after Reset, including the per-cell init-time setMine loop). Per spec
// at 2026-05-08-pr2c-mine-static-bake-design.md.
//
// Stage 1c is build-and-invalidate scaffolding only — RebuildMineStaticVBOIfDirty
// is callable but not yet wired into per-frame from the bridge. Stage 2c
// adds the bridge + draw + legacy gate-off (single-PR per N2 partial-landing
// rule).
// ---------------------------------------------------------------------------

namespace {

GLuint g_mineStaticVBO_GL         = 0;
int    g_mineVertCount            = 0;
bool   g_mineVBODirty             = true;   // first build triggers via setMine init loop
bool   g_mineVBOFirstBuildPending = true;   // gates lazy texture-array build (R7)
GLuint g_mineTextureArrayGL       = 0;
bool   g_mineTextureArrayReady    = false;

// Vertex format: 3 float pos (world space) + 2 float uv (sprite 0..1) +
// 1 uint layer (0=mine, 1=blown). 24 bytes/vert; 6 verts/cell (2 tris).
struct MineVert {
    float    pos[3];
    float    uv[2];
    uint32_t layer;
};

}  // namespace

namespace gos_terrain_indirect {

void MarkMineDirty() {
    g_mineVBODirty = true;
}

void ResetMineStaticVBO() {
    g_mineVertCount            = 0;
    g_mineVBODirty             = true;
    g_mineVBOFirstBuildPending = true;
    // Keep g_mineStaticVBO_GL allocation across missions; reused by next
    // BuildMineStaticVBO via glBufferData (which orphans + reallocates the
    // backing store).
}

void ResetMineTextureArray() {
    g_mineTextureArrayReady = false;
    // Keep g_mineTextureArrayGL allocation across missions. Source TGAs
    // (defaults/mine_00.tga, defaults/minescorch_00.tga) are process-stable;
    // first BuildMineTextureArray call after reset re-marks ready without
    // re-allocating the GL texture.
}

// [TEMP MINE_GLPROBE] eager-drain probe — env MC2_DECAL_GLPROBE=1 (shared
// gate with the decal probe). Removed once root cause is pinned.
#define MINE_GLPROBE(tag) do { \
    static const bool s_p = mc2::gate::get<"MC2_DECAL_GLPROBE">(); \
    if (s_p) { GLenum e; while ((e = glGetError()) != GL_NO_ERROR) \
        printf("[MINE_GLPROBE] at=%s err=0x%x\n", tag, (unsigned)e); \
        fflush(stdout); } } while(0)

void BuildMineTextureArray() {
    // Texture-array contents are process-stable; re-read only if first build
    // or invalidated.
    if (g_mineTextureArrayReady) return;
    MINE_GLPROBE("atlas_entry");

    // R7: handles must be loaded by setupTextures before this fires. If still
    // 0xffffffff, bail — the next dirty event retries. (Stage 2c invokes
    // RebuildMineStaticVBOIfDirty post first-paint via the bridge; setupTextures
    // runs per-quad each frame and lazy-loads the handles at quad.cpp:520-531
    // before any per-frame mine work happens.)
    const DWORD mineSlot  = TerrainQuad::mineTextureHandle;
    const DWORD blownSlot = TerrainQuad::blownTextureHandle;
    if (mineSlot == 0xffffffffu || blownSlot == 0xffffffffu) {
        if (IsTraceEnabled()) {
            mc2::fmt::print_out("[TERRAIN_INDIRECT v1] event=mine_atlas_skip reason=handles_not_loaded mine={:d} blown={:d}\n", mc2::fmt::as_u32((unsigned)mineSlot), mc2::fmt::as_u32((unsigned)blownSlot));
            fflush(stdout);
        }
        return;
    }

    const DWORD mineGosHandle  = tex_resolve(mineSlot);
    const DWORD blownGosHandle = tex_resolve(blownSlot);
    if (mineGosHandle == 0u || blownGosHandle == 0u) return;

    const GLuint mineGLTex  = gos_terrain_bridge_glTextureForGosHandle((unsigned)mineGosHandle);
    const GLuint blownGLTex = gos_terrain_bridge_glTextureForGosHandle((unsigned)blownGosHandle);
    if (mineGLTex == 0 || blownGLTex == 0) return;

    constexpr int kW = 16;
    constexpr int kH = 16;
    std::vector<uint32_t> mineBuf((size_t)kW * kH, 0u);
    std::vector<uint32_t> blownBuf((size_t)kW * kH, 0u);

    GLint savedActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0);
    GLint savedPackAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &savedPackAlign);
    GLint savedUnpackAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    MINE_GLPROBE("before_getteximage");
    MC2_GL_BindTexture(GL_TEXTURE_2D, mineGLTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, mineBuf.data());
    MINE_GLPROBE("after_getteximage_mine");
    MC2_GL_BindTexture(GL_TEXTURE_2D, blownGLTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, blownBuf.data());
    MINE_GLPROBE("after_getteximage_blown");

    // TEX-CLASS: asset-pool -- minefield 2D_ARRAY (content)
    if (g_mineTextureArrayGL == 0) glGenTextures(1, &g_mineTextureArrayGL);
    MC2_GL_BindTexture(GL_TEXTURE_2D_ARRAY, g_mineTextureArrayGL);
    // Allocate 2-layer storage. glTexImage3D with NULL data orphans / reserves.
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kW, kH, 2,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, /*layer=*/0, kW, kH, 1,
                    GL_BGRA, GL_UNSIGNED_BYTE, mineBuf.data());
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, /*layer=*/1, kW, kH, 1,
                    GL_BGRA, GL_UNSIGNED_BYTE, blownBuf.data());
    // NEAREST filter, no mips — matches gosHint_DisableMipmap | DontShrink at
    // quad.cpp:524, :531. CLAMP_TO_EDGE: sprite UVs are 0..1, no edge-bleed risk.
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    MC2_GL_BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    MINE_GLPROBE("after_teximage3d");

    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
    MC2_GL_BindTexture(GL_TEXTURE_2D, (GLuint)savedTex0);
    glActiveTexture((GLenum)savedActive);

    g_mineTextureArrayReady = true;
    MINE_GLPROBE("atlas_exit");

    if (IsTraceEnabled()) {
        mc2::fmt::print_out("[TERRAIN_INDIRECT v1] event=mine_atlas_built mineSlot={:d} blownSlot={:d} gltex={:d} dim={:d}x{:d}\n", mc2::fmt::as_u32((unsigned)mineSlot), mc2::fmt::as_u32((unsigned)blownSlot), mc2::fmt::as_u32((unsigned)g_mineTextureArrayGL), mc2::fmt::as_i32(kW), mc2::fmt::as_i32(kH));
        fflush(stdout);
    }
}

void BuildMineStaticVBO() {
    MINE_GLPROBE("vbo_entry");
    if (!GameMap || !land) {
        g_mineVertCount = 0;
        return;
    }

    // Empirically tiny in stock content — start with 64 cell capacity.
    std::vector<MineVert> verts;
    verts.reserve(64 * 6);

    const int W = (int)GameMap->width;
    const int H = (int)GameMap->height;
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

    g_mineVertCount = (int)verts.size();

    if (g_mineStaticVBO_GL == 0) glGenBuffers(1, &g_mineStaticVBO_GL);
    glBindBuffer(GL_ARRAY_BUFFER, g_mineStaticVBO_GL);
    // glBufferData with size>0 always uploads even if vector is empty
    // (verts.data() returns a non-null sentinel on empty MSVC vectors, but
    // we pass nullptr-on-empty defensively to match Khronos expectations).
    MC2_GL_BufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(verts.size() * sizeof(MineVert)),
                 verts.empty() ? nullptr : verts.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    MINE_GLPROBE("vbo_after_bufferdata");

    if (IsTraceEnabled()) {
        mc2::fmt::print_out("[TERRAIN_INDIRECT v1] event=mine_vbo_built cells={:d} verts={:d} vbo_gl={:d}\n", mc2::fmt::as_i32(g_mineVertCount / 6), mc2::fmt::as_i32(g_mineVertCount), mc2::fmt::as_u32((unsigned)g_mineStaticVBO_GL));
        fflush(stdout);
    }
}

void RebuildMineStaticVBOIfDirty() {
    if (!g_mineVBODirty) return;
    if (g_mineVBOFirstBuildPending) {
        BuildMineTextureArray();
        // Mark first-build done regardless of texture-array ready state —
        // BuildMineTextureArray handles its own retry via g_mineTextureArrayReady.
        g_mineVBOFirstBuildPending = false;
    }
    BuildMineStaticVBO();
    g_mineVBODirty = false;
}

unsigned int GetMineStaticVBO_GL()      { return g_mineStaticVBO_GL; }
int          GetMineVertCount()         { return g_mineVertCount; }
unsigned int GetMineTextureArrayGL()    { return g_mineTextureArrayGL; }
bool         IsMineTextureArrayReady()  { return g_mineTextureArrayReady; }

// PR2c Stage 2c — armed iff MC2_TERRAIN_INDIRECT_MINE=1 (env gate).
//
// We do NOT gate on g_mineTextureArrayReady here — that creates a bootstrap
// circular dependency: the texture-array can only be built inside
// BuildMineTextureArray, which is only called from RebuildMineStaticVBOIfDirty,
// which is only called from DrawMineStatic, which is only called from the
// Render.TerrainMines zone IF IsFrameMineArmed() returns true. Gating on
// readiness here means the first build never happens.
//
// Instead: arming = env on. DrawMineStatic handles the texture-not-ready
// case internally (returns true no-op until the next dirty event lazy-builds).
// First paint cycle after mission load: gate-offs fire, legacy paths skipped,
// DrawMineStatic invokes Rebuild* which lazy-builds the texture-array on
// first call (handles loaded by setupTextures on the SAME frame's earlier
// per-quad loop — R7 timing trap mitigated by paint-order: setupTextures
// runs before Render.TerrainMines).
bool IsFrameMineArmed() {
    return IsMineEnabled();
}

// PR2c Stage 2c — invoked from txmmgr.cpp's Render.TerrainMines zone. Lazy-builds
// VBO + texture-array on first dirty event; issues one glDrawArrays via the
// bridge if there's anything to draw.
bool DrawMineStatic() {
    RebuildMineStaticVBOIfDirty();
    if (g_mineVertCount <= 0) {
        // Successful zero-emit frame (mission has no mines yet). Legacy paths
        // are still gated off — that's the whole point.
        return true;
    }
    if (!g_mineTextureArrayReady) {
        // Build retry pending; the texture-array build couldn't complete (e.g.,
        // handles still 0xffffffff at first call). Treat as no-op until next
        // dirty event re-attempts.
        return true;
    }
    const bool ok = gos_terrain_bridge_drawMineStatic(
        g_mineVertCount,
        g_mineStaticVBO_GL,
        g_mineTextureArrayGL);
    if (ok) {
        // 6 verts per cell (2 tris * 3 verts).
        Counters_AddIndirectMineDrawnCells((long long)(g_mineVertCount / 6));
    }
    return ok;
}

}  // namespace gos_terrain_indirect

// C-linkage forwarder for mclib/move.h's inline setMine — avoids pulling
// the full gos_terrain_indirect.h into the widely-included move.h. The
// symbol is forward-declared at the call site in move.h:
//     extern void gos_terrain_indirect_MarkMineDirty();
// and resolved at link time.
void gos_terrain_indirect_MarkMineDirty() {
    gos_terrain_indirect::MarkMineDirty();
}

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

GLuint                       g_decalStaticVBO_GL  = 0;
int                          g_decalVertCount     = 0;
bool                         g_decalVBODirty      = true;  // first build triggers via Rebuild on first armed draw
std::vector<DecalDrawRange>  g_decalDrawRanges;
#ifdef MC2_VULKAN_ISLAND
// VK-PARITY-OVERLAY-1: CPU copy of the static bake (the GL VBO is write-only here) so the
// VK overlay owner can draw it into the shared GBuffer. Version bumps per rebuild.
std::vector<DecalVert>       g_decalVertsCpu;
uint64_t                     g_decalBakeVersion   = 0;
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
    g_decalVBODirty = true;
}

// Mirror ResetMineStaticVBO — CPU-clear only; keep g_decalStaticVBO_GL
// allocation across missions (next BuildDecalStaticVBO reuses it via
// glBufferData orphan+realloc).
void ResetDecalStaticVBO() {
    g_decalVertCount = 0;
    g_decalVBODirty  = true;
    g_decalDrawRanges.clear();
#ifdef MC2_VULKAN_ISLAND
    g_decalVertsCpu.clear();
#endif
    if (IsTraceEnabled()) {
        mc2::fmt::print_out("[TERRAIN_OVERLAY v1] event=decal_vbo_reset vbo_gl={:d} kept=1\n", mc2::fmt::as_u32((unsigned)g_decalStaticVBO_GL));
        fflush(stdout);
    }
}

// Mirror BuildMineStaticVBO. Iterates the map-immutable Shape-C terrain face
// cache exactly as MapData::buildTerrainFaceCache does (mapdata.cpp:262-309):
// same tile range, same 4-corner block layout, same worldQuadUVMode formula.
void BuildDecalStaticVBO() {
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

    // MISSING-RUNWAY-1 tile census (MC2_TERRAIN_TILE_CENSUS=1, silent by
    // default): one summary line per distinct (terrainType, texTileIdx,
    // cement, alpha, overlayId) combo with count + world bbox. Answers "what
    // IS the runway in this map's data" without knowing its coords up front.
    {
        static const bool s_tileCensus = mc2::gate::get<"MC2_TERRAIN_TILE_CENSUS">();
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
                        ((unsigned long long)(unsigned)terrType << 40) |
                        ((unsigned long long)(texData & 0xffffu) << 8) |
                        ((unsigned long long)((int)ovT + 1) << 2) |
                        ((unsigned)cem << 1) | (unsigned)alp;
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
                mc2::fmt::print_out("[TILE_CENSUS v1] terr={:d} tex={:d} ov={:d} cement={:d} alpha={:d} n={:d} bbox=({:.0f},{:.0f})-({:.0f},{:.0f})\n", mc2::fmt::as_i32((long)(k >> 40)), mc2::fmt::as_u32((unsigned long)((k >> 8) & 0xffffu)), mc2::fmt::as_i32((int)((k >> 2) & 0x3f) - 1), mc2::fmt::as_u64((k >> 1) & 1ull), mc2::fmt::as_u64(k & 1ull), mc2::fmt::as_i32(kv.second.n), kv.second.minX, kv.second.minY, kv.second.maxX, kv.second.maxY);
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
                    mc2::fmt::print_out("[TERRAIN_OVERLAY v1] event=overlay_census tile=({:d},{:d}) world=({:.0f},{:.0f}) type={:d} off={:d} cement={:d} alpha={:d} ovHandle=0x{:08x} terrHandle=0x{:08x}\n", mc2::fmt::as_i32(tileR), mc2::fmt::as_i32(tileC), (double)((tileC - half) * wupv), (double)((half - tileR) * wupv), mc2::fmt::as_i32((int)ovT), mc2::fmt::as_u32((unsigned long)ovO), mc2::fmt::as_i32(e->isCement() ? 1 : 0), mc2::fmt::as_i32(e->isAlpha() ? 1 : 0), mc2::fmt::as_u32((unsigned long)overlayHandle), mc2::fmt::as_u32((unsigned long)e->terrainHandle));
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
            Terrain::mapData->getOverlay(tileR, tileC, decalOverlayType, decalOverlayOffset);
            if (e->isCement() && e->isAlpha() && decalOverlayType == INVALID_OVERLAY) {
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
                static const bool s_runwayFix = mc2::gate::get<"MC2_MISSING_RUNWAY_FIX">();
                bool fragOwnsTile = true;
                if (s_runwayFix && g_cementLayerMapReady && blocks) {
                    auto cemCorner = [&](long r, long c2) -> bool {
                        const size_t vi = (size_t)r * (size_t)mapSide + (size_t)c2;
                        const DWORD  vt = blocks[vi].terrainType;
                        return vt < (DWORD)MC_MAX_TERRAIN_TXMS
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
                        && TileRoadAdjacent8(tileR, tileC, mapSide, blocks))
                        fragOwnsTile = false;
                }
                if (fragOwnsTile) continue;
            }

            const DWORD overlayTexId = tex_resolve(overlayHandle);
            if (overlayTexId == 0) continue;

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
                if (mcTextureManager->tryGetTextureLogicalSize(overlayHandle, ovLogicalW, ovLogicalH))
                {
                    const DWORD ovEdge = ovLogicalW * mcTextureManager->getUVScale(overlayHandle);
                    if (ovEdge > 0)
                    {
                        tileMinU = tileMinV = 0.5f / (float)ovEdge;
                        tileMaxU = tileMaxV = 1.0f - 0.5f / (float)ovEdge;
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
                    if (allInside) continue;   // sidecar owns this tile's overlay
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
                    mc2::fmt::print_out("[TERRAIN_OVERLAY v1] event=decal_corner_probe tileR={:d} tileC={:d} mapSide={:d} half={:d} wupv={:.6f} elev_identical={:d} c0=({:.3f},{:.3f},{:.5f}) c1=({:.3f},{:.3f},{:.5f}) c2=({:.3f},{:.3f},{:.5f}) c3=({:.3f},{:.3f},{:.5f}) elev_raw=({:.5f},{:.5f},{:.5f},{:.5f})\n", mc2::fmt::as_i32(tileR), mc2::fmt::as_i32(tileC), mc2::fmt::as_i32((long)mapSide), mc2::fmt::as_i32((long)half), (double)wupv, mc2::fmt::as_i32(elevIdentical ? 1 : 0), c[0].wx, c[0].wy, c[0].wz, c[1].wx, c[1].wy, c[1].wz, c[2].wx, c[2].wy, c[2].wz, c[3].wx, c[3].wy, c[3].wz, (double)p0->elevation, (double)p1->elevation, (double)p2->elevation, (double)p3->elevation);
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
                t.texId = (uint32_t)overlayTexId;
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
            r.firstVert = (uint32_t)verts.size();
            r.vertCount = 3;
            g_decalDrawRanges.push_back(r);
        }
        verts.push_back(t.v[0]);
        verts.push_back(t.v[1]);
        verts.push_back(t.v[2]);
    }

    g_decalVertCount = (int)verts.size();
#ifdef MC2_VULKAN_ISLAND
    if (mc2_vk_overlay_enabled()) {
        g_decalVertsCpu = verts;
        ++g_decalBakeVersion;
    }
#endif

}

// Mirror RebuildMineStaticVBOIfDirty (no texture-array first-build step —
// cement overlay uses the existing per-mission terrain texture nodes via
// tex_resolve, not a dedicated 2-layer array).
void RebuildDecalStaticVBOIfDirty() {
    if (!g_decalVBODirty) return;
    BuildDecalStaticVBO();
    g_decalVBODirty = false;
}

unsigned int GetDecalStaticVBO_GL() { return g_decalStaticVBO_GL; }
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
        g_decalVertsCpu.size() == (size_t)g_decalVertCount) {
        static_assert(sizeof(DecalDrawRange) == sizeof(Mc2VkOverlayDraw),
                      "DecalDrawRange must layout-match Mc2VkOverlayDraw");
        static_assert(sizeof(DecalVert) == 28, "DecalVert must be the 28B WorldOverlayVert");
        mc2_vk_overlay_submit(0u, g_decalVertsCpu.data(), (uint32_t)g_decalVertsCpu.size(),
                              reinterpret_cast<const Mc2VkOverlayDraw*>(g_decalDrawRanges.data()),
                              (uint32_t)g_decalDrawRanges.size(), g_decalBakeVersion);
        Counters_AddDecalStaticTrisDrawn((long long)(g_decalVertCount / 3));
        return true;
    }
#endif
    return false;

}

}  // namespace gos_terrain_indirect

// C-linkage forwarder for the cement-mutation invalidation sites (code/
// bldng.cpp bridge-destroy + mclib/terrain.cpp Terrain::setOverlay). Mirrors
// gos_terrain_indirect_MarkMineDirty: forward-declared `extern void
// gos_terrain_indirect_MarkDecalDirty();` at the call sites so widely-included
// headers do not need gos_terrain_indirect.h. Idempotent — chain mutations
// debounce to one rebuild/paint via the dirty flag.
void gos_terrain_indirect_MarkDecalDirty() {
    gos_terrain_indirect::MarkDecalDirty();
}
