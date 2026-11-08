// vk_rt_scene_math.cpp -- the Vulkan-free half of the RT scene (vk_rt_scene.h): record keys, the prop transform, the terrain surface mesh and the packet
// index rebase. Unit-tested without a Vulkan link (tests/unit/test_vk_rt_scene.cpp).
#define MC2_RT_PURE_ONLY
#include "vk_rt_scene.h"
#include "terrain_diagonal.h"

#include <algorithm>
#include <bit>
#include <map>

namespace mc2vk::rt::scene {

void propTransform(const float m[16], float out[12]) {
    // Render space (rx, ry, rz) = (-x, z, y) of MC2 world; the model matrix maps object -> render space row by row, so MC2 world x = -row0, y = row2,
    // z = row1.
    for (int c = 0; c < 4; ++c) {
        out[0 + c] = -m[0 + c];
        out[4 + c] = m[8 + c];
        out[8 + c] = m[4 + c];
    }
}

void identityTransform(float out[12]) {
    static constexpr float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::copy(kIdentity, kIdentity + 12, out);
}

std::vector<Record> buildRecords(std::span<const PropPlacement> props, uint32_t terrainChunks, uint32_t* dropped) {
    std::vector<Record> out;
    out.reserve(terrainChunks + props.size());
    for (uint32_t i = 0; i < terrainChunks; ++i) {
        Record r;
        r.key = Key{Kind::Terrain, kTerrainObjectId, i};
        identityTransform(r.transform);
        out.push_back(r);
    }
    uint32_t drop = 0;
    std::map<uint32_t, uint32_t> nextOrdinal;
    for (const PropPlacement& p : props) {
        if (p.objectId == kTerrainObjectId || !customIndexFits(p.objectId) || p.model == nullptr) {
            ++drop;
            continue;
        }
        Record r;
        r.key = Key{Kind::StaticProp, p.objectId, nextOrdinal[p.objectId]++};
        r.typeId = p.typeId;
        propTransform(p.model, r.transform);
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) { return a.key < b.key; });
    if (dropped) *dropped = drop;
    return out;
}

bool terrainUsable(const TerrainSource& t) { return t.mapSide >= 2 && t.heights != nullptr; }

bool terrainUsesFine(const TerrainSource& t) { return terrainUsable(t) && t.fine != nullptr && t.visualSide == (t.mapSide - 1) * 4 + 1; }

uint32_t terrainGridSide(const TerrainSource& t) {
    if (!terrainUsable(t)) return 0;
    return static_cast<uint32_t>(terrainUsesFine(t) ? t.visualSide : t.mapSide);
}

namespace {
float coarseAt(const TerrainSource& t, int32_t x, int32_t y) {
    x = std::clamp(x, 0, t.mapSide - 1);
    y = std::clamp(y, 0, t.mapSide - 1);
    return t.heights[size_t(x) + size_t(y) * size_t(t.mapSide)];
}
}  // namespace

float terrainGridHeight(const TerrainSource& t, int32_t gx, int32_t gy) {
    if (!terrainUsable(t)) return 0.0f;
    if (!terrainUsesFine(t)) return coarseAt(t, gx, gy);
    // terrain_tier1.vert hiresNearHeight, interior path: the coarse triangle under the fine vertex, blended toward the bake by the far fade and an edge ramp.
    const int32_t maxF = t.visualSide - 1;
    gx = std::clamp(gx, 0, maxF);
    gy = std::clamp(gy, 0, maxF);
    const int32_t cx = std::min(gx / 4, t.mapSide - 2);
    const int32_t cy = std::min(gy / 4, t.mapSide - 2);
    const float u = float(gx - cx * 4) * 0.25f;
    const float v = float(gy - cy * 4) * 0.25f;
    const float h00 = coarseAt(t, cx, cy), h10 = coarseAt(t, cx + 1, cy), h01 = coarseAt(t, cx, cy + 1), h11 = coarseAt(t, cx + 1, cy + 1);
    const float hc = (v >= u) ? h00 + (h11 - h01) * u + (h01 - h00) * v : h00 + (h10 - h00) * u + (h11 - h10) * v;
    const float fine = t.fine[size_t(gx) + size_t(gy) * size_t(t.visualSide)];
    const float edge = std::clamp(float(std::min({gx, gy, maxF - gx, maxF - gy})) * 0.125f, 0.0f, 1.0f);
    const float w = std::clamp(t.displaceFar, 0.0f, 1.0f) * edge;
    return hc + (fine - hc) * w;
}

