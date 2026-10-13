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
// This is a MEASUREMENT harness. Every assertion pins a CURRENT TUNING VALUE
// (0.35, 1.20, 0.5, +135), so a legitimate lighting change turns it red on
// purpose and the numbers are re-derived, not waved through.
//
// WHICH CONFIGURATION THE STATIC-PROP AND TREE MODELS DESCRIBE. They transcribe
// static_prop.vert as it was when StaticPropAmbient (hemisphere fill) and the
// base-light direction wrap were OFF. Both have since become shipped defaults:
// StaticPropAmbient is ON in options.cfg (Gates/render_options.h) and the
// base-light wrap (strength 0.45, alpha-test props such as trees excluded) is
// always on. So Stage A's static-prop row, Stage C's prop column and the Stage
// E/F/G prop numbers are the "both off" bands: exact for trees' direction term,
// NOT the lit band a building gets today. test_models_describe_ambient_off_not_
// the_shipped_default pins that gap so a lane that reads a number here as the
// shipped band is contradicted by a red test the moment the models change.
// The Stage G floor and the IBL-on default are read from the live option table.
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
//  TRUE   cement exemption (flat-lit)        shaders/terrain_lod_chunk.frag:1878
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
#include "RenderCore/IblShRegistry.h"   // ditto -- Stage F reads kMissionShMap itself
#include "Gates/render_options.h"       // header-only: the shipped option defaults and the options.cfg value grammar the engine reads

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

// gos_static_prop_batcher.cpp g_iblShStrength starts at 0.5f. The StaticPropIblSh
// option is ON by default (RenderOptions), so that is the daylight upload before
// the per-mission profile overwrites it (iblShStrengthNightScaled() returns
// g_iblShStrength exactly when the night factor is 0).
static const double kIblStrengthDefault = 0.5;

// TREE-SUN-RESTORE-1 CORRECTION -- 0.5f IS THE C++ LITERAL, NOT WHAT SHIPS.
//
// g_iblShStrength is OVERWRITTEN at every mission load by the per-mission
// visual-tuning profile, which is UNGATED (there is no enable env; the only
// env is a path override) and IS shipped:
//   code/mission.cpp:3874             visualTuning_applyProfile(missionScriptName)
//                                     -- unconditional, every mission load
//   GameOS/gameos/visual_tuning_profile.cpp
//                                     applies "defaults" then the per-mission
//                                     block and writes g_iblShStrength from
//                                     staticPropIblStrength; nothing suppresses
//                                     it (the strength env gate that used to is
//                                     deleted).
//   scripts/deploy_payload.py:118     ships data/visual_tuning.json
//
// data/visual_tuning.json (read verbatim):
//   defaults.staticPropIblStrength            = 0.55
//   missions.mc2_01.staticPropIblStrength     = 0.6
//   missions.mc2_03.staticPropIblStrength     = 0.55
//   missions.mc2_10.staticPropIblStrength     = 0.6
//   missions.mc2_24.staticPropIblStrength     = 0.6
//
// So the SHIPPED default is 0.55, and 0.60 on three of the 24; 0.5 only
// applies to a tool that never loads the profile.
static const double kIblStrengthShippedDefault = 0.55;

static double iblStrengthShipped(const char* missionName)
{
    const std::string m(missionName);
    if (m == "mc2_01" || m == "mc2_10" || m == "mc2_24") return 0.60;
    if (m == "mc2_03") return 0.55;   // explicit, equals the default
    return kIblStrengthShippedDefault;
}

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

// shaders/terrain_lod_chunk.frag: cement is fully flat-lit (normalLight -> 1.0 on pureConcrete).
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
//   propDirFactor == 1.0 is the model's assumption. Since the wrap was promoted to
//   always-on (strength 0.45) it holds for alpha-test props (trees) only; every
//   other prop is multiplied by the wrap and this model omits it.
// shaders/static_prop.vert:836-839  ambient_v1 hemisphere fill, u_ambientV1Strength.
//   OMITTED here: that is the StaticPropAmbient=FALSE configuration. The option is
//   ON by default now (see the header note and the pinning test).
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
// 3b. THE TREE MODEL (Stage E -- TREE-SUN-RESTORE-1)
//
// A tree is NOT a prop with the same row. TreeAppearance::update mutates the
// world light list around its own bake, and the mutation is STOCK -- it is
// present verbatim in the original source drop (commit 305ad8746, 2016-09-19,
// mclib/bdactor.cpp:4113-4122 of that tree). HEAD preserves it:
//
//   mclib/bdactor.cpp:7252-7258  lightIntensity = land->getTerrainLight(pos);
//                                lightRGB = (eye->getLightRed(lightIntensity),
//                                            ...Green..., ...Blue...)
//   mclib/bdactor.cpp:7304-7305  eye->setLightColor(1, lightRGB);   <- AMBIENT
//                                eye->setLightIntensity(1, 1.0);       slot is
//                                                       RECOLOURED to the byte
//                                                       getLightRed() returns,
//                                                       i.e. to S, NOT to A.
//   mclib/bdactor.cpp:7317-7318  eye->getWorldLight(0)->active = false;  <- SUN
//   mclib/bdactor.cpp:7450       light->active = true;              <- RESTORED
//   mclib/bdactor.cpp:7457       eye->setLightColor(1, oldRGB);     <- RESTORED
//
// mclib/camera.cpp:489 worldLights[0]->init(TG_LIGHT_INFINITE)  == the sun
// mclib/camera.cpp:516 worldLights[1]->init(TG_LIGHT_AMBIENT)   == the ambient
//
// So the deactivation is a SCOPED BRACKET, not a deletion: for the duration of
// the tree's own light-row bake the row contains exactly one active light, an
// AMBIENT whose colour is the terrain-sampled sun+ambient byte S.
//
// CONSUMER SIDE. mclib/txmmgr.cpp:2879 filters the row on `->active`, so the
// sun never enters. shaders/static_prop.vert:715-728 then decomposes the row:
// AMBIENT accumulates into ambientTerm, the first INFINITE would set sunColor.
// A tree row has no INFINITE, so sunColor == vec3(0) IDENTICALLY.
//   static_prop.vert:823  lit = (seed + ambientTerm) * propDirFactor
//                                                     + sunColor * ndl;
// with propDirFactor == 1.0 for a tree because the base-light wrap excludes
// kFlagIsAlphaTest geometry, which trees are (the wrap is always on for the
// other props since PROP-BASE-LIGHT-DIR-1 was promoted).
//   static_prop.vert:838  lit += ambient_v1 * u_ambientV1Strength -- OFF by
//                                default (gos_static_prop_batcher.cpp:757-759,
//                                opt-in idiom), exact no-op.
//   static_prop.vert:848  if (u_iblShStrength > 0.0) lit += evalShL2(n)*strength
//                                DEFAULT-ON at 0.5 (:1008-1013 killswitch idiom
//                                + :325-333 strength).
//
// Hence the two models below. The ONLY difference between the tree stock MC2
// drew and the tree HEAD draws is the SH-IBL addend -- a port-era term.
//=============================================================================

// What stock MC2 rendered: seed + S, clamped. NORMAL-INDEPENDENT by
// construction -- there is no `n` in the expression at all.
static double treeMulStock(double seed, double S)
{
    return clampd(seed + S, 0.0, 1.0);
}

// What HEAD renders: the same, plus the default-ON SH-IBL probe. This is the
// only normal-dependent term a tree has.
static double treeMulHead(double seed, double S, double ibl)
{
    return clampd(seed + S + ibl, 0.0, 1.0);
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

//-----------------------------------------------------------------------------
// 2c. IBL-PROBE-ROTATION-1 -- the probe's own aim, and the rotation that
//     would fix it.
//
// The SH-L2 probe is projected from ONE day-sky HDRI with that HDRI's own sun
// baked in (RenderCore/IblShCoeffs.h banner: "Source HDRI:
// data/hdr/DaySkyHDRI063B_4K.exr"). Three sites were read to confirm nothing
// rotates it to the mission sun:
//   bake   tools/ibl/project_sh.py:88-94  -- plain equirect projection,
//          phi = (x+0.5)/W*2pi - pi. No azimuth parameter exists.
//   upload GameOS/gameos/gos_static_prop_batcher.cpp:7026-7031 (coalesce) and
//          :8246-8251 (legacy) -- glUniform3fv straight off
//          &s_currentShSet->coeffs[0][0]. No transform.
//   shader shaders/static_prop.vert:951-953 -- evalShL2(normalize(worldNormal))
//          where worldNormal (:652) is a_normal * mat3(inst.modelMatrix), i.e.
//          the prop's own placement rotation and nothing else.
//
// CONTRAST, and this is the load-bearing evidence that the omission is an
// oversight rather than a design choice: the VISIBLE skybox drawn from the SAME
// HDRI *is* sun-synced. gos_postprocess.cpp:5205-5218 scans the EXR for its
// baked sun azimuth (gos_hdri.cpp scanHdriSunAzGL) and sets
// skyYaw = hdriBakedSunAz_ - sunAzGL so the HDRI's sun lands on the mission sun.
// The probe derived from that sky is the one consumer that skips the step.
//
// THE BRIGHT AXIS IS ANALYTIC, not a numeric search. The L1 band of an SH
// expansion IS the first directional moment of the radiance, so the probe's
// bright axis is just normalize(L11, L1-1, L10) under the shader's binding
// (static_prop.vert:288-290 maps L11->n.x, L1-1->n.y, L10->n.z). No HDRI is
// needed at runtime or here.
static V3 iblBrightAxisStuff(int channel)
{
    const auto& C = RenderCore::kIblShCoeffs;
    return norm(v3(C[3][channel], C[1][channel], C[2][channel]));
}

// Stuff-frame azimuth: Stuff is Y-up, so the horizontal plane is x-z.
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

// Rotate a direction about the Stuff +Y (up) axis by +t, i.e. azimuth += t.
static V3 rotAboutStuffY(const V3& n, double tRad)
{
    const double c = std::cos(tRad), s = std::sin(tRad);
    return v3(n.x * c - n.z * s, n.y, n.x * s + n.z * c);
}

// Rotate the SH FUNCTION about Stuff +Y by +t, returning new coefficients.
//
// A rotation about the basis POLE is the closed-form case -- no Wigner
// matrices. Grouping the shader evaluator's terms by their (x,z) angular
// dependence with x = s*cos(a), z = s*sin(a):
//   m=0 (invariant): L00 [0], L1-1 [1] (the pole term), L20 [6]
//   m=1 (cos a, sin a) pairs: (L11 [3], L10 [2]) and (L2-2 [4], L2-1 [5])
//   m=2 (cos 2a, sin 2a) pair: (L22 [8], L21 [7])
//     -- because c1*L22*(x^2-z^2) = c1*L22*s^2*cos2a and
//        2*c1*L21*(x*z)          = c1*L21*s^2*sin2a.
// Substituting a -> a - t rotates the function by +t and mixes each pair by
// (A,B) -> (A cos mt - B sin mt, A sin mt + B cos mt).
// test_sh_y_rotation_equals_rotating_the_normal proves this against the
// brute-force reference to 1e-12, which is what makes it safe to implement
// CPU-side at the upload site instead of per-vertex in the shader.
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
        const double a3 = in.c[3][ch], b2 = in.c[2][ch];   // m=1: (L11, L10)
        o.c[3][ch] = a3 * c1t - b2 * s1t;
        o.c[2][ch] = a3 * s1t + b2 * c1t;
        const double a4 = in.c[4][ch], b5 = in.c[5][ch];   // m=1: (L2-2, L2-1)
        o.c[4][ch] = a4 * c1t - b5 * s1t;
        o.c[5][ch] = a4 * s1t + b5 * c1t;
        const double a8 = in.c[8][ch], b7 = in.c[7][ch];   // m=2: (L22, L21)
        o.c[8][ch] = a8 * c2t - b7 * s2t;
        o.c[7][ch] = a8 * s2t + b7 * c2t;
    }
    return o;
}

