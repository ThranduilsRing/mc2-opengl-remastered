#include "doctest.h"
#include "cone_beam_links.h"
#include <cstdint>

namespace {
struct Triangle { uint32_t Vertices[3]; };
}

TEST_CASE("cone beam links preserve shared bases, winding and invalid indices") {
    bool apex[8] = {true, false, false, true};
    const Triangle faces[] = {{{0, 1, 2}}, {{2, 3, 1}}, {{0, 4, 1}},
                              {{0, 3, 2}}, {{0, 8, 2}}, {{99, 1, 2}}};
    const auto links = mc2_cone_glow::beamBaseLinks(apex, 5, faces, 6);
    CHECK(links[0].count() == 3);
    CHECK(links[0][1]); CHECK(links[0][2]); CHECK(links[0][4]);
    CHECK(links[3].count() == 2);
    CHECK(links[3][1]); CHECK(links[3][2]);
    CHECK(links[1].none()); CHECK(links[2].none());
}

TEST_CASE("cone beam links agree with the legacy per-apex scan") {
    uint32_t state = 0xC0FFEE;
    auto next = [&] { state = state * 1664525u + 1013904223u; return state; };
    for (int trial = 0; trial < 100; ++trial) {
        bool apex[32] = {};
        for (auto& value : apex) value = (next() & 4u) != 0;
        Triangle faces[100];
        for (auto& face : faces) for (auto& vertex : face.Vertices) vertex = next() % 40;
        const auto links = mc2_cone_glow::beamBaseLinks(apex, 32, faces, 100);
        for (uint32_t a = 0; a < 32; ++a) {
            std::bitset<32> expected;
            if (apex[a]) for (const auto& face : faces) {
                const auto& v = face.Vertices;
                if (v[0] != a && v[1] != a && v[2] != a) continue;
                for (const auto b : v) if (b < 32 && !apex[b]) expected.set(b);
            }
            CHECK(links[a] == expected);
        }
    }
}

TEST_CASE("cone beam links bound the largest supported mesh") {
    bool apex[512] = {};
    apex[511] = true;
    const Triangle faces[] = {{{511, 510, 512}}, {{511, 0, 510}}};
    const auto links = mc2_cone_glow::beamBaseLinks(apex, 512, faces, 2);
    CHECK(links[511].count() == 2);
    CHECK(links[511][0]); CHECK(links[511][510]);
    for (auto& value : apex) value = true;
    const auto allBright = mc2_cone_glow::beamBaseLinks(apex, 512, faces, 2);
    for (const auto& row : allBright) CHECK(row.none());
}
