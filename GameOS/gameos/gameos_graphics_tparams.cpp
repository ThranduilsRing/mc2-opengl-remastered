// gameos_graphics: terrain uniform binding for the patch stream, the lighting debug view and the terrain, lighting, night and material setters and getters.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include <algorithm>
#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"


// TERRAIN-MATERIAL-LIB-1: default OFF. Drives u_useMaterialLib (chunk-path
// roughness/AO branch). Evaluated once (static) -- matches terrainNormalArrayEnabled().


// LIGHTING-DEBUG-VIEWS: render-family-shared lighting debug channel on the
// shared surfaceDebugMode integer space. The 40-series is reserved for the
// unified lighting channels so the SAME id means the SAME channel across
// terrain (1A), static_prop (1B) and mech (1C): 40 albedo, 41 normal, 42 sun,
// 43 ambient, 44 shadow, 45 final, 46 overbright, 47 lightcount, 48 lightindex.
// Returns -1 when no channel is selected, so callers keep their existing debug
// mode (default 0 = off = byte-identical render).
// NON-static: also consumed by gos_static_prop_batcher.cpp (extern decl there).
// Not every channel applies to every family — a family ignores ids it does
// not implement (falls through to its normal render), which is safe.
//
// DEBUG-VIEW-SWEEP-1: gos_SetGlobalDebugView() retargets the shared channel at
// runtime (command-channel `debugview N`); -1 = no channel.
static int g_mc2LightingDebugOverride = -1;
int mc2LightingDebugMode()
{
    return g_mc2LightingDebugOverride;
}
static int g_mc2GlobalDebugView = 0;
extern "C" void gos_SetGlobalDebugView(int view)
{
    view = std::clamp(view, 0, 6);
    g_mc2GlobalDebugView = view;
    static const struct { int lighting; int vfx; int propMat; } kMap[7] = {
        { -1, 0, 0 },   // 0 off
        { 44, 0, 0 },   // 1 shadow factor
        { 50, 0, 0 },   // 2 normalized depth
        { 40, 1, 0 },   // 3 unlit albedo
        { 51, 4, 0 },   // 4 overdraw proxy
        { 41, 0, 0 },   // 5 world normals
        { 52, 3, 2 },   // 6 chunk/LOD tint
    };
    g_mc2LightingDebugOverride = kMap[view].lighting;
    gos_vfx_setDebugMode(kMap[view].vfx);
    batcher_setDebugMaterialMode(kMap[view].propMat);
    mc2::fmt::print_err("[DEBUG_VIEW v1] view={:d} lighting={:d} vfx={:d} propMat={:d}\n", mc2::fmt::as_i32(view), mc2::fmt::as_i32(kMap[view].lighting), mc2::fmt::as_i32(kMap[view].vfx), mc2::fmt::as_i32(kMap[view].propMat));
}
extern "C" int gos_GetGlobalDebugView() { return g_mc2GlobalDebugView; }

// TERRAIN-DETAIL-ANTI-TILE-1: pack distance-fade + macro-noise knobs into the
// unused .yzw of the detailNormalStrength uniform (the shader reads only .x today).
// Gate MC2_TERRAIN_DETAIL_ANTITILE; default OFF => all three return 0 =>
// detailNormalStrength stays {strength,0,0,0} => byte-identical render.
//   .y = fadeStart (world units)   .z = fadeEnd (world units)   .w = macroStrength
// Read once. Returns pointer to a static float[3].






void __stdcall gos_SetTerrainTessParams(float level, float near_dist, float far_dist) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTessParams(level, near_dist, far_dist);
}
void __stdcall gos_SetTerrainPhongAlpha(float a) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPhongAlpha(a);
}
void __stdcall gos_SetTerrainDisplaceScale(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDisplaceScale(s);
}
float gos_GetTerrainPhongAlpha() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPhongAlpha() : 0.0f;
}
float gos_GetTerrainDisplaceScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDisplaceScale() : 0.0f;
}
float gos_GetTerrainDetailTiling() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailTiling() : 1.0f;
}
float gos_GetTerrainDetailStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailStrength() : 4.0f;
}
float gos_GetTerrainPOMScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPOMScale() : 0.0f;
}
void __stdcall gos_SetTerrainWireframe(bool w) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWireframe(w);
}
void __stdcall gos_SetTerrainDebugMode(float mode) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDebugMode(mode);
}
float __stdcall gos_GetTerrainDebugMode() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDebugMode() : 0.0f;
}