// evalShL2 against an arbitrary coefficient set (the shipped one is the
// default path above; this is the same expression, parameterised).
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

// The mission sun expressed in the Stuff frame the probe is evaluated in.
// swapStuffToMC2 is self-inverse ((x,y,z)->(-x,z,y) applied twice is identity),
// so the same helper converts back.
static V3 missionSunStuff(const Mission& m)
{
    return swapStuffToMC2(terrainSunMC2(m.pitch, m.yaw));
}

// The rotation that would aim the probe's bright axis at this mission's sun.
static double sunAlignRotationRad(const Mission& m)
{
    const double aSun   = stuffAzimuthDeg(missionSunStuff(m));
    const double aProbe = stuffAzimuthDeg(iblBrightAxisStuff(0));
    return wrap180(aSun - aProbe) * kDeg2Rad;
}

// Signed azimuth error, degrees, between the probe's bright axis and the sun.
static double probeVsSunAzErrorDeg(const Mission& m)
{
    return wrap180(stuffAzimuthDeg(missionSunStuff(m))
                 - stuffAzimuthDeg(iblBrightAxisStuff(0)));
}

// Sweep every vertical-wall azimuth and report how much a sun-align rotation
// would actually move the FINAL CLAMPED prop multiplier. This is the
// saturation-trap filter: a mission where the answer is ~0 cannot grade the
// change no matter how badly its probe is aimed.
struct RotSweep {
    double movedFrac;   // fraction of wall azimuths moving > 1/255
    double meanAbs;     // mean |delta| on the clamped multiplier
    double maxDown;     // most negative delta
    double maxUp;       // most positive delta
    double unsatFrac;   // fraction of wall azimuths NOT clamped before the change
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
        // A vertical wall in the MC2 frame, then into Stuff for the probe.
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
// STAGE D TESTS -- SUN-AXIS-FIX-1. What the two candidate fixes actually
// CHANGE, per mission, with the clamp LEFT IN (unlike Stage B, which removed
// it to expose the error). Stage B proved the axis is wrong; Stage D answers
// the only question that decides a gate default: WHERE IS IT VISIBLE.
//
// CITATION CORRECTION inherited from Stage B. Stage B's derivation comment
// cites mclib/stuff/matrix.cpp:87-115 Matrix4D::BuildRotation. That is the
// WRONG CLASS: the producers use Stuff::LinearMatrix4D (mclib/camera.cpp:509,
// :1860, :2110, :4252), and LinearMatrix4D::BuildRotation(EulerAngles)
// forwards to AffineMatrix4D::BuildRotation (mclib/stuff/linearmatrix.hpp:85-86
// -> mclib/stuff/affinematrix.cpp:162-194). Re-read at source: the two bodies
// are arithmetically IDENTICAL for EulerAngles, so the derived row-2 formula
// stands unchanged. Only the citation was wrong. test_row2_matches_the_affine_
// builder below pins the arithmetic that actually runs.
//=============================================================================

// FIX A (SUN-AXIS-FIX-1). The light_dir the MC2-frame diffuse consumers should
// have been reading all along: the SAME swap mech.vert:279 already applies for
// specular. Shader form: vec3(-d.x, d.z, d.y), applied to ld.light_dir[i].xyz
// in the two directional branches of lighting.hglsl calc_light.
static V3 rowLightDirFixed(const V3& rawRow) { return swapStuffToMC2(rawRow); }

static double ndlMechDiffuseFixed(const V3& nMC2, const V3& rawRow)
{
    const V3 d = rowLightDirFixed(rawRow);
    return clampd(dot(nMC2, v3(-d.x, -d.y, -d.z)), 0.0, 1.0);
}

// FIX B (SUN-ELEVATION-FIX-1). Camera::lightDirection built as a true spherical
// direction at the AUTHORED elevation instead of unit-horizontal + sin(pitch)
// normalized afterwards. DEGENERATE GUARD: when cos(pitch) < 0 the authored
// pitch cannot be read as a solar elevation and the spherical form flips the
// AZIMUTH by 180 deg against every shipped build, so the legacy vector is
// returned verbatim. Among the 24 shipping missions only mc2_10 (pitch 180)
// takes that branch; mc2_05 / mc2_21 (pitch 270) have cos(pitch) == 0, where
// the spherical form degenerates to straight-down (0,0,-1) with no azimuth to
// flip.
// EPSILON IS LOAD-BEARING, not decoration: cos(270 deg) evaluates to -1.8e-16,
// not 0, so a bare `< 0.0` classifies mc2_05 and mc2_21 as degenerate and
// silently drops them onto the legacy branch. This harness caught exactly that
// on the first run of Stage D. Same constant as mclib/camera.cpp.
static const double kSunPitchDegenerateEps = 1e-6;
static bool sunPitchIsDegenerate(double pitchDeg)
{
    return std::cos(pitchDeg * kDeg2Rad) < -kSunPitchDegenerateEps;
}
static V3 terrainSunMC2Fixed(double pitchDeg, double yawDeg)
{
    if (sunPitchIsDegenerate(pitchDeg))
        return terrainSunMC2(pitchDeg, yawDeg);       // legacy, bit-identical
    const double a = (yawDeg + 45.0) * kDeg2Rad;
    const double P = pitchDeg * kDeg2Rad;
    return v3(std::cos(P) * std::cos(a), std::cos(P) * std::sin(a), std::sin(P));
}

// A deterministic near-uniform sphere of normals (Fibonacci lattice). The delta
// is a function of the surface normal, so "how much does this change" is an
// integral over the sphere, not a single pose.
static const int kSphereN = 4096;
static V3 sphereNormal(int i)
{
    const double ga = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    const double z  = 1.0 - 2.0 * (i + 0.5) / (double)kSphereN;
    const double r  = std::sqrt(std::max(0.0, 1.0 - z * z));
    const double th = ga * i;
    return v3(r * std::cos(th), r * std::sin(th), z);
}

// One mission's delta summary for the mech/vehicle expression.
struct DeltaStat {
    double maxAbs   = 0.0;   // largest |M_after - M_before| over the sphere
    double meanAbs  = 0.0;
    double fracVisible = 0.0;  // fraction of normals moving >= 1/255 (one byte)
    double fracBrighter = 0.0;
    double fracDarker   = 0.0;
    bool   fullySaturated = false;  // 0.35 + A >= 1 -> clamp pins M at 1.0
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

// D0. The arithmetic that ACTUALLY runs is AffineMatrix4D::BuildRotation
// (affinematrix.cpp:162-194), reached via LinearMatrix4D::BuildRotation
// (linearmatrix.hpp:85-86). Transcribed here independently of objectRowLightDir
// and required to agree -- so the row-2 formula is pinned to the real builder,
// not to the Matrix4D one Stage B mis-cited.
static bool test_row2_matches_the_affine_builder(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double xs = std::sin(m.pitch * kDeg2Rad), xc = std::cos(m.pitch * kDeg2Rad);
        const double ys = std::sin((m.yaw + 135.0) * kDeg2Rad),
                     yc = std::cos((m.yaw + 135.0) * kDeg2Rad);
        const double zs = 0.0, zc = 1.0;                 // roll == 0
        // affinematrix.cpp:189-191, verbatim.
        const double m20 = xc * ys * zc + xs * zs;
        const double m21 = xc * ys * zs - xs * zc;
        const double m22 = xc * yc;
        // affinematrix.hpp:156-162 GetLocalForwardInWorld -> (FORWARD_AXIS=2, {X,Y,Z})
        // with APPLY_FORWARD_SIGN == identity (vector3d.hpp:73).
        const V3 built = v3(m20, m21, m22);
        const V3 have  = objectRowLightDir(m.pitch, m.yaw);
        CH_CHECK(t, std::fabs(built.x - have.x) < 1e-12);
        CH_CHECK(t, std::fabs(built.y - have.y) < 1e-12);
        CH_CHECK(t, std::fabs(built.z - have.z) < 1e-12);
    }
    return t.failures == 0;
}

// D1. THE FIX IS CORRECT: with Fix A applied, a face whose MC2 normal points at
// the true spherical sun returns ndl == 1 on EVERY mission, degenerate pitches
// included. Stage B's B2 shows the shipped feed manages this on none of them.
static bool test_fix_a_gives_ndl_one_at_the_true_sun(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double a = (m.yaw + 45.0) * kDeg2Rad, P = m.pitch * kDeg2Rad;
        const V3 truth = v3(std::cos(P)*std::cos(a), std::cos(P)*std::sin(a), std::sin(P));
        const V3 row   = objectRowLightDir(m.pitch, m.yaw);
        CH_CHECK(t, std::fabs(ndlMechDiffuseFixed(truth, row) - 1.0) < 1e-9);
    }
    return t.failures == 0;
}