float terrainGridX(const TerrainSource& t, int32_t gx) { return float(gx) * (terrainUsesFine(t) ? 32.0f : 128.0f) - t.halfMap; }
float terrainGridY(const TerrainSource& t, int32_t gy) { return t.halfMap - float(gy) * (terrainUsesFine(t) ? 32.0f : 128.0f); }

uint32_t terrainTilesPerSide(const TerrainSource& t) {
    if (!terrainUsable(t)) return 0;
    return (static_cast<uint32_t>(t.mapSide - 1) + kTerrainTileCoarse - 1) / kTerrainTileCoarse;
}

uint32_t terrainTileCells(const TerrainSource& t) { return kTerrainTileCoarse * (terrainUsesFine(t) ? 4u : 1u); }

void buildTerrainTile(const TerrainSource& t, uint32_t tx, uint32_t ty, std::vector<float>& positions, std::vector<uint32_t>& indices) {
    positions.clear();
    indices.clear();
    const uint32_t per = terrainTilesPerSide(t);
    if (tx >= per || ty >= per) return;
    const uint32_t cells = terrainGridSide(t) - 1, tileCells = terrainTileCells(t);
    const uint32_t x0 = tx * tileCells, y0 = ty * tileCells;
    const uint32_t nx = std::min(tileCells, cells - x0), ny = std::min(tileCells, cells - y0);
    const bool fine = terrainUsesFine(t);
    const size_t pitch = static_cast<size_t>(t.visualSide);
    auto raw = [&](size_t gx, size_t gy) { return t.fine[gx + gy * pitch]; };
    const uint32_t vx = nx + 1;
    positions.reserve(size_t(vx) * (ny + 1) * 3);
    for (uint32_t y = 0; y <= ny; ++y)
        for (uint32_t x = 0; x <= nx; ++x) {
            const int32_t gx = int32_t(x0 + x), gy = int32_t(y0 + y);
            positions.push_back(terrainGridX(t, gx));
            positions.push_back(terrainGridY(t, gy));
            positions.push_back(terrainGridHeight(t, gx, gy));
        }
    indices.reserve(size_t(nx) * ny * 6);
    for (uint32_t y = 0; y < ny; ++y)
        for (uint32_t x = 0; x < nx; ++x) {
            const uint32_t i00 = y * vx + x, i10 = i00 + 1, i01 = i00 + vx, i11 = i01 + 1;
            const uint32_t gx = x0 + x, gy = y0 + y;
            if (fine && mc2terrain::quadSplitsTrBl(raw(gx, gy), raw(gx + 1, gy), raw(gx, gy + 1), raw(gx + 1, gy + 1)))
                indices.insert(indices.end(), {i00, i10, i01, i10, i11, i01});   // TR-BL, same winding
            else
                indices.insert(indices.end(), {i00, i11, i01, i00, i10, i11});   // TL-BR
        }
}

uint64_t terrainKey(const TerrainSource& t) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(uint64_t(uint32_t(t.mapSide)));
    mix(std::bit_cast<uint32_t>(t.halfMap));
    mix(t.heightVersion);
    mix(terrainUsesFine(t) ? 1 : 0);
    mix(t.visualVersion);
    mix(uint64_t(uint32_t(t.visualSide)));
    mix(std::bit_cast<uint32_t>(t.displaceFar));
    return h;
}

uint32_t appendRebasedIndices(std::span<const uint32_t> ibo, std::span<const PacketRange> packets, uint32_t vertexCount, std::vector<uint32_t>& out) {
    uint32_t tris = 0;
    for (const PacketRange& p : packets) {
        if (uint64_t(p.firstIndex) + p.indexCount > ibo.size()) continue;
        const uint32_t whole = p.indexCount / 3 * 3;
        for (uint32_t i = 0; i < whole; i += 3) {
            const uint64_t a = uint64_t(ibo[p.firstIndex + i]) + p.baseVertex;
            const uint64_t b = uint64_t(ibo[p.firstIndex + i + 1]) + p.baseVertex;
            const uint64_t c = uint64_t(ibo[p.firstIndex + i + 2]) + p.baseVertex;
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
            out.insert(out.end(), {uint32_t(a), uint32_t(b), uint32_t(c)});
            ++tris;
        }
    }
    return tris;
}

}  // namespace mc2vk::rt::scene
