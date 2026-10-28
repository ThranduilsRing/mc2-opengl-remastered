// CPU static-prop geometry/material ownership, persistent instance tables and IBL tuning.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "../../mclib/texture_assets.h"
#include "Gates/render_options.h"

namespace gos_static_prop_batcher_detail {

constexpr uint32_t STATIC_PROP_GLOBAL_CAP_DEFAULT = 131072u;

// Immutable per-map geometry.
uint64_t s_sharedVboBytes = 0;
uint64_t s_sharedIboBytes = 0;

uint32_t s_frameSlot = 0;
size_t   s_instanceCapacity = 0;
size_t   s_colorCapacity    = 0;

std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;

// 2b Stage 2 (Mechanism B-reinject): persistent per-type static instance store,
// rebuilt only when the registry generation changes. Each frame the registry
// flush BULK-injects these blocks into s_bucketsByType (one memcpy per type)
// instead of re-walking the registry per leaf — killing the per-leaf walk/push/
// map overhead. Downstream (uploadAllBucketsIfNeeded + coalesce pool build) read
// s_bucketsByType unchanged.
std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> s_persistentStaticStore;
uint64_t s_persistentStaticGen = 0xFFFFFFFFFFFFFFFFull;  // sentinel = never built

// Stage 3.C: per-submitMultiShape batch accumulator. Cleared at the start
// of each submitMultiShape(); populated by submit() per leaf. After
// submitMultiShape() returns true, getLastBuiltBatch() returns this vector
// for snapshot registration in GpuStaticPropRegistry.
std::vector<GpuStaticPropInstance> s_lastBuiltBatch;

// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<uint8_t>                                s_packetAlphaBaked;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<TG_TypeNodeId, uint32_t>  s_typeIndex;
std::unordered_map<uint32_t, PropLodGroup>   s_lodGroups;
std::unordered_map<uint64_t, uint8_t>        s_lodState;
std::vector<RenderCore::StaticPropTypeDesc>        s_typeDescTable; // v0: cross-seam mirror

// [GPUPROPS v1] setup-path instrumentation (internal; path tag lives at file scope).
bool s_gpuPropsTrace = mc2::gate::get<"MC2_GPUPROPS_TRACE">();
// g_regCall counts EVERY registerType invocation after the null guard and
// BEFORE the idempotent s_typeIndex.count early-return, so 0 for a path means
// registerType was genuinely never invoked on it.
unsigned g_regCall[4]     = {0,0,0,0};
unsigned g_regLateDrop[4] = {0,0,0,0};

// Peak instance count per typeID since mission load; resized lazily in the
// per-frame flush loop. Slice-1: tracked but not consumed (foundation for
// slice-2 GPU-emit, where peak[t] drives slot allocation).
std::vector<uint32_t> s_perTypePeak;

// Coalesce ring frame slot — separate from legacy s_frameSlot (line ~:95).
// Advanced inside batcher_prepareBaseInstanceTable() each frame under
// non-legacy mode; mirrors s_frameSlot under legacy mode. Reset per-mission
// in onMapLoad() for hygiene.
uint32_t s_coalesceFrameSlot = 0;

std::vector<uint8_t>  s_stagingVbo;
std::vector<uint32_t> s_stagingIbo;

bool s_geometryFinalized = false;
bool s_fatalRegistrationFailure = false;

// Latched once a compile/link attempt has failed. We never retry inside a
// session because shader source can only change between runs. With this
// latched true, submit() returns false (so callers CPU-fallback), and
// flush()/flushShadow() short-circuit immediately. The user can keep the
// killswitch ON or OFF with no behavioral difference until the next build.

// MaterialGpu-2 sidecar — active by default (v5: default-ON).
// No shader consumer until MaterialGpu-3.
// s_packetMaterialIdx[i] maps draw slot i (= s_sortedPacketOrder[i] position)
// to its entry in s_materialGpuTable.
// Size invariant: s_packetMaterialIdx.size() == s_sortedPacketOrder.size().

extern const bool s_materialGpuEnabled = mc2::gate::get<"MC2_MATERIAL_GPU">();

// MC2_MATERIAL_GPU_SAMPLE — defaults ON (shader sampling active by default).
// Set MC2_MATERIAL_GPU_SAMPLE=0 to disable shader sampling and fall back to texArrayLayer.
// sampleOn requires s_materialGpuEnabled (MC2_MATERIAL_GPU) also active — see sampleOn gate.
// Invariant: materials[materialIdx].albedoTex == texArrayLayer must hold while both paths live.
extern const bool s_materialGpuSampleEnabled = mc2::gate::get<"MC2_MATERIAL_GPU_SAMPLE">();

// V-AMBIENT-STATIC-1: hemisphere ambient fill. Default ON; the options.cfg key StaticPropAmbient = FALSE
// turns it off, and then the uploaded strength is 0.0 so static_prop.vert's hemisphere term is a
// bitwise no-op.
// Read at first use, after the render options are published (gameosmain_init.cpp, before the window exists): a
// namespace-scope initializer would resolve it during static initialization, before that publish, and keep the default.
bool staticPropAmbientV1Enabled() {
    static const bool on = mc2::options::current().staticPropAmbient;
    return on;
}

// V-IBL-STATIC-1: SH-L2 image-based ambient lane.
//   - the options.cfg key StaticPropIblSh is the authoritative switch
//     (resolved once at first use; matches the V-AMBIENT-STATIC-1 pattern).
//   - runtime global g_iblShStrength (below) is the ImGui slider value;
//     it ONLY contributes when the option is on.
//   - per-frame upload: iblShStrengthNightScaled() -- was
//     `s_iblShEnabled ? g_iblShStrength : 0.0f` until IBL-NIGHT-SCALE-1 added
//     the night curve on top (see that block below). Still 0.0f when the
//     feature is off, and still exactly g_iblShStrength in daylight.
//     => env unset/0 -> upload 0.0 -> shader `if (u_iblShStrength > 0.0)`
//        short-circuits to byte-identical pre-slice output (proof of
//        default-OFF kill-switch parity, stronger than multiply-by-zero).
//     => env=1 + slider=0.0 -> upload 0.0 -> visually disabled but the
//        feature flag IS active in the registry (env-only gate).
//     => env=1 + slider=N -> upload N (range 0.0..3.0 enforced by slider UI).
// V-IBL-DEFAULT-FLIP (2026-05-27): default-ON; explicit "=0" is kill-switch.
// env unset -> true; env="0" -> false; env="1"/anything else -> true.
// Read at first use (after main() applied launcher_env.json), see staticPropAmbientV1Enabled().
bool iblShEnabled() {
    static const bool on = mc2::options::current().staticPropIblSh;
    return on;
}

// IBL-NIGHT-SCALE-1: night-scale the SH-IBL ambient probe.
//
// THE DEFECT. The SH set applied to every static prop is projected from a DAY
// SKY (RenderCore/IblShCoeffs.h: "Source HDRI: data/hdr/DaySkyHDRI063B_4K.exr"),
// there is exactly ONE set for every mission and every time of day
// (kMissionShMap[] is empty), and NOTHING scaled it by night. Since the flip to
// default-ON (2026-05-27) that means a day sky has been lighting every night
// mission at full strength. Measured on mc2_10 night (full nightFactor): static
// props run +44%..+83% brighter than with the probe off and one building face
// hard-clips to 255 on 2.49% of its pixels. Stock MC2 had no IBL at all.
//
// WHY NOT THE EXISTING KILLSWITCH. StaticPropIblSh = FALSE removes the term
// outright and drives the perimeter wall, turrets and bunker domes 29%..46%
// darker -- the near-black props the user has separately complained about. The
// probe's DIRECTIONAL STRUCTURE (sky above, ground below) is still correct at
// night; only its INTENSITY was wrong. So this scales toward a floor rather
// than to zero.
//
// THE CURVE: scale = 1 - (1 - floor) * nightFactor, i.e. mix(1.0, floor,
// nightFactor). Chosen by measured sweep on mc2_10 night (see
// .claude/IBL-NIGHT-SCALE-1-STATUS.md), emulating each candidate floor via the
// pre-existing MC2_STATIC_PROP_IBL_SH_STRENGTH env on ONE binary:
//
//   floor  bldg clip%   wall_a   dome_a  turret_c  turret_a
//    1.00     2.49       0.0%     0.0%     0.0%     0.0%   (today's default)
//    0.35     0.57      -9.4%   -11.8%   -11.2%    -7.7%
//    0.25     0.13     -14.1%   -18.4%   -18.3%   -13.0%   <-- chosen (the knee)
//    0.00     0.00     -36.9%   -45.6%   -38.0%   -29.0%   (== blunt killswitch)
//
// 0.25 removes 95% of the clipping for a 13-18% dim, where 0.0 costs 29-46%.
//
// DAY IS BYTE-IDENTICAL BY ARITHMETIC, not by measurement: at day nightFactor
// is exactly 0.0f, so the multiplier is exactly 1.0f and `strength * 1.0f`
// returns the identical float. No epsilon, no rounding.
//
// The night floor is the knee of the sweep above.
static constexpr float s_iblNightFloor = 0.25f;

// Effective SH-IBL strength for this frame: the configured strength (ImGui
// slider / per-mission data/visual_tuning.json "staticPropIblStrength") scaled
// by the night curve.
//
// NOTE ON SOURCING NIGHT STATE. This deliberately calls gos_GetNightState()
// directly instead of reusing the nFactor computed in the uniform-upload
// blocks below, so the probe's night scale does not depend on how those blocks
// source their state.
float iblShStrengthNightScaled() {
    if (!iblShEnabled()) return 0.0f;
    bool  isNight = false;
    float nFactor = 0.0f;
    ::gos_GetNightState(&isNight, &nFactor);
    if (nFactor <= 0.0f) return g_iblShStrength;           // day: exact
    return g_iblShStrength * (1.0f - (1.0f - s_iblNightFloor) * nFactor);
}

// V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular. Default OFF (most props carry no PBR data, so the
// fallback sheen reads as a glint); the options.cfg key StaticPropPbr = TRUE turns it on. While off, the per-frame upload
// pushes u_pbrV1Strength=0.0f, so the shader skips the term and the lit colour is unchanged.
bool pbrV1Enabled() {   // first-use read, see staticPropAmbientV1Enabled()
    static const bool on = mc2::options::current().staticPropPbr;
    return on;
}

// V-MATERIAL-PBR-2 safety interlock: when MC2_VIEW_UNIFORMS=0 the shader's
// PBR block is excluded by `#if defined(MC2_USE_VIEW_UNIFORMS)` (compile-
// time guard). This runtime flag is the belt to the compile-time
// suspenders: even if a build slips through with the guard misconfigured,
// strength is force-zeroed here so the path stays dormant. Cached once
// at process start; mirrors the IBL env-gate idiom.
extern const bool s_viewUniformsDisabled = !mc2::gate::get<"MC2_VIEW_UNIFORMS">();

// Note: definition of `g_iblShStrength` lives at file scope above the
// anonymous namespace (line ~191) so it has external linkage. The lambda-
// initialized s_iblShEnabled stays local here; the slider value is global.

// V-IBL-STATIC-2: per-mission SH set selection.
//   - s_currentShSet points at the active set; initialized to "default"
//     (kIblShSets[0]) so every code path -- including pre-mission frames
//     and code paths that never call setMissionForIbl (editor, savegame
//     load before mission name is plumbed) -- uploads the V-IBL-STATIC-1
//     baseline coefficients. Never nullptr.
//   - setMissionForIbl(name) is called from Mission::init right after
//     onMapLoad(); it consults the compile-time registry (kMissionShMap
//     in IblShRegistry.h) and falls back to "default" for unknown names.
extern const RenderCore::IblShSet* s_currentShSet = &RenderCore::kIblShSets[0];

static bool iblSunRotateEnabled() {
    static const bool on = mc2::options::current().staticPropIblSunRotate;
    return on;
}

// ── IBL-SUN-ROTATE-1's FLOOR, independently adjustable ──────────────────────
//
// WHY A ROTATION NEEDS A FLOOR AT ALL. A rotation is an isometry of the sphere,
// so it cannot create a darker value than the probe already had -- min and max
// over the sphere are invariant. What it changes is WHICH wall gets the dark
// end, and that is the whole problem: once the probe is aimed correctly, the
// probe's dark lobe COINCIDES with the wall that also has NdotL == 0. Before
// the rotation those two darknesses sat on different walls and partially
// covered for each other; after it they stack. Measured worst drops on the
// clamped prop multiplier are -0.705 (mc2_13), -0.704 (mc2_17), -0.692
// (mc2_12) and -0.692 (mc2_18) -- and mc2_12/13/17/18 all carry ambient
// 31/255 == 0.1216, i.e. almost no other fill to catch a face when the probe
// leaves it. An earlier IBL_NIGHT_SCALE floor of 0.0 was rejected for exactly
// this near-black failure; this slice is not going to reintroduce it.
//
// THE SHAPE: a lower bound on the probe's own irradiance, applied as ONE
// UNIFORM LIFT on L00 -- the only isotropic coefficient in evalShL2 (its
// spherical mean is exactly c4*L00, because c5 == c3/3 makes the L20 pair's
// mean vanish). Consequences, all of them wanted:
//   - the AIM IS UNTOUCHED. L1 and L2 are the directional bands and this does
//     not read or write them, so the bright axis lands on the sun to 1e-9
//     whatever the floor is. The floor cannot quietly undo the fix.
//   - it can only LIFT. There is no floor value that makes any direction darker
//     than the plain rotation, so the floor is safe to tune in either direction.
//   - it is UNIFORM across R,G,B (one lift sized by the worst channel) rather
//     than per-channel, which would flatten the dark side's colour: the baked
//     probe's dark end is blue-dominant (min 0.232 R / 0.360 G / 0.477 B) and a
//     per-channel clamp would neutralise exactly that.
//   - the minimum is ROTATION-INVARIANT, so the lift is computed ONCE from the
//     baked table and never per mission.
//
// THE DEFAULT VALUE, 0.45, and where it comes from -- an in-engine anchor, not
// taste. terrain_lod_chunk.frag's own lighting cannot go below
// mix(0.35,1.20,0.02) == 0.367 (pinned by class_light_band_harness
// test_terrain_floor_is_not_0_35). So the GROUND a building stands on has a
// hard 0.367 floor while the building's shadow-side wall has none -- which is
// the "prop reads black against lit ground" complaint in one line. Solving
//     ambient + floor * strength >= 0.367
// at the darkest shipping case (ambient 31/255 == 0.1216 on mc2_12/13/17/18)
// and the shipped daytime IBL strength 0.55 gives floor >= 0.446. 0.45 is that,
// rounded up. On the baked table the resulting lift is +0.218 irradiance, which
// against a bright end of 4.896 is +4.5% -- almost entirely inside the clamp.
//
// "0" or unset-to-0 disables the floor and gives the bare rotation; that is the
// arm to use when measuring how much of a graded difference the floor is worth.
// Bounded [0,2] (out-of-range numbers clamp); text that is not a number parses as
// 0 and so reads as the bare rotation, only NaN reads as the 0.45 default --
// never a silent giant lift.
// Read at first use, see staticPropAmbientV1Enabled().
static float iblSunRotateFloor() {
    static const float v = mc2::options::current().staticPropIblSunRotateFloor;
    return v;
}

// shaders/static_prop.vert::evalShL2, transcribed verbatim (constants c1..c5
// read from the shader body, coefficient binding read from the same lines).
// Used ONLY for the one-off spherical-minimum search below -- the per-vertex
// evaluation stays in the shader. double, not float, so the harness port can
// agree with it exactly.
static double iblEvalShL2Cpu(const float (*C)[3], int ch,
                             double nx, double ny, double nz) {
    const double c1 = 0.429043, c2 = 0.511664, c3 = 0.743125,
                 c4 = 0.886227, c5 = 0.247708;
    return c1 * C[8][ch] * (nx * nx - nz * nz)
         + c3 * C[6][ch] * (ny * ny)
         + c4 * C[0][ch]
         - c5 * C[6][ch]
         + 2.0 * c1 * C[4][ch] * (nx * ny)
         + 2.0 * c1 * C[5][ch] * (ny * nz)
         + 2.0 * c1 * C[7][ch] * (nx * nz)
         + 2.0 * c2 * C[3][ch] * nx
         + 2.0 * c2 * C[1][ch] * ny
         + 2.0 * c2 * C[2][ch] * nz;
}

// The smallest UNIFORM lift, in irradiance units, that brings every channel's
// spherical minimum up to iblSunRotateFloor(). Searched numerically rather than
// baked as a constant on purpose: RenderCore/IblShCoeffs.h is regenerated by
// tools/ibl/project_sh.py, and a hardcoded lift would go silently stale the
// next time it is. 129*256*3 evaluations, once per coefficient table, off the
// render path.
static double iblSunRotateLift(const float (*C)[3]) {
    if (iblSunRotateFloor() <= 0.0f || C == nullptr) return 0.0;
    const double kPi = 3.14159265358979323846;
    double worstDeficit = 0.0;
    const int kLat = 128, kLon = 256;
    for (int i = 0; i <= kLat; ++i) {
        const double th = kPi * static_cast<double>(i) / static_cast<double>(kLat);
        const double sth = std::sin(th), cth = std::cos(th);
        for (int j = 0; j < kLon; ++j) {
            const double ph = 2.0 * kPi * static_cast<double>(j) / static_cast<double>(kLon);
            const double nx = sth * std::cos(ph), ny = cth, nz = sth * std::sin(ph);
            for (int ch = 0; ch < 3; ++ch) {
                const double deficit =
                    static_cast<double>(iblSunRotateFloor()) - iblEvalShL2Cpu(C, ch, nx, ny, nz);
                if (deficit > worstDeficit) worstDeficit = deficit;
            }
        }
    }
    return worstDeficit;
}

static float  s_iblShRotated[9][3]      = {};
static bool   s_iblShRotatedValid       = false;
static float  s_iblShRotatedSun[3]      = { 0.0f, 0.0f, 0.0f };

// The 9x3 coefficient block to upload this flush.
//
// GATE OFF returns the shipped table pointer unchanged -- the identical bytes
// the pre-slice build uploads, not a copy that happens to compare equal.
const float* iblShCoeffsForUpload() {
    const float (*src)[3] = s_currentShSet ? s_currentShSet->coeffs
                                           : RenderCore::kIblShCoeffs;
    if (!iblSunRotateEnabled()) return &src[0][0];

    // NOTE: no local `extern` re-declaration here, unlike the neighbouring
    // upload blocks. This function lives INSIDE the anonymous namespace, where a
    // block-scope extern declares `(anonymous namespace)::gos_GetTerrainLightDir`
    // and links to nothing -- LNK2019, caught on the first build of this slice.
    // gameos.hpp (included at the top of this file) already declares the real
    // one at global scope; `::` says so unambiguously.
    float sx = 0.0f, sy = 0.0f, sz = 1.0f;
    ::gos_GetTerrainLightDir(&sx, &sy, &sz);

    if (s_iblShRotatedValid && src == s_iblShRotatedSrc &&
        sx == s_iblShRotatedSun[0] && sy == s_iblShRotatedSun[1] &&
        sz == s_iblShRotatedSun[2]) {
        return &s_iblShRotated[0][0];
    }

    // THE FRAME. static_prop.vert feeds evalShL2 the STUFF-space normal
    // (`worldNormal`, :652) -- NOT the MC2-swapped `worldNormalMC2` (:664) that
    // the diffuse term uses. So the sun, which arrives in the MC2 frame from
    // gos_GetTerrainLightDir, has to be carried back into Stuff before its
    // azimuth means anything to the probe. The swap is
    // stuff -> mc2 : (x,y,z) -> (-x, z, y)   [static_prop.vert:664, mech.vert:162]
    // and it is its own inverse, so mc2 -> stuff is the same expression:
    // stuff.x = -mc2.x, stuff.y = mc2.z, stuff.z = mc2.y. Stuff is Y-up, so its
    // horizontal plane is x-z and azimuth = atan2(stuff.z, stuff.x)
    //                                     = atan2(mc2.y, -mc2.x).
    const double sunHoriz2 = static_cast<double>(sx) * sx + static_cast<double>(sy) * sy;
    double t = 0.0;
    if (sunHoriz2 > 1e-12) {
        const double sunAz = std::atan2(static_cast<double>(sy), -static_cast<double>(sx));
        // The baked probe's bright axis IS its L1 band. R channel: all three
        // agree to within 0.5 deg (harness F2), and one angle has to serve all
        // three or the channels would shear apart.
        const double probeAz = std::atan2(static_cast<double>(src[2][0]), static_cast<double>(src[3][0]));
        t = sunAz - probeAz;
    }

    const double c1t = std::cos(t),       s1t = std::sin(t);
    const double c2t = std::cos(2.0 * t), s2t = std::sin(2.0 * t);
    for (int ch = 0; ch < 3; ++ch) {
        for (int i = 0; i < 9; ++i) s_iblShRotated[i][ch] = src[i][ch];
        // m=0 ([0] L00, [1] L1-1 pole term, [6] L20) are invariant -- copied above.
        const double a3 = src[3][ch], b2 = src[2][ch];   // m=1 (L11 ~x, L10 ~z)
        s_iblShRotated[3][ch] = static_cast<float>((a3 * c1t - b2 * s1t));
        s_iblShRotated[2][ch] = static_cast<float>((a3 * s1t + b2 * c1t));
        const double a4 = src[4][ch], b5 = src[5][ch];   // m=1 (L2-2 ~xy, L2-1 ~yz)
        s_iblShRotated[4][ch] = static_cast<float>((a4 * c1t - b5 * s1t));
        s_iblShRotated[5][ch] = static_cast<float>((a4 * s1t + b5 * c1t));
        const double a8 = src[8][ch], b7 = src[7][ch];   // m=2 (L22 ~x^2-z^2, L21 ~xz)
        s_iblShRotated[8][ch] = static_cast<float>((a8 * c2t - b7 * s2t));
        s_iblShRotated[7][ch] = static_cast<float>((a8 * s2t + b7 * c2t));
    }

    // The floor. Rotation-invariant, so it is searched once per source table
    // and reused for every sun angle.
    static const float (*s_liftSrc)[3] = nullptr;
    static double s_liftValue = 0.0;
    if (s_liftSrc != src) { s_liftValue = iblSunRotateLift(src); s_liftSrc = src; }
    if (s_liftValue > 0.0) {
        const double dL00 = s_liftValue / 0.886227;   // c4: L00's weight in evalShL2
        for (int ch = 0; ch < 3; ++ch)
            s_iblShRotated[0][ch] = static_cast<float>((static_cast<double>(src[0][ch]) + dL00));
    }

    s_iblShRotatedSrc    = src;
    s_iblShRotatedSun[0] = sx;
    s_iblShRotatedSun[1] = sy;
    s_iblShRotatedSun[2] = sz;
    s_iblShRotatedValid  = true;
    return &s_iblShRotated[0][0];
}

// V-MATERIAL-DEBUG-1: per-fragment material debug view mode for the
// StaticPropOpaque lane. 0 = OFF; the inspector selector sets it at runtime.
// Modes:
//   0 = off (default)
//   1 = albedo (raw texture)
//   2 = materialIdx (hashed palette)
//   3 = normal (worldNormal as RGB)
//   4 = texArrayLayer (hashed palette)
//   5 = roughnessFactor as grayscale (V-MATERIAL-PBR-1)
//   6 = metallicFactor as grayscale  (V-MATERIAL-PBR-1)
// Values outside the implemented range render as hot-pink in the shader.
int s_staticPropDebugMaterialMode = 0;
// Tracks whether finalizeGeometry() produced a correctly-sized sidecar.
// Reset to false at the start of every finalizeGeometry() call; set to true
// only after the sidecar loop completes with size == emitted count.
// Initialized false: no table built yet.
bool s_materialGpuSidecarValid = false;
std::vector<uint32_t>                s_packetMaterialIdx;  // per draw slot
std::vector<RenderCore::MaterialGpu> s_materialGpuTable;   // deduplicated
// V-MATERIAL-STATIC-0: inventory snapshot — one row per s_materialGpuTable entry.
// Built in lockstep with s_materialGpuTable; cleared in onMapUnload. Read-only.
std::vector<StaticPropMaterialInventoryEntry> s_materialInventory;
// v2: per-packet texArrayLayer sidecar. Indexed by global packet index.
// Copied from layerForPacket[] in finalizeGeometry() before that local is discarded.
// Valid until next finalizeGeometry() or onMapUnload(). -1 for unavailable packets.
std::vector<int32_t> s_packetTexArrayLayer;
std::vector<float> s_packetUvScaleX;
std::vector<float> s_packetUvScaleY;
// VK-P11 STATIC-PROP MATERIAL: per-type group-relative albedo layer + alpha-class,
// persisted for mc2_color_prop_island_get_material_feed (the VK island copy-once
// imports s_texArrayOff and samples at typeAlbedoLayer[typeID]). Populated in
// finalizeGeometry() from layerForType[] + s_types[t].alphaClass; cleared in
// onMapUnload(). -1 = type has no alpha-OFF albedo texture.
std::vector<int32_t> s_typeAlbedoLayer;
std::vector<int32_t> s_typeAlphaClass;
std::vector<float> s_typeHotColors;
// v2.1: per-type instance count snapshot from the most recent flush().
// Keyed by typeID. Cleared in onMapUnload().
// Same prior-frame semantics as s_recipeHasSubstrateRecord / hasCullRecord:
//   flush(frame N) snapshots -> extraction(frame N+1) reads -> 0 on frame 1.
std::unordered_map<uint32_t, uint32_t> s_typeInstanceCountPrevFrame;
bool s_reinjectedThisFrame = false;  // item-4 once-per-frame guard

std::vector<uint8_t>              s_slotBucketIndex;    // per sorted draw slot
std::vector<uint32_t>             s_bucketCmdCount;     // per bucket, in sorted-slot order

std::vector<uint32_t> s_sortedTypeOrder;
uint32_t s_alphaOffCount = 0;
uint32_t s_alphaOnCount  = 0;
size_t   s_offGroupTotalBytes = 0;
size_t   s_onGroupTotalBytes  = 0;
size_t   s_coalescePerFrameInstanceBytes = 0;  // populated in Step 5.7

std::vector<uint32_t> s_sortedPacketOrder;     // global packet indices in [OFF | ON] order
uint32_t s_alphaOffCmdCount = 0;               // number of packets in alpha-OFF group
uint32_t s_alphaOnCmdCount  = 0;               // number of packets in alpha-ON group

std::vector<DWORD> s_coalescePinnedNodes;

bool s_coalesceLayoutReady = false;
bool s_coalesceEnabled     = false;
bool s_coalesceArmed       = false;

bool s_coalesceFirstFlushDone = false;

bool s_coalesceEnvDisabled = false;
CoalesceForceDisarm s_coalesceForceDisarm = CoalesceForceDisarm::None;

// Step 12.1 — trace gate for noisy non-lifecycle COALESCE diagnostics.
// Lifecycle events (armed/disarmed/ready/permutation_state when forced)
// are always-on; future per-frame diagnostics gated behind this env.
extern const bool s_coalesceTrace = mc2::gate::get<"MC2_SUBSTRATE_COALESCE_TRACE">();

extern const uint32_t s_globalInstanceCap = []() {
    if (mc2::gate::isNonEmpty<"MC2_STATIC_PROP_GLOBAL_CAP">()) {
        const uint32_t cap = static_cast<uint32_t>(mc2::gate::get<"MC2_STATIC_PROP_GLOBAL_CAP">());
        if (cap > 0) return cap;
    }
    return STATIC_PROP_GLOBAL_CAP_DEFAULT;
}();

std::vector<uint32_t> s_staticBaseInstanceForType;
uint64_t              s_staticBaseBuiltGen     = 0xFFFFFFFFFFFFFFFFull;
std::vector<StaticDrawCmd>   s_staticDrawCmds;          // CPU mirror
// Task 4 criterion 2: fence covering the last StaticPopulation draw. The single-
// region s_staticInstanceSsbo is overwritten in place on a dirty fill; wait on
// this before overwriting so a prior frame's in-flight draw has finished reading
// (GPU executes in submission order -> the latest fence covers all earlier).

// Gate: MC2_STATIC_POP_SPLIT env AND the G2 frozen-records path armed. Defined
// early (before onMapUnload / the draw / reinject) so all use sites resolve.
bool staticPopSplitArmed() {
    static const bool s_on = mc2::gate::get<"MC2_STATIC_POP_SPLIT">();
    return s_on && GpuStaticPropRegistry::frozenRecordsArmed();
}

uint32_t s_offGroupCountThisFrame  = 0;
size_t   s_offGroupBytesThisFrame  = 0;
size_t   s_onGroupBytesThisFrame   = 0;
size_t   s_totalUsedBytesThisFrame = 0;

// --- Step 5.10.c rollback helper (plan v3.7). ---
// Factored out of finalizeGeometry()'s per-group texture-array build so
// Step 12B.5's forced-`size_mismatch` test hook reuses the same code.
// One rollback site, two callers (natural failure + forced disarm).
//
// Per v2r15 out3-MIN-F rollback contract: delete BOTH texture arrays if
// either is non-zero (the OFF group may have succeeded before the ON
// group fails — the half-built array still leaks if not deleted), then
// unpin only the CURRENT group's temp pins. Do NOT touch the other
// group's already-promoted pins in s_coalescePinnedNodes — those release
// at onMapUnload().
//
// Caller responsibility: set s_coalesceLayoutReady / s_coalesceEnabled /
// s_coalesceArmed to false AFTER calling the helper, then return. The
// helper deliberately does NOT mutate the flags so Step 12B.5 (which
// logs a different "(forced)" reason string before invoking the
// rollback) keeps logging-vs-state-change ordering symmetric with the
// natural failure path.
// STATICPROP-MATERIAL-ORM-1 — pick the BPTC internalformat. Albedo arrays are
// sRGB; ORM arrays are LINEAR data (occlusion/roughness/metallic are not color)
// so they MUST use the non-sRGB variant or the values would be gamma-decoded.

// COMPRESSION-BC7-STATICPROP-2 — release all bucketed arrays + reset the
// bucket bookkeeping vectors. No-op (vectors empty) when the gate is OFF, so
// gate-OFF teardown is byte-identical.

void coalesceRollbackTexBuild(std::vector<DWORD>& tempPins) {
    for (DWORD nodeIdx : tempPins) mc2::textures::unpin(nodeIdx);
    tempPins.clear();
}
}  // namespace gos_static_prop_batcher_detail
