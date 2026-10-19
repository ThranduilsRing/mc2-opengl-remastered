/***************************************************************
* FILENAME: MapGeneratorDialog.cpp
* DESCRIPTION: ImGui Map Generator dialog for the MC2 Mission Editor.
*   Provides a single integrated window replacing the old MFC TerrainDlg +
*   MapSizeDlg flow.  Supports biome, size, height, material-mix controls,
*   a fast thumbnail preview, and deferred full generation.
*
* Biome labels map to terrain_gen.py recipe biome strings:
*   "Temperate Forest"  -> "swamp_forest"
*   "Desert"            -> "desert"
*   "Tundra / Snow"     -> "snow_mountain"
*   "Rocky Badlands"    -> "rocky_badlands"
*   "Volcanic"          -> "rocky_badlands"  (no dedicated volcanic biome yet)
*
* Map size labels and k_mapSizeCellSides use the CELL (quad) count per side — the
* user-facing dimension.  Vertex count = cellSide + 1.  The terrain generator recipe
* uses the vertex count so the generator produces the right grid size.  Use
* EditorData::MapSizeToCellSide() / MapSizeToVertexSide() for all conversions.
*
* Recipe JSON is written to terrain_gen_out/genmap_recipe.json.
* Preview PNG is loaded from terrain_gen_out/genmap.preview.png into a service
* texture and displayed via ImGui::Image.
*
* DATE: 2026-06-05
****************************************************************/

// Pull stdafx first (MFC / Windows headers)
#include "stdafx.h"

#ifdef MC2_IMGUI

// ImGui
#include "imgui.h"

#include "EditorTexture.h"

#include "MapGeneratorDialog.h"
#include "EditorData.h"    // EditorData::generateFromDialogParams declaration
#include "EditorTaskRunner.h"  // async subprocess
#include "FoliageRender.h"     // load generated foliage sidecar after apply

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char* k_biomeLabels[5] = {
    "Temperate Forest",
    "Desert",
    "Tundra / Snow",
    "Rocky Badlands",
    "Volcanic",
};

// terrain_gen.py recipe biome strings
static const char* k_biomeKeys[5] = {
    "swamp_forest",
    "desert",
    "snow_mountain",
    "rocky_badlands",
    "rocky_badlands",
};

// Per-biome foliage character (indexed by biomeIndex, same order as the labels).
// The editor renders foliage by KIND (tree/rock/bush colored billboards -- no
// per-species art ships), so "species" here = biome-appropriate placement +
// kind mix + the asset name carried in the JSON (consumed by the game later).
// The user's Tree Density / Tree Line / rocks controls tune these per-biome bases.
struct BiomeFoliageDef {
    const char* treeAsset;    // recorded in foliage.json (kind stays "tree")
    float       treeDensMul;  // multiplies the user density for trees
    float       treeNoise;    // noise_threshold: >0 clusters trees into clumps
    bool        treesAvoidWater;
    bool        bushes;       // emit a bush rule for this biome
    const char* bushAsset;
    float       bushDensMul;
    float       rockDensMul;  // multiplies rock density when rocks are enabled
};
static const BiomeFoliageDef k_biomeFoliage[5] = {
    // Temperate Forest: lush, clumped pines + bushes, some rocks.
    { "tree_pine_a",      1.00f, 0.45f, true,  true,  "bush_leafy_a",  0.60f, 0.40f },
    // Desert: sparse palms, no clumping, cactus bushes, lots of rock.
    { "tree_palm_a",      0.25f, 0.00f, true,  true,  "bush_cactus_a", 0.30f, 0.85f },
    // Tundra / Snow: scattered snow pines (tree line low), no bushes, rocks.
    { "tree_pine_snow_a", 0.50f, 0.20f, true,  false, "bush_leafy_a",  0.00f, 0.70f },
    // Rocky Badlands: very sparse scrub, scrub bushes, heavy rock.
    { "tree_scrub_a",     0.30f, 0.15f, true,  true,  "bush_scrub_a",  0.40f, 1.00f },
    // Volcanic: near-barren dead snags, no bushes, dense rock.
    { "tree_dead_a",      0.15f, 0.10f, true,  false, "bush_scrub_a",  0.00f, 1.25f },
};

// MapSize index -> cell side (quads per side, user-facing label).  Always multiples of 20.
// Use EditorData::MapSizeToCellSide() / MapSizeToVertexSide() for conversions instead of
// indexing this array directly — it is kept here only for the preset dir name and recipe.
static const int   k_mapSizeCellSides[7] = { 60, 80, 100, 120, 260, 520, 1020 };
static const char* k_mapSizeLabels[7]   = {
    "60x60",
    "80x80",
    "100x100",
    "120x120 (large)",
    "260x260 (huge)",
    "520x520 (xl)",
    "1020x1020 (xxl)"
};
static const int k_numMapSizes = 7;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

struct DialogState {
    bool open            = false;

    int  biomeIndex      = 0;    // Temperate Forest default
    int  mapSizeIndex    = 3;    // 120x120 default

    // "Import from..." heightfield (Gaea / Blender / EXR). When importHeightPath is
    // set, the Import button runs import_gaea_height.py to write terrain_gen_out/genmap.*
    // and applies via the same path as Generate. importColorPath is optional.
    char importHeightPath[1024] = "";
    char importColorPath[1024]  = "";

    float maxElevation   = 600.f;
    float minElevation   = 0.f;
    float meanHeight     = 0.5f; // 0..1 slider: controls elevation baseline
    float variation      = 0.5f; // 0..1 slider: controls mountain + ridged amount
    float mountainAmount = 0.5f; // 0..1 slider; mapped to recipe [0.1..0.9]
    float ridgedAmount   = 0.3f; // 0..1 slider
    float grassLowland   = 0.5f;
    float snowLine       = 1.0f; // Default to 1.0 (snow only at very top)