// Vulkan owns the shadow maps independently of the old GL shader programs.
bool __stdcall gos_IsModernShadowPathActive() {
    return g_gos_renderer != nullptr;
}

void gos_SetShadowMode(bool enable) {
    if (g_gos_renderer) g_gos_renderer->setShadowMode(enable);
}

void gos_GetTerrainCameraPos(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& cp = g_gos_renderer->getTerrainCameraPos();
        if (x) *x = cp.x;
        if (y) *y = cp.y;
        if (z) *z = cp.z;
    }
}
// F1 Stage A unified-projection production setter.
// Repackages column-major Stuff::Matrix4D -> row-major M (matching
// the legacy upload convention so all consumers see the same
// shader-visible matrix orientation). Writes the terrain_mvp_ cache
// for 15+ existing gos_GetTerrainMVPMat4() callers (CullUBO at
// gpu_cull_compute.cpp:831, mech-batcher, static-prop-batcher,
// etc.) to inherit transparently. The 10 CPU bind sites (Task 15)
// each upload the cached matrix to their respective programs via
// glUniformMatrix4fv(loc, 1, GL_FALSE, terrain_mvp_).
//
// Signature is parameter-light — no explicit program handle —
// because per-program upload happens at the 10 bind sites via the
// cache, not here. R-clipw polarity is baked into the matrix
// itself via kAxisSwapMC2toGL (Task 7g); shader-visible clip.w > 0
// for in-front MC2 verts.
// [MVP_DIAG v1] S2.7 — shared frame counter incremented at the
// canonical world-to-clip set site. Read by gos_GetTerrainMVPMat4
// and the static-prop coalesce uploader (extern in
// gos_static_prop_batcher.cpp). Kept past S2.7 because every probe
// is throttled (frames {1,5,30,120}) — silent at steady state.
long g_mvpDiagFrame = 0;

// RENDER-VIEW-CURRENCY-1 (M2): object/mech stale-snapshot-read telemetry. Bumped
// by gos_GetObjectDrawMVP() (gos_object_draw_mvp.h) whenever an armed object draw
// requested the dispatch snapshot but its view epoch did not match the current
// view (it fell back to the live MVP instead). Should be 0 in steady state; a
// rising count localizes a future re-introduction of the BUG1/3/5 staleness. The
// counter is read by the debug-state dump. MC2_OBJ_MVP_STALE_FATAL=1 aborts so CI
// catches it instead of a player on NVIDIA three days later.
static unsigned long g_objMvpStaleCount = 0;
unsigned long gos_object_mvp_stale_count() { return g_objMvpStaleCount; }
// VIEW-EPOCH-DEDUPE-1: counts the snapshot-USED path (object draw projected through
// the depth-matched dispatch snapshot = the FixB z-fight fix active). Paired with
// the stale counter it proves the fix is live, not merely "fewer fallbacks".
static unsigned long g_objMvpUsedCount = 0;
unsigned long gos_object_mvp_used_count() { return g_objMvpUsedCount; }
void gos_object_mvp_note_used() { ++g_objMvpUsedCount; }

// RENDER-FRAME-CONTEXT-1 (M-guard): counts divergences between the read-only
// gos_FrameCtx() mirror and the authoritative globals. No-op by construction in
// the additive slice (the mirror reads those globals); becomes load-bearing when
// the authority-inversion slice makes the globals shims. MC2_FRAMECTX_MISMATCH_FATAL=1
// aborts so CI catches a desync instead of it going silent.
static unsigned long g_framectxMismatchCount = 0;
unsigned long gos_framectx_mismatch_count() { return g_framectxMismatchCount; }
void gos_framectx_note_mismatch(const char* what) {
    ++g_framectxMismatchCount;
    static const bool s_fatal = mc2::gate::get<"MC2_FRAMECTX_MISMATCH_FATAL">();
    if (s_fatal) {
        mc2::fmt::print_err("[FRAMECTX] FATAL: frame-context mirror diverged from authoritative global ({}); count={:d}\n", mc2::fmt::safe(what ? what : "?"), mc2::fmt::as_u32(g_framectxMismatchCount));
        fflush(stderr);
        abort();
    }
}
void gos_object_mvp_note_stale() {
    ++g_objMvpStaleCount;
    static const bool s_fatal = mc2::gate::get<"MC2_OBJ_MVP_STALE_FATAL">();
    if (s_fatal) {
        mc2::fmt::print_err("[OBJ_MVP] FATAL: object draw read a stale-view dispatch MVP snapshot (view-epoch mismatch); count={:d}\n", mc2::fmt::as_u32(g_objMvpStaleCount));
        fflush(stderr);
        abort();
    }
}

