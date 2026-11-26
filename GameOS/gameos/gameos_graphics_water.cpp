// gameos_graphics: water tuning state and accessors, the water bridge and gosRenderer::renderWaterFastPath.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mdi_submit.h"                            // MDI-SUBMISSION-SCAFFOLD-1: this piece issues indirect draws
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include "water_planar_view.h"

float mc2NightLightFactor();   // mclib/camera.cpp: 0 day .. 1 full night

namespace { int waterReflectionQuality = -1; }
// Raw legacy mission value (unset = 2); this is what mission profiles save.
int gos_GetWaterReflectionLegacy() { return waterReflectionQuality < 0 ? 2 : waterReflectionQuality; }
int gos_GetWaterReflectionQuality() {
    const int quality = mc2::waterplanar::effectiveReflectionQuality(mc2::options::current().reflectionQuality, gos_GetWaterReflectionLegacy());
    if (!mc2::gate::get<"MC2_WATER_REFLECTION">()) return 0;
    return quality == 2 && !mc2::gate::get<"MC2_WATER_REFLECTION_RT">() ? 1 : quality;
}
void gos_SetWaterReflectionQuality(int quality) {
    waterReflectionQuality = quality < 0 ? 0 : quality > 2 ? 2 : quality;
}


// gos_terrain_bridge_bindThinUniforms deleted by DEAD-BRIDGE-DELETE-1: zero external
// callers (the internal terrainBindThinUniformsForPatchStream IS live; this standalone
// wrapper is not). Whole-repo grep clean.

// gos_terrain_bridge_drawSingleBucketTriangles deleted by TERRAIN-BRIDGE-BODY-DELETE-2:
// callerless after TerrainPatchStream::flush() retired (TERRAIN-BRIDGE-BODY-DELETE-1).

// gos_terrain_bridge_getWaterFastShaderProgram deleted by DEAD-BRIDGE-DELETE-1:
// zero external callers tree-wide. Wrapper around live getWaterFastProgram().

// WATER-DEBUG-VIEWS-1: fragment/material-space debug mode for the MDI water FS
// (gos_terrain_water_mdi.frag uniform u_waterDebugMode). Sentinel -1 = uninit:
// resolve once from MC2_WATER_DEBUG_MODE. ImGui (EditorInspector) may overwrite
// with a live value (>=0). Distinct from the VS geometry-space debugMode latch
// 0=Final default.
int g_waterFsDebugMode = -1;
int gos_GetWaterFsDebugMode()
{
    if (g_waterFsDebugMode < 0) {
        g_waterFsDebugMode = mc2::gate::get<"MC2_WATER_DEBUG_MODE">();
    }
    return g_waterFsDebugMode;
}
void gos_SetWaterFsDebugMode(int m) { g_waterFsDebugMode = (m < 0) ? 0 : m; }
namespace { bool waterProfileRequested = false; }
bool gos_WaterProfileRequested() { return waterProfileRequested; }
void gos_RequestWaterProfile(bool enabled) { waterProfileRequested = enabled; }

// WATER-TUNING-UI-1: runtime-tunable MDI water material params. Defaults MUST
// match the former compile-time consts in gos_terrain_water_mdi.frag EXACTLY
// (byte-identical default render). Uploaded each frame in the MDI bind block;
// driven live by Graphics Options > Water. Only the MDI FS consumes these.
float g_waterAbsorptionDensity = 0.022f;
float g_waterMaxAlpha          = 0.87f;
float g_waterRippleGain        = 0.22f;
float g_waterGlintGain         = 0.30f;
float g_waterDeepColor[3]      = { 0.03f, 0.13f, 0.20f };
float g_waterShallowColor[3]   = { 0.22f, 0.45f, 0.38f };