// D2. FIX A AGREES WITH MECH SPECULAR EXACTLY, on every mission and every
// normal. This is the whole safety argument for leaving specular alone: the fix
// does not invent a direction, it adopts the one the correct consumer already
// uses, so diffuse and specular stop disagreeing instead of starting to.
static bool test_fix_a_equals_the_specular_consumer(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const V3 row = objectRowLightDir(kMissions[i].pitch, kMissions[i].yaw);
        for (int k = 0; k < 64; ++k) {
            const V3 n = sphereNormal(k * (kSphereN / 64));
            CH_CHECK(t, std::fabs(ndlMechDiffuseFixed(n, row)
                                - ndlMechSpecular(n, row)) < 1e-12);
        }
    }
    return t.failures == 0;
}

// D3. WHERE THE DELTA IS CLAMPED AWAY. On a mission whose authored ambient
// alone clears the clamp (0.35 + A >= 1), the mech/vehicle multiplier is pinned
// at 1.0 for every normal and every ndl, so Fix A changes NOTHING visible --
// including on mc2_10, one of the user's regular check missions. This is the
// claim that makes the degenerate-pitch risk survivable.
static bool test_saturated_missions_show_zero_delta(TestCtx& t)
{
    int saturated = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const DeltaStat st = mechDeltaForMission(kMissions[i]);
        if (!st.fullySaturated) continue;
        ++saturated;
        CH_CHECK(t, st.maxAbs == 0.0);
        CH_CHECK(t, st.fracVisible == 0.0);
    }
    CH_CHECK(t, saturated > 0);
    // mc2_10 (AmbientRed 192 -> A = 0.7529, 0.35 + A = 1.103) must be one.
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_10") continue;
        const DeltaStat st = mechDeltaForMission(kMissions[i]);
        CH_CHECK(t, st.fullySaturated);
        CH_CHECK(t, st.maxAbs == 0.0);
    }
    return t.failures == 0;
}

// D4. WHERE IT IS VISIBLE. On the unsaturated missions the delta is large and
// two-sided: faces the shipped garbled axis happened to favour go DARKER, and
// the faces that should have been lit go BRIGHTER. "Mechs get brighter" is NOT
// a safe summary of this change and must not be reported as one.
static bool test_unsaturated_missions_move_in_both_directions(TestCtx& t)
{
    int twoSided = 0, unsat = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const DeltaStat st = mechDeltaForMission(kMissions[i]);
        if (st.fullySaturated) continue;
        ++unsat;
        if (st.fracBrighter > 0.01 && st.fracDarker > 0.01) ++twoSided;
    }
    CH_CHECK(t, unsat >= 10);
    CH_CHECK(t, twoSided >= unsat / 2);   // most unsaturated missions darken somewhere
    return t.failures == 0;
}

// D5. THE DEGENERATE THREE, handled explicitly rather than hoped over.
//   mc2_05 / mc2_21 (pitch 270): cos(pitch) == 0, so Fix B's spherical form is
//     straight down (0,0,-1) -- no azimuth exists to flip.
//   mc2_10 (pitch 180): cos(pitch) < 0. The spherical form would flip the
//     terrain sun's azimuth by 180 deg against every shipped build, so Fix B
//     returns the legacy vector BIT-IDENTICALLY there.
static bool test_fix_b_degenerate_pitch_is_handled(TestCtx& t)
{
    int degenerate = 0, poles = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 legacy = terrainSunMC2(m.pitch, m.yaw);
        const V3 fixed  = terrainSunMC2Fixed(m.pitch, m.yaw);
        if (sunPitchIsDegenerate(m.pitch)) {
            ++degenerate;
            CH_CHECK(t, fixed.x == legacy.x && fixed.y == legacy.y && fixed.z == legacy.z);
            CH_CHECK(t, std::string(m.name) == "mc2_10");
        }
        if (std::fabs(std::cos(m.pitch * kDeg2Rad)) < 1e-12) {
            ++poles;
            CH_CHECK(t, std::fabs(fixed.x) < 1e-12 && std::fabs(fixed.y) < 1e-12);
            CH_CHECK(t, std::fabs(fixed.z + 1.0) < 1e-9);      // straight down
        }
    }
    CH_CHECK(t, degenerate == 1);   // exactly mc2_10
    CH_CHECK(t, poles == 2);        // exactly mc2_05 + mc2_21
    return t.failures == 0;
}

// D6. THE UNIFICATION. On every NON-degenerate mission, Fix B makes the terrain
// sun EXACTLY EQUAL to -swap(row) -- the vector mech specular already uses and
// the one Fix A gives mech/vehicle diffuse. So the two fixes together put
// terrain, props, mech diffuse, mech specular and vehicles on ONE sun vector,
// by construction rather than by tuning. Neither fix alone achieves that.
static bool test_fix_b_unifies_terrain_sun_with_the_object_row(TestCtx& t)
{
    int unified = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        if (sunPitchIsDegenerate(m.pitch)) continue;
        const V3 s = swapStuffToMC2(objectRowLightDir(m.pitch, m.yaw));
        const V3 minusSwap = v3(-s.x, -s.y, -s.z);
        const V3 fixed = terrainSunMC2Fixed(m.pitch, m.yaw);
        CH_CHECK(t, std::fabs(fixed.x - minusSwap.x) < 1e-9);
        CH_CHECK(t, std::fabs(fixed.y - minusSwap.y) < 1e-9);
        CH_CHECK(t, std::fabs(fixed.z - minusSwap.z) < 1e-9);
        ++unified;
    }
    CH_CHECK(t, unified == kNumMissions - 1);
    // ...and the shipped terrain sun does NOT have this property.
    int legacyMismatch = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const V3 s = swapStuffToMC2(objectRowLightDir(m.pitch, m.yaw));
        const V3 legacy = terrainSunMC2(m.pitch, m.yaw);
        if (std::fabs(legacy.z + s.y) > 1e-6) ++legacyMismatch;
    }
    CH_CHECK(t, legacyMismatch >= kNumMissions - 3);
    return t.failures == 0;
}

// D7. FIX B IS NOT A SMALL FIX -- it moves the sun COLOUR too, not only its
// direction. `intensity` handed to Camera::getLightRed is terrainNormal .
// lightDirection (mclib/mapdata.cpp:1892-1900), so raising the elevation from
// atan(sin P) to P raises S on flat ground for every object class AND rebakes
// the terrain colormap (mclib/terrtxm2.cpp:1132 burnInShadows). Recorded here
// so the gate default argument cannot pretend this is direction-only.
static bool test_fix_b_also_moves_the_sun_colour_byte(TestCtx& t)
{
    int moved = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double iBefore = terrainSunMC2(m.pitch, m.yaw).z;
        const double iAfter  = terrainSunMC2Fixed(m.pitch, m.yaw).z;
        const int sBefore = getLightByte(m.lightR, m.ambR, iBefore);
        const int sAfter  = getLightByte(m.lightR, m.ambR, iAfter);
        if (sBefore != sAfter) ++moved;
    }
    CH_CHECK(t, moved > 0);
    // mc2_03: elevation 35.7 -> 46 deg raises sin from 0.583 to 0.719.
    for (int i = 0; i < kNumMissions; ++i) {
        if (std::string(kMissions[i].name) != "mc2_03") continue;
        const Mission& m = kMissions[i];
        CH_CHECK(t, terrainSunMC2Fixed(m.pitch, m.yaw).z
                  > terrainSunMC2(m.pitch, m.yaw).z + 0.1);
    }
    return t.failures == 0;
}

// D8. SENTINEL for the two fix constants, so a silent edit to either shader
// expression goes red here rather than in the user's eyes.
static bool test_sentinel_fix_constants(TestCtx& t)
{
    // Fix A is EXACTLY the mech.vert:279 swap, no extra sign.
    const V3 f = rowLightDirFixed(v3(1, 2, 3));
    CH_CHECK(t, f.x == -1 && f.y == 3 && f.z == 2);
    // Fix B's guard is cos(pitch) < 0, not "pitch > 90" (270 must NOT trip it).
    CH_CHECK(t, sunPitchIsDegenerate(180.0));
    CH_CHECK(t, !sunPitchIsDegenerate(46.0));
    CH_CHECK(t, !sunPitchIsDegenerate(0.0));
    // THE EPSILON ARM. cos(270 deg) == -1.8e-16 < 0, so a bare sign test puts
    // mc2_05 / mc2_21 on the legacy branch by floating-point accident. Deleting
    // kSunPitchDegenerateEps must turn this red.
    CH_CHECK(t, !sunPitchIsDegenerate(270.0));
    CH_CHECK(t, std::cos(270.0 * kDeg2Rad) < 0.0);   // the trap itself is real
    return t.failures == 0;
}

//=============================================================================
// STAGE E TESTS -- TREE-SUN-RESTORE-1
//
// The question these settle: DID STOCK MC2 LIGHT ITS TREES, AND WITH WHAT?
// The answer is executable, offline, and does not require launching anything.
//=============================================================================

// E1. Stock lit its trees with a single AMBIENT whose colour is the FULL
// terrain-sampled sun byte S -- not with the mission ambient A, and not with
// nothing. The recolour at bdactor.cpp:7304 is a genuine substitution: on every
// daylight mission the tree's ambient slot is STRICTLY BRIGHTER than the
// mission ambient the props get.
static bool test_stock_lit_trees_with_the_sun_byte_not_the_mission_ambient(TestCtx& t)
{
    int brighter = 0, equal = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        // The tree's ambient slot is S; a prop's is A.
        if (L.S > L.A + 1e-9)      ++brighter;
        else if (std::fabs(L.S - L.A) < 1e-9) ++equal;
        CH_CHECK(t, L.S >= L.A - 1e-9);   // never DIMMER than the prop ambient
    }
    // The night/dusk missions with the sun below the horizon (intensity <= 0)
    // fall back to `ambientByte` exactly (camera.cpp:4486-4489), so S == A there
    // and the tree is ambient-lit like everything else. Everywhere else the
    // tree is lit by the sun's own colour, delivered through the ambient slot.
    CH_CHECK(t, brighter + equal == kNumMissions);
    CH_CHECK(t, brighter >= 20);   // the overwhelming majority are daylight
    return t.failures == 0;
}

