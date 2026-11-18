#include "doctest.h"
#include "prop_mesh_cache.h"
#include <cstring>
#include <limits>

namespace {
using namespace mc2_prop::cache;
struct Vec { float x = 0, y = 0, z = 0; };
struct Vertex { Vec position, normal; uint32_t aRGBLight = 0xffffffffu; };
struct Triangle {
    uint32_t Vertices[3] = {0, 1, 2};
    uint32_t localTextureHandle = 0;
    struct { float u0 = 0, v0 = 0, u1 = 1, v1 = 0, u2 = 0, v2 = 1; } uvdata;
};
PropMeshBlob shape(bool card = false) {
    PropMeshBlob b;
    b.vertices.resize(3 * kVertexStride);
    for (size_t i = 0; i < b.vertices.size(); ++i) b.vertices[i] = static_cast<uint8_t>(i);
    b.indices = card ? std::vector<uint32_t>{0, 1, 2, 2, 1, 0} : std::vector<uint32_t>{0, 1, 2, 1, 2, 0};
    b.packets = {{0, 3, 0, card ? 1u : 0u}, {3, 3, 1, card ? 1u : 0u}};
    b.triangles = 2; b.missesBefore = 6; b.missesAfter = 4;
    return b;
}
void word(std::vector<uint8_t>& b, size_t offset, uint64_t value, unsigned n = 8) {
    for (unsigned i = 0; i < n; ++i) b[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void checksum(std::vector<uint8_t>& b) { word(b, 24, mc2::content::fastHash(std::span<const uint8_t>(b).subspan(80))); }
}
TEST_CASE("prop_mesh_cache/roundtrip two textures and self-lit two-sided card") {
    for (bool card : {false, true}) {
        auto b = shape(card);
        auto bytes = encode(123, b);
        auto decoded = decode(123, bytes);
        REQUIRE(decoded);
        CHECK(decoded->vertices == b.vertices);
        CHECK(decoded->indices == b.indices);
        REQUIRE(decoded->packets.size() == 2);
        CHECK(std::memcmp(decoded->packets.data(), b.packets.data(), b.packets.size() * sizeof(Packet)) == 0);
        CHECK(decoded->triangles == b.triangles);
        CHECK(decoded->missesBefore == b.missesBefore);
        CHECK(decoded->missesAfter == b.missesAfter);
        CHECK(encode(123, *decoded) == bytes);
    }
}
TEST_CASE("prop_mesh_cache/bounded reader rejects source header counts and corrupt payload") {
    const auto good = encode(123, shape());
    CHECK_FALSE(decode(124, good));
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16), size_t(24), size_t(80)}) {
        auto bad = good; bad[offset] ^= 1;
        CHECK_FALSE(decode(123, bad));
    }
    for (size_t n = 0; n < good.size(); ++n) CHECK_FALSE(decode(123, std::span<const uint8_t>(good).first(n)));
    for (size_t offset : {size_t(32), size_t(40), size_t(48)}) {
        auto bad = good; word(bad, offset, std::numeric_limits<uint64_t>::max());
        CHECK_FALSE(decode(123, bad)); // no count-dependent allocation is reached
    }
    auto bad = good; word(bad, 32, 119); CHECK_FALSE(decode(123, bad)); // stride alignment
    bad = good; word(bad, 56, 3); CHECK_FALSE(decode(123, bad)); // triangles
    const size_t indices = 80 + 3 * kVertexStride, packets = indices + 24;
    for (auto offset : {indices, packets, packets + 4, packets + 12}) {
        bad = good; word(bad, offset, 0xffffffffu, 4); checksum(bad);
        CHECK_FALSE(decode(123, bad)); // index, noncontiguous range, length, flags
    }
    auto b = shape();
    CHECK(canAppend(b, 0, 0, 0));
    CHECK_FALSE(canAppend(b, 1, 0, 0));
    CHECK_FALSE(canAppend(b, size_t(std::numeric_limits<int32_t>::max()) * kVertexStride, 0, 0));
    CHECK_FALSE(canAppend(b, 0, std::numeric_limits<uint32_t>::max(), 0));
    CHECK_FALSE(canAppend(b, 0, 0, std::numeric_limits<uint32_t>::max()));
    b.indices[0] = 3; CHECK_FALSE(canAppend(b, 0, 0, 0)); CHECK(encode(123, b).empty());
}
TEST_CASE("prop_mesh_cache/key covers every consumed field and both flags independent of allocation") {
    std::vector<Vertex> v(3);
    std::vector<Triangle> t(2);
    auto hash = [&](const auto& vs, const auto& ts, bool card = false, bool alpha = false) {
        return key(std::span<const Vertex>(vs), std::span<const Triangle>(ts), card, alpha);
    };
    const auto original = hash(v, t);
    const auto copyV = v; const auto copyT = t;
    REQUIRE(copyV.data() != v.data());
    CHECK(hash(copyV, copyT) == original);
    auto changedV = v; changedV[0].position.x = 1; CHECK(hash(changedV, t) != original);
    changedV = v; changedV[0].normal.z = 1; CHECK(hash(changedV, t) != original);
    changedV = v; changedV[0].aRGBLight ^= 1; CHECK(hash(changedV, t) != original);
    auto changedT = t; changedT[0].Vertices[1] = 2; CHECK(hash(v, changedT) != original);
    changedT = t; changedT[0].uvdata.v2 = 2; CHECK(hash(v, changedT) != original);
    changedT = t; changedT[0].localTextureHandle = 1; CHECK(hash(v, changedT) != original);
    CHECK(hash(v, t, true) != original); CHECK(hash(v, t, false, true) != original);
    v.pop_back(); CHECK(hash(v, t) != original);
    t.pop_back(); CHECK(hash(copyV, t) != original);
}
