// Trigger paths: mclib/math/, mclib/mathfunc.*, 3rdparty/detmath/.
// Golden bits from 100-decimal mpmath 1.3.0, independently rounded to binary32.
#include "math/sim_math.h"
#include "math/mc2_xform.h"
#include "mathfunc.h"
#include "dstd.h"
#include "doctest.h"
#include <array>
#include <bit>
#include <cstdint>
TEST_CASE("sim math portable golden bits") {
    namespace sm = mc2::sim::math;
    struct Row { std::uint32_t x, sine, cosine, arcCosine, arcSine, exponential; };
    constexpr Row rows[] = {
        {0x00000000u, 0x00000000u, 0x3f800000u, 0x3fc90fdbu, 0x00000000u, 0x3f800000u},
        {0x80000000u, 0x80000000u, 0x3f800000u, 0x3fc90fdbu, 0x80000000u, 0x3f800000u},
        {0x3e000000u, 0x3dff5577u, 0x3f7e00abu, 0x3fb9051du, 0x3e0055f0u, 0x3f910b02u},
        {0xbe000000u, 0xbdff5577u, 0x3f7e00abu, 0x3fd91a99u, 0xbe0055f0u, 0x3f61eb51u},
        {0x3f000000u, 0x3ef57744u, 0x3f60a940u, 0x3f860a92u, 0x3f060a92u, 0x3fd3094cu},
        {0xbf000000u, 0xbef57744u, 0x3f60a940u, 0x40060a92u, 0xbf060a92u, 0x3f1b4598u},
        {0x3f7fffffu, 0x3f576aa4u, 0x3f0a5141u, 0x39b504f3u, 0x3fc9048au, 0x402df854u},
        {0xbf7fffffu, 0xbf576aa4u, 0x3f0a5141u, 0x40490a32u, 0xbfc9048au, 0x3ebc5ab2u},
        {0x3f800000u, 0x3f576aa4u, 0x3f0a5140u, 0x00000000u, 0x3fc90fdbu, 0x402df854u},
        {0xbf800000u, 0xbf576aa4u, 0x3f0a5140u, 0x40490fdbu, 0xbfc90fdbu, 0x3ebc5ab2u},
        {0xbf7f504fu, 0xbf570b85u, 0x3f0ae4f7u, 0x40445fe4u, 0xbfbfafeeu, 0x3ebcdc22u},
    };
    for (const auto& r : rows) {
        const float x = std::bit_cast<float>(r.x);
        CAPTURE(r.x);
        CHECK(std::bit_cast<std::uint32_t>(sm::sin(x)) == r.sine);
        CHECK(std::bit_cast<std::uint32_t>(sm::cos(x)) == r.cosine);
        CHECK(std::bit_cast<std::uint32_t>(sm::acos(x)) == r.arcCosine);
        CHECK(std::bit_cast<std::uint32_t>(sm::asin(x)) == r.arcSine);
        CHECK(std::bit_cast<std::uint32_t>(sm::exp(x)) == r.exponential);
    }
    struct Binary { float a, b; std::uint32_t angle, power; };
    constexpr Binary binary[] = {
        {0.125f, 0.5f, 0x3e7adbb0u, 0x3eb504f3u},
        {0.5f, 0.125f, 0x3fa9b465u, 0x3f6ac0c7u},
        {1.0f, 1.0f, 0x3f490fdbu, 0x3f800000u},
        {1.0f, -1.0f, 0x4016cbe4u, 0x3f800000u},
        {-1.0f, -1.0f, 0xc016cbe4u, 0xbf800000u},
        {-1.0f, 1.0f, 0xbf490fdbu, 0xbf800000u},
        {2.0f, 3.0f, 0x3f168757u, 0x41000000u},
        {10.0f, 0.10000000149011612f, 0x3fc7c82fu, 0x3fa12478u},
    };
    for (const auto& r : binary) {
        CHECK(std::bit_cast<std::uint32_t>(sm::atan2(r.a, r.b)) == r.angle);
        CHECK(std::bit_cast<std::uint32_t>(sm::pow(r.a, r.b)) == r.power);
    }
    struct Trig { std::uint32_t x, sine, cosine; };
    constexpr Trig trig[] = {
        {0x00000001u, 0x00000001u, 0x3f800000u},
        {0x80000001u, 0x80000001u, 0x3f800000u},
        {0x3fc90fdbu, 0x3f800000u, 0xb33bbd2eu},
        {0x40490fdbu, 0xb3bbbd2eu, 0xbf800000u},
        {0x4116cbe4u, 0xb2ccde2eu, 0xbf800000u},
        {0x49800000u, 0x3ea93666u, 0x3f719d6du},
        {0x4f000001u, 0xbe4bc111u, 0xbf7ae188u},
        {0x7f7fffffu, 0xbf0599b3u, 0x3f5a5f96u},
    };
    for (const auto& r : trig) {
        const float x = std::bit_cast<float>(r.x);
        CHECK(std::bit_cast<std::uint32_t>(sm::sin(x)) == r.sine);
        CHECK(std::bit_cast<std::uint32_t>(sm::cos(x)) == r.cosine);
    }
    CHECK(std::bit_cast<std::uint32_t>(sm::acos(2.0f)) == 0x7fc00000u);
    CHECK(std::bit_cast<std::uint32_t>(sm::pow(-2.0f, 0.5f)) == 0x7fc00000u);
    CHECK(std::bit_cast<std::uint32_t>(sm::atan2(-0.0f, 1.0f)) == 0x80000000u);
}

