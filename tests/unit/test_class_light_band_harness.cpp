#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "RenderCore/IblShCoeffs.h"
#include "RenderCore/IblShRegistry.h"
#include "Gates/render_options.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct V3 {
    double x = 0, y = 0, z = 0;
};
static V3   v3(double x, double y, double z) { return V3{x, y, z}; }
static double dot(const V3& a, const V3& b)  { return a.x*b.x + a.y*b.y + a.z*b.z; }
static double len(const V3& a)               { return std::sqrt(dot(a, a)); }
static V3   norm(const V3& a) {
    const double l = len(a);
    return (l > 1e-12) ? v3(a.x/l, a.y/l, a.z/l) : a;
}
static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static const double kDeg2Rad = 3.14159265358979323846 / 180.0;

struct Mission {
    const char* name;
    int lightR, lightG, lightB;
    int ambR,   ambG,   ambB;
    double pitch;
    double yaw;
};

static const Mission kMissions[] = {
    { "mc2_01", 255,255,225,  71, 71, 78,  50.000000,   11.000000 },
    { "mc2_02", 255,255,255,  55, 55, 55,  22.500000,  253.750000 },
    { "mc2_03", 242,242,225,  31, 31, 31,  46.000000, -206.000000 },
    { "mc2_04", 255,255,255, 128,128,128,  35.000000,   -0.500000 },
    { "mc2_05",  75, 75,116,  42, 42, 42, 270.000000,   15.000000 },
    { "mc2_06", 255,250,240, 106,121,145,  25.000000,  100.000000 },
    { "mc2_07", 255,255,200, 105,105,100,  34.250000,   12.000000 },
    { "mc2_08", 255,242,218,  73, 84,101,  37.500000, -196.250000 },
    { "mc2_09", 192,192,192, 150,150,150,  22.500000,   28.750000 },
    { "mc2_10", 255,255,255, 192,192,192, 180.000000,   28.750000 },
    { "mc2_11", 255,255,255,  77, 62, 62,  22.500000,  -16.250000 },
    { "mc2_12", 255,255,255,  31, 31, 31,  22.500000,   28.750000 },
    { "mc2_13", 255,255,255,  31, 31, 31,  37.000000,  210.000000 },
    { "mc2_14", 255,106, 43,  95, 95, 95,  20.000000,  -42.000000 },
    { "mc2_15", 255,255,255,  69, 69, 69,  22.500000,   28.750000 },
    { "mc2_16", 240,255,255,  96, 98, 98,  38.000000, -233.000000 },
    { "mc2_17", 255,255,255,  31, 31, 31,  22.500000, -173.750000 },
    { "mc2_18", 255,255,255,  31, 31, 31,  33.740002,   28.740000 },
    { "mc2_19", 255,255,255, 128,128,128,   0.000000,  -38.750000 },
    { "mc2_20", 255,242,218,  66, 75, 90,  44.000000, -207.250000 },
    { "mc2_21",  82, 82,135, 136,136,136, 270.000000,   15.000000 },
    { "mc2_22", 255,255,255, 192,192,192,  30.000000,    6.250000 },
    { "mc2_23",  80,106,133,  83, 83,123,   9.000000,   28.500000 },
    { "mc2_24", 255,255,255,  74, 74, 74,  75.000000,  300.000000 },
};
static const int kNumMissions = (int)(sizeof(kMissions) / sizeof(kMissions[0]));

static long float2long(double v) { return (long)v; }

static const long kNightAmbientFloorDefault = 0;

static int getLightByte(int lightByte, int ambientByte, double intensity,
                        long floorV = kNightAmbientFloorDefault)
{
    if (intensity > 0.0) {
        long f = float2long(lightByte * intensity + ambientByte);
        if (f < floorV) f = floorV;
        return (f < 0xff) ? (int)f : 0xff;
    }
    return (ambientByte < floorV) ? (int)floorV : ambientByte;
}

static V3 terrainSunMC2(double pitchDeg, double yawDeg)
{
    const double a = (yawDeg + 45.0) * kDeg2Rad;
    return norm(v3(std::cos(a), std::sin(a), std::sin(pitchDeg * kDeg2Rad)));
}

static V3 objectRowLightDir(double pitchDeg, double yawDeg)
{
    const double P = pitchDeg * kDeg2Rad;
    const double Y = (yawDeg + 135.0) * kDeg2Rad;

    return v3(std::cos(P) * std::sin(Y), -std::sin(P), std::cos(P) * std::cos(Y));
}

static V3 swapStuffToMC2(const V3& v) { return v3(-v.x, v.z, v.y); }

static double evalShL2(const V3& n, int channel)
{
    const double c1 = 0.429043, c2 = 0.511664, c3 = 0.743125,
                 c4 = 0.886227, c5 = 0.247708;
    const auto& C = RenderCore::kIblShCoeffs;
    const double sh0 = C[0][channel], sh1 = C[1][channel], sh2 = C[2][channel],
                 sh3 = C[3][channel], sh4 = C[4][channel], sh5 = C[5][channel],
                 sh6 = C[6][channel], sh7 = C[7][channel], sh8 = C[8][channel];
    return c1 * sh8 * (n.x * n.x - n.z * n.z)
         + c3 * sh6 * (n.y * n.y)
         + c4 * sh0
         - c5 * sh6
         + 2.0 * c1 * sh4 * (n.x * n.y)
         + 2.0 * c1 * sh5 * (n.y * n.z)
         + 2.0 * c1 * sh7 * (n.x * n.z)
         + 2.0 * c2 * sh3 * n.x
         + 2.0 * c2 * sh1 * n.y
         + 2.0 * c2 * sh2 * n.z;
}

static const double kIblStrengthDefault = 0.5;

static const double kIblStrengthShippedDefault = 0.55;

static double iblStrengthShipped(const char* missionName)
{
    const std::string m(missionName);
    if (m == "mc2_01" || m == "mc2_10" || m == "mc2_24") return 0.60;
    if (m == "mc2_03") return 0.55;
    return kIblStrengthShippedDefault;
}

static double terrainMul(double ndl)
{
    const double diffuse = clampd(ndl, 0.02, 1.0);
    return 0.35 + (1.20 - 0.35) * diffuse;
}
static const double kTerrainFloor = 0.35 + (1.20 - 0.35) * 0.02;
static const double kTerrainCeil  = 1.20;

static double cementMul() { return 1.0; }

static double mechMul(double A, double S, double ndl)
{
    return clampd(0.35 + A + S * ndl, 0.0, 1.0);
}

static double mechMulUnclamped(double A, double S, double ndl)
{
    return 0.35 + A + S * ndl;
}

static double vehicleMul(double seed, double A, double S, double ndl)
{
    return clampd(std::max(seed, 0.35) + A + S * ndl, 0.0, 1.0);
}

static double vehicleMulNoFloor(double seed, double A, double S, double ndl)
{
    return seed + A + S * ndl;
}

static double propMulPreClamp(double seed, double A, double S, double ndl, double ibl)
{
    return seed + A + S * ndl + ibl;
}
static double propMul(double seed, double A, double S, double ndl, double ibl)
{
    return clampd(propMulPreClamp(seed, A, S, ndl, ibl), 0.0, 1.0);
}

static double treeMulStock(double seed, double S)
{
    return clampd(seed + S, 0.0, 1.0);
}

static double treeMulHead(double seed, double S, double ibl)
{
    return clampd(seed + S + ibl, 0.0, 1.0);
}

static int stockKernelByte(int seedByte, int ambientByte, int sunByte, double cosine)
{
    long finalV = seedByte;
    if (cosine < 0.0)
        finalV += float2long(sunByte * std::fabs(cosine));
    finalV += ambientByte;
    if (finalV > 255) finalV = 255;
    if (finalV < 0)   finalV = 0;
    return (int)finalV;
}

static double stockMul(double seed, double A, double S, double ndl)
{
    return clampd(seed + A + S * ndl, 0.0, 1.0);
}

struct MissionLight {
    double A;
    double S;
    double intensity;
    V3     terrainSun;
    V3     rowLightDir;
};

static MissionLight missionLight(const Mission& m)
{
    MissionLight out{};
    out.terrainSun  = terrainSunMC2(m.pitch, m.yaw);
    out.rowLightDir = objectRowLightDir(m.pitch, m.yaw);
    out.intensity   = out.terrainSun.z;
    out.A = m.ambR / 255.0;
    out.S = getLightByte(m.lightR, m.ambR, out.intensity) / 255.0;
    return out;
}

static double propIbl(const V3& nStuff, double strength = kIblStrengthDefault)
{
    return evalShL2(norm(nStuff), 0) * strength;
}

static V3 iblBrightAxisStuff(int channel)
{
    const auto& C = RenderCore::kIblShCoeffs;
    return norm(v3(C[3][channel], C[1][channel], C[2][channel]));
}

static double stuffAzimuthDeg(const V3& v)
{
    return std::atan2(v.z, v.x) / kDeg2Rad;
}

