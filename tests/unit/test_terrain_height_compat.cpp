// Verbatim production arithmetic, isolated from renderer services by data-only
// providers. Expected IEEE bits were recorded from bf5ffbab1, before the rewrite.
// This characterizes the query; it does not claim the native mesh is identical.
#include <cmath>
#include <algorithm>
#include <stuff/stuff.hpp>
#include "mathfunc.h"
#include "Gates/gate.h"
#include "Gates/render_options.h"
#include "mc2fmt.h"
#include "doctest.h"
#include "../../tools/mc2_modlint/modlint_input.h"
#include <map>
#include <sstream>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <vector>

#undef gosASSERT
#undef PAUSE
#define gosASSERT(condition) REQUIRE((condition))
#define PAUSE(message) FAIL("query's legacy coordinate assertion fired")

namespace height_compat {
using States = std::map<std::string, std::vector<uint32_t>>;
using Corpus = std::map<std::string, States>;
struct Fixture { std::vector<glm::vec3> positions; Corpus expectedBits; };

Fixture loadFixture(std::istream& input) {
    std::ostringstream text;
    text << input.rdbuf();
    bool ok = false;
    const auto root = parseModlintJson(text.str(), &ok);
    REQUIRE(ok);
    auto need = [](const MlJson& object, std::string_view key) {
        const auto* value = object.find(std::string(key).c_str());
        REQUIRE(value != nullptr);
        return value;
    };
    Fixture fixture;
    for (const auto& position : need(root, "positions")->arr) {
        REQUIRE(position.arr.size() == 3);
        fixture.positions.emplace_back(static_cast<float>(position.arr[0].n),
            static_cast<float>(position.arr[1].n), static_cast<float>(position.arr[2].n));
    }
    for (const auto& [mode, states] : need(root, "expected_bits")->obj) {
        for (const auto& [state, values] : states.obj) {
            auto& bits = fixture.expectedBits[mode][state];
            for (const auto& value : values.arr) {
                REQUIRE(value.type == MlJson::Num);
                REQUIRE((value.n >= 0 && value.n < 4294967296.0));
                REQUIRE(std::floor(value.n) == value.n);
                bits.push_back(static_cast<uint32_t>(value.n));
            }
        }
    }
    REQUIRE(fixture.positions.size() == 143);
    REQUIRE(fixture.expectedBits.size() == 4);
    return fixture;
}
struct TerrainColorMap {
    unsigned char* cpuColorMap = nullptr;
    int cpuColorMapSize = 0;
    unsigned char* cpuDispAlpha = nullptr;
    int cpuDispAlphaSize = 0;
};
struct PostcompVertex { float elevation = 0; glm::vec3 vertexNormal{0, 0, 1}; };
using PostcompVertexPtr = PostcompVertex*;
// Only data layout/providers are substituted; vector math and all query
// expressions, including casts and option caching, come from production.
struct Terrain {
    static constexpr long verticesBlockSide = 20;
    static constexpr float worldUnitsPerVertex = 128.0f;
    static constexpr float oneOverWorldUnitsPerVertex = 1.0f / worldUnitsPerVertex;
    static inline long blocksMapSide = 1, realVerticesMapSide = 20;
    static inline float worldUnitsMapSide = 2560.0f;
    static inline float oneOverWorldUnitsMapSide = 1.0f / worldUnitsMapSide;
    static inline float waterElevation = 0.0f;
    static inline glm::vec3 mapTopLeft3d{-1280, 1280, 0};
    static inline TerrainColorMap* terrainTextures2 = nullptr;
    static bool IsValidTerrainPosition(glm::vec3 position);
    std::function<float(const glm::vec3&)> height;
    float water = 0;
    float getTerrainElevation(const glm::vec3& position) { return height(position); }
    float getWaterElevation() { return water; }
};
#include "terrain_height_valid.inc"
inline Terrain* land = nullptr;
inline float gos_GetTerrainPhongAlpha() { return 0.65f; }
inline float gos_GetTerrainDisplaceScale() { return 2.0f; }
inline float gos_GetTerrainDetailTiling() { return 7.25f; }
constexpr int BOTTOMRIGHT = 0, BOTTOMLEFT = 1;
struct QueryData {
    PostcompVertexPtr blocks = nullptr;
    Stuff::Vector2DOf<float> topLeftVertex{0, 0};
};
#define HEIGHT_QUERY_CLASS struct MapData : QueryData { float terrainElevation(long, long); float terrainElevation(const glm::vec3&); };
// Separate process-static option caches, exactly as separate engine launches
// with CpuPhongRetire true/false and retained/freed CPU art would produce.
namespace defaults {
HEIGHT_QUERY_CLASS
#include "terrain_height_queries.inc"
}
namespace phong {
HEIGHT_QUERY_CLASS
#include "terrain_height_queries.inc"
}
namespace displacement {
HEIGHT_QUERY_CLASS
#include "terrain_height_queries.inc"
}
namespace combined {
HEIGHT_QUERY_CLASS
#include "terrain_height_queries.inc"
}
#undef HEIGHT_QUERY_CLASS
namespace TerrainRuntime {
#include "terrain_height_runtime.inc"
}

template<class Query>
States observe(const Fixture& fixture, bool retirePhong, bool retainArt) {
    auto options = mc2::options::current();
    options.cpuPhongRetire = retirePhong;
    mc2::options::publish(options);
    std::vector<PostcompVertex> vertices(400);
    for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x) {
        auto& v = vertices[y * 20 + x];
        v.elevation = float((x * x * 7 + y * 11 + x * y * 3) % 113) - 23.25f;
        v.vertexNormal = glm::normalize(glm::vec3(float(x % 3) * 0.17f, float(y % 5) * -0.11f, 1));
    }
    Query query;
    query.blocks = vertices.data();
    Terrain terrain;
    terrain.height = [&](const glm::vec3& p) { return query.terrainElevation(p); };
    land = &terrain;
    std::array<unsigned char, 16> color{72, 98, 120, 255, 65, 90, 130, 255,
                                      70, 100, 140, 255, 60, 95, 125, 255};
    std::array<unsigned char, 4> alpha{0, 64, 192, 255};
    TerrainColorMap material{retainArt ? color.data() : nullptr, 2, retainArt ? alpha.data() : nullptr, 2};
    Terrain::terrainTextures2 = &material;
    std::vector<float> saved;
    for (const auto& v : vertices) saved.push_back(v.elevation);
    States result;
    auto bits = [](float value) { return std::bit_cast<uint32_t>(value); };
    for (const auto* state : {"base", "height_edit", "water_change", "reload_height_bytes"}) {
        if (std::string_view(state) == "height_edit") vertices[10 * 20 + 10].elevation -= 21.375f;
        if (std::string_view(state) == "water_change") terrain.water = 47.125f;
        if (std::string_view(state) == "reload_height_bytes") {
            for (size_t i = 0; i < saved.size(); ++i) vertices[i].elevation = saved[i];
            terrain.water = 0;
        }
        std::vector<uint32_t> row;
        for (const auto& p : fixture.positions) {
            const uint32_t expected = bits(query.terrainElevation(p));
            CHECK(bits(TerrainRuntime::sampleGameplayHeight(p)) == expected);
            CHECK(bits(TerrainRuntime::groundElevation(p)) == expected);
            CHECK(bits(TerrainRuntime::sampleVisualHeight(p)) == expected);
            CHECK(bits(TerrainRuntime::decalElevation(p)) == expected);
            CHECK(bits(TerrainRuntime::sampleWaterLevel(p)) == bits(terrain.water));
            query.topLeftVertex.x = -3;
            query.topLeftVertex.y = 5;
            CHECK(bits(query.terrainElevation(p)) == expected);
            query.topLeftVertex.x = query.topLeftVertex.y = 0;
            row.push_back(expected);
        }
        for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x) {
            CHECK(bits(query.terrainElevation(y, x)) == bits(vertices[y * 20 + x].elevation));
            row.push_back(bits(query.terrainElevation(y, x)));
        }
        result[state] = std::move(row);
    }
    CHECK(result["base"] != result["height_edit"]);
    CHECK(result["height_edit"] == result["water_change"]);
    CHECK(result["base"] == result["reload_height_bytes"]);
    land = nullptr;
    Terrain::terrainTextures2 = nullptr;
    CHECK(bits(TerrainRuntime::sampleGameplayHeight({0, 0, 0})) == bits(0.0f));
    CHECK(bits(TerrainRuntime::groundElevation({0, 0, 0})) == bits(0.0f));
    CHECK(bits(TerrainRuntime::decalElevation({0, 0, 0})) == bits(0.0f));
    return result;
}
} // namespace height_compat

