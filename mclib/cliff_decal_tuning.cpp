// TERRAIN-DECAL-SLICE-0C — live cliff mesh-decal placement tuning (impl).
// See cliff_decal_tuning.h. The face-frame math here is the exact factoring of the
// Slice-0A/0B block formerly inlined in mclib/bdactor.cpp BldgAppearance::registerStatic.

#include "cliff_decal_tuning.h"

#include "Stuff/Stuff.hpp"
#include "gos_static_prop_registry.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace CliffDecalTuning {

namespace {

// Captured live-update context for the single active CLIFF_WALL decal.
int32_t          s_recipeIndex = -1;
Stuff::Vector3D  s_xlatPosition;   // shape-world placement origin
Stuff::Vector3D  s_nAcc;           // accumulated world-axis terrain normal

Knobs s_knobs;
bool  s_knobsSeeded = false;

float envFloat(const char* name, float dflt) {
    const char* v = std::getenv(name);
    return v ? (float)std::atof(v) : dflt;
}

void seedKnobsFromEnv() {
    if (s_knobsSeeded) return;
    s_knobsSeeded = true;
    s_knobs.scale   = envFloat("MC2_TERRAIN_DECAL_SCALE",   1.0f);
    s_knobs.offset  = envFloat("MC2_TERRAIN_DECAL_OFFSET",  8.0f);
    s_knobs.lateral = envFloat("MC2_TERRAIN_DECAL_LATERAL", 0.0f);
    s_knobs.lift    = envFloat("MC2_TERRAIN_DECAL_LIFT",    0.0f);
    s_knobs.yawDeg  = envFloat("MC2_TERRAIN_DECAL_YAW",     0.0f);
    s_knobs.pitchDeg = envFloat("MC2_TERRAIN_DECAL_PITCH",  0.0f);
}

} // namespace

Knobs& knobs() {
    seedKnobsFromEnv();
    return s_knobs;
}

void buildCliffWallMatrix(const Stuff::Vector3D& xlatPosition,
                          const Stuff::Vector3D& nAcc,
                          const Knobs& k,
                          Stuff::Matrix4D& out) {
    // Outward facing = horizontal projection of the terrain normal, remapped to
    // shape-world axes (-nx, 0, ny). Points downslope toward the low side.
    Stuff::Vector3D facing;
    facing.x = -nAcc.x; facing.y = 0.0f; facing.z = nAcc.y;
    float fl = std::sqrt(facing.x * facing.x + facing.z * facing.z);
    if (fl < 1e-4f) { facing.x = 0.0f; facing.z = 1.0f; fl = 1.0f; } // flat: pick +Zsw
    facing.x /= fl; facing.z /= fl;
    Stuff::Vector3D up(0.0f, 1.0f, 0.0f);
    // tangent (contour) = up x facing (right-handed), horizontal.
    Stuff::Vector3D tangent;
    tangent.x = up.y * facing.z - up.z * facing.y; // = facing.z
    tangent.y = up.z * facing.x - up.x * facing.z; // = 0
    tangent.z = up.x * facing.y - up.y * facing.x; // = -facing.x
    float tl = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
    if (tl < 1e-4f) tl = 1.0f;
    tangent.x /= tl; tangent.y /= tl; tangent.z /= tl;

    // YAW: rotate facing & tangent about world-up by k.yawDeg (both lie in the
    // horizontal plane; up is invariant).
    if (k.yawDeg != 0.0f) {
        const float yr = k.yawDeg * 0.01745329252f; // deg -> rad
        const float cy = std::cos(yr), sy = std::sin(yr);
        // Rotate about world-up (shape-world +Y): x' = x*cy + z*sy, z' = -x*sy + z*cy.
        float fx = facing.x * cy + facing.z * sy;
        float fz = -facing.x * sy + facing.z * cy;
        facing.x = fx; facing.z = fz;
        float tx = tangent.x * cy + tangent.z * sy;
        float tz = -tangent.x * sy + tangent.z * cy;
        tangent.x = tx; tangent.z = tz;
    }

    // PITCH: lean the wall backward (top toward the hill) by rotating the {up,facing}
    // basis about the horizontal `tangent` (contour) axis via Rodrigues. tangent is the
    // fixed axis; tangent x up = facing, tangent x facing = -up. Positive pitch tips the
    // TOP backward (away from `facing`, toward the hill) and the face upward, so we use
    // -pitch as the rotation angle: up' gains -facing, facing' gains +up.
    if (k.pitchDeg != 0.0f) {
        const float pr = -k.pitchDeg * 0.01745329252f; // deg -> rad, negated so + leans back
        const float cp = std::cos(pr), sp = std::sin(pr);
        // up'    = up*cp + (tangent x up)*sp    = up*cp + facing*sp
        // facing'= facing*cp + (tangent x facing)*sp = facing*cp - up*sp
        Stuff::Vector3D newUp, newFacing;
        newUp.x = up.x * cp + facing.x * sp;
        newUp.y = up.y * cp + facing.y * sp;
        newUp.z = up.z * cp + facing.z * sp;
        newFacing.x = facing.x * cp - up.x * sp;
        newFacing.y = facing.y * cp - up.y * sp;
        newFacing.z = facing.z * cp - up.z * sp;
        up = newUp;
        facing = newFacing;
    }

    // SCALE: uniform scale of the wall mesh. Multiply the 3 basis rows by k.scale
    // before translation. Uniform only -> no inverse-transpose needed for normals.
    const float sc = k.scale;
    out.BuildIdentity();
    // ROW 0 = local X (wall width / contour) -> tangent
    out(Stuff::X_Axis, Stuff::X_Axis) = tangent.x * sc;
    out(Stuff::X_Axis, Stuff::Y_Axis) = tangent.y * sc;
    out(Stuff::X_Axis, Stuff::Z_Axis) = tangent.z * sc;
    // ROW 1 = local Y (wall height) -> world up
    out(Stuff::Y_Axis, Stuff::X_Axis) = up.x * sc;
    out(Stuff::Y_Axis, Stuff::Y_Axis) = up.y * sc;
    out(Stuff::Y_Axis, Stuff::Z_Axis) = up.z * sc;
    // ROW 2 = local Z (relief / face) -> outward facing
    out(Stuff::Z_Axis, Stuff::X_Axis) = facing.x * sc;
    out(Stuff::Z_Axis, Stuff::Y_Axis) = facing.y * sc;
    out(Stuff::Z_Axis, Stuff::Z_Axis) = facing.z * sc;
    // Translation = placement origin + OUTWARD offset (facing) + LATERAL (tangent)
    // + LIFT (world up). Knob-tuned; defaults reproduce Slice 0A.
    const float kOutwardOffset = k.offset;
    out(Stuff::W_Axis, Stuff::X_Axis) =
        xlatPosition.x + facing.x * kOutwardOffset + tangent.x * k.lateral + up.x * k.lift;
    out(Stuff::W_Axis, Stuff::Y_Axis) =
        xlatPosition.y + facing.y * kOutwardOffset + tangent.y * k.lateral + up.y * k.lift;
    out(Stuff::W_Axis, Stuff::Z_Axis) =
        xlatPosition.z + facing.z * kOutwardOffset + tangent.z * k.lateral + up.z * k.lift;
}