    // Foliage / trees (PCG sidecar -> generator --generate-foliage). When on, the
    // recipe carries a "foliage" rules array and the generated genmap.foliage.json
    // is loaded into FoliageRender after the terrain applies.
    bool  generateFoliage = true;
    float treeDensity     = 0.5f; // 0..1 -> rule density
    float treeLine        = 0.85f;// 0..1 -> tree max_altitude (no trees above)
    bool  placeRocks      = true; // scatter rocks on slopes

    unsigned long seed   = 0;    // 0 = randomise each run
    bool  randomSeed     = true;

    // Preview texture (session service)
    RenderCore::ServiceTextureRef previewTexture;
    int    previewW      = 0;
    int    previewH      = 0;

    // Pending deferred action (consumed once by update())
    MapGeneratorDialog::PendingAction pendingAction = MapGeneratorDialog::PendingAction::None;

    // Status string shown below buttons
    char statusMsg[256]  = "";
};

static DialogState s_state;

// Async Generate hand-off (all touched on the main thread):
//   s_generateReady       -- task succeeded; the terrain apply is pending. The apply
//                            itself is deferred to EditorInterface::update() so it can
//                            run in the proven order eye->reset() -> apply (which primes
//                            the terrain face cache against the FINAL camera) -> setPos.
//   s_pendingApplySizeIdx  -- map-size index captured at task START (slider-change safe)
//   s_pendingApplyBiome    -- biome key captured at task START
static bool        s_generateReady       = false;
static int         s_pendingApplySizeIdx = 0;
static std::string s_pendingApplyBiome;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Map mountain_amount slider [0..1] to recipe value [0.1..0.9]
static float SliderToMountainAmount(float t) {
    return 0.1f + t * 0.8f;
}

// Same for ridged_amount
static float SliderToRidgedAmount(float t) {
    return 0.1f + t * 0.8f;
}

// Build the recipe JSON string for the current dialog params.
static std::string BuildRecipeJSON(bool preview) {
    unsigned long seed = s_state.seed;
    if (s_state.randomSeed || seed == 0) {
        seed = (unsigned long)time(nullptr) ^ (unsigned long)(uintptr_t)&s_state;
        // Store so Preview + Generate share the same seed in one session.
        if (!preview || s_state.seed == 0)
            s_state.seed = seed;
    }

    // Recipe "size" is the VERTEX count so the generator produces the right grid.
    int sideN    = EditorData::MapSizeToVertexSide(s_state.mapSizeIndex);

    // Derive mountain and ridged amounts from variation slider
    float mnt    = SliderToMountainAmount(s_state.variation);
    float ridged = SliderToRidgedAmount(s_state.variation);

    // Derive minElevation from meanHeight slider (0..1 maps to 0..500)
    float minElev = s_state.meanHeight * 500.f;

    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"genmap\",\n"
        "  \"size\": %d,\n"
        "  \"biome\": \"%s\",\n"
        "  \"seed\": %lu,\n"
        "  \"height\": {\n"
        "    \"max_elevation\": %.1f,\n"
        "    \"min_elevation\": %.1f,\n"
        "    \"mountain_amount\": %.3f,\n"
        "    \"ridged_amount\": %.3f\n"
        "  },\n"
        "  \"materials\": {\n"
        "    \"grass_lowland\": %.3f,\n"
        "    \"snow_line\": %.3f\n"
        "  }",                       // NOTE: no top-level close yet (foliage may follow)
        sideN,
        k_biomeKeys[s_state.biomeIndex],
        seed,
        (double)s_state.maxElevation,
        (double)minElev,
        (double)mnt,
        (double)ridged,
        (double)s_state.grassLowland,
        (double)s_state.snowLine
    );

    std::string recipe(buf);

    // Per-biome foliage rules: a tree rule (biome species + clustering), an
    // optional biome bush rule, and an optional rock rule. The user's Tree
    // Density / Tree Line / rocks controls tune the per-biome bases.
    if (s_state.generateFoliage) {
        const BiomeFoliageDef& bf = k_biomeFoliage[s_state.biomeIndex];
        const float baseD = 0.004f + s_state.treeDensity * 0.026f;   // [0.004..0.030]
        const float treeD = baseD * bf.treeDensMul;
        const float bushD = baseD * 0.5f * bf.bushDensMul;
        const float rockD = (0.002f + s_state.treeDensity * 0.010f) * bf.rockDensMul;
        const float treeMaxAlt = s_state.treeLine;

        std::string rules;
        char b[1024];

        snprintf(b, sizeof(b),
            "    { \"asset\": \"%s\", \"kind\": \"tree\", \"density\": %.4f,\n"
            "      \"avoid_water\": %s, \"max_altitude\": %.3f, \"max_slope\": 0.55,\n"
            "      \"noise_threshold\": %.3f, \"min_spacing\": 256.0, \"scale_min\": 0.8, \"scale_max\": 1.25 }",
            bf.treeAsset, (double)treeD, bf.treesAvoidWater ? "true" : "false",
            (double)treeMaxAlt, (double)bf.treeNoise);
        rules += b;

        if (bf.bushes) {
            snprintf(b, sizeof(b),
                ",\n    { \"asset\": \"%s\", \"kind\": \"bush\", \"density\": %.4f,\n"
                "      \"avoid_water\": true, \"max_altitude\": %.3f, \"max_slope\": 0.70,\n"
                "      \"min_spacing\": 192.0, \"scale_min\": 0.7, \"scale_max\": 1.2 }",
                bf.bushAsset, (double)bushD, (double)treeMaxAlt);
            rules += b;
        }

        if (s_state.placeRocks) {
            snprintf(b, sizeof(b),
                ",\n    { \"asset\": \"rock_a\", \"kind\": \"rock\", \"density\": %.4f,\n"
                "      \"avoid_water\": true, \"min_altitude\": 0.15, \"max_slope\": 1.0,\n"
                "      \"min_spacing\": 320.0, \"scale_min\": 0.7, \"scale_max\": 1.4 }",
                (double)rockD);
            rules += b;
        }

        recipe += ",\n  \"foliage\": [\n";
        recipe += rules;
        recipe += "\n  ]";
    }

    recipe += "\n}\n";
    return recipe;
}

