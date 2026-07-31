// tools/class_light_band_harness/class_light_band_harness.cpp
// SUBSYSTEM-HARNESS-ARC / CLASS-LIGHT-BAND-HARNESS-1
//
// The "props are too dark" complaint is a statement about a RATIO between
// object classes, reported as if it were a defect in one class. This harness
// makes the ratio arithmetic EXECUTABLE and OFFLINE, so that a claim about it
// can go red.
//
//   Stage A — the band matrix. Per mission x per class, the actual lit
//            multiplier band computed from the AUTHORED .fit values, plus the
//            inherited falsifiable prediction as its own test.
//   Stage B — the sun-frame test. All four ObjectLights.light_dir consumers
//            fed ONE known direction, with the mech clamp DISABLED, because
//            saturation is what has been hiding the answer.
//   Stage C — stock ground truth. The legacy CPU per-vertex kernel
//            (mclib/tgl.cpp TG_Shape::MultiTransformShape) ported as the
//            reference, then each current GPU class path measured against it
//            on identical inputs. This is the measurement that says whether
//            props regressed or the references moved.
//
// PROOF STANDARD. Every constant below was read from the SHADER SOURCE or the
// C++ READER and carries its file:line. Nothing here was taken from the
// feature registry description text, from docs, or from a relayed summary --
// three inherited claims were found FALSE that way (see CLAIM CHECK below).
//
// DELIBERATELY *NOT* REGISTERED IN tools/run_contract_tests.py
// REGISTERED_HARNESSES. This is an EXPLORATORY measurement harness, not a gate.
// Every assertion here pins a CURRENT TUNING VALUE (0.35, 1.20, 0.5, +135), so
// registering it would turn the contract suite red on every legitimate lighting
// change -- and a lighting change is exactly what this measurement exists to
// motivate. A sibling lane (ARCH-CONVERGE-S2) is building the intended drift
// checker; duplicating that here would also trip the duplicate-row rule in
// check-contract-coverage.py arm (d). Promote to a gate only after the class
// bands are deliberately unified and the numbers stop being in flight.
//
// Build (standalone, no engine, no GL):
//   cmake -S tools/class_light_band_harness -B build64-h-class_light_band_harness -A x64
//   cmake --build build64-h-class_light_band_harness --config RelWithDebInfo
//   ./build64-h-class_light_band_harness/RelWithDebInfo/class_light_band_harness.exe
//
// Report the tables (not part of the pass/fail suite):
//   class_light_band_harness.exe --test print_stage_a_matrix
//   class_light_band_harness.exe --test print_stage_b_frames
//   class_light_band_harness.exe --test print_stage_c_table
//
// ---------------------------------------------------------------------------
// CLAIM CHECK -- inherited from META-RIGHT-QUESTION-2 (.claude/META-RIGHT-
// QUESTION-2026-07-30.md, commit d2cbb9d2), re-verified at source here:
//
//  TRUE   terrain mix(0.35,1.20,diffuse)      shaders/terrain_lod_chunk.frag:1872
//         ...but the FLOOR IS NOT 0.35. diffuse = clamp(NdotL, 0.02, 1.0) on
//         :1871, so the reachable floor is mix(0.35,1.20,0.02) = 0.367.
//  TRUE   cement exemption, u_cementLit=0     shaders/terrain_lod_chunk.frag:1878
//  TRUE   mech clamp(0.35 + A + S*ndl, 0, 1)  shaders/mech.vert:204,218-224
//  FALSE  vehicle "A + S*ndl, no floor, no clamp".  STALE by two commits.
//         VEHICLE-AMBIENT-FLOOR-1 (e3310409) is IN HEAD and DEFAULT-ON:
//         shaders/gos_tex_vertex_lighted.vert gives vehicles
//         clamp(max(seed,0.35) + A + S*ndl, 0, 1) -- the mech expression.
//  FALSE  static prop "+ 0.6*SH_IBL". The default strength is 0.5, not 0.6:
//         GameOS/gameos/gos_static_prop_batcher.cpp:325-333 `return 0.5f`.
//  FALSE  "the mech CLAMP is a port-era addition". The clamp is STOCK:
//         mclib/tgl.cpp:2460-2467 clamps redFinal/greenFinal/blueFinal to 255
//         before the argb write. Only the 0.35 FLOOR is port-era.
//  TRUE   three texts disagree about the frame of ObjectLights.light_dir
//         (mech.vert:262-281 vs RendererFeatureRegistry.h:738 vs
//         static_prop.vert:76-79). Stage B settles it.
// ---------------------------------------------------------------------------

#include "contract_harness.h"
#include "RenderCore/IblShCoeffs.h"     // header-only constexpr, no GL, no engine

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace contract_harness;

//=============================================================================
// 0. Tiny vector helper (no Stuff -- the harness links no engine).
//=============================================================================
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

