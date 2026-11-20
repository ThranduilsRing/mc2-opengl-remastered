#include "ArchivePanel.h"
#include "pak_zip.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void ArchivePanel::open(const std::string& path) {
    std::snprintf(path_, sizeof path_, "%s", path.c_str());
    std::string err;
    src_ = ArchiveSource::open(path, &err);
    status_ = src_ ? "opened " + std::to_string(src_->rows().size()) + " entries" : err;
    selected_.assign(src_ ? src_->rows().size() : 0, 0);
    current_ = previewRow_ = -1;
    if (const char* s = std::getenv("MC2_TOOL_ARCHIVE_SELECT"); s && src_ && std::getenv("MC2_TOOL_CAPTURE"))   // screenshot runs only
        current_ = std::min(std::atoi(s), static_cast<int>(src_->rows().size()) - 1);
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

// Button that stays on the current row only if it fits in the window's remaining width, else wraps.
static bool wrapButton(const char* label, bool first) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize(label).x + st.FramePadding.x * 2.0f;
    if (!first && ImGui::GetCursorPosX() + st.ItemSpacing.x + w <= ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
    return ImGui::Button(label);
}

void ArchivePanel::drawDetails() {
    if (!src_ || current_ < 0 || static_cast<size_t>(current_) >= src_->rows().size()) {
        ImGui::TextDisabled("Select an entry");
        return;
    }
    const size_t i = static_cast<size_t>(current_);
    const auto& row = src_->rows()[i];
    ImGui::TextWrapped("Name: %s", row.name.c_str());
    ImGui::TextWrapped("Source: %s", row.source.c_str());
    ImGui::Text("Method: %s", row.method.c_str());
    ImGui::Text("Size: %llu bytes", (unsigned long long)row.size);
    ImGui::Text("Packed: %llu bytes", (unsigned long long)row.packed);
    if (!row.aliasOf.empty()) ImGui::TextWrapped("Alias of: %s", row.aliasOf.c_str());
    ImGui::Separator();
    if (previewRow_ != current_) {
        previewRow_ = current_;
        previewBytes_.clear();
        previewIsText_ = false;
        previewTotal_ = 0;
        std::vector<uint8_t> all;
        if (src_->read(i, all)) {
            previewTotal_ = all.size();
            all.resize(std::min<size_t>(all.size(), 4096));
            previewBytes_ = std::move(all);
            previewIsText_ = true;
            for (uint8_t b : previewBytes_) if (b == 0 || (b < 32 && b != '\n' && b != '\r' && b != '\t') || b >= 127) { previewIsText_ = false; break; }
        }
    }
    if (previewBytes_.empty()) { ImGui::TextDisabled("No preview (empty or unreadable)"); return; }
    ImGui::TextDisabled("%s preview, first %zu of %zu bytes", previewIsText_ ? "Text" : "Hex", previewBytes_.size(), previewTotal_);
    ImGui::BeginChild("preview", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (previewIsText_) {
        ImGui::TextUnformatted(reinterpret_cast<const char*>(previewBytes_.data()), reinterpret_cast<const char*>(previewBytes_.data()) + previewBytes_.size());
    } else {
        for (size_t off = 0; off < previewBytes_.size(); off += 16) {
            char line[128]; int n = std::snprintf(line, sizeof line, "%04zx ", off);
            char asc[17] = {};
            for (size_t k = 0; k < 16; ++k) {
                if (off + k < previewBytes_.size()) {
                    const uint8_t b = previewBytes_[off + k];
                    n += std::snprintf(line + n, sizeof line - n, " %02x", b);
                    asc[k] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
                } else { n += std::snprintf(line + n, sizeof line - n, "   "); asc[k] = ' '; }
            }
            ImGui::Text("%s  %s", line, asc);
        }
    }
    ImGui::EndChild();
}

void ArchivePanel::draw() {
    const float lblW = ImGui::GetFontSize() * 11.0f;   // room for the right-hand input labels, scales with font/DPI
    ImGui::PushItemWidth(-lblW);
    ImGui::InputText("Archive", path_, sizeof path_);
    if (wrapButton("Open", false)) open(path_);
    if (ImGui::InputText("Filter (name or glob)", filter_, sizeof filter_)) refilter();
    ImGui::InputText("Folder", outDir_, sizeof outDir_);
    if (wrapButton("Extract selected...", true)) extract(true);
    if (wrapButton("Extract all...", false)) extract(false);
    ImGui::InputText("Pack folder", packDir_, sizeof packDir_);
    ImGui::InputText("Pack to", packOut_, sizeof packOut_);
    ImGui::PopItemWidth();
    if (wrapButton("Pack folder...", true)) {
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
    const float fs = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Packed", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthFixed, fs * 4.5f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, fs * 7.0f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip; clip.Begin(static_cast<int>(shown_.size()));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const size_t i = shown_[r]; const auto& row = src_->rows()[i];
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            bool sel = selected_[i] != 0;
            if (ImGui::Selectable((row.name + "##" + std::to_string(i)).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) { selected_[i] = !sel; current_ = static_cast<int>(i); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)row.size);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)row.packed);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.method.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.source.c_str());
        }
    ImGui::EndTable();
}