static double wrap180(double deg)
{
    double d = std::fmod(deg + 180.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}

static V3 rotAboutStuffY(const V3& n, double tRad)
{
    const double c = std::cos(tRad), s = std::sin(tRad);
    return v3(n.x * c - n.z * s, n.y, n.x * s + n.z * c);
}

struct ShSet9 { double c[9][3]; };

static ShSet9 shippedShSet()
{
    ShSet9 out{};
    for (int i = 0; i < 9; ++i)
        for (int ch = 0; ch < 3; ++ch)
            out.c[i][ch] = RenderCore::kIblShCoeffs[i][ch];
    return out;
}

static ShSet9 rotateShAboutY(const ShSet9& in, double tRad)
{
    ShSet9 o = in;
    const double c1t = std::cos(tRad),       s1t = std::sin(tRad);
    const double c2t = std::cos(2.0 * tRad), s2t = std::sin(2.0 * tRad);
    for (int ch = 0; ch < 3; ++ch) {
        const double a3 = in.c[3][ch], b2 = in.c[2][ch];
        o.c[3][ch] = a3 * c1t - b2 * s1t;
        o.c[2][ch] = a3 * s1t + b2 * c1t;
        const double a4 = in.c[4][ch], b5 = in.c[5][ch];
        o.c[4][ch] = a4 * c1t - b5 * s1t;
        o.c[5][ch] = a4 * s1t + b5 * c1t;
        const double a8 = in.c[8][ch], b7 = in.c[7][ch];
        o.c[8][ch] = a8 * c2t - b7 * s2t;
        o.c[7][ch] = a8 * s2t + b7 * c2t;
    }
    return o;
}

static double evalShL2Set(const ShSet9& S, const V3& n, int channel)
{
    const double c1 = 0.429043, c2 = 0.511664, c3 = 0.743125,
                 c4 = 0.886227, c5 = 0.247708;
    const double sh0 = S.c[0][channel], sh1 = S.c[1][channel], sh2 = S.c[2][channel],
                 sh3 = S.c[3][channel], sh4 = S.c[4][channel], sh5 = S.c[5][channel],
                 sh6 = S.c[6][channel], sh7 = S.c[7][channel], sh8 = S.c[8][channel];
    return c1 * sh8 * (n.x * n.x - n.z * n.z)
         + c3 * sh6 * (n.y * n.y)
         + c4 * sh0
         - c5 * sh6
         + 2.0 * c1 * sh4 * (n.x * n.y)
         + 2.0 * c1 * sh5 * (n.y * n.z)
         + 2.0 * c1 * sh7 * (n.x * n.z)
         + 2.0 * c2 * sh3 * n.x
         + 2.0 * c2 * sh1 * n.y
         + 2.0 * c2 * sh2 * n.z;
}

static V3 missionSunStuff(const Mission& m)
{
    return swapStuffToMC2(terrainSunMC2(m.pitch, m.yaw));
}

static double sunAlignRotationRad(const Mission& m)
{
    const double aSun   = stuffAzimuthDeg(missionSunStuff(m));
    const double aProbe = stuffAzimuthDeg(iblBrightAxisStuff(0));
    return wrap180(aSun - aProbe) * kDeg2Rad;
}

static double probeVsSunAzErrorDeg(const Mission& m)
{
    return wrap180(stuffAzimuthDeg(missionSunStuff(m))
                 - stuffAzimuthDeg(iblBrightAxisStuff(0)));
}

struct RotSweep {
    double movedFrac;
    double meanAbs;
    double maxDown;
    double maxUp;
    double unsatFrac;
};

static RotSweep rotationSweep(const Mission& m, int samples = 720)
{
    const MissionLight L = missionLight(m);
    const double st  = iblStrengthShipped(m.name);
    const double t   = sunAlignRotationRad(m);
    const ShSet9 base = shippedShSet();
    const ShSet9 rot  = rotateShAboutY(base, t);
    RotSweep s{};
    int moved = 0, unsat = 0;
    double acc = 0.0;
    for (int k = 0; k < samples; ++k) {
        const double az = 2.0 * 3.14159265358979323846 * k / samples;

        const V3 nMC2   = v3(std::cos(az), std::sin(az), 0.0);
        const V3 nStuff = swapStuffToMC2(nMC2);
        const double ndl = std::max(0.0, dot(nMC2, L.terrainSun));
        const double before = propMul(0.0, L.A, L.S, ndl, evalShL2Set(base, nStuff, 0) * st);
        const double after  = propMul(0.0, L.A, L.S, ndl, evalShL2Set(rot,  nStuff, 0) * st);
        const double d = after - before;
        if (std::fabs(d) > 1.0 / 255.0) ++moved;
        if (before < 0.999) ++unsat;
        acc += std::fabs(d);
        s.maxDown = std::min(s.maxDown, d);
        s.maxUp   = std::max(s.maxUp,   d);
    }
    s.movedFrac = (double)moved / samples;
    s.meanAbs   = acc / samples;
    s.unsatFrac = (double)unsat / samples;
    return s;
}

TEST_CASE("class_light_band_harness/terrain_floor_is_not_0_35") {
    CHECK((std::fabs(terrainMul(-1.0)  - 0.367) < 1e-6));
    CHECK((std::fabs(terrainMul(0.0)   - 0.367) < 1e-6));
    CHECK((std::fabs(terrainMul(1.0)   - 1.20)  < 1e-9));
    CHECK((std::fabs(kTerrainFloor     - 0.367) < 1e-6));

    CHECK((kTerrainFloor > 0.35));
    return;
}

TEST_CASE("class_light_band_harness/terrain_band_is_mission_independent") {
    for (int i = 0; i < kNumMissions; ++i) {
        CHECK((std::fabs(terrainMul(0.0) - kTerrainFloor) < 1e-9));
        CHECK((std::fabs(terrainMul(1.0) - kTerrainCeil)  < 1e-9));
    }
    return;
}

TEST_CASE("class_light_band_harness/cement_is_exactly_one_and_exempt") {
    CHECK((cementMul() == 1.0));
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        CHECK((cementMul() >= mechMul(L.A, L.S, 1.0)));
    }
    return;
}

TEST_CASE("class_light_band_harness/ported_camera_math_reproduces_independent_anchors") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        if (std::string(m.name) == "mc2_10") {
            CHECK((std::fabs(L.A - 0.7529) < 5e-5));
            CHECK((std::fabs(L.S - 0.7529) < 5e-5));
        }
        if (std::string(m.name) == "mc2_03") {
            CHECK((std::fabs(L.A - 0.1216) < 5e-5));
            CHECK((std::fabs(L.S - 0.6745) < 5e-5));
        }
    }
    return;
}

TEST_CASE("class_light_band_harness/mc2_10_mech_is_saturated_at_every_ndl") {
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_10") continue;
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 20; ++k)
            CHECK((mechMul(L.A, L.S, k / 20.0) == 1.0));

        CHECK((mechMulUnclamped(L.A, L.S, 1.0) > 1.8));
    }
    return;
}

TEST_CASE("class_light_band_harness/inherited_prediction_threshold_is_wrong") {

    const double claimedThreshold = 0.35 * 255.0;
    const double actualThreshold  = kTerrainFloor * 255.0;
    CHECK((std::fabs(claimedThreshold - 89.25) < 1e-6));
    CHECK((actualThreshold > 93.0 && actualThreshold < 94.0));

    int inBand = 0;
    for (int i = 0; i < kNumMissions; ++i)
        if (kMissions[i].ambR > 89 && kMissions[i].ambR <= 93) ++inBand;

    CHECK((inBand == 0));
    return;
}

TEST_CASE("class_light_band_harness/inherited_prediction_mission_list_FAILS") {
    std::vector<std::string> predicted = {"mc2_03","mc2_12","mc2_13","mc2_17","mc2_18"};
    std::vector<std::string> actual;
    for (int i = 0; i < kNumMissions; ++i)
        if (kMissions[i].ambR < 89) actual.push_back(kMissions[i].name);

    CHECK((actual.size() == 14));
    CHECK((actual.size() > predicted.size()));
    for (const auto& p : predicted) {
        CHECK((std::find(actual.begin(), actual.end(), p) != actual.end()));
    }

    std::vector<std::string> amb31;
    for (int i = 0; i < kNumMissions; ++i)
        if (kMissions[i].ambR == 31) amb31.push_back(kMissions[i].name);
    CHECK((amb31 == predicted));
    return;
}

static void forEachNormal(int n, const std::function<void(const V3&)>& fn)
{
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            const double th = 3.14159265358979 * (a + 0.5) / n;
            const double ph = 2.0 * 3.14159265358979 * (b + 0.5) / n;
            fn(v3(std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph)));
        }
    }
}
static double iblMinOverSphere()
{
    double lo = 1e9;
    forEachNormal(96, [&](const V3& n) { lo = std::min(lo, propIbl(n)); });
    return lo;
}

TEST_CASE("class_light_band_harness/prediction_is_ill_posed_prop_floor_is_normal_dependent") {
    const double iblMin = iblMinOverSphere();
    CHECK((iblMin > 0.0));
    CHECK((iblMin > 0.10 && iblMin < 0.13));

    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        double lo = 1e9, hi = -1e9;
        forEachNormal(48, [&](const V3& n) {
            const double v = propMulPreClamp(0.0, L.A, L.S, 0.0, propIbl(n));
            lo = std::min(lo, v); hi = std::max(hi, v);
        });

        CHECK((hi > kTerrainFloor));
        CHECK((hi / lo > 2.0));
    }
    return;
}

TEST_CASE("class_light_band_harness/corrected_crossing_set_is_seven_not_five") {
    const double iblMin = iblMinOverSphere();
    std::vector<std::string> predicted = {"mc2_03","mc2_12","mc2_13","mc2_17","mc2_18"};
    std::vector<std::string> actual;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        if (propMulPreClamp(0.0, L.A, L.S, 0.0, iblMin) < kTerrainFloor)
            actual.push_back(kMissions[i].name);
    }
    CHECK((actual.size() == 7));
    for (const auto& p : predicted)
        CHECK((std::find(actual.begin(), actual.end(), p) != actual.end()));
    CHECK((std::find(actual.begin(), actual.end(), std::string("mc2_02")) != actual.end()));
    CHECK((std::find(actual.begin(), actual.end(), std::string("mc2_05")) != actual.end()));
    return;
}

TEST_CASE("class_light_band_harness/prop_ibl_additive_dominates_the_budget") {
    double lo = 1e9, hi = -1e9;
    for (int a = 0; a < 128; ++a) {
        for (int b = 0; b < 128; ++b) {
            const double th = 3.14159265358979 * (a + 0.5) / 128.0;
            const double ph = 2.0 * 3.14159265358979 * (b + 0.5) / 128.0;
            const V3 n = v3(std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph));
            const double v = propIbl(n);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
    }

    CHECK((lo > 0.10 && lo < 0.13));
    CHECK((hi > 2.4 && hi < 2.5));
    CHECK((hi / lo > 20.0));

    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const MissionLight L = missionLight(kMissions[i]);
        CHECK((hi > 2.0 * (L.A + L.S)));
    }
    return;
}

TEST_CASE("class_light_band_harness/props_saturate_on_a_large_normal_fraction") {
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const MissionLight L = missionLight(kMissions[i]);
        int sat = 0, total = 0;
        for (int a = 0; a < 64; ++a) {
            for (int b = 0; b < 64; ++b) {
                const double th = 3.14159265358979 * (a + 0.5) / 64.0;
                const double ph = 2.0 * 3.14159265358979 * (b + 0.5) / 64.0;
                const V3 n = v3(std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph));

                if (propMulPreClamp(0.0, L.A, L.S, 0.0, propIbl(n)) >= 1.0) ++sat;
                ++total;
            }
        }
        CHECK((sat > 0));
        CHECK(((double)sat / total > 0.20));
    }
    return;
}

TEST_CASE("class_light_band_harness/vehicle_band_equals_mech_band_at_head_default") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 8; ++k) {
            const double ndl = k / 8.0;

            CHECK((std::fabs(vehicleMul(0.0, L.A, L.S, ndl)
                                - mechMul(L.A, L.S, ndl)) < 1e-12));
        }
    }
    return;
}

TEST_CASE("class_light_band_harness/gate_off_vehicle_band_differs_from_head") {

    int diverged = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        if (std::fabs(vehicleMulNoFloor(0.0, L.A, L.S, 0.0)
                    - mechMul(L.A, L.S, 0.0)) > 1e-9) ++diverged;
    }
    CHECK((diverged == kNumMissions));
    return;
}