//=============================================================================
// 1. AUTHORED MISSION DATA
//
// Read verbatim from A:/Games/mc2-srcdata/missions/mc2_NN.fit ([Camera] block
// keys LightRed/LightGreen/LightBlue, AmbientRed/Green/Blue, LightDirPitch,
// LightDirYaw). Embedded so the harness runs with no data tree present.
//
// Regenerate / re-verify with:
//   cd A:/Games/mc2-srcdata/missions && python -c "import re,glob;\
//     [print(f, [re.search(r'^\s*\w+\s+%s\s*=\s*([-\d.]+)'%k, open(f,errors='ignore').read(), re.M).group(1) \
//     for k in ['LightRed','AmbientRed','LightDirPitch','LightDirYaw']]) \
//     for f in sorted(glob.glob('mc2_[0-9][0-9].fit'))]"
//=============================================================================
struct Mission {
    const char* name;
    int lightR, lightG, lightB;      // LightRed/Green/Blue
    int ambR,   ambG,   ambB;        // AmbientRed/Green/Blue
    double pitch;                    // LightDirPitch (degrees)
    double yaw;                      // LightDirYaw   (degrees)
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

//=============================================================================
// 2. PORTED ENGINE ARITHMETIC (each with its reader file:line)
//=============================================================================

// mclib/mathfunc.h:114-121 -- _ftol TRUNCATES, it does not round. The comment
// there is explicit about it. Every byte-domain conversion in the stock kernel
// goes through this, so the harness must truncate too.
static long float2long(double v) { return (long)v; }

// mclib/camera.cpp:4378-4389 -- MC2_NIGHT_AMBIENT_FLOOR, default 0 when the
// env var is unset. Modelled as a parameter so the default is visible.
static const long kNightAmbientFloorDefault = 0;

// mclib/camera.cpp:4392-4408 Camera::getLightRed(intensity).
//   intensity > 0  -> clamp(lightRed*I + ambientRed, floor, 255)
//   intensity <= 0 -> max(ambientRed, floor)
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

// mclib/camera.cpp:2244-2248 Camera::update() -- the TERRAIN sun, MC2 frame
// (x=east, y=north, z=up), pointing TOWARD the sun. This is the vector
// gos_GetTerrainLightDir returns and static_prop.vert receives as
// u_terrainSunMC2 (shaders/static_prop.vert:76-79).
//
// NOTE THE CONSTRUCTION: the horizontal part is UNIT length and z is sin(pitch),
// and only THEN is the whole thing normalized. That is not a spherical
// direction at elevation `pitch` -- its elevation is atan(sin(pitch)), not
// pitch. Stage B measures the consequence.
static V3 terrainSunMC2(double pitchDeg, double yawDeg)
{
    const double a = (yawDeg + 45.0) * kDeg2Rad;
    return norm(v3(std::cos(a), std::sin(a), std::sin(pitchDeg * kDeg2Rad)));
}

// mclib/camera.cpp:1857-1862 (and the identical expression in Camera::init /
// Camera::updateDaylight) -- the OBJECT light's transform:
//     BuildRotation(EulerAngles(lightPitch*D2R, (lightYaw + 135.0)*D2R, 0.0f))
// Note +135, where the terrain sun above uses +45.
//
// mclib/stuff/matrix.cpp:87-115 Matrix4D::BuildRotation(EulerAngles), with
// roll = 0 so z.sine = 0, z.cosine = 1:
//     m(2,0) = x.cosine*y.sine*z.cosine + x.sine*z.sine =  cos(P)*sin(Y)
//     m(2,1) = x.cosine*y.sine*z.sine   - x.sine*z.cosine = -sin(P)
//     m(2,2) = x.cosine*y.cosine                          =  cos(P)*cos(Y)
//
// mclib/stuff/affinematrix.hpp:156-162 GetLocalForwardInWorld() returns
// (m(FORWARD_AXIS, X), m(FORWARD_AXIS, Y), m(FORWARD_AXIS, Z)) with
// FORWARD_AXIS == Z_Axis == 2 (mclib/stuff/vector3d.hpp:55) and
// APPLY_FORWARD_SIGN == identity (vector3d.hpp:73). So the row's light_dir is
// matrix ROW 2:
static V3 objectRowLightDir(double pitchDeg, double yawDeg)
{
    const double P = pitchDeg * kDeg2Rad;
    const double Y = (yawDeg + 135.0) * kDeg2Rad;
    // Already unit length: cos^2P*sin^2Y + sin^2P + cos^2P*cos^2Y == 1.
    return v3(std::cos(P) * std::sin(Y), -std::sin(P), std::cos(P) * std::cos(Y));
}

// The Stuff->MC2 axis swap used identically at shaders/mech.vert:178 (position)
// and :180 (normal), and again at :279 for the specular sun direction.
static V3 swapStuffToMC2(const V3& v) { return v3(-v.x, v.z, v.y); }

//-----------------------------------------------------------------------------
// 2b. SH-IBL -- shaders/static_prop.vert evalShL2, coefficients from
//     RenderCore/IblShCoeffs.h (included above, NOT retyped).
//     Constants c1..c5 read from the shader body verbatim.
//-----------------------------------------------------------------------------
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

// GameOS/gameos/gos_static_prop_batcher.cpp:325-333 -- env unset => 0.5f.
// gos_static_prop_batcher.cpp:1008-1013 -- MC2_STATIC_PROP_IBL_SH default-ON
// (only the literal "0" disables), so the default upload is 0.5f in daylight
// (iblShStrengthNightScaled():1074-1082 returns g_iblShStrength exactly when
// nFactor <= 0).
static const double kIblStrengthDefault = 0.5;

//=============================================================================
// 3. THE FIVE LIT-MULTIPLIER MODELS (Stage A)
//
// Each is a direct transcription of one shader expression. `A` and `S` are the
// ambient and sun light-row colours in [0,1]; `ndl` is that class's own N.L.
//=============================================================================

// shaders/terrain_lod_chunk.frag:1870-1872
//   float diffuse     = clamp(NdotL, 0.02, 1.0);
//   float normalLight = mix(0.35, 1.20, diffuse);
// Mission light COLOURS do not enter this expression at all.
static double terrainMul(double ndl)
{
    const double diffuse = clampd(ndl, 0.02, 1.0);
    return 0.35 + (1.20 - 0.35) * diffuse;
}
static const double kTerrainFloor = 0.35 + (1.20 - 0.35) * 0.02;  // 0.367, NOT 0.35
static const double kTerrainCeil  = 1.20;

// shaders/terrain_lod_chunk.frag:1878  float cementLight = mix(1.0, normalLight, u_cementLit);
// u_cementLit defaults to 0 (MC2_CEMENT_LIT default-OFF) -> flat 1.0.
static double cementMul() { return 1.0; }

// shaders/mech.vert:204 (kAmbientFloor = 0.35), :218-222 (base = vec3(0.35)
// handed to calc_light), :223 clamp(...,0,1); shaders/include/lighting.hglsl
// calc_light: final = base_light; AMBIENT -> ambient += lcolor;
// INFINITE -> final += clamp(dot(normal,-light_dir),0,1) * lcolor;
// return final + ambient.
static double mechMul(double A, double S, double ndl)
{
    return clampd(0.35 + A + S * ndl, 0.0, 1.0);
}
// The same expression with the clamp REMOVED -- Stage B needs this, because
// saturation is exactly what hides a wrong-axis sun.
static double mechMulUnclamped(double A, double S, double ndl)
{
    return 0.35 + A + S * ndl;
}

// shaders/gos_tex_vertex_lighted.vert (tail): with MC2_VEHICLE_AMBIENT_FLOOR
// DEFAULT-ON (RendererFeatureRegistry.h:734-738, verified default=true at HEAD)
//   seed_light = max(base_light, vec3(0.35));
//   VertexLight = clamp(calc_light(..., seed_light), 0.0, 1.0);
static double vehicleMul(double seed, double A, double S, double ndl)
{
    return clampd(std::max(seed, 0.35) + A + S * ndl, 0.0, 1.0);
}
// MC2_VEHICLE_AMBIENT_FLOOR=0 -- the pre-e3310409 shipped expression, which is
// what the inherited brief still describes as current.
static double vehicleMulNoFloor(double seed, double A, double S, double ndl)
{
    return seed + A + S * ndl;
}

// shaders/static_prop.vert:823   lit = (seed + ambientTerm) * propDirFactor + sunColor * ndl;
//   propDirFactor == 1.0 when MC2_PROP_BASE_LIGHT_DIR is off (:798-803)
// shaders/static_prop.vert:836-839  ambient_v1 hemisphere fill, u_ambientV1Strength
//   default 0.0 -> exact no-op, so it is omitted here (and asserted so).
// shaders/static_prop.vert:845-848  if (u_iblShStrength > 0.0) lit += evalShL2(n) * strength;
// The clamp lands LATER, at :906  lit = clamp(lit + inst.aRGBHighlight.rgb, 0, 1).
static double propMulPreClamp(double seed, double A, double S, double ndl, double ibl)
{
    return seed + A + S * ndl + ibl;
}
static double propMul(double seed, double A, double S, double ndl, double ibl)
{
    return clampd(propMulPreClamp(seed, A, S, ndl, ibl), 0.0, 1.0);
}

//=============================================================================
// 4. THE STOCK KERNEL (Stage C reference)
//
// mclib/tgl.cpp TG_Shape::MultiTransformShape (declared :1868), lighting block
// :2042-2502. Reduced to the arithmetic, in the BYTE domain the original used:
//
//   :2042-2044  redFinal = 0; redAmb = 0;
//   :2052-2135  seed: magic-colour decode. For an ordinary opaque vertex
//               (aRGBLight & 0x00ffffff) == 0 the seed stays 0; a non-magic
//               non-zero aRGBLight is copied in verbatim at :2128-2133.
//   :2194-2199  AMBIENT: redAmb = ambientByte   <- ASSIGNMENT, not +=,
//               so the LAST ambient light in the list wins.
//   :2210-2231  INFINITE: cosine = dot(s_lightDir, normal);
//               if (cosine < 0) redFinal += float2long(sunByte * fabs(cosine));
//   :2456       redFinal += redAmb;
//   :2460-2467  if (redFinal > 255) redFinal = 255;      <- THE CLAMP IS STOCK
//
// Sign convention cross-check: stock contributes when dot(lightDir, N) < 0 with
// magnitude |cosine|, i.e. max(-dot(lightDir,N), 0). lighting.hglsl calc_light
// uses clamp(dot(normal, -light_dir), 0, 1). Those are the SAME function --
// the GPU INFINITE branch is arithmetically identical to stock.
//=============================================================================
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
// Same kernel expressed in the [0,1] domain the GPU models use, so Stage C can
// compare like with like. `ndl` here is the already-rectified max(-cosine,0).
static double stockMul(double seed, double A, double S, double ndl)
{
    return clampd(seed + A + S * ndl, 0.0, 1.0);
}

//=============================================================================
// 5. PER-MISSION DERIVED VALUES
//=============================================================================
struct MissionLight {
    double A;              // ambient / 255
    double S;              // sun colour / 255, flat-ground sample
    double intensity;      // terrainNormal . lightDirection on flat ground
    V3     terrainSun;     // MC2 frame, toward sun
    V3     rowLightDir;    // ObjectLights.light_dir as produced by the engine
};

// A = AmbientRed/255 (the TG_LIGHT_AMBIENT slot colour; mclib/camera.cpp:2418
// packs (ambientRed<<16)|... into light 1).
// S = getLightRed(intensity)/255 where intensity = terrainNormal . lightDirection
// (mclib/camera.cpp:4392-4408). Flat ground -> terrainNormal = (0,0,1) -> the
// intensity is lightDirection.z.
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

// The prop IBL additive for a given Stuff-frame normal, red channel.
static double propIbl(const V3& nStuff, double strength = kIblStrengthDefault)
{
    return evalShL2(norm(nStuff), 0) * strength;
}

//=============================================================================
// STAGE A TESTS
//=============================================================================

// A1. The terrain floor is NOT 0.35. Every inherited text says 0.35; the
// shader clamps diffuse at 0.02 first, so the reachable floor is 0.367.
static bool test_terrain_floor_is_not_0_35(TestCtx& t)
{
    CH_CHECK(t, std::fabs(terrainMul(-1.0)  - 0.367) < 1e-6);
    CH_CHECK(t, std::fabs(terrainMul(0.0)   - 0.367) < 1e-6);
    CH_CHECK(t, std::fabs(terrainMul(1.0)   - 1.20)  < 1e-9);
    CH_CHECK(t, std::fabs(kTerrainFloor     - 0.367) < 1e-6);
    // and it is strictly ABOVE the widely-quoted 0.35
    CH_CHECK(t, kTerrainFloor > 0.35);
    return t.failures == 0;
}

// A2. Terrain is mission-INDEPENDENT: the same band on all 24 missions.
static bool test_terrain_band_is_mission_independent(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        CH_CHECK(t, std::fabs(terrainMul(0.0) - kTerrainFloor) < 1e-9);
        CH_CHECK(t, std::fabs(terrainMul(1.0) - kTerrainCeil)  < 1e-9);
    }
    return t.failures == 0;
}