// VIEW-EPOCH-DEDUPE-1: semantic VIEW-CONTENT epoch. g_mvpDiagFrame is a raw publish
// counter that bumps on EVERY gos_SetWorldToClipGL (~2x/frame: the early-publish at
// mission.cpp + the gamecam publish, usually the SAME camera). The object/mech MVP
// currency check must NOT treat a redundant same-camera republish as a view change,
// or it false-stales the dispatch snapshot every frame and silently disables the
// FixB z-fight fix (telemetry: stale_mvp_reads ~= frame count). This epoch bumps
// ONLY when the published world-to-clip CONTENT actually changes; real camera motion
// still advances it (so vanish/flicker stays fixed). Consumed via gos_object_draw_mvp.h.
long g_viewContentEpoch = 0;
static float g_prevPublishedWtc[16] = { 0 };
static bool  g_havePrevPublishedWtc = false;

void __stdcall gos_SetWorldToClipGL(const glm::mat4& mat)
{
    if (!g_gos_renderer) return;
    const float* col = reinterpret_cast<const float*>(&mat);
    #define WTC(r,c) col[(c)*4 + (r)]
    // Repackage column-major Stuff -> row-major M (same convention as
    // the legacy terrain upload; consumers see GL_FALSE-interpretation
    // transpose, R-clipw polarity baked into kAxisSwapMC2toGL).
    float M[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            M[i*4 + j] = WTC(i, j);
    #undef WTC
    g_gos_renderer->setTerrainMVP(M);

    // [MVP_DIAG v1] S2.7 — log set_mvp entry. Throttled to frames
    // {1,5,30,120}. row0 is M[0..3] AFTER the column->row repack.
    ++g_mvpDiagFrame;

    // VIEW-EPOCH-DEDUPE-1: advance the semantic view-content epoch only on a real
    // matrix change. Identical same-camera republishes (early-publish then gamecam)
    // leave it unchanged, so the dispatch snapshot is not false-staled and object draw
    // keeps the depth-matched snapshot. Logic lives in the pure kernel so the offline
    // unit harness (tests/unit/test_view_currency.cpp) verifies the SAME code.
    {
        static const float kViewEpsilon = 1e-6f;  // ignore redundant republish, not real motion
        if (mc2::view_currency::viewContentChanged(g_prevPublishedWtc, M,
                                                   g_havePrevPublishedWtc, kViewEpsilon))
            ++g_viewContentEpoch;
        for (int i = 0; i < 16; ++i) g_prevPublishedWtc[i] = M[i];
        g_havePrevPublishedWtc = true;
    }
    if (g_mvpDiagFrame == 1 || g_mvpDiagFrame == 5 ||
        g_mvpDiagFrame == 30 || g_mvpDiagFrame == 120) {
        MC2_DIAGFE(stderr,
                "[MVP_DIAG v1] event=set_mvp frame=%ld renderer=%p row0=[%g %g %g %g]\n",
                g_mvpDiagFrame, static_cast<void*>(g_gos_renderer),
                M[0], M[1], M[2], M[3]);
        fflush(stderr);
    }
}
// gos_SetTerrainMVP / gos_SetTerrainViewport: raw-float counterparts to
// gos_SetWorldToClipGL. Used by EditorCamera.h which builds the matrix
// via axis-swap math rather than going through Stuff::Matrix4D. Both APIs
// write the same terrain_mvp_ cache in the renderer.
void __stdcall gos_SetTerrainMVP(const float* matrix16) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMVP(matrix16);
}
void __stdcall gos_SetTerrainViewport(float vmx, float vmy, float vax, float vay) {
    if (g_gos_renderer) g_gos_renderer->setTerrainViewport(vmx, vmy, vax, vay);
}
void __stdcall gos_SetTerrainCameraPos(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCameraPos(x, y, z);
}
// Static shadow API — world-fixed shadow map rendered once at map load
void gos_SetMapHalfExtent(float halfExtent) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->setMapHalfExtent(halfExtent);
}
void gos_SetWaterElevation(float elevation) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->setWaterElevation(elevation);
}