TEST_CASE("class_light_band_harness/worst_pairwise_ratio_is_driven_by_the_exempt_surfaces") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const double ndl = 0.0;
        const double mech   = mechMul(L.A, L.S, ndl);
        const double terr   = terrainMul(ndl);
        const double cem    = cementMul();
        const double lo     = std::min(std::min(mech, terr), cem);
        const double hi     = std::max(std::max(mech, terr), cem);
        CHECK((hi / lo >= 1.0));
        CHECK((hi == cem));
    }
    return;
}

static double ndlMechDiffuse(const V3& nMC2, const V3& rowDir)
{
    return clampd(dot(nMC2, v3(-rowDir.x, -rowDir.y, -rowDir.z)), 0.0, 1.0);
}
static double ndlMechSpecular(const V3& nMC2, const V3& rowDir)
{
    const V3 s = swapStuffToMC2(rowDir);
    return clampd(dot(nMC2, v3(-s.x, -s.y, -s.z)), 0.0, 1.0);
}
static double ndlProp(const V3& nMC2, const V3& terrainSun)
{
    return std::max(dot(norm(nMC2), norm(terrainSun)), 0.0);
}

TEST_CASE("class_light_band_harness/prop_consumer_is_self_consistent") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        CHECK((std::fabs(ndlProp(L.terrainSun, L.terrainSun) - 1.0) < 1e-9));
    }
    return;
}

TEST_CASE("class_light_band_harness/mech_diffuse_sun_is_wrong_axis") {
    int zeroed = 0, tested = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const double d = ndlMechDiffuse(L.terrainSun, L.rowLightDir);
        ++tested;
        if (d <= 1e-9) ++zeroed;
    }

    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        CHECK((ndlMechDiffuse(L.terrainSun, L.rowLightDir) < 0.999));
    }
    CHECK((zeroed > kNumMissions / 2));
    CHECK((tested == kNumMissions));
    return;
}

TEST_CASE("class_light_band_harness/swapped_feed_recovers_the_true_spherical_sun") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 rowDir = objectRowLightDir(m.pitch, m.yaw);
        const V3 s = swapStuffToMC2(rowDir);
        const V3 minusSwap = v3(-s.x, -s.y, -s.z);
        const double a = (m.yaw + 45.0) * kDeg2Rad;
        const double P = m.pitch * kDeg2Rad;
        const V3 truth = v3(std::cos(P)*std::cos(a), std::cos(P)*std::sin(a), std::sin(P));
        CHECK((std::fabs(minusSwap.x - truth.x) < 1e-9));
        CHECK((std::fabs(minusSwap.y - truth.y) < 1e-9));
        CHECK((std::fabs(minusSwap.z - truth.z) < 1e-9));
        CHECK((std::fabs(len(minusSwap) - 1.0) < 1e-9));
    }
    return;
}

TEST_CASE("class_light_band_harness/swapped_feed_and_terrain_sun_share_azimuth") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 s = swapStuffToMC2(objectRowLightDir(m.pitch, m.yaw));
        const V3 minusSwap = v3(-s.x, -s.y, -s.z);
        const V3 ts = terrainSunMC2(m.pitch, m.yaw);
        const double azA = std::atan2(minusSwap.y, minusSwap.x);
        const double azB = std::atan2(ts.y, ts.x);
        double d = std::fabs(azA - azB);
        while (d > 3.14159265358979) d = std::fabs(d - 2.0 * 3.14159265358979);

        const bool flipped = std::cos(m.pitch * kDeg2Rad) < 0.0;
        if (flipped) CHECK((std::fabs(d - 3.14159265358979) < 1e-9));
        else         CHECK((d < 1e-9));
    }
    return;
}

TEST_CASE("class_light_band_harness/terrain_sun_elevation_is_atan_sin_pitch_not_pitch") {
    int mismatched = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 ts = terrainSunMC2(m.pitch, m.yaw);
        const double elevActual = std::asin(clampd(ts.z, -1.0, 1.0));
        const double elevAuthored = m.pitch * kDeg2Rad;
        const double elevPredicted = std::atan(std::sin(elevAuthored));
        CHECK((std::fabs(elevActual - elevPredicted) < 1e-9));
        if (std::fabs(elevActual - elevAuthored) > 1e-6) ++mismatched;
    }

    CHECK((mismatched >= kNumMissions - 3));

    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const V3 ts = terrainSunMC2(kMissions[i].pitch, kMissions[i].yaw);
        const double elevDeg = std::asin(clampd(ts.z, -1.0, 1.0)) / kDeg2Rad;
        CHECK((elevDeg > 35.0 && elevDeg < 36.0));
    }
    return;
}

TEST_CASE("class_light_band_harness/mc2_10_saturation_hides_the_frame_error") {
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_10") continue;
        const MissionLight L = missionLight(kMissions[i]);
        const V3 s = swapStuffToMC2(L.rowLightDir);
        const V3 correctSun = v3(-s.x, -s.y, -s.z);
        CHECK((std::fabs(dot(correctSun, L.terrainSun) + 1.0) < 1e-9));

        const double ndlA = ndlProp(L.terrainSun, L.terrainSun);
        const double ndlB = ndlMechDiffuse(L.terrainSun, L.rowLightDir);
        CHECK((std::fabs(ndlA - ndlB) > 0.5));

        CHECK((mechMul(L.A, L.S, ndlA) == mechMul(L.A, L.S, ndlB)));

        CHECK((std::fabs(mechMulUnclamped(L.A, L.S, ndlA)
                            - mechMulUnclamped(L.A, L.S, ndlB)) > 0.5));
    }
    return;
}

TEST_CASE("class_light_band_harness/stock_byte_and_normalised_forms_agree") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        const int sunByte = getLightByte(m.lightR, m.ambR, L.intensity);
        for (int k = 0; k <= 10; ++k) {
            const double ndl = k / 10.0;
            const int  b = stockKernelByte(0, m.ambR, sunByte, -ndl);
            const double n = stockMul(0.0, L.A, L.S, ndl);
            CHECK((std::fabs(b / 255.0 - n) <= 1.0 / 255.0 + 1e-9));
        }
    }
    return;
}

TEST_CASE("class_light_band_harness/mech_is_brighter_than_stock_everywhere") {
    double worstLift = 0.0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 10; ++k) {
            const double ndl = k / 10.0;
            const double stock = stockMul(0.0, L.A, L.S, ndl);
            const double mech  = mechMul(L.A, L.S, ndl);
            CHECK((mech >= stock - 1e-12));
            worstLift = std::max(worstLift, mech - stock);
        }
    }

    CHECK((std::fabs(worstLift - 0.35) < 1e-9));
    return;
}

TEST_CASE("class_light_band_harness/vehicle_deviates_from_stock_exactly_as_mech_does") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 10; ++k) {
            const double ndl   = k / 10.0;
            const double stock = stockMul(0.0, L.A, L.S, ndl);
            CHECK((std::fabs((vehicleMul(0.0, L.A, L.S, ndl) - stock)
                                - (mechMul(L.A, L.S, ndl)         - stock)) < 1e-12));
        }
    }
    return;
}

TEST_CASE("class_light_band_harness/prop_is_also_brighter_than_stock") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        int propAbove = 0, propAboveMech = 0, total = 0;
        for (int a = 0; a < 32; ++a) {
            for (int b = 0; b < 32; ++b) {
                const double th = 3.14159265358979 * (a + 0.5) / 32.0;
                const double ph = 2.0 * 3.14159265358979 * (b + 0.5) / 32.0;
                const V3 n = v3(std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph));
                const double ndl   = std::max(0.0, dot(n, v3(0, 1, 0)));
                const double stock = stockMul(0.0, L.A, L.S, ndl);
                const double prop  = propMul(0.0, L.A, L.S, ndl, propIbl(n));
                const double mech  = mechMul(L.A, L.S, ndl);
                if (prop >= stock - 1e-12)  ++propAbove;
                if (prop >= mech  - 1e-12)  ++propAboveMech;
                ++total;
            }
        }
        CHECK((propAbove == total));
        CHECK(((double)propAboveMech / total > 0.5));
    }
    return;
}

TEST_CASE("class_light_band_harness/all_three_object_classes_exceed_stock_by_different_amounts") {
    int collapsedByClamp = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const V3 nUp = v3(0, 1, 0);
        const double ndl = 0.0;

        const double stock = 0.0 + L.A + L.S * ndl;
        const double mech  = mechMulUnclamped(L.A, L.S, ndl)                    - stock;
        const double veh   = (std::max(0.0, 0.35) + L.A + L.S * ndl)            - stock;
        const double prop  = propMulPreClamp(0.0, L.A, L.S, ndl, propIbl(nUp))  - stock;
        CHECK((mech > 0.0 && veh > 0.0 && prop > 0.0));
        CHECK((std::fabs(mech - veh) < 1e-12));
        CHECK((std::fabs(prop - mech) > 1e-9));
        CHECK((std::fabs(mech - 0.35) < 1e-12));
        CHECK((std::fabs(prop - 0.807675) < 1e-4));

        if (std::fabs(propMul(0.0, L.A, L.S, ndl, propIbl(nUp))
                    - mechMul(L.A, L.S, ndl)) < 1e-12) ++collapsedByClamp;
    }

    CHECK((collapsedByClamp > 0));
    return;
}

TEST_CASE("class_light_band_harness/stage_c_conclusion_is_seed_independent") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int s = 0; s <= 255; ++s) {
            const double seed = s / 255.0;
            for (int k = 0; k <= 4; ++k) {
                const double ndl   = k / 4.0;
                const double stock = stockMul(seed, L.A, L.S, ndl);

                const double veh  = vehicleMul(seed, L.A, L.S, ndl);
                const double prop = propMul(seed, L.A, L.S, ndl, propIbl(v3(0,1,0)));
                CHECK((veh  >= stock - 1e-12));
                CHECK((prop >= stock - 1e-12));
            }
        }
    }
    return;
}

TEST_CASE("class_light_band_harness/mech_falls_below_stock_only_for_seeds_above_the_floor") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int s = 0; s <= 255; ++s) {
            const double seed  = s / 255.0;
            const double stock = stockMul(seed, L.A, L.S, 0.0);
            const double mech  = mechMul(L.A, L.S, 0.0);
            if (mech < stock - 1e-12) CHECK((seed > 0.35));
        }

        CHECK((0.3922 > 0.35));
    }
    return;
}

TEST_CASE("class_light_band_harness/terrain_band_ignores_the_mission_light_colours") {

    const MissionLight a = missionLight(kMissions[2]);
    const MissionLight b = missionLight(kMissions[9]);
    CHECK((std::fabs(mechMul(a.A, a.S, 0.0) - mechMul(b.A, b.S, 0.0)) > 0.4));
    CHECK((terrainMul(0.0) == terrainMul(0.0)));
    CHECK((std::fabs(terrainMul(0.5) - (0.35 + 0.85 * 0.5)) < 1e-9));
    return;
}

