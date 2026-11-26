// tools/asset_viewer/TerrainMaterialPanel.cpp
#include "TerrainMaterialPanel.h"
#include "MaterialTextureLoader.h"
#include "tool_vk_gpu.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using nlohmann::json;

static std::string lowerS(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// File role from its name: normal-like maps load linear (Normal), ORM linear, everything else sRGB.
static int kindOf(const std::string& stem) {
    const std::string s = lowerS(stem);
    if (s.find("normal") != std::string::npos || s.find("_nra") != std::string::npos || s.find("_ah") != std::string::npos)
        return (int)MaterialSlotKind::Normal;
    if (s.find("_orm") != std::string::npos) return (int)MaterialSlotKind::Orm;
    return (int)MaterialSlotKind::BaseColor;
}

std::vector<TerrainMaterialPanel::Set> TerrainMaterialPanel::Build(const std::string& deployDir) {
    std::map<std::string, Set> layers;           // layer name -> set
    std::error_code ec;
    const fs::path data = fs::path(deployDir) / "data";
    for (auto& de : fs::directory_iterator(data / "terrain_layers", ec)) {
        if (ec || !de.is_regular_file() || lowerS(de.path().extension().string()) != ".ktx2") continue;
        std::string stem = de.path().stem().string();
        std::string layer = stem.substr(0, stem.find('_'));
        Set& s = layers[layer];
        s.name = layer;
        s.group = "terrain layer";
        s.files.push_back({stem, de.path().generic_string(), kindOf(stem)});
    }
    Set global;
    global.name = "(global terrain settings)";
    global.group = "terrain global";
    std::ifstream jf(data / "terrain_materials.json");
    json doc = jf ? json::parse(jf, nullptr, false) : json();
    if (doc.is_object()) {
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            if (!it.value().is_number()) continue;
            const std::string key = it.key();
            const double v = it.value().get<double>();
            size_t us = key.find('_');
            std::string layer = us == std::string::npos ? "" : key.substr(0, us);
            const bool known = layers.count(layer) || layer == "rock" || layer == "grass" || layer == "dirt" ||
                               layer == "concrete" || layer == "snow";
            if (!known) { global.params.push_back({key, v}); continue; }
            Set& s = layers[layer];
            if (s.name.empty()) { s.name = layer; s.group = "terrain layer"; }
            const std::string rest = key.substr(us + 1);
            if (rest == "tint_r" || rest == "tint_g" || rest == "tint_b") {
                s.hasTint = true;
                s.tint[rest.back() == 'r' ? 0 : rest.back() == 'g' ? 1 : 2] = (float)v;
            }
            s.params.push_back({rest, v});
        }
    }
    std::vector<Set> out;
    for (auto& kv : layers) out.push_back(std::move(kv.second));
    // PBR material sets: <name>_{albedo,normal,orm}.ktx2 grouped by <name>.
    std::map<std::string, Set> pbr;
    for (auto& de : fs::directory_iterator(data / "materials" / "pbr", ec)) {
        if (ec || !de.is_regular_file() || lowerS(de.path().extension().string()) != ".ktx2") continue;
        std::string stem = de.path().stem().string();
        size_t us = stem.rfind('_');
        std::string name = us == std::string::npos ? stem : stem.substr(0, us);
        Set& s = pbr[name];
        s.name = name;
        s.group = "pbr material";
        s.files.push_back({stem, de.path().generic_string(), kindOf(stem)});
    }
    for (auto& kv : pbr) out.push_back(std::move(kv.second));
    if (!global.params.empty()) out.push_back(std::move(global));
    return out;
}

TerrainMaterialPanel::~TerrainMaterialPanel() { releaseTextures(); }

void TerrainMaterialPanel::releaseTextures() {
    for (Set& s : sets_)
        for (File& f : s.files) { if (f.tex) toolvk::destroyTexture(f.tex); f.tex = 0; f.tried = false; }
}

void TerrainMaterialPanel::select(const std::string& name) {
    if (!built_) { built_ = true; sets_ = Build("."); }
    for (int i = 0; i < (int)sets_.size(); ++i)
        if (lowerS(sets_[i].name) == lowerS(name)) selected_ = i;
}

void TerrainMaterialPanel::drawBrowser() {
    if (!built_) { built_ = true; sets_ = Build("."); }
    if (sets_.empty()) { ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "No terrain layers or PBR materials found"); return; }
    ImGui::InputText("##tmfilter", filter_, sizeof(filter_));
    ImGui::Separator();
    const std::string needle = lowerS(filter_);
    for (int i = 0; i < (int)sets_.size(); ++i) {
        if (!needle.empty() && lowerS(sets_[i].name).find(needle) == std::string::npos) continue;
        char label[256];
        std::snprintf(label, sizeof(label), "%s  [%s]##%d", sets_[i].name.c_str(), sets_[i].group.c_str(), i);
        if (ImGui::Selectable(label, selected_ == i)) selected_ = i;
    }
}

void TerrainMaterialPanel::drawDetails() {
    if (selected_ < 0 || selected_ >= (int)sets_.size()) { ImGui::TextDisabled("Select a material set on the left."); return; }
    Set& s = sets_[selected_];
    ImGui::Text("%s", s.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", s.group.c_str());
    // Thumbnails: loaded once per file on first view (kept until the panel is destroyed).
    const float thumb = std::max(96.0f, std::min(256.0f, (ImGui::GetContentRegionAvail().x - 40.0f) / 3.0f));
    int col = 0;
    for (File& f : s.files) {
        if (!f.tried) { f.tried = true; f.tex = MaterialTextureLoader_Load(f.path, (MaterialSlotKind)f.kind, &f.err); }
        ImGui::BeginGroup();
        if (f.tex) ImGui::Image((ImTextureID)toolvk::imguiTex(f.tex), ImVec2(thumb, thumb));
        else ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "load failed: %s", f.err.c_str());
        ImGui::TextUnformatted(f.label.c_str());
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.path.c_str());
        if (++col % 3 != 0) ImGui::SameLine();
    }
    if (col % 3 != 0) ImGui::NewLine();
    if (s.hasTint) {
        ImGui::ColorButton("##tint", ImVec4(s.tint[0], s.tint[1], s.tint[2], 1.0f), ImGuiColorEditFlags_NoAlpha, ImVec2(24, 24));
        ImGui::SameLine();
        ImGui::TextDisabled("tint");
    }
    if (!s.params.empty() && ImGui::BeginTable("##tmparams", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Parameter");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();
        for (const auto& p : s.params) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.first.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%g", p.second);
        }
        ImGui::EndTable();
    }
}
