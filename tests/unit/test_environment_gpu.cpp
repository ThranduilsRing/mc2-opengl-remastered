// U1 environment-gpu: the SH basis the environment block carries, and the one producer.
// The CPU twin below is the evalShL2 of shaders/vulkan/include/prop_sh_l2.glsl, line for line.
#include "doctest.h"
#include "../../GameOS/gameos/mission_environment_gpu.h"
#include "../../RenderCore/IblShCoeffs.h"
#include "axis_convert.h"
#include <array>
#include <cmath>

namespace {
struct V3 { float x, y, z; };
std::array<float,3> evalShL2(V3 n, const float (*sh)[3]) {
    const float c1 = 0.429043f, c2 = 0.511664f, c3 = 0.743125f, c4 = 0.886227f, c5 = 0.247708f;
    std::array<float,3> r{};
    for (int c = 0; c < 3; ++c)
        r[c] = c1*sh[8][c]*(n.x*n.x - n.z*n.z) + c3*sh[6][c]*(n.y*n.y) + c4*sh[0][c] - c5*sh[6][c]
             + 2*c1*sh[4][c]*(n.x*n.y) + 2*c1*sh[5][c]*(n.y*n.z) + 2*c1*sh[7][c]*(n.x*n.z)
             + 2*c2*sh[3][c]*n.x + 2*c2*sh[1][c]*n.y + 2*c2*sh[2][c]*n.z;
    return r;
}
float lum(const std::array<float,3>& v) { return 0.2126f*v[0] + 0.7152f*v[1] + 0.0722f*v[2]; }
}

TEST_CASE("EnvironmentGpu/the sky SH is Y-up: the data pins the shader's up component") {
    const float (*sh)[3] = RenderCore::kIblShCoeffs;
    const float up = lum(evalShL2({0,1,0}, sh)), down = lum(evalShL2({0,-1,0}, sh));
    const float fwd = lum(evalShL2({0,0,1}, sh)), back = lum(evalShL2({0,0,-1}, sh));
    CHECK(up > 1.5f * down);                                      // zenith is far brighter than nadir
    CHECK(std::fabs(up - down) > 10.0f * std::fabs(fwd - back));  // a sky has no .z gradient: .z is not up in this data
}

TEST_CASE("EnvironmentGpu/MC2 up (z) maps to the SH zenith through the one axis conversion") {
    const float (*sh)[3] = RenderCore::kIblShCoeffs;
    struct P { float x, y, z; } mc2Up{0,0,1}, mc2Down{0,0,-1}, r{};
    mc2ToRender(r, mc2Up);
    CHECK(r.y == 1.0f);   // render-space .y is up
    const float roof = lum(evalShL2({r.x, r.y, r.z}, sh));
    mc2ToRender(r, mc2Down);
    const float floorLit = lum(evalShL2({r.x, r.y, r.z}, sh));
    CHECK(roof > floorLit);  // a roof (MC2 +z normal) is lit by the sky above it, not the ground below
}

TEST_CASE("EnvironmentGpu/producer carries the record once and keeps the sky shape") {
    RenderCore::MissionEnvironment env;
    env.lighting.ambientColor = {0.4f, 0.4f, 0.4f};
    env.unitAmbientScale = 1.5f;
    env.sky.hemisphereSky = {0.2f, 0.4f, 0.6f};
    for (int i = 0; i < 9; ++i) for (int c = 0; c < 3; ++c) env.sky.diffuseSh[i][c] = RenderCore::kIblShCoeffs[i][c];
    EnvironmentUBO out{};
    mc2::environment::fillGpu(env, 0.35f, out);
    CHECK(out.sceneLight[0] == doctest::Approx(0.35f));
    CHECK(out.hemiSky[0] == doctest::Approx(0.3f));
    CHECK(out.hemiSky[2] == doctest::Approx(0.9f));
    for (int i = 1; i < 9; ++i) for (int c = 0; c < 3; ++c) CHECK(out.shDiffuse[i][c] == RenderCore::kIblShCoeffs[i][c]);
    // Neutral grey ambient: the L0 hue moves a few percent at most and its luminance is kept.
    const std::array<float,3> l0{out.shDiffuse[0][0], out.shDiffuse[0][1], out.shDiffuse[0][2]};
    const std::array<float,3> ref{RenderCore::kIblShCoeffs[0][0], RenderCore::kIblShCoeffs[0][1], RenderCore::kIblShCoeffs[0][2]};
    CHECK(lum(l0) == doctest::Approx(lum(ref)).epsilon(0.02));
    CHECK(l0[2] / l0[0] == doctest::Approx(1.0f).epsilon(0.02));  // grey ambient reads neutral
}

