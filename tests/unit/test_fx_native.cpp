#include "doctest.h"
#include "contract_process.h"
#include "fx/fx_catalog.h"

#include <cstring>
#include <random>

namespace {
using namespace mc2::fx;

// Hand-authored Effect wire record: one self-event with a recognizable affine
// block; constant lifespan and two polynomial child seeds; independent MLR tag.
std::vector<uint8_t> fixture(unsigned gfx = 17, int mlr = 9) {
    Writer w;
    w.integer(gfx_signature); w.integer(gfx); w.integer(1);
    w.integer(first_effect_class); w.string("native\"fixture"); w.integer(1);
    w.scalar(0.25f); w.integer(7); w.integer(0);
    for (int i = 0; i < 12; ++i) w.scalar(float(i));
    w.scalar(2.5f);
    for (float v : {0.0f, 0.5f, 0.25f, -0.0f, 1.0f, -0.5f, -0.25f, 0.0f}) w.scalar(v);
    if (mlr >= 0) { w.integer(mlr_signature); w.integer(mlr); }
    for (auto word : {1u, 0x12345678u, 0xffffffffu, 2u, 0x100u, 0xffffffffu}) w.integer(word);
    if (mlr < 6) { w.integer(0); w.integer(0); if (mlr > 3) { w.integer(0); w.integer(0); } }
    w.string("native_texture"); w.integer(0x32);
    return w.bytes;
}

void rejects(const std::vector<uint8_t>& bytes) {
    std::string error;
    CHECK_FALSE(read_catalog(bytes, error));
    CHECK(error.find("FX byte ") == 0);
}

std::vector<uint8_t> shape_fixture(std::vector<size_t>& count_offsets) {
    Writer w;
    w.integer(gfx_signature); w.integer(17); w.integer(1);
    w.integer(1323); w.string("shape"); w.integer(0); w.scalar(1);
    for (int i = 0; i < 8; ++i) w.scalar(0);
    w.integer(mlr_signature); w.integer(9);
    for (int i = 0; i < 6; ++i) w.integer(0);
    for (int i = 0; i < 5; ++i) {
        w.integer(1); w.scalars(std::array<float, 3>{0, 0, 1}); // age complex curve
        if (i == 4) { w.integer(1); w.scalars(std::array<float, 3>{0, 0, 1}); }
        else { w.scalar(1); w.scalar(0); }
        w.boolean(true);
    }
    w.boolean(false); w.boolean(false);
    w.integer(mlr_signature); w.integer(9);
    count_offsets.push_back(w.bytes.size()); w.integer(1); // one primitive
    w.integer(95); // multipass polygon
    count_offsets.push_back(w.bytes.size()); w.integer(4);
    for (const Vec3& p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}) w.scalars(p);
    count_offsets.push_back(w.bytes.size()); w.integer(4);
    for (int i = 0; i < 4; ++i) w.scalars(Vec2{float(i), float(i)});
    count_offsets.push_back(w.bytes.size()); w.integer(1); w.integer(4, 1); w.integer(0);
    for (int i = 0; i < 6; ++i) w.integer(0);
    count_offsets.push_back(w.bytes.size()); w.integer(4);
    for (int i = 0; i < 4; ++i) w.integer(i, 2);
    count_offsets.push_back(w.bytes.size()); w.integer(2, 1);
    for (int i = 0; i < 6; ++i) w.integer(0);
    count_offsets.push_back(w.bytes.size()); w.integer(4);
    for (int i = 0; i < 4; ++i) w.scalars(Vec2{float(i) + 0.5f, float(i) + 0.25f});
    w.scalar(2);
    return w.bytes;
}
}

TEST_CASE("fx_native/versioned catalog rebuilds authored fields and owns its storage") {
    for (unsigned gfx = 9; gfx <= 17; ++gfx) {
        for (int mlr : {-1, 1, 3, 4, 5, 6, 9}) {
            auto bytes = fixture(gfx, mlr);
            std::string error;
            auto catalog = read_catalog(bytes, error);
            REQUIRE_MESSAGE(catalog, error);
            CHECK(catalog->rebuild() == bytes);
            CHECK(catalog->version() == gfx);
            const auto& effect = catalog->effects().at(0);
            CHECK(effect.class_id == 1312);
            CHECK(effect.events.at(0).local_to_parent[11] == 11);
            CHECK(effect.material.texture == "native_texture");
            CHECK(effect.material.texture_instance == 0x32);
            CHECK(effect.state_version.value == mlr);
            CHECK(effect.find_curve("m_lifeSpan")->evaluate(0.5f, 0.7f) == 2.5f);
            CHECK(catalog->find(effect.name) == &effect);
            const auto expected = bytes;
            std::fill(bytes.begin(), bytes.end(), 0xff);
            CHECK(catalog->rebuild() == expected);
        }
    }
}

