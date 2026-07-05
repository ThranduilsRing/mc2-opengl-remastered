#include "GraphicsOptionsWindow.h"
#include "imgui.h"
#include "../mclib/dynamic_decal_ring.h"  // MC2_DYNAMIC_DECALS live count

#include "../GameOS/gameos/gos_postprocess.h"
#include "../GameOS/gameos/view_uniforms_gl.h"
#include "../RenderCore/RenderResourceRegistry.h"
#include "gameos.hpp"
#include "../GameOS/gameos/gos_static_prop_killswitch.h"
#include "../GameOS/gameos/gos_mech_killswitch.h"
#include "../GameOS/gameos/ibl_sh_runtime.h"     // g_iblShStrength, g_pbrV1Strength, g_pbrV1RoughnessOverride*
#include "../mclib/projectz_overlay.h"
#include "draw_packet_emitter.h"       // DrawPacketsDebugSnapshot g_dpSnapshot
#include "StaticPropTypeDesc.h"        // RenderCore::StaticPropTypeDesc
// Forward-declare the two batcher accessors we need rather than pulling in
// gos_static_prop_batcher.h, which transitively requires Stuff.hpp / tgl.h.
bool     batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out);
uint32_t batcher_getStaticPropTypeDescCount();
// Terrain PBR parameter accessors (defined in gameos_graphics.cpp).
void  gos_SetTerrainMatNormalBoost(float rock, float grass, float dirt, float concrete);
void  gos_GetTerrainMatNormalBoost(float* rock, float* grass, float* dirt, float* concrete);
void  gos_SetTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow);
void  gos_GetTerrainMatTiling(float* rock, float* grass, float* dirt, float* concrete, float* snow);
void  gos_SetTerrainTintStrengthScale(float s);
float gos_GetTerrainTintStrengthScale();
// TERRAIN-TINT-UI-1: material base tint colors.
void  gos_SetTerrainTintRock(float r, float g, float b);
void  gos_GetTerrainTintRock(float* r, float* g, float* b);
void  gos_SetTerrainTintGrass(float r, float g, float b);
void  gos_GetTerrainTintGrass(float* r, float* g, float* b);
void  gos_SetTerrainTintDirt(float r, float g, float b);
void  gos_GetTerrainTintDirt(float* r, float* g, float* b);
// TERRAIN-TUNING-UI-1 / TERRAIN-LIGHTING-1 — consolidated tunables (the
// Object Inspector "Terrain Pass" panel used to mirror these; they live
// here now alongside the rest of the terrain tuning stack).
void  gos_SetTerrainNormalsFromHeightStrength(float s);
float gos_GetTerrainNormalsFromHeightStrength();
void  gos_SetTerrainLightingV1Strength(float s);
float gos_GetTerrainLightingV1Strength();
void  gos_SetTerrainLightingV2Floor(float f);
float gos_GetTerrainLightingV2Floor();
void  gos_SetTerrainCliffShadowFloor(float f);
float gos_GetTerrainCliffShadowFloor();
// WATER-TUNING-UI-1 / WATER-DEBUG-VIEWS-1 — MDI water FS accessors (defined in
// gameos_graphics.cpp). Debug-mode selector + runtime material tunables
// (defaults match the former gos_terrain_water_mdi.frag consts exactly).
int   gos_GetWaterFsDebugMode();
void  gos_SetWaterFsDebugMode(int m);
float gos_GetWaterAbsorptionDensity();
void  gos_SetWaterAbsorptionDensity(float v);
float gos_GetWaterMaxAlpha();
void  gos_SetWaterMaxAlpha(float v);
float gos_GetWaterRippleGain();
void  gos_SetWaterRippleGain(float v);
float gos_GetWaterGlintGain();
void  gos_SetWaterGlintGain(float v);
void  gos_GetWaterDeepColor(float* rgb);
void  gos_SetWaterDeepColor(float r, float g, float b);
void  gos_GetWaterShallowColor(float* rgb);
void  gos_SetWaterShallowColor(float r, float g, float b);
// WATER-VISUAL-FIRST-SLICE — gated camera-independent sky tint.
float gos_GetWaterSkyTintStrength();
void  gos_SetWaterSkyTintStrength(float v);
void  gos_GetWaterSkyTintColor(float* rgb);
void  gos_SetWaterSkyTintColor(float r, float g, float b);
// WATER-SKY-REFLECTION-1 — gated camera-dependent SH-L2 sky reflection.
float gos_GetWaterReflStrength();
void  gos_SetWaterReflStrength(float v);
int   gos_GetWaterReflectionGate();
// WATER-REFLECTION-SAMPLE-1 — terrain reflection RT blend strength.
float gos_GetWaterRtStrength();
void  gos_SetWaterRtStrength(float v);
int   gos_GetWaterReflectionRtGate();
// TERRAIN-RESAMPLE-1 — height-tex source/render/factor accessors + live
// factor setter. C-linkage (declared extern "C" in gos_terrain_height_tex.h).
extern "C" {
    int  __stdcall gos_terrainHeightSourceSide(void);
    int  __stdcall gos_terrainHeightTexSide(void);
    int  __stdcall gos_terrainHeightResampleFactor(void);
    void __stdcall gos_setTerrainHeightResampleFactor(int factor);
}
// TERRAIN-CLASSIFY-TUNING-1: colormap RGB classifier thresholds (gameos_graphics.cpp).
void gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi);
void gos_GetTerrainClassGrass(float* gMinusRLo, float* gMinusRHi, float* gBrightLo, float* gBrightHi);
void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi);
void gos_GetTerrainClassDirt(float* rMinusGLo, float* rMinusGHi, float* rBrightLo, float* rBrightHi);
// MECH-LIGHTING-UI-1: mech ambient + specular + PBR roughness (gos_mech_batcher.cpp).
extern "C" int   batcher_getMechAmbientEnabled(void);
extern "C" void  batcher_setMechAmbientEnabled(int on);
extern "C" float batcher_getMechAmbientStrength(void);
extern "C" void  batcher_setMechAmbientStrength(float s);
extern "C" int   batcher_getMechSpecularEnabled(void);
extern "C" void  batcher_setMechSpecularEnabled(int on);
extern "C" float batcher_getMechSpecularStrength(void);
extern "C" void  batcher_setMechSpecularStrength(float s);
extern "C" float batcher_getMechMetalRoughness(void);
extern "C" void  batcher_setMechMetalRoughness(float r);
// BT2018-MECH-MATERIAL-GAMMA-1/TUNING-1: imported-mech albedo knobs (imported only).
extern "C" float batcher_getImportedMechGamma(void);
extern "C" void  batcher_setImportedMechGamma(float g);
extern "C" float batcher_getImportedMechAlbedoScale(void);
extern "C" void  batcher_setImportedMechAlbedoScale(float s);
extern "C" float batcher_getImportedMechAoStrength(void);
extern "C" void  batcher_setImportedMechAoStrength(float s);
extern "C" float batcher_getImportedMechNormalStrength(void);
extern "C" void  batcher_setImportedMechNormalStrength(float s);
extern "C" int   batcher_getStandardLitEnabled(void);
extern "C" void  batcher_setStandardLitEnabled(int on);
extern "C" float batcher_getPbrMetallicInfluence(void);
extern "C" void  batcher_setPbrMetallicInfluence(float v);
extern "C" float batcher_getPbrRoughnessMin(void);
extern "C" void  batcher_setPbrRoughnessMin(float v);
extern "C" float batcher_getPbrRoughnessMax(void);
extern "C" void  batcher_setPbrRoughnessMax(float v);
extern "C" float batcher_getPbrAmbientSpecularStrength(void);
extern "C" void  batcher_setPbrAmbientSpecularStrength(float v);
extern "C" float batcher_getPbrWearStrength(void);
extern "C" void  batcher_setPbrWearStrength(float v);
extern "C" int   batcher_getPbrTriplanar(void);
extern "C" void  batcher_setPbrTriplanar(int on);
extern "C" float batcher_getPbrTriplanarScale(void);
extern "C" void  batcher_setPbrTriplanarScale(float v);
extern "C" float batcher_getMechGlassRoughness(void);
extern "C" void  batcher_setMechGlassRoughness(float r);
extern "C" float batcher_getMechBackFillStrength(void);
extern "C" void  batcher_setMechBackFillStrength(float v);
// VFX-TUNING-UI-1: GPU particle debug-mode + intensity scales (defined in
// GameOS/gameos/gos_particle_bridge.cpp). All scales default 1.0 = no-op.
extern "C" int   gos_vfx_getDebugMode(void);
extern "C" void  gos_vfx_setDebugMode(int mode);
extern "C" float gos_vfx_getBrightness(void);
extern "C" float gos_vfx_getAdditiveBrightness(void);
extern "C" float gos_vfx_getAlphaScale(void);
extern "C" void  gos_vfx_setBrightness(float v);
extern "C" void  gos_vfx_setAdditiveBrightness(float v);
extern "C" void  gos_vfx_setAlphaScale(float v);
extern "C" int   gos_vfx_getSoftEnabled(void);
extern "C" void  gos_vfx_setSoftEnabled(int e);
extern "C" float gos_vfx_getSoftDistance(void);
extern "C" void  gos_vfx_setSoftDistance(float v);
extern "C" int   gos_vfx_getLitEnabled(void);
extern "C" void  gos_vfx_setLitEnabled(int e);
extern "C" float gos_vfx_getLitStrength(void);
extern "C" void  gos_vfx_setLitStrength(float v);
// MISSION-VISUAL-TUNING-1: profile status accessors (defined in visual_tuning_profile.cpp)
void        visualTuning_applyProfile(const char* missionName);
bool        visualTuning_saveCurrentToMission();
const char* visualTuning_getProfilePath();
const char* visualTuning_getActiveMission();
bool        visualTuning_hasProfileFile();
int         visualTuning_getAppliedKeyCount();

#include <cstdio>
#include <cmath>
#include <cstdlib>   // getenv

namespace GraphicsOptionsWindow {

static bool s_open = false;

void setOpen(bool open) { s_open = open; }
bool isOpen()           { return s_open; }

// ─────────────────────────────────────────────────────────────────────────────
// Terrain debug mode table — mirrors gos_terrain.frag branch logic exactly.
// ─────────────────────────────────────────────────────────────────────────────
struct TerrainMode {
    float       value;
    const char* name;
    const char* channels;   // null = no sub-line
};

static const TerrainMode kTerrainModes[] = {
    //  value    name                     channel breakdown
    {  0.0f, "OFF",               nullptr },
    // ── Visual modes ──────────────────────────────────────────────────────────
    {  1.0f, "Depth Comparison",  "R = rasterized depth  |  G = undisplaced depth  (range 0.85–1.0)" },
    {  2.0f, "Raw Colormap",      "terrain colormap tex1, unlit" },
    {  3.0f, "Blurred Colormap",  "LOD-tiered blur: far=1-tap, mid=5-tap, near=9-tap disc" },
    {  4.0f, "Material Weights",  "R = rock  |  G = grass  |  B = dirt  (classification weights)" },
    {  5.0f, "Normal Lighting",   "diffuse contribution (0.35–1.20 remapped to 0–1)" },
    {  6.0f, "Shadow Factor",     "PCF static shadow × dynamic shadow" },
    {  7.0f, "Cloud Shadow (n/a)", "removed — cloud is now the fullscreen cloud.frag pass (see Render Passes > Cloud Shadows)" },
    // ── Diagnostic modes ──────────────────────────────────────────────────────
    {  8.0f, "Cement Diagnostic", "R = layer valid  |  G = layer index/255  |  B = useCementAtlas==0" },
    {  9.0f, "Thin-Record",       "R = recipeIdx/255  |  G = flags/255  |  B = terrainHandle/255" },
    { 10.0f, "Height Normal",     "TERRAIN-NORMALS-FROM-HEIGHT-1: derived normal as RGB (centered around 0.5)" },
    { 11.0f, "Hemi Additive",     "TERRAIN-LIGHTING-2: V1 hemi contribution after V2 modulation (×4 for visibility)" },
    { -1.0f, "Tess Alive Probe",  "solid red = tessellation frag shader is running" },
};
static const int kTerrainModeCount = (int)(sizeof(kTerrainModes) / sizeof(kTerrainModes[0]));
static const int kDiagnosticStart  = 8; // index into kTerrainModes where diagnostics begin

// ─────────────────────────────────────────────────────────────────────────────
// Env gate table — baked at startup, shown read-only.
// defaultOn=true means the feature is enabled when the var is absent/non-zero.
// ─────────────────────────────────────────────────────────────────────────────
struct EnvGate {
    const char* var;
    const char* desc;
    bool        defaultOn;
    bool        inverted;   // true = presence of var DISABLES the feature
};

static const EnvGate kEnvGates[] = {
    { "MC2_GPU_DRIVEN",             "GPU-driven rendering master gate",          false, false },
    { "MC2_GPU_OBJECTS",            "GPU static prop batcher",                   true,  false },
    { "MC2_GPU_MECHS",              "GPU mech batcher",                          true,  false },
    { "MC2_GPU_CULL",               "GPU frustum cull",                          false, false },
    { "MC2_GPU_CULL_SUBSTRATE",     "GPU substrate (default-on)",                true,  false },
    { "MC2_GPU_PARTICLES",          "GPU particle system",                       false, false },
    { "MC2_MATERIAL_GPU",           "MaterialGpu lookup path",                   false, false },
    { "MC2_RENDER_WATER_FASTPATH",  "Fast water render path",                    false, false },
    { "MC2_OBJECT_ID_BUFFER",       "Object-ID picking buffer",                  false, false },
    { "MC2_HDRI_SKY",               "HDRI skybox",                               false, false },
    { "MC2_DISABLE_GOSFX",          "FX/particles (var disables)",               false, true  },
    { "MC2_RENDER_CONTRACT_ASSERT", "Render-contract GL state validation",        false, false },
    { "MC2_IMGUI",                  "Dear ImGui overlay (default-on)",           true,  false },
    { "MC2_GL_DEBUG",               "GL debug callback",                         false, false },
    { "MC2_DEBUG_RENDERER",         "Debug primitive renderer",                  false, false },
};
static const int kEnvGateCount = (int)(sizeof(kEnvGates) / sizeof(kEnvGates[0]));

// ─────────────────────────────────────────────────────────────────────────────

static void drawTerrainSection() {
    // ── Draw + wireframe ──────────────────────────────────────────────────────
    bool terrainDraw = gos_GetTerrainDrawEnabled();
    if (ImGui::Checkbox("Terrain Draw", &terrainDraw))
        gos_SetTerrainDrawEnabled(terrainDraw);

    // No public getter for wireframe state — track locally.
    static bool s_terrainWire = false;
    if (ImGui::Checkbox("Terrain Wireframe", &s_terrainWire))
        gos_SetTerrainWireframe(s_terrainWire);

    // ── Debug mode scrollable child ───────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextUnformatted("Surface Debug Mode");
    ImGui::SameLine();
    if (ImGui::SmallButton("OFF##toff")) gos_SetTerrainDebugMode(0.0f);

    const float lineH = ImGui::GetTextLineHeight();
    const float pad   = ImGui::GetStyle().ItemSpacing.y;

    float childH = (lineH + pad) * 2.0f                                  // OFF row
                 + (lineH * 2.2f + pad) * (float)(kDiagnosticStart - 1)  // visual 1..7
                 + (lineH + pad) * 1.2f                                   // separator + label
                 + (lineH * 2.2f + pad) * (float)(kTerrainModeCount - kDiagnosticStart);
    if (childH < 120.0f) childH = 120.0f;
    if (childH > 420.0f) childH = 420.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::BeginChild("##terrain_modes", ImVec2(0, childH), ImGuiChildFlags_Border);
    ImGui::PopStyleVar();

    float    curMode = gos_GetTerrainDebugMode();
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    bool        sentDiagHeader = false;

    for (int i = 0; i < kTerrainModeCount; ++i) {
        const TerrainMode& m = kTerrainModes[i];

        if (i == kDiagnosticStart && !sentDiagHeader) {
            ImGui::Separator();
            ImGui::TextDisabled("  Diagnostics");
            sentDiagHeader = true;
        }

        bool  active = fabsf(curMode - m.value) < 0.4f;
        bool  hasSub = (m.channels != nullptr);
        float rowH   = hasSub ? lineH * 2.2f : lineH * 1.3f;

        ImGui::PushID(i);

        if (active)
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.40f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));

