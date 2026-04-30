// GameOS/gameos/gos_terrain_water_stream.cpp
//
// Stage 2 of the renderWater architectural slice (CPU→GPU offload).
// Builds a static, map-keyed CPU-side recipe array describing every
// water-bearing terrain quad in the mission. Each frame, walks the live
// (camera-windowed) quadList and emits a thin record per in-window water
// quad, looked up by stable map vertexNum → recipe-index hash.
//
// Why the indirection: `Terrain::quadList` is rebuilt every frame as a
// camera-relative window (mapdata.cpp:1072 makeLists), so a "static recipe
// indexed by quadList slot" is invalid by frame 2. The MAP coordinates
// (mapX, mapY) of each vertex are stable; the recipe is keyed by the
// top-left vertex's `vertexNum = mapY*W + mapX` (set at mapdata.cpp:1104).
//
// Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.

#include "gos_terrain_water_stream.h"

#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include <GL/glew.h>

#include "../../mclib/terrain.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/mapdata.h"

namespace WaterStream {

namespace {

std::vector<WaterRecipe> g_recipes;
// vertexNum (= mapY*realVerticesMapSide + mapX of top-left corner) → recipeIdx
std::unordered_map<uint32_t, uint32_t> g_vertexNumToRecipe;
bool g_ready = false;

// Static recipe buffer (uploaded once per mission).
GLuint   g_recipeBuffer = 0;
uint32_t g_recipeBufferUploadedCount = 0;

// Per-frame thin-record ring (mirrors patch_stream thin-record ring).
constexpr uint32_t kThinRingSlots = 3;
GLuint   g_thinBuffer = 0;
uint32_t g_thinSlot = 0;
uint32_t g_thinSlotCapacity = 0;
std::vector<WaterThinRecord> g_thinStaging;

bool s_debugEnabledKnown = false;
bool s_debugEnabled = false;
bool DebugOn() {
    if (!s_debugEnabledKnown) {
        s_debugEnabled = (getenv("MC2_WATER_STREAM_DEBUG") != nullptr);
        s_debugEnabledKnown = true;
    }
    return s_debugEnabled;
}

// Mirrors the file-static `terrainTypeToMaterial` in mclib/quad.cpp.
// Kept here as a duplicate (~3 lookups) so neither this file nor the parity
// helper widens quad.cpp's API surface for what's effectively a pure-function
// lookup table. Behavior must stay in sync; any drift surfaces immediately
// at the parity check's `frgb_lo` field comparison the next time
// PARITY_CHECK runs.
inline uint8_t terrainTypeToMaterialLocal(uint32_t terrainType) {
    switch (terrainType) {
        case 3:  case 8:  case 9:  case 12:           return 1; // Grass
        case 2:  case 4:                              return 2; // Dirt
        case 10: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19:                    return 3; // Concrete
        default:                                      return 0; // Rock
    }
}

} // namespace

void Reset() {
    g_recipes.clear();
    g_recipes.shrink_to_fit();
    g_vertexNumToRecipe.clear();
    g_ready = false;
    g_recipeBufferUploadedCount = 0;
}

void Build() {
    g_recipes.clear();
    g_vertexNumToRecipe.clear();
    g_ready = false;

    // Source of truth: MapData::blocks (PostcompVertex array, mission-static,
    // dimensioned realVerticesMapSide × realVerticesMapSide). Iterate the
    // FULL MAP and build one WaterRecipe per water-bearing quad. World
    // coordinates derived from map indices via mapdata.cpp:1123-1124.
    if (!Terrain::mapData) {
        if (DebugOn()) {
            fprintf(stderr,
                    "[WATER_STREAM v1] event=build_skipped reason=no_mapdata\n");
            fflush(stderr);
        }
        return;
    }

    const PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    if (!blocks) {
        if (DebugOn()) {
            fprintf(stderr,
                    "[WATER_STREAM v1] event=build_skipped reason=no_blocks\n");
            fflush(stderr);
        }
        return;
    }

    const long mapSide  = Terrain::realVerticesMapSide;
    const long halfSide = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;
    const bool has_terrainTextures2 = (Terrain::terrainTextures2 != nullptr);

    auto blockAt = [blocks, mapSide](long mx, long my) -> const PostcompVertex& {
        return blocks[mx + my * mapSide];
    };
    auto worldX = [halfSide, wupv](long mx) -> float {
        return float(mx - halfSide) * wupv;
    };
    auto worldY = [halfSide, wupv](long my) -> float {
        return float(halfSide - my) * wupv;
    };

    g_recipes.reserve((size_t)(mapSide * mapSide / 8));  // rough upper bound
    g_vertexNumToRecipe.reserve((size_t)(mapSide * mapSide / 8));

    long candidates = 0;

    // Quads span (mx, my)..(mx+1, my+1) so the outer loop runs to mapSide-1.
    // The corner ordering matches mapdata.cpp:1175-ish setup of TerrainQuad:
    //   v0 = (mx,   my)     top-left
    //   v1 = (mx+1, my)     top-right
    //   v2 = (mx+1, my+1)   bottom-right
    //   v3 = (mx,   my+1)   bottom-left
    for (long my = 0; my < mapSide - 1; ++my) {
        for (long mx = 0; mx < mapSide - 1; ++mx) {
            ++candidates;
            const PostcompVertex& p0 = blockAt(mx,     my);
            const PostcompVertex& p1 = blockAt(mx + 1, my);
            const PostcompVertex& p2 = blockAt(mx + 1, my + 1);
            const PostcompVertex& p3 = blockAt(mx,     my + 1);

            // Include EVERY map quad in the recipe (no water-bit filter).
            //
            // Why: legacy `setupTextures` sets `waterHandle != 0xffffffff` for
            // any quad where the WATER PLANE PROJECTS INTO THE FRUSTUM (per
            // the `clipped1||clipped2` gate at quad.cpp:963). That's a
            // dynamic, camera-driven criterion — independent of whether the
            // quad has any per-vertex `water & 1` bit set. On flat-but-large
            // missions like mc2_03 / mc2_10 there exist pure-land quads above
            // the water plane where the water plane still projects on-screen
            // and legacy emits semi-transparent (alphaEdge) water vertices.
            //
            // The Stage 2 build used `water & 1` as a recipe-inclusion gate,
            // which fired `lookup_miss` parity mismatches on those missions
            // because `UploadAndBindThinRecords` would silently skip quads
            // with no recipe (mc2_03: 7,131 misses/run; mc2_10: 12,888).
            //
            // Including every map quad lifts recipe SSBO size ~10× (mc2_01
            // 8K → ~80K records, 0.5 → ~5 MB) but keeps the per-frame thin
            // record count unchanged — pz-gate culls non-visible quads at
            // draw time exactly as before. Memory growth is bounded by
            // mapSide² ; well within budget.
            (void)p0; (void)p1; (void)p2; (void)p3;

            const float v0x = worldX(mx);
            const float v0y = worldY(my);
            const float v1x = worldX(mx + 1);
            const float v1y = worldY(my);
            const float v2x = worldX(mx + 1);
            const float v2y = worldY(my + 1);
            const float v3x = worldX(mx);
            const float v3y = worldY(my + 1);

            // The runtime quad uvMode depends on the (mx, my) parity per
            // mapdata.cpp:115 — `((tileR & 1) == (tileC & 1)) ? BOTTOMRIGHT : BOTTOMLEFT`.
            // tileR = my, tileC = mx for our convention.
            const bool isBottomLeft = ((my & 1L) != (mx & 1L));

            WaterRecipe r{};
            r.v0x = v0x; r.v0y = v0y;
            r.v1x = v1x; r.v1y = v1y;
            r.v2x = v2x; r.v2y = v2y;
            r.v3x = v3x; r.v3y = v3y;
            r.v0e = p0.elevation;
            r.v1e = p1.elevation;
            r.v2e = p2.elevation;
            r.v3e = p3.elevation;
            r.quadIdx = (uint32_t)(mx + my * mapSide);  // top-left vertexNum

            uint32_t flags = 0;
            if (isBottomLeft)         flags |= kFlagBitUvModeBottomLeft;
            if (has_terrainTextures2) flags |= kFlagBitHasDetail;
            r.flags = flags;

            const uint32_t t0 = (uint32_t)(uint8_t)p0.terrainType;
            const uint32_t t1 = (uint32_t)(uint8_t)p1.terrainType;
            const uint32_t t2 = (uint32_t)(uint8_t)p2.terrainType;
            const uint32_t t3 = (uint32_t)(uint8_t)p3.terrainType;
            r.terrainTypes = t0 | (t1 << 8) | (t2 << 16) | (t3 << 24);

            const uint32_t w0 = (uint32_t)(p0.water & 0xFFu);
            const uint32_t w1 = (uint32_t)(p1.water & 0xFFu);
            const uint32_t w2 = (uint32_t)(p2.water & 0xFFu);
            const uint32_t w3 = (uint32_t)(p3.water & 0xFFu);
            r.waterBits = w0 | (w1 << 8) | (w2 << 16) | (w3 << 24);

            const uint32_t recipeIdx = (uint32_t)g_recipes.size();
            g_recipes.push_back(r);
            g_vertexNumToRecipe.emplace((uint32_t)(mx + my * mapSide), recipeIdx);
        }
    }

    g_ready = true;

    if (DebugOn()) {
        fprintf(stderr,
                "[WATER_STREAM v1] event=build_done recipes=%zu "
                "candidates_walked=%ld waterElevation=%.3f has_terrainTextures2=%d "
                "mapSide=%ld halfSide=%ld wupv=%.3f\n",
                g_recipes.size(), candidates,
                (double)Terrain::waterElevation,
                has_terrainTextures2 ? 1 : 0,
                mapSide, halfSide, (double)wupv);
        // Dump several recipes spread across the map to spot uniformity bugs.
        const size_t n = g_recipes.size();
        const size_t dumps[] = { 0, n/8, n/4, n/2, n*3/4, n-1 };
        for (size_t di = 0; di < sizeof(dumps)/sizeof(dumps[0]); ++di) {
            if (dumps[di] >= n) continue;
            const WaterRecipe& r = g_recipes[dumps[di]];
            const bool uniform_elev = (r.v0e == r.v1e && r.v1e == r.v2e && r.v2e == r.v3e);
            fprintf(stderr,
                    "[WATER_STREAM v1] event=recipe[%zu] elev=(%.1f,%.1f,%.1f,%.1f) %s "
                    "v0=(%.0f,%.0f) v3=(%.0f,%.0f) quadIdx=%u\n",
                    dumps[di],
                    (double)r.v0e,(double)r.v1e,(double)r.v2e,(double)r.v3e,
                    uniform_elev ? "[UNIFORM]" : "[VARIED]",
                    (double)r.v0x,(double)r.v0y,(double)r.v3x,(double)r.v3y,
                    r.quadIdx);
        }
        fflush(stderr);
    }
}

const WaterRecipe* GetRecipes() {
    return g_recipes.empty() ? nullptr : g_recipes.data();
}

uint32_t GetRecipeCount() {
    return (uint32_t)g_recipes.size();
}

bool IsReady() {
    return g_ready;
}

unsigned int EnsureRecipeBufferUploaded() {
    if (!g_ready || g_recipes.empty())
        return 0;

    if (g_recipeBuffer != 0 && g_recipeBufferUploadedCount == g_recipes.size())
        return g_recipeBuffer;

    const GLsizeiptr bytes = (GLsizeiptr)(g_recipes.size() * sizeof(WaterRecipe));

    if (g_recipeBuffer == 0)
        glGenBuffers(1, &g_recipeBuffer);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeBuffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, g_recipes.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    g_recipeBufferUploadedCount = (uint32_t)g_recipes.size();

    if (DebugOn()) {
        fprintf(stderr,
                "[WATER_STREAM v1] event=recipe_upload bytes=%lld records=%u buf=%u\n",
                (long long)bytes, g_recipeBufferUploadedCount,
                (unsigned)g_recipeBuffer);
        fflush(stderr);
    }

    return g_recipeBuffer;
}

uint32_t UploadAndBindThinRecords() {
    if (!g_ready || g_recipes.empty())
        return 0;

    const TerrainPtr terrainPtr = land;
    const TerrainQuadPtr quads = terrainPtr ? terrainPtr->getQuadList() : nullptr;
    const long total = terrainPtr ? terrainPtr->getNumQuads() : 0;
    if (!quads || total <= 0) return 0;

    // Walk the live (camera-windowed) quadList. For each water-bearing quad,
    // look up its stable recipe by top-left-vertex `vertexNum` and emit a
    // thin record carrying live light/fog/pzValid.
    g_thinStaging.clear();
    g_thinStaging.reserve((size_t)total);

    uint32_t pzValidCount = 0;
    for (long i = 0; i < total; ++i) {
        const TerrainQuad& q = quads[i];
        if (!q.vertices[0] || !q.vertices[1] ||
            !q.vertices[2] || !q.vertices[3]) continue;
        // Skip quads where ANY corner is the map-edge blankVertex (vertexNum < 0).
        // Legacy `setupTextures` reads blankVertex's zero data for those corners
        // and emits degenerate triangles that don't rasterize, but the parity
        // check would surface them as `lookup_miss` against a recipe built
        // only for fully-bounded quads. Matching legacy's effective behavior:
        // treat any-corner-blankVertex as a no-emit case.
        if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;
        // Outer gate: legacy water emit at quad.cpp:2742. Skip non-water quads
        // and quads where setupTextures decided no water emission this frame.
        if (q.waterHandle == 0xffffffffu) continue;

        const uint32_t topLeftVN = (uint32_t)q.vertices[0]->vertexNum;
        auto it = g_vertexNumToRecipe.find(topLeftVN);
        if (it == g_vertexNumToRecipe.end()) continue;

        // Per-triangle pz validity. For each triangle (BOTTOMRIGHT or
        // BOTTOMLEFT diagonal) check that ALL THREE corners' wz ∈ [0,1).
        // wz comes from the water-projected screen.z stored by setupTextures
        // at quad.cpp:715-722. This is THE LOAD-BEARING gate per
        // memory:terrain_tes_projection.md — there is no GPU-side equivalent.
        // Mirrors the legacy per-triangle gVertex.z range check at
        // quad.cpp:2812-2817.
        const float wz0 = q.vertices[0]->wz;
        const float wz1 = q.vertices[1]->wz;
        const float wz2 = q.vertices[2]->wz;
        const float wz3 = q.vertices[3]->wz;
        auto pzOk = [](float z) { return z >= 0.0f && z < 1.0f; };
        const bool ok0 = pzOk(wz0);
        const bool ok1 = pzOk(wz1);
        const bool ok2 = pzOk(wz2);
        const bool ok3 = pzOk(wz3);

        bool pzTri1, pzTri2;
        if (q.uvMode == BOTTOMRIGHT) {
            // tri1=corners[0,1,2], tri2=corners[0,2,3]
            pzTri1 = ok0 && ok1 && ok2;
            pzTri2 = ok0 && ok2 && ok3;
        } else {
            // tri1=corners[0,1,3], tri2=corners[1,2,3]
            pzTri1 = ok0 && ok1 && ok3;
            pzTri2 = ok1 && ok2 && ok3;
        }
        if (!pzTri1 && !pzTri2) continue;  // entire quad fails — drop record

        WaterThinRecord tr{};
        tr.recipeIdx = it->second;
        uint32_t flags = 0;
        if (pzTri1) flags |= kWaterThinFlagPzTri1Valid;
        if (pzTri2) flags |= kWaterThinFlagPzTri2Valid;
        tr.flags = flags;
        ++pzValidCount;
        tr.lightRGB0 = q.vertices[0]->lightRGB;
        tr.lightRGB1 = q.vertices[1]->lightRGB;
        tr.lightRGB2 = q.vertices[2]->lightRGB;
        tr.lightRGB3 = q.vertices[3]->lightRGB;
        // Legacy `drawWater()` patches the LOW byte of each vertex's fogRGB
        // with `terrainTypeToMaterial(terrainType)` before queueing
        // (quad.cpp:2781 etc.). The high 24 bits carry the per-vertex
        // FogValue alpha that the FS samples; the low byte carries the
        // material index. The current water FS only consumes the high byte,
        // but for byte-parity with the legacy `addVertices` arg stream we
        // mirror the patch here. Pure CPU; cost is one switch per vertex.
        const uint32_t m0 = terrainTypeToMaterialLocal((uint32_t)q.vertices[0]->pVertex->terrainType);
        const uint32_t m1 = terrainTypeToMaterialLocal((uint32_t)q.vertices[1]->pVertex->terrainType);
        const uint32_t m2 = terrainTypeToMaterialLocal((uint32_t)q.vertices[2]->pVertex->terrainType);
        const uint32_t m3 = terrainTypeToMaterialLocal((uint32_t)q.vertices[3]->pVertex->terrainType);
        tr.fogRGB0   = (q.vertices[0]->fogRGB & 0xFFFFFF00u) | m0;
        tr.fogRGB1   = (q.vertices[1]->fogRGB & 0xFFFFFF00u) | m1;
        tr.fogRGB2   = (q.vertices[2]->fogRGB & 0xFFFFFF00u) | m2;
        tr.fogRGB3   = (q.vertices[3]->fogRGB & 0xFFFFFF00u) | m3;
        g_thinStaging.push_back(tr);
    }

    const uint32_t thinCount = (uint32_t)g_thinStaging.size();
    if (thinCount == 0) return 0;

    const GLsizeiptr slotBytes = (GLsizeiptr)(thinCount * sizeof(WaterThinRecord));

    // Lazy alloc / grow the ring buffer.
    if (g_thinBuffer == 0 || (uint32_t)slotBytes > g_thinSlotCapacity) {
        if (g_thinBuffer != 0) {
            glDeleteBuffers(1, &g_thinBuffer);
            g_thinBuffer = 0;
        }
        glGenBuffers(1, &g_thinBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
        // Round capacity up to leave headroom (max recipe count is the
        // worst case — every map water quad in the window simultaneously).
        const uint32_t capPerSlot =
            (uint32_t)(g_recipes.size() * sizeof(WaterThinRecord));
        const uint32_t cap = (capPerSlot > (uint32_t)slotBytes)
                              ? capPerSlot : (uint32_t)slotBytes;
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(cap * kThinRingSlots),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        g_thinSlotCapacity = cap;
        g_thinSlot = 0;
    }

    g_thinSlot = (g_thinSlot + 1) % kThinRingSlots;
    const GLintptr slotOffset = (GLintptr)(g_thinSlot * g_thinSlotCapacity);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinBuffer);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, slotOffset, slotBytes,
                    g_thinStaging.data());
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, kWaterThinSsboBinding,
                      g_thinBuffer, slotOffset, slotBytes);

    if (DebugOn()) {
        static uint32_t s_diagFramesPrinted = 0;
        static uint32_t s_diagFrameCounter = 0;
        if (++s_diagFrameCounter >= 1200 && s_diagFramesPrinted < 5) {
            ++s_diagFramesPrinted;
            fprintf(stderr,
                    "[WATER_STREAM v1] event=thin_upload records=%u pz_valid=%u\n",
                    thinCount, pzValidCount);
            fflush(stderr);
        }
    }

    return thinCount;
}

