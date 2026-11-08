// RT scene registry logic (vk_rt_scene.h Vulkan-free half): record keys and ordinals, the drop rules that keep the 24-bit custom index equal to the raster
// object ID, the prop transform into MC2 world, the terrain surface the BLAS traces (coarse and 4x fine) and its chunking, and the packet index rebase.
#include "doctest.h"

#include <vector>

#define MC2_RT_PURE_ONLY
#include "vk_rt_scene.h"

using namespace mc2vk::rt::scene;

namespace {
void model(float tx, float ty, float tz, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    out[3] = tx;
    out[7] = ty;
    out[11] = tz;
}
}  // namespace

TEST_CASE("rt scene: records are keyed by kind, object ID and per-ID ordinal") {
    float a[16], b[16], c[16];
    model(1, 2, 3, a);
    model(4, 5, 6, b);
    model(7, 8, 9, c);
    const PropPlacement props[] = {{7, 0x100002, b}, {3, 0x100001, a}, {7, 0x100002, c}};
    uint32_t dropped = 99;
    const std::vector<Record> r = buildRecords(props, 2, &dropped);
    REQUIRE(r.size() == 5);
    CHECK(dropped == 0);
    CHECK(r[0].key == Key{Kind::Terrain, kTerrainObjectId, 0});
    CHECK(r[1].key == Key{Kind::Terrain, kTerrainObjectId, 1});
    CHECK(r[2].key == Key{Kind::StaticProp, 0x100001, 0});
    CHECK(r[2].typeId == 3);
    CHECK(r[3].key == Key{Kind::StaticProp, 0x100002, 0});
    CHECK(r[3].transform[3] == -4.0f);   // input order decides the ordinal: b first
    CHECK(r[4].key == Key{Kind::StaticProp, 0x100002, 1});
    CHECK(r[4].transform[3] == -7.0f);
    CHECK(r[0].transform[0] == 1.0f);
    CHECK(r[0].transform[5] == 1.0f);
    CHECK(r[0].transform[10] == 1.0f);
    // The same input gives the same keys (stable while the registry is).
    const std::vector<Record> again = buildRecords(props, 2);
    for (size_t i = 0; i < r.size(); ++i) CHECK(again[i].key == r[i].key);
}

TEST_CASE("rt scene: a prop without a recipe, an oversize ID or no matrix is dropped, never aliased") {
    float m[16];
    model(0, 0, 0, m);
    const PropPlacement props[] = {{1, 0, m}, {2, 0x1000000, m}, {3, 0x100003, nullptr}, {4, 0x100004, m}};
    uint32_t dropped = 0;
    const std::vector<Record> r = buildRecords(props, 0, &dropped);
    CHECK(dropped == 3);
    REQUIRE(r.size() == 1);
    CHECK(r[0].key.objectId == 0x100004);
    CHECK(customIndexOf(r[0].key) == 0x100004);   // the custom index is the raster object ID
    CHECK(customIndexFits(0xFFFFFF));
    CHECK_FALSE(customIndexFits(0x1000000));
    CHECK(isMechId(0x10000));
    CHECK(isMechId(0x3FFFF));
    CHECK_FALSE(isMechId(0x100000));   // static props never land in the reserved mover range
    CHECK_FALSE(isMechId(kTerrainObjectId));
}

TEST_CASE("rt scene: the prop transform maps render space (-x, z, y) into MC2 world") {
    float m[16];
    model(10, 20, 30, m);    // render translation (10, 20, 30) = MC2 (-10, 30, 20)
    m[1] = 2.0f;             // render x gets object y
    float t[12];
    propTransform(m, t);
    CHECK(t[3] == -10.0f);
    CHECK(t[7] == 30.0f);
    CHECK(t[11] == 20.0f);
    CHECK(t[0] == -1.0f);
    CHECK(t[1] == -2.0f);
    CHECK(t[6] == 1.0f);    // MC2 y <- render z <- object z
    CHECK(t[9] == 1.0f);    // MC2 z (up) <- render y <- object y
}

TEST_CASE("rt scene: the coarse terrain grid is the vertex heights at 128-unit spacing") {
    const float h[9] = {0, 8, 16, 1, 2, 3, 4, 5, 6};
    TerrainSource t;
    t.mapSide = 3;
    t.halfMap = 128.0f;
    t.heights = h;
    REQUIRE(terrainUsable(t));
    CHECK_FALSE(terrainUsesFine(t));
    CHECK(terrainGridSide(t) == 3);
    CHECK(terrainGridX(t, 0) == -128.0f);
    CHECK(terrainGridX(t, 2) == 128.0f);
    CHECK(terrainGridY(t, 0) == 128.0f);
    CHECK(terrainGridY(t, 2) == -128.0f);
    CHECK(terrainGridHeight(t, 1, 0) == 8.0f);
    CHECK(terrainGridHeight(t, 2, 2) == 6.0f);
    // A bake whose side does not match (mapSide-1)*4+1 is ignored.
    const std::vector<float> fine(8 * 8, 100.0f);
    t.fine = fine.data();
    t.visualSide = 8;
    CHECK_FALSE(terrainUsesFine(t));
    CHECK_FALSE(terrainUsable(TerrainSource{}));
    CHECK(terrainChunksPerSide(TerrainSource{}) == 0);
}