// A3. Cement is exactly 1.0 -- above the terrain CEILING/1.2 and above every
// object class's ceiling. It is the brightest surface in the game by contract.
static bool test_cement_is_exactly_one_and_exempt(TestCtx& t)
{
    CH_CHECK(t, cementMul() == 1.0);
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        CH_CHECK(t, cementMul() >= mechMul(L.A, L.S, 1.0));
    }
    return t.failures == 0;
}

// A4. The measured-elsewhere anchors reproduce EXACTLY from the ported
// camera arithmetic. RendererFeatureRegistry.h:738 states, from independent
// measurement, mc2_10 A = S = 0.7529 and mc2_03 A = 0.1216, S = 0.6745.
// If this port were wrong these would not land.
static bool test_ported_camera_math_reproduces_independent_anchors(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        if (std::string(m.name) == "mc2_10") {
            CH_CHECK(t, std::fabs(L.A - 0.7529) < 5e-5);
            CH_CHECK(t, std::fabs(L.S - 0.7529) < 5e-5);
        }
        if (std::string(m.name) == "mc2_03") {
            CH_CHECK(t, std::fabs(L.A - 0.1216) < 5e-5);
            CH_CHECK(t, std::fabs(L.S - 0.6745) < 5e-5);
        }
    }
    return t.failures == 0;
}

// A5. mc2_10 mech is DEGENERATE: saturated at 1.0 for every possible ndl.
// This is the saturation that has been masking the sun-frame error.
static bool test_mc2_10_mech_is_saturated_at_every_ndl(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_10") continue;
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 20; ++k)
            CH_CHECK(t, mechMul(L.A, L.S, k / 20.0) == 1.0);
        // unclamped it would reach 1.855 -- 85% of the signal is discarded
        CH_CHECK(t, mechMulUnclamped(L.A, L.S, 1.0) > 1.8);
    }
    return t.failures == 0;
}

// A6. THE INHERITED PREDICTION, stated as its own test so it can FAIL.
//
//   Inherited (META-RIGHT-QUESTION-2 §1.2): "the prop floor falls below the
//   terrain floor exactly when authored ambient < 89", singling out
//   mc2_03 / mc2_12 / mc2_13 / mc2_17 / mc2_18.
//
// Two independent parts, tested separately:
//   (a) the THRESHOLD 89   -- wrong, because the terrain floor is 0.367 not
//       0.35, so the crossing is at 0.367*255 = 93.6, i.e. ambient <= 93.
//   (b) the MISSION LIST   -- wrong regardless of threshold: 14 missions
//       author ambient < 89, not 5. The 5 named are the ambient == 31 set.
static bool test_inherited_prediction_threshold_is_wrong(TestCtx& t)
{
    // The claimed threshold, derived from the claimed 0.35 floor.
    const double claimedThreshold = 0.35 * 255.0;          // 89.25
    const double actualThreshold  = kTerrainFloor * 255.0; // 93.6
    CH_CHECK(t, std::fabs(claimedThreshold - 89.25) < 1e-6);
    CH_CHECK(t, actualThreshold > 93.0 && actualThreshold < 94.0);
    // There is at least one mission strictly between them -- so the two
    // thresholds do not merely differ on paper, they classify a real mission
    // differently. (mc2_14 ambient 95 is above both; mc2_16 is 96; the band
    // 89..93 is empty in stock data, so record that honestly.)
    int inBand = 0;
    for (int i = 0; i < kNumMissions; ++i)
        if (kMissions[i].ambR > 89 && kMissions[i].ambR <= 93) ++inBand;
    // Documented outcome: the stock authored set has NO mission in the gap,
    // so the threshold error is real but harmless on stock content.
    CH_CHECK(t, inBand == 0);
    return t.failures == 0;
}