void gos_GetTerrainLightDir(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& ld = g_gos_renderer->getTerrainLightDir();
        if (x) *x = ld.x;
        if (y) *y = ld.y;
        if (z) *z = ld.z;
    }
}
void gos_SetTerrainLightDir(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainLightDir(x, y, z);
}

// EDGE-LIGHT-EMISSION-1: night-state bridge for the GPU static-prop path.
//
// THE BUG THIS EXISTS TO FIX. Stock MC2 lights runway/taxiway edge lights and
// beacons through the authored "hot colour magic" vertex tags: every one of
// data/tgl/{runway,red,green,blue}beaconlight.ase paints its post base pure
// green (0xff00ff00 = "Hot Green -- Building Base Lights -- ONLY at NIGHT"),
// and each fixture's .ini carries a `//Night Lights` block giving the colour
// to emit (runwaylight/redbeaconlight 0x00ff0000 red, greenbeaconlight
// 0x0000ff00, bluebeaconlight 0x000000ff). The legacy CPU path
// (mclib/tgl.cpp:1901) reads eye->getIsNight()/getNightFactor() and honours
// those branches. The GPU static-prop batcher path -- which is the path these
// props ACTUALLY take at runtime -- passed `false, 0.0` hardcoded into
// get_base_light() (static_prop.vert, "stubbed for now"), so every night-only
// branch evaluated its DAYTIME arm and the emission was simply never applied.
// The lights were switched off. This restores the dropped stock state; it is
// not a new visual treatment.
//
// Push model mirrors gos_SetTerrainLightDir / gos_SetTerrainMissionFlatLight:
// gamecam.cpp feeds it once per frame, GameOS reads it at uniform-upload time.
static bool  s_gosIsNight     = false;
static float s_gosNightFactor = 0.0f;

void gos_SetNightState(bool isNight, float nightFactor) {
    s_gosIsNight     = isNight;
    nightFactor = std::clamp(nightFactor, 0.0f, 1.0f);
    s_gosNightFactor = nightFactor;
}
void gos_GetNightState(bool* isNight, float* nightFactor) {
    if (isNight)     *isNight     = s_gosIsNight;
    if (nightFactor) *nightFactor = s_gosNightFactor;
}

// TERRAIN-AMBIENT-FLOOR-2: daylight level in [0,1] for the terrain ambient
// floor, pushed from gamecam.cpp once per frame in the same place the night
// state is pushed. The DECISION lives in GameOS/gameos/utils/terrain_daylight.h
// (pure, harness-executed); this is only the transport.
//
// Deliberately NOT derived here from s_gosIsNight/s_gosNightFactor alone.
//
// CORRECTED 2026-07-30 (DAYLIGHT-GATE-RECHECK-1). This comment used to justify
// that by claiming Camera::update() re-derives the flags "AFTER camera.cpp:2033
// has folded a below-horizon authored pitch into a moon pitch -- so on mc2_05
// (authored 270) they report full day with the light overhead." That is FALSE.
// The fold is at camera.cpp:2079-2080 inside updateDaylight(), which never runs
// in a mission (GameCamera::activate overrides the virtual base without chaining
// -- code/gamecam.cpp:1024, its own call commented at :1060; per-frame site
// commented at camera.cpp:2238). On mc2_05 lightPitch stays 270, so
// Camera::update() (camera.cpp:2254-2281) gets altitude -90 and correctly
// reports isNight = true, nightFactor = 1.
//
// The decision to key on the AUTHORED pitch still stands, for a sounder reason:
// dayLightPitch is written once at load and is never folded or animated, so it
// stays correct even if the fold is ever revived, whereas the runtime flags are
// one commented-out line away from being wrong. The gate takes the MIN of both,
// which fails dark on disagreement. See the header for the measured evidence.
//
// Default 0.0 = fully night = the floor is OFF until something feeds it. A
// missing feed can only make this change a no-op, never a bright regression.
static float s_gosTerrainDaylight = 0.0f;

