#pragma once
#include "ObjectCatalog.h"
#include "CommandHistory.h"

namespace editor::document {
struct PlacementChoice {
    std::int32_t player{}, team{};
    Transform transform;
    unsigned variant{};
    std::int32_t forestInstance = -1, forestScale{};
};
struct PlacementPlan {
    std::variant<UnitPlacement, BuildingPlacement> value;
    std::vector<CatalogSource> sources;
    std::expected<EntityId, LoadError> insert(CommandHistory& history) const;
};

// Pilot tables and their resolved definitions belong to this policy, never legacy globals.
class PlacementPolicy {
public:
    static std::expected<PlacementPolicy, LoadError> load(const FileResolver& resolver, Diagnostic log = {});
    std::expected<PlacementPlan, LoadError> make(const MissionDocument& document,
        const CatalogEntry& entry, const PlacementChoice& choice, Diagnostic log = {}) const;
private:
    struct Pilot { std::string profile; CatalogSource table, definition; };
    std::array<std::vector<Pilot>, 2> pilots_;
};
}
