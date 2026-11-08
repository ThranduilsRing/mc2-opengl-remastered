// RT scene registry logic (vk_rt_scene.h Vulkan-free half): record keys and ordinals, the drop rules that keep the 24-bit custom index equal to the raster
// object ID, the prop transform into MC2 world, the terrain surface the BLAS traces (coarse and 4x fine) and its chunking, and the packet index rebase.
#include "doctest.h"

#include <vector>

#define MC2_RT_PURE_ONLY
#include "vk_rt_scene.h"
#include "terrain_diagonal.h"

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
    CHECK(terrainTilesPerSide(TerrainSource{}) == 0);
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

TEST_CASE("rt scene: terrain tiles are 16 coarse intervals, cover the grid once and keep the TL-BR diagonal without a bake") {
    std::vector<float> h(130 * 130, 0.0f);
    TerrainSource t;
    t.mapSide = 130;
    t.halfMap = 129.0f * 64.0f;
    t.heights = h.data();
    CHECK(kTerrainTileWorld == 2048.0f);
    CHECK(terrainTileCells(t) == 16);
    CHECK(terrainTilesPerSide(t) == 9);   // 129 intervals = 8 * 16 + 1
    CHECK(terrainTileId(t, 3, 2) == 21);
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    size_t tris = 0;
    for (uint32_t ty = 0; ty < 9; ++ty)
        for (uint32_t tx = 0; tx < 9; ++tx) {
            buildTerrainTile(t, tx, ty, pos, idx);
            tris += idx.size() / 3;
            for (uint32_t i : idx) CHECK(i < pos.size() / 3);
        }
    CHECK(tris == size_t(129) * 129 * 2);
    buildTerrainTile(t, 8, 0, pos, idx);
    CHECK(pos.size() == size_t(2) * 17 * 3);
    CHECK(pos[0] == terrainGridX(t, 128));   // tiles share their border vertices
    buildTerrainTile(t, 1, 1, pos, idx);
    CHECK(pos[0] == -t.halfMap + kTerrainTileWorld);   // tile origins sit on the 2048-unit world lattice
    CHECK(pos[1] == t.halfMap - kTerrainTileWorld);
    buildTerrainTile(t, 0, 0, pos, idx);
    REQUIRE(idx.size() >= 6);
    CHECK(idx[0] == 0);
    CHECK(idx[1] == 18);
    CHECK(idx[2] == 17);
    buildTerrainTile(t, 9, 0, pos, idx);
    CHECK(pos.empty());
    CHECK(idx.empty());
}

TEST_CASE("rt scene: tile count and IDs do not depend on the visual bake") {
    std::vector<float> h(33 * 33, 0.0f), fine(129 * 129, 0.0f);
    TerrainSource t;
    t.mapSide = 33;
    t.halfMap = 32.0f * 64.0f;
    t.heights = h.data();
    CHECK(terrainTilesPerSide(t) == 2);
    t.fine = fine.data();
    t.visualSide = 129;
    REQUIRE(terrainUsesFine(t));
    CHECK(terrainTilesPerSide(t) == 2);
    CHECK(terrainTileCells(t) == 64);
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    buildTerrainTile(t, 1, 1, pos, idx);
    CHECK(pos[0] == -t.halfMap + kTerrainTileWorld);
    CHECK(pos[1] == t.halfMap - kTerrainTileWorld);
}

TEST_CASE("terrain diagonal: one rule, the flatter diagonal, TL-BR on ties") {
    CHECK_FALSE(mc2terrain::quadSplitsTrBl(0, 0, 0, 0));
    // Saddle: TL / BR low (0, 0.2), TR / BL high (1, 1): TR-BL spread 0 < TL-BR spread 0.2, so TR-BL.
    CHECK(mc2terrain::quadSplitsTrBl(0.0f, 1.0f, 1.0f, 0.2f));
    CHECK_FALSE(mc2terrain::quadSplitsTrBl(1.0f, 0.0f, 0.2f, 1.0f));
}