TEST_CASE("EnvironmentGpu/per-mission L0 follows the ambient hue by day and not at night") {
    RenderCore::MissionEnvironment env;
    for (int i = 0; i < 9; ++i) for (int c = 0; c < 3; ++c) env.sky.diffuseSh[i][c] = RenderCore::kIblShCoeffs[i][c];
    env.lighting.ambientColor = {0.6f, 0.4f, 0.2f};   // warm mission
    EnvironmentUBO day{}, night{};
    mc2::environment::fillGpu(env, 1.0f, day);
    CHECK(day.shDiffuse[0][0] / day.shDiffuse[0][2] > 1.3f);   // warm mission, warm ambient
    env.lighting.night = true;   // the night SH is already built from the mission ambient: no second tint
    mc2::environment::fillGpu(env, 1.0f, night);
    for (int c = 0; c < 3; ++c) CHECK(night.shDiffuse[0][c] == RenderCore::kIblShCoeffs[0][c]);
    float chroma[3];
    mc2::environment::missionL0Chroma({0, 0, 0}, chroma);      // no mission colour: sky hue kept
    CHECK((chroma[0] == 1.0f && chroma[1] == 1.0f && chroma[2] == 1.0f));
}

#include "../../mclib/authored_sun.h"

TEST_CASE("EnvironmentGpu/flat-light ratio is exactly 1 without sun keys, at night and with no mission") {
    using namespace mc2::authored_sun;
    const std::array<unsigned char,3> sun{255, 255, 225}, amb{71, 71, 78};
    // no per-mission sun block (night / no mission): no authored record
    auto r = flatRatio(AuthoredSun{}, sun, amb, 0.608f, 0);
    CHECK((r[0] == 1.0f && r[1] == 1.0f && r[2] == 1.0f));
    // keys absent: authored == live
    r = flatRatio(resolve(sun, amb, false, {}, {}), sun, amb, 0.425f, 0);
    CHECK((r[0] == 1.0f && r[1] == 1.0f && r[2] == 1.0f));
    // keys present: the ratio is live flat / authored flat per channel, and the producer carries it
    const std::array<unsigned char,3> keySun{255, 214, 160}, keyAmb{50, 50, 55};
    r = flatRatio(resolve(sun, amb, false, {}, {}), keySun, keyAmb, 0.425f, 0);
    CHECK(r[0] < 0.9f);
    CHECK(r[2] < 0.8f);
    RenderCore::MissionEnvironment env;
    env.lighting.flatRatio = r;
    EnvironmentUBO out{};
    mc2::environment::fillGpu(env, 1.0f, out);
    for (int c = 0; c < 3; ++c) CHECK(out.sceneLight[1 + c] == r[c]);
    EnvironmentUBO def{};
    mc2::environment::fillGpu(RenderCore::MissionEnvironment{}, 1.0f, def);
    CHECK((def.sceneLight[1] == 1.0f && def.sceneLight[2] == 1.0f && def.sceneLight[3] == 1.0f));
}

