// tests/unit/test_vfx_draw_plan.cpp
// VFX island draw packing: stable primitive order, descriptor-indexed pages and blend equivalence audit.
#include "doctest.h"
#include "vfx_draw_plan.h"
#include "vfx_native_batch.h"
#include "vulkan_vfx_geometry.h"
#include <array>
#include <cmath>
#include "fx/fx_render.h"
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace mc2vfx;

namespace {
PlanItem item(uint32_t src, uint32_t count, uint32_t blend, uint64_t page, uint32_t tag) {
    PlanItem i;
    i.src = src; i.count = count; i.blend = blend; i.page = page; i.atlas = 0xffffffffu; i.tag = tag;
    return i;
}

namespace fx = mc2::fx;
namespace {
fx::Material authored(uint32_t alpha, bool clamp = false, const char* texture = "") {
    fx::Material m;
    m.words = {1u | (alpha << 14) | (clamp ? 1u << 21 : 0), 0xffffffffu, 0xffffffffu, 0, 0xffffffffu, 0xffffffffu};
    m.texture = texture; return m;
}
fx::MeshShape triangle(float x) {
    fx::MeshPrimitive p;
    p.class_id = 101;
    p.positions = {{x, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    p.indices = {0, 1, 2}; p.texcoords = {{0, 0}, {1, 0}, {0, 1}};
    p.colors = {0xff804020u, 0xffffffffu, 0xffffffffu};
    p.materials = {authored(0, true, "base"), authored(4, false, "overlay")};
    p.extra_texcoords = {{{0.25f, 0.5f}, {0.75f, 0.5f}, {0.25f, 1}}};
    fx::MeshShape s; s.primitives.push_back(std::move(p)); return s;
}
}
TEST_CASE("fx_render/reload at the same address invalidates mesh and material identities") {
    fx::RenderResources resources;
    fx::FramePackets frame;
    std::optional<fx::MeshShape> owner(triangle(1));
    fx::Geometry g; g.kind = fx::GeometryKind::mesh; g.shape = &*owner;
    const auto first = resources.mesh(*owner);
    resources.append(std::span(&g, 1), frame);
    const auto material = frame.draws.front().material;
    CHECK(frame.vertices.front().position[0] == 1);
    const auto* address = &*owner;
    const auto generation = resources.generation();
    resources.reset(); frame.clear();
    owner.emplace(triangle(9));
    REQUIRE(&*owner == address);
    CHECK(resources.generation() != generation);
    CHECK(resources.mesh(*owner) != first);
    CHECK(resources.material(material) == nullptr);
    resources.append(std::span(&g, 1), frame);
    CHECK(frame.vertices.front().position[0] == 9);
    CHECK(frame.draws.front().material != material);
}
TEST_CASE("fx_render/compact indexed packets retain colors clamp and multipass order") {
    fx::RenderResources resources;
    fx::FramePackets frame;
    const auto shape = triangle(2);
    fx::Geometry mesh; mesh.kind = fx::GeometryKind::mesh; mesh.shape = &shape;
    mesh.color = {0.5f, 1, 1, 0.75f}; mesh.scale = {2, 3, 4};
    resources.append(std::span(&mesh, 1), frame);
    REQUIRE(frame.draws.size() == 2);
    REQUIRE(frame.vertices.size() == 6);
    CHECK(sizeof(fx::RenderVertex) == 40);
    CHECK(sizeof(fx::RenderDraw) == 16);
    CHECK(frame.vertices[0].position[0] == 4);
    CHECK(frame.vertices[0].color[0] == doctest::Approx(0.5f * 128 / 255));
    CHECK(frame.vertices[0].color[3] == 0.75f);
    CHECK((frame.vertices[0].flags & 16u) != 0);
    CHECK((frame.vertices[3].flags & 16u) == 0);
    CHECK(frame.vertices[3].uv[0] == 0.25f);
    CHECK(resources.material(frame.draws[0].material)->blend == 3);
    CHECK(resources.material(frame.draws[1].material)->blend == 0);
    CHECK(frame.indices == std::vector<uint32_t>{0, 1, 2, 3, 4, 5});
    const auto capacity = frame.vertices.capacity();
    frame.clear();
    CHECK(frame.vertices.capacity() == capacity);
    fx::Geometry tube;
    tube.vertices = {{{0, 0, 0}, {1, 1, 1, 1}, {0, 0}}, {{1, 0, 0}, {1, 1, 1, 1}, {1, 0}},
        {{1, 1, 0}, {1, 1, 1, 1}, {1, 1}}, {{0, 1, 0}, {1, 1, 1, 1}, {0, 1}}};
    tube.indices = {0, 1, 2, 2, 3, 0};
    resources.append(std::span(&tube, 1), frame);
    CHECK(frame.vertices.size() == 4); CHECK(frame.indices.size() == 6);
}
TEST_CASE("fx_render/material recipes compose permissions and resolve once per frame") {
    constexpr uint32_t recipes[] = {3, 2, 1, 4, 0, 5};
    fx::RenderResources resources;
    fx::FramePackets frame;
    for (uint32_t alpha = 0; alpha < 6; ++alpha) {
        const auto m = authored(alpha, alpha % 2 != 0, "page");
        const auto mat = fx::render_material(m);
        CHECK(mat.blend == recipes[alpha]); CHECK(mat.clamp == (alpha % 2 != 0));
        const auto id = resources.intern(mat);
        CHECK(resources.intern(mat) == id);
        frame.draws.push_back({id, 0, 3}); frame.draws.push_back({id, 3, 3});
    }
    uint32_t calls = 0;
    std::map<fx::MaterialId, uint32_t> textures;
    const auto resolve = [](const fx::RenderMaterial&, void* count) { ++*static_cast<uint32_t*>(count); return 99u; };
    fx::resolve_materials(resources, frame, resolve, &calls, textures);
    CHECK(calls == 6); CHECK(textures.size() == 6);
    fx::resolve_materials(resources, frame, resolve, &calls, textures);
    CHECK(calls == 12);
    auto parent = authored(0, false, "parent"), child = authored(4, true, "child");
    parent.words[2] &= ~0x3fffu;
    const auto combined = fx::compose_material(parent, child);
    CHECK(combined.texture == "parent");
    CHECK(fx::render_material(combined).blend == 0);
    CHECK(fx::render_material(combined).clamp);
    // Opaque and alpha barriers keep additive runs and multipass surfaces apart.
    std::vector<PlanItem> items{item(0, 3, 1, 9, 0), item(3, 3, 3, 8, 1),
        item(6, 3, 0, 7, 2), item(9, 3, 1, 9, 3)};
    std::vector<PlanDraw> plan; plan_draws(items, plan);
    REQUIRE(plan.size() == 4);
    for (size_t i = 0; i < items.size(); ++i) CHECK(items[i].tag == i);
}
TEST_CASE("fx_render/shader scalar loads match the indexed packet layout") {
    const auto root = std::filesystem::path(MC2_REPO_ROOT);
    std::ifstream input(root / "shaders/vulkan/vfx_particle.vert");
    REQUIRE(input.good());
    std::ostringstream text; text << input.rdbuf();
    const auto source = text.str();
    CHECK(source.find("uint b = uint(gl_VertexIndex) * 10u;") != std::string::npos);
    CHECK(source.find("mc2ToRender(read3(b))") != std::string::npos);
    CHECK(source.find("v_color = read4(b+3u)") != std::string::npos);
    CHECK(source.find("v_uv = read2(b+7u)") != std::string::npos);
    CHECK(source.find("v_flags = words[b+9u]") != std::string::npos);
    CHECK(offsetof(fx::RenderVertex, color) / 4 == 3);
    CHECK(offsetof(fx::RenderVertex, uv) / 4 == 7);
    CHECK(offsetof(fx::RenderVertex, flags) / 4 == 9);
}
}

TEST_SUITE("VfxDrawPlan") {

TEST_CASE("indexed additive pages merge with primitive order unchanged") {
    std::vector<PlanItem> items{item(0, 3, 1, 7, 0), item(3, 3, 1, 9, 1), item(6, 3, 1, 7, 2), item(9, 3, 1, 9, 3)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 1);
    CHECK(items[0].tag == 0); CHECK(items[1].tag == 1); CHECK(items[2].tag == 2); CHECK(items[3].tag == 3);
    CHECK(draws[0].start == 0); CHECK(draws[0].count == 12);
    plan_draws(items, draws, false);
    REQUIRE(draws.size() == 4);
    CHECK(items[1].tag == 1); CHECK(items[2].tag == 2);
}

TEST_CASE("alpha groups keep submission order and split additive runs") {
    // add(7) alpha(9) add(7): the alpha group is a barrier, nothing merges across it.
    std::vector<PlanItem> items{item(0, 2, 1, 7, 0), item(2, 2, 0, 9, 1), item(4, 2, 1, 7, 2)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws);
    REQUIRE(draws.size() == 3);
    CHECK(items[0].tag == 0); CHECK(items[1].tag == 1); CHECK(items[2].tag == 2);
}

TEST_CASE("alpha neighbours with equal state merge without reordering") {
    std::vector<PlanItem> items{item(0, 2, 0, 7, 0), item(2, 2, 0, 9, 1), item(4, 2, 0, 9, 2), item(6, 2, 0, 7, 3)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws);
    REQUIRE(draws.size() == 3);
    CHECK(draws[1].start == 2); CHECK(draws[1].count == 4);
    CHECK(items[3].tag == 3);
}

TEST_CASE("sprite rect and blend mode are part of the state") {
    PlanItem a = item(0, 1, 1, 7, 0), b = item(1, 1, 1, 7, 1), c = item(2, 1, 2, 7, 2);
    b.u0 = 0.5f;
    std::vector<PlanItem> items{a, b, c};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws);
    CHECK(draws.size() == 3);
    plan_draws(items, draws, true);   // indexed: the sprite rect and blend 1/2 ride per record
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].count == 3);
}