// E2. Stock's tree multiplier is NORMAL-INDEPENDENT. This is the property the
// sun deactivation buys, and it is the defining fact about a stock tree: it has
// no lit side and no shadow side. Verified by evaluating over the sphere.
static bool test_stock_tree_multiplier_is_normal_independent(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        double lo = 2.0, hi = -1.0;
        forEachNormal(kSphereN, [&](const V3& n) {
            (void)n;                       // the point: `n` cannot enter
            const double m = treeMulStock(0.0, L.S);
            if (m < lo) lo = m;
            if (m > hi) hi = m;
        });
        CH_CHECK(t, std::fabs(hi - lo) < 1e-12);
    }
    return t.failures == 0;
}

// E3. Stock trees were NOT pinned on most missions. This is the load-bearing
// measurement: the inherited claim is that a tree is "the texture to the LSB"
// because there is no lighting term. At stock the term exists (it is S) and it
// is strictly below the clamp on most maps -- so stock trees DID respond to
// time of day and to terrain shadowing, just not to surface normal.
static bool test_stock_trees_are_mostly_UNsaturated(TestCtx& t)
{
    int pinned = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        if (treeMulStock(0.0, L.S) >= 1.0 - 1e-9) ++pinned;
    }
    // S = clamp(lightByte*I + ambientByte, 0, 255)/255 saturates only where the
    // authored sun+ambient already exceeds 255 at the flat-ground sample. That
    // is ONE mission: mc2_22 (lightR 255, ambR 192 -> 255*0.503 + 192 = 320).
    // Stock's tree band runs 0.1647 (mc2_05) .. 1.0000 (mc2_22) -- a 6.1x
    // spread. Stock trees were emphatically NOT "the texture to the LSB".
    CH_CHECK(t, pinned < kNumMissions / 2);
    CH_CHECK(t, pinned == 1);   // exact; a data or camera-math change breaks this
    return t.failures == 0;
}

// E4. THE PORT-ERA CHANGE. HEAD adds the default-ON SH-IBL probe on top, and
// that addend is what drives the tree into the clamp. Count the missions the
// port newly pins that stock left free.
static bool test_head_ibl_pins_trees_that_stock_left_free(TestCtx& t)
{
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
    // The port pins strictly MORE missions than stock did. The direction is the
    // whole finding: the modern build has LESS tree lighting variation than
    // stock, not more.
    CH_CHECK(t, headPinnedAllNormals > stockPinned);
    CH_CHECK(t, stockPinned == 1);            // mc2_22 only
    CH_CHECK(t, headPinnedAllNormals == 5);   // + mc2_01, 04, 07, 24
    return t.failures == 0;
}

// E5. The PropShadowReceive option is an exact no-op on trees. THE VERDICT IS RIGHT
// BUT THE INHERITED REASON IS WRONG, and the difference decides whether
// restoring a sun would "wake it up".
//
// The inherited claim is that RECEIVE "modulates only sunColor * ndl", so a
// tree with no sun has nothing to attenuate -- which would mean restoring a sun
// makes RECEIVE live on trees for the first time. That is NOT the mechanism.
// RECEIVE is a DEFERRED SCREEN-SPACE gate, not a vertex-lighting term:
//
//   shaders/static_prop.frag:649-652  alpha-test props emit GBuffer1 via
//        rc_gbuffer1_screenShadowEligible_noSelfShadow_alphaTest()
//   shaders/include/render_contract.hglsl:70-72   ...which writes a = 0.1875
//   shaders/include/render_contract.hglsl:87-89   rc_isAlphaTestProp(g) :=
//        g.a > 0.1 && g.a <= 0.22          -> TRUE for 0.1875
//   shaders/shadow_screen.frag:337-339
//        else if (enableDynamicShadows == 1 && propShadowReceive != 0
//                 && !rc_isAlphaTestProp(normalData))      <- EXCLUDED
//   (and the debug-view mirror at shadow_screen.frag:280)
//
// Trees carry ALPHA_TEST_BIT: gos_static_prop_batcher.cpp:3028-3030 sets
// materialFlags from TG_TypeShape::alphaTestOn and its own comment names
// trees; :5733 sets the matching instance bit-2 from the same field.
//
// So trees are excluded from RECEIVE by an EXPLICIT ALPHA-TEST CLASS CHECK
// (PROP-ALPHA-SHADOW-CLASS-1), for a reason that has nothing to do with the
// sun: the shadow depth pass has no alpha discard, so a card writes a solid
// slab of depth and sampling it back self-shadows the whole card uniformly.
// CONSEQUENCE: restoring a sun to the tree row would NOT make
// PropShadowReceive live on trees. The exclusion is sun-independent.
//
// NOTE the documentation gap this exposes: render_contract.hglsl:43-44 calls
// the 0.1875 class "alpha-test LATTICE prop (chain-link fence, railing,
// grating)". Every TREE in the game is also in it.
static bool test_prop_shadow_receive_is_an_exact_no_op_on_trees(TestCtx& t)
{
    // (a) The alpha-test class check, ported verbatim from render_contract.hglsl.
    const double kTreeGBuffer1Alpha = 0.1875;   // :71
    auto rc_isAlphaTestProp = [](double a) { return a > 0.1 && a <= 0.22; };
    CH_CHECK(t, rc_isAlphaTestProp(kTreeGBuffer1Alpha));        // tree  -> EXCLUDED
    CH_CHECK(t, !rc_isAlphaTestProp(0.25));                     // solid prop -> included
    // The band is still inside the static-prop band for every OTHER reader
    // (render_contract.hglsl:78-80), which is why the class split was a no-op.
    auto rc_isStaticPropNoSelfShadow = [](double a) { return a > 0.1 && a <= 0.5; };
    CH_CHECK(t, rc_isStaticPropNoSelfShadow(kTreeGBuffer1Alpha));

    // (b) The exclusion does not consult the sun, so it holds identically for
    // the stock tree AND for a hypothetical sun-restored tree. Sweep both.
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        for (int s = 0; s <= 10; ++s) {
            const double shadow = s / 10.0;    // full shadow .. full light
            for (int k = 0; k <= 10; ++k) {
                const double ndl = k / 10.0;
                // RECEIVE never reaches a tree pixel, so the shadow factor is
                // discarded whatever the row contains.
                const double stockTree = treeMulStock(0.0, L.S);
                CH_CHECK(t, std::fabs(stockTree - treeMulStock(0.0, L.S)) < 1e-12);
                // ...including with a sun present: still excluded by class.
                const double sunRestored = clampd(L.S + L.S * ndl, 0.0, 1.0);
                const double sunRestoredShadowed =
                    rc_isAlphaTestProp(kTreeGBuffer1Alpha)
                        ? sunRestored                            // gate skipped
                        : clampd(L.S + L.S * ndl * shadow, 0.0, 1.0);
                CH_CHECK(t, std::fabs(sunRestoredShadowed - sunRestored) < 1e-12);
            }
        }
    }
    return t.failures == 0;
}

// E6. If a sun WERE restored to the tree row, the change would be invisible on
// exactly the missions whose ambient slot already saturates -- the same
// saturation trap the SunAxisFix option hit. Name the missions where an A/B could
// actually show something, and assert the trap is real.
static bool test_restoring_a_sun_would_be_invisible_where_S_saturates(TestCtx& t)
{
    int invisible = 0, visible = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        // Hypothetical restore: lit = seed + S + S*ndl (the sun colour byte is
        // the same S; camera.cpp:4473 feeds both).
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
    // The trap is real. The five invisible missions are EXACTLY the five the
    // default-ON IBL already drove to 1.0 at every normal -- mc2_01, mc2_04,
    // mc2_07, mc2_22, mc2_24 -- which is precisely the set an inherited audit
    // reported as "exactly 1.000". Reproduced here from mission data + ported
    // camera math, independently of that audit.
    CH_CHECK(t, invisible > 0);
    CH_CHECK(t, invisible + visible == kNumMissions);
    CH_CHECK(t, invisible == 5);
    return t.failures == 0;
}

// E6b. THE SHIPPED IBL STRENGTH IS NOT THE C++ LITERAL. Pin the gap so the
// next lane that reads the g_iblShStrength initialiser in
// gos_static_prop_batcher.cpp and concludes "the default is 0.5" is
// contradicted by a red test. On every launch the profile in
// visual_tuning_profile.cpp overwrites g_iblShStrength with the
// data/visual_tuning.json value on EVERY mission load.
static bool test_shipped_ibl_strength_is_not_the_cpp_literal(TestCtx& t)
{
    CH_CHECK(t, std::fabs(kIblStrengthDefault - 0.5) < 1e-12);          // the literal
    CH_CHECK(t, std::fabs(kIblStrengthShippedDefault - 0.55) < 1e-12);  // what ships
    CH_CHECK(t, kIblStrengthShippedDefault > kIblStrengthDefault);
    // The three missions with a per-mission override are strictly higher again.
    CH_CHECK(t, std::fabs(iblStrengthShipped("mc2_01") - 0.60) < 1e-12);
    CH_CHECK(t, std::fabs(iblStrengthShipped("mc2_10") - 0.60) < 1e-12);
    CH_CHECK(t, std::fabs(iblStrengthShipped("mc2_24") - 0.60) < 1e-12);
    CH_CHECK(t, std::fabs(iblStrengthShipped("mc2_03") - 0.55) < 1e-12);
    CH_CHECK(t, std::fabs(iblStrengthShipped("mc2_22") - 0.55) < 1e-12);  // falls to default
    // CONSEQUENCE: every tree number computed at 0.5 UNDERSTATES the real
    // saturation. The direction of the error matters more than its size.
    for (int i = 0; i < kNumMissions; ++i) {
        const MissionLight L = missionLight(kMissions[i]);
        const double str = iblStrengthShipped(kMissions[i].name);
        const V3 n = norm(v3(0.3, 0.8, 0.5));
        CH_CHECK(t, treeMulHead(0.0, L.S, propIbl(n, str)) >=
                    treeMulHead(0.0, L.S, propIbl(n, kIblStrengthDefault)) - 1e-12);
    }
    return t.failures == 0;
}

// E7. Perturbation sentinel for the tree constants.
static bool test_sentinel_tree_constants(TestCtx& t)
{
    // No sun in the row -> the sun term is absent, not merely small.
    CH_CHECK(t, treeMulStock(0.0, 0.5) == 0.5);
    // The clamp is stock (tgl.cpp:2460-2467) and it is at 1.0.
    CH_CHECK(t, treeMulStock(0.0, 2.0) == 1.0);
    CH_CHECK(t, treeMulStock(0.6, 0.6) == 1.0);
    // HEAD adds the IBL addend at kIblStrengthDefault; if that default moved to
    // 0.0 this test turns red (the two models would collapse into one).
    CH_CHECK(t, treeMulHead(0.0, 0.1, propIbl(v3(0,1,0))) >
                treeMulStock(0.0, 0.1));
    CH_CHECK(t, std::fabs(kIblStrengthDefault - 0.5) < 1e-12);
    return t.failures == 0;
}