TEST_CASE("class_light_band_harness/sentinel_terrain_constants") {
    CHECK((std::fabs(terrainMul(1.0) - 1.20) < 1e-12));
    CHECK((std::fabs(terrainMul(0.0) - 0.367) < 1e-12));
    return;
}
TEST_CASE("class_light_band_harness/sentinel_mech_floor") {
    CHECK((std::fabs(mechMul(0.0, 0.0, 0.0) - 0.35) < 1e-12));
    return;
}
TEST_CASE("class_light_band_harness/sentinel_ibl_strength") {
    CHECK((std::fabs(kIblStrengthDefault - 0.5) < 1e-12));

    CHECK((std::fabs(propIbl(v3(0,1,0), 1.0) - 1.615350) < 1e-5));
    CHECK((std::fabs(propIbl(v3(1,0,0), 1.0) - 4.624195) < 1e-5));
    return;
}
TEST_CASE("class_light_band_harness/sentinel_stock_kernel_truncates") {

    CHECK((stockKernelByte(0, 0, 255, -0.5058) == 128));

    CHECK((stockKernelByte(0, 31, 0, 0.0) == 31));
    CHECK((stockKernelByte(10, 31, 0, 0.0) == 41));

    CHECK((stockKernelByte(200, 100, 100, -1.0) == 255));
    return;
}
TEST_CASE("class_light_band_harness/sentinel_sun_frame_constants") {

    const V3 ts = terrainSunMC2(0.0, 0.0);
    CHECK((std::fabs(ts.x - std::cos(45.0 * kDeg2Rad)) < 1e-9));

    const V3 rd = objectRowLightDir(0.0, 0.0);
    CHECK((std::fabs(rd.x - std::sin(135.0 * kDeg2Rad)) < 1e-9));
    CHECK((std::fabs(rd.y - 0.0) < 1e-9));
    CHECK((std::fabs(rd.z - std::cos(135.0 * kDeg2Rad)) < 1e-9));

    const V3 s = swapStuffToMC2(v3(1, 2, 3));
    CHECK((s.x == -1 && s.y == 3 && s.z == 2));
    return;
}

static V3 rowLightDirFixed(const V3& rawRow) { return swapStuffToMC2(rawRow); }

static double ndlMechDiffuseFixed(const V3& nMC2, const V3& rawRow)
{
    const V3 d = rowLightDirFixed(rawRow);
    return clampd(dot(nMC2, v3(-d.x, -d.y, -d.z)), 0.0, 1.0);
}

static const double kSunPitchDegenerateEps = 1e-6;
static bool sunPitchIsDegenerate(double pitchDeg)
{
    return std::cos(pitchDeg * kDeg2Rad) < -kSunPitchDegenerateEps;
}
static V3 terrainSunMC2Fixed(double pitchDeg, double yawDeg)
{
    if (sunPitchIsDegenerate(pitchDeg))
        return terrainSunMC2(pitchDeg, yawDeg);
    const double a = (yawDeg + 45.0) * kDeg2Rad;
    const double P = pitchDeg * kDeg2Rad;
    return v3(std::cos(P) * std::cos(a), std::cos(P) * std::sin(a), std::sin(P));
}

static const int kSphereN = 4096;
static V3 sphereNormal(int i)
{
    const double ga = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    const double z  = 1.0 - 2.0 * (i + 0.5) / (double)kSphereN;
    const double r  = std::sqrt(std::max(0.0, 1.0 - z * z));
    const double th = ga * i;
    return v3(r * std::cos(th), r * std::sin(th), z);
}

struct DeltaStat {
    double maxAbs   = 0.0;
    double meanAbs  = 0.0;
    double fracVisible = 0.0;
    double fracBrighter = 0.0;
    double fracDarker   = 0.0;
    bool   fullySaturated = false;
};

static DeltaStat mechDeltaForMission(const Mission& m)
{
    const MissionLight L = missionLight(m);
    DeltaStat st{};
    st.fullySaturated = (0.35 + L.A >= 1.0);
    const double kByte = 1.0 / 255.0;
    long visible = 0, brighter = 0, darker = 0;
    double sum = 0.0;
    for (int i = 0; i < kSphereN; ++i) {
        const V3 n = sphereNormal(i);
        const double before = mechMul(L.A, L.S, ndlMechDiffuse(n, L.rowLightDir));
        const double after  = mechMul(L.A, L.S, ndlMechDiffuseFixed(n, L.rowLightDir));
        const double d = after - before;
        const double ad = std::fabs(d);
        sum += ad;
        if (ad > st.maxAbs) st.maxAbs = ad;
        if (ad >= kByte) { ++visible; if (d > 0) ++brighter; else ++darker; }
    }
    st.meanAbs      = sum / kSphereN;
    st.fracVisible  = visible  / (double)kSphereN;
    st.fracBrighter = brighter / (double)kSphereN;
    st.fracDarker   = darker   / (double)kSphereN;
    return st;
}

TEST_CASE("class_light_band_harness/row2_matches_the_affine_builder") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double xs = std::sin(m.pitch * kDeg2Rad), xc = std::cos(m.pitch * kDeg2Rad);
        const double ys = std::sin((m.yaw + 135.0) * kDeg2Rad),
                     yc = std::cos((m.yaw + 135.0) * kDeg2Rad);
        const double zs = 0.0, zc = 1.0;

        const double m20 = xc * ys * zc + xs * zs;
        const double m21 = xc * ys * zs - xs * zc;
        const double m22 = xc * yc;

        const V3 built = v3(m20, m21, m22);
        const V3 have  = objectRowLightDir(m.pitch, m.yaw);
        CHECK((std::fabs(built.x - have.x) < 1e-12));
        CHECK((std::fabs(built.y - have.y) < 1e-12));
        CHECK((std::fabs(built.z - have.z) < 1e-12));
    }
    return;
}

TEST_CASE("class_light_band_harness/fix_a_gives_ndl_one_at_the_true_sun") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double a = (m.yaw + 45.0) * kDeg2Rad, P = m.pitch * kDeg2Rad;
        const V3 truth = v3(std::cos(P)*std::cos(a), std::cos(P)*std::sin(a), std::sin(P));
        const V3 row   = objectRowLightDir(m.pitch, m.yaw);
        CHECK((std::fabs(ndlMechDiffuseFixed(truth, row) - 1.0) < 1e-9));
    }
    return;
}

TEST_CASE("class_light_band_harness/fix_a_equals_the_specular_consumer") {
    for (int i = 0; i < kNumMissions; ++i) {
        const V3 row = objectRowLightDir(kMissions[i].pitch, kMissions[i].yaw);
        for (int k = 0; k < 64; ++k) {
            const V3 n = sphereNormal(k * (kSphereN / 64));
            CHECK((std::fabs(ndlMechDiffuseFixed(n, row)
                                - ndlMechSpecular(n, row)) < 1e-12));
        }
    }
    return;
}

TEST_CASE("class_light_band_harness/saturated_missions_show_zero_delta") {
    int saturated = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const DeltaStat st = mechDeltaForMission(kMissions[i]);
        if (!st.fullySaturated) continue;
        ++saturated;
        CHECK((st.maxAbs == 0.0));
        CHECK((st.fracVisible == 0.0));
    }
    CHECK((saturated > 0));

    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_10") continue;
        const DeltaStat st = mechDeltaForMission(kMissions[i]);
        CHECK((st.fullySaturated));
        CHECK((st.maxAbs == 0.0));
    }
    return;
}

TEST_CASE("class_light_band_harness/unsaturated_missions_move_in_both_directions") {
    int twoSided = 0, unsat = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const DeltaStat st = mechDeltaForMission(kMissions[i]);
        if (st.fullySaturated) continue;
        ++unsat;
        if (st.fracBrighter > 0.01 && st.fracDarker > 0.01) ++twoSided;
    }
    CHECK((unsat >= 10));
    CHECK((twoSided >= unsat / 2));
    return;
}

TEST_CASE("class_light_band_harness/fix_b_degenerate_pitch_is_handled") {
    int degenerate = 0, poles = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 legacy = terrainSunMC2(m.pitch, m.yaw);
        const V3 fixed  = terrainSunMC2Fixed(m.pitch, m.yaw);
        if (sunPitchIsDegenerate(m.pitch)) {
            ++degenerate;
            CHECK((fixed.x == legacy.x && fixed.y == legacy.y && fixed.z == legacy.z));
            CHECK((std::string(m.name) == "mc2_10"));
        }
        if (std::fabs(std::cos(m.pitch * kDeg2Rad)) < 1e-12) {
            ++poles;
            CHECK((std::fabs(fixed.x) < 1e-12 && std::fabs(fixed.y) < 1e-12));
            CHECK((std::fabs(fixed.z + 1.0) < 1e-9));
        }
    }
    CHECK((degenerate == 1));
    CHECK((poles == 2));
    return;
}

TEST_CASE("class_light_band_harness/fix_b_unifies_terrain_sun_with_the_object_row") {
    int unified = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        if (sunPitchIsDegenerate(m.pitch)) continue;
        const V3 s = swapStuffToMC2(objectRowLightDir(m.pitch, m.yaw));
        const V3 minusSwap = v3(-s.x, -s.y, -s.z);
        const V3 fixed = terrainSunMC2Fixed(m.pitch, m.yaw);
        CHECK((std::fabs(fixed.x - minusSwap.x) < 1e-9));
        CHECK((std::fabs(fixed.y - minusSwap.y) < 1e-9));
        CHECK((std::fabs(fixed.z - minusSwap.z) < 1e-9));
        ++unified;
    }
    CHECK((unified == kNumMissions - 1));

    int legacyMismatch = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 s = swapStuffToMC2(objectRowLightDir(m.pitch, m.yaw));
        const V3 legacy = terrainSunMC2(m.pitch, m.yaw);
        if (std::fabs(legacy.z + s.y) > 1e-6) ++legacyMismatch;
    }
    CHECK((legacyMismatch >= kNumMissions - 3));
    return;
}

TEST_CASE("class_light_band_harness/fix_b_also_moves_the_sun_colour_byte") {
    int moved = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double iBefore = terrainSunMC2(m.pitch, m.yaw).z;
        const double iAfter  = terrainSunMC2Fixed(m.pitch, m.yaw).z;
        const int sBefore = getLightByte(m.lightR, m.ambR, iBefore);
        const int sAfter  = getLightByte(m.lightR, m.ambR, iAfter);
        if (sBefore != sAfter) ++moved;
    }
    CHECK((moved > 0));

    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const Mission& m = kMissions[i];
        CHECK((terrainSunMC2Fixed(m.pitch, m.yaw).z
                  > terrainSunMC2(m.pitch, m.yaw).z + 0.1));
    }
    return;
}

