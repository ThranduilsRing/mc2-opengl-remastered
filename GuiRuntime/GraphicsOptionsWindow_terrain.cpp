// Graphics options window: terrain, static-prop and water tuning sections.
// Declared in GraphicsOptionsWindow_internal.h.
#include "GraphicsOptionsWindow_internal.h"
#include "Gates/gate.h"

namespace GraphicsOptionsWindow {

struct TerrainMode {
    float       value;
    const char* name;
    const char* channels;   // null = no sub-line
};

static const TerrainMode kTerrainModes[] = {
    //  value    name                     channel breakdown
    {  0.0f, "OFF",               nullptr },
    {  1.0f, "Depth Comparison",  "R = rasterized depth  |  G = undisplaced depth  (range 0.85–1.0)" },
    {  2.0f, "Raw Colormap",      "terrain colormap tex1, unlit" },
    {  3.0f, "Blurred Colormap",  "LOD-tiered blur: far=1-tap, mid=5-tap, near=9-tap disc" },
    {  4.0f, "Material Weights",  "R = rock  |  G = grass  |  B = dirt  (classification weights)" },
    {  5.0f, "Normal Lighting",   "diffuse contribution (0.35–1.20 remapped to 0–1)" },
    {  6.0f, "Shadow Factor",     "PCF static shadow × dynamic shadow" },
    {  7.0f, "Cloud Shadow (n/a)", "removed — cloud is now the fullscreen cloud.frag pass (see Render Passes > Cloud Shadows)" },
    {  8.0f, "Cement Diagnostic", "R = layer valid  |  G = layer index/255  |  B = useCementAtlas==0" },
    {  9.0f, "Thin-Record",       "R = recipeIdx/255  |  G = flags/255  |  B = terrainHandle/255" },
    { 10.0f, "Height Normal",     "height-texture-derived normal as RGB (centered around 0.5)" },
    { -1.0f, "Tess Alive Probe",  "solid red = tessellation frag shader is running" },
};
static const int kTerrainModeCount = (int)(sizeof(kTerrainModes) / sizeof(kTerrainModes[0]));
static const int kDiagnosticStart  = 8; // index into kTerrainModes where diagnostics begin
void drawTerrainSection() {
    bool terrainDraw = gos_GetTerrainDrawEnabled();
    if (ImGui::Checkbox("Terrain Draw", &terrainDraw))
        gos_SetTerrainDrawEnabled(terrainDraw);

    // No public getter for wireframe state — track locally.
    static bool s_terrainWire = false;
    if (ImGui::Checkbox("Terrain Wireframe", &s_terrainWire))
        gos_SetTerrainWireframe(s_terrainWire);

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


void drawTerrainTuningSection() {
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

    ImGui::SeparatorText("Phong Smoothing");
    float phong = gos_GetTerrainPhongAlpha();
    if (ImGui::SliderFloat("Alpha##phong", &phong, 0.0f, 1.0f))
        gos_SetTerrainPhongAlpha(phong);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = linear TES interpolation, 1 = full Phong smoothing. Default: 0.5\n"
                          "Only visible on curved/hilly terrain; flat terrain is unaffected.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##phong")) gos_SetTerrainPhongAlpha(0.5f);

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

    ImGui::SeparatorText("Shadow");

    float softness = gos_GetTerrainShadowSoftness();
    if (ImGui::SliderFloat("Shadow softness##ss", &softness, 0.0f, 1.0f))
        gos_SetTerrainShadowSoftness(softness);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PCF penumbra radius. 0=hard shadows, 1=maximum softness. Default: 0.9");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##ss")) gos_SetTerrainShadowSoftness(0.9f);

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

    // ── Normals-from-Height ──
    // Consolidated home for the height-texture / resample / strength controls so all
    // terrain tuning is in one window.
    ImGui::SeparatorText("Normals-from-Height");

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

    float nfh = ::gos_GetTerrainNormalsFromHeightStrength();
    if (ImGui::SliderFloat("NfH strength##tnfh", &nfh, 0.0f, 1.5f, "%.2f")) {
        ::gos_SetTerrainNormalsFromHeightStrength(nfh);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scales the macro-slope tilt on the chunk terrain path.\n"
                          "1.0 = full slope tilt (default). 0.0 = no slope contribution.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##tnfh")) ::gos_SetTerrainNormalsFromHeightStrength(1.0f);
    ImGui::TextDisabled("Debug Mode 10 = height-normal RGB");

    // CLIFF SHADOW FLOOR: lifts shadow-side steep terrain faces off near-black.
    {
        bool cliffGateOn = mc2::gate::get<"MC2_TERRAIN_CLIFF_SHADOW_FLOOR">();
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
        bool ddOn = mc2::gate::get<"MC2_DYNAMIC_DECALS">();
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


// Static-prop visual tuning. Previously lived in the Object Inspector PBR
// block (selection-driven); consolidated here so the controls are reachable
// without picking a prop. Env gates remain authoritative — sliders only
// modulate strength when the matching env gate is ON.
void drawStaticPropTuningSection() {
    // IBL SH ambient (V-IBL-STATIC-1; default-ON env). Slider modulates
    // strength when MC2_STATIC_PROP_IBL_SH != 0 (default-on).
    ImGui::SeparatorText("IBL SH ambient");
    {
        bool iblOn = mc2::gate::get<"MC2_STATIC_PROP_IBL_SH">();
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
        bool pbrOn = mc2::gate::get<"MC2_STATIC_PROP_PBR_V1">();
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
        bool pbrOn = mc2::gate::get<"MC2_STATIC_PROP_PBR_V1">();
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
void drawWaterSection() {
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
        bool gateOn = mc2::gate::get<"MC2_WATER_SKYTINT">();
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



} // namespace GraphicsOptionsWindow