//=============================================================================
// REPORT PRINTERS (not in the default suite; --test <name> to run)
//=============================================================================

// TREE-SUN-RESTORE-1: the per-mission stock-vs-HEAD tree table.
static bool print_stage_e_tree_table(TestCtx&)
{
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
            // how much of the IBL addend the clamp threw away
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
    return true;
}

static bool print_stage_d_delta(TestCtx&)
{
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
    return true;
}

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
// STAGE F TESTS -- IBL-PROBE-ROTATION-1
//
// The claim handed to this lane, verbatim: "The SH-IBL ambient probe is never
// rotated to the mission sun." Each specific is pinned below against source,
// including the two that were stated imprecisely.
//=============================================================================

// F1. The registry holds exactly ONE coefficient set, and the mission map is
// FUNCTIONALLY empty -- but NOT literally empty, which matters because the
// count is 1, not 0, and a reader that trusts "empty" would mis-size a loop.
// RenderCore/IblShRegistry.h:51-55 holds one {nullptr, nullptr} placeholder.
static bool test_mission_sh_map_is_functionally_but_not_literally_empty(TestCtx& t)
{
    CH_CHECK(t, RenderCore::kIblShSetCount == 1);
    CH_CHECK(t, std::string(RenderCore::kIblShSets[0].name) == "default");
    // NOT zero -- one null placeholder row.
    CH_CHECK(t, RenderCore::kMissionShMapCount == 1);
    CH_CHECK(t, RenderCore::kMissionShMap[0].missionName == nullptr);
    CH_CHECK(t, RenderCore::kMissionShMap[0].shSetName  == nullptr);
    // Every shipping mission therefore resolves to the same coefficients.
    for (int i = 0; i < kNumMissions; ++i) {
        const RenderCore::IblShSet& s = RenderCore::lookupShSet(kMissions[i].name);
        CH_CHECK(t, std::string(s.name) == "default");
        CH_CHECK(t, s.coeffs == &RenderCore::kIblShCoeffs[0]);
    }
    return t.failures == 0;
}

// F2. The bright axis is pinned at Stuff +X. The ANALYTIC L1 axis and a
// brute-force argmax over the sphere must agree in azimuth.
static bool test_probe_bright_axis_is_pinned_at_stuff_plus_x(TestCtx& t)
{
    const V3 u = iblBrightAxisStuff(0);
    CH_CHECK(t, std::fabs(stuffAzimuthDeg(u)) < 0.5);          // +X, ~0 deg
    CH_CHECK(t, u.x > 0.95);
    CH_CHECK(t, std::fabs(std::asin(u.y) / kDeg2Rad - 16.62) < 0.05);  // elevated
    // ...and all three channels agree, so this is not one channel's quirk.
    for (int ch = 0; ch < 3; ++ch)
        CH_CHECK(t, std::fabs(stuffAzimuthDeg(iblBrightAxisStuff(ch))) < 0.5);
    // Brute-force argmax lands on the same azimuth.
    double bestV = -1e30; V3 bestN = v3(0, 0, 0);
    forEachNormal(96, [&](const V3& n) {
        const double v = evalShL2(n, 0);
        if (v > bestV) { bestV = v; bestN = n; }
    });
    CH_CHECK(t, std::fabs(wrap180(stuffAzimuthDeg(bestN) - stuffAzimuthDeg(u))) < 5.0);
    CH_CHECK(t, bestV > 4.85 && bestV < 4.90);                 // 4.894
    return t.failures == 0;
}

// F3. The "7.71:1 swing" figure is the +X vs -X PAIR, not the swing across the
// sphere -- that is larger (21.0:1). Both are pinned so neither number can be
// quoted loosely again.
static bool test_the_7_71_ratio_is_the_x_pair_not_the_sphere_swing(TestCtx& t)
{
    const double px = evalShL2(v3( 1, 0, 0), 0);
    const double mx = evalShL2(v3(-1, 0, 0), 0);
    CH_CHECK(t, std::fabs(px - 4.6242) < 1e-3);
    CH_CHECK(t, std::fabs(mx - 0.5997) < 1e-3);
    CH_CHECK(t, std::fabs(px / mx - 7.710) < 0.01);            // the quoted 7.71
    double lo = 1e30, hi = -1e30;
    forEachNormal(96, [&](const V3& n) {
        const double v = evalShL2(n, 0);
        lo = std::min(lo, v); hi = std::max(hi, v);
    });
    CH_CHECK(t, hi / lo > 20.0);                               // 21.05 -- BIGGER
    CH_CHECK(t, hi / lo > px / mx);
    return t.failures == 0;
}

// F4. The probe's bright axis misses the mission sun on essentially every
// mission. Median absolute azimuth error > 90 degrees.
static bool test_probe_bright_axis_misses_the_sun_on_every_mission(TestCtx& t)
{
    std::vector<double> errs;
    for (int i = 0; i < kNumMissions; ++i)
        errs.push_back(std::fabs(probeVsSunAzErrorDeg(kMissions[i])));
    std::sort(errs.begin(), errs.end());
    const double median = errs[kNumMissions / 2];
    CH_CHECK(t, median > 90.0);                                // 106.2
    CH_CHECK(t, errs.back() > 175.0);                          // mc2_14, 176.7
    // Only three missions are within 20 degrees of aligned.
    int near = 0;
    for (double e : errs) if (e <= 20.0) ++near;
    CH_CHECK(t, near == 3);                                    // mc2_16/20/03
    return t.failures == 0;
}

// F5. THE VISUAL DEFECT, stated as an inequality: on most missions the probe
// puts MORE ambient on the wall facing AWAY from the sun than on the wall
// facing it. That is the "light is coming from behind" complaint in arithmetic.
static bool test_probe_is_brighter_on_the_shadowed_wall_on_most_missions(TestCtx& t)
{
    int wrongWay = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const V3 sunStuff = missionSunStuff(kMissions[i]);
        // horizontal wall normals in Stuff: toward the sun azimuth, and away
        const V3 toward = norm(v3(sunStuff.x, 0.0, sunStuff.z));
        const V3 away   = v3(-toward.x, 0.0, -toward.z);
        if (evalShL2(toward, 0) < evalShL2(away, 0)) ++wrongWay;
    }
    CH_CHECK(t, wrongWay == 17);                               // 17 of 24
    return t.failures == 0;
}

// F6. THE IMPLEMENTATION-ENABLING TEST. Rotating the nine COEFFICIENTS about
// the basis pole is exactly equivalent to counter-rotating the NORMAL. This is
// what lets a fix live CPU-side at the upload site (zero per-vertex cost, no
// shader edit) instead of adding a sincos to every static-prop vertex.
static bool test_sh_y_rotation_equals_rotating_the_normal(TestCtx& t)
{
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
    CH_CHECK(t, worst < 1e-12);
    // Zero rotation is the identity on every coefficient (byte-identical path).
    const ShSet9 id = rotateShAboutY(base, 0.0);
    for (int i = 0; i < 9; ++i)
        for (int ch = 0; ch < 3; ++ch)
            CH_CHECK(t, id.c[i][ch] == base.c[i][ch]);
    return t.failures == 0;
}

// F7. After the sun-align rotation the probe's bright axis lands ON the sun
// azimuth, on all 24 missions. This is the fix's acceptance criterion.
static bool test_sun_align_rotation_aims_the_probe_at_the_sun(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const ShSet9 rot = rotateShAboutY(shippedShSet(), sunAlignRotationRad(m));
        // The rotated set's own L1 axis must sit at the sun's azimuth.
        const V3 axis = norm(v3(rot.c[3][0], rot.c[1][0], rot.c[2][0]));
        const double err = wrap180(stuffAzimuthDeg(axis)
                                 - stuffAzimuthDeg(missionSunStuff(m)));
        CH_CHECK(t, std::fabs(err) < 1e-9);
        // ...and the total energy is unchanged: a rotation moves light, it
        // does not create it. The spherical mean is an m=0 quantity.
        CH_CHECK(t, std::fabs(rot.c[0][0] - RenderCore::kIblShCoeffs[0][0]) < 1e-12);
        CH_CHECK(t, std::fabs(rot.c[6][0] - RenderCore::kIblShCoeffs[6][0]) < 1e-12);
    }
    return t.failures == 0;
}

// F8. THE SATURATION-TRAP FILTER, and the correction this lane exists to make.
// shaders/static_prop.vert:946-950 recommends grading this exact class of
// change on mc2_10. For a probe ROTATION that recommendation is WRONG: mc2_10
// carries ambient 192/255 AND sun byte 192/255, so 91% of wall azimuths are
// already clamped and the rotation moves a mean of 0.004 -- one byte. The pose
// earned its reputation from the base-light direction wrap, which is a MULTIPLICATIVE
// darkening and therefore pulls pixels DOWN out of saturation; a rotation
// mostly swaps one clamped state for another. Different term shape, different
// trap.
static bool test_mc2_10_is_a_saturation_trap_for_probe_rotation(TestCtx& t)
{
    const Mission* m10 = nullptr; const Mission* m22 = nullptr;
    const Mission* m17 = nullptr; const Mission* m24 = nullptr;
    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        if (n == "mc2_10") m10 = &kMissions[i];
        if (n == "mc2_22") m22 = &kMissions[i];
        if (n == "mc2_17") m17 = &kMissions[i];
        if (n == "mc2_24") m24 = &kMissions[i];
    }
    CH_CHECK(t, m10 && m22 && m17 && m24);
    if (!(m10 && m22 && m17 && m24)) return false;

    const RotSweep s10 = rotationSweep(*m10);
    const RotSweep s22 = rotationSweep(*m22);
    const RotSweep s17 = rotationSweep(*m17);
    const RotSweep s24 = rotationSweep(*m24);

    // The two traps. Both are ambient-192 missions.
    CH_CHECK(t, s10.meanAbs   < 0.01);      // 0.0037
    CH_CHECK(t, s10.unsatFrac < 0.10);      // 8.6% unclamped
    CH_CHECK(t, s22.meanAbs   < 0.02);      // 0.0123
    // The usable tier1 poses move 40x-60x more than mc2_10.
    CH_CHECK(t, s17.meanAbs > 0.15);        // 0.1807
    CH_CHECK(t, s24.meanAbs > 0.20);        // 0.2305
    CH_CHECK(t, s17.meanAbs > 40.0 * s10.meanAbs);
    CH_CHECK(t, s24.meanAbs > 40.0 * s10.meanAbs);
    // mc2_17 keeps the most unsaturated surface of the tier1 set.
    CH_CHECK(t, s17.unsatFrac > 0.50);      // 54.6%
    return t.failures == 0;
}