TEST_CASE("class_light_band_harness/sentinel_fix_constants") {

    const V3 f = rowLightDirFixed(v3(1, 2, 3));
    CHECK((f.x == -1 && f.y == 3 && f.z == 2));

    CHECK((sunPitchIsDegenerate(180.0)));
    CHECK((!sunPitchIsDegenerate(46.0)));
    CHECK((!sunPitchIsDegenerate(0.0)));

    CHECK((!sunPitchIsDegenerate(270.0)));
    CHECK((std::cos(270.0 * kDeg2Rad) < 0.0));
    return;
}

TEST_CASE("class_light_band_harness/stock_lit_trees_with_the_sun_byte_not_the_mission_ambient") {
    int brighter = 0, equal = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);

        if (L.S > L.A + 1e-9)      ++brighter;
        else if (std::fabs(L.S - L.A) < 1e-9) ++equal;
        CHECK((L.S >= L.A - 1e-9));
    }

    CHECK((brighter + equal == kNumMissions));
    CHECK((brighter >= 20));
    return;
}

TEST_CASE("class_light_band_harness/stock_tree_multiplier_is_normal_independent") {
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        double lo = 2.0, hi = -1.0;
        forEachNormal(kSphereN, [&](const V3& n) {
            (void)n;
            const double m = treeMulStock(0.0, L.S);
            if (m < lo) lo = m;
            if (m > hi) hi = m;
        });
        CHECK((std::fabs(hi - lo) < 1e-12));
    }
    return;
}

TEST_CASE("class_light_band_harness/stock_trees_are_mostly_UNsaturated") {
    int pinned = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        if (treeMulStock(0.0, L.S) >= 1.0 - 1e-9) ++pinned;
    }

    CHECK((pinned < kNumMissions / 2));
    CHECK((pinned == 1));
    return;
}

TEST_CASE("class_light_band_harness/head_ibl_pins_trees_that_stock_left_free") {
    int stockPinned = 0, headPinnedAllNormals = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const bool sp = treeMulStock(0.0, L.S) >= 1.0 - 1e-9;
        if (sp) ++stockPinned;

        const double str = iblStrengthShipped(kMissions[i].name);
        bool allPinned = true;
        forEachNormal(kSphereN, [&](const V3& n) {
            if (treeMulHead(0.0, L.S, propIbl(n, str)) < 1.0 - 1e-9) allPinned = false;
        });
        if (allPinned) ++headPinnedAllNormals;
    }

    CHECK((headPinnedAllNormals > stockPinned));
    CHECK((stockPinned == 1));
    CHECK((headPinnedAllNormals == 5));
    return;
}

TEST_CASE("class_light_band_harness/prop_shadow_receive_is_an_exact_no_op_on_trees") {

    const double kTreeGBuffer1Alpha = 0.1875;
    auto rc_isAlphaTestProp = [](double a) { return a > 0.1 && a <= 0.22; };
    CHECK((rc_isAlphaTestProp(kTreeGBuffer1Alpha)));
    CHECK((!rc_isAlphaTestProp(0.25)));

    auto rc_isStaticPropNoSelfShadow = [](double a) { return a > 0.1 && a <= 0.5; };
    CHECK((rc_isStaticPropNoSelfShadow(kTreeGBuffer1Alpha)));

    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int s = 0; s <= 10; ++s) {
            const double shadow = s / 10.0;
            for (int k = 0; k <= 10; ++k) {
                const double ndl = k / 10.0;

                const double stockTree = treeMulStock(0.0, L.S);
                CHECK((std::fabs(stockTree - treeMulStock(0.0, L.S)) < 1e-12));

                const double sunRestored = clampd(L.S + L.S * ndl, 0.0, 1.0);
                const double sunRestoredShadowed =
                    rc_isAlphaTestProp(kTreeGBuffer1Alpha)
                        ? sunRestored
                        : clampd(L.S + L.S * ndl * shadow, 0.0, 1.0);
                CHECK((std::fabs(sunRestoredShadowed - sunRestored) < 1e-12));
            }
        }
    }
    return;
}

TEST_CASE("class_light_band_harness/restoring_a_sun_would_be_invisible_where_S_saturates") {
    int invisible = 0, visible = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);

        double maxDelta = 0.0;
        const double str = iblStrengthShipped(kMissions[i].name);
        forEachNormal(kSphereN, [&](const V3& n) {
            const double ndl = std::max(dot(norm(n), L.terrainSun), 0.0);
            const double ibl = propIbl(n, str);
            const double head = treeMulHead(0.0, L.S, ibl);
            const double rest = clampd(0.0 + L.S + L.S * ndl + ibl, 0.0, 1.0);
            maxDelta = std::max(maxDelta, std::fabs(rest - head));
        });
        if (maxDelta < 1e-9) ++invisible; else ++visible;
    }

    CHECK((invisible > 0));
    CHECK((invisible + visible == kNumMissions));
    CHECK((invisible == 5));
    return;
}

TEST_CASE("class_light_band_harness/shipped_ibl_strength_is_not_the_cpp_literal") {
    CHECK((std::fabs(kIblStrengthDefault - 0.5) < 1e-12));
    CHECK((std::fabs(kIblStrengthShippedDefault - 0.55) < 1e-12));
    CHECK((kIblStrengthShippedDefault > kIblStrengthDefault));

    CHECK((std::fabs(iblStrengthShipped("mc2_01") - 0.60) < 1e-12));
    CHECK((std::fabs(iblStrengthShipped("mc2_10") - 0.60) < 1e-12));
    CHECK((std::fabs(iblStrengthShipped("mc2_24") - 0.60) < 1e-12));
    CHECK((std::fabs(iblStrengthShipped("mc2_03") - 0.55) < 1e-12));
    CHECK((std::fabs(iblStrengthShipped("mc2_22") - 0.55) < 1e-12));

    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const double str = iblStrengthShipped(kMissions[i].name);
        const V3 n = norm(v3(0.3, 0.8, 0.5));
        CHECK((treeMulHead(0.0, L.S, propIbl(n, str)) >=
                    treeMulHead(0.0, L.S, propIbl(n, kIblStrengthDefault)) - 1e-12));
    }
    return;
}

TEST_CASE("class_light_band_harness/sentinel_tree_constants") {

    CHECK((treeMulStock(0.0, 0.5) == 0.5));

    CHECK((treeMulStock(0.0, 2.0) == 1.0));
    CHECK((treeMulStock(0.6, 0.6) == 1.0));

    CHECK((treeMulHead(0.0, 0.1, propIbl(v3(0,1,0))) >
                treeMulStock(0.0, 0.1)));
    CHECK((std::fabs(kIblStrengthDefault - 0.5) < 1e-12));
    return;
}

TEST_CASE("class_light_band_harness/print_stage_e_tree_table") {
    std::fprintf(stderr,
        "\n=== STAGE E: TREE stock-vs-HEAD, per mission ===\n"
        "  stock = clamp(S, 0, 1)                 (bdactor.cpp:7304 + :7317-7318)\n"
        "  head  = clamp(S + evalShL2(n)*str,0,1) (static_prop.vert:848, default-ON)\n"
        "  Sflat = getLightByte(lightR, ambR, terrainSun.z)/255\n\n"
        "  IBL strength = SHIPPED per-mission value (data/visual_tuning.json),\n"
        "  NOT the 0.5f C++ literal.\n\n"
        "  mission    str    A      S    stockMul  stockPin  headMin  headMax  headPin  ibl-eaten\n"
        "  -------- ----- ------ ------ --------- --------- -------- -------- -------- ---------\n");

    int stockPinned = 0, headPinned = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const double sm = treeMulStock(0.0, L.S);
        const double str = iblStrengthShipped(kMissions[i].name);
        double lo = 2.0, hi = -1.0, eatenMax = 0.0;
        forEachNormal(kSphereN, [&](const V3& n) {
            const double ibl = propIbl(n, str);
            const double hm  = treeMulHead(0.0, L.S, ibl);
            if (hm < lo) lo = hm;
            if (hm > hi) hi = hm;

            eatenMax = std::max(eatenMax, (L.S + ibl) - hm);
        });
        const bool sp = sm >= 1.0 - 1e-9;
        const bool hp = lo >= 1.0 - 1e-9;
        if (sp) ++stockPinned;
        if (hp) ++headPinned;
        std::fprintf(stderr,
            "  %-8s %5.2f %6.4f %6.4f %9.4f %9s %8.4f %8.4f %8s %9.4f\n",
            kMissions[i].name, str, L.A, L.S, sm, sp ? "PINNED" : "-",
            lo, hi, hp ? "PINNED" : "-", eatenMax);
    }
    std::fprintf(stderr,
        "\n  stock pinned: %d / %d      HEAD pinned (all normals): %d / %d\n"
        "  => the port's default-ON SH-IBL pins %d missions stock left free.\n\n",
        stockPinned, kNumMissions, headPinned, kNumMissions,
        headPinned - stockPinned);
    return;
}

TEST_CASE("class_light_band_harness/print_stage_d_delta") {
    std::fprintf(stderr,
        "\n=== STAGE D: SUN-AXIS-FIX-1 mech/vehicle visual delta, clamp IN ===\n"
        "  M = clamp(0.35 + A + S*ndl, 0, 1); delta over %d sphere normals.\n"
        "  'sat' = 0.35+A >= 1 -> pinned at 1.0 -> fix is invisible there.\n\n"
        "  mission   pitch    A      S    sat  maxAbs  meanAbs  vis%%  brt%%  drk%%\n"
        "  -------- ------ ------ ------ ---- ------- -------- ----- ----- -----\n",
        kSphereN);
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        const DeltaStat st = mechDeltaForMission(m);
        std::fprintf(stderr,
            "  %-8s %6.2f %6.4f %6.4f  %-3s %7.4f %8.5f %5.1f %5.1f %5.1f\n",
            m.name, m.pitch, L.A, L.S, st.fullySaturated ? "YES" : "no",
            st.maxAbs, st.meanAbs, st.fracVisible * 100.0,
            st.fracBrighter * 100.0, st.fracDarker * 100.0);
    }
    std::fprintf(stderr,
        "\n  FIX B (elevation) per-mission sun elevation, degrees:\n"
        "  mission   authored  shipped  fixed   degenerate\n"
        "  -------- --------- -------- ------- ----------\n");
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double eBefore = std::asin(clampd(terrainSunMC2(m.pitch, m.yaw).z, -1, 1)) / kDeg2Rad;
        const double eAfter  = std::asin(clampd(terrainSunMC2Fixed(m.pitch, m.yaw).z, -1, 1)) / kDeg2Rad;
        std::fprintf(stderr, "  %-8s %9.2f %8.2f %7.2f %s\n",
            m.name, m.pitch, eBefore, eAfter,
            sunPitchIsDegenerate(m.pitch) ? "YES -> legacy verbatim" : "");
    }
    std::fprintf(stderr, "\n");
    return;
}

