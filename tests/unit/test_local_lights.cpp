// LOCAL-LIGHTS: pool-record packing vs the shader decode, shadow-tile selection, mission light-profile match.
#include "doctest.h"
#include "../../GameOS/gameos/gos_terrain_lighting.h"
#include "../../GameOS/gameos/local_light_shadow.h"
#include "../../mclib/mission_light_profile.h"
#include <cmath>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "stock_lamp_fixture.h"
#include "../../mclib/stock_lamp_definition.h"

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
    const float aims[][3] = {{0, 0, -1}, {0.349f, 0.268f, -0.898f}, {-0.3f, -0.514f, -0.804f}, {1, 0, 0}, {0, 0.6f, 0.8f},
                            {0, 1, -0.25f}, {0, -1, -0.25f}, {1, 0, -0.25f}, {-1, 0, -0.25f}};
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

TEST_CASE("stock lamp definitions exclude every authored override") {
    const std::string stock(kStockLampFixture);
    REQUIRE(mc2_stock_lamp::definition("2civliving", stock, false));
    CHECK(mc2_stock_lamp::definition("Data/TGL/2CivLiving.INI", stock, false));
    CHECK_FALSE(mc2_stock_lamp::definition("2civliving", stock, true));
    CHECK_FALSE(mc2_stock_lamp::definition("custom", stock, false));
    for (const char* intensity : {"0.4", "0.5", "1.0"}) {
        for (const char* radius : {"39.0", "40.0", "41.0", "100.0"}) {
            std::string mod = stock;
            auto offset = mod.find("0.07");
            REQUIRE(offset != std::string::npos);
            mod.replace(offset, 4, intensity);
            offset = mod.find("200.0");
            REQUIRE(offset != std::string::npos);
            mod.replace(offset, 5, radius);
            CHECK_FALSE(mc2_stock_lamp::definition("2civliving", mod, false));
        }
    }
}

TEST_CASE("loose stock lamps retune by hash and mod ownership, independent of disk source") {
    const auto source = std::filesystem::path(MC2_REPO_ROOT);
    std::ifstream caller(source / "mclib/bdactor_type.cpp");
    REQUIRE(caller.good());
    const std::string code{std::istreambuf_iterator<char>(caller), {}};
    const auto begin = code.find("stockTerrainLamp = mc2_stock_lamp::definition(");
    REQUIRE(begin != std::string::npos);
    const auto call = code.substr(begin, code.find(';', begin) - begin);
    CHECK(call.find("isLoadedFromDisk") == std::string::npos);
    CHECK(call.find("LookupModOwner(iniName) != nullptr") != std::string::npos);

    const auto loose = std::filesystem::temp_directory_path() / "mc2-stock-lamp-regression.ini";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove(path); } } cleanup{loose};
    for (bool changed : {false, true}) {
        { std::ofstream out(loose, std::ios::binary); out << kStockLampFixture; if (changed) out << "\r\n; authored"; }
        std::ifstream in(loose, std::ios::binary);
        REQUIRE(in.good());
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        CHECK(mc2_stock_lamp::definition("2civliving", bytes, false) == !changed);
        CHECK_FALSE(mc2_stock_lamp::definition("2civliving", bytes, true));
    }
}

TEST_CASE("mission darkness follows night noon night without accumulating color scales") {
    CHECK(mc2_mission_light::darkness(270.0f) == 1.0f);
    CHECK(mc2_mission_light::darkness(90.0f) == 0.0f);
    CHECK(mc2_mission_light::darkness(270.0f) == 1.0f);
    CHECK(mc2_mission_light::darkness(25.0f) == 0.0f);
    CHECK(mc2_mission_light::darkness(20.0f) == 0.0f);
    CHECK(mc2_mission_light::darkness(15.0f) > 0.0f);
    CHECK(mc2_mission_light::nightFromSun(160.0f) == doctest::Approx(1.0f / 3.0f));
}

