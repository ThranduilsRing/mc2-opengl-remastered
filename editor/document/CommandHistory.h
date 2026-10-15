#pragma once
#include "MissionDocument.h"

#include <deque>

namespace editor::document {
struct SetTextCommand {
    NodeId field;
    Value before, after;
    std::string label;
};
struct StructuralCommand {
    StructuralState before, after;
    std::string label;
};
using DocumentCommand = std::variant<SetTextCommand, StructuralCommand>;

class CommandHistory {
public:
    explicit CommandHistory(MissionDocument& document, std::size_t budget = 32 * 1024 * 1024, Diagnostic log = {})
        : document_(document), budget_(budget), log_(std::move(log)) {}
    CommandHistory(const CommandHistory&) = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;
    std::expected<void, LoadError> setText(NodeId field, std::string text, std::string label);
    std::expected<void, LoadError> setObjectiveScalar(NodeId objective, std::string_view key, Scalar value);
    std::expected<NodeId, LoadError> addObjective(std::uint32_t team);
    std::expected<NodeId, LoadError> copyObjective(NodeId objective);
    std::expected<void, LoadError> deleteObjective(NodeId objective);
    std::expected<void, LoadError> reorderObjective(NodeId objective, std::uint32_t index);
    std::expected<void, LoadError> resolveObjectiveEntry(NodeId entry, EntityId target);
    std::expected<NodeId, LoadError> addObjectiveEntry(NodeId objective, std::size_t list);
    std::expected<void, LoadError> deleteObjectiveEntry(NodeId entry);
    std::expected<void, LoadError> reorderObjectiveEntry(NodeId entry, std::uint32_t index);
    std::expected<void, LoadError> setObjectiveFlag(NodeId entry, std::optional<std::string> id,
        std::int64_t index, bool value);
    std::expected<EntityId, LoadError> insertUnit(const UnitPlacement& unit);
    std::expected<EntityId, LoadError> insertBuilding(const BuildingPlacement& building);
    std::expected<void, LoadError> erase(EntityId entity);
    std::expected<void, LoadError> transform(EntityId entity, const Transform& transform);
    std::expected<void, LoadError> setTeam(EntityId entity, std::int64_t team);
    std::expected<void, LoadError> setPlayerTeam(NodeId player, std::int64_t team);
    std::expected<void, LoadError> setBrainMode(EntityId unit, std::string_view mode);
    std::expected<void, LoadError> setLegacyBrain(EntityId unit, std::string module);
    std::expected<void, LoadError> replace(std::string fit, std::vector<std::byte> pak,
        std::string mission, const Diagnostic& log);
    void adopt(MissionDocument document);
    bool undo();
    bool redo();
    bool canUndo() const { return cursor_ != 0; }
    bool canRedo() const { return cursor_ != commands_.size(); }
    std::size_t evicted() const { return evicted_; }
    std::size_t bytesUsed() const { return used_; }
private:
    MissionDocument& document_;
    std::deque<DocumentCommand> commands_;
    std::size_t cursor_{}, used_{}, evicted_{}, budget_;
    Diagnostic log_;
    enum class ObjectiveEdit { add, copy, erase, reorder };
    LoadError rejectObjective(std::string reason);
    std::expected<NodeId, LoadError> editObjectiveEntries(ObjectiveEdit edit, NodeId objective,
        std::size_t list, NodeId entry = {}, std::uint32_t index = 0);
    std::expected<void, LoadError> commitObjectiveFit(std::string fit, std::string label,
        const std::unordered_map<std::string, std::string>& renamed = {});
    std::expected<NodeId, LoadError> editObjectives(ObjectiveEdit edit, std::uint32_t team,
        NodeId objective = {}, std::uint32_t index = 0);
    std::expected<void, LoadError> commit(DocumentCommand command);
    std::expected<void, LoadError> apply(const DocumentCommand& command, bool forward);
    std::expected<void, LoadError> setScalar(NodeId section, std::string_view key,
        std::string_view prefix, Scalar value, std::string label, bool exactReal = false);
    std::expected<void, LoadError> commitStructure(std::string fit, std::vector<std::byte> placements,
        const std::vector<EntityId>& ids, std::string label,
        const std::unordered_map<std::string, std::string>& renamed = {});
};
}