        bool hit = ImGui::Selectable("##row", active, ImGuiSelectableFlags_AllowOverlap,
                                     ImVec2(0.0f, rowH));
        if (hit) gos_SetTerrainDebugMode(m.value);

        ImGui::PopStyleColor();
        if (active) ImGui::PopStyleColor();

        ImVec2 rmin = ImGui::GetItemRectMin();

        char heading[80];
        if (m.value < -0.5f)
            snprintf(heading, sizeof(heading), "[-1]  %s", m.name);
        else
            snprintf(heading, sizeof(heading), "[%d]  %s", (int)(m.value + 0.5f), m.name);

        ImU32 headCol = active ? IM_COL32(120, 255, 120, 255)
                               : IM_COL32(230, 230, 230, 255);
        dl->AddText(ImVec2(rmin.x + 6.0f, rmin.y + 3.0f), headCol, heading);

        if (hasSub)
            dl->AddText(ImVec2(rmin.x + 18.0f, rmin.y + lineH + 4.0f),
                        IM_COL32(140, 140, 140, 255), m.channels);

        ImGui::PopID();
    }

    ImGui::EndChild();
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawTerrainTuningSection() {
    // ── Tessellation ──────────────────────────────────────────────────────────
    // No public getters for tess params — mirror defaults here.
    static float s_tessLevel    = 4.0f;
    static float s_tessNear     = 200.0f;
    static float s_tessFar      = 2000.0f;

    ImGui::SeparatorText("Tessellation");
    bool tessChanged = false;
    tessChanged |= ImGui::SliderFloat("Level##tess",      &s_tessLevel, 1.0f, 16.0f, "%.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max tessellation subdivisions (full tiling at near dist). Default: 4.0\nOnly visible on curved/hilly terrain — flat terrain looks unchanged.");
    tessChanged |= ImGui::SliderFloat("Near dist##tess",  &s_tessNear,  50.0f, 1000.0f, "%.0f wu");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera distance at which max tessellation applies. Default: 200 wu");
    tessChanged |= ImGui::SliderFloat("Far dist##tess",   &s_tessFar,   500.0f, 8000.0f, "%.0f wu");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera distance at which tessellation falls to 1× (disabled). Default: 2000 wu");
    if (tessChanged)
        gos_SetTerrainTessParams(s_tessLevel, s_tessNear, s_tessFar);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tess")) {
        s_tessLevel = 4.0f; s_tessNear = 200.0f; s_tessFar = 2000.0f;
        gos_SetTerrainTessParams(4.0f, 200.0f, 2000.0f);
    }

    // ── Phong smoothing ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Phong Smoothing");
    float phong = gos_GetTerrainPhongAlpha();
    if (ImGui::SliderFloat("Alpha##phong", &phong, 0.0f, 1.0f))
        gos_SetTerrainPhongAlpha(phong);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = linear TES interpolation, 1 = full Phong smoothing. Default: 0.5\n"
                          "Only visible on curved/hilly terrain; flat terrain is unaffected.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##phong")) gos_SetTerrainPhongAlpha(0.5f);

    // ── Displacement ──────────────────────────────────────────────────────────
    ImGui::SeparatorText("Displacement");
    float disp = gos_GetTerrainDisplaceScale();
    if (ImGui::SliderFloat("Scale##disp", &disp, 0.0f, 8.0f))
        gos_SetTerrainDisplaceScale(disp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Vertex displacement amplitude (dirt material only). Default: 2.0\n"
                          "Requires dirt normal map with height data in alpha channel.\n"
                          "Set > 0 and look at a dirt-heavy area to see the effect.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##disp")) gos_SetTerrainDisplaceScale(2.0f);

    // ── Normal / detail maps ──────────────────────────────────────────────────
    ImGui::SeparatorText("Normal Maps");

    float tiling   = gos_GetTerrainDetailTiling();
    static float s_strength = 1.0f;  // no public getter; matches terrain_detail_strength_ default

    if (ImGui::SliderFloat("Tiling##nm", &tiling, 0.1f, 4.0f))
        gos_SetTerrainDetailParams(tiling, s_strength);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Global UV tiling multiplier on top of per-material values.\n"
                          "Per-material defaults: rock=3×, grass=2×, dirt=1×, concrete=6×, snow=1×\n"
                          "(tune per-material in the 'Per-Material Tiling' section below). Default: 1.0");

    if (ImGui::SliderFloat("Strength##nm", &s_strength, 0.0f, 12.0f))
        gos_SetTerrainDetailParams(tiling, s_strength);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Global normal-map amplitude multiplier.\n"
                          "Per-material boost applied on top: rock=0.9×, grass=0.5×, dirt=1.1×, concrete=2.5×. Default: 1.0");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##nm")) {
        s_strength = 1.0f;
        gos_SetTerrainDetailParams(1.0f, 1.0f);
    }

    // ── Material tint colors ──────────────────────────────────────────────────
    ImGui::SeparatorText("Material Tint Colors");
    ImGui::TextDisabled("Base color per material. Mixed with colormap by Tint Strength above.");

    static float s_tintRock[3]  = { 0.36f, 0.37f, 0.40f };
    static float s_tintGrass[3] = { 0.35f, 0.42f, 0.25f };
    static float s_tintDirt[3]  = { 0.48f, 0.42f, 0.33f };
    static bool  s_tintInited   = false;
    if (!s_tintInited) {
        gos_GetTerrainTintRock( &s_tintRock[0],  &s_tintRock[1],  &s_tintRock[2]);
        gos_GetTerrainTintGrass(&s_tintGrass[0], &s_tintGrass[1], &s_tintGrass[2]);
        gos_GetTerrainTintDirt( &s_tintDirt[0],  &s_tintDirt[1],  &s_tintDirt[2]);
        s_tintInited = true;
    }

    if (ImGui::ColorEdit3("Rock##tc",  s_tintRock,  ImGuiColorEditFlags_Float))
        gos_SetTerrainTintRock(s_tintRock[0], s_tintRock[1], s_tintRock[2]);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tcr")) {
        s_tintRock[0] = 0.36f; s_tintRock[1] = 0.37f; s_tintRock[2] = 0.40f;
        gos_SetTerrainTintRock(0.36f, 0.37f, 0.40f);
    }

    if (ImGui::ColorEdit3("Grass##tc", s_tintGrass, ImGuiColorEditFlags_Float))
        gos_SetTerrainTintGrass(s_tintGrass[0], s_tintGrass[1], s_tintGrass[2]);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tcg")) {
        s_tintGrass[0] = 0.35f; s_tintGrass[1] = 0.42f; s_tintGrass[2] = 0.25f;
        gos_SetTerrainTintGrass(0.35f, 0.42f, 0.25f);
    }

    if (ImGui::ColorEdit3("Dirt##tc",  s_tintDirt,  ImGuiColorEditFlags_Float))
        gos_SetTerrainTintDirt(s_tintDirt[0], s_tintDirt[1], s_tintDirt[2]);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tcd")) {
        s_tintDirt[0] = 0.48f; s_tintDirt[1] = 0.42f; s_tintDirt[2] = 0.33f;
        gos_SetTerrainTintDirt(0.48f, 0.42f, 0.33f);
    }

    // ── Per-material normal boost ─────────────────────────────────────────────
    ImGui::SeparatorText("Per-Material Normal Boost");
    ImGui::TextDisabled("Scales the shader's per-material normal strength (on top of global Strength above).");

    static float s_boostRock     = 0.9f;
    static float s_boostGrass    = 1.1f;
    static float s_boostDirt     = 1.1f;
    static float s_boostConcrete = 2.5f;
    static bool  s_boostInited   = false;
    if (!s_boostInited) {
        gos_GetTerrainMatNormalBoost(&s_boostRock, &s_boostGrass, &s_boostDirt, &s_boostConcrete);
        s_boostInited = true;
    }

    bool boostChanged = false;
    boostChanged |= ImGui::SliderFloat("Rock##nb",     &s_boostRock,     0.0f, 5.0f);
    boostChanged |= ImGui::SliderFloat("Grass##nb",    &s_boostGrass,    0.0f, 5.0f);
    boostChanged |= ImGui::SliderFloat("Dirt##nb",     &s_boostDirt,     0.0f, 5.0f);
    boostChanged |= ImGui::SliderFloat("Concrete##nb", &s_boostConcrete, 0.0f, 5.0f);
    if (boostChanged)
        gos_SetTerrainMatNormalBoost(s_boostRock, s_boostGrass, s_boostDirt, s_boostConcrete);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##nb")) {
        s_boostRock = 0.9f; s_boostGrass = 1.1f; s_boostDirt = 1.1f; s_boostConcrete = 2.5f;
        gos_SetTerrainMatNormalBoost(0.9f, 1.1f, 1.1f, 2.5f);
    }

    // ── Per-material tiling ───────────────────────────────────────────────────
    ImGui::SeparatorText("Per-Material Tiling");
    ImGui::TextDisabled("UV repeat multiplier per material. Grass default 2x (was 12x).");

    static float s_tilingRock     = 3.0f;
    static float s_tilingGrass    = 2.0f;
    static float s_tilingDirt     = 1.0f;
    static float s_tilingConcrete = 6.0f;
    static float s_tilingSnow     = 1.0f;
    static bool  s_tilingInited   = false;
    if (!s_tilingInited) {
        gos_GetTerrainMatTiling(&s_tilingRock, &s_tilingGrass, &s_tilingDirt, &s_tilingConcrete, &s_tilingSnow);
        s_tilingInited = true;
    }

    bool tilingChanged = false;
    tilingChanged |= ImGui::SliderFloat("Rock##mt",     &s_tilingRock,     0.1f, 20.0f, "%.1f");
    tilingChanged |= ImGui::SliderFloat("Grass##mt",    &s_tilingGrass,    0.1f, 20.0f, "%.1f");
    tilingChanged |= ImGui::SliderFloat("Dirt##mt",     &s_tilingDirt,     0.1f, 20.0f, "%.1f");
    tilingChanged |= ImGui::SliderFloat("Concrete##mt", &s_tilingConcrete, 0.1f, 20.0f, "%.1f");
    tilingChanged |= ImGui::SliderFloat("Snow##mt",     &s_tilingSnow,     0.1f, 20.0f, "%.1f");
    if (tilingChanged)
        gos_SetTerrainMatTiling(s_tilingRock, s_tilingGrass, s_tilingDirt, s_tilingConcrete, s_tilingSnow);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mt")) {
        s_tilingRock = 3.0f; s_tilingGrass = 2.0f; s_tilingDirt = 1.0f;
        s_tilingConcrete = 6.0f; s_tilingSnow = 1.0f;
        gos_SetTerrainMatTiling(3.0f, 2.0f, 1.0f, 6.0f, 1.0f);
    }

    // ── POM ───────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Parallax Occlusion Mapping");

    static float s_pomScale = 0.02f;
    static float s_pomMin   = 8.0f;
    static float s_pomMax   = 32.0f;

    bool pomChanged = false;
    pomChanged |= ImGui::SliderFloat("Scale##pom",     &s_pomScale, 0.0f, 0.08f, "%.4f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Height-map depth scale. Dirt gets 2.5× internal boost. Default: 0.02");
    pomChanged |= ImGui::SliderFloat("Min layers##pom", &s_pomMin,  1.0f, 32.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Minimum ray-march samples at grazing angles. Default: 8");
    pomChanged |= ImGui::SliderFloat("Max layers##pom", &s_pomMax,  8.0f, 64.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Maximum ray-march samples looking straight down. Default: 32");
    if (pomChanged)
        gos_SetTerrainPOMParams(s_pomScale, s_pomMin, s_pomMax);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##pom")) {
        s_pomScale = 0.02f; s_pomMin = 8.0f; s_pomMax = 32.0f;
        gos_SetTerrainPOMParams(0.02f, 8.0f, 32.0f);
    }

    // ── Color tint ────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Color / Tinting");

    float tintScale = gos_GetTerrainTintStrengthScale();
    if (ImGui::SliderFloat("Tint strength##cs", &tintScale, 0.0f, 2.0f))
        gos_SetTerrainTintStrengthScale(tintScale);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = pure colormap passthrough (no material tint)\n"
                          "1 = default tint (0.18–0.50 blend by luminance, 0.85 for snow)\n"
                          "2 = double tint intensity");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##cs")) gos_SetTerrainTintStrengthScale(1.0f);

    // ── Shadow softness ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Shadow");

    float softness = gos_GetTerrainShadowSoftness();
    if (ImGui::SliderFloat("Shadow softness##ss", &softness, 0.0f, 1.0f))
        gos_SetTerrainShadowSoftness(softness);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PCF penumbra radius. 0=hard shadows, 1=maximum softness. Default: 0.9");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##ss")) gos_SetTerrainShadowSoftness(0.9f);

    // ── Cell-bomb noise ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Cell-Bomb Noise");
    ImGui::TextDisabled("Hash-based breakup applied on top of normal maps.");

    static float s_cellScale    = 8.0f;
    static float s_cellJitter   = 0.8f;
    static float s_cellRotation = 1.0f;

    bool cellChanged = false;
    cellChanged |= ImGui::SliderFloat("Scale##cb",    &s_cellScale,    1.0f, 20.0f);
    cellChanged |= ImGui::SliderFloat("Jitter##cb",   &s_cellJitter,   0.0f,  1.0f);
    cellChanged |= ImGui::SliderFloat("Rotation##cb", &s_cellRotation, 0.0f,  1.0f);
    if (cellChanged)
        gos_SetTerrainCellBombParams(s_cellScale, s_cellJitter, s_cellRotation);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##cb")) {
        s_cellScale = 8.0f; s_cellJitter = 0.8f; s_cellRotation = 1.0f;
        gos_SetTerrainCellBombParams(8.0f, 0.8f, 1.0f);
    }

    // ── Normals-from-Height + Lighting V1 ────────────────────────────────────
    // Consolidated home for the TERRAIN-NORMALS-FROM-HEIGHT-1 /
    // TERRAIN-RESAMPLE-1 / TERRAIN-LIGHTING-1 stack. Originally lived in the
    // Object Inspector "Terrain Pass" panel; moved here so all terrain
    // tuning is in one window. Env vars MC2_TERRAIN_NORMALS_FROM_HEIGHT and
    // MC2_TERRAIN_LIGHTING_V1 are authoritative on/off gates — sliders
    // tune strength only when the env gate is enabled.
    ImGui::SeparatorText("Normals-from-Height + Lighting V1");

    // Gate status — read env locally; the slider widgets stay enabled even
    // when the gate is off so the user can pre-arm a strength for the next
    // mission load.
    bool nfhGateOn = false;
    {
        const char* env = std::getenv("MC2_TERRAIN_NORMALS_FROM_HEIGHT");
        nfhGateOn = (env && env[0] && env[0] != '0');
        if (nfhGateOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "Normals-from-Height: ON  (MC2_TERRAIN_NORMALS_FROM_HEIGHT=%s)", env);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Normals-from-Height: OFF (set MC2_TERRAIN_NORMALS_FROM_HEIGHT=1)");
        }
    }

    // Height tex source/render readout + live resample combo.
    int srcSide = ::gos_terrainHeightSourceSide();
    int rndSide = ::gos_terrainHeightTexSide();
    int factor  = ::gos_terrainHeightResampleFactor();
    if (srcSide > 0) {
        ImGui::Text("Height tex: source %d^2 -> render %d^2", srcSide, rndSide);
    } else {
        ImGui::TextDisabled("Height tex: not uploaded (no mission loaded)");
    }
    const char* factorLabels[3] = { "1x", "2x", "4x" };
    int curIdx = (factor == 4) ? 2 : (factor == 2) ? 1 : 0;
    int newIdx = curIdx;
    if (ImGui::Combo("Resample##trf", &newIdx, factorLabels, 3)) {
        if (newIdx != curIdx) {
            const int f = (newIdx == 2) ? 4 : (newIdx == 1) ? 2 : 1;
            ::gos_setTerrainHeightResampleFactor(f);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("CPU bilinear resample factor for the height tex.\n"
                          "1x = source grid only; 4x = 4× sub-grid (finer slope normals).\n"
                          "env MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR sets startup default.");

    // Normals-from-Height strength.
    float nfh = ::gos_GetTerrainNormalsFromHeightStrength();
    if (ImGui::SliderFloat("NfH strength##tnfh", &nfh, 0.0f, 1.5f, "%.2f")) {
        ::gos_SetTerrainNormalsFromHeightStrength(nfh);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Multiplier on the additive height-derived normal term.\n"
                          "1.0 = full slope tilt (default). 0.0 = no slope contribution.\n"
                          "Only effective when MC2_TERRAIN_NORMALS_FROM_HEIGHT=1.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tnfh")) ::gos_SetTerrainNormalsFromHeightStrength(1.0f);
    if (!nfhGateOn) {
        ImGui::TextDisabled("(NfH strength only takes effect when env gate enabled)");
    }

    // Lighting V1 (hemisphere ambient) gate status + strength.
    bool litGateOn = false;
    {
        const char* env = std::getenv("MC2_TERRAIN_LIGHTING_V1");
        litGateOn = (env && env[0] && env[0] != '0');
        if (litGateOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "Lighting V1 (hemi ambient): ON");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Lighting V1 (hemi ambient): OFF (set MC2_TERRAIN_LIGHTING_V1=1)");
        }
    }
    float ambient = ::gos_GetTerrainLightingV1Strength();
    if (ImGui::SliderFloat("Ambient strength##tlv1", &ambient, 0.0f, 2.0f, "%.2f")) {
        ::gos_SetTerrainLightingV1Strength(ambient);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hemisphere sky/ground ambient fill on terrain.\n"
                          "Added AFTER shadow multiplication — shadowed terrain still receives\n"
                          "sky bounce, which is intentional. Default 1.0; 0.5 for subtler fill.\n"
                          "Only effective when MC2_TERRAIN_LIGHTING_V1=1.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tlv1")) ::gos_SetTerrainLightingV1Strength(1.0f);
    if (!litGateOn) {
        ImGui::TextDisabled("(slider has no effect until env gate enabled)");
    }

    // TERRAIN-LIGHTING-2: shadow-aware fill floor. Effective only when
    // both V1 and V2 env gates are ON. floor=1.0 → V1 unmodulated;
    // floor=0.3 = default; floor=0.0 → hemi follows shadow exactly.
    bool litV2GateOn = false;
    {
        const char* env = std::getenv("MC2_TERRAIN_LIGHTING_V2");
        litV2GateOn = (env && env[0] && env[0] != '0');
        if (litV2GateOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "Lighting V2 (shadow-aware fill): ON");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Lighting V2 (shadow-aware fill): OFF (set MC2_TERRAIN_LIGHTING_V2=1)");
        }
    }
    float v2Floor = ::gos_GetTerrainLightingV2Floor();
    if (ImGui::SliderFloat("Shadow fill floor##tlv2", &v2Floor, 0.0f, 1.0f, "%.2f")) {
        ::gos_SetTerrainLightingV2Floor(v2Floor);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hemi fill multiplier in fully shadowed terrain.\n"
                          "1.0 = V1 unmodulated (full hemi in shadows — too bright).\n"
                          "0.3 = default — 30%% hemi in shadows, 100%% in lit terrain.\n"
                          "0.0 = hemi follows shadow exactly (lifeless shadows).\n"
                          "Only effective when both MC2_TERRAIN_LIGHTING_V1=1 AND\n"
                          "MC2_TERRAIN_LIGHTING_V2=1 are set.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tlv2")) ::gos_SetTerrainLightingV2Floor(0.3f);
    if (!litV2GateOn) {
        ImGui::TextDisabled("(slider has no effect until MC2_TERRAIN_LIGHTING_V2=1)");
    } else if (!litGateOn) {
        // Reviewer-flagged UX gap: V2 ON but V1 OFF leaves the slider live
        // with no visible effect (V2 only modulates the V1 additive). Make
        // the combined-dependency explicit.
        ImGui::TextDisabled("(V2 floor only acts on the V1 hemi term — set MC2_TERRAIN_LIGHTING_V1=1 too)");
    }
    ImGui::TextDisabled("Debug Mode 10 = height-normal RGB; Mode 11 = hemi additive ×4");

    // CLIFF SHADOW FLOOR: lifts shadow-side steep terrain faces off near-black.
    {
        const char* cliffEnv = std::getenv("MC2_TERRAIN_CLIFF_SHADOW_FLOOR");
        bool cliffGateOn = (cliffEnv && cliffEnv[0] && cliffEnv[0] != '0');
        if (cliffGateOn)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Cliff Shadow Floor: ON");
        else
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Cliff Shadow Floor: OFF (set MC2_TERRAIN_CLIFF_SHADOW_FLOOR=<0..0.6>)");
        float cliffFloor = ::gos_GetTerrainCliffShadowFloor();
        if (ImGui::SliderFloat("Cliff Shadow Floor", &cliffFloor, 0.0f, 0.6f, "%.2f")) {
            ::gos_SetTerrainCliffShadowFloor(cliffFloor);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Minimum shadow value on steep terrain cliff faces.\n"
                              "0.0 = byte-identical (steep faces can go near-black).\n"
                              "0.3 = lift shadow-side cliffs to 30%% min light.\n"
                              "Only effective when MC2_TERRAIN_CLIFF_SHADOW_FLOOR is set.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##cliffsf")) ::gos_SetTerrainCliffShadowFloor(0.0f);
        if (!cliffGateOn)
            ImGui::TextDisabled("(slider has no effect until MC2_TERRAIN_CLIFF_SHADOW_FLOOR is set)");
    }

    // MC2_DYNAMIC_DECALS: live ring-buffer status (gate-off shows 0/64 inactive).
    {
        const char* ddEnv = std::getenv("MC2_DYNAMIC_DECALS");
        bool ddOn = (ddEnv && ddEnv[0] != '0');
        int live = DynDecal::liveCount();
        if (ddOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "DynDecals: %d/%d active (MC2_DYNAMIC_DECALS=1)", live, DynDecal::kCapacity);
        } else {
            ImGui::TextDisabled("DynDecals: off (set MC2_DYNAMIC_DECALS=1 to enable impact rings)");
        }
    }

    // ── Material color classifier ──────────────────────────────────────────────
    // TERRAIN-CLASSIFY-TUNING-1: tune the HSV thresholds that map colormap pixels
    // to rock / grass / dirt. Saved to visual_tuning.json via "Set as Mission Defaults".
    // Debug Mode 4 = Material Weights (R=rock  G=grass  B=dirt) to visualise effect.
    ImGui::SeparatorText("Material Color Classifier");
    ImGui::TextDisabled("HSV thresholds: colormap pixel → rock / grass / dirt");

    static float s_grassGmRLo = -0.02f, s_grassGmRHi = 0.06f;
    static float s_grassBrLo  =  0.22f, s_grassBrHi  = 0.40f;
    static float s_dirtRmGLo  = -0.02f, s_dirtRmGHi  = 0.06f;
    static float s_dirtBrLo   =  0.22f, s_dirtBrHi   = 0.45f;
    static bool  s_classInited = false;
    if (!s_classInited) {
        gos_GetTerrainClassGrass(&s_grassGmRLo, &s_grassGmRHi, &s_grassBrLo, &s_grassBrHi);
        gos_GetTerrainClassDirt(&s_dirtRmGLo, &s_dirtRmGHi, &s_dirtBrLo, &s_dirtBrHi);
        s_classInited = true;
    }

    // Grass swatch: approximate green from mid-brightness
    {
        float gc = (s_grassBrLo + s_grassBrHi) * 0.5f;
        ImGui::ColorButton("##gc", ImVec4(gc * 0.70f, gc * 1.10f, gc * 0.50f, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();
        ImGui::TextUnformatted("Grass  (G-R delta → green tilt)");
    }
    bool grassChanged = false;
    grassChanged |= ImGui::SliderFloat("G-R lo##gc", &s_grassGmRLo, -0.3f, 0.3f, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("G minus R lower bound: -0.02 = slight warm still ok");
    grassChanged |= ImGui::SliderFloat("G-R hi##gc", &s_grassGmRHi, -0.3f, 0.3f, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("G minus R upper bound: 0.06 = clearly green");
    grassChanged |= ImGui::SliderFloat("G bright lo##gc", &s_grassBrLo, 0.0f, 1.0f, "%.3f");
    grassChanged |= ImGui::SliderFloat("G bright hi##gc", &s_grassBrHi, 0.0f, 1.0f, "%.3f");
    if (grassChanged)
        gos_SetTerrainClassGrass(s_grassGmRLo, s_grassGmRHi, s_grassBrLo, s_grassBrHi);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##gc")) {
        s_grassGmRLo = -0.02f; s_grassGmRHi = 0.06f; s_grassBrLo = 0.22f; s_grassBrHi = 0.40f;
        gos_SetTerrainClassGrass(-0.02f, 0.06f, 0.22f, 0.40f);
    }

    ImGui::Spacing();

    // Dirt swatch: approximate warm brown from mid-brightness
    {
        float rc = (s_dirtBrLo + s_dirtBrHi) * 0.5f;
        ImGui::ColorButton("##dc", ImVec4(rc * 1.15f, rc * 0.88f, rc * 0.65f, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();
        ImGui::TextUnformatted("Dirt  (R-G delta → warm tilt)");
    }
    bool dirtChanged = false;
    dirtChanged |= ImGui::SliderFloat("R-G lo##dc", &s_dirtRmGLo, -0.3f, 0.3f, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("R minus G lower bound: -0.02 = slight cool still ok");
    dirtChanged |= ImGui::SliderFloat("R-G hi##dc", &s_dirtRmGHi, -0.3f, 0.3f, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("R minus G upper bound. Sand_M24: ~0.12 for washed-out sand");
    dirtChanged |= ImGui::SliderFloat("R bright lo##dc", &s_dirtBrLo, 0.0f, 1.0f, "%.3f");
    dirtChanged |= ImGui::SliderFloat("R bright hi##dc", &s_dirtBrHi, 0.0f, 1.0f, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sand_M24: raise to ~0.80 for bright sun-lit sand");
    if (dirtChanged)
        gos_SetTerrainClassDirt(s_dirtRmGLo, s_dirtRmGHi, s_dirtBrLo, s_dirtBrHi);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##dc")) {
        s_dirtRmGLo = -0.02f; s_dirtRmGHi = 0.06f; s_dirtBrLo = 0.22f; s_dirtBrHi = 0.45f;
        gos_SetTerrainClassDirt(-0.02f, 0.06f, 0.22f, 0.45f);
    }

    ImGui::TextDisabled("Save via Visual Tuning Profile > Set as Mission Defaults");
    ImGui::TextDisabled("Debug Mode 4 = Material Weights (R=rock  G=grass  B=dirt)");
}

// ─────────────────────────────────────────────────────────────────────────────

// Static-prop visual tuning. Previously lived in the Object Inspector PBR
// block (selection-driven); consolidated here so the controls are reachable
// without picking a prop. Env gates remain authoritative — sliders only
// modulate strength when the matching env gate is ON.
static void drawStaticPropTuningSection() {
    // IBL SH ambient (V-IBL-STATIC-1; default-ON env). Slider modulates
    // strength when MC2_STATIC_PROP_IBL_SH != 0 (default-on).
    ImGui::SeparatorText("IBL SH ambient");
    {
        const char* env = std::getenv("MC2_STATIC_PROP_IBL_SH");
        bool iblOn = !(env != nullptr && env[0] == '0');
        const char* setName = ibl_sh_runtime_currentSetName();
        if (iblOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "IBL SH: ON (set=%s, strength=%.2f)",
                setName ? setName : "default", g_iblShStrength);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "IBL SH: OFF (env-gated; MC2_STATIC_PROP_IBL_SH=0)");
        }
        ImGui::BeginDisabled(!iblOn);
        if (ImGui::SliderFloat("Strength##ibl_sh", &g_iblShStrength, 0.0f, 3.0f, "%.2f")) {}
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("V-IBL-STATIC-1 SH-L2 image-based ambient on the\n"
                              "StaticPropOpaque lane. Slider modulates strength\n"
                              "when env gate is on. Default 0.5.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##ibl_sh")) g_iblShStrength = 0.5f;
        ImGui::EndDisabled();
    }

    // PBR V1 specular (V-MATERIAL-PBR-3; default-OFF env). When gate ON,
    // slider controls strength. Default value 0.5 (dialled back from 1.0).
    ImGui::SeparatorText("PBR V1 specular");
    {
        const char* env = std::getenv("MC2_STATIC_PROP_PBR_V1");
        bool pbrOn = (env && env[0] && env[0] != '0');
        if (pbrOn) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "PBR V1: ON (strength=%.2f)", g_pbrV1Strength);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "PBR V1: OFF (set MC2_STATIC_PROP_PBR_V1=1)");
        }
        ImGui::BeginDisabled(!pbrOn);
        if (ImGui::SliderFloat("Strength##pbr_v1", &g_pbrV1Strength, 0.0f, 3.0f, "%.2f")) {}
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("V-MATERIAL-PBR-3 per-fragment Schlick-Fresnel +\n"
                              "power-lobe specular in static_prop.frag.\n"
                              "Default 0.5 (was 1.0 — full strength was too blunt\n"
                              "on flat-roofed legacy assets without material masks).\n"
                              "Env MC2_STATIC_PROP_PBR_V1_STRENGTH overrides startup.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbr_v1")) g_pbrV1Strength = 0.5f;
        ImGui::EndDisabled();
    }

    // PBR roughness override (V-MATERIAL-PBR-3-TUNE-UI). Forces roughness
    // to a fixed value across all materials when enabled. Default
    // enabled=true / value=0.95 (was disabled/0.6) — the 0.6 literal made
    // legacy assets too glossy.
    ImGui::SeparatorText("PBR roughness override");
    {
        const char* env = std::getenv("MC2_STATIC_PROP_PBR_V1");
        bool pbrOn = (env && env[0] && env[0] != '0');
        if (pbrOn) {
            ImGui::TextColored(g_pbrV1RoughnessOverrideEnabled
                                   ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                   : ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Override: %s (value=%.2f)",
                g_pbrV1RoughnessOverrideEnabled ? "ON" : "OFF",
                g_pbrV1RoughnessOverrideValue);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                "Override: inert (PBR V1 env gate OFF)");
        }
        ImGui::BeginDisabled(!pbrOn);
        ImGui::Checkbox("Enable##pbr_rough", &g_pbrV1RoughnessOverrideEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, overrides static_prop.vert's roughness\n"
                              "literal/MaterialGpu value with the slider value.\n"
                              "Default-ON at 0.95 (was OFF / 0.6 literal — too glossy).");
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_pbrV1RoughnessOverrideEnabled);
        if (ImGui::SliderFloat("Value##pbr_rough", &g_pbrV1RoughnessOverrideValue,
                               0.05f, 1.0f, "%.2f")) {}
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbr_rough")) {
            g_pbrV1RoughnessOverrideEnabled = true;
            g_pbrV1RoughnessOverrideValue   = 0.95f;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WATER-TUNING-UI-1 / WATER-DEBUG-VIEWS-1: MDI water FS debug view + material