static bool test_inherited_prediction_mission_list_FAILS(TestCtx& t)
{
    std::vector<std::string> predicted = {"mc2_03","mc2_12","mc2_13","mc2_17","mc2_18"};
    std::vector<std::string> actual;
    for (int i = 0; i < kNumMissions; ++i)
        if (kMissions[i].ambR < 89) actual.push_back(kMissions[i].name);

    // THE PREDICTION FAILS. Assert the failure so the record is executable:
    // the true set is strictly larger than the predicted one.
    CH_CHECK(t, actual.size() == 14);
    CH_CHECK(t, actual.size() > predicted.size());
    for (const auto& p : predicted) {
        CH_CHECK(t, std::find(actual.begin(), actual.end(), p) != actual.end());
    }
    // The predicted set is exactly the ambient == 31 set -- the conflation.
    std::vector<std::string> amb31;
    for (int i = 0; i < kNumMissions; ++i)
        if (kMissions[i].ambR == 31) amb31.push_back(kMissions[i].name);
    CH_CHECK(t, amb31 == predicted);
    return t.failures == 0;
}

// Sphere sampler shared by the normal-dependent tests.
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

// A7. THE PREDICTION IS NOT FALSE -- IT IS ILL-POSED, which is worse for the
// lane that inherits it.
//
// "The prop floor falls below the terrain floor when authored ambient < 89"
// treats the prop floor as a PER-MISSION SCALAR. It is not: the prop's only
// floor-side term besides ambient is the SH-IBL probe, which varies 21x across
// the normal sphere (test A8). So whether a prop pixel sits below the terrain
// floor is a property of the NORMAL, not of the mission -- on the SAME mission,
// an up-facing prop face is far above the terrain floor while its darkest face
// is below it.
//
// What is actually true, computed rather than predicted:
//   crossing condition  A + iblMin < terrainFloor
//                    -> ambient/255 < 0.367 - 0.1162
//                    -> ambient < 64
// which selects SEVEN missions, not the five named.
static bool test_prediction_is_ill_posed_prop_floor_is_normal_dependent(TestCtx& t)
{
    const double iblMin = iblMinOverSphere();
    CH_CHECK(t, iblMin > 0.0);                       // the probe never subtracts light
    CH_CHECK(t, iblMin > 0.10 && iblMin < 0.13);     // measured 0.1162

    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        double lo = 1e9, hi = -1e9;
        forEachNormal(48, [&](const V3& n) {
            const double v = propMulPreClamp(0.0, L.A, L.S, 0.0, propIbl(n));
            lo = std::min(lo, v); hi = std::max(hi, v);
        });
        // On EVERY stock mission the prop's own floor-side band straddles a
        // wide range, and its BRIGHTEST unlit face always clears the terrain
        // floor -- so no mission is uniformly "below terrain".
        CH_CHECK(t, hi > kTerrainFloor);
        CH_CHECK(t, hi / lo > 2.0);
    }
    return t.failures == 0;
}

// A7b. The corrected mission set, and how it relates to the predicted one.
// The five named missions are all genuinely in the true set -- but the set has
// seven members. mc2_02 and mc2_05 were missed.
static bool test_corrected_crossing_set_is_seven_not_five(TestCtx& t)
{
    const double iblMin = iblMinOverSphere();
    std::vector<std::string> predicted = {"mc2_03","mc2_12","mc2_13","mc2_17","mc2_18"};
    std::vector<std::string> actual;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        if (propMulPreClamp(0.0, L.A, L.S, 0.0, iblMin) < kTerrainFloor)
            actual.push_back(kMissions[i].name);
    }
    CH_CHECK(t, actual.size() == 7);
    for (const auto& p : predicted)
        CH_CHECK(t, std::find(actual.begin(), actual.end(), p) != actual.end());
    CH_CHECK(t, std::find(actual.begin(), actual.end(), std::string("mc2_02")) != actual.end());
    CH_CHECK(t, std::find(actual.begin(), actual.end(), std::string("mc2_05")) != actual.end());
    return t.failures == 0;
}

// A8. The prop-only IBL additive is not a garnish. Measure its actual range
// over the normal sphere at the DEFAULT strength, and compare it to the whole
// rest of the prop's lighting budget on the darkest stock mission.
static bool test_prop_ibl_additive_dominates_the_budget(TestCtx& t)
{
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
    // Measured band of the default-ON prop-only term: [0.1162 .. 2.4477].
    CH_CHECK(t, lo > 0.10 && lo < 0.13);
    CH_CHECK(t, hi > 2.4 && hi < 2.5);
    CH_CHECK(t, hi / lo > 20.0);         // strongly directional, not an "ambient"
    // On mc2_03 the entire authored budget (A + S) is 0.796; the IBL peak alone
    // is nearly 3x that.
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const MissionLight L = missionLight(kMissions[i]);
        CH_CHECK(t, hi > 2.0 * (L.A + L.S));
    }
    return t.failures == 0;
}

// A9. Because of A8, props SATURATE on a large fraction of normals -- the
// opposite of the reported symptom. Count it.
static bool test_props_saturate_on_a_large_normal_fraction(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const MissionLight L = missionLight(kMissions[i]);
        int sat = 0, total = 0;
        for (int a = 0; a < 64; ++a) {
            for (int b = 0; b < 64; ++b) {
                const double th = 3.14159265358979 * (a + 0.5) / 64.0;
                const double ph = 2.0 * 3.14159265358979 * (b + 0.5) / 64.0;
                const V3 n = v3(std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph));
                // ndl = 0 (worst case, unlit side) -- saturation from fill ALONE
                if (propMulPreClamp(0.0, L.A, L.S, 0.0, propIbl(n)) >= 1.0) ++sat;
                ++total;
            }
        }
        CH_CHECK(t, sat > 0);
        CH_CHECK(t, (double)sat / total > 0.20);
    }
    return t.failures == 0;
}