float gos_GetWaterAbsorptionDensity()       { return g_waterAbsorptionDensity; }
void  gos_SetWaterAbsorptionDensity(float v){ g_waterAbsorptionDensity = v; }
float gos_GetWaterMaxAlpha()                { return g_waterMaxAlpha; }
void  gos_SetWaterMaxAlpha(float v)         { g_waterMaxAlpha = v; }
float gos_GetWaterRippleGain()              { return g_waterRippleGain; }
void  gos_SetWaterRippleGain(float v)       { g_waterRippleGain = v; }
float gos_GetWaterGlintGain()               { return g_waterGlintGain; }
void  gos_SetWaterGlintGain(float v)        { g_waterGlintGain = v; }
void  gos_GetWaterDeepColor(float* rgb)     { rgb[0]=g_waterDeepColor[0]; rgb[1]=g_waterDeepColor[1]; rgb[2]=g_waterDeepColor[2]; }
void  gos_SetWaterDeepColor(float r,float g,float b)    { g_waterDeepColor[0]=r; g_waterDeepColor[1]=g; g_waterDeepColor[2]=b; }
void  gos_GetWaterShallowColor(float* rgb)  { rgb[0]=g_waterShallowColor[0]; rgb[1]=g_waterShallowColor[1]; rgb[2]=g_waterShallowColor[2]; }
void  gos_SetWaterShallowColor(float r,float g,float b) { g_waterShallowColor[0]=r; g_waterShallowColor[1]=g; g_waterShallowColor[2]=b; }

// WATER-VISUAL-FIRST-SLICE: gated camera-INDEPENDENT sky/horizon tint. Strength
// default 0.0 = exact no-op (byte-identical). Env MC2_WATER_SKYTINT=1 bumps the
// default to a small value for quick A/B; the ImGui slider is authoritative once
// touched. Sentinel -1 on strength = uninit (resolve env once). NOT fresnel /
// reflection (those stay shelved per the 2026-05-17 camera-independence ruling).
float g_waterSkyTintStrength = -1.0f;
float g_waterSkyTintColor[3] = { 0.55f, 0.70f, 0.85f };  // soft sky-blue/horizon
float gos_GetWaterSkyTintStrength()
{
    if (g_waterSkyTintStrength < 0.0f) {
        g_waterSkyTintStrength = mc2::gate::get<"MC2_WATER_SKYTINT">() ? 0.15f : 0.0f;
    }
    return g_waterSkyTintStrength;
}
void  gos_SetWaterSkyTintStrength(float v) { g_waterSkyTintStrength = (v < 0.0f) ? 0.0f : v; }
void  gos_GetWaterSkyTintColor(float* rgb) { rgb[0]=g_waterSkyTintColor[0]; rgb[1]=g_waterSkyTintColor[1]; rgb[2]=g_waterSkyTintColor[2]; }
void  gos_SetWaterSkyTintColor(float r,float g,float b) { g_waterSkyTintColor[0]=r; g_waterSkyTintColor[1]=g; g_waterSkyTintColor[2]=b; }

// WATER-SKY-REFLECTION-1: gated camera-DEPENDENT SH-L2 sky reflection on the MDI
// water FS. Default 0.12 = un-gated (sky reflection is ON by default as of
// WATER-SUN-SPEC-1; MC2_WATER_REFLECTION=0 kills it back to 0.0 byte-identical).
// Env MC2_WATER_REFLECTION=0 -> 0.0 no-op; =1 or unset -> 0.12 default.
// Slider authoritative once touched. Sentinel -1 = uninit (resolve env once).
float g_waterReflStrength = -1.0f;
float gos_GetWaterReflStrength()
{
    if (g_waterReflStrength < 0.0f) {
        // WATER-REFL-DEFAULT-ON: default ON (1.5); MC2_WATER_REFLECTION=0 kills it.
        // Higher default exposes the HDRI specular contribution at gameplay camera.
        const bool reflOff = !mc2::gate::get<"MC2_WATER_REFLECTION">();
        g_waterReflStrength = reflOff ? 0.0f : 1.5f;
    }
    return g_waterReflStrength;
}
void  gos_SetWaterReflStrength(float v) { g_waterReflStrength = (v < 0.0f) ? 0.0f : v; }
// Whether reflection is enabled (default: ON). Env MC2_WATER_REFLECTION=0 kills it.
// Resolved live each call (cheap). UI uses this to toggle gate-state label.
int   gos_GetWaterReflectionGate()
{
    return mc2::gate::get<"MC2_WATER_REFLECTION">();  // default ON; =0 disables
}