// tuning. ONLY the GPU-driven/MDI water path (gos_terrain_water_mdi.frag)
// consumes these — arm it with MC2_GPU_DRIVEN_WATER=1; the legacy sin-wave FS
// ignores them. Defaults match the former shader consts EXACTLY; Reset buttons
// restore them (so the default render stays byte-identical).
static void drawWaterSection() {
    ImGui::SeparatorText("Debug View (MDI path)");
    {
        const char* kWaterModes[] = {
            "0: Final", "1: Tint", "2: Alpha", "3: Normal",
            "4: Depth", "5: Shore", "6: Lighting", "7: Refl SH sky",
            "8: Refl RT sample", "9: Refl blend"
        };
        int mode = gos_GetWaterFsDebugMode();
        if (mode < 0 || mode > 9) mode = 0;
        if (ImGui::Combo("Debug mode##wat", &mode, kWaterModes, IM_ARRAYSIZE(kWaterModes)))
            gos_SetWaterFsDebugMode(mode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fragment/material-space debug for gos_terrain_water_mdi.frag.\n"
                              "Requires the MDI water path (MC2_GPU_DRIVEN_WATER=1); the legacy\n"
                              "sin-wave water FS ignores this. Mirrors env MC2_WATER_DEBUG_MODE.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watdbg")) gos_SetWaterFsDebugMode(0);
    }

    ImGui::SeparatorText("Material (MDI path)");
    {
        float absd = gos_GetWaterAbsorptionDensity();
        if (ImGui::SliderFloat("Absorption density##wat", &absd, 0.0f, 0.10f, "%.4f"))
            gos_SetWaterAbsorptionDensity(absd);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Beer-Lambert k (1/world-units). Higher = light absorbed faster -> deep color sooner.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watabs")) gos_SetWaterAbsorptionDensity(0.022f);

        float maxA = gos_GetWaterMaxAlpha();
        if (ImGui::SliderFloat("Max alpha##wat", &maxA, 0.0f, 1.0f, "%.3f"))
            gos_SetWaterMaxAlpha(maxA);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Opacity cap for deep water. 1.0 = opaque slab; lower = lakebed shows through.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watmaxa")) gos_SetWaterMaxAlpha(0.87f);

        float rip = gos_GetWaterRippleGain();
        if (ImGui::SliderFloat("Ripple gain##wat", &rip, 0.0f, 1.0f, "%.3f"))
            gos_SetWaterRippleGain(rip);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("fBm crest BRIGHTEN amount (camera-independent).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watrip")) gos_SetWaterRippleGain(0.22f);

        float glint = gos_GetWaterGlintGain();
        if (ImGui::SliderFloat("Glint gain##wat", &glint, 0.0f, 1.0f, "%.3f"))
            gos_SetWaterGlintGain(glint);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Additive white crest shimmer (camera-independent).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watglint")) gos_SetWaterGlintGain(0.30f);

        float deep[3];    gos_GetWaterDeepColor(deep);
        if (ImGui::ColorEdit3("Deep color##wat", deep))
            gos_SetWaterDeepColor(deep[0], deep[1], deep[2]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watdeep")) gos_SetWaterDeepColor(0.03f, 0.13f, 0.20f);

        float shallow[3]; gos_GetWaterShallowColor(shallow);
        if (ImGui::ColorEdit3("Shallow color##wat", shallow))
            gos_SetWaterShallowColor(shallow[0], shallow[1], shallow[2]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watshal")) gos_SetWaterShallowColor(0.22f, 0.45f, 0.38f);
    }

    ImGui::SeparatorText("Sky Tint (gated, camera-independent)");
    {
        const char* env = std::getenv("MC2_WATER_SKYTINT");
        bool gateOn = (env && env[0] && env[0] != '0');
        if (gateOn) ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "MC2_WATER_SKYTINT: ON (default strength 0.15)");
        else        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "MC2_WATER_SKYTINT: off (strength 0 = no-op; slider still works)");

        float strength = gos_GetWaterSkyTintStrength();
        if (ImGui::SliderFloat("Sky tint strength##wat", &strength, 0.0f, 1.0f, "%.3f"))
            gos_SetWaterSkyTintStrength(strength);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Camera-INDEPENDENT additive pull of water color toward the tint color.\n"
                              "0 = exact no-op (byte-identical). NOT fresnel/reflection.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watskystr")) gos_SetWaterSkyTintStrength(0.0f);

        float sky[3]; gos_GetWaterSkyTintColor(sky);
        if (ImGui::ColorEdit3("Sky tint color##wat", sky))
            gos_SetWaterSkyTintColor(sky[0], sky[1], sky[2]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watskycol")) gos_SetWaterSkyTintColor(0.55f, 0.70f, 0.85f);
    }

    ImGui::SeparatorText("Sky Reflection (gated, camera-dependent)");
    {
        // Env is the HARD gate: when OFF, the slider is disabled so it cannot
        // bypass the gate (unlike sky tint). Enable MC2_WATER_REFLECTION=1.
        bool gateOn = (gos_GetWaterReflectionGate() != 0);
        if (gateOn) ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "MC2_WATER_REFLECTION: ON (default strength 0.15)");
        else        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "MC2_WATER_REFLECTION: off (set =1 to enable; slider disabled)");

        if (!gateOn) ImGui::BeginDisabled();
        float refl = gos_GetWaterReflStrength();
        if (ImGui::SliderFloat("Reflection strength##wat", &refl, 0.0f, 0.5f, "%.3f"))
            gos_SetWaterReflStrength(refl);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("SH-L2 sky reflection (camera-dependent). Suggested 0.10-0.25.\n"
                              "Fresnel/grazing-weighted, capped. Debug mode 7 shows the sky term.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watrefl")) gos_SetWaterReflStrength(gateOn ? 0.15f : 0.0f);
        if (!gateOn) ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Terrain Reflection RT (gated; blends over sky)");
    {
        // Env is the HARD gate (also drives the C1 RT fill pass); slider disabled
        // when OFF so it cannot bypass it. Needs BOTH this + Sky Reflection ON to
        // contribute (the RT modulates the reflected COLOR; Sky Reflection drives
        // the reflection mix factor). ~0% coverage at the steep camera -> falls
        // back to SH sky (water never goes empty).
        bool rtGate = (gos_GetWaterReflectionRtGate() != 0);
        if (rtGate) ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "MC2_WATER_REFLECTION_RT: ON (default 0.85)");
        else        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "MC2_WATER_REFLECTION_RT: off (set =1; needs Sky Reflection ON too)");

        if (!rtGate) ImGui::BeginDisabled();
        float rt = gos_GetWaterRtStrength();
        if (ImGui::SliderFloat("RT blend strength##wat", &rt, 0.0f, 1.0f, "%.3f"))
            gos_SetWaterRtStrength(rt);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How strongly valid terrain-RT pixels replace the SH sky in the\n"
                              "reflection. Debug mode 8 = RT sample, 9 = final reflection blend.\n"
                              "Marginal at the steep gameplay camera (terrain off-frustum).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##watrt")) gos_SetWaterRtStrength(rtGate ? 0.85f : 0.0f);
        if (!rtGate) ImGui::EndDisabled();
    }

    if (ImGui::SmallButton("Reset ALL water defaults##wat")) {
        gos_SetWaterFsDebugMode(0);
        gos_SetWaterAbsorptionDensity(0.022f);
        gos_SetWaterMaxAlpha(0.87f);
        gos_SetWaterRippleGain(0.22f);
        gos_SetWaterGlintGain(0.30f);
        gos_SetWaterDeepColor(0.03f, 0.13f, 0.20f);
        gos_SetWaterShallowColor(0.22f, 0.45f, 0.38f);
        gos_SetWaterSkyTintStrength(0.0f);
        gos_SetWaterSkyTintColor(0.55f, 0.70f, 0.85f);
        // Reflection: reset to the gate-appropriate default (0 when gate OFF).
        gos_SetWaterReflStrength(gos_GetWaterReflectionGate() ? 0.15f : 0.0f);
        gos_SetWaterRtStrength(gos_GetWaterReflectionRtGate() ? 0.85f : 0.0f);
    }
    ImGui::TextDisabled("MDI path only (MC2_GPU_DRIVEN_WATER=1). Defaults = byte-identical.");
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawDrawPacketsSection() {
    // ── Frame health ──────────────────────────────────────────────────────────
    const bool anyError = (g_dpSnapshot.emitted != g_dpSnapshot.expected
                           || g_dpSnapshot.invalidRanges > 0
                           || g_dpSnapshot.overflow);
    if (anyError && g_dpSnapshot.frame > 0)
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "! packet integrity error");

    char buf[64];

    // Top table: frame + emitted vs expected
    if (ImGui::BeginTable("##dphealth", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##dk",  ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##dv",  ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Frame");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)g_dpSnapshot.frame);
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Emitted");
        ImGui::TableSetColumnIndex(1);
        {
            const bool ok = (g_dpSnapshot.emitted == g_dpSnapshot.expected);
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.emitted);
            ImGui::TextColored(ok ? ImVec4(0.3f,1.f,0.3f,1.f)
                                  : ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Expected");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.expected);
        ImGui::TextUnformatted(buf);

        ImGui::EndTable();
    }

    ImGui::Separator();

    // Bottom table: diagnostic counters
    if (ImGui::BeginTable("##dpdiag", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##ddk", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##ddv", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Distinct types");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.distinctTypes);
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Invalid ranges");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.invalidRanges > 0) {
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.invalidRanges);
            ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
        } else {
            ImGui::TextUnformatted("0");
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Overflow");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.overflow)
            ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "YES");
        else
            ImGui::TextDisabled("no");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Skipped ranges");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.skippedRanges);
        ImGui::TextDisabled("%s", buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Mat mismatches");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.materialMismatches > 0) {
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.materialMismatches);
            ImGui::TextColored(ImVec4(1.f,0.8f,0.3f,1.f), "%s", buf);
        } else {
            ImGui::TextDisabled("0");
        }

        ImGui::EndTable();
    }

    // ── Type Inspector ────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Type Inspector");

    const uint32_t typeCount = g_dpSnapshot.typeDescCount;
    if (typeCount == 0) {
        ImGui::TextDisabled("(geometry not finalized — load a mission)");
        return;
    }

    static int s_selTypeId = 0;
    if (s_selTypeId >= (int)typeCount) s_selTypeId = 0;

    ImGui::SetNextItemWidth(110.f);
    ImGui::InputInt("TypeId##dp", &s_selTypeId, 1, 10);
    if (s_selTypeId < 0)             s_selTypeId = 0;
    if (s_selTypeId >= (int)typeCount) s_selTypeId = (int)typeCount - 1;
    ImGui::SameLine();
    ImGui::TextDisabled("/ %u", typeCount - 1u);

    RenderCore::StaticPropTypeDesc desc{};
    const bool descOk = batcher_getStaticPropTypeDesc((uint32_t)s_selTypeId, &desc);

    if (descOk) {
        if (ImGui::BeginTable("##dptype", 2,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("##dtk", ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("##dtv", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("firstPacket");
            ImGui::TableSetColumnIndex(1);
            snprintf(buf, sizeof(buf), "%u", desc.firstPacket);
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
            ImGui::TableSetColumnIndex(1);
            snprintf(buf, sizeof(buf), "%u", desc.packetCount);
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
            ImGui::TableSetColumnIndex(1);
            switch (desc.alphaClass) {
                case 0:  ImGui::TextUnformatted("OPAQUE");     break;
                case 1:  ImGui::TextUnformatted("ALPHA_TEST"); break;
                default:
                    snprintf(buf, sizeof(buf), "?(%u)", desc.alphaClass);
                    ImGui::TextColored(ImVec4(1.f,0.8f,0.3f,1.f), "%s", buf);
                    break;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("materialIdx");
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("per-instance");

            ImGui::EndTable();
        }

        if (ImGui::SmallButton("Copy CSV##dp")) {
            const char* alphaStr = desc.alphaClass == 0 ? "OPAQUE"
                                 : desc.alphaClass == 1 ? "ALPHA_TEST" : "?";
            snprintf(buf, sizeof(buf),
                "typeId,firstPacket,packetCount,alphaClass\n%u,%u,%u,%s",
                (uint32_t)s_selTypeId, desc.firstPacket, desc.packetCount, alphaStr);
            ImGui::SetClipboardText(buf);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy type descriptor row as CSV.");
    } else {
        ImGui::TextDisabled("(typeId %d out of range)", s_selTypeId);
    }

    // ── Packet range minimap ──────────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Packet Range Minimap##dp")) {
        const uint32_t total  = g_dpSnapshot.expected > 0u ? g_dpSnapshot.expected : 1u;
        const float    availW = ImGui::GetContentRegionAvail().x;
        const float    barH   = 14.f;
        const ImVec2   p0     = ImGui::GetCursorScreenPos();
        ImDrawList*    dl     = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(p0, ImVec2(p0.x + availW, p0.y + barH),
                          IM_COL32(45, 45, 45, 255));

        // Selected type range in yellow
        if (descOk && desc.packetCount > 0u) {
            const float x0 = p0.x + availW * ((float)desc.firstPacket / (float)total);
            float       x1 = p0.x + availW * ((float)(desc.firstPacket + desc.packetCount) / (float)total);
            if (x1 < x0 + 2.f) x1 = x0 + 2.f;
            dl->AddRectFilled(ImVec2(x0, p0.y + 2.f), ImVec2(x1, p0.y + barH - 2.f),
                              IM_COL32(255, 220, 50, 200));
        }

        // Border
        dl->AddRect(p0, ImVec2(p0.x + availW, p0.y + barH),
                    IM_COL32(110, 110, 110, 255));
        ImGui::Dummy(ImVec2(availW, barH));

        snprintf(buf, sizeof(buf), "type %d: [%u, %u) of %u total",
            s_selTypeId,
            descOk ? desc.firstPacket : 0u,
            descOk ? (desc.firstPacket + desc.packetCount) : 0u,
            total);
        ImGui::TextDisabled("%s", buf);
    }

    // ── Selected Prop Packet Inspector ────────────────────────────────────────
    // Populated each frame by gameosmain after emitStaticPropDrawPackets.
    // Selection: Ctrl+Shift+Click a static prop in-game (Object Inspector).
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Selected Prop##dp")) {
        const DrawPacketSelectedPropSnapshot& sel = g_dpSelProp;

        if (!sel.valid) {
            ImGui::TextDisabled("(no static prop selected)");
            ImGui::TextDisabled("Ctrl+Shift+Click a prop in-game to inspect.");
        } else {
            // Header line: shape | typeId | recipe
            const char* sname = sel.shapeName[0] ? sel.shapeName : "(unnamed)";
            ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.f),
                "%s", sname);
            ImGui::SameLine();
            ImGui::TextDisabled("| typeId: %u | recipe: %d",
                sel.typeId, sel.recipeIndex);

            // Summary row: instances + materialIdx + alphaClass
            if (ImGui::BeginTable("##selpropsummary", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("##spsk", ImGuiTableColumnFlags_WidthFixed, 130.f);
                ImGui::TableSetupColumn("##spsv", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Instances (vis)");
                ImGui::TableSetColumnIndex(1);
                snprintf(buf, sizeof(buf), "%u", sel.instanceCount);
                ImGui::TextUnformatted(buf);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("materialIdx");
                ImGui::TableSetColumnIndex(1);
                if (sel.materialIdx == 0xFFFFFFFFu)
                    ImGui::TextDisabled("(sentinel)");
                else {
                    snprintf(buf, sizeof(buf), "%u", sel.materialIdx);
                    ImGui::TextUnformatted(buf);
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
                ImGui::TableSetColumnIndex(1);
                switch (sel.alphaClass) {
                    case 0:  ImGui::TextUnformatted("OPAQUE");      break;
                    case 1:  ImGui::TextUnformatted("ALPHA_TEST");  break;
                    default:
                        snprintf(buf, sizeof(buf), "?(%u)", sel.alphaClass);
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                        break;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
                ImGui::TableSetColumnIndex(1);
                snprintf(buf, sizeof(buf), "%u", sel.packetCount);
                ImGui::TextUnformatted(buf);

                ImGui::EndTable();
            }

            // Per-packet table
            if (sel.rowCount > 0) {
                ImGui::Spacing();
                ImGui::TextUnformatted("Packets:");
                if (ImGui::BeginTable("##selproppkts", 7,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                        | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("pktIdx",    ImGuiTableColumnFlags_WidthFixed, 48.f);
                    ImGui::TableSetupColumn("firstIdx",  ImGuiTableColumnFlags_WidthFixed, 64.f);
                    ImGui::TableSetupColumn("idxCount",  ImGuiTableColumnFlags_WidthFixed, 64.f);
                    ImGui::TableSetupColumn("baseVtx",   ImGuiTableColumnFlags_WidthFixed, 56.f);
                    ImGui::TableSetupColumn("pipeline",  ImGuiTableColumnFlags_WidthFixed, 74.f);
                    ImGui::TableSetupColumn("matFlags",  ImGuiTableColumnFlags_WidthFixed, 62.f);
                    ImGui::TableSetupColumn("baseInst",  ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (uint32_t r = 0; r < sel.rowCount; ++r) {
                        const DrawPacketPropRow& row = sel.rows[r];
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        snprintf(buf, sizeof(buf), "%u", row.globalPacketIdx);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(1);
                        snprintf(buf, sizeof(buf), "%u", row.firstIndex);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(2);
                        snprintf(buf, sizeof(buf), "%u", row.indexCount);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(3);
                        snprintf(buf, sizeof(buf), "%d", row.baseVertex);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(4);
                        switch (row.pipelineId) {
                            case 0:  ImGui::TextDisabled("invalid");      break;
                            case 1:  ImGui::TextUnformatted("opaque");    break;
                            case 2:  ImGui::TextUnformatted("alpha_test"); break;
                            default:
                                snprintf(buf, sizeof(buf), "?(%u)", row.pipelineId);
                                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                                break;
                        }

                        ImGui::TableSetColumnIndex(5);
                        snprintf(buf, sizeof(buf), "0x%02X", row.materialFlags);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(6);
                        ImGui::TextDisabled("--");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("baseInstance populated at dispatch (v1+)");
                    }

                    ImGui::EndTable();
                }
            }

            if (ImGui::SmallButton("Copy CSV##selp")) {
                // Header
                int n = snprintf(buf, sizeof(buf),
                    "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                ImGui::SetClipboardText(buf);
                // Rows: build into a larger buffer
                char cbuf[1024]; int cn = 0;
                cn += snprintf(cbuf+cn, sizeof(cbuf)-cn,
                    "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                for (uint32_t r = 0; r < sel.rowCount && cn < (int)sizeof(cbuf)-80; ++r) {
                    const DrawPacketPropRow& row = sel.rows[r];
                    const char* plName = row.pipelineId == 1 ? "opaque"
                                       : row.pipelineId == 2 ? "alpha_test" : "invalid";
                    cn += snprintf(cbuf+cn, sizeof(cbuf)-cn,
                        "%u,%u,%u,%d,%s,0x%02X\n",
                        row.globalPacketIdx, row.firstIndex, row.indexCount,
                        row.baseVertex, plName, row.materialFlags);
                }
                ImGui::SetClipboardText(cbuf);
                (void)n;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawRenderPassesSection() {
    gosPostProcess* pp = getGosPostProcess();

    // ── Shadow pass ───────────────────────────────────────────────────────────
    if (pp) {
        ImGui::Checkbox("Shadow Pass##rp", &pp->shadowsEnabled_);
        ImGui::SameLine();
        ImGui::TextDisabled("(also in Post-Process)");
    }

    // ── Cloud shadows (fullscreen procedural pass) ────────────────────────────
    if (pp && ImGui::CollapsingHeader("Cloud Shadows##rp")) {
        bool cloudOn = pp->getCloudShadowEnabled();
        if (ImGui::Checkbox("Enable##cloud", &cloudOn))
            pp->setCloudShadowEnabled(cloudOn);

        float strength = pp->getCloudStrength();
        if (ImGui::SliderFloat("Strength##cloud", &strength, 0.0f, 0.6f, "%.3f"))
            pp->setCloudStrength(strength);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Max darkening amplitude. 0=clear, 0.15=default.");

        float scale = pp->getCloudScale();
        if (ImGui::SliderFloat("Cloud Scale##cloud", &scale, 0.0001f, 0.003f, "%.4f"))
            pp->setCloudScale(scale);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World-XY frequency. Smaller = bigger clouds. Default 0.0006.");

        float scroll[2] = { pp->getCloudScrollX(), pp->getCloudScrollY() };
        if (ImGui::SliderFloat2("Scroll Speed##cloud", scroll, -0.05f, 0.05f, "%.4f")) {
            pp->setCloudScrollX(scroll[0]);
            pp->setCloudScrollY(scroll[1]);
        }

        float thresh[2] = { pp->getCloudThreshLo(), pp->getCloudThreshHi() };
        if (ImGui::SliderFloat2("Coverage (lo/hi)##cloud", thresh, 0.0f, 1.0f, "%.3f")) {
            pp->setCloudThreshLo(thresh[0]);
            pp->setCloudThreshHi(thresh[1]);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smoothstep band on the FBM noise. Default 0.3 / 0.7.");

        int octaves = pp->getCloudOctaves();
        if (ImGui::SliderInt("Octaves##cloud", &octaves, 1, 6))
            pp->setCloudOctaves(octaves);

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##cloud")) {
            pp->setCloudShadowEnabled(true);
            pp->setCloudStrength(0.15f);
            pp->setCloudScale(0.0006f);
            pp->setCloudScrollX(0.012f);
            pp->setCloudScrollY(0.005f);
            pp->setCloudThreshLo(0.3f);
            pp->setCloudThreshHi(0.7f);
            pp->setCloudOctaves(4);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("GPU Geometry Passes");

    // Draw Mechs: hides mechs entirely (gates both GPU submit and CPU fallback).
    ImGui::Checkbox("Draw Mechs##rp", &g_drawMechs);
    ImGui::SameLine();
    // GPU Mechs: switches between GPU batcher and CPU legacy path (mechs still visible).
    ImGui::Checkbox("GPU path##rp", &g_useGpuMechs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("GPU Mechs ON = GPU batcher (PBR lit).\n"
                          "GPU Mechs OFF = CPU legacy path (older shading, dimmer).\n"
                          "Use 'Draw Mechs' to hide mechs entirely.");

    // GPU objects (full GPU-driven batcher)
    if (g_useGpuObjects) {
        ImGui::Checkbox("GPU Objects##rp",  &g_useGpuObjects);
    } else {
        ImGui::Checkbox("GPU Objects##rp",  &g_useGpuObjects);
        ImGui::SameLine();
        // Legacy static props only active when GPU Objects batcher is off
        ImGui::Checkbox("GPU Static Props##rp", &g_useGpuStaticProps);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("FX / Particles");

    bool fxDisabled = (getenv("MC2_DISABLE_GOSFX") != nullptr);
    if (fxDisabled) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "[off]  FX/Particles");
        ImGui::SameLine();
        ImGui::TextDisabled("MC2_DISABLE_GOSFX set — requires restart to re-enable");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[ON]   FX/Particles");
        ImGui::SameLine();
        ImGui::TextDisabled("(set MC2_DISABLE_GOSFX=1 to disable)");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Static Props Debug Mode");

    static const char* kPropModes[] = {
        "0: Normal",
        "1: Addr-gradient",
        "2: Addr-hash",
        "3: WHITE",
        "4: ARGB only",
        "5: TEX only",
        "6: HIGHLIGHT only",
        "7: TEX+HIGHLIGHT",
    };
    int target = gos_GpuPropsGetDebugMode();
    if (ImGui::Combo("##propdbg", &target, kPropModes, IM_ARRAYSIZE(kPropModes))) {
        int cur   = gos_GpuPropsGetDebugMode();
        int steps = (target - cur + 8) % 8;
        for (int i = 0; i < steps; ++i)
            gos_GpuPropsCycleDebugMode();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawGBufferPreview() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) {
        ImGui::TextDisabled("(post-process system unavailable)");
        return;
    }

    // Compute thumbnail width from available content region.
    // Aspect ratio: fixed 16:9 approximation.
    const float availW = ImGui::GetContentRegionAvail().x;
    const float thumbW = (availW * 0.5f) - ImGui::GetStyle().ItemSpacing.x;
    const float thumbH = thumbW * (9.0f / 16.0f);

    // UV flip: OpenGL textures have (0,0) at bottom-left; ImGui expects top-left.
    const ImVec2 uv0(0.0f, 1.0f);
    const ImVec2 uv1(1.0f, 0.0f);
    const ImVec2 sz(thumbW, thumbH);

    // ── Scene Color (RGBA16F HDR) ─────────────────────────────────────────────
    {
        unsigned int tex = pp->getSceneColorTexture();
        if (tex) {
            ImGui::TextUnformatted("Scene Color  (RGBA16F, HDR)");
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("HDR scene output before tone-mapping.\nValues above 1.0 are bloom sources.");
        } else {
            ImGui::TextDisabled("Scene Color: not allocated");
        }
    }

    ImGui::SameLine();

    // ── Scene Normals (RGBA16F world-space + shadow-skip) ─────────────────────
    {
        unsigned int tex = pp->getSceneNormalTexture();
        if (tex) {
            ImGui::TextUnformatted("Scene Normals  (RGBA16F)");
            // Column-layout: label is already on the SameLine'd column.
            // SetCursorPosY to align label with opposite column.
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("World-space normals (RGB = XYZ mapped 0-1).\n"
                                  "Alpha = shadow-skip flag (0=skip, 1=apply).\n"
                                  "Clear sentinel: (0.5, 0.5, 1.0, 0.0) = flat-up.");
        } else {
            ImGui::TextDisabled("Scene Normals: not allocated");
        }
    }

    // ── Shadow maps ───────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextUnformatted("Shadow Maps  (DEPTH_COMPONENT24)");
    ImGui::TextDisabled("  Depth textures need GL_TEXTURE_COMPARE_MODE=GL_NONE to preview.");
    ImGui::TextDisabled("  Use the shadow debug overlay in Post-Process instead.");

    // ── Object-ID buffer ──────────────────────────────────────────────────────
    {
        unsigned int tex = pp->getSceneObjectIdTex();
        if (tex) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Object-ID Buffer  (R32UI)");
            ImGui::TextDisabled("  Uint texture — needs a hash-to-RGB visualization shader.");
            ImGui::TextDisabled("  Use Ctrl+Shift+I + click to inspect individual pixels.");
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("Object-ID buffer: not active (set MC2_OBJECT_ID_BUFFER=1)");
        }
    }

    // ── Water Reflection (1/4-res RGBA16F) ────────────────────────────────────
    // WATER-REFLECTION-RESOURCE-1: substrate target. Renders BLACK until Phase C
    // (WATER-TERRAIN-REFLECTION-1) renders sky+terrain into it.
    {
        unsigned int tex = pp->getWaterReflectionTexture();
        ImGui::Spacing();
        if (tex) {
            ImGui::Text("Water Reflection  (1/4-res RGBA16F, %dx%d)",
                        pp->getWaterReflectionWidth(), pp->getWaterReflectionHeight());
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Quarter-res water reflection target (WATER-REFLECTION-RESOURCE-1).\n"
                                  "BLACK until Phase C renders sky+terrain into it; substrate only.");
        } else {
            ImGui::TextDisabled("Water Reflection: not allocated");
        }
    }

    // ── HZB depth pyramid (HZB-DEBUG-PREVIEW-1) ───────────────────────────────
    // Diagnostic preview of the reverse-Z Hi-Z pyramid. Each level is its own
    // R32F texture, so ImGui::Image previews a chosen level directly. R32F shows
    // in the red channel: reverse-Z near=1.0 = bright red, far/sky=0.0 = black.
    {
        ImGui::Spacing();
        ImGui::Separator();
        if (!pp->isHzbEnabled()) {
            ImGui::TextDisabled("HZB pyramid: off (set MC2_HZB_BUILD=1 to build + preview)");
        } else {
            const int mips = pp->getHzbMipCount();
            ImGui::Text("HZB Pyramid  (R32F per-level, %dx%d, %d mips, builds=%llu)",
                        pp->getHzbWidth(), pp->getHzbHeight(), mips,
                        (unsigned long long)pp->getHzbBuildCount());
            static int s_hzbMip = 0;
            if (s_hzbMip >= mips) s_hzbMip = mips > 0 ? mips - 1 : 0;
            if (mips > 1)
                ImGui::SliderInt("HZB mip##hzb", &s_hzbMip, 0, mips - 1);

            // Level dimensions via the same ceil ladder the runtime uses.
            int lw = pp->getHzbWidth(), lh = pp->getHzbHeight();
            for (int k = 0; k < s_hzbMip; ++k) {
                lw = (lw + 1) / 2; if (lw < 1) lw = 1;
                lh = (lh + 1) / 2; if (lh < 1) lh = 1;
            }
            unsigned int tex = pp->getHzbLevelTexture(s_hzbMip);
            if (tex) {
                ImGui::Text("  level %d = %dx%d", s_hzbMip, lw, lh);
                ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("HZB level %d (%dx%d). R32F reverse-Z depth in red:\n"
                                      "near (closer) = bright, far/sky = black.\n"
                                      "Coarser levels store the MIN (farthest occluder).",
                                      s_hzbMip, lw, lh);
            } else {
                ImGui::TextDisabled("  HZB level texture not allocated");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

static void drawEnvGatesSection() {
    ImGui::TextDisabled("Baked at startup — changes require restart.");
    ImGui::Spacing();

    if (!ImGui::BeginTable("##envtbl", 3,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("##st",   ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("##var",  ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("##desc", ImGuiTableColumnFlags_WidthStretch);

    for (int i = 0; i < kEnvGateCount; ++i) {
        const EnvGate& g = kEnvGates[i];
        const char* val  = getenv(g.var);

        bool enabled;
        if (g.inverted) {
            // Present → feature disabled.  Absent → feature enabled.
            enabled = (val == nullptr);
        } else if (g.defaultOn) {
            enabled = (!val || val[0] != '0');
        } else {
            enabled = (val && val[0] != '0');
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (enabled)
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "[ON] ");
        else
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[off]");

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(g.var);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", g.desc);
    }

    ImGui::EndTable();
}

// ─────────────────────────────────────────────────────────────────────────────

// MECH-LIGHTING-UI-1: mech ambient/specular/PBR roughness controls.
static void drawMechSection() {
    // BT2018 imported-skin albedo correction (imported BT mechs ONLY; stock untouched).
    // Imported skins are sRGB-authored PBR albedo; the legacy lighting path samples
    // them un-linearized, so they read washed/flat. Gamma decodes sRGB->linear;
    // Scale brings dark skins back to legacy brightness parity.
    ImGui::SeparatorText("BT2018 Imported Skin");
    {
        float ig = batcher_getImportedMechGamma();
        if (ImGui::SliderFloat("Imported Albedo Gamma##mech", &ig, 1.0f, 2.4f, "%.2f"))
            batcher_setImportedMechGamma(ig);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("sRGB->linear exponent, IMPORTED BT mechs only.\n"
                              "2.2 = full decode (over-darkens dark skins in legacy\n"
                              "lighting); lower eases it. Stock mechs unaffected.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impgamma")) batcher_setImportedMechGamma(2.2f);

        float is = batcher_getImportedMechAlbedoScale();
        if (ImGui::SliderFloat("Imported Albedo Scale##mech", &is, 0.25f, 2.5f, "%.2f"))
            batcher_setImportedMechAlbedoScale(is);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Post-gamma brightness, IMPORTED BT mechs only.\n"
                              "Raise to pull dark skins toward legacy parity.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impscale")) batcher_setImportedMechAlbedoScale(1.0f);

        float ao = batcher_getImportedMechAoStrength();
        if (ImGui::SliderFloat("Imported AO Strength##mech", &ao, 0.0f, 1.0f, "%.2f"))
            batcher_setImportedMechAoStrength(ao);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ambient-occlusion multiply, IMPORTED BT mechs with AO only.\n"
                              "0 = no-op (pre-AO look); 1 = full. Default 0.5. Mechs without\n"
                              "an AO map are unaffected (white/no-op).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impao")) batcher_setImportedMechAoStrength(0.5f);

        float ns = batcher_getImportedMechNormalStrength();
        if (ImGui::SliderFloat("Imported Normal Strength##mech", &ns, 0.0f, 1.0f, "%.2f"))
            batcher_setImportedMechNormalStrength(ns);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tangent-space normal-map strength, IMPORTED BT mechs with a\n"
                              "normal map only. 0 = no-op (geometric normal / fallback-proven);\n"
                              "1 = full map. Default 1.0. Mechs without a normal map are\n"
                              "unaffected (v_normal fallback).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impnrm")) batcher_setImportedMechNormalStrength(1.0f);
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Mech Lighting");

    // Ambient
    bool ambOn = batcher_getMechAmbientEnabled() != 0;
    if (ImGui::Checkbox("Ambient##mech", &ambOn))
        batcher_setMechAmbientEnabled(ambOn ? 1 : 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hemisphere ambient fill on mech hull (legacy path).\n"
                          "No effect when StandardLit GGX is ON (replaced by IBL+back-fill).\n"
                          "Kill-switch: MC2_MECH_AMBIENT_V1=0.");
    ImGui::SameLine();
    float as = batcher_getMechAmbientStrength();
    if (ImGui::SliderFloat("Strength##mechamb", &as, 0.0f, 2.0f, "%.2f"))
        batcher_setMechAmbientStrength(as);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mechamb")) batcher_setMechAmbientStrength(0.15f);

    // Specular
    bool specOn = batcher_getMechSpecularEnabled() != 0;
    if (ImGui::Checkbox("Specular##mech", &specOn))
        batcher_setMechSpecularEnabled(specOn ? 1 : 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blinn specular sheen on mech hull (legacy path).\n"
                          "No effect when StandardLit GGX is ON (Blinn path bypassed).\n"
                          "Kill-switch: MC2_MECH_SPECULAR_V1=0.");
    ImGui::SameLine();
    float ss = batcher_getMechSpecularStrength();
    if (ImGui::SliderFloat("Strength##mechspec", &ss, 0.0f, 2.0f, "%.2f"))
        batcher_setMechSpecularStrength(ss);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mechspec")) batcher_setMechSpecularStrength(0.05f);

    ImGui::Spacing();
    ImGui::SeparatorText("PBR Material");

    float mr = batcher_getMechMetalRoughness();
    if (ImGui::SliderFloat("Metal roughness##mech", &mr, 0.04f, 1.0f, "%.2f"))
        batcher_setMechMetalRoughness(mr);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PBR roughness for armour. 0.04=mirror, 1.0=matte. Default: 0.85");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mechmr")) batcher_setMechMetalRoughness(0.85f);

    float gr = batcher_getMechGlassRoughness();
    if (ImGui::SliderFloat("Glass roughness##mech", &gr, 0.04f, 1.0f, "%.2f"))
        batcher_setMechGlassRoughness(gr);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PBR roughness for cockpit glass. Default: 0.25");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mechgr")) batcher_setMechGlassRoughness(0.25f);

    ImGui::TextDisabled("ambient/specular strength also settable via visual_tuning.json");

    ImGui::Spacing();
    ImGui::SeparatorText("StandardLit GGX (MC2_STANDARD_LIT_V1)");
    {
        bool slOn = batcher_getStandardLitEnabled() != 0;
        if (ImGui::Checkbox("StandardLit GGX##mech", &slOn))
            batcher_setStandardLitEnabled(slOn ? 1 : 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cook-Torrance GGX PBR on mechs. Default ON.\n"
                              "Kill-switch: MC2_STANDARD_LIT_V1=0.\n"
                              "Requires MC2_MECH_SURFACE_MATERIAL=metal061b.");

        float mi = batcher_getPbrMetallicInfluence();
        if (ImGui::SliderFloat("Metallic influence##pbr", &mi, 0.0f, 1.0f, "%.2f"))
            batcher_setPbrMetallicInfluence(mi);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scale ORM metallic channel. Low = painted dielectric look.\n"
                              "High metallic + no IBL = black armour. Default: 0.15");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbrmi")) batcher_setPbrMetallicInfluence(0.15f);

        float rMin = batcher_getPbrRoughnessMin();
        if (ImGui::SliderFloat("Roughness min##pbr", &rMin, 0.0f, 1.0f, "%.2f"))
            batcher_setPbrRoughnessMin(rMin);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Floor for ORM roughness. Prevents mirror-like armour.\n"
                              "Default: 0.45");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbrrmin")) batcher_setPbrRoughnessMin(0.45f);

        float rMax = batcher_getPbrRoughnessMax();
        if (ImGui::SliderFloat("Roughness max##pbr", &rMax, 0.0f, 1.0f, "%.2f"))
            batcher_setPbrRoughnessMax(rMax);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ceiling for ORM roughness. Prevents fully matte armour.\n"
                              "Default: 0.90");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbrrmax")) batcher_setPbrRoughnessMax(0.90f);

        float as = batcher_getPbrAmbientSpecularStrength();
        if (ImGui::SliderFloat("Ambient specular##pbr", &as, 0.0f, 2.0f, "%.2f"))
            batcher_setPbrAmbientSpecularStrength(as);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Specular env fill via IBL sky color for metals.\n"
                              "Prevents pure-metal armour going black. Default: 0.25");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbras")) batcher_setPbrAmbientSpecularStrength(0.25f);

        // PBR-IBL-MECH-1: IBL SH ambient status.
        // Strength slider lives in Static Prop Tuning (shared g_iblShStrength).
        {
            const char* mechIblEnv = std::getenv("MC2_MECH_IBL_SH");
            bool mechIblOn = !(mechIblEnv != nullptr && mechIblEnv[0] == '0');
            if (mechIblOn)
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    "IBL SH ambient: ON (strength=%.2f from Static Prop Tuning)", g_iblShStrength);
            else
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                    "IBL SH ambient: OFF (MC2_MECH_IBL_SH=0)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("PBR-IBL-MECH-1: SH-L2 HDRI ambient replaces flat\n"
                                  "sky color. Kill-switch: MC2_MECH_IBL_SH=0.\n"
                                  "Strength shared with Static Prop IBL SH slider.");
        }

        // MECH-BACK-FILL-1: cool-sky fill for shadow hemisphere.
        float bf = batcher_getMechBackFillStrength();
        if (ImGui::SliderFloat("Back fill##pbr", &bf, 0.0f, 4.0f, "%.2f"))
            batcher_setMechBackFillStrength(bf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cool-sky fill for the shadow hemisphere (faces away from sun).\n"
                              "Prevents dark side going pitch-black. Works only in StandardLit path.\n"
                              "Default: 0.25. Kill-switch: MC2_MECH_BACK_FILL=0.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##bf")) batcher_setMechBackFillStrength(2.0f);

        ImGui::Spacing();
        ImGui::SeparatorText("Paint/wear layer (PaintedMetal003)");

        float ws = batcher_getPbrWearStrength();
        if (ImGui::SliderFloat("Wear strength##pbr", &ws, 0.0f, 4.0f, "%.2f"))
            batcher_setPbrWearStrength(ws);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scales PaintedMetal003 metalness as wear mask.\n"
                              "0=all paint, 1=natural wear, 4=fully exposed metal.\n"
                              "Default: 1.0");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbrws")) batcher_setPbrWearStrength(1.0f);

        ImGui::Separator();
        bool triOn = batcher_getPbrTriplanar() != 0;
        if (ImGui::Checkbox("Triplanar##pbr", &triOn))
            batcher_setPbrTriplanar(triOn ? 1 : 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World-space triplanar UV sampling (no UV seams).\n"
                              "Default OFF. Enable with MC2_PBR_TRIPLANAR=1.");

        float ts = batcher_getPbrTriplanarScale();
        if (ImGui::SliderFloat("Triplanar scale##pbr", &ts, 0.01f, 2.0f, "%.3f"))
            batcher_setPbrTriplanarScale(ts);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World-unit tile scale for triplanar sampling.\n"
                              "Higher = smaller/finer tiles. Default: 0.2");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##pbrts")) batcher_setPbrTriplanarScale(0.2f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

// VFX-TUNING-UI-1: GPU particle look tuning. All scales default 1.0 (no-op,
// byte-identical default frame). Look-only — no emission/lifetime/sorting
// change. Tuning is invisible when MC2_GPU_PARTICLES=0 (legacy CPU FX) or in
// scenes with no routed (Card/CardCloud/Point/Shard/Tube) particles.
static void drawVfxTuningSection() {
    const char* penv = std::getenv("MC2_GPU_PARTICLES");
    bool particlesOn = !(penv != nullptr && penv[0] == '0');  // default-ON
    if (particlesOn) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "GPU particles: ON (MC2_GPU_PARTICLES default ON)");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
            "GPU particles: OFF (MC2_GPU_PARTICLES=0 — sliders have no effect)");
    }

    ImGui::BeginDisabled(!particlesOn);

    // Debug view selector (also exposed via MC2_VFX_DEBUG_MODE / inspector).
    ImGui::SeparatorText("Debug view");
    {
        static const char* kVfxModes[] = {
            "0 Final", "1 Albedo", "2 Alpha", "3 ParticleKind", "4 Overdraw" };
        int mode = gos_vfx_getDebugMode();
        if (ImGui::Combo("Mode##vfxdbg", &mode, kVfxModes, 5))
            gos_vfx_setDebugMode(mode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("particle_billboard.frag u_debugMode. Mode 0 = "
                              "byte-identical default. Diagnostic only.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxdbg")) gos_vfx_setDebugMode(0);
    }

    // Intensity scales. Defaults 1.0 = byte-identical.
    ImGui::SeparatorText("Intensity");
    {
        float b = gos_vfx_getBrightness();
        // Slider range matches the backend clamp (0..8) so an env-seeded
        // MC2_TUNE_VFX_BRIGHTNESS > 4 is not silently reduced on first touch.
        if (ImGui::SliderFloat("Brightness##vfx", &b, 0.0f, 8.0f, "%.2f"))
            gos_vfx_setBrightness(b);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Global RGB scale on all particles. Default 1.0 (no-op).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxb")) gos_vfx_setBrightness(1.0f);

        float ab = gos_vfx_getAdditiveBrightness();
        if (ImGui::SliderFloat("Additive brightness##vfx", &ab, 0.0f, 8.0f, "%.2f"))
            gos_vfx_setAdditiveBrightness(ab);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Extra RGB scale applied ONLY to additive groups\n"
                              "(flares/explosions). Default 1.0 (no-op). The\n"
                              "highest-value lever for pre-bloom additive overdraw.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxab")) gos_vfx_setAdditiveBrightness(1.0f);

        float a = gos_vfx_getAlphaScale();
        if (ImGui::SliderFloat("Opacity##vfx", &a, 0.0f, 2.0f, "%.2f"))
            gos_vfx_setAlphaScale(a);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Alpha (opacity) scale on all particles. Default 1.0 (no-op).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxa")) gos_vfx_setAlphaScale(1.0f);

        // VFX-SOFT-PARTICLES-MVP-1: depth-fade enable + world-unit fade band.
        ImGui::Separator();
        bool softOn = gos_vfx_getSoftEnabled() != 0;
        if (ImGui::Checkbox("Soft particles##vfx", &softOn))
            gos_vfx_setSoftEnabled(softOn ? 1 : 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Depth-fade alpha particles where they meet opaque geometry\n"
                              "(no hard intersection lines). Default OFF (env MC2_VFX_SOFT_PARTICLES).\n"
                              "Alpha groups only; additive flashes/lasers unaffected.");
        float sd = gos_vfx_getSoftDistance();
        if (ImGui::SliderFloat("Soft fade dist##vfx", &sd, 0.0f, 200.0f, "%.1f"))
            gos_vfx_setSoftDistance(sd);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World-unit fade band for soft particles. Larger = softer "
                              "intersection. Default 30.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxsd")) gos_vfx_setSoftDistance(30.0f);

        // VFX-LIT-PARTICLES-MVP-1: scene-lighting enable + strength.
        bool litOn = gos_vfx_getLitEnabled() != 0;
        if (ImGui::Checkbox("Lit particles##vfx", &litOn))
            gos_vfx_setLitEnabled(litOn ? 1 : 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tint alpha smoke/dust by the scene sun + ambient so it\n"
                              "reads as lit volume. Default OFF (env MC2_VFX_LIT_PARTICLES).\n"
                              "Alpha groups only; additive flashes/lasers stay emissive.");
        float ls = gos_vfx_getLitStrength();
        if (ImGui::SliderFloat("Lit strength##vfx", &ls, 0.0f, 1.0f, "%.2f"))
            gos_vfx_setLitStrength(ls);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = unlit (byte-identical), 1 = fully scene-lit. Default 0.7.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxls")) gos_vfx_setLitStrength(0.7f);
    }

    if (ImGui::SmallButton("Reset all##vfx")) {
        gos_vfx_setBrightness(1.0f);
        gos_vfx_setAdditiveBrightness(1.0f);
        gos_vfx_setAlphaScale(1.0f);
    }
    ImGui::TextDisabled("Defaults (1.0) are byte-identical. Look-only: no "
                        "emission/lifetime/timing change.");

    ImGui::EndDisabled();
}

