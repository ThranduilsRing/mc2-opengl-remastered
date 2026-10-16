#include "CommandHistory.h"
#include <algorithm>

namespace editor::document {
ObjectiveTemplateDraft objectiveTemplateDraft(ObjectiveTemplate recipe)
{
    ObjectiveTemplateDraft draft;
    draft.recipe = recipe;
    switch (recipe) {
    case ObjectiveTemplate::hiddenTrigger:
        draft.title.literal = "Hidden trigger";
        draft.description.literal = "Flag-driven hidden trigger. Set the activation/condition flags.";
        break;
    case ObjectiveTemplate::destroyEnemies:
        draft.title.literal = "Destroy enemy units";
        draft.description.literal = "Destroy the required number of enemy units.";
        break;
    case ObjectiveTemplate::moveToArea:
        draft.title.literal = "Move to area";
        draft.description.literal = "Move any unit into the target area. Set the area center via Edit.";
        break;
    }
    return draft;
}

std::expected<NodeId, LoadError> CommandHistory::addObjectiveTemplate(std::uint32_t team,
    const ObjectiveTemplateDraft& parameters)
{
    // Staging keeps payload validation and the entire recipe inside one undo boundary.
    auto draft = document_;
    CommandHistory edits(draft);
    const auto apply = [&]() -> std::expected<NodeId, LoadError> {
        auto id = edits.addObjective(team);
        if (!id) return id;
        if (auto result = edits.setObjectiveText(*id, "Title", parameters.title); !result) return std::unexpected(result.error());
        if (auto result = edits.setObjectiveText(*id, "Description", parameters.description); !result) return std::unexpected(result.error());
        if (parameters.model) {
            if (auto result = edits.setObjectiveModel(*id, *parameters.model); !result) return std::unexpected(result.error());
        }
        switch (parameters.recipe) {
        case ObjectiveTemplate::hiddenTrigger: {
            if (auto result = edits.setObjectiveScalar(*id, "HiddenTrigger", true); !result) return std::unexpected(result.error());
            auto condition = edits.addObjectiveEntry(*id, 0);
            if (!condition) return std::unexpected(condition.error());
            if (auto result = edits.setObjectiveFlag(*condition, parameters.conditionFlag, 0, parameters.conditionValue); !result)
                return std::unexpected(result.error());
            auto action = edits.addObjectiveAction(*id, 1,
                {ActionSpecies::flag, {parameters.actionFlag, parameters.actionValue}});
            if (!action) return std::unexpected(action.error());
            break;
        }
        case ObjectiveTemplate::destroyEnemies: {
            auto condition = edits.addObjectiveCondition(*id, 0, ConditionSpecies::destroyNumber);
            if (!condition) return std::unexpected(condition.error());
            if (auto result = edits.setObjectivePayload(*condition, "Num", parameters.count); !result) return std::unexpected(result.error());
            break;
        }
        case ObjectiveTemplate::moveToArea: {
            auto condition = edits.addObjectiveCondition(*id, 0, ConditionSpecies::moveAny);
            if (!condition) return std::unexpected(condition.error());
            for (const auto& [key, value] : {std::pair{"TargetCenterX", parameters.centerX},
                {"TargetCenterY", parameters.centerY}, {"TargetRadius", parameters.radius}}) {
                if (auto result = edits.setObjectivePayload(*condition, key, value); !result) return std::unexpected(result.error());
            }
            break;
        }
        default: return std::unexpected(LoadError{"objective", "unsupported_objective_template", 0});
        }
        return id;
    };
    auto result = apply();
    if (!result) return std::unexpected(rejectObjective(result.error().reason));
    const auto index = draft.objective(*result)->exportedIndex;
    if (auto committed = commitObjectiveFit(draft.fitBytes(), "Add objective from template"); !committed)
        return std::unexpected(committed.error());
    for (const auto& objective : document_.objectives())
        if (objective.team == team && objective.exportedIndex == index) return objective.section;
    return std::unexpected(rejectObjective("missing_added_objective"));
}

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