// WATER-REFLECTION-SAMPLE-1: strength of the terrain reflection RT blended OVER
// the SH sky in the MDI water FS. Gate MC2_WATER_REFLECTION_RT (same env that
// drives the C1 RT fill pass). Default 0 = no RT blend -> SH sky only ->
// byte-identical to Phase A. Sentinel -1 = uninit (resolve env once).
float g_waterRtStrength = -1.0f;
float gos_GetWaterRtStrength()
{
    if (g_waterRtStrength < 0.0f) {
        g_waterRtStrength = mc2::gate::get<"MC2_WATER_REFLECTION_RT">() ? 0.85f : 0.0f;
    }
    return g_waterRtStrength;
}
void gos_SetWaterRtStrength(float v) { g_waterRtStrength = (v < 0.0f) ? 0.0f : v; }
int  gos_GetWaterReflectionRtGate()
{
    return mc2::gate::get<"MC2_WATER_REFLECTION_RT">();
}

void gos_terrain_bridge_renderWaterFast(
    unsigned int recordCount,
    unsigned int waterGosHandle,
    unsigned int waterDetailGosHandle,
    float waterElevation,
    float alphaDepth,
    unsigned int alphaEdgeByte,
    unsigned int alphaMiddleByte,
    unsigned int alphaDeepByte,
    float mapTopLeftX,
    float mapTopLeftY,
    float frameCos,
    float frameCosAlpha,
    float oneOverTF,
    float oneOverWaterTF,
    float cloudOffsetX,
    float cloudOffsetY,
    float sprayOffsetX,
    float sprayOffsetY,
    float maxMinUV)
{
    if (!g_gos_renderer || recordCount == 0) return;
    g_gos_renderer->renderWaterFastPath(
        recordCount, waterGosHandle, waterDetailGosHandle,
        waterElevation, alphaDepth,
        alphaEdgeByte, alphaMiddleByte, alphaDeepByte,
        mapTopLeftX, mapTopLeftY,
        frameCos, frameCosAlpha,
        oneOverTF, oneOverWaterTF,
        cloudOffsetX, cloudOffsetY,
        sprayOffsetX, sprayOffsetY,
        maxMinUV);
}

// VK-P9a WATER-INTEROP: the water island entry points (defined in
// GameOS/gameos/vulkan_water_island.cpp under the same #ifdef). File-scope
// declarations so renderWaterFastPath can call them (extern "C" must be at
// namespace scope, not inside a function body).
extern "C" int mc2_vk_water_island_enabled();
extern "C" int mc2_vk_water_island_run(
    int w, int h,
    const void* recipes, unsigned int recipeCount,
    const void* vsParams, const void* fsParams,
    uint64_t hdriGeneration,
    const void* nativeInputs);

// WATER-SHORE-EDGE-1 feather half-width K (world units), shared by the GL MDI
// bind block and the VK water island fsParams pack (VK-PARITY-WATER-SHORE-1) so
// the two paths cannot drift. MC2_WATER_SHORE_FEATHER_WU overrides; default 7.
static float mc2WaterShoreFeatherWu()
{
    static const float s_shoreFeatherWu = mc2::gate::get<"MC2_WATER_SHORE_FEATHER_WU">();
    return s_shoreFeatherWu;
}

// WATER-HDRI-REFL-PERF-1 LOD default, shared by the GL MDI bind block and the
// VK water island fsParams pack (RUNG-6.5) so the two paths cannot drift.
// MC2_WATER_HDRI_LOD (float) overrides; else MC2_WATER_HDRI_REFL_FULL=1 -> 1.0,
// default 4.0 (256x128 mip of the 4K source — see the bind-block comment).
static float mc2WaterHdriLodDefault()
{
    static float s_lod = -999.0f;
    if (s_lod < -1.0f) {
        if (mc2::gate::isNonEmpty<"MC2_WATER_HDRI_LOD">()) {
            s_lod = mc2::gate::get<"MC2_WATER_HDRI_LOD">();
        } else {
            const bool fullRate = false;
            s_lod = fullRate ? 1.0f : 4.0f;
        }
    }
    return s_lod;
}