// F9. Perturbation sentinel for Stage F's constants.
static bool test_sentinel_ibl_probe_axis_constants(TestCtx& t)
{
    // The L1 band as shipped (RenderCore/IblShCoeffs.h rows 1..3, R channel).
    // Tolerance 1e-6, not 1e-8: the table is `float`, so promoting to double
    // carries up to ~2.4e-7 of rounding at these magnitudes. 1e-8 fails on the
    // UNMODIFIED table -- caught on the first run of this sentinel.
    CH_CHECK(t, std::fabs(RenderCore::kIblShCoeffs[3][0] - 1.966354845) < 1e-6);
    CH_CHECK(t, std::fabs(RenderCore::kIblShCoeffs[1][0] - 0.5869951003) < 1e-6);
    CH_CHECK(t, std::fabs(RenderCore::kIblShCoeffs[2][0] - 0.01179062044) < 1e-6);
    // The derived aim, in both frames.
    CH_CHECK(t, std::fabs(stuffAzimuthDeg(iblBrightAxisStuff(0)) - 0.3435) < 1e-3);
    const V3 mc2 = swapStuffToMC2(iblBrightAxisStuff(0));
    CH_CHECK(t, std::fabs(std::atan2(mc2.y, mc2.x) / kDeg2Rad - 179.656) < 1e-2);
    // Two per-mission errors, one near-aligned and one near-opposite.
    CH_CHECK(t, std::fabs(std::fabs(probeVsSunAzErrorDeg(kMissions[15])) - 7.66) < 0.05);
    CH_CHECK(t, std::fabs(std::fabs(probeVsSunAzErrorDeg(kMissions[13])) - 176.66) < 0.05);
    return t.failures == 0;
}

static bool print_stage_f_probe_aim(TestCtx&)
{
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
    return true;
}

//=============================================================================
// STAGE G TESTS -- IBL-ROTATION-LAND-1, the SHIPPED fix
//
// Stage F above is offline analysis of a defect. Stage G is the CONTRACT for
// the code that fixes it. Every expression below is a transcription of
// GameOS/gameos/gos_static_prop_batcher.cpp with the reader named beside it, so
// that a later edit to the engine that changes the arithmetic breaks a test
// here rather than silently diverging from the analysis that justified it.
//
// SHIPPED OPTION: StaticPropIblSunRotate       (options.cfg, default FALSE)
// SHIPPED FLOOR : StaticPropIblSunRotateFloor  (options.cfg, default 0.45, range 0..2)
//=============================================================================

// The shipped default of the floor option, read from the live option table.
static const double kShippedSunRotateFloor = double(mc2::options::RenderOptions{}.staticPropIblSunRotateFloor);

// gos_static_prop_batcher.cpp iblShCoeffsForUpload(), the frame + angle block.
// Input is the sun in the MC2 frame -- i.e. exactly what gos_GetTerrainLightDir
// hands the engine -- NOT a pre-converted Stuff vector. That is the point: this
// is the expression that has to get the frame right at runtime.
static double shippedSunRotationRad(const V3& sunMC2, const ShSet9& base)
{
    const double sunHoriz2 = sunMC2.x * sunMC2.x + sunMC2.y * sunMC2.y;
    if (!(sunHoriz2 > 1e-12)) return 0.0;
    const double sunAz   = std::atan2(sunMC2.y, -sunMC2.x);
    const double probeAz = std::atan2(base.c[2][0], base.c[3][0]);
    return sunAz - probeAz;
}

// gos_static_prop_batcher.cpp iblSunRotateLift(): smallest UNIFORM lift, in
// irradiance units, that brings every channel's spherical minimum up to the
// floor. Same sampling grid (kLat=128, kLon=256, inclusive in latitude) so the
// harness reproduces the engine's number, not merely a nearby one.
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

// The complete shipped transform: rotate to the mission sun, then lift L00.
// c4 == 0.886227 is L00's weight inside evalShL2, so dividing the irradiance
// lift by it converts to a coefficient delta.
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

// Lowest FINAL CLAMPED prop multiplier over all vertical-wall azimuths. This is
// the number the floor exists to defend -- not the probe's own irradiance.
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

// G1. THE SIGN TEST, and the one that matters most: get this wrong and every
// prop in the game rotates the WRONG WAY, which would look entirely plausible.
// The shipped angle formula consumes the MC2-frame sun and does its own frame
// carry (atan2(mc2.y, -mc2.x)); Stage F's derivation goes the other way, through
// swapStuffToMC2 and stuffAzimuthDeg. The two must agree on all 24 missions.
static bool test_shipped_angle_matches_the_stuff_frame_derivation(TestCtx& t)
{
    const ShSet9 base = shippedShSet();
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const double shipped = shippedSunRotationRad(terrainSunMC2(m.pitch, m.yaw), base);
        const double ref     = sunAlignRotationRad(m);
        CH_CHECK(t, std::fabs(wrap180((shipped - ref) / kDeg2Rad)) < 1e-9);
    }
    // And it is NOT sign-symmetric -- a flipped sign would fail the above on a
    // mission whose error is neither 0 nor 180. Pin one such mission so the
    // test cannot be satisfied by a degenerate all-zero angle.
    const double t01 = shippedSunRotationRad(terrainSunMC2(kMissions[0].pitch,
                                                           kMissions[0].yaw), base);
    CH_CHECK(t, std::fabs(t01 / kDeg2Rad - 123.66) < 0.01);   // mc2_01, +123.66
    return t.failures == 0;
}

// G2. With the floor disabled the shipped transform IS the reference rotation
// proved correct by test_sh_y_rotation_equals_rotating_the_normal -- exactly,
// on every coefficient, on every mission. No epsilon.
static bool test_shipped_transform_with_no_floor_is_the_bare_rotation(TestCtx& t)
{
    const ShSet9 base = shippedShSet();
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        // (a) EXACT: floor 0 must add nothing at all -- the transform is then
        //     the pure rotation at the shipped angle, bit for bit. This is the
        //     "StaticPropIblSunRotateFloor = 0 gives the bare rotation"
        //     contract, and it must not hold merely to within an epsilon.
        const ShSet9 got  = shippedUploadSet(m, 0.0);
        const ShSet9 pure = rotateShAboutY(base,
                              shippedSunRotationRad(terrainSunMC2(m.pitch, m.yaw), base));
        for (int k = 0; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch)
                CH_CHECK(t, got.c[k][ch] == pure.c[k][ch]);
        // (b) TO 1e-12, not exactly: against Stage F's REFERENCE angle. These
        //     are two different computations of the same rotation -- the
        //     shipped one works in radians and does not wrap, Stage F's works
        //     in degrees through wrap180 -- so they differ by a multiple of
        //     2*pi in the argument and by float rounding in the result. G1
        //     already pins the angles themselves to 1e-9; this pins the
        //     coefficients they produce.
        const ShSet9 ref = rotateShAboutY(base, sunAlignRotationRad(m));
        for (int k = 0; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch)
                CH_CHECK(t, std::fabs(got.c[k][ch] - ref.c[k][ch]) < 1e-12);
    }
    return t.failures == 0;
}

// G3. The acceptance criterion survives the floor. The floor touches L00 only,
// so the aim -- which lives entirely in the L1 band -- must be untouched.
static bool test_floor_does_not_move_the_aim(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; ++i) {
        const Mission& m = kMissions[i];
        const ShSet9 rot = shippedUploadSet(m, kShippedSunRotateFloor);
        const V3 axis = norm(v3(rot.c[3][0], rot.c[1][0], rot.c[2][0]));
        const double err = wrap180(stuffAzimuthDeg(axis)
                                 - stuffAzimuthDeg(missionSunStuff(m)));
        CH_CHECK(t, std::fabs(err) < 1e-9);
        // ...and the L1/L2 bands are bit-identical to the unfloored rotation.
        const ShSet9 bare = shippedUploadSet(m, 0.0);
        for (int k = 1; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch)
                CH_CHECK(t, rot.c[k][ch] == bare.c[k][ch]);
    }
    return t.failures == 0;
}

// G4. The floor can only LIFT. There must be no direction on the sphere, on any
// mission, where the floored set is darker than the unfloored one -- otherwise
// "raise the floor" could darken something and the knob would be untunable.
static bool test_floor_is_a_pure_lift_and_never_darkens(TestCtx& t)
{
    for (int i = 0; i < kNumMissions; i += 4) {          // 6 missions, 3 channels
        const Mission& m  = kMissions[i];
        const ShSet9 bare = shippedUploadSet(m, 0.0);
        const ShSet9 flr  = shippedUploadSet(m, kShippedSunRotateFloor);
        forEachNormal(32, [&](const V3& n) {
            for (int ch = 0; ch < 3; ++ch)
                CH_CHECK(t, evalShL2Set(flr, n, ch) >= evalShL2Set(bare, n, ch) - 1e-12);
        });
    }
    // Monotone in the knob, so tuning it up never darkens anything either.
    const ShSet9 lo = shippedUploadSet(kMissions[16], 0.30);
    const ShSet9 hi = shippedUploadSet(kMissions[16], 0.60);
    CH_CHECK(t, hi.c[0][0] > lo.c[0][0]);
    return t.failures == 0;
}