TEST_CASE("mission profiles keep frontend colors ground and save markers camera owned") {
    struct CameraFixture {
        std::string missionLightName;
        float lightPitch = 22.5f;
        float missionGroundScale = 1.0f;
        bool missionLightApplied = false;
        unsigned char lightRed = 100;
        unsigned char lightGreen = 100;
        unsigned char lightBlue = 100;
        unsigned char dayLightRed = 100;
        unsigned char dayLightGreen = 100;
        unsigned char dayLightBlue = 100;
        unsigned char nightLightRed = 100;
        unsigned char nightLightGreen = 100;
        unsigned char nightLightBlue = 100;
        unsigned char sunsetLightRed = 100;
        unsigned char sunsetLightGreen = 100;
        unsigned char sunsetLightBlue = 100;
        unsigned char ambientRed = 100;
        unsigned char ambientGreen = 100;
        unsigned char ambientBlue = 100;
        unsigned char dayAmbientRed = 100;
        unsigned char dayAmbientGreen = 100;
        unsigned char dayAmbientBlue = 100;
        unsigned char nightAmbientRed = 100;
        unsigned char nightAmbientGreen = 100;
        unsigned char nightAmbientBlue = 100;
        unsigned char terrainShadowRed = 100;
        unsigned char terrainShadowGreen = 100;
        unsigned char terrainShadowBlue = 100;
    };
    using mc2_mission_light::apply;
    using mc2_mission_light::sceneScale;
    CameraFixture coldFrontend;
    apply(coldFrontend, false);
    CHECK(coldFrontend.lightRed == 100);
    CHECK(coldFrontend.ambientRed == 100);
    CHECK(sceneScale(coldFrontend) == 1.0f);
    CHECK_FALSE(coldFrontend.missionLightApplied);
    CameraFixture mission;
    mission.missionLightName = "mc2_10";
    mission.lightPitch = 270;
    apply(mission, false);
    CHECK(mission.lightRed == 45);
    CHECK(mission.ambientRed == 40);
    CHECK(sceneScale(mission) == doctest::Approx(0.55f));
    CHECK(mission.missionLightApplied);
    CameraFixture frontendAfterNight;
    apply(frontendAfterNight, false);
    CHECK(frontendAfterNight.lightRed == coldFrontend.lightRed);
    CHECK(frontendAfterNight.ambientRed == coldFrontend.ambientRed);
    CHECK(sceneScale(frontendAfterNight) == 1.0f);
    CHECK(mission.missionLightApplied);
    auto loaded = mission;
    loaded.lightPitch = 90; // Saved after changing a night mission to noon.
    apply(loaded, mission.missionLightApplied);
    CHECK(loaded.lightRed == 45);
    CHECK(loaded.ambientRed == 40);
    CHECK(loaded.missionLightApplied);
    CHECK(sceneScale(loaded) == 1.0f);
    loaded.lightPitch = 270;
    apply(loaded, loaded.missionLightApplied);
    CHECK(loaded.lightRed == 45);
    CHECK(sceneScale(loaded) == doctest::Approx(0.55f));
    CameraFixture day;
    day.missionLightName = "mc2_01";
    day.lightPitch = 90;
    apply(day, false);
    CHECK(day.lightRed == 100);
    CHECK(day.ambientRed == 100);
    CHECK(sceneScale(day) == 1.0f);
    CHECK_FALSE(day.missionLightApplied);
    day.lightPitch = 270;
    CHECK(sceneScale(day) == doctest::Approx(0.55f));
    CameraFixture modNight;
    modNight.missionLightName = "sudden defend";
    modNight.lightPitch = 270;
    apply(modNight, false);
    CHECK(sceneScale(modNight) == sceneScale(mission));
    modNight.lightPitch = 90;
    CHECK(sceneScale(modNight) == 1.0f);
}

TEST_CASE("mover night shadow keeps a readable floor and leaves the day alone") {
    using namespace mc2_mission_light;
    // mc2_10 full night: the ground under a mech must be at least 25% darker than the lit ground beside it.
    const float night = moverNightFade(darkness(270.0f), 0.45f, 0.40f);
    CHECK(moverShadowFloor(0.4f, night) <= 0.60f);
    CHECK(moverShadowFloor(0.4f, night) >= 0.50f);   // never lighter than a soft shadow, never below the day floor
    // day and evening: the fade is exactly 0, so the day floor is untouched
    CHECK(moverNightFade(darkness(90.0f), 0.45f, 0.40f) == 0.0f);
    CHECK(moverShadowFloor(0.4f, 0.0f) == 0.4f);
    // a dim ambient or a missing profile cannot leave [day floor, 1]
    CHECK(moverNightFade(1.0f, 1.0f, 0.0f) == doctest::Approx(0.175f));
    CHECK(moverNightFade(1.0f, 0.0f, 0.0f) == doctest::Approx(0.25f));
    CHECK(moverNightFade(2.0f, 0.0f, 1.0f) == doctest::Approx(0.325f));
}
