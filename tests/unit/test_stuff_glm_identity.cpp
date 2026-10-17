#include <stuff/stuff.hpp>
#include "stuff_glm_config.h"
#include "math/mc2_math.h"
#include "math/stuff_glm_bridge.h"
#include "axis_convert.h"
#include "doctest.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <string>

#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error Bit identity requires the engine's precise floating point mode
#endif

// Link stubs: rotation.cpp's unused class-init/diagnostic code references engine services.
void AddStatistic(const char*, const char*, gosType, void*, DWORD) {}
void AddDebuggerMenuItem(const char*, bool(__stdcall*)(), void(__stdcall*)(), bool(__stdcall*)(),
                         DWORD(__stdcall*)(const char*, DWORD)) {}
namespace Stuff {
bool Small_Enough(const Vector4D& v, Scalar e) {
    return Small_Enough(v.x, e) && Small_Enough(v.y, e) && Small_Enough(v.z, e) && Small_Enough(v.w, e);
}
}  // namespace Stuff

namespace {
constexpr unsigned seed = 0x53545546;
constexpr unsigned randomCases = 100000;

bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

struct Row {
    const char* tag = nullptr;
    bool requireIdentity = false;
    unsigned samples = 0, mismatches = 0;
};

struct Corpus {
    std::map<std::string, Row> rows;

    void record(const char* name, const char* tag, bool required, bool equal) {
        auto& row = rows[name];
        row.tag = tag;
        row.requireIdentity = required;
        ++row.samples;
        row.mismatches += !equal;
    }
    void scalar(const char* name, float old, float candidate) {
        record(name, "sim-side", false, same(old, candidate));
    }
    void exact(const char* name, float old, float candidate) {
        record(name, "sim-side", true, same(old, candidate));
    }
    void vector(const char* name, const Stuff::Vector3D& old, glm::vec3 candidate,
                bool required = false, const char* tag = "sim-side") {
        record(name, tag, required, same(old.x, candidate.x) &&
               same(old.y, candidate.y) && same(old.z, candidate.z));
    }
    template<class M>
    void matrix(const char* name, const M& old, const glm::mat4& candidate, int columns) {
        bool equal = true;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < columns; ++c)
                equal &= same(old(r, c), candidate[c][r]);
        record(name, "render-only", false, equal);
    }
    void quaternion(const char* name, const Stuff::UnitQuaternion& old, glm::quat candidate) {
        record(name, "render-only", false, same(old.x, candidate.x) &&
               same(old.y, candidate.y) && same(old.z, candidate.z) && same(old.w, candidate.w));
    }
};

// The production bridge (mclib/math/stuff_glm_bridge.h) is the converter under test: Stuff multiplies row vectors, glm
// indices are [column][row], an affine matrix gets the implicit (0, 0, 0, 1) fourth column.
glm::vec3 glmVector(const Stuff::Vector3D& v) { return mc2::math::toGlm(v); }
template<class M>
glm::mat4 glmMatrix(const M& m, int) { return mc2::math::toGlm(m); }

float finite(std::mt19937& rng) {
    const auto bits = static_cast<std::uint32_t>(rng());
    const auto exponent = 1u + ((bits >> 23) % 253u);
    return std::bit_cast<float>((bits & 0x807fffffu) | (exponent << 23));
}

