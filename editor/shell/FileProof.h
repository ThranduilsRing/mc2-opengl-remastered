#pragma once
#include "FileWorkflow.h"

namespace editor::shell {
// These scripts drive ordinary ImGui input; setup and byte assertions stay outside production.
class FileProof {
public:
    FileProof(document::MissionDocument& document, document::CommandHistory& history,
        EntityPanels& panels, FileWorkflow& files, std::filesystem::path output,
        std::string kind, document::Diagnostic log, std::function<void()> close);
    bool tick();
    void finish(bool loopClosed);
    bool completed() const { return completed_; }
    bool passed() const { return passed_; }
private:
    struct Step { std::string widget, input; std::function<void()> verify; };
    document::MissionDocument& document_;
    document::CommandHistory& history_;
    EntityPanels& panels_;
    FileWorkflow& files_;
    std::filesystem::path output_, first_, second_;
    std::string kind_, original_, changed_, secondFit_;
    std::vector<std::byte> originalPak_;
    std::uint64_t changedRevision_{};
    std::size_t historyBytes_{};
    document::NodeId player_;
    document::Diagnostic log_;
    std::vector<Step> steps_;
    std::size_t step_{};
    unsigned phase_{}, frames_{}, priorActivations_{};
    bool passed_{}, completed_{};
    void click(std::string key, std::function<void()> verify = {});
    void input(std::string key, std::string value);
    void edit();
    void path(bool saveAs, const std::filesystem::path& value);
    void disk(const std::filesystem::path& stem, std::string_view fit);
    void unchanged();
};
}