// Write recipe JSON to disk.  Returns false on error.
static bool WriteRecipeFile(const std::string& json) {
    const char* dir  = "terrain_gen_out";
    const char* path = "terrain_gen_out\\genmap_recipe.json";
    CreateDirectoryA(dir, nullptr);
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fputs(json.c_str(), f);
    fclose(f);
    return true;
}

// Build the terrain-generator command line.  Launches Python UNBUFFERED (-u) so
// PROGRESS lines reach the task runner's pipe immediately instead of being held in
// stdio buffers until exit.  preview=true adds --preview (fast, ~2-3s).
static std::string BuildTerrainGenCmd(bool preview) {
    const char* recipeArg = "terrain_gen_out\\genmap_recipe.json";
    const char* outArg    = "terrain_gen_out";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "py -3 -u tools\\terrain_gen\\terrain_gen.py \"%s\" --out \"%s\"%s%s",
        recipeArg, outArg,
        preview ? " --preview" : "",
        // Foliage is a full-generate artifact only; skip it for the fast preview.
        (s_state.generateFoliage && !preview) ? " --generate-foliage" : "");
    return std::string(cmd);
}

// Open a Win32 file picker and copy the chosen path into dst (size dstLen).
// Returns true if the user picked a file. Mirrors the GetOpenFileName idiom the
// MFC editor TUs use (CFileDialog wraps this same call). The owner is the active
// window so the dialog is parented correctly over the editor.
static bool BrowseForFile(const char* filter, char* dst, size_t dstLen) {
    char path[1024] = "";
    if (dst && dst[0]) {
        strncpy(path, dst, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = ::GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn))
        return false;
    strncpy(dst, path, dstLen - 1);
    dst[dstLen - 1] = '\0';
    return true;
}

// Free the existing preview texture (if any).
static void FreePreviewTexture() {
    s_state.previewTexture = {};
    s_state.previewW = s_state.previewH = 0;
}

// The generator output is a new source version each time it changes.
static void LoadPreviewPNG() {
    FreePreviewTexture();
    s_state.previewTexture = loadEditorTexture("terrain_gen_out/genmap.preview.png");
    if (!s_state.previewTexture.get().isValid()) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Preview: failed to load generator output");
        return;
    }
    const auto dimensions = RenderCore::textureService().dimensions(s_state.previewTexture.get());
    s_state.previewW = dimensions.width; s_state.previewH = dimensions.height;
    snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Preview ready (%dx%d)", s_state.previewW, s_state.previewH);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MapGeneratorDialog::Open() {
    s_state.open = true;
    // Preserve params so the user keeps their last settings when reopening.
}

void MapGeneratorDialog::Close() {
    s_state.open = false;
    FreePreviewTexture();
    s_state.statusMsg[0] = '\0';
}

bool MapGeneratorDialog::IsOpen() {
    return s_state.open;
}

MapGeneratorDialog::PendingAction MapGeneratorDialog::TakeAction() {
    PendingAction a = s_state.pendingAction;
    s_state.pendingAction = PendingAction::None;
    return a;
}


