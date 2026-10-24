// LOCAL-LIGHTS: pool-record packing vs the shader decode, shadow-tile selection, mission light-profile match.
#include "doctest.h"
#include "../../GameOS/gameos/gos_terrain_lighting.h"
#include "../../GameOS/gameos/local_light_shadow.h"
#include "../../mclib/mission_light_profile.h"
#include <cmath>
#include <vector>

using gos_terrain_lighting::GpuTerrainLight;
using gos_terrain_lighting::PackPoolLightType;

namespace {
// The shader's lpa_spotCone / lpa_localShadow read: bits 0-1 type, bit 3 mover lamp, 4-7 tile+1, 8-23 aim, 24-31 cos.
float angleDeg(const float a[3], const float b[3]) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::acos(std::fmax(-1.0f, std::fmin(1.0f, d))) * 57.29578f;
}
GpuTerrainLight lamp(float x, float y, float ax, float ay, float az, bool mover) {
    GpuTerrainLight l{};
    l.position[0] = x; l.position[1] = y; l.position[2] = 120.0f;
    l.color[0] = l.color[1] = l.color[2] = 1.0f;
    l.farDistance = 200.0f; l.oneOverDistance = 1.0f / 200.0f; l.intensity = 0.5f;
    l.lightType = PackPoolLightType(1u, ax, ay, az, 0.8f, 0u, mover);
    return l;
}
}  // namespace

TEST_CASE("PackPoolLightType round-trips through the shader decode") {
    const float aims[][3] = {{0, 0, -1}, {0.349f, 0.268f, -0.898f}, {-0.3f, -0.514f, -0.804f}, {1, 0, 0}, {0, 0.6f, 0.8f}};
    for (const auto& a : aims) {
        const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        const float in[3] = {a[0] / l, a[1] / l, a[2] / l};
        const uint32_t p = PackPoolLightType(2u, in[0], in[1], in[2], 0.83f, 5u, true);
        float out[3];
        mc2localshadow::decodeAim(p, out);
        CHECK(angleDeg(in, out) < 1.5f);
        CHECK((p & 0x3u) == 2u);
        CHECK((p & gos_terrain_lighting::kPoolMoverLampBit) != 0u);
        CHECK(((p >> 4) & 15u) == 5u);
        CHECK(std::fabs(static_cast<float>(p >> 24) / 255.0f - 0.83f) < 0.003f);
    }
    // A coned light never packs a zero cone byte (the shader would read it as omni) and never a zero type word.
    CHECK((PackPoolLightType(1u, 0, 0, -1, 0.0f, 0u) >> 24) != 0u);
    CHECK(mc2localshadow::kMoverLampBit == gos_terrain_lighting::kPoolMoverLampBit);
}

TEST_CASE("selectTiles keeps static lamps in their tile and never bakes mover lamps") {
    for (auto& t : mc2localshadow::g_tiles) t = mc2localshadow::Tile{};
    std::vector<GpuTerrainLight> lights{lamp(0, 0, 0, 0, -1, false), lamp(50, 0, 0, 0, -1, false), lamp(20, 20, 1, 0, -0.2f, true)};
    mc2localshadow::selectTiles(std::span(lights), 0.0f, 0.0f, true);
    int tile[3];
    for (int i = 0; i < 3; ++i) { tile[i] = static_cast<int>((lights[i].lightType >> 4) & 15u) - 1; REQUIRE(tile[i] >= 0); }
    CHECK(mc2localshadow::g_tiles[tile[2]].mover == 1u);
    CHECK(mc2localshadow::g_tiles[tile[0]].mover == 0u);

    // Next frame: the mover lamp moved; the static lamps keep their tiles (their baked casters stay valid).
    std::vector<GpuTerrainLight> next{lamp(0, 0, 0, 0, -1, false), lamp(50, 0, 0, 0, -1, false), lamp(27, 23, 0.9f, 0.1f, -0.2f, true)};
    mc2localshadow::selectTiles(std::span(next), 0.0f, 0.0f, true);
    CHECK(static_cast<int>((next[0].lightType >> 4) & 15u) - 1 == tile[0]);
    CHECK(static_cast<int>((next[1].lightType >> 4) & 15u) - 1 == tile[1]);
    const int moverTile = static_cast<int>((next[2].lightType >> 4) & 15u) - 1;
    REQUIRE(moverTile >= 0);
    CHECK(mc2localshadow::g_tiles[moverTile].mover == 1u);
    CHECK(moverTile != tile[0]);
    CHECK(moverTile != tile[1]);
}

TEST_CASE("selectTiles caps at kTiles nearest and clears when disabled") {
    for (auto& t : mc2localshadow::g_tiles) t = mc2localshadow::Tile{};
    std::vector<GpuTerrainLight> lights;
    for (int i = 0; i < 12; ++i) lights.push_back(lamp(static_cast<float>(i * 100), 0, 0, 0, -1, false));
    mc2localshadow::selectTiles(std::span(lights), 0.0f, 0.0f, true);
    for (int i = 0; i < 12; ++i)
        CHECK((((lights[i].lightType >> 4) & 15u) != 0u) == (i < static_cast<int>(mc2localshadow::kTiles)));
    for (auto& l : lights) l.lightType &= ~0xF0u;
    mc2localshadow::selectTiles(std::span(lights), 0.0f, 0.0f, false);
    for (const auto& l : lights) CHECK(((l.lightType >> 4) & 15u) == 0u);
    for (const auto& t : mc2localshadow::g_tiles) CHECK(t.used == 0u);
}

TEST_CASE("mission light profile matches the base mission and its saves") {
    using mc2_mission_light::matches;
    CHECK(matches("mc2_10", "mc2_10"));
    CHECK(matches("data\\missions\\MC2_10.fit", "mc2_10"));
    CHECK(matches("mc2_10_saved_204523", "mc2_10"));
    CHECK_FALSE(matches("mc2_100", "mc2_10"));
    CHECK_FALSE(matches("mc2_1", "mc2_10"));
    CHECK_FALSE(matches("", "mc2_10"));
}
