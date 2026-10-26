#include "doctest.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <tuple>
#include <vector>
#include <array>

namespace {
using VkDeviceSize = uint64_t;
constexpr int VK_SUCCESS = 0, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 1;
constexpr int kOvlVertStride = 28;
struct Buffer {
    std::unique_ptr<uint8_t[]> bytes;
    explicit operator bool() const { return bool(bytes); }
    void* allocation() const { return bytes.get(); }
};
struct TerrainIsland { int allocator = 0; };
struct OvlState {
    Buffer stat;
    VkDeviceSize statCap = 0;
    uint64_t statVersion = UINT64_MAX;
    bool statUsed = false;
};
struct OvlPending {
    bool isStatic = true;
    uint32_t vertCount = 1;
    uint64_t version = 1;
    const uint8_t* staticPtr = nullptr;
};
int allocations = 0, retirements = 0;
bool failAllocation = false, failMap = false;
bool make_buffer(TerrainIsland*, Buffer& buffer, VkDeviceSize bytes, int, bool, const char*) {
    ++allocations;
    if (failAllocation) return false;
    buffer.bytes = std::make_unique<uint8_t[]>(bytes);
    return true;
}
int vmaMapMemory(int, void* allocation, void** dst) {
    *dst = allocation;
    return failMap ? -1 : VK_SUCCESS;
}
void vmaUnmapMemory(int, void*) {}
namespace mc2vk {
struct Context {
    void retire(Buffer&& buffer, uint64_t) {
        if (buffer) ++retirements;
        buffer = {};
    }
};
Context& getOrCreateEngineVulkanContext() { static Context ctx; return ctx; }
}
#define ZoneScopedN(name)
#include "overlay_static_upload.inc"
#undef ZoneScopedN
struct UploadFixture {
    TerrainIsland island;
    OvlState state;
    uint8_t vertices[kOvlVertStride]{};
    std::vector<OvlPending> pending{{true, 1, 1, vertices}};
    UploadFixture() { allocations = retirements = 0; failAllocation = failMap = false; }
    bool prepare() { return ovl_prepare_static(&island, &state, pending); }
};
}

TEST_CASE("OverlayPrestage/first draw corrects a provisional upload in place") {
    UploadFixture f;
    REQUIRE(f.prepare());
    CHECK(allocations == 1);
    auto* loadBuffer = f.state.stat.bytes.get();
    f.vertices[0] = 73;
    ++f.pending[0].version;
    REQUIRE(f.prepare());
    CHECK(f.state.stat.bytes.get() == loadBuffer);
    CHECK(f.state.stat.bytes[0] == 73);
    CHECK(allocations == 1);
    CHECK(retirements == 0);
    f.state.statUsed = true;
    REQUIRE(f.prepare());
    CHECK(allocations == 1); // unchanged version is never uploaded again
    ++f.pending[0].version;
    REQUIRE(f.prepare());
    CHECK(allocations == 2);
    CHECK(retirements == 1); // the preceding frame retains its buffer until retirement
}

TEST_CASE("OverlayPrestage/failed preparation remains retryable") {
    UploadFixture f;
    failAllocation = true;
    CHECK_FALSE(f.prepare());
    CHECK(f.state.statVersion == UINT64_MAX);
    failAllocation = false;
    failMap = true;
    CHECK_FALSE(f.prepare());
    CHECK(f.state.statVersion == UINT64_MAX);
    failMap = false;
    REQUIRE(f.prepare());
    CHECK(f.state.statVersion == 1);
    CHECK(allocations == 2);
}

TEST_CASE("OverlayPrestage/HUD warm plan covers ten distinct pipeline variants") {
    constexpr uint32_t warmTexture = UINT32_MAX - 1;
#include "hud_warm_variants.inc"
    std::set<std::tuple<uint32_t, bool, uint32_t>> actual;
    for (const auto& variant : variants)
        actual.emplace(variant.kind, variant.texture != 0, variant.alpha);
    CHECK(actual.size() == 10);
    for (uint32_t alpha : {1u, 2u, 3u, 4u}) {
        CHECK(actual.contains({2, false, alpha}));
        CHECK(actual.contains({2, true, alpha}));
    }
    CHECK(actual.contains({1, false, 1}));
    CHECK(actual.contains({1, false, 3}));
}

namespace roadInputs {
using DWORD = uint32_t;
enum Overlays { INVALID_OVERLAY = -1, Road = 7 };
constexpr int MC_MAX_TERRAIN_TXMS = 4;
struct Vertex { DWORD terrainType = 0; };
using PostcompVertexPtr = Vertex*;
std::array<uint16_t, MC_MAX_TERRAIN_TXMS> g_cementLayerIndexBySlot;
struct TextureMetadata {
    bool isCement(DWORD slot) const { return slot == 1; }
    bool isAlpha(DWORD) const { return false; }
};
struct Map {
    bool metadata = true;
    void getOverlay(long r, long c, Overlays& overlay, DWORD&) {
        overlay = metadata && r == 2 && c == 2 ? Road : INVALID_OVERLAY;
    }
    DWORD getTexture(long r, long c) { return r == 2 && c == 2 ? 1 : 0; }
};
struct Terrain {
    static inline Map* mapData;
    static inline TextureMetadata* terrainTextures;
};
#include "overlay_road_inputs.inc"
}

TEST_CASE("OverlayPrestage/load selection matches the first-update metadata purge") {
    using namespace roadInputs;
    Map map;
    TextureMetadata textures;
    Terrain::mapData = &map;
    Terrain::terrainTextures = &textures;
    g_cementLayerIndexBySlot.fill(0xffff);
    g_cementLayerIndexBySlot[1] = 0;
    Vertex blocks[16]{};
    CHECK(TileRoadAdjacent8(1, 1, 4, blocks, true));
    const bool staged = TileRoadAdjacent8(1, 1, 4, blocks, false);
    CHECK_FALSE(staged);
    // TerrainTextures::update discards authored overlay metadata before drawing.
    map.metadata = false;
    CHECK(TileRoadAdjacent8(1, 1, 4, blocks, true) == staged);
    Terrain::mapData = nullptr;
    Terrain::terrainTextures = nullptr;
}
