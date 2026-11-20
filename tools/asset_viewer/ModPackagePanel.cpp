// tools/asset_viewer/ModPackagePanel.cpp
#include "ModPackagePanel.h"
#include "OverrideManifest.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

static std::string lowerS(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

int ModPackageInfo::blocks() const {
    int n = 0;
    for (const auto& w : problems) n += w.severity == WarnSeverity::Block;
    for (const auto& o : overrides) for (const auto& w : o.warnings) n += w.severity == WarnSeverity::Block;
    return n;
}
int ModPackageInfo::warns() const {
    int n = 0;
    for (const auto& w : problems) n += w.severity == WarnSeverity::Warn;
    for (const auto& o : overrides) for (const auto& w : o.warnings) n += w.severity == WarnSeverity::Warn;
    return n;
}

std::vector<std::string> ModPackagePanel::ListMods(const std::string& deployDir) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(fs::path(deployDir) / "mods", ec))
        if (!ec && de.is_directory() && fs::exists(de.path() / "mod.json", ec)) ids.push_back(de.path().filename().string());
    std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) { return lowerS(a) < lowerS(b); });
    return ids;
}

static std::string show(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_array()) {
        std::string s;
        for (const auto& e : v) s += (s.empty() ? "" : ", ") + show(e);
        return s.empty() ? "-" : s;
    }
    return v.dump();
}

ModPackageInfo ModPackagePanel::Read(const std::string& deployDir, const std::string& id) {
    ModPackageInfo info;
    info.id = id;
    const fs::path dir = fs::path(deployDir) / "mods" / id;
    info.dir = dir.generic_string();
    std::ifstream mf(dir / "mod.json");
    json mod = mf ? json::parse(mf, nullptr, false) : json();
    if (!mod.is_object()) {
        info.problems.push_back({WarnSeverity::Block, "manifest", "mod.json missing or not valid JSON"});
    } else {
        for (auto it = mod.begin(); it != mod.end(); ++it) info.manifest.push_back({it.key(), show(it.value())});
        if (mod.contains("dependencies") && mod["dependencies"].is_array())
            for (const auto& d : mod["dependencies"])
                if (d.is_string() && !fs::exists(fs::path(deployDir) / "mods" / d.get<std::string>()))
                    info.problems.push_back({WarnSeverity::Warn, "dependency", "dependency '" + d.get<std::string>() + "' is not installed"});
        if (mod.value("id", id) != id)
            info.problems.push_back({WarnSeverity::Warn, "id", "manifest id differs from the folder name"});
    }
    std::error_code ec;
    for (auto& de : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        ModPackageInfo::FileRow r;
        r.rel = fs::relative(de.path(), dir, ec).generic_string();
        r.size = de.file_size(ec);
        info.totalBytes += r.size;
        info.files.push_back(std::move(r));
    }
    std::sort(info.files.begin(), info.files.end(), [](const auto& a, const auto& b) { return a.rel < b.rel; });

    // models.json records through the Workbench record rules.
    const fs::path ovRoot = dir / "data" / "model_overrides";
    std::ifstream jf(ovRoot / "models.json");
    json doc = jf ? json::parse(jf, nullptr, false) : json();
    if (doc.is_object() && doc.contains("overrides") && doc["overrides"].is_array()) {
        for (const auto& rec : doc["overrides"]) {
            if (!rec.is_object()) continue;
            const std::string repl = rec.value("replaces", "");
            const size_t colon = repl.find(':');
            WorkbenchOverride w;
            w.overrideClass = colon == std::string::npos ? "" : repl.substr(0, colon);
            w.appearanceName = colon == std::string::npos ? repl : repl.substr(colon + 1);
            w.appearanceVerified = true;   // read-only browse: the roster check is the Mod Workbench's job
            w.sourceRelPath = rec.value("source", "");
            w.scale = rec.value("scale", 1.0f);
            w.renderOnly = rec.value("renderOnly", true);
            w.fallback = rec.value("fallback", "stock");
            if (rec.contains("lods") && rec["lods"].is_array())
                for (const auto& l : rec["lods"])
                    if (l.is_object()) w.lods.push_back({l.value("lod", 0), l.value("source", ""), l.value("distance", 0.0f)});
            ModPackageInfo::OverrideRow row;
            row.key = repl;
            row.source = w.sourceRelPath;
            row.lods = (int)w.lods.size();
            row.warnings = ValidateRecordRules(w);
            if (!w.sourceRelPath.empty() && !fs::exists(ovRoot / w.sourceRelPath, ec))
                row.warnings.push_back({WarnSeverity::Block, "source-missing", "source file not found: " + w.sourceRelPath});
            for (const auto& l : w.lods)
                if (!l.sourceRelPath.empty() && !fs::exists(ovRoot / l.sourceRelPath, ec))
                    row.warnings.push_back({WarnSeverity::Block, "lod-missing", "LOD" + std::to_string(l.lod) + " file not found: " + l.sourceRelPath});
            info.overrides.push_back(std::move(row));
        }
    }
    return info;
}

