// Step 5 (C++23, 2026-10-18): the engine math on glm after the last Stuff:: math type went.
// WHAT: (1) the convention adapters of mclib/math/mc2_xform.h and the exact wrappers of mc2_math.h against the Stuff bodies they
// replaced, kept here verbatim as the oracle (bit-identical over a seeded corpus: these rows are REQUIRED);
// (2) the byte formats old files read back (E1 quaternion x,y,z,w; E2 the 48-byte matrix / 64-byte OBB stream records; E3 the
// 12-byte vectors), (3) the convention guard with its negative control: the engine's row-vector product must differ from glm's
// column product on a non-symmetric matrix, and the engine rotation of a quaternion is the transpose of glm::mat4_cast.
// WHY: owner ruling 2026-10-18 relaxed bit identity to byte formats (E) and baselines re-recorded once (R); the adapters exist
// for convention, so their bodies were kept verbatim and this corpus proves they still are.
#include <stuff/stuff.hpp>
#include "stuff_glm_config.h"
#include "math/mc2_math.h"
#include "math/mc2_xform.h"
#include "doctest.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <string>

namespace {

constexpr unsigned seed = 0x53545546;
constexpr unsigned randomCases = 100000;

bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

struct Row {
    unsigned samples = 0, mismatches = 0;
};
struct Corpus {
    std::map<std::string, Row> rows;
    void exact(const char* name, float a, float b) {
        auto& r = rows[name];
        ++r.samples;
        r.mismatches += !same(a, b);
    }
    void vec(const char* name, const glm::vec3& a, const glm::vec3& b) {
        auto& r = rows[name];
        ++r.samples;
        r.mismatches += !(same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z));
    }
    void mat(const char* name, const float (&legacy)[4][4], const glm::mat4& m, int rows4 = 4, int cols = 4) {
        auto& r = rows[name];
        ++r.samples;
        bool eq = true;
        for (int i = 0; i < rows4; ++i)
            for (int c = 0; c < cols; ++c) eq &= same(legacy[i][c], m[c][i]);
        r.mismatches += !eq;
    }
};

// ---- the legacy oracle: Stuff bodies verbatim on a plain row-major M[row][column] (deleted with the types at step 5).
struct L4 {
    float m[4][4];
};
L4 legacyIdentity() {
    L4 r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
}
// AffineMatrix4D::BuildRotation(const EulerAngles&) (affinematrix.cpp), on (pitch, yaw, roll).
void legacyRotationEuler(L4& t, float pitch, float yaw, float roll) {
    const float xSine = sinf(pitch), xCosine = cosf(pitch), ySine = sinf(yaw), yCosine = cosf(yaw), zSine = sinf(roll),
                zCosine = cosf(roll);
    t.m[0][0] = yCosine * zCosine;
    t.m[0][1] = yCosine * zSine;
    t.m[0][2] = -ySine;
    t.m[1][0] = xSine * ySine * zCosine - xCosine * zSine;
    t.m[1][1] = xSine * ySine * zSine + xCosine * zCosine;
    t.m[1][2] = xSine * yCosine;
    t.m[2][0] = xCosine * ySine * zCosine + xSine * zSine;
    t.m[2][1] = xCosine * ySine * zSine - xSine * zCosine;
    t.m[2][2] = xCosine * yCosine;
}
// AffineMatrix4D::BuildRotation(const UnitQuaternion&).
void legacyRotationQuat(L4& t, float x, float y, float z, float w) {
    const float a = x * y, b = y * z, c = z * x, d = w * x, e = w * y, f = w * z, g = w * w, h = x * x, i = y * y, j = z * z;
    t.m[0][0] = g + h - i - j;
    t.m[1][0] = 2.0f * (a - f);
    t.m[2][0] = 2.0f * (c + e);
    t.m[0][1] = 2.0f * (f + a);
    t.m[1][1] = g - h + i - j;
    t.m[2][1] = 2.0f * (b - d);
    t.m[0][2] = 2.0f * (c - e);
    t.m[1][2] = 2.0f * (b + d);
    t.m[2][2] = g - h - i + j;
}
// LinearMatrix4D::Invert.
L4 legacyInvertRigid(const L4& s) {
    L4 t = legacyIdentity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t.m[i][j] = s.m[j][i];
    t.m[3][0] = -s.m[3][0] * s.m[0][0] - s.m[3][1] * s.m[0][1] - s.m[3][2] * s.m[0][2];
    t.m[3][1] = -s.m[3][0] * s.m[1][0] - s.m[3][1] * s.m[1][1] - s.m[3][2] * s.m[1][2];
    t.m[3][2] = -s.m[3][0] * s.m[2][0] - s.m[3][1] * s.m[2][1] - s.m[3][2] * s.m[2][2];
    return t;
}
// Point3D::Multiply(p, AffineMatrix4D) and Vector3D::Multiply(v, AffineMatrix4D).
glm::vec3 legacyPoint(const glm::vec3& p, const L4& t) {
    return {p.x * t.m[0][0] + p.y * t.m[1][0] + p.z * t.m[2][0] + t.m[3][0],
            p.x * t.m[0][1] + p.y * t.m[1][1] + p.z * t.m[2][1] + t.m[3][1],
            p.x * t.m[0][2] + p.y * t.m[1][2] + p.z * t.m[2][2] + t.m[3][2]};
}
glm::vec3 legacyVector(const glm::vec3& v, const L4& t) {
    return {v.x * t.m[0][0] + v.y * t.m[1][0] + v.z * t.m[2][0], v.x * t.m[0][1] + v.y * t.m[1][1] + v.z * t.m[2][1],
            v.x * t.m[0][2] + v.y * t.m[1][2] + v.z * t.m[2][2]};
}
// Matrix4D::SetPerspective(near, far, left, right, top, bottom).
void legacyPerspective(L4& t, float n, float f, float l, float r, float tp, float b) {
    const float hr = 1.0f / (l - r), vr = 1.0f / (tp - b), dr = 1.0f / (f - n);
    t = L4{};
    t.m[0][0] = n * hr;
    t.m[1][1] = n * vr;
    t.m[2][0] = -r * hr;
    t.m[2][1] = -b * vr;
    t.m[2][2] = f * dr;
    t.m[2][3] = 1.0f;
    t.m[3][2] = -f * n * dr;
}
glm::mat4 toGlm(const L4& t) {
    glm::mat4 g;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) g[c][r] = t.m[r][c];
    return g;
}