TEST_CASE("indexed alpha pages and sprite rects merge while geometry stays separate") {
    auto a = item(0, 2, 0, 7, 0), b = item(2, 3, 0, 9, 1), c = item(5, 3, 0, 7, 2);
    a.atlas = 1; b.atlas = 4; b.u0 = 0.5f;
    std::vector<PlanItem> items{a, b, c};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 2);
    CHECK(draws[0].count == 5); CHECK(draws[1].start == 5);
    CHECK(items[0].tag == 0); CHECK(items[1].tag == 1); CHECK(items[2].tag == 2);
}

TEST_CASE("a mixed additive/alpha run is one draw on the indexed path, split into runs without it") {
    // alpha(0), SRC_ALPHA/ONE(1), ONE/ONE(2), ONE/1-SRC_ALPHA(5), alpha(0): submission order kept, one pipeline.
    std::vector<PlanItem> items{item(0, 2, 0, 7, 0), item(2, 2, 1, 9, 1), item(4, 2, 2, 7, 2), item(6, 2, 5, 9, 3), item(8, 2, 0, 7, 4)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].count == 10);
    for (uint32_t k = 0; k < 5; ++k) CHECK(items[k].tag == k);
    plan_draws(items, draws, false);
    CHECK(draws.size() == 5);
}