void MapGeneratorDialog::Draw() {
    if (!s_state.open) return;

    // Layout sizes are base pixels; TEXT scales through the global io.FontGlobalScale
    // (s_uiScale) like every other editor panel. Do NOT apply a second DPI factor here
    // -- the old GetDpiScale()*SetWindowFontScale path multiplied on top of
    // FontGlobalScale (which is already DPI-derived), giving this dialog ~DPI-squared
    // text far larger than the docked panels. sc=1.0 keeps it matched to the dock size.
    const float sc = 1.0f;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540.f * sc, 0), ImGuiCond_Appearing);

    bool windowOpen = true;
    if (!ImGui::Begin("Map Generator", &windowOpen,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!windowOpen) Close();
        return;
    }
    if (!windowOpen) {
        ImGui::End();
        Close();
        return;
    }
    // Text scales via the global io.FontGlobalScale (set once for all panels); no
    // per-window font scale here (that double-applied DPI -- see sc above).

    // --- Import from heightfield (prominent, above the procedural controls) ---
    // Bring an external heightmap (Gaea OR Blender export -- both emit EXR/PNG
    // heightfields) in as the editor terrain. Runs import_gaea_height.py, then
    // applies via the SAME path as Generate (genmap.* -> ApplyPendingGenerate).
    {
        const bool taskBusyTop = EditorTaskRunner::HasActiveTasks();
        if (ImGui::CollapsingHeader("Import from heightfield (Gaea / Blender / EXR)",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();

            ImGui::Text("Height file");
            ImGui::SetNextItemWidth(330.f * sc);
            ImGui::InputText("##importHeight", s_state.importHeightPath,
                             sizeof(s_state.importHeightPath),
                             ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...##height", ImVec2(90.f * sc, 0))) {
                BrowseForFile(
                    "Heightfields (*.exr;*.png;*.tif;*.tiff;*.r16;*.r32)\0"
                    "*.exr;*.png;*.tif;*.tiff;*.r16;*.r32\0"
                    "All files\0*.*\0",
                    s_state.importHeightPath, sizeof(s_state.importHeightPath));
            }

            ImGui::Text("Color file (optional)");
            ImGui::SetNextItemWidth(330.f * sc);
            ImGui::InputText("##importColor", s_state.importColorPath,
                             sizeof(s_state.importColorPath),
                             ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...##color", ImVec2(90.f * sc, 0))) {
                BrowseForFile(
                    "Images (*.png;*.exr;*.jpg;*.tga)\0*.png;*.exr;*.jpg;*.tga\0"
                    "All files\0*.*\0",
                    s_state.importColorPath, sizeof(s_state.importColorPath));
            }

            ImGui::TextDisabled("Uses the Size / Biome / Peak Height below.");

            const bool canImport = !taskBusyTop && s_state.importHeightPath[0] != '\0';
            if (!canImport) ImGui::BeginDisabled();
            if (ImGui::Button("Import", ImVec2(-1.f, 0))) {
                snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Queuing import...");
                s_state.pendingAction = PendingAction::Import;
            }
            if (!canImport) ImGui::EndDisabled();

            ImGui::Unindent();
        }
        ImGui::Separator();
    }

    // --- Biome ---
    ImGui::Text("Biome");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.f * sc);
    if (ImGui::BeginCombo("##biome", k_biomeLabels[s_state.biomeIndex])) {
        for (int i = 0; i < 5; ++i) {
            bool sel = (s_state.biomeIndex == i);
            if (ImGui::Selectable(k_biomeLabels[i], sel))
                s_state.biomeIndex = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // --- Map Size ---
    ImGui::Text("Size  ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.f * sc);
    if (ImGui::BeginCombo("##size", k_mapSizeLabels[s_state.mapSizeIndex])) {
        for (int i = 0; i < k_numMapSizes; ++i) {
            bool sel = (s_state.mapSizeIndex == i);
            if (ImGui::Selectable(k_mapSizeLabels[i], sel))
                s_state.mapSizeIndex = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::Text("Height");
    ImGui::Indent();

    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Mean Height (baseline)",  &s_state.meanHeight, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Peak Height (max)",  &s_state.maxElevation, 100.f, 600.f, "%.0f");

    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Variation (flat to mountainous)", &s_state.variation, 0.f, 1.f, "%.2f");
    ImGui::Unindent();

    ImGui::Separator();
    ImGui::Text("Materials");
    ImGui::Indent();
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Grass Lowland",  &s_state.grassLowland,  0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(280.f * sc);
    ImGui::SliderFloat("Snow Line",      &s_state.snowLine,       0.f, 1.f, "%.2f");
    ImGui::Unindent();

    ImGui::Separator();
    // --- Foliage / Trees (IN PROGRESS) ---
    // Real prop-instance foliage rendering is not wired up yet (the preview only
    // produced flat placeholder cards). Disable the controls and force the option
    // off so generation never emits a foliage sidecar, until the prop path lands.
    s_state.generateFoliage = false;
    ImGui::Text("Foliage");
    ImGui::SameLine();
    ImGui::TextDisabled("(in progress)");
    ImGui::Indent();
    ImGui::BeginDisabled(true);
    bool foliageStub = false;
    ImGui::Checkbox("Generate trees & rocks", &foliageStub);
    ImGui::EndDisabled();
    ImGui::Unindent();

    ImGui::Separator();
    // --- Seed ---
    ImGui::Checkbox("Random seed", &s_state.randomSeed);
    if (!s_state.randomSeed) {
        ImGui::SameLine();
        int seed_i = (int)(s_state.seed & 0x7fffffff);
        ImGui::SetNextItemWidth(120.f * sc);
        if (ImGui::InputInt("##seed", &seed_i))
            s_state.seed = (unsigned long)(seed_i < 0 ? 0 : seed_i);
    }

    ImGui::Separator();
    // --- Preview thumbnail ---
    const float previewDisplayW = 256.f * sc;
    if (s_state.previewTexture.get().isValid()) {
        ImGui::Text("Preview:");
        float aspect = (s_state.previewH > 0)
                       ? (float)s_state.previewW / (float)s_state.previewH
                       : 1.f;
        float dh = previewDisplayW / aspect;
        ImGui::Image((ImTextureID)RenderCore::uiTextureId(s_state.previewTexture.get()),
                     ImVec2(previewDisplayW, dh));
    } else {
        ImGui::Dummy(ImVec2(previewDisplayW, previewDisplayW * 0.4f));
        ImGui::SameLine();
        ImGui::TextDisabled("(click Preview)");
    }

    // Status line
    if (s_state.statusMsg[0]) {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.f), "%s", s_state.statusMsg);
    }

    ImGui::Separator();
    // --- Buttons ---

    // "Load Preset" -- instant load from pre-baked flat terrain files (no Python).
    // Three states based on manifest.json (written last during generation = completion flag):
    //   1. Folder absent          -> preset missing
    //   2. Folder present, no manifest -> incomplete / interrupted generation
    //   3. manifest.json present  -> preset complete, button enabled
    {
        char presetDir[512];
        snprintf(presetDir, sizeof(presetDir),
            "terrain_gen_presets\\%s_%d",
            k_biomeKeys[s_state.biomeIndex],
            EditorData::MapSizeToCellSide(s_state.mapSizeIndex));
        char presetManifest[640];
        snprintf(presetManifest, sizeof(presetManifest), "%s\\manifest.json", presetDir);

        bool dirExists      = (GetFileAttributesA(presetDir)      != INVALID_FILE_ATTRIBUTES);
        bool presetComplete = (GetFileAttributesA(presetManifest) != INVALID_FILE_ATTRIBUTES);
        bool presetPartial  = dirExists && !presetComplete;

        if (!presetComplete) ImGui::BeginDisabled();
        if (ImGui::Button("Load Preset", ImVec2(110.f * sc, 0))) {
            snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Loading preset...");
            s_state.pendingAction = PendingAction::LoadPreset;
        }
        if (!presetComplete) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(presetPartial
                    ? "Preset folder exists but is incomplete (generation may have been interrupted).\n"
                      "Re-run: py tools/terrain_gen/gen_presets.py"
                    : "No pre-baked preset for this biome+size.\n"
                      "Run: py tools/terrain_gen/gen_presets.py\n"
                      "Or use Generate to run the full generator.");
            }
        } else {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Load pre-baked flat terrain instantly (no Python run).\n"
                    "Preset: %s", presetDir);
            }
        }
    }

    // Disable Preview/Generate while an async generation task is in flight so the
    // user cannot launch a second overlapping run (apply order would be ambiguous).
    const bool taskBusy = EditorTaskRunner::HasActiveTasks();
    if (taskBusy) ImGui::BeginDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Preview (~3s)", ImVec2(120.f * sc, 0))) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Queuing preview...");
        s_state.pendingAction = PendingAction::Preview;
    }
    ImGui::SameLine();
    if (ImGui::Button("Generate", ImVec2(100.f * sc, 0))) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Queuing generate...");
        s_state.pendingAction = PendingAction::Generate;
    }
    if (taskBusy) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80.f * sc, 0))) {
        Close();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Deferred execution (called from EditorInterface::update, not inside ImGui)
// ---------------------------------------------------------------------------

bool MapGeneratorDialog::IsTaskActive() {
    return EditorTaskRunner::HasActiveTasks();
}

// ===========================================================================
// Foliage Detail panel
// ===========================================================================
namespace {

struct FoliageState {
    bool  open          = false;
    float treeDensity   = 0.5f;
    float clumpiness    = 0.45f; // -> noise_threshold (forests in patches)
    float bushDensity   = 0.5f;
    float treeLine      = 0.85f; // max_altitude for trees/bushes
    float scaleVar      = 0.25f; // +/- fraction on instance scale
    float spacing       = 256.f; // world units between instances
    bool  shoreline     = false; // reeds at the water's edge
    bool  rocks         = true;
    char  status[192]   = "";
};
static FoliageState s_fol;

static const char* k_recipePath    = "terrain_gen_out\\genmap_recipe.json";
static const char* k_folRulesPath  = "terrain_gen_out\\foliage_rules.json";
static const char* k_folJsonPath   = "terrain_gen_out\\genmap.foliage.json";

// Pull the biome index from the saved recipe so the foliage species match the
// terrain. Falls back to 0 (Temperate Forest) if not found.
static int ReadRecipeBiomeIndex() {
    FILE* f = fopen(k_recipePath, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char* p = strstr(buf, "\"biome\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    const char* q = strchr(p + 1, '"');
    if (!q) return 0;
    std::string key(p + 1, q - (p + 1));
    for (int i = 0; i < 5; ++i)
        if (key == k_biomeKeys[i]) return i;
    return 0;
}

// Build a {"foliage":[...]} rules file from the knobs + the biome species table.
static std::string BuildFoliageRulesJSON(int biomeIndex) {
    if (biomeIndex < 0 || biomeIndex > 4) biomeIndex = 0;
    const BiomeFoliageDef& bf = k_biomeFoliage[biomeIndex];

    const float baseD = 0.004f + s_fol.treeDensity * 0.026f;
    const float treeD = baseD * bf.treeDensMul;
    const float bushD = (0.004f + s_fol.bushDensity * 0.026f) * 0.6f * (bf.bushes ? 1.0f : 0.6f);
    const float rockD = (0.002f + s_fol.treeDensity * 0.010f) * bf.rockDensMul;
    const float noise = s_fol.clumpiness * 0.7f;
    const float sMin  = 1.0f - 0.4f * s_fol.scaleVar;
    const float sMax  = 1.0f + 0.4f * s_fol.scaleVar;

    std::string out = "{\n  \"foliage\": [\n";
    char b[1024];

    snprintf(b, sizeof(b),
        "    { \"asset\": \"%s\", \"kind\": \"tree\", \"density\": %.4f,\n"
        "      \"avoid_water\": %s, \"max_altitude\": %.3f, \"max_slope\": 0.55,\n"
        "      \"noise_threshold\": %.3f, \"min_spacing\": %.1f, \"scale_min\": %.2f, \"scale_max\": %.2f }",
        bf.treeAsset, (double)treeD, bf.treesAvoidWater ? "true" : "false",
        (double)s_fol.treeLine, (double)noise, (double)s_fol.spacing, (double)sMin, (double)sMax);
    out += b;

    if (s_fol.bushDensity > 0.01f) {
        snprintf(b, sizeof(b),
            ",\n    { \"asset\": \"%s\", \"kind\": \"bush\", \"density\": %.4f,\n"
            "      \"avoid_water\": true, \"max_altitude\": %.3f, \"max_slope\": 0.70,\n"
            "      \"min_spacing\": %.1f, \"scale_min\": %.2f, \"scale_max\": %.2f }",
            bf.bushAsset, (double)bushD, (double)s_fol.treeLine,
            (double)(s_fol.spacing * 0.75f), (double)sMin, (double)sMax);
        out += b;
    }

    if (s_fol.shoreline) {
        snprintf(b, sizeof(b),
            ",\n    { \"asset\": \"reed_a\", \"kind\": \"bush\", \"density\": 0.020,\n"
            "      \"avoid_water\": false, \"min_altitude\": 0.0, \"max_altitude\": 0.06,\n"
            "      \"max_slope\": 0.30, \"min_spacing\": 160.0, \"scale_min\": 0.7, \"scale_max\": 1.1 }");
        out += b;
    }

    if (s_fol.rocks) {
        snprintf(b, sizeof(b),
            ",\n    { \"asset\": \"rock_a\", \"kind\": \"rock\", \"density\": %.4f,\n"
            "      \"avoid_water\": true, \"min_altitude\": 0.15, \"max_slope\": 1.0,\n"
            "      \"min_spacing\": 320.0, \"scale_min\": 0.7, \"scale_max\": 1.4 }",
            (double)rockD);
        out += b;
    }

    out += "\n  ]\n}\n";
    return out;
}

// Kick off the fast foliage-only generation on the current map's recipe.
static void RunFoliageGenerate() {
    if (EditorTaskRunner::HasActiveTasks()) {
        snprintf(s_fol.status, sizeof(s_fol.status), "A generation task is already running.");
        return;
    }
    if (GetFileAttributesA(k_recipePath) == INVALID_FILE_ATTRIBUTES) {
        snprintf(s_fol.status, sizeof(s_fol.status),
            "No generated-map recipe. Use Generate Map first.");
        return;
    }

    std::string rules = BuildFoliageRulesJSON(ReadRecipeBiomeIndex());
    CreateDirectoryA("terrain_gen_out", nullptr);
    FILE* f = fopen(k_folRulesPath, "w");
    if (!f) {
        snprintf(s_fol.status, sizeof(s_fol.status), "Could not write foliage rules.");
        return;
    }
    fputs(rules.c_str(), f);
    fclose(f);

    char cmd[1100];
    snprintf(cmd, sizeof(cmd),
        "py -3 -u tools\\terrain_gen\\terrain_gen.py \"%s\" --out terrain_gen_out "
        "--foliage-only --foliage-rules \"%s\"",
        k_recipePath, k_folRulesPath);

    EditorTaskRunner::TaskSpec spec;
    spec.name        = "Foliage";
    spec.commandLine = cmd;
    spec.onSuccessMainThread = [](const EditorTaskRunner::TaskResult&) {
        if (FoliageRender::Load(k_folJsonPath)) {
            if (!FoliageRender::Visible())
                FoliageRender::Toggle();
            snprintf(s_fol.status, sizeof(s_fol.status),
                "Placed %d foliage instances.", FoliageRender::Count());
        } else {
            snprintf(s_fol.status, sizeof(s_fol.status),
                "Foliage ran but no instances were placed.");
        }
    };
    spec.onFailureMainThread = [](const EditorTaskRunner::TaskResult& r) {
        snprintf(s_fol.status, sizeof(s_fol.status),
            "Foliage failed (exit %d). See Task Monitor log.", r.exitCode);
    };
    EditorTaskRunner::StartTask(spec);
    snprintf(s_fol.status, sizeof(s_fol.status), "Generating foliage...");
}

} // anonymous namespace

void MapGeneratorDialog::ToggleFoliagePanel() { s_fol.open = !s_fol.open; }
bool MapGeneratorDialog::FoliagePanelOpen()   { return s_fol.open; }

void MapGeneratorDialog::DrawFoliagePanel() {
    if (!s_fol.open)
        return;

    const ImGuiIO& io = ImGui::GetIO();
    const float sc = io.DisplaySize.x / 1280.f;

    ImGui::SetNextWindowSize(ImVec2(320.f * sc, 0.f), ImGuiCond_FirstUseEver);
    bool open = s_fol.open;
    if (!ImGui::Begin("Foliage Detail", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        s_fol.open = open;
        return;
    }
    s_fol.open = open;

    const bool haveRecipe =
        (GetFileAttributesA(k_recipePath) != INVALID_FILE_ATTRIBUTES);
    if (!haveRecipe) {
        ImGui::TextDisabled("Generate a base map first (Generate Map),");
        ImGui::TextDisabled("then add trees/rocks here.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Detail the visible map. Fast foliage-only regen.");
    ImGui::Separator();

    // IN PROGRESS: foliage currently only renders flat placeholder cards; real
    // prop-instance rendering is not wired up yet. Disable the whole control block
    // until it lands so nothing emits a foliage sidecar or draws placeholders.
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.f), "(in progress) - real prop foliage not yet available");
    ImGui::Separator();
    ImGui::BeginDisabled(true);

    ImGui::SetNextItemWidth(200.f * sc);
    ImGui::SliderFloat("Tree Density",  &s_fol.treeDensity, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200.f * sc);
    ImGui::SliderFloat("Clumpiness",    &s_fol.clumpiness,  0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200.f * sc);
    ImGui::SliderFloat("Bush Density",  &s_fol.bushDensity, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200.f * sc);
    ImGui::SliderFloat("Tree Line",     &s_fol.treeLine,    0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200.f * sc);
    ImGui::SliderFloat("Size Variation",&s_fol.scaleVar,    0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200.f * sc);
    ImGui::SliderFloat("Spacing",       &s_fol.spacing,     96.f, 512.f, "%.0f");
    ImGui::Checkbox("Shoreline reeds",  &s_fol.shoreline);
    ImGui::Checkbox("Scatter rocks",    &s_fol.rocks);

    ImGui::Separator();

    const bool busy = EditorTaskRunner::HasActiveTasks();
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("Generate Foliage", ImVec2(-1.f, 0.f)))
        RunFoliageGenerate();
    if (busy) ImGui::EndDisabled();

    if (ImGui::Button("Clear", ImVec2(150.f * sc, 0.f)))
        FoliageRender::Clear();
    ImGui::SameLine();
    if (ImGui::Button(FoliageRender::Visible() ? "Hide" : "Show", ImVec2(150.f * sc, 0.f)))
        FoliageRender::Toggle();

    ImGui::EndDisabled();

    if (s_fol.status[0])
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.f), "%s", s_fol.status);

    ImGui::End();
}