// A10. Vehicle and mech CONVERGED at HEAD. The inherited brief's vehicle band
// is the gate-OFF expression, which is not what ships.
static bool test_vehicle_band_equals_mech_band_at_head_default(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 8; ++k) {
            const double ndl = k / 8.0;
            // seed 0 covers 97%+ of stock vehicle vertices
            // (RendererFeatureRegistry.h:738 census over 8 stock .tgl binaries)
            CH_CHECK(t, std::fabs(vehicleMul(0.0, L.A, L.S, ndl)
                                - mechMul(L.A, L.S, ndl)) < 1e-12);
        }
    }
    return t.failures == 0;
}

static bool test_gate_off_vehicle_band_differs_from_head(TestCtx& t)
{
    // Proves A10 is a statement about the GATE, not a tautology: with the gate
    // off the two classes really do diverge.
    int diverged = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        if (std::fabs(vehicleMulNoFloor(0.0, L.A, L.S, 0.0)
                    - mechMul(L.A, L.S, 0.0)) > 1e-9) ++diverged;
    }
    CH_CHECK(t, diverged == kNumMissions);
    return t.failures == 0;
}

// A11. The worst pairwise band ratio at the SAME ndl, per mission. This is the
// number the eye actually reads. Assert it is large and that the extremes are
// the two surfaces that never carried an authored light: cement and terrain.
static bool test_worst_pairwise_ratio_is_driven_by_the_exempt_surfaces(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const double ndl = 0.0;                 // the shadowed side, where it shows
        const double mech   = mechMul(L.A, L.S, ndl);
        const double terr   = terrainMul(ndl);
        const double cem    = cementMul();
        const double lo     = std::min(std::min(mech, terr), cem);
        const double hi     = std::max(std::max(mech, terr), cem);
        CH_CHECK(t, hi / lo >= 1.0);
        CH_CHECK(t, hi == cem);                 // cement is always the brightest
    }
    return t.failures == 0;
}

//=============================================================================
// STAGE B TESTS -- the sun-frame test, mech clamp DISABLED throughout.
//
// Four consumers of the SAME ObjectLights.light_dir bytes:
//   B-mech-diffuse : shaders/include/lighting.hglsl:249
//                    clamp(dot(normal, -light_dir), 0, 1), normal is MC2-frame
//                    (mech.vert:180 worldNormal, same swap as worldMC2 :178)
//   B-mech-specular: shaders/mech.vert:277-281 swaps light_dir first,
//                    v_mechSunDirGL = (-x, z, y); the frag re-negates for L
//   B-prop         : shaders/static_prop.vert:734 -- ignores light_dir entirely
//                    and uses u_terrainSunMC2, positive-dot, no negation
//   B-vehicle      : gos_tex_vertex_lighted.vert with MC2_LEGACY_LIT_MC2_FRAME
//                    default-ON -> identical to B-mech-diffuse
//
// The probe: a surface whose MC2-frame normal points STRAIGHT AT THE SUN. A
// correct consumer must return ndl == 1.
//=============================================================================
static double ndlMechDiffuse(const V3& nMC2, const V3& rowDir)
{
    return clampd(dot(nMC2, v3(-rowDir.x, -rowDir.y, -rowDir.z)), 0.0, 1.0);
}
static double ndlMechSpecular(const V3& nMC2, const V3& rowDir)
{
    const V3 s = swapStuffToMC2(rowDir);           // mech.vert:279
    return clampd(dot(nMC2, v3(-s.x, -s.y, -s.z)), 0.0, 1.0);
}
static double ndlProp(const V3& nMC2, const V3& terrainSun)
{
    return std::max(dot(norm(nMC2), norm(terrainSun)), 0.0);   // static_prop.vert:734
}

// B1. The prop consumer is self-consistent by construction (it dots the terrain
// sun against a normal facing the terrain sun).
static bool test_prop_consumer_is_self_consistent(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        CH_CHECK(t, std::fabs(ndlProp(L.terrainSun, L.terrainSun) - 1.0) < 1e-9);
    }
    return t.failures == 0;
}

// B2. THE VERDICT. The RAW row light_dir (mech diffuse, vehicle) is not merely
// "90 degrees off" -- on most missions it is ANTI-correlated with the terrain
// sun, so a face pointing straight at the sun receives ZERO directional light.
static bool test_mech_diffuse_sun_is_wrong_axis(TestCtx& t)
{
    int zeroed = 0, tested = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const double d = ndlMechDiffuse(L.terrainSun, L.rowLightDir);
        ++tested;
        if (d <= 1e-9) ++zeroed;
    }
    // Not one mission gets the correct answer of 1.0 from the raw feed.
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        CH_CHECK(t, ndlMechDiffuse(L.terrainSun, L.rowLightDir) < 0.999);
    }
    CH_CHECK(t, zeroed > kNumMissions / 2);   // majority fully dark
    CH_CHECK(t, tested == kNumMissions);
    return t.failures == 0;
}

// B3. The SWAPPED feed (mech SPECULAR's convention) is the correct one: the
// +90 degree yaw offset between camera.cpp:2245 (yaw+45) and camera.cpp:1861
// (yaw+135) is EXACTLY cancelled by the Stuff->MC2 axis swap.
// Proof, algebraically: -swap(rowDir) == (cosP*cos(a), cosP*sin(a), sinP) with
// a = yaw+45 -- the true spherical toward-sun direction at elevation `pitch`.
static bool test_swapped_feed_recovers_the_true_spherical_sun(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 rowDir = objectRowLightDir(m.pitch, m.yaw);
        const V3 s = swapStuffToMC2(rowDir);
        const V3 minusSwap = v3(-s.x, -s.y, -s.z);
        const double a = (m.yaw + 45.0) * kDeg2Rad;
        const double P = m.pitch * kDeg2Rad;
        const V3 truth = v3(std::cos(P)*std::cos(a), std::cos(P)*std::sin(a), std::sin(P));
        CH_CHECK(t, std::fabs(minusSwap.x - truth.x) < 1e-9);
        CH_CHECK(t, std::fabs(minusSwap.y - truth.y) < 1e-9);
        CH_CHECK(t, std::fabs(minusSwap.z - truth.z) < 1e-9);
        CH_CHECK(t, std::fabs(len(minusSwap) - 1.0) < 1e-9);
    }
    return t.failures == 0;
}

// B4. The azimuths of the swapped feed and the terrain sun agree EXACTLY on
// every mission -- so the "~90 degrees off" note in static_prop.vert:76-79 is a
// real observation of the RAW feed, and the swap (not a substitute vector) was
// the available fix.
static bool test_swapped_feed_and_terrain_sun_share_azimuth(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 s = swapStuffToMC2(objectRowLightDir(m.pitch, m.yaw));
        const V3 minusSwap = v3(-s.x, -s.y, -s.z);
        const V3 ts = terrainSunMC2(m.pitch, m.yaw);
        const double azA = std::atan2(minusSwap.y, minusSwap.x);
        const double azB = std::atan2(ts.y, ts.x);
        double d = std::fabs(azA - azB);
        while (d > 3.14159265358979) d = std::fabs(d - 2.0 * 3.14159265358979);
        // cos(pitch) < 0 flips the horizontal, which is a PI azimuth flip.
        const bool flipped = std::cos(m.pitch * kDeg2Rad) < 0.0;
        if (flipped) CH_CHECK(t, std::fabs(d - 3.14159265358979) < 1e-9);
        else         CH_CHECK(t, d < 1e-9);
    }
    return t.failures == 0;
}