TEST_CASE("opaque and ONE/SRC_ALPHA groups keep their own pipeline on the indexed path") {
    std::vector<PlanItem> items{item(0, 1, 0, 7, 0), item(1, 1, 3, 7, 1), item(2, 1, 4, 7, 2), item(3, 1, 4, 9, 3), item(4, 1, 1, 7, 4)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 4);   // fold | opaque | ONE/SRC_ALPHA x2 | fold
    CHECK(draws[2].count == 2);
}

// Old per-mode blend factors (vulkan_vfx_island.cpp make_pipeline; pinned by check-vfx-blend-distinction.py) against the folded
// equation: shader outputs (rgb*w, alphaOut), hardware ONE / ONE_MINUS_SRC_ALPHA. Scene alpha is never read after the VFX pass
// (scripts/tests/test_vfx_scene_alpha_unread.py), so rgb is the contract; the alpha channel is left unequal by design.
TEST_CASE("folded premultiplied equation matches modes 0/1/2/5 in rgb within 1/255") {
    constexpr std::array<unsigned, 4> modes{0, 1, 2, 5};
    constexpr std::array<float, 8> values{0.0f, 1.0f / 255.0f, 0.2f, 0.5f, 0.8f, 254.0f / 255.0f, 1.0f, 0.37f};
    for (const auto mode : modes) {
        for (const float a : values) for (const float c : values) for (const float d : values) {
            const float src = mode == 0 || mode == 1 ? a : 1.0f;                 // old src colour factor
            const float dst = mode == 1 || mode == 2 ? 1.0f : 1.0f - a;          // old dst colour factor
            const float oldRgb = c * src + d * dst;
            const float weight = mode == 0 || mode == 1 ? a : 1.0f;              // foldBlend in vfx_particle.frag
            const float outAlpha = mode == 0 || mode == 5 ? a : 0.0f;
            const float foldedRgb = c * weight + d * (1.0f - outAlpha);
            CHECK(std::abs(oldRgb - foldedRgb) <= 1.0f / 255.0f);
        }
    }
}