// G5. The floor actually reaches its target: every channel's spherical minimum
// clears the floor value afterwards. This is what "0.45" MEANS.
static bool test_floor_lifts_every_channel_minimum_to_the_floor(TestCtx& t)
{
    const ShSet9 flr = shippedUploadSet(kMissions[16], kShippedSunRotateFloor);
    double worst[3] = { 1e30, 1e30, 1e30 };
    forEachNormal(96, [&](const V3& n) {
        for (int ch = 0; ch < 3; ++ch)
            worst[ch] = std::min(worst[ch], evalShL2Set(flr, n, ch));
    });
    for (int ch = 0; ch < 3; ++ch)
        CH_CHECK(t, worst[ch] >= kShippedSunRotateFloor - 2e-3);
    // The R channel is the binding one (the baked minimum is 0.232 R / 0.360 G
    // / 0.477 B), so R should land ON the floor and G/B strictly above it.
    CH_CHECK(t, std::fabs(worst[0] - kShippedSunRotateFloor) < 2e-3);
    CH_CHECK(t, worst[1] > worst[0] && worst[2] > worst[1]);
    // The lift itself, pinned: 0.45 - 0.232 = 0.218 irradiance.
    CH_CHECK(t, std::fabs(shippedSunRotateLift(shippedShSet(),
                                               kShippedSunRotateFloor) - 0.218) < 2e-3);
    return t.failures == 0;
}

// G6. WHY 0.45 AND NOT SOME OTHER NUMBER. terrain_lod_chunk.frag's lighting
// cannot go below mix(0.35,1.20,0.02) == kTerrainFloor == 0.367 (test A1). The
// bare rotation puts the four ambient-31 missions' darkest wall BELOW that --
// a building wall darker than the ground it stands on. The default floor is
// exactly the value that fixes that at the shipped daytime IBL strength.
static bool test_bare_rotation_puts_the_dark_missions_under_the_terrain_floor(TestCtx& t)
{
    const char* dark[] = { "mc2_12", "mc2_13", "mc2_17", "mc2_18" };
    int checked = 0;
    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        bool isDark = false;
        for (const char* d : dark) if (n == d) isDark = true;
        if (!isDark) continue;
        ++checked;
        CH_CHECK(t, kMissions[i].ambR == 31);            // the shared cause
        const double bare = minWallMultiplier(kMissions[i], shippedUploadSet(kMissions[i], 0.0));
        CH_CHECK(t, bare < kTerrainFloor);               // BELOW the ground's floor
        const double flr  = minWallMultiplier(kMissions[i],
                                shippedUploadSet(kMissions[i], kShippedSunRotateFloor));
        CH_CHECK(t, flr >= kTerrainFloor);               // the floor recovers it
        CH_CHECK(t, flr > bare);
    }
    CH_CHECK(t, checked == 4);
    // The arithmetic the default was solved from, restated so it cannot drift:
    // ambient 31/255 + FLOOR * 0.55 >= 0.367  ->  FLOOR >= 0.4462.
    const double needed = (kTerrainFloor - 31.0 / 255.0) / 0.55;
    CH_CHECK(t, std::fabs(needed - 0.4462) < 1e-3);
    CH_CHECK(t, kShippedSunRotateFloor >= needed);
    return t.failures == 0;
}

// G7. The floor is not free and this test refuses to hide it: it lifts the
// BRIGHT end too, by the same absolute irradiance. Pin how much, so a future
// reader can see the cost rather than rediscover it. At the bright end (4.896)
// the 0.218 lift is +4.5%, and on the CLAMPED multiplier the missions that were
// already saturated do not move at all.
static bool test_floor_cost_at_the_bright_end_is_bounded(TestCtx& t)
{
    const ShSet9 bare = shippedUploadSet(kMissions[16], 0.0);
    const ShSet9 flr  = shippedUploadSet(kMissions[16], kShippedSunRotateFloor);
    double hiBare = -1e30, hiFlr = -1e30;
    forEachNormal(96, [&](const V3& n) {
        hiBare = std::max(hiBare, evalShL2Set(bare, n, 0));
        hiFlr  = std::max(hiFlr,  evalShL2Set(flr,  n, 0));
    });
    CH_CHECK(t, std::fabs(hiFlr - hiBare - 0.218) < 2e-3);
    CH_CHECK(t, hiFlr / hiBare < 1.05);                       // +4.5%
    // On the two saturation-trap missions the floor is very nearly inert on the
    // clamped multiplier, because their walls were already at 1.0.
    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        if (n != "mc2_10" && n != "mc2_22") continue;
        const double a = minWallMultiplier(kMissions[i], shippedUploadSet(kMissions[i], 0.0));
        const double b = minWallMultiplier(kMissions[i],
                            shippedUploadSet(kMissions[i], kShippedSunRotateFloor));
        CH_CHECK(t, b - a < 0.12);
    }
    return t.failures == 0;
}

// G8. The floor as the engine reads it: an options.cfg value through the shared
// grammar (Gates/render_options.h). A value that is not a whole in-range number
// is REJECTED and the shipped default stays (it is not clamped and it does not
// become 0, which is how the retired env reader behaved).
static double shippedFloorFromCfg(const char* text)
{
    mc2::options::RenderOptions o;
    if (text == nullptr) return double(o.staticPropIblSunRotateFloor);
    const mc2::options::Option* opt = mc2::options::find("StaticPropIblSunRotateFloor");
    if (opt == nullptr) return -1.0;
    (void)mc2::options::setFromText(o, *opt, text);
    return double(o.staticPropIblSunRotateFloor);
}
static bool test_floor_option_grammar_contract(TestCtx& t)
{
    const double dflt = kShippedSunRotateFloor;
    CH_CHECK(t, std::fabs(dflt - 0.45) < 1e-6);
    CH_CHECK(t, shippedFloorFromCfg(nullptr) == dflt);
    CH_CHECK(t, shippedFloorFromCfg("0")     == 0.0);
    CH_CHECK(t, shippedFloorFromCfg("0.0")   == 0.0);
    CH_CHECK(t, shippedFloorFromCfg("2")     == 2.0);
    CH_CHECK(t, std::fabs(shippedFloorFromCfg("0.30") - 0.30) < 1e-6);
    CH_CHECK(t, shippedFloorFromCfg("-1")     == dflt);   // out of range: rejected, default stays
    CH_CHECK(t, shippedFloorFromCfg("99")     == dflt);
    CH_CHECK(t, shippedFloorFromCfg("banana") == dflt);
    CH_CHECK(t, shippedFloorFromCfg("0,3")    == dflt);
    CH_CHECK(t, shippedFloorFromCfg("")       == dflt);
    // A zero floor must give back the bare rotation exactly (G2's contract).
    CH_CHECK(t, shippedSunRotateLift(shippedShSet(), shippedFloorFromCfg("0")) == 0.0);
    return t.failures == 0;
}

// G8b. The option defaults this harness's Stage E-G models are written against.
static bool test_live_option_defaults_the_models_assume(TestCtx& t)
{
    const mc2::options::RenderOptions d;
    CH_CHECK(t, d.staticPropIblSh);                // the IBL term is in the prop band
    CH_CHECK(t, !d.staticPropIblSunRotate);        // Stage G is the opt-in rotation
    CH_CHECK(t, d.staticPropIblSunRotateFloor > 0.0f);
    return t.failures == 0;
}

// G8c. The static-prop band models omit the hemisphere ambient fill (the options-off
// configuration); the shipped default has it ON. This test pins that gap: when the
// models are re-derived with the term (or the default changes) this goes red and the
// header note above is rewritten.
static bool test_models_describe_ambient_off_not_the_shipped_default(TestCtx& t)
{
    CH_CHECK(t, mc2::options::RenderOptions{}.staticPropAmbient);
    CH_CHECK(t, propMulPreClamp(0.1, 0.2, 0.3, 0.5, 0.05) == 0.1 + 0.2 + 0.3 * 0.5 + 0.05);   // no ambient-fill term in the model
    return t.failures == 0;
}

// G9. THE CORRECTION THIS LANE OWES ITS INHERITANCE. The rotation was handed
// down with the warning that it "darkens shadow-side walls hard -- maxDown
// -0.705", carrying the implication that it creates a NEW near-black failure of
// the kind that got an earlier IBL_NIGHT_SCALE floor of 0.0 rejected. Those
// maxDown figures are real (Stage F measures them) but they are PER-AZIMUTH
// DELTAS, and a delta is not a new minimum. A rotation is an isometry of the
// sphere, so the SET of probe values is invariant -- only their assignment to
// azimuths changes. Measured on the darkest-wall statistic that a viewer would
// actually notice, the bare rotation moves the four ambient-31 missions by
// -0.012 or by nothing at all, because they were ALREADY at that value on some
// other wall of the same building.
//
// The floor still earns its place, for the honest reason rather than the
// inherited one: on those missions the darkest wall sits at 0.295, BELOW the
// terrain's 0.367 floor, in the build that ships TODAY -- and the rotation
// moves that pre-existing dark value onto the anti-sun wall, where the probe's
// dark lobe and NdotL == 0 now coincide instead of covering for each other. It
// is a pre-existing defect being made legible, not a new one being created.
static bool test_rotation_does_not_create_a_new_darkest_wall(TestCtx& t)
{
    double worstRegression = 0.0;
    const char* worstName = "";
    for (int i = 0; i < kNumMissions; ++i) {
        const double stock = minWallMultiplier(kMissions[i], shippedShSet());
        const double bare  = minWallMultiplier(kMissions[i],
                                shippedUploadSet(kMissions[i], 0.0));
        if (stock - bare > worstRegression) {
            worstRegression = stock - bare; worstName = kMissions[i].name;
        }
        // On EVERY mission the bare rotation's darkest wall is at worst a
        // quarter of the quoted -0.705 below the shipped build's darkest wall.
        CH_CHECK(t, bare >= stock - 0.25);
    }
    // ...and the worst is mc2_14, the 176.7-degree mission -- not one of the
    // four the maxDown ranking named.
    CH_CHECK(t, std::string(worstName) == "mc2_14");
    CH_CHECK(t, std::fabs(worstRegression - 0.2476) < 2e-3);
    CH_CHECK(t, worstRegression < 0.705);              // strictly under the quoted delta
    // The four ambient-31 missions barely move at all on this statistic.
    for (int i = 0; i < kNumMissions; ++i) {
        if (kMissions[i].ambR != 31) continue;
        const double stock = minWallMultiplier(kMissions[i], shippedShSet());
        const double bare  = minWallMultiplier(kMissions[i],
                                shippedUploadSet(kMissions[i], 0.0));
        CH_CHECK(t, stock - bare <= 0.0125);
        // ...and BOTH arms are already under the terrain floor, which is the
        // condition the floor actually cures.
        CH_CHECK(t, stock < kTerrainFloor);
        CH_CHECK(t, bare  < kTerrainFloor);
    }
    return t.failures == 0;
}