TEST_CASE("sim math gameplay call sites") {
    const auto bits = [](float x) { return std::bit_cast<std::uint32_t>(x); };
    const auto value = [](std::uint32_t x) { return std::bit_cast<float>(x); };
    CHECK(bits(my_acos(value(0xbf7f504fu))) == 0x40445fe4u);
    CHECK(bits(my_acos(1.000001f)) == 0u);
    CHECK(bits(my_acos(-1.000001f)) == 0x40490fdbu);
    // atan's previously uncovered [19/16,39/16) reduction branch, including edges.
    for (const auto& r : std::array<std::pair<float, std::uint32_t>, 4>{{
             {19.0f/16.0f, 0x3f5ef387u}, {1.5f, 0x3f7b985fu},
             {2.0f, 0x3f8db70du}, {value(0x401bffffu), 0x3f973ab9u}}}) {
        CHECK(bits(mc2::sim::math::atan2(r.first, 1.0f)) == r.second);
        CHECK(bits(mc2_atan2(r.first, 1.0f)) == r.second);
    }
    CHECK(bits(mc2_atan2(-1.0f, 0.0f)) == bits(1.570796f));
    // Independent mpmath sine/cosine bits; this input differs from MSVC sinf.
    const float angle = value(0x3f476c8bu), sine = value(0x3f33db81u), cosine = value(0x3f362c7fu);
    for (int axis = 0; axis < 3; ++axis) {
        mc2::math::Euler e;
        e[axis] = angle;
        glm::mat4 reference(1.0f);
        const int a = (axis + 1) % 3, b = (axis + 2) % 3;
        reference[a][a] = reference[b][b] = cosine;
        reference[b][a] = sine;
        reference[a][b] = -sine;
        const auto expected = mc2::math::quatFromRotation(reference);
        const auto actual = mc2::math::quatFromEulerSim(e);
        for (int component = 0; component < 4; ++component)
            CHECK(bits(actual[component]) == bits(expected[component]));
    }
    glm::vec3 v(0.0f, 1.0f, 7.0f);
    Stuff::Vector2DOf<float> v2;
    v2.x = 0.0f; v2.y = 1.0f;
    const float degrees = angle / DEGREES_TO_RADS;
    REQUIRE(bits(degrees * DEGREES_TO_RADS) == bits(angle));
    Rotate(v, degrees);
    Rotate(v2, degrees);
    CHECK(bits(v.x) == bits(sine)); CHECK(bits(v.y) == bits(cosine));
    CHECK(v.z == 7.0f);
    CHECK(bits(v2.x) == bits(sine)); CHECK(bits(v2.y) == bits(cosine));
}