void gos_SetTerrainDaylightLevel(float level) {
    if (!(level >= 0.0f)) level = 0.0f;   // also catches NaN
    if (level > 1.0f)     level = 1.0f;
    s_gosTerrainDaylight = level;
}
float gos_GetTerrainDaylightLevel() {
    return s_gosTerrainDaylight;
}
// VK-TERRAIN-LODFADE: reuses the existing gos_GetTerrainCameraPos free function
// (defined above at the gos_SetShadowMode block) for the camera-distance fade port --
// no new accessor needed.

// CEMENT-NIGHT-LIGHT-1: mission flat-pad light colour feed (gamecam.cpp, per
// frame). Killswitch MC2_CEMENT_MISSION_LIGHT=0 -> force (1,1,1) (repro of the
// old bright-pads-at-night behaviour); default ON.
void gos_SetTerrainMissionFlatLight(float r, float g, float b) {
    static const bool s_enabled = mc2::gate::get<"MC2_CEMENT_MISSION_LIGHT">();
    if (!s_enabled) { r = 1.0f; g = 1.0f; b = 1.0f; }
    if (g_gos_renderer) g_gos_renderer->setTerrainMissionFlatLight(r, g, b);
}
void gos_GetTerrainMissionFlatLight(float* r, float* g, float* b) {
    float rr = 1.0f, gg = 1.0f, bb = 1.0f;
    if (g_gos_renderer) {
        const vec4& fl = g_gos_renderer->getTerrainMissionFlatLight();
        rr = fl.x; gg = fl.y; bb = fl.z;
    }
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
}
void gos_SetTerrainDetailParams(float tiling, float strength) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDetailParams(tiling, strength);
}
void gos_SetTerrainMaterialNormal(int index, DWORD handle) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMaterialNormal(index, handle);
}
void gos_SetTerrainWorldScale(float scale) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWorldScale(scale);
}
void gos_SetTerrainCellBombParams(float scale, float jitter, float rotation) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCellBombParams(scale, jitter, rotation);
}
void gos_SetTerrainPOMParams(float scale, float minLayers, float maxLayers) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPOMParams(scale, minLayers, maxLayers);
}





void gos_SetTerrainShadowSoftness(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainShadowSoftness(s);
}
float gos_GetTerrainShadowSoftness() {
    return g_gos_renderer ? g_gos_renderer->getTerrainShadowSoftness() : 2.5f;
}