TEST_CASE("TerrainHeightCompat/visual bake and near plane ridge") {
    using namespace height_compat;
    Terrain terrain;
    terrain.height = [](const glm::vec3&) { return 7.0f; };
    land = &terrain;
    const auto origin = Terrain::mapTopLeft3d;
    const glm::vec3 eye = origin + glm::vec3(0, -32, 0);
    const glm::vec3 nearOffsets[4] = {{32, 32, -4}, {96, 32, -4}, {32, -32, 4}, {96, -32, 4}};
    TerrainRuntime::setCpuVisualHeight(nullptr, 0);
    CHECK(TerrainRuntime::sampleVisualHeight(eye) == 7.0f);
    CHECK(TerrainRuntime::cameraClearanceHeight(eye, nearOffsets, 10) == 17.0f);
    float bake[16] = {0, 0, 0, 0, 0, 20, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    TerrainRuntime::setCpuVisualHeight(bake, 4);
    const auto midpoint = origin + glm::vec3(48, -32, 0);
    CHECK(TerrainRuntime::sampleVisualHeight(midpoint) == 50.0f);
    CHECK(TerrainRuntime::decalElevation(midpoint) == 50.0f);
    CHECK(TerrainRuntime::sampleGameplayHeight(midpoint) == 7.0f);
    CHECK(TerrainRuntime::groundElevation(midpoint) == 7.0f);
    CHECK(TerrainRuntime::sampleVisualHeight(origin + glm::vec3(96, -32, 0)) == 0.0f);
    CHECK(TerrainRuntime::sampleVisualHeight(origin + glm::vec3(-1, 0, 0)) == 7.0f);
    const float floor = TerrainRuntime::cameraClearanceHeight(eye, nearOffsets, 10);
    CHECK(floor == 94.0f);
    // The ridge at (64,-32) is between corners, and the plane slopes down 4 wu.
    CHECK(floor - 4 >= 80 + 10);
    Terrain::waterElevation = 120;
    CHECK(TerrainRuntime::cameraClearanceHeight(eye, nearOffsets, 10) == 134.0f);
    Terrain::waterElevation = 0;
    TerrainRuntime::setCpuVisualHeight(nullptr, 0);
    CHECK(TerrainRuntime::sampleVisualHeight(midpoint) == 7.0f);
    land = nullptr;
    CHECK(TerrainRuntime::sampleVisualHeight(midpoint) == 0.0f);
}

TEST_CASE("TerrainHeightCompat/production query corpus stays bit identical") {
    using namespace height_compat;
    const auto previousOptions = mc2::options::current();
    std::ifstream input(std::string(MC2_REPO_ROOT) + "/tests/fixtures/terrain_height_compat.json");
    REQUIRE(input.good());
    const Fixture fixture = loadFixture(input);
    Corpus observed;
    observed["default"] = observe<defaults::MapData>(fixture, true, false);
    observed["phong"] = observe<phong::MapData>(fixture, false, false);
    observed["displacement"] = observe<displacement::MapData>(fixture, true, true);
    observed["combined"] = observe<combined::MapData>(fixture, false, true);
    mc2::options::publish(previousOptions);
    CHECK((observed == fixture.expectedBits));
    CHECK(observed["default"] != observed["phong"]);
    CHECK(observed["default"] != observed["displacement"]);
    CHECK(observed["default"]["base"][0] == std::bit_cast<uint32_t>(26.75f));
    // This non-planar cell's bilinear value is 37.375; the legacy diagonal
    // returns about 30.5. A bilinear replacement must fail the corpus.
    CHECK(observed["default"]["base"][2] != std::bit_cast<uint32_t>(37.375f));
}