bool MapGeneratorDialog::GenerateReady() {
    return s_generateReady;
}

// Apply the terrain generated by the just-finished async task. MAIN THREAD ONLY,
// and the caller MUST have already reset the camera (eye->reset()) so that the
// terrain face cache primed inside generateFromDialogParams culls against the final
// view -- otherwise the terrain renders empty/black. Returns true on a clean apply.
bool MapGeneratorDialog::ApplyPendingGenerate() {
    if (!s_generateReady)
        return false;
    s_generateReady = false;

    bool applied = EditorData::generateFromDialogParams(
        s_pendingApplySizeIdx, s_pendingApplyBiome.c_str());
    if (!applied) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate succeeded but terrain apply failed.");
        return false;
    }
    EditorData::MarkMoveDataDirty();

    // Load the generated foliage sidecar so trees/rocks show immediately. Load is
    // failure-tolerant (missing/empty -> 0 instances, no crash); force-visible.
    if (s_state.generateFoliage) {
        if (FoliageRender::Load("terrain_gen_out\\genmap.foliage.json")) {
            if (!FoliageRender::Visible())
                FoliageRender::Toggle();
        }
    }

    Close();
    return true;
}

// Preview: run the generator (async) then load the thumbnail PNG on success.
// The success callback publishes the service image on the main thread.
void MapGeneratorDialog::ExecutePreview() {
    if (EditorTaskRunner::HasActiveTasks()) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "A generation task is already running.");
        return;
    }
    std::string json = BuildRecipeJSON(/*preview=*/true);
    if (!WriteRecipeFile(json)) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preview failed: could not write recipe JSON");
        return;
    }

    EditorTaskRunner::TaskSpec spec;
    spec.name        = "Preview terrain";
    spec.commandLine = BuildTerrainGenCmd(/*preview=*/true);
    spec.onSuccessMainThread = [](const EditorTaskRunner::TaskResult&) {
        LoadPreviewPNG();   // main thread: publishes service image, sets statusMsg
    };
    spec.onFailureMainThread = [](const EditorTaskRunner::TaskResult& r) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preview failed (exit %d). See Task Monitor log.", r.exitCode);
    };
    spec.onCancelMainThread = []() {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg), "Preview cancelled.");
    };
    EditorTaskRunner::StartTask(spec);
    snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
        "Running preview in background... (Task Monitor)");
}

