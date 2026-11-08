#include "doctest.h"
#include "../../GameOS/gameos/terrain_vertex_pack.h"
#include "../../mclib/terrain_vertex_dirty.h"
#include <array>
#include <cstring>

namespace {
using namespace gos_terrain_lighting;
using namespace mc2::terrain;
struct Source {
    struct { float x = 0, y = 0, z = 1; } vertexNormal;
    float elevation = 0;
    uint32_t shadow = 0, water = 0;
};
struct Vertex {
    int vertexNum;
    Source* pVertex;
    float vx, vy;
    uint32_t calcThisFrame = 0;
};

// Independent full repack of the old producer's field mapping.
GpuTerrainVertexInput reference(const Vertex& v) {
    GpuTerrainVertexInput out{};
    out.xy[0] = v.vx; out.xy[1] = v.vy;
    out.normal[2] = 1;
    if (v.pVertex) {
        out.elevation = v.pVertex->elevation;
        out.normal[0] = v.pVertex->vertexNormal.x;
        out.normal[1] = v.pVertex->vertexNormal.y;
        out.normal[2] = v.pVertex->vertexNormal.z;
        out.flags = (v.pVertex->shadow ? 1u : 0u) |
            ((v.calcThisFrame & 1u) ? 2u : 0u) |
            ((v.pVertex->water & 1u) ? 16u : 0u) |
            ((v.pVertex->water & 128u) ? 32u : 0u) |
            ((v.pVertex->water & 64u) ? 64u : 0u) |
            ((v.calcThisFrame & 2u) ? 128u : 0u);
    }
    return out;
}

struct Scene {
    static constexpr uint32_t count = 128;
    std::array<Source, count> source{};
    std::vector<Vertex> window;
    TerrainVertexPack pack;
    VertexDirtyFeed feed;
    std::array<VertexUploadRanges, 2> uploads;
    std::array<std::array<GpuTerrainVertexInput, count>, 2> resident{};
    Scene() {
        feed.reset(count); pack.reset(count);
        for (auto& slot : uploads) slot.reset(count);
        for (uint32_t i = 0; i < count; ++i)
            window.push_back({static_cast<int>(i), &source[i], static_cast<float>(i % 16), static_cast<float>(i / 16)});
    }
    size_t frame(uint32_t slot, uint32_t first = 0, uint32_t size = count) {
        for (uint32_t i = first; i < first + size; ++i)
            pack.update(window[i], feed.revision(i));
        size_t bytes = 0;
        for (const auto range : uploads[slot].pending(pack.versions, first, size)) {
            const size_t n = range.count * sizeof(GpuTerrainVertexInput);
            std::memcpy(resident[slot].data() + range.first, pack.vertices.data() + range.first, n);
            uploads[slot].acknowledge(range, pack.versions);
            bytes += n;
        }
        for (uint32_t i = first; i < first + size; ++i) {
            const auto full = reference(window[i]);
            CHECK(std::memcmp(&full, &resident[slot][i], sizeof(full)) == 0);
        }
        return bytes;
    }
};
}

TEST_CASE("Terrain vertex dirty ranges: one change is one range; clean uploads zero bytes") {
    Scene s;
    CHECK(s.frame(0) == Scene::count * sizeof(GpuTerrainVertexInput));
    CHECK(s.frame(1) == Scene::count * sizeof(GpuTerrainVertexInput));
    const auto cleanVersions = s.pack.versions;
    CHECK(s.frame(0) == 0);
    CHECK(s.pack.versions == cleanVersions); // no per-frame candidate repack
    s.source[37].elevation = -18; // local height edit / deformation
    s.feed.mark(37);
    s.pack.update(s.window[37], s.feed.revision(37));
    const auto ranges = s.uploads[0].pending(s.pack.versions, 0, Scene::count);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].first == 37);
    CHECK(ranges[0].count == 1);
    // Merely querying (or a failed upload) does not consume the edit.
    CHECK(s.frame(0) == sizeof(GpuTerrainVertexInput));
    CHECK(s.frame(0) == 0);
    CHECK(s.frame(1) == sizeof(GpuTerrainVertexInput));
    s.source[38].elevation = 9; s.feed.mark(38);
    s.source[39].elevation = 8; s.feed.mark(39);
    CHECK(s.frame(0) == 2 * sizeof(GpuTerrainVertexInput));
    CHECK(s.uploads[1].pending(s.pack.versions, 0, Scene::count).size() == 1);
}

TEST_CASE("Terrain vertex residency equals full repack after random edits and lighting epochs") {
    Scene s;
    s.frame(0); s.frame(1);
    uint32_t random = 0x3268u;
    for (uint32_t step = 0; step < 500; ++step) {
        random = random * 1664525u + 1013904223u;
        const uint32_t i = (random >> 16) % Scene::count;
        auto& p = s.source[i];
        switch (step % 9) {
        case 0: p.elevation += 0.5f; s.feed.mark(i); break;
        case 1: p.vertexNormal.x += 0.125f; s.feed.invalidateAll(); break;
        case 2: p.shadow ^= 1u; s.feed.invalidateAll(); break;
        case 3: p.water ^= 193u; s.feed.invalidateAll(); break;
        case 4: s.window[i].calcThisFrame ^= 3u; break;
        case 5: s.window[i].vx += 1; s.window[i].vy -= 2; break;
        case 6: s.window[i].pVertex = s.window[i].pVertex ? nullptr : &p; break;
        case 7: s.feed.mark(i); s.feed.mark(i); break; // dedup repeated edits
        default: break;
        }
        // Moving windows skip half the map; off-screen source changes survive
        // until re-entry. Alternate slots also miss different intermediate edits.
        const uint32_t first = (step & 1u) * (Scene::count / 2);
        s.frame((step / 3) & 1u, first, Scene::count / 2);
    }
    s.frame(0); s.frame(1);
    CHECK(s.frame(0) == 0);
    CHECK(s.frame(1) == 0);
    // Mission reset at the same map size must discard every previous receipt.
    s.feed.reset(Scene::count); s.pack.reset(Scene::count);
    for (auto& slot : s.uploads) slot.reset(Scene::count);
    CHECK(s.frame(0) == Scene::count * sizeof(GpuTerrainVertexInput));
    CHECK(s.frame(1) == Scene::count * sizeof(GpuTerrainVertexInput));
}