void ModPackagePanel::select(const std::string& id) {
    if (!built_) { built_ = true; ids_ = ListMods("."); }
    for (int i = 0; i < (int)ids_.size(); ++i)
        if (lowerS(ids_[i]) == lowerS(id)) { selected_ = i; info_ = Read(".", ids_[i]); }
}

void ModPackagePanel::drawBrowser() {
    if (!built_) { built_ = true; ids_ = ListMods("."); }
    if (ids_.empty()) { ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "No mods/ folder with a mod.json in this install"); return; }
    ImGui::Text("%d installed mods", (int)ids_.size());
    ImGui::Separator();
    for (int i = 0; i < (int)ids_.size(); ++i)
        if (ImGui::Selectable(ids_[i].c_str(), selected_ == i)) { selected_ = i; info_ = Read(".", ids_[i]); }
}

void ModPackagePanel::drawDetails() {
    if (selected_ < 0) { ImGui::TextDisabled("Select an installed mod on the left."); return; }
    const ModPackageInfo& m = info_;
    ImGui::Text("%s", m.id.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m.dir.c_str());
    if (ImGui::BeginTable("##modman", 2, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& kv : m.manifest) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(kv.first.c_str());
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", kv.second.c_str());
        }
        ImGui::EndTable();
    }
    const int blocks = m.blocks(), warns = m.warns();
    ImGui::SeparatorText("Validation");
    if (blocks == 0 && warns == 0) ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "clean: %d override records checked", (int)m.overrides.size());
    else ImGui::TextColored(blocks ? ImVec4(1.0f, 0.4f, 0.3f, 1.0f) : ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%d blocking, %d warnings", blocks, warns);
    for (const auto& w : m.problems)
        ImGui::TextColored(w.severity == WarnSeverity::Block ? ImVec4(1.0f, 0.4f, 0.3f, 1.0f) : ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[%s] %s", w.code.c_str(), w.message.c_str());
    if (!m.overrides.empty()) {
        ImGui::SeparatorText("Model overrides");
        if (ImGui::BeginTable("##modov", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Replaces");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("LODs");
            ImGui::TableSetupColumn("Validation");
            ImGui::TableHeadersRow();
            for (const auto& o : m.overrides) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(o.key.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(o.source.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", o.lods);
                ImGui::TableNextColumn();
                if (o.warnings.empty()) ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "ok");
                for (const auto& w : o.warnings)
                    ImGui::TextColored(w.severity == WarnSeverity::Block ? ImVec4(1.0f, 0.4f, 0.3f, 1.0f) : ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[%s] %s", w.code.c_str(), w.message.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::SeparatorText("Files");
    ImGui::Text("%d files, %.1f MB", (int)m.files.size(), m.totalBytes / (1024.0 * 1024.0));
    if (ImGui::BeginTable("##modfiles", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("Size");
        ImGui::TableHeadersRow();
        for (const auto& f : m.files) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.rel.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)f.size);
        }
        ImGui::EndTable();
    }
}
