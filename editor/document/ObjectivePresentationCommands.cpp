#include "CommandHistory.h"
#include <algorithm>

namespace editor::document {
std::optional<ObjectiveModelDraft> objectiveModel(const CatalogEntry& entry)
{
    // Camera types describe appearances, independently of object runtime classes.
    std::int64_t type{};
    switch (entry.appearanceType) {
    case CatalogAppearance::mech: type = 13; break;
    case CatalogAppearance::vehicle: type = 5; break;
    case CatalogAppearance::building: type = 14; break;
    case CatalogAppearance::tree: type = 15; break;
    }
    return ObjectiveModelDraft{entry.stem, type, entry.id, 2, entry.scale};
}

std::expected<void, LoadError> CommandHistory::setObjectivePresentation(NodeId id,
    std::span<const std::pair<std::string_view, Scalar>> changes, std::string label)
{
    if (!document_.objective(id)) return std::unexpected(rejectObjective("missing_objective"));
    auto draft = document_;
    CommandHistory edits(draft);
    for (const auto& [key, value] : changes) {
        auto result = edits.setObjectiveScalar(id, key, value);
        if (!result) return std::unexpected(rejectObjective(result.error().reason));
    }
    if (draft.fitBytes() == document_.fitBytes()) return {};
    return commitObjectiveFit(draft.fitBytes(), std::move(label));
}

std::expected<void, LoadError> CommandHistory::setObjectiveText(NodeId id, std::string_view key,
    const ObjectiveTextDraft& draft)
{
    if (key != "Title" && key != "Description") return std::unexpected(rejectObjective("unsupported_objective_text"));
    const auto mode = std::string(key) + "UseResourceString";
    const auto resource = std::string(key) + "ResourceStringID";
    const std::pair<std::string_view, Scalar> changes[] = {
        {key, draft.literal}, {resource, draft.id}, {mode, draft.resource}};
    return setObjectivePresentation(id, changes, "Edit objective text");
}

std::expected<void, LoadError> CommandHistory::setObjectiveModel(NodeId id, const ObjectiveModelDraft& draft)
{
    const std::pair<std::string_view, Scalar> changes[] = {
        {"ModelType", draft.type}, {"ObjectiveModel", draft.name}, {"ModelScale", draft.scale},
        {"ModelID", draft.id}, {"ModelID Version", draft.version}};
    return setObjectivePresentation(id, changes, "Edit objective model");
}
}