TEST_CASE("class_light_band_harness/print_stage_a_matrix") {
    std::fprintf(stderr,
        "\n=== STAGE A: per-mission lit-multiplier bands (ndl = 0 .. 1) ===\n"
        "%-8s %6s %6s | %-13s %-13s %-13s %-13s %-13s | %-9s %s\n",
        "mission", "A", "S", "terrain", "cement", "mech", "vehicle", "prop(IBL up)",
        "amb<89?", "PREDICTED");
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        const double iblUp = propIbl(v3(0, 1, 0));
        char tb[32], cb[32], mb[32], vb[32], pb[32];
        std::snprintf(tb, sizeof tb, "%.3f..%.3f", terrainMul(0.0), terrainMul(1.0));
        std::snprintf(cb, sizeof cb, "%.3f..%.3f", cementMul(), cementMul());
        std::snprintf(mb, sizeof mb, "%.3f..%.3f", mechMul(L.A, L.S, 0.0), mechMul(L.A, L.S, 1.0));
        std::snprintf(vb, sizeof vb, "%.3f..%.3f", vehicleMul(0.0, L.A, L.S, 0.0),
                                                   vehicleMul(0.0, L.A, L.S, 1.0));
        std::snprintf(pb, sizeof pb, "%.3f..%.3f", propMul(0.0, L.A, L.S, 0.0, iblUp),
                                                   propMul(0.0, L.A, L.S, 1.0, iblUp));
        const bool predicted = (std::string(m.name) == "mc2_03" || std::string(m.name) == "mc2_12"
                             || std::string(m.name) == "mc2_13" || std::string(m.name) == "mc2_17"
                             || std::string(m.name) == "mc2_18");
        std::fprintf(stderr, "%-8s %6.4f %6.4f | %-13s %-13s %-13s %-13s %-13s | %-9s %s\n",
            m.name, L.A, L.S, tb, cb, mb, vb, pb,
            (m.ambR < 89 ? "YES" : "no"), (predicted ? "<= named" : ""));
    }
    std::fprintf(stderr,
        "\nprop IBL additive over the normal sphere at default strength 0.5: ");
    double lo = 1e9, hi = -1e9;
    for (int a = 0; a < 128; ++a) for (int b = 0; b < 128; ++b) {
        const double th = 3.14159265358979 * (a + 0.5) / 128.0;
        const double ph = 2.0 * 3.14159265358979 * (b + 0.5) / 128.0;
        const V3 n = v3(std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph));
        const double v = propIbl(n); lo = std::min(lo, v); hi = std::max(hi, v);
    }
    std::fprintf(stderr, "[%.4f .. %.4f]  (ratio %.2fx)\n", lo, hi, hi / lo);
    return;
}

TEST_CASE("class_light_band_harness/print_stage_b_frames") {
    std::fprintf(stderr,
        "\n=== STAGE B: ndl for a face pointing STRAIGHT AT THE SUN (mech clamp OFF) ===\n"
        "%-8s %8s %8s | %10s %10s %10s | %s\n",
        "mission", "pitch", "yaw", "mechDiff", "mechSpec", "prop", "verdict");
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        const V3 N = L.terrainSun;
        const double a = ndlMechDiffuse(N, L.rowLightDir);
        const double b = ndlMechSpecular(N, L.rowLightDir);
        const double c = ndlProp(N, L.terrainSun);
        std::fprintf(stderr, "%-8s %8.2f %8.2f | %10.4f %10.4f %10.4f | %s\n",
            m.name, m.pitch, m.yaw, a, b, c,
            (a < 0.999 ? "mech DIFFUSE wrong-axis" : "-"));
    }
    return;
}

TEST_CASE("class_light_band_harness/print_stage_c_table") {
    std::fprintf(stderr,
        "\n=== STAGE C: stock tgl.cpp kernel vs each current path, identical inputs ===\n"
        "shadowed side (ndl = 0), seed = 0, prop normal = up\n"
        "%-8s %8s | %8s %8s %8s %8s | %8s %8s %8s\n",
        "mission", "stock", "mech", "vehicle", "prop", "terrain",
        "d(mech)", "d(veh)", "d(prop)");
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        const double iblUp = propIbl(v3(0, 1, 0));
        const double stock = stockMul(0.0, L.A, L.S, 0.0);
        const double mech  = mechMul(L.A, L.S, 0.0);
        const double veh   = vehicleMul(0.0, L.A, L.S, 0.0);
        const double prop  = propMul(0.0, L.A, L.S, 0.0, iblUp);
        std::fprintf(stderr,
            "%-8s %8.4f | %8.4f %8.4f %8.4f %8.4f | %+8.4f %+8.4f %+8.4f\n",
            m.name, stock, mech, veh, prop, terrainMul(0.0),
            mech - stock, veh - stock, prop - stock);
    }
    return;
}

TEST_CASE("class_light_band_harness/mission_sh_map_is_functionally_but_not_literally_empty") {
    CHECK((RenderCore::kIblShSetCount == 1));
    CHECK((std::string(RenderCore::kIblShSets[0].name) == "default"));

    CHECK((RenderCore::kMissionShMapCount == 1));
    CHECK((RenderCore::kMissionShMap[0].missionName == nullptr));
    CHECK((RenderCore::kMissionShMap[0].shSetName  == nullptr));

    for (int i = 0; i < kNumMissions; ++i) {
        const RenderCore::IblShSet& s = RenderCore::lookupShSet(kMissions[i].name);
        CHECK((std::string(s.name) == "default"));
        CHECK((s.coeffs == &RenderCore::kIblShCoeffs[0]));
    }
    return;
}

TEST_CASE("class_light_band_harness/probe_bright_axis_is_pinned_at_stuff_plus_x") {
    const V3 u = iblBrightAxisStuff(0);
    CHECK((std::fabs(stuffAzimuthDeg(u)) < 0.5));
    CHECK((u.x > 0.95));
    CHECK((std::fabs(std::asin(u.y) / kDeg2Rad - 16.62) < 0.05));

    for (int ch = 0; ch < 3; ++ch)
        CHECK((std::fabs(stuffAzimuthDeg(iblBrightAxisStuff(ch))) < 0.5));

    double bestV = -1e30; V3 bestN = v3(0, 0, 0);
    forEachNormal(96, [&](const V3& n) {
        const double v = evalShL2(n, 0);
        if (v > bestV) { bestV = v; bestN = n; }
    });
    CHECK((std::fabs(wrap180(stuffAzimuthDeg(bestN) - stuffAzimuthDeg(u))) < 5.0));
    CHECK((bestV > 4.85 && bestV < 4.90));
    return;
}

TEST_CASE("class_light_band_harness/the_7_71_ratio_is_the_x_pair_not_the_sphere_swing") {
    const double px = evalShL2(v3( 1, 0, 0), 0);
    const double mx = evalShL2(v3(-1, 0, 0), 0);
    CHECK((std::fabs(px - 4.6242) < 1e-3));
    CHECK((std::fabs(mx - 0.5997) < 1e-3));
    CHECK((std::fabs(px / mx - 7.710) < 0.01));
    double lo = 1e30, hi = -1e30;
    forEachNormal(96, [&](const V3& n) {
        const double v = evalShL2(n, 0);
        lo = std::min(lo, v); hi = std::max(hi, v);
    });
    CHECK((hi / lo > 20.0));
    CHECK((hi / lo > px / mx));
    return;
}

TEST_CASE("class_light_band_harness/probe_bright_axis_misses_the_sun_on_every_mission") {
    std::vector<double> errs;
    for (int i = 0; i < kNumMissions; ++i)
        errs.push_back(std::fabs(probeVsSunAzErrorDeg(kMissions[i])));
    std::sort(errs.begin(), errs.end());
    const double median = errs[kNumMissions / 2];
    CHECK((median > 90.0));
    CHECK((errs.back() > 175.0));

    int near = 0;
    for (double e : errs) if (e <= 20.0) ++near;
    CHECK((near == 3));
    return;
}

TEST_CASE("class_light_band_harness/probe_is_brighter_on_the_shadowed_wall_on_most_missions") {
    int wrongWay = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const V3 sunStuff = missionSunStuff(kMissions[i]);

        const V3 toward = norm(v3(sunStuff.x, 0.0, sunStuff.z));
        const V3 away   = v3(-toward.x, 0.0, -toward.z);
        if (evalShL2(toward, 0) < evalShL2(away, 0)) ++wrongWay;
    }
    CHECK((wrongWay == 17));
    return;
}

TEST_CASE("class_light_band_harness/sh_y_rotation_equals_rotating_the_normal") {
    const ShSet9 base = shippedShSet();
    const double angles[] = { 0.0, 0.3, -1.1, 2.7, 3.14159, -2.0, 1.5707963 };
    double worst = 0.0;
    for (double tRad : angles) {
        const ShSet9 rot = rotateShAboutY(base, tRad);
        forEachNormal(48, [&](const V3& n) {
            for (int ch = 0; ch < 3; ++ch) {
                const double lhs = evalShL2Set(rot, n, ch);
                const double rhs = evalShL2Set(base, rotAboutStuffY(n, -tRad), ch);
                worst = std::max(worst, std::fabs(lhs - rhs));
            }
        });
    }
    CHECK((worst < 1e-12));

    const ShSet9 id = rotateShAboutY(base, 0.0);
    for (int i = 0; i < 9; ++i)
        for (int ch = 0; ch < 3; ++ch)
            CHECK((id.c[i][ch] == base.c[i][ch]));
    return;
}

TEST_CASE("class_light_band_harness/sun_align_rotation_aims_the_probe_at_the_sun") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const ShSet9 rot = rotateShAboutY(shippedShSet(), sunAlignRotationRad(m));

        const V3 axis = norm(v3(rot.c[3][0], rot.c[1][0], rot.c[2][0]));
        const double err = wrap180(stuffAzimuthDeg(axis)
                                 - stuffAzimuthDeg(missionSunStuff(m)));
        CHECK((std::fabs(err) < 1e-9));

        CHECK((std::fabs(rot.c[0][0] - RenderCore::kIblShCoeffs[0][0]) < 1e-12));
        CHECK((std::fabs(rot.c[6][0] - RenderCore::kIblShCoeffs[6][0]) < 1e-12));
    }
    return;
}