// ----------------------------------------------------------------------------
// Stage 3: parity check
// ----------------------------------------------------------------------------
namespace {

bool s_parityEnabledKnown = false;
bool s_parityEnabled      = false;

uint64_t s_parityFrameCounter   = 0;
uint64_t s_parityQuadsChecked   = 0;
uint64_t s_parityMismatchTotal  = 0;

constexpr uint32_t kMaxMismatchPrintsPerFrame = 16;
constexpr uint64_t kSummaryEveryFrames        = 600;

// Look up a per-corner DWORD-byte from a packed uint (corner 0 in low byte).
inline uint32_t cornerByte(uint32_t packed, uint32_t cornerIdx) {
    return (packed >> (cornerIdx * 8u)) & 0xFFu;
}

// Replicate the legacy alphaMode classifier from quad.cpp:2825-2856.
inline uint32_t legacyAlphaMode(float elev, float waterElevation,
                                 float alphaDepth,
                                 uint32_t alphaEdgeDw, uint32_t alphaMiddleDw,
                                 uint32_t alphaDeepDw)
{
    uint32_t mode = alphaMiddleDw;
    if (elev >= (waterElevation - alphaDepth))             mode = alphaEdgeDw;
    if (elev <= (waterElevation - (alphaDepth * 3.0f)))    mode = alphaDeepDw;
    return mode;
}

// Replicate the wave-displacement modulator from setupTextures' water-projection
// block at quad.cpp:689-700 (and the fast-path VS waveOurCos at
// shaders/gos_terrain_water_fast.vert:133).
inline float legacyWaveOurCos(uint32_t waterBits, float frameCos) {
    if ((waterBits & 0x80u) != 0u) return -frameCos;
    return frameCos;
}

// Per-tri corner index table. uvMode 0 = BOTTOMRIGHT (BR), 1 = BOTTOMLEFT (BL).
//   BR top: 0,1,2  bot: 0,2,3
//   BL top: 0,1,3  bot: 1,2,3
inline uint32_t triCorner(uint32_t uvMode, uint32_t tri, uint32_t vert) {
    if (uvMode == 0) {
        if (tri == 0) { return (vert == 0) ? 0u : (vert == 1) ? 1u : 2u; }
        else          { return (vert == 0) ? 0u : (vert == 1) ? 2u : 3u; }
    } else {
        if (tri == 0) { return (vert == 0) ? 0u : (vert == 1) ? 1u : 3u; }
        else          { return (vert == 0) ? 1u : (vert == 1) ? 2u : 3u; }
    }
}

inline bool pzInRange(float z) { return z >= 0.0f && z < 1.0f; }

// One-shot per-frame mismatch counter for throttling.
struct MismatchPrintBudget {
    uint32_t printed = 0;
    bool canPrint() { return printed < kMaxMismatchPrintsPerFrame; }
    void note()    { ++printed; }
};

inline void printMismatch(MismatchPrintBudget& budget,
                          uint64_t frame, uint32_t quadIdx,
                          const char* layer, uint32_t tri, uint32_t vert,
                          const char* field,
                          uint32_t legacyBits, uint32_t fastBits)
{
    if (!budget.canPrint()) return;
    budget.note();
    fprintf(stderr,
            "[WATER_PARITY v1] event=mismatch frame=%llu quad=%u layer=%s "
            "tri=%u vert=%u field=%s legacy=0x%08x fast=0x%08x\n",
            (unsigned long long)frame, (unsigned)quadIdx,
            layer, (unsigned)tri, (unsigned)vert,
            field, (unsigned)legacyBits, (unsigned)fastBits);
    fflush(stderr);
}

// `screen=0` (recipe) or `screen=1` (thin) field tag for a non-derived field.
inline void printFieldMismatch(MismatchPrintBudget& budget,
                               uint64_t frame, uint32_t quadIdx,
                               const char* scope, uint32_t cornerIdx,
                               const char* field,
                               uint32_t legacyBits, uint32_t fastBits)
{
    if (!budget.canPrint()) return;
    budget.note();
    fprintf(stderr,
            "[WATER_PARITY v1] event=mismatch frame=%llu quad=%u scope=%s "
            "corner=%u field=%s legacy=0x%08x fast=0x%08x\n",
            (unsigned long long)frame, (unsigned)quadIdx,
            scope, (unsigned)cornerIdx, field,
            (unsigned)legacyBits, (unsigned)fastBits);
    fflush(stderr);
}

inline uint32_t bitcastFloatToUint(float f) {
    uint32_t u = 0;
    static_assert(sizeof(u) == sizeof(f), "float must be 32 bits");
    memcpy(&u, &f, sizeof(u));
    return u;
}

} // namespace