float unit(std::mt19937& rng) { return static_cast<float>(rng() >> 8) / 16777216.0f; }
float angle(std::mt19937& rng) { return (unit(rng) - 0.5f) * 8.0f; }

}  // namespace

TEST_CASE("glm engine math: convention adapters keep the Stuff bodies (REQUIRED, 0 mismatches)") {
    CHECK_FALSE(same(0.0f, -0.0f));  // comparator negative controls
    CHECK_FALSE(same(1.0f, std::nextafter(1.0f, 2.0f)));
    std::mt19937 rng(seed);
    Corpus corpus;
    for (unsigned i = 0; i < randomCases; ++i) {
        const float p = angle(rng), y = angle(rng), r = angle(rng);
        L4 le = legacyIdentity();
        legacyRotationEuler(le, p, y, r);
        le.m[3][0] = angle(rng);
        le.m[3][1] = angle(rng);
        le.m[3][2] = angle(rng);
        glm::mat4 ge(1.0f);
        mc2::math::buildRotation(ge, mc2::math::Euler(p, y, r));
        mc2::math::buildTranslation(ge, glm::vec3(le.m[3][0], le.m[3][1], le.m[3][2]));
        corpus.mat("exact.rotation-from-euler", le.m, ge);
        float qx = angle(rng), qy = angle(rng), qz = angle(rng), qw = angle(rng);
        const float len = 1.0f / std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        qx *= len; qy *= len; qz *= len; qw *= len;
        L4 lq = legacyIdentity();
        legacyRotationQuat(lq, qx, qy, qz, qw);
        corpus.mat("exact.rotation-from-quat", lq.m, mc2::math::rotationMatrix(mc2::math::quatXYZW(qx, qy, qz, qw)));
        corpus.mat("exact.invert-rigid", legacyInvertRigid(le).m, mc2::math::invertRigid(ge));
        const glm::vec3 v(angle(rng), angle(rng), angle(rng));
        corpus.vec("exact.transform-point", legacyPoint(v, le), mc2::math::transformPoint(v, ge));
        corpus.vec("exact.transform-vector", legacyVector(v, le), mc2::math::transformVector(v, ge));
        corpus.vec("exact.translation", glm::vec3(le.m[3][0], le.m[3][1], le.m[3][2]), mc2::math::getTranslation(ge));
        corpus.vec("exact.local-forward", glm::vec3(le.m[2][0], le.m[2][1], le.m[2][2]), mc2::math::localForwardInWorld(ge));
        corpus.vec("exact.local-right", glm::vec3(-le.m[0][0], -le.m[0][1], -le.m[0][2]), mc2::math::localRightInWorld(ge));
        corpus.vec("exact.world-up-in-local", glm::vec3(le.m[0][1], le.m[1][1], le.m[2][1]), mc2::math::worldUpInLocal(ge));
        const float n = 1.0f + unit(rng) * 10.0f, f = n + 1.0f + unit(rng) * 5000.0f, w = 0.1f + unit(rng), h = 0.1f + unit(rng);
        L4 lp;
        legacyPerspective(lp, n, f, w, -w, h, -h);
        glm::mat4 gp(1.0f);
        mc2::math::setPerspective(gp, n, f, w, -w, h, -h);
        corpus.mat("exact.set-perspective", lp.m, gp);
    }
    std::printf("[GLM_MATH] seed=%u random_cases=%u glm=1.0.1 mode=scalar-precise\n", seed, randomCases);
    REQUIRE(corpus.rows.size() == 10);
    for (const auto& [name, row] : corpus.rows) {
        std::printf("[GLM_MATH] op=%s samples=%u mismatches=%u policy=required\n", name.c_str(), row.samples, row.mismatches);
        INFO(name);
        CHECK(row.samples == randomCases);
        CHECK(row.mismatches == 0);
    }
}