TEST_CASE("fx_native/every truncation of a nested record fails without partial output") {
    auto bytes = fixture();
    for (size_t size = 0; size < bytes.size(); ++size) {
        CAPTURE(size);
        std::string error;
        CHECK_FALSE(read_catalog(std::span(bytes).first(size), error));
        CHECK_FALSE(error.empty());
    }
    std::string error;
    CHECK_FALSE(read_catalog({}, error));
}

TEST_CASE("fx_native/corrupt signatures ordinals counts strings and references are rejected") {
    const auto valid = fixture();
    for (size_t offset : {0u, 4u, 8u, 12u, 16u}) {
        auto bytes = valid;
        std::fill_n(bytes.begin() + offset, 4, 0xff);
        rejects(bytes);
    }
    auto bytes = valid;
    bytes.push_back(0); rejects(bytes);
    // Name's terminating NUL is mandatory, not consumed as another record.
    bytes = valid; bytes[20 + std::strlen("native\"fixture")] = 1; rejects(bytes);
    bytes = valid;
    const size_t event = 20 + std::strlen("native\"fixture") + 1 + 4;
    bytes[event + 8] = 1; rejects(bytes); // no effect #1 exists
    bytes = valid;
    bytes[event] = 0; bytes[event + 1] = 0; bytes[event + 2] = 0x80; bytes[event + 3] = 0x7f;
    rejects(bytes); // infinity is not a valid event time
}

TEST_CASE("fx_native/deterministic corruption fuzz never publishes unrebuildable records") {
    const auto valid = fixture();
    std::mt19937 random(0x17f00d);
    for (int i = 0; i < 2000; ++i) {
        auto bytes = valid;
        for (unsigned j = 0, n = 1 + random() % 4; j < n; ++j) bytes[random() % bytes.size()] ^= uint8_t(1u << (random() % 8));
        std::string error;
        const auto parsed = read_catalog(bytes, error);
        if (parsed) CHECK(parsed->rebuild() == bytes);
        else CHECK_FALSE(error.empty());
    }
}

TEST_CASE("fx_native/nested multipass truncation counts and index corruption fail safely") {
    std::vector<size_t> offsets;
    auto bytes = shape_fixture(offsets);
    std::string error;
    const auto catalog = read_catalog(bytes, error);
    REQUIRE_MESSAGE(catalog, error);
    CHECK(catalog->rebuild() == bytes);
    const auto& mesh = catalog->effects()[0].geometry[0].shape.primitives[0];
    CHECK(mesh.class_id == 95);
    CHECK(mesh.materials.size() == 2);
    CHECK(mesh.bounds.maximum == Vec3{1, 1, 0});
    for (size_t i = 0; i < bytes.size(); ++i) {
        CHECK_FALSE(read_catalog(std::span(bytes).first(i), error));
        CHECK_FALSE(error.empty());
    }
    for (auto offset : offsets) {
        auto corrupt = bytes;
        std::fill_n(corrupt.begin() + offset, 4, 0xff);
        rejects(corrupt);
    }
    auto corrupt = bytes;
    corrupt[offsets[4] + 4] = 4; // index equal to vertex count
    rejects(corrupt);
    for (int i = 0; i < 2000; ++i) {
        corrupt = bytes;
        const auto offset = size_t(i * 313u) % bytes.size();
        corrupt[offset] ^= uint8_t(1u << (i % 8));
        const auto parsed = read_catalog(corrupt, error);
        if (parsed) CHECK(parsed->rebuild() == corrupt);
        else CHECK_FALSE(error.empty());
    }
}

TEST_CASE("fx_native/curve polynomial seed and legacy tail semantics retain wire values") {
    Curve curve;
    curve.age.kind = CurveKind::spline;
    curve.age.coefficients = {1, 2, 3, 4};
    curve.seed = CurveData{}; curve.seed->coefficients[0] = 0.5f; curve.seeded = true;
    CHECK(curve.evaluate(0.5f, 0.7f) == 1.625f);
    curve.age.kind = CurveKind::complex;
    curve.age.keys = {{0, 2, 1}, {0.5f, -8, 2}};
    CHECK(curve.age.evaluate(0.25f) == 1.5f);
    CHECK(curve.age.evaluate(1) == 0);
    CHECK(curve.age.keys.back().slope == -8); // authored data remains unchanged
    Writer w; write_curve(w, curve, 14);
    Reader r(w.bytes);
    const auto again = read_curve(r, "curve", CurveKind::complex, CurveKind::constant, 14);
    CHECK(again.age.keys.back().slope == -8);
    CHECK(r.remaining() == 0);
}

TEST_CASE("fx_native/existing non-stock multipass and debris fixtures match legacy") {
    CHECK(runContractChild(contractExecutable("vfx_lifecycle_tests"),
        {"--no-breaks", "--test-case=vfx_frozen/B6*", "--no-skip"}) == 0);
}