// Generate: run the generator (async); on success apply terrain to the editor on
// the MAIN thread (generateFromDialogParams touches GL/editor state).  Outputs are
// applied ONLY when the subprocess exits 0 -- cancel/failure leaves the map untouched.
//
// TODO(phase1-atomicity): the generator writes directly into terrain_gen_out/.
// A cancel mid-write can leave partial files there, but they are never APPLIED (no
// success callback), and the next successful run overwrites them.  A future pass
// should generate into a temp dir and atomic-rename into terrain_gen_out/ on success.
void MapGeneratorDialog::ExecuteGenerate() {
    if (EditorTaskRunner::HasActiveTasks()) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "A generation task is already running.");
        return;
    }
    std::string json = BuildRecipeJSON(/*preview=*/false);
    if (!WriteRecipeFile(json)) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate failed: could not write recipe JSON");
        return;
    }

    // Snapshot the params NOW so later slider changes can't alter this run's apply.
    s_pendingApplySizeIdx = s_state.mapSizeIndex;
    s_pendingApplyBiome   = k_biomeKeys[s_state.biomeIndex];

    EditorTaskRunner::TaskSpec spec;
    spec.name        = "Generate terrain";
    spec.commandLine = BuildTerrainGenCmd(/*preview=*/false);
    spec.onSuccessMainThread = [](const EditorTaskRunner::TaskResult&) {
        // Do NOT apply here -- the apply primes the terrain face cache against the
        // current camera, so it must run AFTER eye->reset() in EditorInterface::update().
        // Just signal readiness; update() drives reset -> ApplyPendingGenerate() -> setPos.
        s_generateReady = true;
    };
    spec.onFailureMainThread = [](const EditorTaskRunner::TaskResult& r) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate failed (exit %d). See Task Monitor log.", r.exitCode);
    };
    spec.onCancelMainThread = []() {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Generate cancelled (map unchanged).");
    };
    EditorTaskRunner::StartTask(spec);
    snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
        "Generating in background... (Task Monitor)");
}

