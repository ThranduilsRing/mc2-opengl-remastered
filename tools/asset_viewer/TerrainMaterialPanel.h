// tools/asset_viewer/TerrainMaterialPanel.h
// Terrain Materials: terrain layer texture sets (data/terrain_layers/<layer>_*.ktx2 + the layer's keys in
// data/terrain_materials.json) and PBR material sets (data/materials/pbr/<name>_{albedo,normal,orm}.ktx2).
// Thumbnails per file, parameters as a table. Read-only.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class TerrainMaterialPanel {
public:
    ~TerrainMaterialPanel();
    void drawBrowser();
    void drawDetails();
    void select(const std::string& name);
    struct File { std::string label, path; int kind = 0; uint32_t tex = 0; bool tried = false; std::string err; };
    struct Set {
        std::string name, group;                                   // group: "terrain layer" | "pbr material" | "terrain global"
        std::vector<File> files;
        std::vector<std::pair<std::string, double>> params;
        bool hasTint = false;
        float tint[3] = {0, 0, 0};
    };
    // Pure builder: scans <deployDir>/data.
    static std::vector<Set> Build(const std::string& deployDir);
private:
    void releaseTextures();
    bool built_ = false;
    std::vector<Set> sets_;
    int selected_ = -1;
    char filter_[128] = {0};
};
