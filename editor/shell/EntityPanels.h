#pragma once
#include "CommandHistory.h"
#include "PlacementPolicy.h"
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
    void placementPolicy(const document::ObjectCatalog& catalog, const document::PlacementPolicy& policy,
        document::Diagnostic log = {}) {
        catalog_ = &catalog; policy_ = &policy; placementLog_ = std::move(log);
    }
    void resetDocument() {
        unit_ = {}; building_ = {}; player_ = {}; unitDraft_ = {}; buildingDraft_ = {};
        module_.clear(); status_.clear(); revision_ = ~std::uint64_t{};
        objective_ = {}; objectiveRevision_ = ~std::uint64_t{};
        presentationObjective_ = {}; presentationRevision_ = ~std::uint64_t{}; modelDraft_.reset();
        templateDraft_ = document::objectiveTemplateDraft(document::ObjectiveTemplate::hiddenTrigger);
        objectiveEntry_ = {}; entryRevision_ = ~std::uint64_t{}; showObjectiveEntries_ = false;
        refresh();
    }
    void persistence(std::function<void()> save, std::function<void()> reopen) {
        save_ = std::move(save); reopen_ = std::move(reopen);
    }
#ifdef MC2_EDITOR_SMOKE_HOST
    const std::map<std::string, Widget>& widgets() const { return widgets_; }
    const std::vector<std::string>& objectiveRows() const { return objectiveRows_; }
#endif
    const std::string& status() const { return status_; }
    document::EntityId unit() const { return unit_; }
    document::EntityId building() const { return building_; }
    document::NodeId player() const { return player_; }
    std::vector<const document::CatalogEntry*> chooserEntries() const;
    const document::CatalogEntry* chooserSelection() const;
    const std::vector<document::CatalogSource>& placementSources() const { return placementSources_; }
    static std::string mode(const document::MissionDocument& document, document::EntityId unit);
    static std::string opordSummary(const document::MissionDocument& document, document::EntityId unit);
private:
    document::MissionDocument& document_;
    document::CommandHistory& history_;
    document::EntityId unit_, building_;
    document::NodeId player_;
    document::UnitPlacement unitDraft_;
    document::BuildingPlacement buildingDraft_;
    const document::ObjectCatalog* catalog_{};
    const document::PlacementPolicy* policy_{};
    document::Diagnostic placementLog_;
    int placementPlayer_{};
    int chooserFamily_ = -1;
    std::string chooserSearch_;
    const document::CatalogEntry* chooserSelected_{};
    document::Transform chooserTransform_;
    unsigned chooserVariant_{};
    bool showChooser_ = true;
    bool showObjectives_{};
    document::NodeId objective_;
    std::uint32_t objectiveTeam_{};
    std::size_t objectiveField_{};
    document::Scalar objectiveDraft_ = std::string{};
    std::uint64_t objectiveRevision_ = ~std::uint64_t{};
    document::NodeId objectiveEntry_;
    std::uint64_t entryRevision_ = ~std::uint64_t{};
    std::string flagDraft_;
    std::int64_t flagIndexDraft_{};
    bool flagUseIndex_{}, flagValueDraft_ = true;
    bool showObjectiveEntries_{};
    std::size_t objectiveList_{};
    int conditionCreation_{};
    std::array<document::ActionDraft, 2> actionCreation_;
    std::array<bool, 2> actionCreationOpen_{};
    std::optional<document::EntityId> conditionTarget_;
    std::array<document::Scalar, 3> payloadDraft_{};
    std::vector<document::CatalogSource> placementSources_;
    std::string module_, status_;
    document::PatrolPath pathDraft_;
    std::optional<document::BrainAssignment> brainDraft_;
    std::string brainPathError_;
    void brainPaths();
    int unitTeam_{}, buildingTeam_{}, playerTeam_{};
    std::uint64_t revision_ = ~std::uint64_t{};
#ifdef MC2_EDITOR_SMOKE_HOST
    std::map<std::string, Widget> widgets_;
    std::vector<std::string> objectiveRows_;
#endif
    std::function<void()> save_, reopen_;
    bool button(std::string_view key, const char* label);
    bool choice(std::string_view key, const char* label, bool selected);
    void record(std::string_view key, bool activated = false);
    void selector(bool building);
    void team(std::string_view key, int& value, bool neutral);
    void transform(std::string_view key, document::Transform& value);
    void report(std::expected<void, document::LoadError> result);
    std::expected<document::EntityId, document::LoadError> place(bool building);
    std::expected<document::EntityId, document::LoadError> place(const document::CatalogEntry& entry,
        const document::Transform& transform, unsigned variant);
    void chooser();
    void objectives();
    void objectivePresentation();
    void objectiveTemplates();
    void objectiveTextControls(const std::string& key, document::ObjectiveTextDraft& draft);
    void objectiveModelControls(const std::string& key, std::optional<document::ObjectiveModelDraft>& draft);
    void resourceDraft(const std::string& key, std::int64_t& id);
    document::NodeId presentationObjective_;
    std::uint64_t presentationRevision_ = ~std::uint64_t{};
    std::array<document::ObjectiveTextDraft, 2> textDrafts_;
    std::optional<document::ObjectiveModelDraft> modelDraft_;
    document::ObjectiveTemplateDraft templateDraft_ = document::objectiveTemplateDraft(document::ObjectiveTemplate::hiddenTrigger);
    std::string resourceSearch_, modelSearch_;
};
}