void CheckParityFrame(const ParityFrameUniforms& u) {
    if (!s_parityEnabledKnown) {
        s_parityEnabled = (getenv("MC2_RENDER_WATER_PARITY_CHECK") != nullptr);
        s_parityEnabledKnown = true;
        if (s_parityEnabled) {
            fprintf(stderr,
                    "[WATER_PARITY v1] event=enabled note=silent_on_pass "
                    "scope=stock_tier1_only print_budget=%u summary_every=%llu\n",
                    (unsigned)kMaxMismatchPrintsPerFrame,
                    (unsigned long long)kSummaryEveryFrames);
            fflush(stderr);
        }
    }
    if (!s_parityEnabled) return;
    if (!g_ready || g_recipes.empty()) return;

    const uint64_t frame = ++s_parityFrameCounter;
    MismatchPrintBudget budget;

    const TerrainPtr terrainPtr = land;
    const TerrainQuadPtr quads  = terrainPtr ? terrainPtr->getQuadList() : nullptr;
    const long total            = terrainPtr ? terrainPtr->getNumQuads() : 0;
    if (!quads || total <= 0) return;

    // Walk quads in the SAME order UploadAndBindThinRecords uses, so the i-th
    // qualifying quad matches g_thinStaging[i] by construction. No lookup map.
    uint32_t thinIdx = 0;
    const uint32_t thinCount = (uint32_t)g_thinStaging.size();

    for (long i = 0; i < total; ++i) {
        const TerrainQuad& q = quads[i];
        if (!q.vertices[0] || !q.vertices[1] || !q.vertices[2] || !q.vertices[3]) continue;
        // Mirror UploadAndBindThinRecords' all-corners-valid gate; map-edge
        // blankVertex degenerate quads have no recipe and emit garbage in
        // legacy. Treat both paths as no-emit on these quads.
        if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;
        if (q.waterHandle == 0xffffffffu) continue;

        const uint32_t topLeftVN = (uint32_t)q.vertices[0]->vertexNum;
        auto it = g_vertexNumToRecipe.find(topLeftVN);
        if (it == g_vertexNumToRecipe.end()) {
            // Recipe miss: the static recipe didn't classify this quad as
            // water-bearing, but legacy is about to emit. Real bug class.
            printFieldMismatch(budget, frame, topLeftVN,
                               "recipe", 0, "lookup_miss", 1u, 0u);
            continue;
        }

        const WaterRecipe& rec = g_recipes[it->second];

        // Per-vertex pz validity exactly mirrors UploadAndBindThinRecords.
        const float wz0 = q.vertices[0]->wz;
        const float wz1 = q.vertices[1]->wz;
        const float wz2 = q.vertices[2]->wz;
        const float wz3 = q.vertices[3]->wz;
        const bool ok0 = pzInRange(wz0);
        const bool ok1 = pzInRange(wz1);
        const bool ok2 = pzInRange(wz2);
        const bool ok3 = pzInRange(wz3);
        bool pzTri1, pzTri2;
        if (q.uvMode == BOTTOMRIGHT) {
            pzTri1 = ok0 && ok1 && ok2;
            pzTri2 = ok0 && ok2 && ok3;
        } else {
            pzTri1 = ok0 && ok1 && ok3;
            pzTri2 = ok1 && ok2 && ok3;
        }
        if (!pzTri1 && !pzTri2) continue;  // dropped — no thin record

        // Bounds-check thin record before deref.
        if (thinIdx >= thinCount) {
            printFieldMismatch(budget, frame, topLeftVN,
                               "thin", 0, "missing_record", 1u, 0u);
            ++thinIdx;
            continue;
        }
        const WaterThinRecord& trec = g_thinStaging[thinIdx];
        ++thinIdx;
        ++s_parityQuadsChecked;

        // 1. Recipe-input parity ---------------------------------------------
        // Verify recipe carries the same per-corner data that the legacy emit
        // would read from q.vertices[i] / blocks[].
        const VertexPtr v[4] = { q.vertices[0], q.vertices[1],
                                  q.vertices[2], q.vertices[3] };
        const float recVx[4] = { rec.v0x, rec.v1x, rec.v2x, rec.v3x };
        const float recVy[4] = { rec.v0y, rec.v1y, rec.v2y, rec.v3y };
        const float recElev[4] = { rec.v0e, rec.v1e, rec.v2e, rec.v3e };
        for (uint32_t c = 0; c < 4; ++c) {
            if (recVx[c] != v[c]->vx)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c, "vx",
                                   bitcastFloatToUint(v[c]->vx),
                                   bitcastFloatToUint(recVx[c]));
            if (recVy[c] != v[c]->vy)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c, "vy",
                                   bitcastFloatToUint(v[c]->vy),
                                   bitcastFloatToUint(recVy[c]));
            const float legElev = (float)v[c]->pVertex->elevation;
            if (recElev[c] != legElev)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c, "elevation",
                                   bitcastFloatToUint(legElev),
                                   bitcastFloatToUint(recElev[c]));
            const uint32_t recTType = cornerByte(rec.terrainTypes, c);
            const uint32_t legTType = (uint32_t)(uint8_t)v[c]->pVertex->terrainType;
            if (recTType != legTType)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c,
                                   "terrainType", legTType, recTType);
            const uint32_t recWBits = cornerByte(rec.waterBits, c);
            const uint32_t legWBits = (uint32_t)(v[c]->pVertex->water & 0xFFu);
            if (recWBits != legWBits)
                printFieldMismatch(budget, frame, topLeftVN, "recipe", c,
                                   "waterBits", legWBits, recWBits);
        }
        // uvMode parity (recipe.flags bit 0 vs q.uvMode).
        const uint32_t recUvMode = (rec.flags & kFlagBitUvModeBottomLeft) ? 1u : 0u;
        const uint32_t legUvMode = (q.uvMode == BOTTOMRIGHT) ? 0u : 1u;
        if (recUvMode != legUvMode)
            printFieldMismatch(budget, frame, topLeftVN, "recipe", 0, "uvMode",
                               legUvMode, recUvMode);
        // hasDetail parity (recipe.flags bit 1 vs runtime terrainTextures2 presence).
        const uint32_t recHasDetail = (rec.flags & kFlagBitHasDetail) ? 1u : 0u;
        const uint32_t legHasDetail = u.terrainTextures2Present ? 1u : 0u;
        if (recHasDetail != legHasDetail)
            printFieldMismatch(budget, frame, topLeftVN, "recipe", 0, "hasDetail",
                               legHasDetail, recHasDetail);

        // 2. Thin-record parity ---------------------------------------------
        // recipeIdx
        if (trec.recipeIdx != it->second)
            printFieldMismatch(budget, frame, topLeftVN, "thin", 0, "recipeIdx",
                               (uint32_t)it->second, trec.recipeIdx);
        // pz bits
        const uint32_t expectedFlags =
            (pzTri1 ? kWaterThinFlagPzTri1Valid : 0u) |
            (pzTri2 ? kWaterThinFlagPzTri2Valid : 0u);
        if ((trec.flags & (kWaterThinFlagPzTri1Valid | kWaterThinFlagPzTri2Valid))
            != expectedFlags) {
            printFieldMismatch(budget, frame, topLeftVN, "thin", 0, "pz_flags",
                               expectedFlags, trec.flags);
        }
        // per-corner lightRGB / fogRGB
        const uint32_t trLight[4] = { trec.lightRGB0, trec.lightRGB1,
                                       trec.lightRGB2, trec.lightRGB3 };
        const uint32_t trFog[4]   = { trec.fogRGB0, trec.fogRGB1,
                                       trec.fogRGB2, trec.fogRGB3 };
        for (uint32_t c = 0; c < 4; ++c) {
            if (trLight[c] != v[c]->lightRGB)
                printFieldMismatch(budget, frame, topLeftVN, "thin", c, "lightRGB",
                                   v[c]->lightRGB, trLight[c]);
            // The thin record's fogRGB has its low byte patched with
            // terrainTypeToMaterial(terrainType) at upload time so the GPU's
            // emitted gos_VERTEX matches legacy's `(fogRGB & 0xFFFFFF00) |
            // material` per quad.cpp:2781. Parity expectation matches.
            const uint32_t expectFog =
                (v[c]->fogRGB & 0xFFFFFF00u) |
                terrainTypeToMaterialLocal((uint32_t)v[c]->pVertex->terrainType);
            if (trFog[c] != expectFog)
                printFieldMismatch(budget, frame, topLeftVN, "thin", c, "fogRGB",
                                   expectFog, trFog[c]);
        }

        // 3. Derived gos_VERTEX byte parity (u, v, argb, frgb-high-byte) -----
        // Both sides synthesize on CPU using the same uniforms; identity by
        // construction once recipe + thin-record fields above match. Surfaces
        // formula divergence (e.g. cornerIdx mapping bug, uvMode swap, alpha-
        // band classifier ordering bug, MaxMinUV wrap miscompute).
        for (uint32_t tri = 0; tri < 2; ++tri) {
            const bool pzOkTri = (tri == 0) ? pzTri1 : pzTri2;

            // Legacy emit gate (per-tri):
            //   base   : pzOkTri && (alphaMode0+alphaMode1+alphaMode2 != 0)
            //   detail : pzOkTri && useWaterInterestTexture &&
            //            q.waterDetailHandle != 0xffffffff
            // Compute alphaMode sum to mirror legacy gate at quad.cpp:2886.
            const uint32_t triAM[3] = {
                legacyAlphaMode((float)v[triCorner(legUvMode, tri, 0)]->pVertex->elevation,
                                u.waterElevation, u.alphaDepth,
                                u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword),
                legacyAlphaMode((float)v[triCorner(legUvMode, tri, 1)]->pVertex->elevation,
                                u.waterElevation, u.alphaDepth,
                                u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword),
                legacyAlphaMode((float)v[triCorner(legUvMode, tri, 2)]->pVertex->elevation,
                                u.waterElevation, u.alphaDepth,
                                u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword),
            };
            const uint32_t alphaSum = triAM[0] + triAM[1] + triAM[2];
            const bool legBaseEmit   = pzOkTri && (alphaSum != 0u);
            const bool legDetailEmit = pzOkTri && u.useWaterInterestTexture &&
                                       (q.waterDetailHandle != 0xffffffffu);
            // Fast-path side: emits base whenever the per-tri pz bit is set.
            // Detail emits when the recipe.hasDetail bit is set AND the runtime
            // detail handle is bound (per-frame uniform). Per-quad equivalence:
            const bool fastBaseEmit   = pzOkTri;
            const bool fastDetailEmit = pzOkTri && (rec.flags & kFlagBitHasDetail) &&
                                        (u.waterDetailHandleSentinel != 0xffffffffu);
            if (legBaseEmit != fastBaseEmit) {
                printMismatch(budget, frame, topLeftVN, "base", tri, 0,
                              "emit_gate", legBaseEmit ? 1u : 0u, fastBaseEmit ? 1u : 0u);
            }
            if (legDetailEmit != fastDetailEmit) {
                printMismatch(budget, frame, topLeftVN, "detail", tri, 0,
                              "emit_gate", legDetailEmit ? 1u : 0u, fastDetailEmit ? 1u : 0u);
            }
            if (!legBaseEmit && !legDetailEmit) continue;

            // Per-vertex CPU-side synthesis. We compute u, v, argb, frgb-high
            // for each of the 3 verts, in cornerIdx-resolved order.
            // Pre-compute MaxMinUV wrap shift (legacy quad.cpp:2863-2884).
            float uPre[3], vPre[3];
            float uDetPre[3], vDetPre[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t c = triCorner(legUvMode, tri, k);
                uPre[k] = (v[c]->vx - u.mapTopLeftX) * u.oneOverTF + u.cloudOffsetX;
                vPre[k] = (u.mapTopLeftY - v[c]->vy) * u.oneOverTF + u.cloudOffsetY;
                uDetPre[k] = (v[c]->vx - u.mapTopLeftX) * u.oneOverWaterTF + u.sprayOffsetX;
                vDetPre[k] = (u.mapTopLeftY - v[c]->vy) * u.oneOverWaterTF + u.sprayOffsetY;
            }
            // Legacy wrap correction (matches drawWater() block at 2863-2884).
            auto applyWrap = [&](float (&uu)[3], float (&vv)[3], float maxMinUV) {
                if ((uu[0] > maxMinUV) || (vv[0] > maxMinUV) ||
                    (uu[1] > maxMinUV) || (vv[1] > maxMinUV) ||
                    (uu[2] > maxMinUV) || (vv[2] > maxMinUV))
                {
                    float maxU = uu[0]; if (uu[1]>maxU) maxU=uu[1]; if (uu[2]>maxU) maxU=uu[2];
                    float maxV = vv[0]; if (vv[1]>maxV) maxV=vv[1]; if (vv[2]>maxV) maxV=vv[2];
                    maxU = floorf(maxU - (maxMinUV - 1.0f));
                    maxV = floorf(maxV - (maxMinUV - 1.0f));
                    uu[0] -= maxU; uu[1] -= maxU; uu[2] -= maxU;
                    vv[0] -= maxV; vv[1] -= maxV; vv[2] -= maxV;
                }
            };
            float uLeg[3]    = { uPre[0], uPre[1], uPre[2] };
            float vLeg[3]    = { vPre[0], vPre[1], vPre[2] };
            float uLegD[3]   = { uDetPre[0], uDetPre[1], uDetPre[2] };
            float vLegD[3]   = { vDetPre[0], vDetPre[1], vDetPre[2] };
            applyWrap(uLeg, vLeg, u.maxMinUV);
            // Detail layer is memcpy'd from base BEFORE base's wrap shift in
            // legacy (quad.cpp:2897 `memcpy(sVertex, gVertex, ...)` happens
            // AFTER the base wrap-shift block exits the inner if). The detail
            // ARGB is then patched, but UVs are reassigned to detail-scale —
            // detail UVs do NOT inherit the base wrap shift. They compute
            // fresh against oneOverWaterTF + sprayOffset, with NO wrap-shift
            // applied (legacy doesn't apply the MaxMinUV wrap to detail). Fast
            // path mirrors this: detail uses uvScale=oneOverWaterTF + uvOffset
            // = sprayOffset, with the MaxMinUV wrap branch firing only against
            // the detail-derived UVs (which are typically much smaller, so
            // wrap rarely applies). To stay byte-faithful, parity-check the
            // wrap behavior on the detail layer the same way the VS does:
            applyWrap(uLegD, vLegD, u.maxMinUV);

            // Fast-path-equivalent CPU synthesis. Recipe lives in `rec`; per-
            // frame uniforms in `u`. cornerIdx → recipe's stored vx/vy,
            // identical to legacy v[c]->vx/vy when recipe parity holds.
            float uFast[3], vFast[3], uFastD[3], vFastD[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t c   = triCorner(recUvMode, tri, k);
                const float    rvx = (c == 0) ? rec.v0x : (c == 1) ? rec.v1x : (c == 2) ? rec.v2x : rec.v3x;
                const float    rvy = (c == 0) ? rec.v0y : (c == 1) ? rec.v1y : (c == 2) ? rec.v2y : rec.v3y;
                uFast[k]  = (rvx - u.mapTopLeftX) * u.oneOverTF + u.cloudOffsetX;
                vFast[k]  = (u.mapTopLeftY - rvy) * u.oneOverTF + u.cloudOffsetY;
                uFastD[k] = (rvx - u.mapTopLeftX) * u.oneOverWaterTF + u.sprayOffsetX;
                vFastD[k] = (u.mapTopLeftY - rvy) * u.oneOverWaterTF + u.sprayOffsetY;
            }
            applyWrap(uFast, vFast, u.maxMinUV);
            applyWrap(uFastD, vFastD, u.maxMinUV);

            // Derived ARGB (base): legacy = (vertices[c]->lightRGB & 0x00ffffff) | alphaMode_c
            // Fast path equivalent: (thinRec.lightRGB[c] & 0x00FFFFFFu) |
            //                       (alphaByte_from_elev << 24)
            // alphaByte fast path = (alphaModeDword >> 24) & 0xFF for the same
            // band classifier — yields identical DWORD when the legacy
            // alphaMode DWORDs match the bridge-passed bytes (which they do,
            // since the bridge uses (alphaEdge >> 24) & 0xFF etc.).
            // Derived ARGB (detail): legacy = (sVertex.argb & 0xff000000) | 0x00ffffff
            // Fast detail equivalent: (alphaByte << 24) | 0x00FFFFFFu
            // We replicate both formulas and compare.
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t legC  = triCorner(legUvMode, tri, k);
                const uint32_t recC  = triCorner(recUvMode, tri, k);

                // base u/v
                if (bitcastFloatToUint(uLeg[k]) != bitcastFloatToUint(uFast[k]))
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "u",
                                  bitcastFloatToUint(uLeg[k]),
                                  bitcastFloatToUint(uFast[k]));
                if (bitcastFloatToUint(vLeg[k]) != bitcastFloatToUint(vFast[k]))
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "v",
                                  bitcastFloatToUint(vLeg[k]),
                                  bitcastFloatToUint(vFast[k]));
                // detail u/v
                if (bitcastFloatToUint(uLegD[k]) != bitcastFloatToUint(uFastD[k]))
                    printMismatch(budget, frame, topLeftVN, "detail", tri, k, "u",
                                  bitcastFloatToUint(uLegD[k]),
                                  bitcastFloatToUint(uFastD[k]));
                if (bitcastFloatToUint(vLegD[k]) != bitcastFloatToUint(vFastD[k]))
                    printMismatch(budget, frame, topLeftVN, "detail", tri, k, "v",
                                  bitcastFloatToUint(vLegD[k]),
                                  bitcastFloatToUint(vFastD[k]));

                // base argb
                const uint32_t legAlphaMode =
                    legacyAlphaMode((float)v[legC]->pVertex->elevation,
                                    u.waterElevation, u.alphaDepth,
                                    u.alphaEdgeDword, u.alphaMiddleDword, u.alphaDeepDword);
                const uint32_t legArgbBase = (v[legC]->lightRGB & 0x00ffffffu) | (legAlphaMode & 0xff000000u);
                // Fast path: alpha byte derived from elev band (recipe elev),
                // OR'd with low 24 bits of thinRec.lightRGB[c].
                const float    fastElev      = (recC == 0) ? rec.v0e : (recC == 1) ? rec.v1e : (recC == 2) ? rec.v2e : rec.v3e;
                uint32_t       fastAlphaByte = (u.alphaMiddleDword >> 24) & 0xFFu;
                if (fastElev >= (u.waterElevation - u.alphaDepth))
                    fastAlphaByte = (u.alphaEdgeDword >> 24) & 0xFFu;
                if (fastElev <= (u.waterElevation - (u.alphaDepth * 3.0f)))
                    fastAlphaByte = (u.alphaDeepDword >> 24) & 0xFFu;
                const uint32_t fastLight = trLight[recC];
                const uint32_t fastArgbBase = (fastLight & 0x00ffffffu) | (fastAlphaByte << 24);
                if (legArgbBase != fastArgbBase)
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "argb",
                                  legArgbBase, fastArgbBase);

                // detail argb
                const uint32_t legArgbDetail  = (legArgbBase & 0xff000000u) | 0x00ffffffu;
                const uint32_t fastArgbDetail = (fastAlphaByte << 24) | 0x00ffffffu;
                if (legArgbDetail != fastArgbDetail)
                    printMismatch(budget, frame, topLeftVN, "detail", tri, k, "argb",
                                  legArgbDetail, fastArgbDetail);

                // frgb high byte (FogValue) — only consumed byte downstream.
                const uint32_t legFrgbHi  = (v[legC]->fogRGB >> 24) & 0xFFu;
                const uint32_t fastFrgbHi = (trFog[recC]   >> 24) & 0xFFu;
                if (legFrgbHi != fastFrgbHi)
                    printMismatch(budget, frame, topLeftVN, "base", tri, k, "frgb_hi",
                                  legFrgbHi, fastFrgbHi);

                // frgb low byte: legacy patches with terrainTypeToMaterial; the
                // current fast-path FS uses only FogValue (high byte). The low
                // byte is therefore an information-only diff: surface it but
                // tag the field so future readers know it's pixel-irrelevant
                // today. If we ever wire material into the water FS, the
                // mismatch will already be calling this out.
                const uint32_t legFrgbLo =
                    terrainTypeToMaterialLocal((uint32_t)v[legC]->pVertex->terrainType);
                const uint32_t fastFrgbLo = (trFog[recC] & 0xFFu);
                if (legFrgbLo != fastFrgbLo) {
                    // After the UploadAndBindThinRecords fix that ORs material
                    // into the thin record's fogRGB low byte, this should be
                    // silent. If it fires, either the thin-record builder lost
                    // the material patch or quad.cpp's terrainTypeToMaterial
                    // table drifted from the local copy — both worth surfacing.
                    printMismatch(budget, frame, topLeftVN, "base", tri, k,
                                  "frgb_lo", legFrgbLo, fastFrgbLo);
                }
            }
        }

        if (budget.printed > 0) {
            // The first mismatch this frame already incremented; bump the
            // global tally only once per frame to keep the summary line
            // meaningful (counts frames-with-issue, not raw print events).
        }
    }

    if (budget.printed > 0) s_parityMismatchTotal += budget.printed;

    if ((frame % kSummaryEveryFrames) == 0) {
        fprintf(stderr,
                "[WATER_PARITY v1] event=summary frames=%llu "
                "quads_checked=%llu total_mismatches=%llu\n",
                (unsigned long long)frame,
                (unsigned long long)s_parityQuadsChecked,
                (unsigned long long)s_parityMismatchTotal);
        fflush(stderr);
    }
}

// ----------------------------------------------------------------------------

void ReleaseGlResources() {
    if (g_recipeBuffer != 0) {
        glDeleteBuffers(1, &g_recipeBuffer);
        g_recipeBuffer = 0;
    }
    if (g_thinBuffer != 0) {
        glDeleteBuffers(1, &g_thinBuffer);
        g_thinBuffer = 0;
    }
    g_recipeBufferUploadedCount = 0;
    g_thinSlotCapacity = 0;
    g_thinSlot = 0;
    g_thinStaging.clear();
    g_thinStaging.shrink_to_fit();

    if (s_parityEnabled) {
        fprintf(stderr,
                "[WATER_PARITY v1] event=summary frames=%llu "
                "quads_checked=%llu total_mismatches=%llu reason=shutdown\n",
                (unsigned long long)s_parityFrameCounter,
                (unsigned long long)s_parityQuadsChecked,
                (unsigned long long)s_parityMismatchTotal);
        fflush(stderr);
    }
}

} // namespace WaterStream