void gosRenderer::renderWaterFastPath(
    unsigned int recordCount,
    unsigned int waterGosHandle,
    unsigned int waterDetailGosHandle,
    float waterElevation,
    float alphaDepth,
    unsigned int alphaEdgeByte,
    unsigned int alphaMiddleByte,
    unsigned int alphaDeepByte,
    float mapTopLeftX,
    float mapTopLeftY,
    float frameCos,
    float frameCosAlpha,
    float oneOverTF,
    float oneOverWaterTF,
    float cloudOffsetX,
    float cloudOffsetY,
    float sprayOffsetX,
    float sprayOffsetY,
    float maxMinUV)
{
    ZoneScopedN("renderWaterFastPath");

    // The GL water draw (the MDI program, the GL cull/pack compute, the thin ring, the readback feed) is gone: the native Vulkan producers cull
    // and pack the thin records and the water island draws them. The handles and the detail / spray tuning went with the GL program.
    (void)waterGosHandle; (void)waterDetailGosHandle; (void)frameCosAlpha; (void)oneOverWaterTF; (void)sprayOffsetX; (void)sprayOffsetY;
    if (recordCount == 0) return;
    render_contract::noteRenderPass(render_contract::PassIdentity::Water,
                                    "gosRenderer::renderWaterFastPath");

    if (!mc2_vk_water_island_enabled()) return;
    // The per-frame scalars the native cull/pack reads. False = no water this frame.
    if (!WaterStream::CaptureVkNativeInputs(frameCos)) return;
    WaterStream::VkNativeInputs vkNat{};
    if (!WaterStream::GetVkNativeInputs(vkNat)) return;
    const void*    vkRecipes = static_cast<const void*>(WaterStream::GetRecipes());
    const uint32_t vkRecipeN = WaterStream::GetRecipeCount();
    gosPostProcess* ppVk = getGosPostProcess();
    if (!vkRecipes || vkRecipeN == 0 || !ppVk) return;

    // Same MVP the native cull/pack uses: the baked dispatch matrix when the solid pass is armed.
    const float* wMvpWaterMdi =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!wMvpWaterMdi) wMvpWaterMdi = gos_GetTerrainMVPMat4();  // safety: pre-arm/first frame
    if (!wMvpWaterMdi) return;

    // WATER reflection frame fix: terrain_camera_pos_ is the Stuff/MLR eye (.x=left, .y=elevation, .z=forward); the water FS consumes cameraPos in
    // RAW MC2 (.x=east, .y=north, .z=up), so apply the documented Stuff->MC2 swap here.
    vec4 camMC2;
    mc2ToRender(camMC2, terrain_camera_pos_);
    camMC2.w = 1.0f;

    // Pack VS/FS UBO params LOCKSTEP with water_geom.{vert,frag}.
    struct VkWaterVsP { float m[16]; float p0[4],p1[4],p2[4],p3[4]; } vsP{};
    struct VkWaterFsP { float fog[4],f0[4],f1[4],f2[4],f3[4],deep[4],shallow[4],f4[4],f5[4],f6[4],f7[4],f8[4],f9[4],bake[4],debug[4]; } fsP{};
    memcpy(vsP.m, wMvpWaterMdi, 16 * sizeof(float));
    vsP.p0[0]=waterElevation; vsP.p0[1]=alphaDepth; vsP.p0[2]=mapTopLeftX; vsP.p0[3]=mapTopLeftY;
    vsP.p1[0]=frameCos; vsP.p1[1]=maxMinUV; vsP.p1[2]=oneOverTF; vsP.p1[3]=cloudOffsetX;
    vsP.p2[0]=cloudOffsetY;
    vsP.p2[2]=static_cast<float>(alphaEdgeByte); vsP.p2[3]=static_cast<float>(alphaMiddleByte);
    vsP.p3[0]=static_cast<float>(alphaDeepByte);
    {
        const vec4 waterFog = uint32_to_vec4(renderStates_[gos_State_Fog]);
        memcpy(fsP.fog, &waterFog, 4*sizeof(float));
    }
    fsP.f0[0]= SmokeMode::fixedTimestepEnabled()
                 ? static_cast<float>(SmokeMode::fixedClockSeconds())
                 : static_cast<float>((static_cast<double>((timing::get_wall_time_ms() - timeStart_)) / 1000.0));
    fsP.f0[1]=camMC2.x; fsP.f0[2]=camMC2.y; fsP.f0[3]=camMC2.z;
    fsP.f1[0]=alphaDepth; fsP.f1[1]=g_waterAbsorptionDensity;
    fsP.f1[2]=g_waterMaxAlpha; fsP.f1[3]=g_waterRippleGain;
    fsP.f2[0]=g_waterGlintGain;
    // RUNG-6.5 WATER-REFLECTION PORT: reflection inputs (MC2_WATER_REFLECTION=0 -> strength 0.0 -> FS block skipped; !isHdriReady -> lod -1 -> SH-L2 fallback).
    {
        const bool vkHdriAvail = ppVk->isHdriReady();
        fsP.f2[1] = gos_GetWaterReflectionQuality() ? gos_GetWaterReflStrength() : 0.0f;
        fsP.f2[2] = vkHdriAvail ? ppVk->getSkyYaw() : 0.0f;           // u_skyYaw
        fsP.f2[3] = vkHdriAvail ? mc2WaterHdriLodDefault() : -1.0f;   // u_waterHdriLod
        fsP.f3[0] = static_cast<float>(ppVk->getWidth());                          // u_waterScreenSize
        fsP.f3[1] = static_cast<float>(ppVk->getHeight());
        // Low/Potato retain a static foam band in the existing water pass.
        fsP.f3[3] = mc2::options::lowCost() ? 0.0f : 1.0f;
    }
    memcpy(fsP.deep,    g_waterDeepColor,    3*sizeof(float));
    memcpy(fsP.shallow, g_waterShallowColor, 3*sizeof(float));
    // VK-PARITY-WATER-SHORE-1: WATER-SHORE-EDGE-1 inputs. f4.y/f4.z (hasHeight / hasMask) are set by the island, which owns the VK copies of the
    // shore textures.
    {
        extern int          gos_WaterShoreMode();
        extern const float* gos_TerrainLodChunk_ShorelineMaskBounds();
        extern const float* gos_TerrainLodChunk_WaterShoreHeightBounds();
        fsP.f4[0] = static_cast<float>(gos_WaterShoreMode());     // u_shoreMode
        fsP.f4[3] = mc2WaterShoreFeatherWu();        // u_shoreFeatherWu
        memcpy(fsP.f5, gos_TerrainLodChunk_WaterShoreHeightBounds(), 4*sizeof(float)); // u_shoreHeightBounds
        memcpy(fsP.f6, gos_TerrainLodChunk_ShorelineMaskBounds(),    4*sizeof(float)); // u_shoreMaskBounds
        fsP.f7[0] = waterElevation;                  // waterElevation
    }
    // VK-PARITY-F3: edge feather killswitch/strength, map half extent, authored sky tint, frustum aspect.
    {
        static const int s_vkWEdgeFeather = true;
        static const float s_vkWEdgeFeatherStr = mc2::gate::get<"MC2_WATER_EDGE_FEATHER_STRENGTH">();
        extern bool g_cameraProjAspectNative;
        const float vkVpAspect = ppVk->getHeight() > 0
            ? static_cast<float>(ppVk->getWidth()) / static_cast<float>(ppVk->getHeight()) : (4.0f / 3.0f);
        fsP.f8[0] = static_cast<float>(s_vkWEdgeFeather);
        fsP.f8[1] = s_vkWEdgeFeatherStr;
        fsP.f8[2] = ppVk->getMapHalfExtent();
        fsP.f8[3] = gos_GetWaterSkyTintStrength();
        memcpy(fsP.f9, g_waterSkyTintColor, 3*sizeof(float));
        fsP.f9[3] = g_cameraProjAspectNative ? vkVpAspect : (4.0f / 3.0f);
    }
    (void)mc2_vk_water_island_run(
        ppVk->getWidth(), ppVk->getHeight(),
        vkRecipes, vkRecipeN, &vsP, &fsP,
        ppVk->isHdriReady() ? ppVk->getHdriGeneration() : 0,
        &vkNat);
}