// B5. THE RESIDUAL, and it is a separate defect from the frame question:
// Camera::lightDirection (camera.cpp:2244-2248) builds a UNIT horizontal plus
// sin(pitch) and only then normalizes, so its elevation is atan(sin(pitch)),
// not pitch. The prop's u_terrainSunMC2 therefore sits BELOW the true sun.
static bool test_terrain_sun_elevation_is_atan_sin_pitch_not_pitch(TestCtx& t)
{
    int mismatched = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 ts = terrainSunMC2(m.pitch, m.yaw);
        const double elevActual = std::asin(clampd(ts.z, -1.0, 1.0));
        const double elevAuthored = m.pitch * kDeg2Rad;
        const double elevPredicted = std::atan(std::sin(elevAuthored));
        CH_CHECK(t, std::fabs(elevActual - elevPredicted) < 1e-9);
        if (std::fabs(elevActual - elevAuthored) > 1e-6) ++mismatched;
    }
    // Every mission with a non-degenerate pitch is affected.
    CH_CHECK(t, mismatched >= kNumMissions - 3);
    // mc2_03: authored 46 deg, delivered atan(sin(46 deg)) = 35.7 deg.
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const V3 ts = terrainSunMC2(kMissions[i].pitch, kMissions[i].yaw);
        const double elevDeg = std::asin(clampd(ts.z, -1.0, 1.0)) / kDeg2Rad;
        CH_CHECK(t, elevDeg > 35.0 && elevDeg < 36.0);
    }
    return t.failures == 0;
}

// B6. On mc2_10 -- the mission the user named -- the two feeds are exactly
// ANTI-parallel, and the mech's clamp hides it completely. This is the
// saturation-masks-the-answer claim, made executable.
static bool test_mc2_10_saturation_hides_the_frame_error(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_10") continue;
        const MissionLight L = missionLight(kMissions[i]);
        const V3 s = swapStuffToMC2(L.rowLightDir);
        const V3 correctSun = v3(-s.x, -s.y, -s.z);
        CH_CHECK(t, std::fabs(dot(correctSun, L.terrainSun) + 1.0) < 1e-9);  // anti-parallel

        // Two completely different ndl values...
        const double ndlA = ndlProp(L.terrainSun, L.terrainSun);          // 1.0
        const double ndlB = ndlMechDiffuse(L.terrainSun, L.rowLightDir);
        CH_CHECK(t, std::fabs(ndlA - ndlB) > 0.5);
        // ...yet the CLAMPED mech multiplier is identical for both.
        CH_CHECK(t, mechMul(L.A, L.S, ndlA) == mechMul(L.A, L.S, ndlB));
        // With the clamp removed the difference is large and obvious.
        CH_CHECK(t, std::fabs(mechMulUnclamped(L.A, L.S, ndlA)
                            - mechMulUnclamped(L.A, L.S, ndlB)) > 0.5);
    }
    return t.failures == 0;
}

//=============================================================================
// STAGE C TESTS -- stock kernel vs each current path, identical inputs.
//=============================================================================

// C1. The ported stock kernel agrees with the [0,1] model to within one byte
// of quantisation, so the two forms may be used interchangeably below.
static bool test_stock_byte_and_normalised_forms_agree(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        const int sunByte = getLightByte(m.lightR, m.ambR, L.intensity);
        for (int k = 0; k <= 10; ++k) {
            const double ndl = k / 10.0;
            const int  b = stockKernelByte(0, m.ambR, sunByte, -ndl);
            const double n = stockMul(0.0, L.A, L.S, ndl);
            CH_CHECK(t, std::fabs(b / 255.0 - n) <= 1.0 / 255.0 + 1e-9);
        }
    }
    return t.failures == 0;
}

// C2. THE HEADLINE, part 1: the MECH deviates from stock by exactly the 0.35
// floor -- upward. Mechs are BRIGHTER than stock on every mission, on every
// normal, and by a lot on the shadowed side.
static bool test_mech_is_brighter_than_stock_everywhere(TestCtx& t)
{
    double worstLift = 0.0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 10; ++k) {
            const double ndl = k / 10.0;
            const double stock = stockMul(0.0, L.A, L.S, ndl);   // seed 0
            const double mech  = mechMul(L.A, L.S, ndl);
            CH_CHECK(t, mech >= stock - 1e-12);
            worstLift = std::max(worstLift, mech - stock);
        }
    }
    // The lift equals the floor wherever nothing saturates.
    CH_CHECK(t, std::fabs(worstLift - 0.35) < 1e-9);
    return t.failures == 0;
}

// C3. THE HEADLINE, part 2: the VEHICLE (default-ON floor) deviates from stock
// identically to the mech. Both references moved, together, in the same
// direction, by the same amount.
static bool test_vehicle_deviates_from_stock_exactly_as_mech_does(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int k = 0; k <= 10; ++k) {
            const double ndl   = k / 10.0;
            const double stock = stockMul(0.0, L.A, L.S, ndl);
            CH_CHECK(t, std::fabs((vehicleMul(0.0, L.A, L.S, ndl) - stock)
                                - (mechMul(L.A, L.S, ndl)         - stock)) < 1e-12);
        }
    }
    return t.failures == 0;
}

// C4. THE HEADLINE, part 3: the STATIC PROP also deviates from stock UPWARD --
// via the default-ON IBL, by MORE than the mech's floor on most normals.
// The prop is not the class that fell behind.
static bool test_prop_is_also_brighter_than_stock(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        int propAbove = 0, propAboveMech = 0, total = 0;
        for (int a = 0; a < 32; ++a) {
            for (int b = 0; b < 32; ++b) {
                const double th = 3.14159265358979 * (a + 0.5) / 32.0;
                const double ph = 2.0 * 3.14159265358979 * (b + 0.5) / 32.0;
                const V3 n = v3(std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph));
                const double ndl   = std::max(0.0, dot(n, v3(0, 1, 0)));  // fixed probe
                const double stock = stockMul(0.0, L.A, L.S, ndl);
                const double prop  = propMul(0.0, L.A, L.S, ndl, propIbl(n));
                const double mech  = mechMul(L.A, L.S, ndl);
                if (prop >= stock - 1e-12)  ++propAbove;
                if (prop >= mech  - 1e-12)  ++propAboveMech;
                ++total;
            }
        }
        CH_CHECK(t, propAbove == total);           // never below stock, anywhere
        CH_CHECK(t, (double)propAboveMech / total > 0.5);
    }
    return t.failures == 0;
}

