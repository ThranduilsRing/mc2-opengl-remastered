#pragma once
#include "EntityPanels.h"

namespace editor::shell {
// File transitions own the active path; candidate loads cannot disturb live edits.
class FileWorkflow {
public:
    enum class Action { None, Open, Reopen, Close };
    enum class Decision { Save, Discard, Cancel };
    FileWorkflow(document::MissionDocument& document, document::CommandHistory& history,
        EntityPanels& panels, std::filesystem::path active, document::Diagnostic log,
        std::function<void()> refreshed = {}, std::filesystem::path recentStore = {});
    ~FileWorkflow();
    void draw();
    void request(Action action, std::filesystem::path target = {});
    void decide(Decision decision);
    bool save(std::filesystem::path target = {});
    bool closing() const { return closing_; }
    bool pending() const { return action_ != Action::None; }
    const std::filesystem::path& active() const { return active_; }
    const std::vector<std::filesystem::path>& recent() const { return recent_; }
    const std::string& status() const { return status_; }
#ifdef MC2_EDITOR_SMOKE_HOST
    const std::map<std::string, Widget>& widgets() const { return widgets_; }
    const std::string& pathDraft() const { return pathDraft_; }
#endif
    static std::string pathText(const std::filesystem::path& path);
private:
    document::MissionDocument& document_;
    document::CommandHistory& history_;
    EntityPanels& panels_;
    std::filesystem::path active_, target_, recentStore_;
    document::Diagnostic log_;
    std::function<void()> refreshed_;
    std::vector<std::filesystem::path> recent_;
    Action action_ = Action::None;
    bool closing_{}, pathPanel_{}, saveAs_{};
    std::string pathDraft_, status_;
#ifdef MC2_EDITOR_SMOKE_HOST
    std::map<std::string, Widget> widgets_;
#endif
    bool button(const std::string& key, const char* label);
    void record(const std::string& key, bool activated);
    void perform(bool saveFirst = false);
    void remember();
    void loadRecent();
    void reject(std::string_view reason);
};
}
