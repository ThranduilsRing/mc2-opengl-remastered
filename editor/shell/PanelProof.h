#pragma once
#include "EntityPanels.h"
#include "FileWorkflow.h"
#include <memory>
#include <functional>

namespace editor::shell {
// Harness input uses rendered widget rectangles, with ordinary ImGui input events.
class PanelProof {
public:
    PanelProof(document::MissionDocument& document, document::CommandHistory& history,
        EntityPanels& panels, std::filesystem::path output, std::string kind, document::Diagnostic log,
        bool captureRendered = true);
    bool tick();
    bool passed() const { return passed_; }
private:
    struct Step { std::string widget, input; std::function<void()> verify; };
    document::MissionDocument& document_;
    document::CommandHistory& history_;
    EntityPanels& panels_;
    std::filesystem::path output_;
    std::string kind_;
    document::Diagnostic log_;
    std::vector<Step> steps_;
    std::string beforeFit_, afterFit_;
    std::vector<std::byte> beforePak_, afterPak_;
    std::size_t step_{};
    unsigned phase_{}, frames_{}, priorActivations_{};
    bool passed_{};
    bool captureRendered_{};
    std::unique_ptr<FileWorkflow> files_;
    void click(std::string key, std::function<void()> verify = {});
    void input(std::string key, std::string value);
    void operation(const std::string& name, const std::string& widget, std::function<void()> semantic);
    void saveCheck(const std::string& stem);
};
}
