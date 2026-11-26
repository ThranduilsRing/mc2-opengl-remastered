// tools/asset_viewer/VfxPanel.cpp
#include "VfxPanel.h"
#include "MaterialTextureLoader.h"
#include "tool_vk_gpu.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::string lowerS(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

const char* VfxPanel::ClassName(uint32_t classId) {
    // gosfx.hpp ClassID enum order, relative to first_effect_class.
    static const char* kNames[] = {"Effect", "ParticleCloud", "PointCloud", "SpinningCloud", "ShardCloud", "PertCloud",
                                   "CardCloud", "ShapeCloud", "EffectCloud", "Singleton", "Card", "Shape", "Tube",
                                   "DebrisCloud", "PointLight"};
    const uint32_t ord = classId - mc2::fx::first_effect_class;
    return ord < sizeof(kNames) / sizeof(kNames[0]) ? kNames[ord] : "Unknown";
}

int VfxPanel::Load(const std::string& deployDir, std::vector<std::string>& errors) {
    std::vector<fs::path> dirs{fs::path(deployDir) / "data" / "effects"};
    std::error_code ec;
    for (auto& de : fs::directory_iterator(fs::path(deployDir) / "mods", ec))
        if (!ec && de.is_directory()) dirs.push_back(de.path() / "data" / "effects");
    for (const fs::path& dir : dirs) {
        for (auto& de : fs::directory_iterator(dir, ec)) {
            if (ec || !de.is_regular_file() || lowerS(de.path().extension().string()) != ".fx") continue;
            std::ifstream f(de.path(), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string err;
            auto cat = mc2::fx::read_catalog(bytes, err);
            const std::string label = de.path().parent_path().parent_path().parent_path().filename().string() == "mods"
                                          ? de.path().parent_path().parent_path().parent_path().filename().string() + "/" +
                                                de.path().parent_path().parent_path().filename().string() + "/" + de.path().filename().string()
                                          : de.path().filename().string();
            if (!cat) { errors.push_back(label + ": " + err); continue; }
            catalogs_.push_back(std::make_unique<mc2::fx::Catalog>(std::move(*cat)));
            const int fi = (int)catalogs_.size() - 1;
            for (int i = 0; i < (int)catalogs_.back()->effects().size(); ++i) entries_.push_back({label, fi, i});
        }
    }
    return (int)entries_.size();
}

VfxPanel::~VfxPanel() {
    for (auto& kv : thumbs_) if (kv.second) toolvk::destroyTexture(kv.second);
}

// Effect textures are tga names; the cooked copies sit with the model textures (data/tgl/<tier>/<stem>.ktx2).
uint32_t VfxPanel::thumb(const std::string& texName) {
    auto it = thumbs_.find(texName);
    if (it != thumbs_.end()) return it->second;
    std::string stem = lowerS(texName);
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    uint32_t id = 0;
    for (const char* tier : {"512", "1024", "256", "128"}) {
        const std::string p = std::string("data/tgl/") + tier + "/" + stem + ".ktx2";
        if (fs::exists(p)) { std::string e; id = MaterialTextureLoader_Load(p, MaterialSlotKind::BaseColor, &e); if (id) break; }
    }
    thumbs_[texName] = id;
    return id;
}

void VfxPanel::select(const std::string& name) {
    if (!built_) { built_ = true; Load(".", errors_); }
    for (int i = 0; i < (int)entries_.size(); ++i)
        if (lowerS(record(entries_[i]).name) == lowerS(name)) { selected_ = i; return; }
}

void VfxPanel::drawBrowser() {
    if (!built_) { built_ = true; Load(".", errors_); }
    if (entries_.empty()) {
        ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "No effects decoded");
        for (const auto& e : errors_) ImGui::TextWrapped("%s", e.c_str());
        return;
    }
    ImGui::InputText("##vfxfilter", filter_, sizeof(filter_));
    ImGui::Text("%d effects", (int)entries_.size());
    ImGui::Separator();
    const std::string needle = lowerS(filter_);
    ImGui::BeginChild("##vfxlist", ImVec2(0, 0), false);
    for (int i = 0; i < (int)entries_.size(); ++i) {
        const auto& r = record(entries_[i]);
        if (!needle.empty() && lowerS(r.name).find(needle) == std::string::npos) continue;
        char label[256];
        std::snprintf(label, sizeof(label), "%s  [%s]##%d", r.name.c_str(), ClassName(r.class_id), i);
        if (ImGui::Selectable(label, selected_ == i)) selected_ = i;
    }
    ImGui::EndChild();
}

void VfxPanel::drawDetails() {
    if (selected_ < 0 || selected_ >= (int)entries_.size()) { ImGui::TextDisabled("Select an effect on the left."); return; }
    const Entry& en = entries_[selected_];
    const mc2::fx::EffectRecord& r = record(en);
    ImGui::Text("%s", r.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]  %s", ClassName(r.class_id), en.file.c_str());
    if (ImGui::BeginTable("##vfxp", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
        };
        row("events", std::to_string(r.events.size()));
        row("population limit", std::to_string(r.population_limit));
        row("profile type", std::to_string(r.profile_type));
        row("vertex count", std::to_string(r.vertex_count));
        row("child effect", std::to_string(r.child_effect));
        row("flags", std::string(r.animated ? "animated " : "") + (r.inside_out ? "inside-out " : "") + (r.two_sided ? "two-sided" : ""));
        row("radius", std::to_string(r.radius));
        row("geometry pieces", std::to_string(r.geometry.size()));
        row("texture", r.material.texture.empty() ? "-" : r.material.texture);
        row("light map", r.light_map.empty() ? "-" : r.light_map);
        ImGui::EndTable();
    }
    if (!r.material.texture.empty()) {
        if (uint32_t t = thumb(r.material.texture)) ImGui::Image((ImTextureID)toolvk::imguiTex(t), ImVec2(128, 128));
        else ImGui::TextDisabled("(texture %s not found in data/tgl/<tier>)", r.material.texture.c_str());
    }
    ImGui::SeparatorText("Curves");
    if (r.curves.empty()) { ImGui::TextDisabled("none"); return; }
    if (ImGui::BeginTable("##vfxc", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Curve");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Age 0");
        ImGui::TableSetupColumn("Age 0.5");
        ImGui::TableSetupColumn("Age 1");
        ImGui::TableSetupColumn("Plot");
        ImGui::TableHeadersRow();
        static const char* kKinds[] = {"constant", "linear", "spline", "complex"};
        for (const auto& c : r.curves) {
            float plot[24];
            for (int k = 0; k < 24; ++k) plot[k] = c.evaluate(k / 23.0f, 0.5f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s", kKinds[(int)c.age.kind], c.seeded ? " +seed" : "");
            ImGui::TableNextColumn(); ImGui::Text("%.3g", plot[0]);
            ImGui::TableNextColumn(); ImGui::Text("%.3g", c.evaluate(0.5f, 0.5f));
            ImGui::TableNextColumn(); ImGui::Text("%.3g", plot[23]);
            ImGui::TableNextColumn(); ImGui::PlotLines(("##p" + c.name).c_str(), plot, 24, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(120, 18));
        }
        ImGui::EndTable();
    }
}
