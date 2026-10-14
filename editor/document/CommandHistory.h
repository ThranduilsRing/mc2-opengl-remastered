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
    explicit CommandHistory(MissionDocument& document, std::size_t budget = 32 * 1024 * 1024)
        : document_(document), budget_(budget) {}
    CommandHistory(const CommandHistory&) = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;
    std::expected<void, LoadError> setText(NodeId field, std::string text, std::string label);
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
    std::expected<void, LoadError> commit(DocumentCommand command);
    std::expected<void, LoadError> apply(const DocumentCommand& command, bool forward);
    std::expected<void, LoadError> setScalar(NodeId section, std::string_view key,
        std::string_view prefix, Scalar value, std::string label);
    std::expected<void, LoadError> commitStructure(std::string fit, std::vector<std::byte> placements,
        const std::vector<EntityId>& ids, std::string label,
        const std::unordered_map<std::string, std::string>& renamed = {});
};
}