TEST_CASE("rt scene: the fine grid follows hiresNearHeight (coarse triangle, far fade, edge ramp)") {
    const float h[9] = {0, 8, 0, 0, 8, 0, 0, 0, 0};
    const std::vector<float> fine(9 * 9, 100.0f);
    TerrainSource t;
    t.mapSide = 3;
    t.halfMap = 128.0f;
    t.heights = h;
    t.fine = fine.data();
    t.visualSide = 9;
    t.displaceFar = 1.0f;
    REQUIRE(terrainUsesFine(t));
    CHECK(terrainGridSide(t) == 9);
    CHECK(terrainGridX(t, 1) == -96.0f);
    CHECK(terrainGridY(t, 8) == -128.0f);
    CHECK(terrainGridHeight(t, 0, 0) == doctest::Approx(0.0f));     // border: edge ramp 0 -> the coarse surface
    CHECK(terrainGridHeight(t, 2, 0) == doctest::Approx(4.0f));     // u = 0.5 on the h00-h10 edge
    CHECK(terrainGridHeight(t, 4, 4) == doctest::Approx(54.0f));    // hc 8, edge 0.5: 8 + (100 - 8) * 0.5
    t.displaceFar = 0.0f;
    CHECK(terrainGridHeight(t, 4, 4) == doctest::Approx(8.0f));
}

TEST_CASE("rt scene: terrain chunks cover the grid once with the TL-BR diagonal") {
    std::vector<float> h(130 * 130, 0.0f);
    TerrainSource t;
    t.mapSide = 130;
    t.halfMap = 129.0f * 64.0f;
    t.heights = h.data();
    CHECK(terrainChunksPerSide(t) == 3);   // 129 cells = 64 + 64 + 1
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    size_t tris = 0;
    for (uint32_t cy = 0; cy < 3; ++cy)
        for (uint32_t cx = 0; cx < 3; ++cx) {
            buildTerrainChunk(t, cx, cy, pos, idx);
            tris += idx.size() / 3;
            for (uint32_t i : idx) CHECK(i < pos.size() / 3);
        }
    CHECK(tris == size_t(129) * 129 * 2);
    buildTerrainChunk(t, 2, 0, pos, idx);
    CHECK(pos.size() == size_t(2) * 65 * 3);
    CHECK(pos[0] == terrainGridX(t, 128));   // chunks share their border vertices
    buildTerrainChunk(t, 0, 0, pos, idx);
    REQUIRE(idx.size() >= 6);
    CHECK(idx[0] == 0);
    CHECK(idx[1] == 66);
    CHECK(idx[2] == 65);
    buildTerrainChunk(t, 3, 0, pos, idx);
    CHECK(pos.empty());
    CHECK(idx.empty());
}

TEST_CASE("rt scene: the terrain key moves with every surface input") {
    std::vector<float> h(9, 0.0f), fine(81, 1.0f);
    TerrainSource t;
    t.mapSide = 3;
    t.halfMap = 128.0f;
    t.heights = h.data();
    const uint64_t k0 = terrainKey(t);
    CHECK(terrainKey(t) == k0);
    TerrainSource u = t;
    u.heightVersion = 1;
    CHECK(terrainKey(u) != k0);
    u = t;
    u.fine = fine.data();
    u.visualSide = 9;
    const uint64_t kFine = terrainKey(u);
    CHECK(kFine != k0);
    u.displaceFar = 0.5f;
    CHECK(terrainKey(u) != kFine);
    u.displaceFar = 1.0f;
    u.visualVersion = 7;
    CHECK(terrainKey(u) != kFine);
}

TEST_CASE("rt scene: packet indices are rebased onto the shared VBO, bad ranges dropped") {
    const std::vector<uint32_t> ibo = {0, 1, 2, 0, 2, 3, 0, 1};
    std::vector<uint32_t> out;
    const PacketRange ok[] = {{0, 3, 10}, {3, 3, 20}};
    CHECK(appendRebasedIndices(ibo, ok, 24, out) == 2);
    CHECK(out == std::vector<uint32_t>{10, 11, 12, 20, 22, 23});
    out.clear();
    const PacketRange bad[] = {{3, 3, 21}, {6, 3, 0}, {3, 4, 0}};   // index 24 out of range; past the IBO; a partial triangle
    CHECK(appendRebasedIndices(ibo, bad, 24, out) == 1);
    CHECK(out == std::vector<uint32_t>{0, 2, 3});
}