TEST_CASE("glm engine math: the row-vector convention (guard and negative control)") {
    // A non-symmetric affine matrix: the engine product p * M must DIFFER from glm's column product M * p (negative control:
    // the guard can fail), and equal the legacy row product.
    glm::mat4 m(1.0f);
    mc2::math::buildRotation(m, mc2::math::Euler(0.3f, 1.1f, -0.7f));
    mc2::math::buildTranslation(m, glm::vec3(5.0f, -2.0f, 9.0f));
    const glm::vec3 p(1.0f, 2.0f, 3.0f);
    const glm::vec3 row = mc2::math::transformPoint(p, m), column = glm::vec3(m * glm::vec4(p, 1.0f));
    CHECK(glm::length(row - column) > 1.0f);
    // translation is row 3 (glm::translate would write column 3)
    CHECK(m[3] == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK(mc2::math::getTranslation(m) == glm::vec3(5.0f, -2.0f, 9.0f));
    // the engine rotation of a quaternion is the transpose of glm::mat4_cast (to 4 ULP-ish)
    const glm::quat q = glm::normalize(mc2::math::quatXYZW(0.2f, -0.4f, 0.1f, 0.85f));
    const glm::mat4 engine = mc2::math::rotationMatrix(q), cast = glm::transpose(glm::mat4_cast(q));
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) CHECK(std::fabs(engine[c][r] - cast[c][r]) < 1e-6f);
    CHECK(std::fabs(engine[0][1] - glm::mat4_cast(q)[0][1]) > 1e-3f);  // negative control: not glm's own orientation
    // quatXYZW keeps the engine argument order (glm's constructor takes w first)
    const glm::quat x = mc2::math::quatXYZW(1.0f, 2.0f, 3.0f, 4.0f);
    CHECK((x.x == 1.0f && x.y == 2.0f && x.z == 3.0f && x.w == 4.0f));
    // composeQuat(a, b) is "a then b": the same rotation as the matrix product rotationMatrix(a) * rotationMatrix(b)
    const glm::quat a = glm::normalize(mc2::math::quatXYZW(0.1f, 0.7f, -0.2f, 0.6f));
    const glm::mat4 lhs = mc2::math::rotationMatrix(mc2::math::composeQuat(a, q)),
                    rhs = mc2::math::rotationMatrix(a) * mc2::math::rotationMatrix(q);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) CHECK(std::fabs(lhs[c][r] - rhs[c][r]) < 1e-5f);
    // invertRigid is an inverse
    const glm::mat4 id = m * mc2::math::invertRigid(m);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(std::fabs(id[c][r] - (c == r ? 1.0f : 0.0f)) < 1e-5f);
}

TEST_CASE("glm engine math: byte formats old files read back (E1-E3)") {
    // E1: quaternions are x, y, z, w in memory (the anim cache stores these bytes).
    const glm::quat q = mc2::math::quatXYZW(1.0f, 2.0f, 3.0f, 4.0f);
    float qb[4];
    std::memcpy(qb, &q, sizeof(qb));
    CHECK((qb[0] == 1.0f && qb[1] == 2.0f && qb[2] == 3.0f && qb[3] == 4.0f));
    // E3: vectors are 12 bytes x, y, z.
    const glm::vec3 v(5.0f, 6.0f, 7.0f);
    float vb[3];
    static_assert(sizeof(v) == sizeof(vb));
    std::memcpy(vb, &v, sizeof(vb));
    CHECK((vb[0] == 5.0f && vb[1] == 6.0f && vb[2] == 7.0f));
    // E2: a matrix goes to a stream as the 48-byte affine block (entry (row r, column c) at float c * 4 + r) and comes back
    // with column 3 = (0, 0, 0, 1); an OBB is that block + extents + radius = 64 bytes.
    glm::mat4 m(1.0f);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 4; ++r) m[c][r] = static_cast<float>(c * 4 + r + 1);
    Stuff::DynamicMemoryStream stream(16);
    stream << m;
    CHECK(stream.GetBytesUsed() == 48);
    const float* bytes = static_cast<const float*>(stream.GetPointer()) - 12;
    for (int i = 0; i < 12; ++i) CHECK(bytes[i] == static_cast<float>(i + 1));
    stream.Rewind();
    glm::mat4 back(0.0f);
    stream >> back;
    CHECK(back[0] == m[0]);
    CHECK(back[2] == m[2]);
    CHECK(back[3] == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    Stuff::DynamicMemoryStream obbStream(16);
    const mc2::math::Obb obb(m, glm::vec3(1.0f, 2.0f, 3.0f), 9.0f);
    obbStream << obb;
    CHECK(obbStream.GetBytesUsed() == 64);
}