// Native frame batch: three runs (gosFX packets, a ribbon, a mesh) appended in order read as one call to the island.
TEST_CASE("native batch rebases indices and draw ranges per append, in append order") {
    NativeBatch b;
    b.begin();
    const float mvp[16]{1};
    mc2::fx::RenderVertex va[3]{}, vb[2]{};
    const uint32_t ia[] = {0, 1, 2, 2, 1, 0};
    const uint32_t ib[] = {1, 0};
    VfxParticleDraw da{}, db{};
    da.start = 0; da.count = 6; da.atlasColumns = kVfxCompactGeometryAtlas;
    db.start = 0; db.count = 2; db.atlasColumns = kVfxCompactGeometryAtlas;
    b.append(va, 3, ia, 6, &da, 1, mvp, nullptr, nullptr, 640, 480);
    b.append(vb, 2, ib, 2, &db, 1, mvp, nullptr, nullptr, 640, 480);
    REQUIRE(b.vertices.size() == 5);
    REQUIRE(b.indices.size() == 8);
    CHECK(b.indices[5] == 0);
    CHECK(b.indices[6] == 3 + 1);   // second run's indices start after the first run's 3 vertices
    CHECK(b.indices[7] == 3 + 0);
    REQUIRE(b.draws.size() == 2);
    CHECK(b.draws[0].start == 0);
    CHECK(b.draws[1].start == 6);   // second run's index range starts after the first run's 6 indices
    CHECK(b.draws[1].count == 2);
    CHECK_FALSE(b.hasCam);
    CHECK(b.hasMvp);
}

TEST_CASE("native batch keeps an invalid range or index invalid so the island skips the draw") {
    NativeBatch b;
    b.begin();
    const float mvp[16]{1};
    mc2::fx::RenderVertex v[2]{};
    const uint32_t idx[] = {0, 1, 7};   // 7 >= vertexCount
    VfxParticleDraw bad{}, good{};
    bad.start = 2; bad.count = 5;       // past indexCount
    good.start = 0; good.count = 2;
    VfxParticleDraw both[2] = {bad, good};
    b.append(v, 2, idx, 3, both, 2, mvp, nullptr, nullptr, 64, 64);
    CHECK(b.draws[0].start == UINT32_MAX);
    CHECK(b.draws[1].start == 0);
    CHECK(b.indices[2] == UINT32_MAX);
    b.begin();   // capacity kept, contents gone
    CHECK(b.vertices.empty());
    CHECK(b.indices.empty());
    CHECK(b.draws.empty());
}

}