TEST_CASE("EnvironmentGpu/a keyed save keeps the authored sun across load") {
    using namespace mc2::authored_sun;
    const std::array<unsigned char,3> sun{255, 255, 225}, amb{71, 71, 78}, keySun{255, 214, 160}, keyAmb{50, 50, 55};
    // the save holds the keyed colours AND the authored ones: load resolves to the authored, so the ratio survives
    const auto a = resolve(keySun, keyAmb, true, sun, amb);
    CHECK(a.sun == sun);
    CHECK(a.amb == amb);
    CHECK(flatRatio(a, keySun, keyAmb, 0.425f, 0)[2] < 0.8f);
    // an old save without the record takes the read colours (nothing better exists)
    const auto old = resolve(keySun, keyAmb, false, {}, {});
    CHECK(old.sun == keySun);
    // old save, no sun keys: live colours = read colours, so R is exactly 1 at every light z
    for (float z : {0.0f, 0.3f, 0.608f, 1.0f}) {
        const auto r = flatRatio(old, keySun, keyAmb, z, 0);
        CHECK((r[0] == 1.0f && r[1] == 1.0f && r[2] == 1.0f));
    }
}

TEST_CASE("EnvironmentGpu/prop SH follows the ambient key and is untouched without keys") {
    using namespace mc2::authored_sun;
    const std::array<unsigned char,3> sun{255, 255, 225}, amb{71, 71, 78}, keyAmb{50, 50, 55};
    const auto a = resolve(sun, amb, false, {}, {});
    CHECK(ambientRatio(a, amb) == 1.0f);          // no keys: exactly 1
    CHECK(ambientRatio(AuthoredSun{}, keyAmb) == 1.0f);   // night / no mission
    const float r = ambientRatio(a, keyAmb);
    CHECK(r < 0.8f);
    RenderCore::MissionEnvironment env;
    env.sky.diffuseSh[0] = {1.0f, 1.0f, 1.0f};
    EnvironmentUBO base{}, keyed{};
    mc2::environment::fillGpu(env, 1.0f, base);
    env.lighting.ambientRatio = r;
    mc2::environment::fillGpu(env, 1.0f, keyed);
    for (int c = 0; c < 3; ++c) CHECK(keyed.shDiffuse[0][c] == doctest::Approx(base.shDiffuse[0][c] * std::pow(r, 2.2f)));
    CHECK(base.hemiSky[3] == 1.0f);
    CHECK(keyed.hemiSky[3] == doctest::Approx(std::pow(r, 2.2f)));
}

TEST_CASE("EnvironmentGpu/mech sun gain is 1 without keys and sqrt(sin(authored)/sin(live)) with a lower keyed sun") {
    using namespace mc2::authored_sun;
    AuthoredSun a = resolve({255, 255, 225}, {71, 71, 78}, false, {}, {});
    a.pitch = 45.0f;
    CHECK(pitchGain(a, 45.0f) == 1.0f);                 // no keys
    CHECK(pitchGain(AuthoredSun{}, 28.0f) == 1.0f);     // night / no mission
    CHECK(pitchGain(a, 28.0f) == doctest::Approx(std::sqrt(std::sin(45.0f * 0.0174533f) / std::sin(28.0f * 0.0174533f))));
    CHECK(pitchGain(a, 0.0f) == 1.0f);                  // live pitch <= 0
    CHECK(pitchGain(a, -5.0f) == 1.0f);
    CHECK(pitchGain(a, 1.0f) == 2.0f);                  // clamped: raw sqrt(sin45/sin1) is ~6.4
    CHECK(pitchGain(a, 179.99f) == 2.0f);
    CHECK(pitchGain(a, 180.0f) == 1.0f);                // sin ~ 0
    CHECK(pitchGain(a, 270.0f) == 1.0f);                // (180, 360): finite, so LegacyMissionLighting equality holds
    a.pitch = 0.0f;
    CHECK(pitchGain(a, 28.0f) == 1.0f);              // keyed save without the authored pitch
}

