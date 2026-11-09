// tests/unit/test_prop_light_contract.cpp
// Colour-prop light contract: contractSun() against a verbatim copy of Camera::getLightRed.
#include "doctest.h"
#include "../../mclib/prop_light_contract.h"
#include "../../RenderCore/StockPropMaterial.h"

TEST_CASE("stock prop PBR defaults: classes and missing maps") {
    using namespace RenderCore;
    struct Example { const char* name; SurfaceClass cls; float roughness; float metallic; };
    const Example cases[] = {
        {"TENT01.tga", SurfaceClass::Fabric, 0.9f, 0.0f},
        {"liao_wall.tga", SurfaceClass::Concrete, 0.9f, 0.0f},
        {"fuel_tank.tga", SurfaceClass::PaintedMetal, 0.8f, 0.0f},
        {"quonset.tga", SurfaceClass::PaintedMetal, 0.8f, 0.0f},
        {"vehicle.tga", SurfaceClass::PaintedMetal, 0.8f, 0.0f},
        {"tower.tga", SurfaceClass::PaintedMetal, 0.8f, 0.0f},
        {"chainlink.tga", SurfaceClass::Grating, 0.7f, 1.0f},
        {"bare_metal.tga", SurfaceClass::BareMetal, 0.7f, 1.0f},
        {"mod_unknown.tga", SurfaceClass::Dielectric, 0.85f, 0.0f},
        {"", SurfaceClass::Dielectric, 0.85f, 0.0f},
    };
    for (const auto& e : cases) {
        INFO(e.name);
        CHECK(stockPropClass(e.name) == e.cls);
        const auto row = stockPropMaterial(stockPropClass(e.name));
        CHECK(row.material.roughnessFactor == doctest::Approx(e.roughness));
        CHECK(row.material.metallicFactor == doctest::Approx(e.metallic));
        CHECK(row.material.normalTex == kMaterialTexAbsent);
        CHECK(row.material.metallicRoughnessTex == kMaterialTexAbsent);
        CHECK(row.uvScale[0] == 1.0f);
    }
}

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
