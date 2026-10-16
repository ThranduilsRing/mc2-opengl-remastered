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
}
