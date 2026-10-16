#pragma once
#include "ObjectCatalog.h"

namespace editor::document {
struct ObjectiveTextDraft {
    bool resource{};
    std::string literal;
    std::int64_t id{};
};
struct ObjectiveModelDraft {
    std::string name;
    std::int64_t type{}, id{}, version = 2;
    double scale = 1;
};
std::optional<ObjectiveModelDraft> objectiveModel(const CatalogEntry& entry);
enum class ObjectiveTemplate { hiddenTrigger, destroyEnemies, moveToArea };
struct ObjectiveTemplateDraft {
    ObjectiveTemplate recipe = ObjectiveTemplate::hiddenTrigger;
    ObjectiveTextDraft title, description;
    std::optional<ObjectiveModelDraft> model;
    std::string conditionFlag = "flag0", actionFlag = "flag0";
    bool conditionValue = true, actionValue = true;
    std::int64_t count = 1;
    double centerX{}, centerY{}, radius = 256;
};
ObjectiveTemplateDraft objectiveTemplateDraft(ObjectiveTemplate recipe);
}