void vectors(Corpus& corpus, const Stuff::Vector3D& a, const Stuff::Vector3D& b, float t) {
    const auto ga = glmVector(a), gb = glmVector(b);
    Stuff::Vector3D out;
    corpus.vector("vector.negate", out.Negate(a), -ga, true);
    corpus.vector("vector.add", out.Add(a, b), ga + gb, true);
    corpus.vector("vector.subtract", out.Subtract(a, b), ga - gb, true);
    corpus.vector("vector.scale", out.Multiply(a, t), ga * t, true);
    corpus.vector("vector.component-multiply", out.Multiply(a, b), ga * gb, true);
    corpus.vector("vector.add-scaled", out.AddScaled(a, b, t), ga + gb * t);
    corpus.vector("vector.combine", out.Combine(a, t, b, 1.0f - t), t * ga + (1.0f - t) * gb);
    corpus.vector("vector.lerp", out.Lerp(a, b, t), glm::mix(ga, gb, t));
    corpus.vector("exact.lerp", out.Lerp(a, b, t), mc2::math::lerp(ga, gb, t), true);
    corpus.vector("vector.cross", out.Cross(a, b), glm::cross(ga, gb));
    corpus.scalar("vector.dot", a * b, glm::dot(ga, gb));
    corpus.scalar("vector.length-squared", a.GetLengthSquared(), glm::dot(ga, ga));
    corpus.scalar("vector.length", a.GetLength(), glm::length(ga));
    corpus.scalar("vector.approximate-length", a.GetApproximateLength(), glm::length(ga));
    if (!Stuff::Small_Enough(t))
    {
        corpus.vector("vector.scalar-divide", out.Divide(a, t), ga / t);
        corpus.vector("exact.divide", out.Divide(a, t), mc2::math::divide(ga, t), true);
    }
    if (!Stuff::Small_Enough(b.x) && !Stuff::Small_Enough(b.y) && !Stuff::Small_Enough(b.z))
        corpus.vector("vector.component-divide", out.Divide(a, b), ga / gb, true);
    const float length = a.GetLength();
    if (std::isfinite(length) && !Stuff::Small_Enough(length)) {
        corpus.vector("vector.normalize", out.Normalize(a), glm::normalize(ga));
        Stuff::UnitVector3D unit(a);
        corpus.vector("unit-vector.normalize", unit, glm::normalize(ga));
    }
    Stuff::Point3D point;
    corpus.vector("point.add", point.Add(a, b), ga + gb, true);
    corpus.vector("bridge.round-trip", mc2::math::toVector3D(ga), ga, true);
    corpus.vector("point.subtract", point.Subtract(Stuff::Point3D(a), b), ga - gb, true);
    Stuff::Vector3D delta;
    delta.Subtract(a, b);
    corpus.scalar("chain.path-distance", delta.GetApproximateLength(), glm::distance(ga, gb));
    Stuff::Vector3D velocity, position;
    velocity.Multiply(b, t);
    position.Add(a, velocity);
    corpus.vector("chain.mover-integration", position, ga + gb * t);
    if (std::isfinite(delta.GetLength()) && !Stuff::Small_Enough(delta.GetLength())) {
        Stuff::Vector3D direction, step;
        direction.Normalize(delta);
        step.Multiply(direction, t);
        position.Add(b, step);
        corpus.vector("chain.line-of-sight-step", position, gb + glm::normalize(ga - gb) * t);
    }
    const auto axis = mc2ToRender(a);
    corpus.vector("axis-convert", axis, glm::vec3(-ga.x, ga.z, ga.y), true, "render-only"); // axis-swizzle: allow (independent oracle for mc2ToRender)
}

void scalars(Corpus& corpus, float x, float y) {
    const float positive = std::fabs(x);
    corpus.scalar("scalar.sqrt", Stuff::Sqrt(positive), glm::sqrt(positive));
    corpus.scalar("scalar.approximate-sqrt", Stuff::SqrtApproximate(positive), glm::sqrt(positive));
    corpus.scalar("scalar.sin", Stuff::Sin(x), glm::sin(x));
    corpus.scalar("scalar.cos", Stuff::Cos(x), glm::cos(x));
    corpus.scalar("scalar.tan", Stuff::Tan(x), glm::tan(x));
    if (!Stuff::Small_Enough(x) || !Stuff::Small_Enough(y))
        corpus.scalar("scalar.atan2", Stuff::Arctan(y, x), glm::atan(y, x));
    const float bounded = std::isfinite(x) ? x / (1.0f + std::fabs(x)) : 0.0f;
    corpus.scalar("scalar.asin", Stuff::Arcsin(bounded), glm::asin(bounded));
    corpus.scalar("scalar.acos", Stuff::Arccos(bounded), glm::acos(bounded));
    // The harness TU includes stuff.hpp before <cmath>: Stuff binds the double CRT here, as in the non-PCH libraries.
    corpus.exact("exact.sin", Stuff::Sin(x), mc2::math::sinD(x));
    corpus.exact("exact.cos", Stuff::Cos(x), mc2::math::cosD(x));
    if (!Stuff::Small_Enough(x) || !Stuff::Small_Enough(y))
        corpus.exact("exact.atan2", Stuff::Arctan(y, x), mc2::math::atan2D(y, x));
    corpus.exact("exact.acos", Stuff::Arccos(bounded), mc2::math::acosD(bounded));
    corpus.exact("exact.wrap-static", Stuff::Radian::Normalize(x), mc2::math::wrapAngleStatic(x));
    {
        Stuff::Radian member(x);
        member.Normalize();
        corpus.exact("exact.wrap-member", member.angle, mc2::math::wrapAngleMember(x));
    }
    const Stuff::SinCosPair pair{Stuff::Radian(x)};
    corpus.scalar("angle.sincos-sin", pair.sine, glm::sin(x));
    corpus.scalar("angle.sincos-cos", pair.cosine, glm::cos(x));
    corpus.scalar("angle.wrap-static", Stuff::Radian::Normalize(x),
                  glm::mod(x + Stuff::Pi, Stuff::Two_Pi) - Stuff::Pi);
    Stuff::Radian angle(x);
    angle.Normalize();
    corpus.scalar("angle.wrap-member", angle.angle,
                  glm::mod(x + Stuff::Pi, Stuff::Two_Pi) - Stuff::Pi);
}