// ─────────────────────────────────────────────────────────────────────────────

void draw() {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G))
        s_open = !s_open;

    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(500, 700), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Options  [Ctrl+Shift+G]", &s_open)) {
        ImGui::End();
        return;
    }

    // ── Post-Process ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Post-Process")) {
        gosPostProcess* pp = getGosPostProcess();
        if (pp) {
            // ── Track V: HDR post + grounding (live tuners) ──────────────────
            // All gates are plain runtime bools read every frame, so toggling
            // here takes effect immediately (env var seeds the startup state).
            ImGui::SeparatorText("Track V (Exposure / SSAO)");
            ImGui::Indent();
            ImGui::SliderFloat("Exposure##trackv", &pp->exposure_, 0.0f, 4.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("composite exposure multiplier (default 1.0)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##expo")) pp->exposure_ = 1.0f;
            ImGui::Unindent();

            ImGui::Checkbox("SSAO##trackv", &pp->ssaoEnabled_);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("MC2_SSAO. Half-res grounding AO.\nIndependent of the HDR master gate.");
            if (pp->ssaoEnabled_) {
                ImGui::Indent();
                ImGui::SliderFloat("Radius (wu)##ssao", &pp->aoRadius_,   0.1f, 32.0f);
                ImGui::SliderFloat("Strength##ssao",    &pp->aoStrength_, 0.0f, 2.0f);
                ImGui::SliderFloat("Bias##ssao",        &pp->aoBias_,     0.0f, 0.05f, "%.4f");
                ImGui::SliderFloat("Power##ssao",       &pp->aoPower_,    0.1f, 4.0f);
                bool ssaoDbg = (pp->ssaoDebug_ != 0);
                if (ImGui::Checkbox("Debug: show AO buffer##ssao", &ssaoDbg))
                    pp->ssaoDebug_ = ssaoDbg ? 1 : 0;
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##ssao")) {
                    pp->aoRadius_ = 3.0f; pp->aoStrength_ = 0.7f;
                    pp->aoBias_ = 0.0025f; pp->aoPower_ = 1.5f;
                }
                ImGui::Unindent();
            }

            // POST-FX-FXAA-1: post anti-aliasing tunables. pp->fxaa* members are
            // read every frame by endScene(), so edits take effect immediately.
            // Env (MC2_FXAA / _SUBPIX / _EDGE_THRESHOLD / _EDGE_THRESHOLD_MIN)
            // seeds startup; these sliders override live.
            ImGui::SeparatorText("FXAA (post anti-aliasing)");
            ImGui::Checkbox("Enable##fxaa", &pp->fxaaEnabled_);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("MC2_FXAA. Edge-smoothing on the composited scene (before UI).\n"
                                  "Fixes terrain / map-edge jaggies on straight lines. Does NOT\n"
                                  "fully kill camera-motion edge crawl — use render-scale/SSAA for that.");
            ImGui::BeginDisabled(!pp->fxaaEnabled_);
            ImGui::SliderFloat("Subpixel##fxaa",    &pp->fxaaSubpix_,           0.0f,   1.0f,   "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Subpixel aliasing removal. Higher = softer thin features.\n"
                                  "Sharper preset = 0.25, Stronger = 0.50.");
            ImGui::SliderFloat("Edge thresh##fxaa", &pp->fxaaEdgeThreshold_,    0.063f, 0.333f, "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Min local contrast to antialias. Lower = catches more edges.\n"
                                  "Sharper preset = 0.166, Stronger = 0.125.");
            ImGui::SliderFloat("Edge min##fxaa",    &pp->fxaaEdgeThresholdMin_, 0.0312f, 0.0833f, "%.4f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Dark-region contrast floor.\nSharper preset = 0.0833, Stronger = 0.0625.");
            if (ImGui::SmallButton("Sharper##fxaa")) {
                pp->fxaaSubpix_ = 0.25f; pp->fxaaEdgeThreshold_ = 0.166f; pp->fxaaEdgeThresholdMin_ = 0.0833f;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Stronger##fxaa")) {
                pp->fxaaSubpix_ = 0.50f; pp->fxaaEdgeThreshold_ = 0.125f; pp->fxaaEdgeThresholdMin_ = 0.0625f;
            }
            ImGui::EndDisabled();

            // VIEWMODE-POSTPROCESS-PRESENTATION-1: view-mode selector.
            // Gated on MC2_VIEWMODE_FRAMEWORK (resolved once at init); when OFF
            // the combo is hidden and endScene() forces Visual (byte-identical).
            if (gos_IsViewmodeFrameworkEnabled()) {
                ImGui::SeparatorText("View Mode");
                // Index == RenderCore::ViewMode value (0..5) so the combo
                // selection maps straight to the mode int.
                const char* kModeNames[] = {
                    "Visual (normal)",
                    "Object ID Debug",
                    "Tactical Overlay",
                    "Thermal (placeholder)",
                    "Infrared (n/a)",
                    "Low Light",
                };
                static const int kModeCount = 6;
                int curMode = gos_GetSelectedViewMode();
                if (curMode < 0 || curMode >= kModeCount) curMode = 0;
                if (ImGui::Combo("##viewmode", &curMode, kModeNames, kModeCount))
                    gos_SetSelectedViewMode(curMode);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Visual: normal rendered output (default).\n"
                        "Object ID Debug: per-pixel ID colorization (needs MC2_OBJECT_ID_BUFFER=1).\n"
                        "Tactical Overlay: drawn in-scene by the debug renderer\n"
                        "  (MC2_DEBUG_RENDERER=1 + MC2_TACTICAL_ARC_OVERLAY=1); here it is a passthrough.\n"
                        "Thermal: luminance->iron-palette PLACEHOLDER (not real IR).\n"
                        "Infrared: no implementer (passthrough).\n"
                        "Low Light: night-vision luminance boost + green tint.");
                // LOWLIGHT-NIGHTVISION-MVP-1: live tuners (only meaningful in Low Light).
                if (curMode == 5) {
                    ImGui::Indent();
                    float gain = gos_GetLowLightGain();
                    if (ImGui::SliderFloat("Gain##lowlight", &gain, 0.1f, 16.0f))
                        gos_SetLowLightGain(gain);
                    ImGui::Unindent();
                }
            }

            ImGui::SeparatorText("Legacy / shared");

            ImGui::Checkbox("Shadows##pp",      &pp->shadowsEnabled_);
            ImGui::Checkbox("Shadow Debug##pp", &pp->showShadowDebug_);
            if (pp->showShadowDebug_) {
                ImGui::Indent();
                ImGui::RadioButton("Static##sd",  &pp->shadowDebugMode_, 0); ImGui::SameLine();
                ImGui::RadioButton("Dynamic##sd", &pp->shadowDebugMode_, 1);
                if (ImGui::TreeNodeEx("Registries##sd")) {
                    ImGui::TextDisabled("Views:");
                    uint32_t nv = RenderCore::getViewCount();
                    bool foundShadow = false;
                    for (uint32_t i = 0; i < nv; ++i) {
                        const RenderCore::EngineView* v = RenderCore::getViewByIndex(i);
                        if (!v) continue;
                        if (v->kind == RenderCore::ViewKind::ShadowStatic ||
                            v->kind == RenderCore::ViewKind::ShadowDynamic) {
                            ImGui::Text("  id=%u  %s  %dx%d",
                                v->id, v->debugName ? v->debugName : "?",
                                v->viewport[2], v->viewport[3]);
                            foundShadow = true;
                        }
                    }
                    if (!foundShadow)
                        ImGui::TextDisabled("  (none — load a mission first)");
                    ImGui::TextDisabled("Resources:");
                    for (auto rid : { RenderCore::RenderResourceId::ShadowStaticMap,
                                      RenderCore::RenderResourceId::ShadowDynamicMap }) {
                        const RenderCore::RenderResourceDesc* r =
                            RenderCore::getRenderResource(rid);
                        if (r)
                            ImGui::Text("  %s  %ux%u  %s",
                                r->debugName ? r->debugName : "?",
                                r->width, r->height,
                                RenderCore::toString(r->format));
                        else
                            ImGui::TextDisabled("  %s  (not registered)",
                                RenderCore::toString(rid));
                    }
                    ImGui::TreePop();
                }
                ImGui::Unindent();
            }
            if (ImGui::CollapsingHeader("Shadow Tuning##pp")) {
                ImGui::Indent();
                ImGui::TextDisabled("Polygon offset (static shadow pass only):");
                ImGui::SliderFloat("Factor##bias", &pp->shadowBiasFactor_, 0.0f, 8.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("lower = shadow acne risk\nhigher = peter-panning risk\ndefault 2.0");
                ImGui::SliderFloat("Units##bias",  &pp->shadowBiasUnits_,  0.0f, 20.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("offset units\ndefault 4.0");
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##bias"))
                    { pp->shadowBiasFactor_ = 2.0f; pp->shadowBiasUnits_ = 4.0f; }
                ImGui::Spacing();
                ImGui::TextDisabled("Softness (all shadow receivers):");
                {
                    float softness = gos_GetTerrainShadowSoftness();
                    if (ImGui::SliderFloat("Softness##sdtune", &softness, 0.5f, 8.0f))
                        gos_SetTerrainShadowSoftness(softness);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("higher = blurrier shadows\ndefault 0.9 (also in Terrain Tuning)");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset##sdtune")) gos_SetTerrainShadowSoftness(0.9f);
                }
                ImGui::Unindent();
            }
            ImGui::Separator();
            ImGui::Checkbox("Screen Shadows##pp", &pp->screenShadowEnabled_);
            if (pp->screenShadowEnabled_) {
                ImGui::Indent();
                ImGui::RadioButton("Normal##scr",    &pp->screenShadowDebug_, 0); ImGui::SameLine();
                ImGui::RadioButton("Visualize##scr", &pp->screenShadowDebug_, 1);
                ImGui::Unindent();
            }
            ImGui::Separator();
            ImGui::Checkbox("Shorelines", &pp->shorelineEnabled_);
        } else {
            ImGui::TextDisabled("(post-process system unavailable)");
        }
    }

    // ── Terrain ───────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Terrain"))
        drawTerrainSection();

    // ── Draw Packets ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Draw Packets"))
        drawDrawPacketsSection();

    // ── Render Passes ─────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Render Passes"))
        drawRenderPassesSection();

    // ── Terrain Tuning ────────────────────────────────────────────────────────
    // Heavy section (15 sliders) — collapsed by default so it doesn't push other
    // sections off-screen on first open.
    if (ImGui::CollapsingHeader("Terrain Tuning"))
        drawTerrainTuningSection();

    // ── Static Prop Tuning ────────────────────────────────────────────────────
    // IBL SH + PBR V1 strength + roughness override. Previously lived in
    // the Object Inspector PBR selection block; consolidated here so the
    // controls are reachable without picking a prop.
    if (ImGui::CollapsingHeader("Static Prop Tuning"))
        drawStaticPropTuningSection();

    // ── Mech ──────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Mech"))
        drawMechSection();

    // ── Water ─────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Water"))
        drawWaterSection();

    // ── VFX Tuning ────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("VFX Tuning"))
        drawVfxTuningSection();

    // ── HUD ───────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("HUD")) {
        float scale = gos_GetHudScale();
        if (ImGui::SliderFloat("Scale##hud", &scale, 0.5f, 1.5f, "%.2f"))
            gos_SetHudScale(scale);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##hud")) gos_SetHudScale(1.0f);
    }

    // ── GBuffer Preview ───────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("GBuffer Preview"))
        drawGBufferPreview();

    // ── Env Gates ─────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Env Gates"))
        drawEnvGatesSection();

    // ── Debug Overlays ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Debug Overlays")) {
        if (ImGui::Button("Advance ProjectZ Overlay"))
            projectz_overlay_advance();
        ImGui::SameLine();
        ImGui::TextDisabled("(cycles predicates)");
    }

    // ── Visual Tuning Profile ─────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Visual Tuning Profile")) {
        const char* path    = visualTuning_getProfilePath();
        const char* mission = visualTuning_getActiveMission();
        bool        hasFile = visualTuning_hasProfileFile();
        int         keys    = visualTuning_getAppliedKeyCount();
        ImGui::TextDisabled("File: %s", path);
        if (hasFile) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Loaded");
            ImGui::SameLine();
            ImGui::Text("mission=%s  keys=%d", mission[0] ? mission : "(none)", keys);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No profile file (data/visual_tuning.json)");
        }
        if (ImGui::Button("Reset to Profile"))
            visualTuning_applyProfile(mission[0] ? mission : nullptr);
        ImGui::SameLine();
        ImGui::TextDisabled("re-applies profile; ImGui sliders override after");

        static int s_saveFlash = 0;
        if (mission[0]) {
            if (ImGui::Button("Set as Mission Defaults")) {
                s_saveFlash = visualTuning_saveCurrentToMission() ? 120 : -120;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("writes current slider state to JSON for '%s'", mission);
        }
        if (s_saveFlash > 0) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Saved!");
            s_saveFlash--;
        } else if (s_saveFlash < 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Write failed!");
            s_saveFlash++;
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("RAlt hotkeys still active.  Ctrl+Shift+G to close.");

    ImGui::End();
}

} // namespace GraphicsOptionsWindow
