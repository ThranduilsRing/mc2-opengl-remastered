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
struct OvlTex {};
struct OvlItem { uint32_t firstVtx, count, kind; OvlTex* tex; };
struct Mc2VkOverlayDraw { uint32_t texHandle, firstVert, vertCount; };
struct OvlStatic {
    uint64_t resolvedVersion = UINT64_MAX, textureGeneration = UINT64_MAX;
    std::vector<OvlItem> items;
    Buffer buffer;
    VkDeviceSize capacity = 0;
    uint64_t version = UINT64_MAX;
    bool used = false;
};
struct OvlState { std::array<OvlStatic, 3> statics; };
struct OvlPending {
    bool isStatic = true;
    uint32_t vertCount = 1;
    uint64_t version = 1;
    const uint8_t* staticPtr = nullptr;
    uint32_t kind = 0;
    std::vector<Mc2VkOverlayDraw> draws;
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
uint64_t textureGeneration = 0;
int resolutions = 0;
bool texturePending = false;
OvlTex texture;
uint64_t gos_GetTextureGeneration() { return textureGeneration; }
OvlTex* ovl_get_tex(TerrainIsland*, OvlState*, uint32_t) {
    ++resolutions;
    return texturePending ? nullptr : &texture;
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
    auto* loadBuffer = f.state.statics[0].buffer.bytes.get();
    f.vertices[0] = 73;
    ++f.pending[0].version;
    REQUIRE(f.prepare());
    CHECK(f.state.statics[0].buffer.bytes.get() == loadBuffer);
    CHECK(f.state.statics[0].buffer.bytes[0] == 73);
    CHECK(allocations == 1);
    CHECK(retirements == 0);
    f.state.statics[0].used = true;
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
    CHECK(f.state.statics[0].version == UINT64_MAX);
    failAllocation = false;
    failMap = true;
    CHECK_FALSE(f.prepare());
    CHECK(f.state.statics[0].version == UINT64_MAX);
    failMap = false;
    REQUIRE(f.prepare());
    CHECK(f.state.statics[0].version == 1);
    CHECK(allocations == 2);
}

TEST_CASE("OverlayPrestage/cement and mines retain independent source versions") {
    UploadFixture f;
    uint8_t mines[kOvlVertStride] = {42};
    f.pending.push_back({true, 1, 1, mines, 2});
    REQUIRE(f.prepare());
    CHECK(allocations == 2);
    auto* cement = f.state.statics[0].buffer.bytes.get();
    f.state.statics[2].used = true;
    mines[0] = 99;
    ++f.pending[1].version;
    REQUIRE(f.prepare());
    CHECK(allocations == 3);
    CHECK(retirements == 1);
    CHECK(f.state.statics[0].buffer.bytes.get() == cement);
    CHECK(f.state.statics[0].version == 1);
    CHECK(f.state.statics[2].version == 2);
    CHECK(f.state.statics[2].buffer.bytes[0] == 99);
}

TEST_CASE("OverlayPrestage/resolved items follow version and texture generation without sorting") {
    UploadFixture f;
    f.pending[0].vertCount = 12;
    f.pending[0].draws = {{7, 6, 6}, {3, 0, 6}};
    texturePending = false;
    resolutions = 0;
    std::vector<OvlItem> items;
    auto resolve = [&] { items.clear(); ovl_resolve_items(&f.island, &f.state, f.pending[0], 0, items); };
    resolve();
    REQUIRE(items.size() == 2);
    CHECK(items[0].firstVtx == 6);
    CHECK(items[1].firstVtx == 0);
    resolve();
    CHECK(resolutions == 2);
    ++f.pending[0].version;
    resolve();
    CHECK(resolutions == 4);
    ++textureGeneration; // CPU texture re-import must invalidate static items
    resolve();
    CHECK(resolutions == 6);
    texturePending = true;
    ++textureGeneration;
    resolve();
    CHECK(items.empty());
    texturePending = false;
    resolve(); // an incomplete import retries even without another generation bump
    CHECK(items.size() == 2);
    CHECK(resolutions == 10);
    f.pending[0].isStatic = false;
    items.clear();
    ovl_resolve_items(&f.island, &f.state, f.pending[0], 12, items);
    REQUIRE(items.size() == 2);
    CHECK(items[0].firstVtx == 18);
    CHECK(items[0].kind == 0);
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

TEST_CASE("OverlayPrestage/HUD mip upload preserves premultiplied pixels and every level") {
    constexpr size_t baseBytes = 2048u * 2048u * 4u;
    for (bool mipmapped : {false, true}) {
        const size_t bytes = mipmapped ? (baseBytes * 4 - 4) / 3 : baseBytes;
        std::vector<uint8_t> rgba(baseBytes), uploaded(bytes);
        for (size_t i = 0; i < baseBytes; i += 4) {
            rgba[i] = 64; rgba[i+1] = 128; rgba[i+2] = 255; rgba[i+3] = 128;
        }
        void* mapped = uploaded.data();
#include "hud_mip_upload.inc"
        const std::array<uint8_t,4> expected = mipmapped
            ? std::array<uint8_t,4>{32,64,128,128} : std::array<uint8_t,4>{64,128,255,128};
        CHECK(std::equal(uploaded.begin(), uploaded.begin()+4, expected.begin()));
        CHECK(std::equal(uploaded.end()-4, uploaded.end(), expected.begin()));
        CHECK(rgba[0] == 64); // immutable input survives premultiplication
    }
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

TEST_CASE("OverlayPrestage/authored transition coverage precedes corner masks without sidecars") {
    // A road can cross any cement-corner arrangement. Its actual alpha art,
    // including the runway middle and pad fence boundary, must survive all 15.
    for (unsigned corners = 0; corners < 15; ++corners) {
        CHECK(roadInputs::TransitionCementWord(7, 3, corners) == 0xc0800007u);
        CHECK(roadInputs::TransitionCementWord(7, 0xffff, corners) == 0xc0800007u);
    }
    // Solid interiors remain opaque; unavailable mod art retains corner fallback.
    CHECK(roadInputs::TransitionCementWord(7, 3, 15) == 0x80000003u);
    for (unsigned corners = 1; corners < 15; ++corners)
        CHECK(roadInputs::TransitionCementWord(0xffff, 3, corners)
              == (0xc0000003u | ((corners - 1u) << 24u)));
    CHECK(roadInputs::TransitionCementWord(0xffff, 3, 0) == 0u);
    CHECK(roadInputs::TransitionCementWord(0xffff, 0xffff, 8) == 0u);
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