void matrices(Corpus& corpus, std::mt19937& rng, unsigned index) {
    auto smallRand = [&]() { return (static_cast<float>(rng() >> 8) / 16777216.0f - 0.5f) * 4.0f; };
    Stuff::AffineMatrix4D a, b, out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) { a(r, c) = smallRand(); b(r, c) = smallRand(); }
    // Positive, tiny and negative determinants expose the general-inverse fallback.
    if (index % 8 < 4) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) a(r, c) = r == c ? 1.0f : 0.0f;
        a(2, 2) = index % 8 == 0 ? 0x1p-22f :
            (index % 8 == 1 ? -1.0f : (index % 8 == 2 ? 0.0f : 0x1p-12f));
    }
    const auto ga = glmMatrix(a, 3), gb = glmMatrix(b, 3);
    out.Multiply(a, b);
    corpus.matrix("affine.multiply", out, ga * gb, 3);
    Stuff::Vector3D v(smallRand(), smallRand(), smallRand()), transformed;
    transformed.Multiply(v, a);
    corpus.vector("vector.affine-transform", transformed, glm::vec3(glm::vec4(glmVector(v), 0.0f) * ga), false, "render-only");
    Stuff::Point3D p(v), point;
    point.Multiply(p, a);
    corpus.vector("point.affine-transform", point, glm::vec3(glm::vec4(glmVector(p), 1.0f) * ga), false, "render-only");
    Stuff::Point3D world;
    world.Multiply(point, b);
    corpus.vector("chain.row-transform", mc2ToRender(world),
        mc2ToRender(glm::vec3((glm::vec4(glmVector(p), 1.0f) * ga) * gb)), false, "render-only");
    const float determinant = a(0, 0) * (a(1, 1)*a(2, 2) - a(1, 2)*a(2, 1)) +
        a(0, 1) * (a(1, 2)*a(2, 0) - a(1, 0)*a(2, 2)) +
        a(0, 2) * (a(1, 0)*a(2, 1) - a(1, 1)*a(2, 0));
    if (!Stuff::Small_Enough(determinant)) {
        out.Invert(a);
        corpus.matrix("affine.inverse", out, glm::inverse(ga), 3);
    }
    Stuff::Matrix4D ma, mb, mo;
    ma = a; mb = b;
    ma(0, 3) = smallRand(); mb(1, 3) = smallRand();
    const auto gma = glmMatrix(ma, 4), gmb = glmMatrix(mb, 4);
    mo.Multiply(ma, mb);
    corpus.matrix("matrix.multiply", mo, gma * gmb, 4);
    mo.Invert(ma);
    corpus.matrix("matrix.inverse", mo, glm::inverse(gma), 4);
    Stuff::LinearMatrix4D la(true), lb(true), lo;
    const Stuff::SinCosPair rotation{Stuff::Radian(smallRand())};
    la(0, 0) = rotation.cosine; la(0, 1) = rotation.sine;
    la(1, 0) = -rotation.sine; la(1, 1) = rotation.cosine;
    lb(3, 0) = smallRand(); lb(3, 1) = smallRand(); lb(3, 2) = smallRand();
    lo.Multiply(la, lb);
    corpus.matrix("linear.multiply", lo, glmMatrix(la, 3) * glmMatrix(lb, 3), 3);
    lo.Invert(la);
    corpus.matrix("linear.inverse", lo, glm::inverse(glmMatrix(la, 3)), 3);
    transformed.MultiplyByInverse(v, la);
    corpus.vector("vector.inverse-transform", transformed,
        glm::vec3(glm::vec4(glmVector(v), 0.0f) * glm::inverse(glmMatrix(la, 3))), false, "render-only");
    const float halfA = smallRand(), halfB = smallRand();
    const Stuff::UnitQuaternion qa(std::sin(halfA), 0.0f, 0.0f, std::cos(halfA));
    const Stuff::UnitQuaternion qb(0.0f, std::sin(halfB), 0.0f, std::cos(halfB));
    const glm::quat gqa(qa.w, qa.x, qa.y, qa.z), gqb(qb.w, qb.x, qb.y, qb.z);
    Stuff::UnitQuaternion qo;
    qo.Multiply(qa, qb);
    corpus.quaternion("quaternion.multiply", qo, gqb * gqa);
    qo = qa; qo.Normalize();
    corpus.quaternion("quaternion.normalize", qo, glm::normalize(gqa));
}
}

