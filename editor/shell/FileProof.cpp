#include "FileProof.h"
#include <fstream>
#include <stdexcept>

namespace editor::shell {
using namespace document;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::vector<std::byte> pak(const MissionDocument& doc) { return {doc.pakBytes().begin(), doc.pakBytes().end()}; }
}
void FileProof::click(std::string key, std::function<void()> verify) { steps_.push_back({std::move(key), {}, std::move(verify)}); }
void FileProof::input(std::string key, std::string value) { steps_.push_back({std::move(key), std::move(value), {}}); }
void FileProof::disk(const std::filesystem::path& stem, const std::string& fit)
{
    auto fitPath = stem; fitPath.replace_extension(".fit");
    auto pakPath = stem; pakPath.replace_extension(".pak");
    auto loaded = MissionDocument::read(fitPath, pakPath, log_);
    require(loaded && loaded->fitBytes() == fit && pak(*loaded) == originalPak_, "file_saved_pair_differs");
}
void FileProof::unchanged()
{
    require(document_.fitBytes() == changed_ && pak(document_) == originalPak_ &&
        history_.canUndo() && !history_.canRedo() && document_.modified() &&
        files_.active() == first_ && document_.revision() == changedRevision_ &&
        history_.bytesUsed() == historyBytes_ && panels_.player() == player_,
        "file_failed_or_cancelled_transition_changed_state");
    require(document_.find(panels_.player(), "DefaultTeam")->value == Value{std::int64_t{3}}, "file_selection_stale");
}
void FileProof::edit()
{
    click("player.team"); click("player.team.3");
    click("player.team.apply", [this] {
        changed_ = document_.fitBytes();
        require(changed_ != original_ && document_.modified(), "file_edit_no_change");
        auto copy = document_; require(copy.save(output_ / "edited", log_).has_value(), "file_edited_evidence_failed");
    });
    click("undo", [this] {
        require(document_.fitBytes() == original_ && !document_.modified(), "file_undo_differs");
        auto copy = document_; require(copy.save(output_ / "undone", log_).has_value(), "file_undone_evidence_failed");
    });
    click("redo", [this] {
        changedRevision_ = document_.revision(); historyBytes_ = history_.bytesUsed(); player_ = panels_.player();
        unchanged();
        auto copy = document_; require(copy.save(output_ / "redone", log_).has_value(), "file_redone_evidence_failed");
    });
}
void FileProof::path(bool saveAs, const std::filesystem::path& value)
{
    click(saveAs ? "file.save_as" : "file.open");
    input("file.path", FileWorkflow::pathText(value));
    click("file.confirm");
}
FileProof::FileProof(MissionDocument& doc, CommandHistory& history, EntityPanels& panels,
    FileWorkflow& files, std::filesystem::path output, std::string kind, Diagnostic log, std::function<void()> close)
    : document_(doc), history_(history), panels_(panels), files_(files), output_(std::move(output)),
      kind_(std::move(kind)), log_(std::move(log))
{
    const std::string seed = "FITini\n[MissionSettings]\nst MissionName = \"E4 lifecycle \"\n"
        "st Blurb2 = \"\"\nuc scenarioTuneNum = 0\n[Script]\nst ScenarioScript = \"unit\"\n"
        "[Objectives Version]\nul Version = 2\n[Objectives]\nul NumObjectives = 0\n"
        "[Parts]\nul NumParts = 0\n[Player0]\nul DefaultTeam = 0\nFITend\n";
    auto empty = encodePlacements(doc.archive(), {}); require(empty.has_value(), "file_fixture_packet_failed");
    require(history.replace(seed, std::move(*empty), "e4-fixture", log_).has_value(), "file_fixture_seed_failed");
    auto* title = doc.find("MissionSettings", "MissionName");
    require(history.setText(title->id, "E4 lifecycle " + std::string(8192, 'x') + " retained tail", "long name").has_value(), "file_long_name_failed");
    original_ = doc.fitBytes(); originalPak_ = pak(doc);
    first_ = std::filesystem::absolute(output_ / "first.fit").lexically_normal();
    second_ = std::filesystem::absolute(output_ / "second.fit").lexically_normal();
    require(files_.save(first_), "file_fixture_save_failed");
    require(doc.save(output_ / "baseline", log_).has_value(), "file_baseline_save_failed");
    require(history.setText(title->id, "Second mission", "second name").has_value(), "file_second_name_failed");
    secondFit_ = doc.fitBytes(); require(doc.save(second_, log_).has_value(), "file_second_save_failed");
    files_.request(FileWorkflow::Action::Open, first_);
    require(doc.fitBytes() == original_ && !history.canUndo(), "file_fixture_open_failed");
    panels_.resetDocument();
    edit();
    if (kind_ == "file_save") {
        click("file.save", [this] {
            disk(first_, changed_);
            require(!document_.modified() && history_.canUndo() && files_.active() == first_, "file_save_checkpoint_failed");
        });
        click("undo", [this] { require(document_.modified() && document_.fitBytes() == original_, "file_save_undo_failed"); });
        click("redo", [this] { require(!document_.modified() && document_.fitBytes() == changed_, "file_save_redo_failed"); });
        click("file.reopen", [this] { require(!history_.canUndo() && !history_.canRedo(), "file_saved_reopen_history_failed"); disk(first_, changed_); });
    } else if (kind_ == "file_save_as" || kind_ == "file_recent") {
        const auto destination = output_ / std::filesystem::path(std::u8string(u8"mission with spaces Ω ") + std::u8string(100, u8'p') + u8" retained tail.fit");
        path(true, destination);
        click("", [this, destination] {
            disk(destination, changed_);
            require(files_.active() == std::filesystem::absolute(destination) && !document_.modified() && history_.canUndo(), "file_save_as_path_failed");
            disk(first_, original_);
        });
        click("undo", [this] { require(document_.modified() && document_.fitBytes() == original_, "file_save_as_undo_failed"); });
        click("redo", [this] { require(!document_.modified() && document_.fitBytes() == changed_, "file_save_as_redo_failed"); });
        if (kind_ == "file_recent") {
            path(false, second_);
            click("", [this] { require(document_.fitBytes() == secondFit_, "file_open_second_failed"); });
            click("file.recent"); click("file.recent.1", [this, destination] {
                require(files_.active() == std::filesystem::absolute(destination) && document_.fitBytes() == changed_ && !history_.canUndo(), "file_recent_wrong_path");
                disk(destination, changed_);
            });
        } else click("file.reopen", [this, destination] { disk(destination, changed_); require(!history_.canUndo(), "file_save_as_stale_history"); });
    } else if (kind_ == "file_failures") {
        path(false, output_ / "missing.fit");
        click("dirty.discard", [this] { unchanged(); });
        const auto malformed = output_ / "malformed.fit";
        { std::ofstream bad(malformed, std::ios::binary); bad << "FITini\n"; bad.put('\0'); }
        std::filesystem::copy_file(output_ / "first.pak", output_ / "malformed.pak");
        path(false, malformed);
        click("dirty.save", [this] { unchanged(); disk(first_, original_); });
        const auto blocked = output_ / "blocked";
        { std::ofstream file(blocked); file << "not a directory"; }
        path(true, blocked / "cannot-save.fit");
        click("", [this] { unchanged(); disk(first_, original_); });
        click("file.path_cancel");
        // Invalid path text remains an owned string and is rejected without truncation.
        click("file.open"); input("file.path", std::string(8192, 'p') + " retained path tail.fit"); click("file.confirm");
        click("dirty.discard", [this] { unchanged(); });
        click("", [this] {
            std::filesystem::rename(first_, output_ / "first.retained.fit");
            std::filesystem::create_directory(first_);
            files_.request(FileWorkflow::Action::Close);
        });
        click("dirty.save", [this] { unchanged(); require(files_.pending() && !files_.closing(), "file_failed_close_save_exited"); });
        click("dirty.cancel", [this] {
            unchanged();
            std::filesystem::rename(first_, output_ / "first.blocked-directory");
            std::filesystem::rename(output_ / "first.retained.fit", first_);
        });
        click("file.save", [this] { disk(first_, changed_); });
        click("file.reopen", [this] { require(!history_.canUndo(), "file_failure_recovery_history_failed"); });
    } else {
        const bool isOpen = kind_.starts_with("file_open_");
        const bool isReopen = kind_.starts_with("file_reopen_");
        const bool isClose = kind_.starts_with("file_close_");
        require(isOpen || isReopen || isClose, "unknown_file_smoke");
        if (isOpen) path(false, second_);
        else if (isReopen) click("file.reopen");
        else click("", std::move(close));
        click("", [this] { require(files_.pending() && !files_.closing(), "file_dirty_prompt_missing"); unchanged(); });
        const std::string decision = kind_.ends_with("_save") ? "save" : kind_.ends_with("_discard") ? "discard" : "cancel";
        click("dirty." + decision, [this, decision, isOpen, isClose] {
            require(!files_.pending(), "file_dirty_transition_stuck");
            if (decision == "cancel") { unchanged(); require(!files_.closing(), "file_cancel_closed"); }
            else {
                require(files_.closing() == isClose, "file_close_not_owned");
                if (!isClose) {
                    require(document_.fitBytes() == (isOpen ? secondFit_ : decision == "save" ? changed_ : original_) &&
                        !history_.canUndo() && !history_.canRedo() && !document_.modified(), "file_transition_wrong_document_or_history");
                    require(files_.active() == (isOpen ? second_ : first_), "file_transition_wrong_path");
                    require(document_.find(panels_.player(), "DefaultTeam")->value == Value{std::int64_t{isOpen || decision == "discard" ? 0 : 3}}, "file_transition_stale_player");
                }
            }
            disk(first_, decision == "save" ? changed_ : original_);
        });
    }
}
bool FileProof::tick()
{
    require(++frames_ < 3000, "file_widget_timeout");
    if (step_ == steps_.size()) {
        if (!files_.closing()) require(ImGui::GetDrawData()->TotalVtxCount > 0, "file_render_empty");
        completed_ = true;
        // Close proofs wait for production loop termination; they cannot stop it themselves.
        return files_.closing();
    }
    auto& step = steps_[step_];
    if (step.widget.empty()) { if (step.verify) step.verify(); ++step_; return true; }
    const auto& widgets = step.widget.starts_with("file.") || step.widget.starts_with("dirty.") ? files_.widgets() : panels_.widgets();
    const auto found = widgets.find(step.widget);
    require(found != widgets.end(), "file_widget_not_rendered");
    auto& io = ImGui::GetIO();
    if (phase_ == 0) {
        priorActivations_ = found->second.activations;
        io.AddMousePosEvent(found->second.center.x, found->second.center.y);
    } else if (phase_ == 2) {
        io.AddMousePosEvent(found->second.center.x, found->second.center.y); io.AddMouseButtonEvent(0, true);
    } else if (phase_ == 4) io.AddMouseButtonEvent(0, false);
    if (!step.input.empty()) {
        if (phase_ == 6) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); }
        if (phase_ == 8) { io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddInputCharactersUTF8(step.input.c_str()); }
        if (phase_ == 10) io.AddKeyEvent(ImGuiKey_Enter, true);
        if (phase_ == 12) io.AddKeyEvent(ImGuiKey_Enter, false);
    }
    if (++phase_ > (step.input.empty() ? 6U : 14U) || files_.closing()) {
        if (!(found->second.activations > priorActivations_ || step.widget == "player.team" || step.widget == "file.recent"))
            throw std::runtime_error("file_widget_not_activated:" + step.widget);
        if (step.widget == "file.path") require(files_.pathDraft() == step.input, "file_path_input_truncated");
        log_("[EDITOR_FILE_WIDGET v1] key=" + step.widget + " reached=1");
        if (step.verify) step.verify(); ++step_; phase_ = 0;
        if (files_.closing()) {
            require(step_ == steps_.size(), "file_close_before_proof_complete");
            completed_ = true;
        }
    }
    return true;
}
void FileProof::finish(bool loopClosed)
{
    const bool expectsClose = kind_ == "file_close_save" || kind_ == "file_close_discard";
    require(completed_, "file_proof_interrupted");
    require(loopClosed == expectsClose, "file_loop_termination_missing");
    log_("[EDITOR_FILE_LOOP v1] name=" + kind_ + " terminated=" + (loopClosed ? "1" : "0") + " complete=1");
    log_("[EDITOR_FILE v1] name=" + kind_ + " result=PASS widgets=1 changed=1 undo=1 redo=1 reopened=1 state=1");
    passed_ = true;
}
}