void captureDecalContext(int32_t recipeIndex,
                         const Stuff::Vector3D& xlatPosition,
                         const Stuff::Vector3D& nAcc) {
    seedKnobsFromEnv();
    s_recipeIndex  = recipeIndex;
    s_xlatPosition = xlatPosition;
    s_nAcc         = nAcc;
}

void clearDecalContext() {
    s_recipeIndex = -1;
}

bool cliffDecal_hasDecal() {
    return s_recipeIndex >= 0;
}

void cliffDecal_getKnobs(float* scale, float* offset, float* lateral,
                         float* lift, float* yawDeg, float* pitchDeg) {
    seedKnobsFromEnv();
    if (scale)    *scale    = s_knobs.scale;
    if (offset)   *offset   = s_knobs.offset;
    if (lateral)  *lateral  = s_knobs.lateral;
    if (lift)     *lift     = s_knobs.lift;
    if (yawDeg)   *yawDeg   = s_knobs.yawDeg;
    if (pitchDeg) *pitchDeg = s_knobs.pitchDeg;
}

void cliffDecal_setKnobsAndApply(float scale, float offset, float lateral,
                                 float lift, float yawDeg, float pitchDeg) {
    seedKnobsFromEnv();
    s_knobs.scale    = scale;
    s_knobs.offset   = offset;
    s_knobs.lateral  = lateral;
    s_knobs.lift     = lift;
    s_knobs.yawDeg   = yawDeg;
    s_knobs.pitchDeg = pitchDeg;
    if (s_recipeIndex < 0) return;
    Stuff::Matrix4D m;
    buildCliffWallMatrix(s_xlatPosition, s_nAcc, s_knobs, m);
    // buildRecipeFromShape fills GpuStaticPropInstance.modelMatrix by raw memcpy of
    // Matrix4D's 16-float `entries` array (gos_static_prop_batcher.cpp), so the live
    // update must hand the registry the SAME raw entries layout. Matrix4D exposes a
    // `const Scalar*` conversion returning &entries[0]; copy those 16 floats verbatim.
    const float* entries = static_cast<const float*>(m);
    GpuStaticPropRegistry::staticPropSetAllLeafMatrices(s_recipeIndex, entries);
}

void cliffDecal_logValues() {
    seedKnobsFromEnv();
    std::fprintf(stderr,
        "[TERRAIN_DECAL v1] tuned knobs: "
        "MC2_TERRAIN_DECAL_SCALE=%.4f MC2_TERRAIN_DECAL_OFFSET=%.2f "
        "MC2_TERRAIN_DECAL_LATERAL=%.2f MC2_TERRAIN_DECAL_LIFT=%.2f "
        "MC2_TERRAIN_DECAL_YAW=%.2f MC2_TERRAIN_DECAL_PITCH=%.2f\n",
        s_knobs.scale, s_knobs.offset, s_knobs.lateral, s_knobs.lift, s_knobs.yawDeg,
        s_knobs.pitchDeg);
    std::fflush(stderr);
}

} // namespace CliffDecalTuning

// Cross-TU (C-linkage) mission-reset hook, called from
// GpuStaticPropRegistry::init() to drop a stale decal capture.
extern "C" void CliffDecalTuning_clearOnMissionReset() {
    CliffDecalTuning::clearDecalContext();
}