TEST_CASE("rt scene: a saddle quad in the bake is split like the raster (TR-BL), so the traced interior height matches") {
    // 2x2 coarse map, 5x5 bake. Every fine quad is flat except fine quad (2, 2), a saddle: TL 0, TR 1, BL 1, BR 0.2.
    const float hc[4] = {0, 0, 0, 0};
    std::vector<float> fine(25, 0.0f);
    fine[2 + 2 * 5] = 0.0f;
    fine[3 + 2 * 5] = 1.0f;
    fine[2 + 3 * 5] = 1.0f;
    fine[3 + 3 * 5] = 0.2f;
    TerrainSource t;
    t.mapSide = 2;
    t.halfMap = 64.0f;
    t.heights = hc;
    t.fine = fine.data();
    t.visualSide = 5;
    t.displaceFar = 1.0f;
    REQUIRE(terrainUsesFine(t));
    REQUIRE(terrainTilesPerSide(t) == 1);
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    buildTerrainTile(t, 0, 0, pos, idx);
    REQUIRE(idx.size() == size_t(4) * 4 * 6);
    const size_t q = (2 * 4 + 2) * 6;   // quad (2, 2)
    const uint32_t i00 = 2 * 5 + 2, i10 = i00 + 1, i01 = i00 + 5, i11 = i01 + 1;
    CHECK(idx[q + 0] == i00);
    CHECK(idx[q + 1] == i10);
    CHECK(idx[q + 2] == i01);
    CHECK(idx[q + 3] == i10);
    CHECK(idx[q + 4] == i11);
    CHECK(idx[q + 5] == i01);
    // The raster rule picks the same split on the same corners.
    CHECK(mc2terrain::quadSplitsTrBl(fine[i00], fine[i10], fine[i01], fine[i11]));
    // Height at (0.25, 0.25) inside the quad: on the TL / TR / BL triangle of the TR-BL split, h = TL + (TR - TL) u + (BL - TL) v.
    const float zTL = pos[i00 * 3 + 2], zTR = pos[i10 * 3 + 2], zBL = pos[i01 * 3 + 2];
    const float traced = zTL + (zTR - zTL) * 0.25f + (zBL - zTL) * 0.25f;
    const float raster = fine[i00] + (fine[i10] - fine[i00]) * 0.25f + (fine[i01] - fine[i00]) * 0.25f;
    // RT vertex heights are the blended hiresNearHeight values (coarse 0, so bake x edge ramp: TL 0 x 0.25, TR / BL 1 x 0.125, BR 0.2 x 0.125); the split is
    // the shared one. u + v = 0.5 < 1 puts the point in the TL / TR / BL triangle for both. The TL-BR split would trace 0.00625 there.
    CHECK(raster == doctest::Approx(0.5f));
    CHECK(traced == doctest::Approx(0.0625f));
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

TEST_CASE("rt scene movers: custom index slots stay inside the reserved mech range (24-bit)") {
    REQUIRE(moverCustomIndex(0).has_value());
    CHECK(*moverCustomIndex(0) == kMechIdFirst);
    const std::optional<uint32_t> last = moverCustomIndex(kMoverSlots - 1);
    REQUIRE(last.has_value());
    CHECK(*last == 0x3FFFFu);
    CHECK(isMechId(*last));
    CHECK(*last <= 0xFFFFFFu);
    CHECK_FALSE(moverCustomIndex(kMoverSlots).has_value());
}

TEST_CASE("rt scene movers: Stuff space to MC2 world and the mover transform") {
    const float in[3] = {1.0f, 2.0f, 3.0f};
    float out[3] = {};
    stuffToWorld(in, out);
    CHECK(out[0] == -1.0f);
    CHECK(out[1] == 3.0f);
    CHECK(out[2] == 2.0f);
    float xf[12] = {};
    moverTransform(xf);
    for (int r = 0; r < 3; ++r) {   // the 3x4 applied to a Stuff-space point matches stuffToWorld
        const float v = xf[r * 4 + 0] * in[0] + xf[r * 4 + 1] * in[1] + xf[r * 4 + 2] * in[2] + xf[r * 4 + 3];
        CHECK(v == out[r]);
    }
}

TEST_CASE("rt scene movers: leave rules") {
    CHECK(moverIsHandleKey(0x00100005u));
    CHECK_FALSE(moverIsHandleKey(0x80001234u));
    CHECK(moverLeaves(true, false, 0));                               // a dead handle leaves at once
    CHECK_FALSE(moverLeaves(true, true, kMoverUnseenFrames * 10));    // a live off-screen mech stays
    CHECK_FALSE(moverLeaves(false, false, kMoverUnseenFrames - 1));
    CHECK(moverLeaves(false, false, kMoverUnseenFrames));             // vehicles leave by timeout
}