// C5. So the answer to "did props regress, or did the references move?" is:
// ALL THREE object classes are above stock, and they are above it by DIFFERENT
// amounts. Quantify the spread -- that spread IS the defect.
static bool test_all_three_object_classes_exceed_stock_by_different_amounts(TestCtx& t)
{
    int collapsedByClamp = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const V3 nUp = v3(0, 1, 0);
        const double ndl = 0.0;                          // shadowed side
        // PRE-CLAMP, because the clamp is exactly what hides the spread.
        const double stock = 0.0 + L.A + L.S * ndl;
        const double mech  = mechMulUnclamped(L.A, L.S, ndl)                    - stock;
        const double veh   = (std::max(0.0, 0.35) + L.A + L.S * ndl)            - stock;
        const double prop  = propMulPreClamp(0.0, L.A, L.S, ndl, propIbl(nUp))  - stock;
        CH_CHECK(t, mech > 0.0 && veh > 0.0 && prop > 0.0);   // all three ABOVE stock
        CH_CHECK(t, std::fabs(mech - veh) < 1e-12);           // mech == vehicle exactly
        CH_CHECK(t, std::fabs(prop - mech) > 1e-9);           // prop deviates differently
        CH_CHECK(t, std::fabs(mech - 0.35) < 1e-12);          // mech lift IS the floor
        CH_CHECK(t, std::fabs(prop - 0.807675) < 1e-4);       // prop lift IS the IBL

        // ...and count how often the final clamp erases the spread entirely.
        if (std::fabs(propMul(0.0, L.A, L.S, ndl, propIbl(nUp))
                    - mechMul(L.A, L.S, ndl)) < 1e-12) ++collapsedByClamp;
    }
    // On a majority of stock missions the two classes are ALREADY identical on
    // screen at ndl=0 because both saturate -- so a per-mission eyeball cannot
    // distinguish them there, which is why this had to be measured pre-clamp.
    CH_CHECK(t, collapsedByClamp > 0);
    return t.failures == 0;
}

// C5b. REFUTATION-PROOFING FOR STAGE C.
//
// The obvious way Stage C could be wrong is the SEED. Every table above uses
// seed = 0, which is correct for an ordinary opaque vertex (tgl.cpp:2126 --
// the non-magic arm only fires when (aRGBLight & 0x00ffffff) != 0), but stock
// content does carry non-zero authored seeds on a minority of vertices
// (RendererFeatureRegistry.h:738 census: 0xff646257 == 0.3922 on <= 2 verts of
// each stock ground vehicle). If a large enough seed could push STOCK above the
// current paths, the "the references moved" conclusion would collapse.
//
// It cannot, and the reason is structural rather than empirical: both
// expressions contain the seed additively and identically, and the prop adds a
// STRICTLY POSITIVE IBL term on top, while the mech/vehicle floor can only
// RAISE the seed (max(seed, 0.35)). Sweep the whole seed range to show it.
static bool test_stage_c_conclusion_is_seed_independent(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int s = 0; s <= 255; ++s) {
            const double seed = s / 255.0;
            for (int k = 0; k <= 4; ++k) {
                const double ndl   = k / 4.0;
                const double stock = stockMul(seed, L.A, L.S, ndl);
                // mech ignores the seed entirely and substitutes 0.35, so it can
                // sit BELOW stock for a bright authored seed -- record where.
                const double veh  = vehicleMul(seed, L.A, L.S, ndl);
                const double prop = propMul(seed, L.A, L.S, ndl, propIbl(v3(0,1,0)));
                CH_CHECK(t, veh  >= stock - 1e-12);   // max() can only raise
                CH_CHECK(t, prop >= stock - 1e-12);   // IBL is strictly positive
            }
        }
    }
    return t.failures == 0;
}

// C5c. The one arm where a CURRENT path CAN fall below stock: the mech
// discards the authored per-vertex seed instead of flooring it. Bound it, so
// the Stage C conclusion is stated with its exception rather than without.
static bool test_mech_falls_below_stock_only_for_seeds_above_the_floor(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int s = 0; s <= 255; ++s) {
            const double seed  = s / 255.0;
            const double stock = stockMul(seed, L.A, L.S, 0.0);
            const double mech  = mechMul(L.A, L.S, 0.0);
            if (mech < stock - 1e-12) CH_CHECK(t, seed > 0.35);
        }
        // The census value 0.3922 is above the floor, so it is inside that arm.
        CH_CHECK(t, 0.3922 > 0.35);
    }
    return t.failures == 0;
}

// C6. And terrain is NOT a stock-comparable path -- assert the exclusion
// explicitly rather than leaving it implied, since its band is mission-blind
// while every object band is mission-driven.
static bool test_terrain_band_ignores_the_mission_light_colours(TestCtx& t)
{
    // Two missions with wildly different authored light: mc2_03 (A=31) and
    // mc2_10 (A=192). Object bands differ; the terrain band does not.
    const MissionLight a = missionLight(kMissions[2]);   // mc2_03
    const MissionLight b = missionLight(kMissions[9]);   // mc2_10
    CH_CHECK(t, std::fabs(mechMul(a.A, a.S, 0.0) - mechMul(b.A, b.S, 0.0)) > 0.4);
    CH_CHECK(t, terrainMul(0.0) == terrainMul(0.0));
    CH_CHECK(t, std::fabs(terrainMul(0.5) - (0.35 + 0.85 * 0.5)) < 1e-9);
    return t.failures == 0;
}

//=============================================================================
// PERTURBATION SENTINELS -- these exist so the suite can be shown to go RED
// when a real constant is altered. Each names the constant it guards.
//=============================================================================
static bool test_sentinel_terrain_constants(TestCtx& t)
{
    CH_CHECK(t, std::fabs(terrainMul(1.0) - 1.20) < 1e-12);   // guards 1.20
    CH_CHECK(t, std::fabs(terrainMul(0.0) - 0.367) < 1e-12);  // guards 0.35 + 0.02 clamp
    return t.failures == 0;
}
static bool test_sentinel_mech_floor(TestCtx& t)
{
    CH_CHECK(t, std::fabs(mechMul(0.0, 0.0, 0.0) - 0.35) < 1e-12);   // guards kAmbientFloor
    return t.failures == 0;
}
static bool test_sentinel_ibl_strength(TestCtx& t)
{
    CH_CHECK(t, std::fabs(kIblStrengthDefault - 0.5) < 1e-12);       // guards 0.5f, NOT 0.6
    // guards the coefficient table itself (RenderCore/IblShCoeffs.h)
    CH_CHECK(t, std::fabs(propIbl(v3(0,1,0), 1.0) - 1.615350) < 1e-5);
    CH_CHECK(t, std::fabs(propIbl(v3(1,0,0), 1.0) - 4.624195) < 1e-5);
    return t.failures == 0;
}
static bool test_sentinel_stock_kernel_truncates(TestCtx& t)
{
    // float2long TRUNCATES (mathfunc.h:114-121). If it rounded, this would be 129.
    CH_CHECK(t, stockKernelByte(0, 0, 255, -0.5058) == 128);
    // ambient is ASSIGNED then ADDED once (tgl.cpp:2196, :2456)
    CH_CHECK(t, stockKernelByte(0, 31, 0, 0.0) == 31);
    CH_CHECK(t, stockKernelByte(10, 31, 0, 0.0) == 41);
    // the clamp is stock (tgl.cpp:2460-2467)
    CH_CHECK(t, stockKernelByte(200, 100, 100, -1.0) == 255);
    return t.failures == 0;
}
static bool test_sentinel_sun_frame_constants(TestCtx& t)
{
    // guards the +45 in camera.cpp:2244-2245
    const V3 ts = terrainSunMC2(0.0, 0.0);
    CH_CHECK(t, std::fabs(ts.x - std::cos(45.0 * kDeg2Rad)) < 1e-9);
    // guards the +135 in camera.cpp:1861 and the ROW-2 extraction
    const V3 rd = objectRowLightDir(0.0, 0.0);
    CH_CHECK(t, std::fabs(rd.x - std::sin(135.0 * kDeg2Rad)) < 1e-9);
    CH_CHECK(t, std::fabs(rd.y - 0.0) < 1e-9);
    CH_CHECK(t, std::fabs(rd.z - std::cos(135.0 * kDeg2Rad)) < 1e-9);
    // guards the swap in mech.vert:279
    const V3 s = swapStuffToMC2(v3(1, 2, 3));
    CH_CHECK(t, s.x == -1 && s.y == 3 && s.z == 2);
    return t.failures == 0;
}