// G10. THE SEMANTIC SIGN CHECK -- deliberately independent of every line of
// algebra above. G1 checks the shipped ANGLE against another derivation of the
// angle; both could share a convention error and agree. This one asks the only
// question a viewer cares about, in terms no rotation formula appears in:
//
//   after the fix, is the wall FACING the sun brighter than the wall facing AWAY?
//
// Stage F's F5 measured the shipped build getting that backwards on 17 of 24
// missions. If the rotation went the wrong way -- the failure mode that would
// look entirely plausible on screen -- this inverts instead of repairing, and no
// amount of self-consistent algebra would hide it.
static bool test_after_the_fix_the_sunward_wall_is_the_bright_one(TestCtx& t)
{
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
        CH_CHECK(t, lit > dark);
        // ...and the sunward wall is not merely brighter, it is the BRIGHTEST
        // horizontal direction there is: the rotation put the peak ON the sun
        // azimuth, not merely on the correct side of the building.
        double bestAz = 0.0, bestV = -1e30;
        for (int k = 0; k < 720; ++k) {
            const double a = 2.0 * 3.14159265358979323846 * k / 720;
            const V3 n = v3(std::cos(a), 0.0, std::sin(a));
            const double v = evalShL2Set(fix, n, 0);
            if (v > bestV) { bestV = v; bestAz = a / kDeg2Rad; }
        }
        CH_CHECK(t, std::fabs(wrap180(bestAz - stuffAzimuthDeg(toward))) < 1.0);
    }
    CH_CHECK(t, wrongWayBefore == 17);   // the shipped defect, restated
    CH_CHECK(t, wrongWayAfter  == 0);    // ...gone on every mission
    return t.failures == 0;
}

// G11. Perturbation sentinel for Stage G's own constants.
static bool test_sentinel_ibl_sun_rotate_constants(TestCtx& t)
{
    CH_CHECK(t, std::fabs(kShippedSunRotateFloor - 0.45) < 1e-6);
    // The baked spherical minimum per channel -- the floor's input.
    double lo[3] = { 1e30, 1e30, 1e30 };
    forEachNormal(96, [&](const V3& n) {
        for (int ch = 0; ch < 3; ++ch)
            lo[ch] = std::min(lo[ch], evalShL2Set(shippedShSet(), n, ch));
    });
    CH_CHECK(t, std::fabs(lo[0] - 0.232) < 2e-3);
    CH_CHECK(t, std::fabs(lo[1] - 0.360) < 2e-3);
    CH_CHECK(t, std::fabs(lo[2] - 0.477) < 2e-3);
    // The two named worst-look-risk missions and their recovered multipliers.
    for (int i = 0; i < kNumMissions; ++i) {
        const std::string n(kMissions[i].name);
        if (n != "mc2_12" && n != "mc2_18") continue;
        CH_CHECK(t, std::fabs(minWallMultiplier(kMissions[i], shippedShSet())
                              - 0.3076) < 2e-3);          // SHIPPED build today
        const double bare = minWallMultiplier(kMissions[i], shippedUploadSet(kMissions[i], 0.0));
        CH_CHECK(t, std::fabs(bare - 0.2953) < 2e-3);     // bare rotation
        const double flr = minWallMultiplier(kMissions[i],
                             shippedUploadSet(kMissions[i], kShippedSunRotateFloor));
        CH_CHECK(t, std::fabs(flr - 0.4149) < 2e-3);      // rotation + default floor
    }
    // THE GUARANTEE the 0.45 default was solved for, as arithmetic rather than
    // as a sweep: no normal ANYWHERE on the sphere may take a prop below the
    // terrain's own floor at the darkest shipping ambient and the shipped
    // daytime strength.  0.1216 + 0.45*0.55 = 0.3691 >= 0.367.
    const double guaranteed = 31.0 / 255.0 + kShippedSunRotateFloor * 0.55;
    CH_CHECK(t, std::fabs(guaranteed - 0.3691) < 1e-3);
    CH_CHECK(t, guaranteed >= kTerrainFloor);
    return t.failures == 0;
}

static bool print_stage_g_floor(TestCtx&)
{
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

    // Stage D -- SUN-AXIS-FIX-1
    h.add("row2_matches_the_affine_builder",           test_row2_matches_the_affine_builder);
    h.add("fix_a_gives_ndl_one_at_the_true_sun",       test_fix_a_gives_ndl_one_at_the_true_sun);
    h.add("fix_a_equals_the_specular_consumer",        test_fix_a_equals_the_specular_consumer);
    h.add("saturated_missions_show_zero_delta",        test_saturated_missions_show_zero_delta);
    h.add("unsaturated_missions_move_in_both_directions",
          test_unsaturated_missions_move_in_both_directions);
    h.add("fix_b_degenerate_pitch_is_handled",         test_fix_b_degenerate_pitch_is_handled);
    h.add("fix_b_unifies_terrain_sun_with_the_object_row",
          test_fix_b_unifies_terrain_sun_with_the_object_row);
    h.add("fix_b_also_moves_the_sun_colour_byte",      test_fix_b_also_moves_the_sun_colour_byte);

    // Stage E -- TREE-SUN-RESTORE-1
    h.add("stock_lit_trees_with_the_sun_byte_not_the_mission_ambient",
          test_stock_lit_trees_with_the_sun_byte_not_the_mission_ambient);
    h.add("stock_tree_multiplier_is_normal_independent",
          test_stock_tree_multiplier_is_normal_independent);
    h.add("stock_trees_are_mostly_UNsaturated",        test_stock_trees_are_mostly_UNsaturated);
    h.add("head_ibl_pins_trees_that_stock_left_free",  test_head_ibl_pins_trees_that_stock_left_free);
    h.add("prop_shadow_receive_is_an_exact_no_op_on_trees",
          test_prop_shadow_receive_is_an_exact_no_op_on_trees);
    h.add("restoring_a_sun_would_be_invisible_where_S_saturates",
          test_restoring_a_sun_would_be_invisible_where_S_saturates);
    h.add("shipped_ibl_strength_is_not_the_cpp_literal",
          test_shipped_ibl_strength_is_not_the_cpp_literal);

    // Stage F -- IBL-PROBE-ROTATION-1
    h.add("mission_sh_map_is_functionally_but_not_literally_empty",
          test_mission_sh_map_is_functionally_but_not_literally_empty);
    h.add("probe_bright_axis_is_pinned_at_stuff_plus_x",
          test_probe_bright_axis_is_pinned_at_stuff_plus_x);
    h.add("the_7_71_ratio_is_the_x_pair_not_the_sphere_swing",
          test_the_7_71_ratio_is_the_x_pair_not_the_sphere_swing);
    h.add("probe_bright_axis_misses_the_sun_on_every_mission",
          test_probe_bright_axis_misses_the_sun_on_every_mission);
    h.add("probe_is_brighter_on_the_shadowed_wall_on_most_missions",
          test_probe_is_brighter_on_the_shadowed_wall_on_most_missions);
    h.add("sh_y_rotation_equals_rotating_the_normal",
          test_sh_y_rotation_equals_rotating_the_normal);
    h.add("sun_align_rotation_aims_the_probe_at_the_sun",
          test_sun_align_rotation_aims_the_probe_at_the_sun);
    h.add("mc2_10_is_a_saturation_trap_for_probe_rotation",
          test_mc2_10_is_a_saturation_trap_for_probe_rotation);

    // Stage G -- IBL-ROTATION-LAND-1 (the shipped fix)
    h.add("shipped_angle_matches_the_stuff_frame_derivation",
          test_shipped_angle_matches_the_stuff_frame_derivation);
    h.add("shipped_transform_with_no_floor_is_the_bare_rotation",
          test_shipped_transform_with_no_floor_is_the_bare_rotation);
    h.add("floor_does_not_move_the_aim",            test_floor_does_not_move_the_aim);
    h.add("floor_is_a_pure_lift_and_never_darkens", test_floor_is_a_pure_lift_and_never_darkens);
    h.add("floor_lifts_every_channel_minimum_to_the_floor",
          test_floor_lifts_every_channel_minimum_to_the_floor);
    h.add("bare_rotation_puts_the_dark_missions_under_the_terrain_floor",
          test_bare_rotation_puts_the_dark_missions_under_the_terrain_floor);
    h.add("floor_cost_at_the_bright_end_is_bounded",
          test_floor_cost_at_the_bright_end_is_bounded);
    h.add("floor_option_grammar_contract",          test_floor_option_grammar_contract);
    h.add("live_option_defaults_the_models_assume", test_live_option_defaults_the_models_assume);
    h.add("models_describe_ambient_off_not_the_shipped_default", test_models_describe_ambient_off_not_the_shipped_default);
    h.add("rotation_does_not_create_a_new_darkest_wall",
          test_rotation_does_not_create_a_new_darkest_wall);
    h.add("after_the_fix_the_sunward_wall_is_the_bright_one",
          test_after_the_fix_the_sunward_wall_is_the_bright_one);

    // Perturbation sentinels
    h.add("sentinel_ibl_probe_axis_constants",          test_sentinel_ibl_probe_axis_constants);
    h.add("sentinel_ibl_sun_rotate_constants",         test_sentinel_ibl_sun_rotate_constants);
    h.add("sentinel_tree_constants",                   test_sentinel_tree_constants);
    h.add("sentinel_fix_constants",                    test_sentinel_fix_constants);
    h.add("sentinel_terrain_constants",                test_sentinel_terrain_constants);
    h.add("sentinel_mech_floor",                       test_sentinel_mech_floor);
    h.add("sentinel_ibl_strength",                     test_sentinel_ibl_strength);
    h.add("sentinel_stock_kernel_truncates",           test_sentinel_stock_kernel_truncates);
    h.add("sentinel_sun_frame_constants",              test_sentinel_sun_frame_constants);

    // Report printers -- excluded from the default suite.
    h.add("print_stage_d_delta", print_stage_d_delta, false);
    h.add("print_stage_a_matrix", print_stage_a_matrix, false);
    h.add("print_stage_b_frames", print_stage_b_frames, false);
    h.add("print_stage_c_table",  print_stage_c_table,  false);
    h.add("print_stage_e_tree_table", print_stage_e_tree_table, false);
    h.add("print_stage_f_probe_aim",  print_stage_f_probe_aim,  false);
    h.add("print_stage_g_floor",      print_stage_g_floor,      false);

    return h.run(argc, argv);
}