// Import: run import_gaea_height.py on an external heightfield (Gaea / Blender /
// EXR/PNG) to write terrain_gen_out/genmap.* (--flat-out, no template), then apply
// to the editor on the MAIN thread via the SAME machinery as Generate
// (s_pendingApply* -> GenerateReady() -> ApplyPendingGenerate()).
void MapGeneratorDialog::ExecuteImport() {
    if (EditorTaskRunner::HasActiveTasks()) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "A generation task is already running.");
        return;
    }
    if (s_state.importHeightPath[0] == '\0') {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Import: choose a height file first.");
        return;
    }

    // --size MUST be the vertex-side for the selected map size (cellSide + 1):
    // the apply path (initTerrainFromTGA) reads genmap.elev.r32 as
    // MapSizeToVertexSide(mapSizeIndex)^2. The importer writes its elev.r32 at
    // exactly --size^2, so these must match.
    const int sideN = EditorData::MapSizeToVertexSide(s_state.mapSizeIndex);

    // --flat-out drops genmap.* directly in terrain_gen_out\ (no <name>/ subdir),
    // exactly where EditorData::generateFromDialogParams reads them.
    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd),
        "py -3 -u tools\\terrain_gen\\import_gaea_height.py "
        "--height-file \"%s\" ",
        s_state.importHeightPath);
    if (s_state.importColorPath[0] != '\0') {
        n += snprintf(cmd + n, sizeof(cmd) - n,
            "--color-file \"%s\" ", s_state.importColorPath);
    }
    snprintf(cmd + n, sizeof(cmd) - n,
        "--name genmap --out terrain_gen_out --flat-out "
        "--size %d --biome %s --max-elev %.1f",
        sideN, k_biomeKeys[s_state.biomeIndex], (double)s_state.maxElevation);

    // Snapshot params NOW so later slider changes can't alter this run's apply
    // (identical to ExecuteGenerate -- import applies via the same path).
    s_pendingApplySizeIdx = s_state.mapSizeIndex;
    s_pendingApplyBiome   = k_biomeKeys[s_state.biomeIndex];

    EditorTaskRunner::TaskSpec spec;
    spec.name        = "Import heightfield";
    spec.commandLine = cmd;
    spec.onSuccessMainThread = [](const EditorTaskRunner::TaskResult&) {
        // Same as Generate: signal readiness; update() drives
        // eye->reset() -> ApplyPendingGenerate() -> setPos. Also refresh the
        // thumbnail from the genmap.preview.png the importer wrote.
        LoadPreviewPNG();
        s_generateReady = true;
    };
    spec.onFailureMainThread = [](const EditorTaskRunner::TaskResult& r) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Import failed (exit %d). See Task Monitor log.", r.exitCode);
    };
    spec.onCancelMainThread = []() {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Import cancelled (map unchanged).");
    };
    EditorTaskRunner::StartTask(spec);
    snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
        "Importing heightfield in background... (Task Monitor)");
}

