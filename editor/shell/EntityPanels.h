#pragma once
#include "CommandHistory.h"
#include <imgui.h>
#include <map>

namespace editor::shell {
#ifdef MC2_EDITOR_SMOKE_HOST
struct Widget {
    ImVec2 center;
    unsigned activations{};
};
#endif

// Panel drafts belong to the window; all authored mutations belong to history.
class EntityPanels {
public:
    EntityPanels(document::MissionDocument& document, document::CommandHistory& history)
        : document_(document), history_(history) {}
    void draw();
    void refresh();
    void persistence(std::filesystem::path stem, document::Diagnostic log, std::function<void()> reopened = {}) {
        saveStem_ = std::move(stem); log_ = std::move(log); reopened_ = std::move(reopened);
    }
#ifdef MC2_EDITOR_SMOKE_HOST
    const std::map<std::string, Widget>& widgets() const { return widgets_; }
#endif
    const std::string& status() const { return status_; }
    document::EntityId unit() const { return unit_; }
    document::EntityId building() const { return building_; }
    document::NodeId player() const { return player_; }
    static std::string mode(const document::MissionDocument& document, document::EntityId unit);
    static std::string opordSummary(const document::MissionDocument& document, document::EntityId unit);
private:
    document::MissionDocument& document_;
    document::CommandHistory& history_;
    document::EntityId unit_, building_;
    document::NodeId player_;
    document::UnitPlacement unitDraft_;
    document::BuildingPlacement buildingDraft_;
    std::string module_, status_;
    int unitTeam_{}, buildingTeam_{}, playerTeam_{};
    std::uint64_t revision_ = ~std::uint64_t{};
#ifdef MC2_EDITOR_SMOKE_HOST
    std::map<std::string, Widget> widgets_;
#endif
    std::filesystem::path saveStem_;
    document::Diagnostic log_;
    std::function<void()> reopened_;
    bool button(const char* key, const char* label);
    bool choice(const std::string& key, const char* label, bool selected);
    void record(const std::string& key, bool activated = false);
    void selector(bool building);
    void team(const char* key, int& value, bool neutral);
    void transform(const char* key, document::Transform& value);
    void report(std::expected<void, document::LoadError> result);
};
}