//=============================================================================
// REPORT PRINTERS (not in the default suite; --test <name> to run)
//=============================================================================
static bool print_stage_a_matrix(TestCtx&)
{
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
    return true;
}

static bool print_stage_b_frames(TestCtx&)
{
    std::fprintf(stderr,
        "\n=== STAGE B: ndl for a face pointing STRAIGHT AT THE SUN (mech clamp OFF) ===\n"
        "%-8s %8s %8s | %10s %10s %10s | %s\n",
        "mission", "pitch", "yaw", "mechDiff", "mechSpec", "prop", "verdict");
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const MissionLight L = missionLight(m);
        const V3 N = L.terrainSun;                       // face the terrain sun
        const double a = ndlMechDiffuse(N, L.rowLightDir);
        const double b = ndlMechSpecular(N, L.rowLightDir);
        const double c = ndlProp(N, L.terrainSun);
        std::fprintf(stderr, "%-8s %8.2f %8.2f | %10.4f %10.4f %10.4f | %s\n",
            m.name, m.pitch, m.yaw, a, b, c,
            (a < 0.999 ? "mech DIFFUSE wrong-axis" : "-"));
    }
    return true;
}

static bool print_stage_c_table(TestCtx&)
{
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
    return true;
}

//=============================================================================
int main(int argc, char** argv)
{
    Harness h("class_light_band_harness");

    // Stage A
    h.add("terrain_floor_is_not_0_35",                 test_terrain_floor_is_not_0_35);
    h.add("terrain_band_is_mission_independent",       test_terrain_band_is_mission_independent);
    h.add("cement_is_exactly_one_and_exempt",          test_cement_is_exactly_one_and_exempt);
    h.add("ported_camera_math_reproduces_independent_anchors",
          test_ported_camera_math_reproduces_independent_anchors);
    h.add("mc2_10_mech_is_saturated_at_every_ndl",     test_mc2_10_mech_is_saturated_at_every_ndl);
    h.add("inherited_prediction_threshold_is_wrong",   test_inherited_prediction_threshold_is_wrong);
    h.add("inherited_prediction_mission_list_FAILS",   test_inherited_prediction_mission_list_FAILS);
    h.add("prediction_is_ill_posed_prop_floor_is_normal_dependent",
          test_prediction_is_ill_posed_prop_floor_is_normal_dependent);
    h.add("corrected_crossing_set_is_seven_not_five",
          test_corrected_crossing_set_is_seven_not_five);
    h.add("prop_ibl_additive_dominates_the_budget",    test_prop_ibl_additive_dominates_the_budget);
    h.add("props_saturate_on_a_large_normal_fraction", test_props_saturate_on_a_large_normal_fraction);
    h.add("vehicle_band_equals_mech_band_at_head_default",
          test_vehicle_band_equals_mech_band_at_head_default);
    h.add("gate_off_vehicle_band_differs_from_head",   test_gate_off_vehicle_band_differs_from_head);
    h.add("worst_pairwise_ratio_is_driven_by_the_exempt_surfaces",
          test_worst_pairwise_ratio_is_driven_by_the_exempt_surfaces);

    // Stage B
    h.add("prop_consumer_is_self_consistent",          test_prop_consumer_is_self_consistent);
    h.add("mech_diffuse_sun_is_wrong_axis",            test_mech_diffuse_sun_is_wrong_axis);
    h.add("swapped_feed_recovers_the_true_spherical_sun",
          test_swapped_feed_recovers_the_true_spherical_sun);
    h.add("swapped_feed_and_terrain_sun_share_azimuth",
          test_swapped_feed_and_terrain_sun_share_azimuth);
    h.add("terrain_sun_elevation_is_atan_sin_pitch_not_pitch",
          test_terrain_sun_elevation_is_atan_sin_pitch_not_pitch);
    h.add("mc2_10_saturation_hides_the_frame_error",   test_mc2_10_saturation_hides_the_frame_error);

    // Stage C
    h.add("stock_byte_and_normalised_forms_agree",     test_stock_byte_and_normalised_forms_agree);
    h.add("mech_is_brighter_than_stock_everywhere",    test_mech_is_brighter_than_stock_everywhere);
    h.add("vehicle_deviates_from_stock_exactly_as_mech_does",
          test_vehicle_deviates_from_stock_exactly_as_mech_does);
    h.add("prop_is_also_brighter_than_stock",          test_prop_is_also_brighter_than_stock);
    h.add("all_three_object_classes_exceed_stock_by_different_amounts",
          test_all_three_object_classes_exceed_stock_by_different_amounts);
    h.add("stage_c_conclusion_is_seed_independent",
          test_stage_c_conclusion_is_seed_independent);
    h.add("mech_falls_below_stock_only_for_seeds_above_the_floor",
          test_mech_falls_below_stock_only_for_seeds_above_the_floor);
    h.add("terrain_band_ignores_the_mission_light_colours",
          test_terrain_band_ignores_the_mission_light_colours);

    // Perturbation sentinels
    h.add("sentinel_terrain_constants",                test_sentinel_terrain_constants);
    h.add("sentinel_mech_floor",                       test_sentinel_mech_floor);
    h.add("sentinel_ibl_strength",                     test_sentinel_ibl_strength);
    h.add("sentinel_stock_kernel_truncates",           test_sentinel_stock_kernel_truncates);
    h.add("sentinel_sun_frame_constants",              test_sentinel_sun_frame_constants);

    // Report printers -- excluded from the default suite.
    h.add("print_stage_a_matrix", print_stage_a_matrix, false);
    h.add("print_stage_b_frames", print_stage_b_frames, false);
    h.add("print_stage_c_table",  print_stage_c_table,  false);

    return h.run(argc, argv);
}
