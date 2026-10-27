#include "FileWorkflow.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace editor::shell {
using namespace document;
namespace {
std::filesystem::path fitPath(std::filesystem::path path)
{
    path.replace_extension(".fit");
    return std::filesystem::absolute(path).lexically_normal();
}
std::filesystem::path fromText(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
bool validText(std::string_view text)
{
    return !text.empty() && text.find_first_of(std::string_view("\0\r\n", 3)) == text.npos;
}
}
std::string FileWorkflow::pathText(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}
FileWorkflow::FileWorkflow(MissionDocument& document, CommandHistory& history, EntityPanels& panels,
    std::filesystem::path active, Diagnostic log, std::function<void()> refreshed, std::filesystem::path recentStore)
    : document_(document), history_(history), panels_(panels), active_(fitPath(std::move(active))),
      recentStore_(std::move(recentStore)), log_(std::move(log)), refreshed_(std::move(refreshed))
{
    loadRecent();
    remember();
    panels_.persistence([this] { save(); }, [this] { request(Action::Reopen); });
}
FileWorkflow::~FileWorkflow() { panels_.persistence({}, {}); }
void FileWorkflow::reject(std::string_view reason)
{
    status_ = reason;
    if (log_) log_("[EDITOR_FILES v1] rejected=1 reason=" + status_);
}
void FileWorkflow::loadRecent()
{
    if (recentStore_.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(recentStore_, ec) && !ec) return;
    const auto size = std::filesystem::file_size(recentStore_, ec);
    if (ec || size > 12 * 32768) { reject("recent_read_failed"); return; }
    std::ifstream input(recentStore_, std::ios::binary);
    std::vector<std::filesystem::path> candidate;
    std::string line;
    try {
        while (std::getline(input, line)) {
            if (!validText(line) || candidate.size() == 12) { reject("recent_content_invalid"); return; }
            const auto path = fromText(line);
            if (!path.is_absolute() || path.extension() != ".fit" || pathText(path) != line) {
                reject("recent_content_invalid"); return;
            }
            candidate.push_back(path.lexically_normal());
        }
        if (!input.eof()) { reject("recent_read_failed"); return; }
    } catch (const std::filesystem::filesystem_error&) { reject("recent_content_invalid"); return; }
    recent_ = std::move(candidate);
}
void FileWorkflow::remember()
{
    auto candidate = recent_;
    std::erase(candidate, active_);
    candidate.insert(candidate.begin(), active_);
    if (candidate.size() > 12) candidate.resize(12);
    try {
        if (!recentStore_.empty()) {
            const auto transaction = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            auto stage = recentStore_; stage += ".stage." + transaction;
            std::ofstream output(stage, std::ios::binary | std::ios::trunc);
            for (const auto& path : candidate) output << pathText(path) << '\n';
            output.close();
            if (!output) throw std::filesystem::filesystem_error("recent_write_failed", stage, std::make_error_code(std::errc::io_error));
            // Publication must preserve the old list on failure, including on Windows.
#ifdef _WIN32
            if (!MoveFileExW(stage.c_str(), recentStore_.c_str(), MOVEFILE_REPLACE_EXISTING))
                throw std::filesystem::filesystem_error("recent_write_failed", recentStore_,
                    std::error_code(GetLastError(), std::system_category()));
#else
            std::filesystem::rename(stage, recentStore_);
#endif
        }
        recent_ = std::move(candidate);
    } catch (const std::filesystem::filesystem_error&) {
        status_ += "; recent list update failed";
        if (log_) log_("[EDITOR_FILES v1] recent_updated=0 reason=recent_write_failed");
    }
}
bool FileWorkflow::save(std::filesystem::path target)
{
    try {
        if (target.empty()) target = active_;
        if (!validText(pathText(target))) { reject("invalid_path"); return false; }
        target = fitPath(std::move(target));
        const auto result = document_.save(target, log_);
        if (!result) { status_ = result.error().reason; return false; }
        active_ = std::move(target);
        status_ = "Saved";
        remember();
        return true;
    } catch (const std::filesystem::filesystem_error&) { reject("invalid_path"); return false; }
}
void FileWorkflow::request(Action action, std::filesystem::path target)
{
    if (action == Action::None || pending() || pathPanel_ || closing_) return;
    action_ = action;
    target_ = action == Action::Reopen ? active_ : std::move(target);
    if (!document_.modified()) perform();
}
void FileWorkflow::decide(Decision decision)
{
    if (!pending()) return;
    if (decision == Decision::Cancel) { action_ = Action::None; target_.clear(); return; }
    perform(decision == Decision::Save);
}
void FileWorkflow::perform(bool saveFirst)
{
    if (action_ == Action::Close) {
        if (saveFirst && !save()) return;
        closing_ = true; action_ = Action::None; return;
    }
    try {
        if (!validText(pathText(target_))) { reject("invalid_path"); action_ = Action::None; return; }
        const auto fit = fitPath(target_);
        auto pak = fit; pak.replace_extension(".pak");
        auto loaded = MissionDocument::read(fit, pak, log_, document_.markerObjects());
        if (!loaded) { status_ = loaded.error().reason; action_ = Action::None; return; }
        if (saveFirst) {
            if (!save()) return;
            // Reopening the active pair after Save must retain the just-published values.
            if (fit == active_) *loaded = document_;
        }
        history_.adopt(std::move(*loaded));
        active_ = fit;
        panels_.resetDocument();
        if (refreshed_) refreshed_();
        status_ = "Opened";
        remember();
    } catch (const std::filesystem::filesystem_error&) { reject("invalid_path"); }
    action_ = Action::None;
    target_.clear();
}
void FileWorkflow::record(std::string_view key, bool activated)
{
#ifdef MC2_EDITOR_SMOKE_HOST
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto& widget = widgets_[std::string(key)];
    widget.center = {(a.x + b.x) / 2, (a.y + b.y) / 2};
    if (activated) ++widget.activations;
#else
    (void)key; (void)activated;
#endif
}
bool FileWorkflow::button(std::string_view key, const char* label)
{
    const bool activated = ImGui::Button(label); record(key, activated); return activated;
}
void FileWorkflow::draw()
{
    ImGui::TextUnformatted(pathText(active_).c_str());
    ImGui::BeginDisabled(pending() || pathPanel_);
    if (button("file.open", "Open...")) { pathPanel_ = true; saveAs_ = false; pathDraft_ = pathText(active_); }
    ImGui::SameLine(); if (button("file.save", "Save")) save();
    ImGui::SameLine(); if (button("file.save_as", "Save as...")) { pathPanel_ = true; saveAs_ = true; pathDraft_ = pathText(active_); }
    ImGui::SameLine(); if (button("file.reopen", "Reopen")) request(Action::Reopen);
    ImGui::SameLine(); if (button("file.close", "Close")) request(Action::Close);
    ImGui::SameLine();
    const bool recentOpen = ImGui::BeginCombo("Recent", "Choose mission"); record("file.recent", false);
    if (recentOpen) {
        for (std::size_t i = 0; i < recent_.size(); ++i) {
            const bool selected = ImGui::Selectable(pathText(recent_[i]).c_str()); record("file.recent." + std::to_string(i), selected);
            if (selected) request(Action::Open, recent_[i]);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextUnformatted(status_.c_str());
    if (pathPanel_) {
        ImGui::OpenPopup("Mission path");
        if (ImGui::BeginPopupModal("Mission path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(saveAs_ ? "Save FIT/PAK pair as" : "Open FIT/PAK pair");
            ImGui::SetNextItemWidth(900);
            const bool edited = ImGui::InputText("Path", &pathDraft_); record("file.path", edited);
            if (button("file.confirm", saveAs_ ? "Save pair" : "Open pair")) {
                try {
                    if (!validText(pathDraft_)) reject("invalid_path");
                    else if (saveAs_) { if (save(fromText(pathDraft_))) { pathPanel_ = false; ImGui::CloseCurrentPopup(); } }
                    else { pathPanel_ = false; ImGui::CloseCurrentPopup(); request(Action::Open, fromText(pathDraft_)); }
                } catch (const std::filesystem::filesystem_error&) { reject("invalid_path"); }
            }
            ImGui::SameLine(); if (button("file.path_cancel", "Cancel")) { pathPanel_ = false; ImGui::CloseCurrentPopup(); }
            ImGui::TextUnformatted(status_.c_str());
            ImGui::EndPopup();
        }
    }
    if (pending()) {
        ImGui::OpenPopup("Unsaved mission");
        if (ImGui::BeginPopupModal("Unsaved mission", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Save changes before continuing?");
            ImGui::TextUnformatted(pathText(active_).c_str());
            if (button("dirty.save", "Save changes")) { decide(Decision::Save); if (!pending()) ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(); if (button("dirty.discard", "Discard changes")) { decide(Decision::Discard); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(); if (button("dirty.cancel", "Cancel")) { decide(Decision::Cancel); ImGui::CloseCurrentPopup(); }
            ImGui::TextUnformatted(status_.c_str());
            ImGui::EndPopup();
        }
    }
}
}
