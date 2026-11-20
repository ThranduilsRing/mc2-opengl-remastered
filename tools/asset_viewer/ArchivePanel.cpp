#include "ArchivePanel.h"
#include "pak_zip.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>

void ArchivePanel::open(const std::string& path) {
    std::snprintf(path_, sizeof path_, "%s", path.c_str());
    std::string err;
    src_ = ArchiveSource::open(path, &err);
    status_ = src_ ? "opened " + std::to_string(src_->rows().size()) + " entries" : err;
    selected_.assign(src_ ? src_->rows().size() : 0, 0);
    refilter();
}

void ArchivePanel::refilter() {
    shown_.clear();
    if (!src_) return;
    const std::string pat = std::strpbrk(filter_, "*?") ? filter_ : std::string("*") + filter_ + "*";
    for (size_t i = 0; i < src_->rows().size(); ++i)
        if (archiveGlob(pat.c_str(), src_->rows()[i].name.c_str())) shown_.push_back(i);
}

void ArchivePanel::extract(bool selectedOnly) {
    if (!src_) return;
    std::vector<size_t> rows;
    for (size_t i : shown_) if (!selectedOnly || selected_[i]) rows.push_back(i);
    size_t written = 0, refused = 0; std::string err;
    if (!archiveOutputAllowed(outDir_)) { status_ = "refused: output inside the protected install"; return; }
    const bool ok = archiveExtract(*src_, rows, outDir_, &written, &refused, &err);
    status_ = (ok ? "extracted " : "FAILED after ") + std::to_string(written) + " to " + outDir_ + (ok ? "" : ": " + err);
}

void ArchivePanel::draw() {
    ImGui::InputText("Archive", path_, sizeof path_);
    ImGui::SameLine();
    if (ImGui::Button("Open")) open(path_);
    if (ImGui::InputText("Filter (name or glob)", filter_, sizeof filter_)) refilter();
    ImGui::InputText("Folder", outDir_, sizeof outDir_);
    ImGui::SameLine();
    if (ImGui::Button("Extract selected...")) extract(true);
    ImGui::SameLine();
    if (ImGui::Button("Extract all...")) extract(false);
    ImGui::InputText("Pack folder", packDir_, sizeof packDir_);
    ImGui::InputText("Pack to", packOut_, sizeof packOut_);
    ImGui::SameLine();
    if (ImGui::Button("Pack folder...")) {
        mc2::vfs::PakWriteStats st; std::string err;
        const bool ok = archiveOutputAllowed(packOut_) && mc2::vfs::packDirectory(packDir_, packOut_, {}, &st, &err);
        status_ = ok ? "packed " + std::to_string(st.total.files) + " entries, " + std::to_string(st.total.aliases) + " aliases, " +
                       std::to_string(st.archiveBytes) + " bytes" : "pack failed: " + err;
    }
    ImGui::TextWrapped("%s", status_.c_str());
    if (!src_) { ImGui::TextDisabled("Open or drop a .mc2pak, .zip or .fst."); return; }
    ImGui::Text("%zu of %zu entries", shown_.size(), src_->rows().size());
    if (!ImGui::BeginTable("entries", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"Name", "Size", "Packed", "Method", "Source"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip; clip.Begin(static_cast<int>(shown_.size()));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const size_t i = shown_[r]; const auto& row = src_->rows()[i];
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            bool sel = selected_[i] != 0;
            if (ImGui::Selectable((row.name + "##" + std::to_string(i)).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) selected_[i] = !sel;
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)row.size);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)row.packed);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.method.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.source.c_str());
        }
    ImGui::EndTable();
}