TEST_CASE("EnvironmentGpu/one sun: layout, and every field exactly identity/1 with no keys, at night, with no mission and for an old save") {
    using namespace mc2::authored_sun;
    static_assert(sizeof(EnvironmentUBO) == 288);
    static_assert(offsetof(EnvironmentUBO, sunDirLive) == 176 && offsetof(EnvironmentUBO, sunAuthFromLive) == 192 && offsetof(EnvironmentUBO, pitchUndo) == 240 &&
                  offsetof(EnvironmentUBO, ambRelight) == 256 && offsetof(EnvironmentUBO, sunRelight) == 272);
    const std::array<unsigned char,3> sun{255, 255, 225}, amb{71, 71, 78};
    const Vec3 live = normalized3({0.7f, 0.5f, 0.6f});
    const std::array<float,3> one{1.0f, 1.0f, 1.0f};
    const OneSun id{};
    auto isId = [&](OneSun s) { s.authoredValid = false; return s == id; };   // the record-exists flag is not a value
    AuthoredSun a = resolve(sun, amb, false, {}, {});
    a.pitch = 36.87f; a.yaw = 12.0f; a.hasYaw = true;
    CHECK(isId(oneSun(a, live, 36.87f, 12.0f, sun, amb, 0.6f, one, 0)));              // no keys: live angles == authored angles
    CHECK(isId(oneSun(AuthoredSun{}, live, 20.0f, 0.0f, {255, 214, 160}, {50, 50, 55}, 0.34f, one, 0)));   // night / no mission: no record
    a.hasYaw = false;                                                                  // an old keyed save has no yaw: the live yaw stands in
    CHECK(isId(oneSun(a, live, 36.87f, 99.0f, sun, amb, 0.6f, one, 0)));
    // the producer carries the identity through
    EnvironmentUBO def{};
    mc2::environment::fillGpu(RenderCore::MissionEnvironment{}, 1.0f, def);
    for (int c = 0; c < 3; ++c) {
        CHECK((def.pitchUndo[c] == 1.0f && def.ambRelight[c] == 1.0f && def.sunRelight[c] == 1.0f));
        for (int col = 0; col < 3; ++col) CHECK(def.sunAuthFromLive[col][c] == (col == c ? 1.0f : 0.0f));
    }
    CHECK(def.sunAuthFromLive[0][3] == 0.0f);
}

TEST_CASE("EnvironmentGpu/one sun: keyed values (pitch 45 -> 28, colour 255/214/160, ambient x0.70)") {
    using namespace mc2::authored_sun;
    const std::array<unsigned char,3> sun{255, 255, 225}, amb{71, 71, 78}, keySun{255, 214, 160}, keyAmb{50, 50, 55};
    AuthoredSun a = resolve(sun, amb, false, {}, {});
    a.pitch = 45.0f; a.yaw = 0.0f; a.hasYaw = true;
    const float k = 0.01745329252f;
    const Vec3 live = {std::cos(45.0f * k), std::sin(45.0f * k), std::sin(28.0f * k)};
    const float zLive = std::sin(28.0f * k), zAuth = std::sin(45.0f * k);
    const auto R = flatRatio(a, keySun, keyAmb, zLive, 0);
    const auto s = oneSun(a, live, 28.0f, 0.0f, keySun, keyAmb, zLive, R, 0);
    // the rotation takes the live direction to the authored one and keeps lengths
    const Vec3 ln = normalized3(live), want = authoredDir(a, 0.0f);
    Vec3 got{};
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) got[r] += s.authFromLive[c][r] * ln[c];
    for (int i = 0; i < 3; ++i) CHECK(got[i] == doctest::Approx(want[i]).epsilon(1e-4));
    // pitchUndo: authored flat light at the authored elevation over the live elevation, per channel (bytes truncate like Camera::getLight)
    for (int i = 0; i < 3; ++i) {
        const float num = std::floor(sun[i] * zAuth + amb[i]), den = std::floor(sun[i] * zLive + amb[i]);
        CHECK(s.pitchUndo[i] == doctest::Approx(std::min(num, 255.0f) / std::min(den, 255.0f)));
        CHECK(s.pitchUndo[i] > 1.0f);                    // a lower live sun: the undo brightens
        CHECK(s.sunRelight[i] == doctest::Approx(float(keySun[i]) / float(sun[i])));
        CHECK(s.ambRelight[i] == doctest::Approx(R[i] * float(amb[i]) / float(keyAmb[i])));
    }
    CHECK(s.authoredValid);
}
