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

            const bool waterBearing =
                ((p0.water & 1u) != 0) || ((p1.water & 1u) != 0) ||
                ((p2.water & 1u) != 0) || ((p3.water & 1u) != 0);
            if (!waterBearing) continue;

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
        if (q.vertices[0]->vertexNum < 0) continue;
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
        tr.fogRGB0   = q.vertices[0]->fogRGB;
        tr.fogRGB1   = q.vertices[1]->fogRGB;
        tr.fogRGB2   = q.vertices[2]->fogRGB;
        tr.fogRGB3   = q.vertices[3]->fogRGB;
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
}

} // namespace WaterStream
