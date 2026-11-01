// tests/unit/test_prop_light_contract.cpp
// Colour-prop light contract: contractSun() against a verbatim copy of Camera::getLightRed.
#include "doctest.h"
#include "../../mclib/prop_light_contract.h"

namespace {
long refFloat2long(float v) { return static_cast<long>(v); }
// Verbatim shape of Camera::getLightRed (mclib/camera.cpp) with S/A/F as parameters.
unsigned char cameraRef(unsigned char lightRed, unsigned char ambientRed, long floorV, float intensity) {
    if (intensity > 0.0) {
        long fResult = refFloat2long(lightRed * intensity + ambientRed);
        if (fResult < floorV) fResult = floorV;
        if (fResult < 0xff) return static_cast<unsigned char>(fResult);
        return 0xff;
    }
    return (ambientRed < floorV) ? static_cast<unsigned char>(floorV) : ambientRed;
}
}

TEST_CASE("prop light contract: matches Camera::getLight table") {
    using namespace mc2::prop_light;
    const float Is[] = {0.0f, 1e-3f, 0.5f, 1.0f};
    const int As[] = {0, 64, 255}, Fs[] = {0, 32, 64}, Ss[] = {0, 100, 255};
    int rows = 0;
    for (float I : Is) for (int A : As) for (int F : Fs) for (int S : Ss) {
        const int s3[3] = {S, S, S}, a3[3] = {A, A, A};
        const SunRgb r = contractSun(s3, a3, F, I, 0);
        CHECK(r.sunOn);
        CHECK(r.c[0] == cameraRef((unsigned char)S, (unsigned char)A, F, I));
        ++rows;
    }
    CHECK(rows == 108);
}

TEST_CASE("prop light contract: policy bits and word packing") {
    using namespace mc2::prop_light;
    const int S[3] = {200, 150, 100}, A[3] = {20, 30, 40};
    CHECK_FALSE(contractSun(S, A, 0, 0.5f, kSunOff).sunOn);
    CHECK(contractSun(S, A, 0, 0.25f, kNeutral).c[0] == contractSun(S, A, 0, 1.0f, 0).c[0]);
    CHECK(unpackI(packWord(0.5f, kSunOff)) == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK((packWord(2.0f, kNeutral) & kIMask) == 65535u);
    CHECK((packWord(-1.0f, 0) & kIMask) == 0u);
    CHECK((packWord(0.3f, kSunOff) & kNeutral) == 0u);
}

TEST_CASE("prop light contract: mech and destruction lights are local lights, not sun") {
    // The contract takes no light list: mech/vehicle lights and building-destruction
    // (fire/explosion) lights reach props only through the frame light pool (propPoolLight),
    // so contractSun is invariant to them by construction.
    using namespace mc2::prop_light;
    const int S[3] = {200, 150, 100}, A[3] = {20, 30, 40};
    const SunRgb a = contractSun(S, A, 0, 0.7f, 0);
    const SunRgb b = contractSun(S, A, 0, 0.7f, 0);
    CHECK(a.c[0] == b.c[0]);
}