TEST_CASE("Stuff/GLM identity detects byte changes and classifies migration candidates") {
    CHECK_FALSE(same(0.0f, -0.0f));
    CHECK_FALSE(same(1.0f, std::nextafter(1.0f, 2.0f)));
    REQUIRE(sizeof(float) == sizeof(std::uint32_t));
    REQUIRE(std::numeric_limits<float>::is_iec559);
    Corpus corpus;
    std::mt19937 rng(seed);
    const std::array axes{Stuff::Vector3D(1, 0, 0), Stuff::Vector3D(0, 1, 0),
                         Stuff::Vector3D(0, 0, 1), Stuff::Vector3D(0, 0, 0)};
    for (const auto& a : axes)
        for (const auto& b : axes) vectors(corpus, a, b, 0.5f);
    const std::array edges{0.0f, -0.0f, std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(),
        1e-30f, -1e-30f, 1e30f, -1e30f, std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(), 1.0f, -1.0f, Stuff::Pi, -Stuff::Pi,
        Stuff::Two_Pi, -Stuff::Two_Pi};
    for (float x : edges)
        for (float y : edges) {
            vectors(corpus, Stuff::Vector3D(x, y, x), Stuff::Vector3D(y, x, y), 0.25f);
            scalars(corpus, x, y);
        }
    for (unsigned i = 0; i < randomCases; ++i) {
        const float x = finite(rng), y = finite(rng), z = finite(rng);
        const float u = finite(rng), v = finite(rng), w = finite(rng);
        vectors(corpus, Stuff::Vector3D(x, y, z), Stuff::Vector3D(u, v, w),
                static_cast<float>(rng() >> 8) / 16777216.0f);
        scalars(corpus, x, y);
        matrices(corpus, rng, i);
    }
    std::printf("[STUFF_GLM] seed=%u random_cases=%u glm=1.0.1 mode=scalar-precise\n", seed, randomCases);
    REQUIRE(corpus.rows.size() == 56);
    // Step 5: every operation identical on the opening corpus is now REQUIRED identical (a port relies on it), as are the exact
    // wrappers (mclib/math/mc2_math.h) and the bridge. Differing render-only operations stay characterized.
    static const char* const identical[] = {"angle.sincos-cos", "angle.sincos-sin", "chain.line-of-sight-step",
        "chain.mover-integration", "chain.path-distance", "chain.row-transform", "linear.multiply", "matrix.multiply",
        "point.affine-transform", "quaternion.multiply", "scalar.approximate-sqrt", "scalar.sqrt", "unit-vector.normalize",
        "vector.add-scaled", "vector.approximate-length", "vector.combine", "vector.cross", "vector.dot", "vector.length",
        "vector.length-squared", "vector.normalize"};
    for (const char* name : identical) {
        REQUIRE(corpus.rows.count(name) == 1);
        corpus.rows[name].requireIdentity = true;
    }
    for (const auto& [name, row] : corpus.rows) {
        std::printf("[STUFF_GLM] op=%s tag=%s samples=%u mismatches=%u class=%s policy=%s\n",
            name.c_str(), row.tag, row.samples, row.mismatches,
            row.mismatches ? "not-identical" : "identical-on-corpus",
            row.requireIdentity ? "required" : "characterize");
        INFO(name);
        CHECK(row.samples > 0);
        if (row.requireIdentity) CHECK(row.mismatches == 0);
    }
}
