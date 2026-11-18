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

#include "../../mclib/vk_scene_renderer_overlays_algo.h"

TEST_CASE("OverlayPrestage/overlay event stream equals the legacy scans") {
    using namespace mc2_overlay_algo;
    // Recorded renderer inputs. Values are the production MC2_* flag bits.
    constexpr Masks m{1, 4, 8, 16, 128, 256, 512, 1024, 2048, 4096, 8192};
    struct RecordedNode {
        Node view;
        uint32_t texture, resolvedTexture;
        std::array<uint32_t, 3> vehicleVertices;
    };
    // Independent old scan oracle: no bucket predicates or scheduler helpers.
    const auto old = [&](std::vector<RecordedNode> nodes, bool software) {
        std::vector<Event> events;
        Pass p = Pass::Water;
        const auto e = [&](Op op, int v = 0, int i = -1, uint32_t count = 0) {
            events.push_back({op, p, i, 0, count, v});
        };
        const auto block = [&](Block b, int i = -1) { e(Op::RenderStateBlock, static_cast<int>(b), i); };
        const auto begin = [&](Pass pass) { p = pass; e(Op::PassBegin); };
        const auto reset = [&](int i) { e(Op::ResetVertex, 0, i, nodes[i].view.current); nodes[i].view.current = 0; };
        const auto submit = [&](int i, int alpha = 0) {
            const auto& n = nodes[i].view;
            const uint32_t count = n.current == n.capacity ? n.capacity : n.current;
            e(Op::Submit, alpha, i, count);
            nodes[i].view.current = 0; // The existing submit helper's own reset.
        };
        block(Block::Initial); begin(Pass::Water);
        for (int a = 0; a < 2; ++a) {
            e(Op::AlphaTest, a);
            for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
                const auto& n = nodes[i].view;
                if ((n.flags & 1) && (n.flags & 4) && (n.flags & 1024) == uint32_t(a * 1024) && n.vertices) {
                    if (!(n.flags & 2048) && !(n.flags & 4096)) { reset(i); continue; }
                    block(Block::WaterNode, i); reset(i);
                }
            }
        }
        e(Op::AlphaTest); e(Op::PassEnd);
        begin(Pass::NoUnderlayer); block(Block::NoUnderlayer);
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            const auto& n = nodes[i].view;
            if ((n.flags & 1) && !(n.flags & 4) && (n.flags & 8192) && n.vertices) reset(i);
        }
        e(Op::PassEnd); begin(Pass::Shadow); block(Block::Shadow);
        if (!software) for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            const auto& n = nodes[i].view;
            if ((n.flags & 8) && (n.flags & 4) && n.vertices) { submit(i); reset(i); }
        }
        e(Op::Flush); e(Op::PassEnd); begin(Pass::Alpha); block(Block::Alpha);
        for (int a = 0; a < 2; ++a) {
            e(Op::AlphaTest, a);
            for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
                const auto& n = nodes[i].view;
                if (!(n.flags & 1) && !(n.flags & 8) && !(n.flags & 128) && (n.flags & 4)
                    && (n.flags & 1024) == uint32_t(a * 1024) && n.vertices) { submit(i, a); reset(i); }
            }
        }
        e(Op::AlphaTest); block(Block::AlphaRestore); e(Op::PassEnd);
        begin(Pass::Effects); block(Block::Effects);
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            const auto& n = nodes[i].view;
            if ((n.flags & 16) && n.vertices) { submit(i); reset(i); }
        }
        e(Op::PassEnd); begin(Pass::Spotlight); block(Block::Spotlight);
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            const auto& n = nodes[i].view;
            if ((n.flags & 256) && n.vertices) { submit(i); reset(i); }
        }
        e(Op::Flush); e(Op::PassEnd); begin(Pass::Compass); block(Block::Compass);
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            const auto& n = nodes[i].view;
            if ((n.flags & 128) && n.vertices) reset(i);
        }
        e(Op::PassEnd); begin(Pass::Hud); block(Block::Hud);
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            const auto& n = nodes[i].view;
            if ((n.flags & 512) && n.vertices) reset(i);
        }
        block(Block::End); e(Op::PassEnd);
        return events;
    };
    const auto compare = [&](const std::vector<RecordedNode>& nodes, bool software) {
        const auto expected = old(nodes, software);
        std::vector<Event> actual;
        int reads = 0;
        Schedule(static_cast<int>(nodes.size()), software, m,
            [&](int i) { ++reads; return nodes[i].view; },
            [&](Event e) { actual.push_back(e); });
        CHECK(reads == static_cast<int>(nodes.size()));
        REQUIRE(actual.size() == expected.size());
        for (size_t k = 0; k < expected.size(); ++k) {
            INFO("event ", k);
            CHECK(actual[k] == expected[k]);
        }
        // Expand unchanged helper inputs at each boundary: texture binding,
        // alpha/source accounting, water mode, render-state block identity,
        // initial renderer state and vkLegacyRun frame/fog/viewport inputs.
        // These immutable inputs do not affect membership, but must travel
        // with the same node, range and boundary in the same order.
        const auto inputs = [&](const std::vector<Event>& stream) {
            std::vector<std::array<uint32_t, 16>> trace;
            uint32_t alpha = 0;
            for (const auto& e : stream) {
                if (e.op == Op::AlphaTest) alpha = e.value;
                std::array<uint32_t, 16> row{uint32_t(e.op), uint32_t(e.pass), uint32_t(e.value),
                    e.start, e.count, alpha, uint32_t(software), 0x12345678, 1920, 1080};
                if (e.node >= 0) {
                    const auto& n = nodes[e.node];
                    row[10] = n.texture; row[11] = n.resolvedTexture == 0xffffffffu ? 0u : n.resolvedTexture;
                    row[12] = n.view.flags;
                    uint32_t remaining = e.count;
                    const bool vehicle = e.pass == Pass::Alpha && !(n.view.flags & (256 | 16));
                    for (int c = 0; c < 3; ++c) {
                        row[13 + c] = vehicle ? (remaining < n.vehicleVertices[c] ? remaining : n.vehicleVertices[c]) : 0;
                        remaining -= row[13 + c];
                    }
                }
                trace.push_back(row);
            }
            return trace;
        };
        CHECK(inputs(actual) == inputs(expected));
    };
    compare({}, false); compare({}, true);
    std::vector<RecordedNode> all;
    constexpr uint32_t bits[]{1, 4, 8, 16, 128, 256, 512, 1024, 2048, 4096, 8192};
    // Every overlap, each alpha-test state, null/empty/partial/full vertex
    // ranges, both renderer branches, single nodes and long binding runs.
    for (uint32_t mask = 0; mask < 2048; ++mask) {
        uint32_t flags = 0;
        for (int b = 0; b < 11; ++b) if (mask & (1u << b)) flags |= bits[b];
        for (uint32_t range = 0; range < 4; ++range) {
            RecordedNode n{{flags, range != 0, 12, range == 3 ? 12u : range == 2 ? 6u : 0u},
                mask % 7, mask % 3 ? mask + 100 : 0xffffffffu, {3, 6, 3}};
            compare({n}, false); compare({n}, true);
            all.push_back(n);
        }
    }
    compare(all, false); compare(all, true);
    // Schedule keeps its scratch across calls: populated -> empty -> populated must match the oracle with no stale rows.
    compare(all, false); compare({}, false); compare(all, false);
}
