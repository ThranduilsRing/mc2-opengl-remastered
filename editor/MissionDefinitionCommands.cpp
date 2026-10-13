#include "EditorInterface_internal.h"
#include <memory>

namespace
{
// Catalog refresh is a document edit, including alternate units and objective
// models. Its snapshots preserve authored values across undo and redo.
class RefreshDefinitionsAction final : public Action
{
    struct UnitSnapshot
    {
        float x, y;
        long id;
        unsigned long squad;
        std::optional<PartDefinition> part;
        std::vector<std::optional<PartDefinition>> alternatives;
        explicit UnitSnapshot(Unit& unit) : x(unit.getPosition().x), y(unit.getPosition().y),
            id(unit.getID()), squad(unit.getSquad()), part(unit.partDefinition())
        {
            for (auto it = unit.pAlternativeInstances->Begin(); !it.IsDone(); it++) alternatives.push_back((*it).partDefinition());
        }
    };
    std::vector<UnitSnapshot> units;
    TeamsAction teams;
    Unit* resolveUnit(const UnitSnapshot& saved) const
    {
        const auto all = EditorObjectMgr::instance()->getUnits();
        for (auto it = all.Begin(); !it.IsDone(); it++)
            if ((*it)->getID() == saved.id && (*it)->getSquad() == saved.squad
                && (*it)->getPosition().x == saved.x && (*it)->getPosition().y == saved.y) return *it;
        return nullptr;
    }
public:
    RefreshDefinitionsAction() : Action("Refresh mission definitions from catalog"),
        teams(EditorData::instance->TeamsRef())
    {
        const auto all = EditorObjectMgr::instance()->getUnits();
        for (auto it = all.Begin(); !it.IsDone(); it++) units.emplace_back(**it);
    }
    bool undo() override
    {
        // Undo history can outlive a deleted instance. Resolve restored units by
        // document position; changing definition state must not re-bake visuals.
        for (auto& previous : units)
        {
            auto* unit = resolveUnit(previous);
            if (!unit || static_cast<size_t>(unit->pAlternativeInstances->Count()) != previous.alternatives.size()) return false;
        }
        for (auto& previous : units)
        {
            auto& unit = *resolveUnit(previous);
            UnitSnapshot current(unit);
            unit.restorePartDefinition(previous.part);
            size_t index = 0;
            for (auto it = unit.pAlternativeInstances->Begin(); !it.IsDone(); it++) (*it).restorePartDefinition(previous.alternatives[index++]);
            previous = std::move(current);
        }
        return teams.undo();
    }
    bool redo() override { return undo(); }
};

class AuthorCameraZoomAction final : public Action
{
    std::optional<EditorCamera::MissionView> previous;
public:
    explicit AuthorCameraZoomAction(EditorCamera& camera) : Action("Use current zoom for mission camera"),
        previous(camera.missionView) {}
    bool undo() override
    {
        if (!eye) return false;
        std::swap(previous, static_cast<EditorCamera*>(eye)->missionView);
        return true;
    }
    bool redo() override { return undo(); }
};
}

bool EditorInterface::refreshMissionDefinitionsFromCatalog()
{
    if (!EditorData::instance || !land) return false;
    auto action = std::make_unique<RefreshDefinitionsAction>();
    const auto all = EditorObjectMgr::instance()->getUnits();
    for (auto it = all.Begin(); !it.IsDone(); it++) (*it)->refreshPartFromCatalog();
    for (int team = 0; team < GAME_MAX_PLAYERS; ++team)
    {
        auto& objectives = EditorData::instance->TeamsRef().TeamRef(team).ObjectivesRef();
        for (auto it = objectives.Begin(); !it.IsDone(); it++) (*it)->refreshModelFromCatalog();
    }
    undoMgr.AddAction(action.release());
    EditorData::instance->MissionNeedsSaving(true);
    return true;
}

bool EditorInterface::authorMissionCameraZoom()
{
    if (!eye || !land) return false;
    auto& camera = *static_cast<EditorCamera*>(eye);
    auto action = std::make_unique<AuthorCameraZoomAction>(camera);
    camera.missionView = camera.currentMissionView();
    undoMgr.AddAction(action.release());
    EditorData::instance->MissionNeedsSaving(true);
    return true;
}