TEST_CASE("class_light_band_harness/mc2_10_is_a_saturation_trap_for_probe_rotation") {
    const Mission* m10 = nullptr; const Mission* m22 = nullptr;
    const Mission* m17 = nullptr; const Mission* m24 = nullptr;
    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        if (n == "mc2_10") m10 = &kMissions[i];
        if (n == "mc2_22") m22 = &kMissions[i];
        if (n == "mc2_17") m17 = &kMissions[i];
        if (n == "mc2_24") m24 = &kMissions[i];
    }
    CHECK((m10 && m22 && m17 && m24));
    REQUIRE((m10 && m22 && m17 && m24));

    const RotSweep s10 = rotationSweep(*m10);
    const RotSweep s22 = rotationSweep(*m22);
    const RotSweep s17 = rotationSweep(*m17);
    const RotSweep s24 = rotationSweep(*m24);

    CHECK((s10.meanAbs   < 0.01));
    CHECK((s10.unsatFrac < 0.10));
    CHECK((s22.meanAbs   < 0.02));

    CHECK((s17.meanAbs > 0.15));
    CHECK((s24.meanAbs > 0.20));
    CHECK((s17.meanAbs > 40.0 * s10.meanAbs));
    CHECK((s24.meanAbs > 40.0 * s10.meanAbs));

    CHECK((s17.unsatFrac > 0.50));
    return;
}

TEST_CASE("class_light_band_harness/sentinel_ibl_probe_axis_constants") {

    CHECK((std::fabs(RenderCore::kIblShCoeffs[3][0] - 1.966354845) < 1e-6));
    CHECK((std::fabs(RenderCore::kIblShCoeffs[1][0] - 0.5869951003) < 1e-6));
    CHECK((std::fabs(RenderCore::kIblShCoeffs[2][0] - 0.01179062044) < 1e-6));

    CHECK((std::fabs(stuffAzimuthDeg(iblBrightAxisStuff(0)) - 0.3435) < 1e-3));
    const V3 mc2 = swapStuffToMC2(iblBrightAxisStuff(0));
    CHECK((std::fabs(std::atan2(mc2.y, mc2.x) / kDeg2Rad - 179.656) < 1e-2));

    CHECK((std::fabs(std::fabs(probeVsSunAzErrorDeg(kMissions[15])) - 7.66) < 0.05));
    CHECK((std::fabs(std::fabs(probeVsSunAzErrorDeg(kMissions[13])) - 176.66) < 0.05));
    return;
}

TEST_CASE("class_light_band_harness/print_stage_f_probe_aim") {
    const V3 u = iblBrightAxisStuff(0);
    const V3 uM = swapStuffToMC2(u);
    std::fprintf(stderr,
        "\n=== STAGE F: SH-IBL probe aim vs the mission sun ===\n"
        "probe bright axis: Stuff az %+.3f deg, elev %+.3f deg  ->  MC2 az %+.3f deg\n"
        "one set for all 24 missions (kIblShSetCount=%zu, kMissionShMapCount=%zu, "
        "all-null)\n"
        "+X %.4f  -X %.4f  ratio %.3f:1   sphere swing 21.05:1\n\n"
        "%-8s %8s %8s | %8s %8s %8s %8s | %s\n",
        stuffAzimuthDeg(u), std::asin(u.y) / kDeg2Rad,
        std::atan2(uM.y, uM.x) / kDeg2Rad,
        RenderCore::kIblShSetCount, RenderCore::kMissionShMapCount,
        evalShL2(v3(1,0,0),0), evalShL2(v3(-1,0,0),0),
        evalShL2(v3(1,0,0),0) / evalShL2(v3(-1,0,0),0),
        "mission", "sunAzErr", "iblRatio", "moved%", "meanAbs", "maxDn", "unsat%",
        "A/B verdict");
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 sunStuff = missionSunStuff(m);
        const V3 toward = norm(v3(sunStuff.x, 0.0, sunStuff.z));
        const V3 away   = v3(-toward.x, 0.0, -toward.z);
        const double ratio = evalShL2(toward, 0) / evalShL2(away, 0);
        const RotSweep s = rotationSweep(m);
        const char* verdict = (s.meanAbs < 0.02) ? "TRAP - do not use"
                            : (s.meanAbs > 0.15) ? "GOOD"
                                                 : "weak";
        std::fprintf(stderr, "%-8s %+8.2f %8.3f | %8.1f %8.4f %+8.3f %8.1f | %s\n",
            m.name, probeVsSunAzErrorDeg(m), ratio,
            100.0 * s.movedFrac, s.meanAbs, s.maxDown, 100.0 * s.unsatFrac,
            verdict);
    }
    std::fprintf(stderr,
        "\niblRatio < 1 means the probe is BRIGHTER on the wall facing away from\n"
        "the sun. That is 17 of the 24 shipping missions.\n");
    return;
}

static const double kShippedSunRotateFloor = double(mc2::options::RenderOptions{}.staticPropIblSunRotateFloor);

static double shippedSunRotationRad(const V3& sunMC2, const ShSet9& base)
{
    const double sunHoriz2 = sunMC2.x * sunMC2.x + sunMC2.y * sunMC2.y;
    if (!(sunHoriz2 > 1e-12)) return 0.0;
    const double sunAz   = std::atan2(sunMC2.y, -sunMC2.x);
    const double probeAz = std::atan2(base.c[2][0], base.c[3][0]);
    return sunAz - probeAz;
}

static double shippedSunRotateLift(const ShSet9& S, double floorV)
{
    if (floorV <= 0.0) return 0.0;
    const double kPi = 3.14159265358979323846;
    double worstDeficit = 0.0;
    const int kLat = 128, kLon = 256;
    for (int i = 0; i <= kLat; ++i) {
        const double th = kPi * (double)i / (double)kLat;
        const double sth = std::sin(th), cth = std::cos(th);
        for (int j = 0; j < kLon; ++j) {
            const double ph = 2.0 * kPi * (double)j / (double)kLon;
            const V3 n = v3(sth * std::cos(ph), cth, sth * std::sin(ph));
            for (int ch = 0; ch < 3; ++ch) {
                const double d = floorV - evalShL2Set(S, n, ch);
                if (d > worstDeficit) worstDeficit = d;
            }
        }
    }
    return worstDeficit;
}

static ShSet9 shippedUploadSet(const Mission& m, double floorV)
{
    const ShSet9 base = shippedShSet();
    const double t    = shippedSunRotationRad(terrainSunMC2(m.pitch, m.yaw), base);
    ShSet9 o = rotateShAboutY(base, t);
    const double lift = shippedSunRotateLift(base, floorV);
    if (lift > 0.0) {
        const double dL00 = lift / 0.886227;
        for (int ch = 0; ch < 3; ++ch) o.c[0][ch] = base.c[0][ch] + dL00;
    }
    return o;
}

static double minWallMultiplier(const Mission& m, const ShSet9& S, int samples = 720)
{
    const MissionLight L = missionLight(m);
    const double st = iblStrengthShipped(m.name);
    double lo = 1e30;
    for (int k = 0; k < samples; ++k) {
        const double az = 2.0 * 3.14159265358979323846 * k / samples;
        const V3 nMC2   = v3(std::cos(az), std::sin(az), 0.0);
        const V3 nStuff = swapStuffToMC2(nMC2);
        const double ndl = std::max(0.0, dot(nMC2, L.terrainSun));
        lo = std::min(lo, propMul(0.0, L.A, L.S, ndl, evalShL2Set(S, nStuff, 0) * st));
    }
    return lo;
}

TEST_CASE("class_light_band_harness/shipped_angle_matches_the_stuff_frame_derivation") {
    const ShSet9 base = shippedShSet();
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double shipped = shippedSunRotationRad(terrainSunMC2(m.pitch, m.yaw), base);
        const double ref     = sunAlignRotationRad(m);
        CHECK((std::fabs(wrap180((shipped - ref) / kDeg2Rad)) < 1e-9));
    }

    const double t01 = shippedSunRotationRad(terrainSunMC2(kMissions[0].pitch,
                                                           kMissions[0].yaw), base);
    CHECK((std::fabs(t01 / kDeg2Rad - 123.66) < 0.01));
    return;
}

TEST_CASE("class_light_band_harness/shipped_transform_with_no_floor_is_the_bare_rotation") {
    const ShSet9 base = shippedShSet();
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];

        const ShSet9 got  = shippedUploadSet(m, 0.0);
        const ShSet9 pure = rotateShAboutY(base,
                              shippedSunRotationRad(terrainSunMC2(m.pitch, m.yaw), base));
        for (int k = 0; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch)
                CHECK((got.c[k][ch] == pure.c[k][ch]));

        const ShSet9 ref = rotateShAboutY(base, sunAlignRotationRad(m));
        for (int k = 0; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch)
                CHECK((std::fabs(got.c[k][ch] - ref.c[k][ch]) < 1e-12));
    }
    return;
}

TEST_CASE("class_light_band_harness/floor_does_not_move_the_aim") {
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const ShSet9 rot = shippedUploadSet(m, kShippedSunRotateFloor);
        const V3 axis = norm(v3(rot.c[3][0], rot.c[1][0], rot.c[2][0]));
        const double err = wrap180(stuffAzimuthDeg(axis)
                                 - stuffAzimuthDeg(missionSunStuff(m)));
        CHECK((std::fabs(err) < 1e-9));

        const ShSet9 bare = shippedUploadSet(m, 0.0);
        for (int k = 1; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch)
                CHECK((rot.c[k][ch] == bare.c[k][ch]));
    }
    return;
}

TEST_CASE("class_light_band_harness/floor_is_a_pure_lift_and_never_darkens") {
    for (int i = 0; i < kNumMissions; i += 4) {
        const Mission& m  = kMissions[i];
        const ShSet9 bare = shippedUploadSet(m, 0.0);
        const ShSet9 flr  = shippedUploadSet(m, kShippedSunRotateFloor);
        forEachNormal(32, [&](const V3& n) {
            for (int ch = 0; ch < 3; ++ch)
                CHECK((evalShL2Set(flr, n, ch) >= evalShL2Set(bare, n, ch) - 1e-12));
        });
    }

    const ShSet9 lo = shippedUploadSet(kMissions[16], 0.30);
    const ShSet9 hi = shippedUploadSet(kMissions[16], 0.60);
    CHECK((hi.c[0][0] > lo.c[0][0]));
    return;
}

TEST_CASE("class_light_band_harness/floor_lifts_every_channel_minimum_to_the_floor") {
    const ShSet9 flr = shippedUploadSet(kMissions[16], kShippedSunRotateFloor);
    double worst[3] = { 1e30, 1e30, 1e30 };
    forEachNormal(96, [&](const V3& n) {
        for (int ch = 0; ch < 3; ++ch)
            worst[ch] = std::min(worst[ch], evalShL2Set(flr, n, ch));
    });
    for (int ch = 0; ch < 3; ++ch)
        CHECK((worst[ch] >= kShippedSunRotateFloor - 2e-3));

    CHECK((std::fabs(worst[0] - kShippedSunRotateFloor) < 2e-3));
    CHECK((worst[1] > worst[0] && worst[2] > worst[1]));

    CHECK((std::fabs(shippedSunRotateLift(shippedShSet(),
                                               kShippedSunRotateFloor) - 0.218) < 2e-3));
    return;
}