void gos_SetTerrainMatNormalBoost(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatNormalBoost(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatNormalBoost(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatNormalBoost(rock, grass, dirt, concrete);
    else { *rock = 0.9f; *grass = 1.1f; *dirt = 1.1f; *concrete = 2.5f; }
}
void gos_SetTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatTiling(rock, grass, dirt, concrete, snow);
}
void gos_GetTerrainMatTiling(float* rock, float* grass, float* dirt, float* concrete, float* snow) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatTiling(rock, grass, dirt, concrete, snow);
    else { *rock = 3.0f; *grass = 2.0f; *dirt = 1.0f; *concrete = 6.0f; *snow = 1.0f; }
}
void gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi) {
    if (!g_gos_renderer) return;
    const float v[4] = { gMinusRLo, gMinusRHi, gBrightLo, gBrightHi };
    g_gos_renderer->setTerrainClassGrass(v);
}
void gos_GetTerrainClassGrass(float* gMinusRLo, float* gMinusRHi, float* gBrightLo, float* gBrightHi) {
    if (g_gos_renderer) {
        float v[4]; g_gos_renderer->getTerrainClassGrass(v);
        *gMinusRLo = v[0]; *gMinusRHi = v[1]; *gBrightLo = v[2]; *gBrightHi = v[3];
    } else { *gMinusRLo = -0.02f; *gMinusRHi = 0.06f; *gBrightLo = 0.22f; *gBrightHi = 0.40f; }
}
void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi) {
    if (!g_gos_renderer) return;
    const float v[4] = { rMinusGLo, rMinusGHi, rBrightLo, rBrightHi };
    g_gos_renderer->setTerrainClassDirt(v);
}
void gos_GetTerrainClassDirt(float* rMinusGLo, float* rMinusGHi, float* rBrightLo, float* rBrightHi) {
    if (g_gos_renderer) {
        float v[4]; g_gos_renderer->getTerrainClassDirt(v);
        *rMinusGLo = v[0]; *rMinusGHi = v[1]; *rBrightLo = v[2]; *rBrightHi = v[3];
    } else { *rMinusGLo = -0.02f; *rMinusGHi = 0.06f; *rBrightLo = 0.22f; *rBrightHi = 0.45f; }
}
void gos_SetTerrainTintStrengthScale(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintStrengthScale(s);
}
float gos_GetTerrainTintStrengthScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainTintStrengthScale() : 1.0f;
}
void gos_SetTerrainControlAlbedoStrength(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainControlAlbedoStrength(s);
}
float gos_GetTerrainControlAlbedoStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainControlAlbedoStrength() : 0.0f;
}
void gos_SetTerrainSnowBrightnessDampen(float v) {
    if (g_gos_renderer) g_gos_renderer->setTerrainSnowBrightnessDampen(v);
}
float gos_GetTerrainSnowBrightnessDampen() {
    return g_gos_renderer ? g_gos_renderer->getTerrainSnowBrightnessDampen() : 0.78f;
}
void gos_SetTerrainTintRock(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintRock(r, g, b);
}
void gos_GetTerrainTintRock(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintRock(r, g, b);
    else { *r = 0.36f; *g = 0.37f; *b = 0.40f; }
}
void gos_SetTerrainTintGrass(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintGrass(r, g, b);
}
void gos_GetTerrainTintGrass(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintGrass(r, g, b);
    else { *r = 0.35f; *g = 0.42f; *b = 0.25f; }
}
void gos_SetTerrainTintDirt(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintDirt(r, g, b);
}
void gos_GetTerrainTintDirt(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintDirt(r, g, b);
    else { *r = 0.48f; *g = 0.42f; *b = 0.33f; }
}
void gos_SetTerrainTintConcrete(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintConcrete(r, g, b);
}
void gos_GetTerrainTintConcrete(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintConcrete(r, g, b);
    else { *r = 0.55f; *g = 0.53f; *b = 0.50f; }
}
void gos_SetTerrainTintSnow(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintSnow(r, g, b);
}
void gos_GetTerrainTintSnow(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintSnow(r, g, b);
    else { *r = 0.75f; *g = 0.78f; *b = 0.84f; }
}
void gos_SetTerrainMatRoughness(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatRoughness(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatRoughness(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatRoughness(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
void gos_SetTerrainMatAO(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatAO(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatAO(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatAO(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
// POM-PERLAYER-HEIGHT-1: per-layer POM displacement-height multipliers.
void gos_SetTerrainMatHeight(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatHeight(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatHeight(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatHeight(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
// TERRAIN-MATERIAL-LIB-1: public wrapper -- terrainMaterialLibEnabled() above is
// file-static (TU-local); the live chunk binder (gos_terrain_lod_chunk.cpp) is a
// separate TU and needs the same MC2_TERRAIN_MATERIAL_LIB gate value to drive its
// own u_useMaterialLib upload. Same cached-static evaluation as the bridge path.
bool gos_TerrainMaterialLibEnabled() {
    static const bool enabled = mc2::gate::get<"MC2_TERRAIN_MATERIAL_LIB">();
    return enabled;
}
void gos_SetTerrainNormalsFromHeightStrength(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainNormalsFromHeightStrength(s);
}
float gos_GetTerrainNormalsFromHeightStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainNormalsFromHeightStrength() : 1.0f;
}
void gos_SetTerrainCliffShadowFloor(float f) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCliffShadowFloor(f);
}
float gos_GetTerrainCliffShadowFloor() {
    return g_gos_renderer ? g_gos_renderer->getTerrainCliffShadowFloor() : 0.0f;
}

void gos_SetTerrainDrawEnabled(bool e) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDrawEnabled(e);
}
bool gos_GetTerrainDrawEnabled() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDrawEnabled() : true;
}