// Load a pre-baked flat preset produced by tools/terrain_gen/gen_presets.py.
// Copies {biome}_{sizeN}/genmap.elev.r32 and genmap.burnin.tga into
// terrain_gen_out/ (where generateFromDialogParams expects them) then calls
// generateFromDialogParams() -- no Python invocation.
void MapGeneratorDialog::ExecuteLoadPreset() {
    const char* biomeKey = k_biomeKeys[s_state.biomeIndex];
    // Preset dirs are named by CELL side (user-facing).  e.g. desert_60, not desert_61.
    const int   sizeN    = EditorData::MapSizeToCellSide(s_state.mapSizeIndex);

    char srcDir[512];
    snprintf(srcDir, sizeof(srcDir), "terrain_gen_presets\\%s_%d", biomeKey, sizeN);

    // Verify manifest (written last during generation — presence guarantees data files intact)
    char srcManifest[640], srcElev[640], srcBurnin[640], srcPreview[640];
    snprintf(srcManifest, sizeof(srcManifest), "%s\\manifest.json",      srcDir);
    snprintf(srcElev,     sizeof(srcElev),     "%s\\genmap.elev.r32",    srcDir);
    snprintf(srcBurnin,   sizeof(srcBurnin),   "%s\\genmap.burnin.tga",  srcDir);
    snprintf(srcPreview,  sizeof(srcPreview),  "%s\\genmap.preview.png", srcDir);

    if (GetFileAttributesA(srcManifest) == INVALID_FILE_ATTRIBUTES) {
        bool dirExists = (GetFileAttributesA(srcDir) != INVALID_FILE_ATTRIBUTES);
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            dirExists
                ? "Preset incomplete — re-run tools/terrain_gen/gen_presets.py"
                : "Preset not found: %s", srcDir);
        return;
    }

    // Ensure output directory exists
    CreateDirectoryA("terrain_gen_out", nullptr);

    // Atomic copy: write both files to .tmp names first, then rename when both succeed.
    // This guarantees terrain_gen_out/ never holds a mixed old+new terrain pair.
    const char* dstElev   = "terrain_gen_out\\genmap.elev.r32";
    const char* dstBurnin = "terrain_gen_out\\genmap.burnin.tga";
    const char* tmpElev   = "terrain_gen_out\\genmap.elev.r32.tmp";
    const char* tmpBurnin = "terrain_gen_out\\genmap.burnin.tga.tmp";

    DeleteFileA(tmpElev);   // clear any stale temps from a prior interrupted copy
    DeleteFileA(tmpBurnin);

    bool ok = CopyFileA(srcElev,   tmpElev,   FALSE)
           && CopyFileA(srcBurnin, tmpBurnin, FALSE);

    if (!ok) {
        DeleteFileA(tmpElev);
        DeleteFileA(tmpBurnin);
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Failed to copy preset files from %s", srcDir);
        return;
    }

    // Both temp files written successfully — commit by replacing the live files.
    SetFileAttributesA(dstElev,   FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesA(dstBurnin, FILE_ATTRIBUTE_NORMAL);
    MoveFileExA(tmpElev,   dstElev,   MOVEFILE_REPLACE_EXISTING);
    MoveFileExA(tmpBurnin, dstBurnin, MOVEFILE_REPLACE_EXISTING);

    // Preview is optional; don't fail if absent
    if (GetFileAttributesA(srcPreview) != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesA("terrain_gen_out\\genmap.preview.png", FILE_ATTRIBUTE_NORMAL);
        CopyFileA(srcPreview, "terrain_gen_out\\genmap.preview.png", FALSE);
        LoadPreviewPNG();
    }

    // Hand off to editor (same path as ExecuteGenerate, no Python needed)
    bool applied = EditorData::generateFromDialogParams(s_state.mapSizeIndex, biomeKey);
    if (!applied) {
        snprintf(s_state.statusMsg, sizeof(s_state.statusMsg),
            "Preset files copied but terrain apply failed.");
    } else {
        // Build MOVE in-memory immediately so the passability grid works without
        // requiring a save/load round-trip.  land is ready at this point.
        // Mark MOVE dirty — rebuild fires in EditorInterface::update() after this
        // frame, once terrain/overlay/object state has fully settled.
        EditorData::MarkMoveDataDirty();
        Close();
    }
}

#endif // MC2_IMGUI