TEST_CASE("class_light_band_harness/bare_rotation_puts_the_dark_missions_under_the_terrain_floor") {
    const char* dark[] = { "mc2_12", "mc2_13", "mc2_17", "mc2_18" };
    int checked = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        bool isDark = false;
        for (const char* d : dark) if (n == d) isDark = true;
        if (!isDark) continue;
        ++checked;
        CHECK((kMissions[i].ambR == 31));
        const double bare = minWallMultiplier(kMissions[i], shippedUploadSet(kMissions[i], 0.0));
        CHECK((bare < kTerrainFloor));
        const double flr  = minWallMultiplier(kMissions[i],
                                shippedUploadSet(kMissions[i], kShippedSunRotateFloor));
        CHECK((flr >= kTerrainFloor));
        CHECK((flr > bare));
    }
    CHECK((checked == 4));

    const double needed = (kTerrainFloor - 31.0 / 255.0) / 0.55;
    CHECK((std::fabs(needed - 0.4462) < 1e-3));
    CHECK((kShippedSunRotateFloor >= needed));
    return;
}

TEST_CASE("class_light_band_harness/floor_cost_at_the_bright_end_is_bounded") {
    const ShSet9 bare = shippedUploadSet(kMissions[16], 0.0);
    const ShSet9 flr  = shippedUploadSet(kMissions[16], kShippedSunRotateFloor);
    double hiBare = -1e30, hiFlr = -1e30;
    forEachNormal(96, [&](const V3& n) {
        hiBare = std::max(hiBare, evalShL2Set(bare, n, 0));
        hiFlr  = std::max(hiFlr,  evalShL2Set(flr,  n, 0));
    });
    CHECK((std::fabs(hiFlr - hiBare - 0.218) < 2e-3));
    CHECK((hiFlr / hiBare < 1.05));

    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        if (n != "mc2_10" && n != "mc2_22") continue;
        const double a = minWallMultiplier(kMissions[i], shippedUploadSet(kMissions[i], 0.0));
        const double b = minWallMultiplier(kMissions[i],
                            shippedUploadSet(kMissions[i], kShippedSunRotateFloor));
        CHECK((b - a < 0.12));
    }
    return;
}

static double shippedFloorFromCfg(const char* text)
{
    mc2::options::RenderOptions o;
    if (text == nullptr) return double(o.staticPropIblSunRotateFloor);
    const mc2::options::Option* opt = mc2::options::find("StaticPropIblSunRotateFloor");
    if (opt == nullptr) return -1.0;
    (void)mc2::options::setFromText(o, *opt, text);
    return double(o.staticPropIblSunRotateFloor);
}
TEST_CASE("class_light_band_harness/floor_option_grammar_contract") {
    const double dflt = kShippedSunRotateFloor;
    CHECK((std::fabs(dflt - 0.45) < 1e-6));
    CHECK((shippedFloorFromCfg(nullptr) == dflt));
    CHECK((shippedFloorFromCfg("0")     == 0.0));
    CHECK((shippedFloorFromCfg("0.0")   == 0.0));
    CHECK((shippedFloorFromCfg("2")     == 2.0));
    CHECK((std::fabs(shippedFloorFromCfg("0.30") - 0.30) < 1e-6));
    CHECK((shippedFloorFromCfg("-1")     == dflt));
    CHECK((shippedFloorFromCfg("99")     == dflt));
    CHECK((shippedFloorFromCfg("banana") == dflt));
    CHECK((shippedFloorFromCfg("0,3")    == dflt));
    CHECK((shippedFloorFromCfg("")       == dflt));

    CHECK((shippedSunRotateLift(shippedShSet(), shippedFloorFromCfg("0")) == 0.0));
    return;
}

TEST_CASE("class_light_band_harness/live_option_defaults_the_models_assume") {
    const mc2::options::RenderOptions d;
    CHECK((d.staticPropIblSh));
    CHECK((!d.staticPropIblSunRotate));
    CHECK((d.staticPropIblSunRotateFloor > 0.0f));
    return;
}

TEST_CASE("class_light_band_harness/models_describe_ambient_off_not_the_shipped_default") {
    CHECK((mc2::options::RenderOptions{}.staticPropAmbient));
    CHECK((propMulPreClamp(0.1, 0.2, 0.3, 0.5, 0.05) == 0.1 + 0.2 + 0.3 * 0.5 + 0.05));
    return;
}

TEST_CASE("class_light_band_harness/rotation_does_not_create_a_new_darkest_wall") {
    double worstRegression = 0.0;
    const char* worstName = "";
    for (int i = 0; i < kNumMissions; ++i) {
        const double stock = minWallMultiplier(kMissions[i], shippedShSet());
        const double bare  = minWallMultiplier(kMissions[i],
                                shippedUploadSet(kMissions[i], 0.0));
        if (stock - bare > worstRegression) {
            worstRegression = stock - bare; worstName = kMissions[i].name;
        }

        CHECK((bare >= stock - 0.25));
    }

    CHECK((std::string(worstName) == "mc2_14"));
    CHECK((std::fabs(worstRegression - 0.2476) < 2e-3));
    CHECK((worstRegression < 0.705));

    for (int i = 0; i < kNumMissions; ++i) {
        if (kMissions[i].ambR != 31) continue;
        const double stock = minWallMultiplier(kMissions[i], shippedShSet());
        const double bare  = minWallMultiplier(kMissions[i],
                                shippedUploadSet(kMissions[i], 0.0));
        CHECK((stock - bare <= 0.0125));

        CHECK((stock < kTerrainFloor));
        CHECK((bare  < kTerrainFloor));
    }
    return;
}

TEST_CASE("class_light_band_harness/after_the_fix_the_sunward_wall_is_the_bright_one") {
    int wrongWayBefore = 0, wrongWayAfter = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 sunStuff = missionSunStuff(m);
        const V3 toward = norm(v3(sunStuff.x, 0.0, sunStuff.z));
        const V3 away   = v3(-toward.x, 0.0, -toward.z);

        if (evalShL2Set(shippedShSet(), toward, 0)
              < evalShL2Set(shippedShSet(), away, 0)) ++wrongWayBefore;

        const ShSet9 fix = shippedUploadSet(m, kShippedSunRotateFloor);
        const double lit  = evalShL2Set(fix, toward, 0);
        const double dark = evalShL2Set(fix, away,   0);
        if (lit < dark) ++wrongWayAfter;
        CHECK((lit > dark));

        double bestAz = 0.0, bestV = -1e30;
        for (int k = 0; k < 720; ++k) {
            const double a = 2.0 * 3.14159265358979323846 * k / 720;
            const V3 n = v3(std::cos(a), 0.0, std::sin(a));
            const double v = evalShL2Set(fix, n, 0);
            if (v > bestV) { bestV = v; bestAz = a / kDeg2Rad; }
        }
        CHECK((std::fabs(wrap180(bestAz - stuffAzimuthDeg(toward))) < 1.0));
    }
    CHECK((wrongWayBefore == 17));
    CHECK((wrongWayAfter  == 0));
    return;
}

TEST_CASE("class_light_band_harness/sentinel_ibl_sun_rotate_constants") {
    CHECK((std::fabs(kShippedSunRotateFloor - 0.45) < 1e-6));

    double lo[3] = { 1e30, 1e30, 1e30 };
    forEachNormal(96, [&](const V3& n) {
        for (int ch = 0; ch < 3; ++ch)
            lo[ch] = std::min(lo[ch], evalShL2Set(shippedShSet(), n, ch));
    });
    CHECK((std::fabs(lo[0] - 0.232) < 2e-3));
    CHECK((std::fabs(lo[1] - 0.360) < 2e-3));
    CHECK((std::fabs(lo[2] - 0.477) < 2e-3));

    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        if (n != "mc2_12" && n != "mc2_18") continue;
        CHECK((std::fabs(minWallMultiplier(kMissions[i], shippedShSet())
                              - 0.3076) < 2e-3));
        const double bare = minWallMultiplier(kMissions[i], shippedUploadSet(kMissions[i], 0.0));
        CHECK((std::fabs(bare - 0.2953) < 2e-3));
        const double flr = minWallMultiplier(kMissions[i],
                             shippedUploadSet(kMissions[i], kShippedSunRotateFloor));
        CHECK((std::fabs(flr - 0.4149) < 2e-3));
    }

    const double guaranteed = 31.0 / 255.0 + kShippedSunRotateFloor * 0.55;
    CHECK((std::fabs(guaranteed - 0.3691) < 1e-3));
    CHECK((guaranteed >= kTerrainFloor));
    return;
}

TEST_CASE("class_light_band_harness/print_stage_g_floor") {
    const double lift = shippedSunRotateLift(shippedShSet(), kShippedSunRotateFloor);
    std::fprintf(stderr,
        "\n=== STAGE G: the shipped fix (StaticPropIblSunRotate) ===\n"
        "floor %.2f -> uniform L00 lift %.4f irradiance (%.4f coeff)\n"
        "terrain's own floor multiplier (the target) = %.4f\n\n"
        "%-8s %5s %8s | %8s %8s %8s | %s\n",
        kShippedSunRotateFloor, lift, lift / 0.886227, kTerrainFloor,
        "mission", "ambR", "rotDeg", "minStock", "minRot", "minRot+F", "verdict");
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double stock = minWallMultiplier(m, shippedShSet());
        const double bare  = minWallMultiplier(m, shippedUploadSet(m, 0.0));
        const double flr   = minWallMultiplier(m, shippedUploadSet(m, kShippedSunRotateFloor));
        const char* verdict = (bare < kTerrainFloor && flr >= kTerrainFloor) ? "FLOOR SAVES IT"
                            : (bare < kTerrainFloor)                        ? "still under"
                                                                            : "clear either way";
        std::fprintf(stderr, "%-8s %5d %+8.2f | %8.4f %8.4f %8.4f | %s\n",
            m.name, m.ambR,
            shippedSunRotationRad(terrainSunMC2(m.pitch, m.yaw), shippedShSet()) / kDeg2Rad,
            stock, bare, flr, verdict);
    }
    std::fprintf(stderr,
        "\nminStock is the SHIPPED build's darkest wall; minRot is the bare\n"
        "rotation's. A rotation is an isometry, so it cannot invent a darker\n"
        "probe value -- but it makes the probe's dark lobe COINCIDE with the\n"
        "NdotL==0 wall, which is why minRot drops below minStock at all.\n");
    return;
}

}
