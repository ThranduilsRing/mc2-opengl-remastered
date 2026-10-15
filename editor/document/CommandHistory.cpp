#include "CommandHistory.h"

namespace editor::document {
namespace {
std::size_t cost(const SetTextCommand& command)
{
    std::size_t size = sizeof(command) + command.label.capacity();
    for (const auto* values : {&command.before, &command.after}) {
        size += values->capacity() * sizeof(Scalar);
        for (const auto& value : *values) if (const auto* text = std::get_if<std::string>(&value)) size += text->capacity();
    }
    return size;
}
std::size_t cost(const StructuralState& state)
{
    std::size_t size = sizeof(state) + state.syntax.originalBytes.capacity() + state.placements.capacity() +
        state.entities.capacity() * sizeof(Entity) + state.objectives.capacity() * sizeof(Objective) +
        state.syntax.fields.capacity() * sizeof(Field) + state.syntax.sections.capacity() * sizeof(Section);
    for (const auto& field : state.syntax.fields) {
        size += field.name.capacity() + field.prefix.capacity();
        for (const auto* values : {&field.original, &field.value}) {
            size += values->capacity() * sizeof(Scalar);
            for (const auto& v : *values) if (const auto* text = std::get_if<std::string>(&v)) size += text->capacity();
        }
    }
    for (const auto& section : state.syntax.sections) size += section.name.capacity() + section.fields.capacity() * sizeof(NodeId);
    for (const auto& entity : state.entities) {
        if (entity.objectProfile) size += entity.objectProfile->capacity();
        if (entity.csvFile) size += entity.csvFile->capacity();
    }
    for (const auto& objective : state.objectives) {
        for (const auto& list : objective.entries) size += list.capacity() * sizeof(ObjectiveEntry);
        size += objective.scalars.capacity() * sizeof(ObjectiveScalar);
        for (const auto& scalar : objective.scalars)
            if (const auto* text = std::get_if<std::string>(&scalar.value)) size += text->capacity();
    }
    return size;
}
std::size_t cost(const DocumentCommand& command)
{
    return std::visit([](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, SetTextCommand>) return cost(c) + sizeof(DocumentCommand) - sizeof(c);
        else return sizeof(c) + c.label.capacity() + cost(c.before) + cost(c.after);
    }, command);
}
}
std::expected<void, LoadError> CommandHistory::replace(std::string fit, std::vector<std::byte> pak,
    std::string mission, const Diagnostic& log)
{
    auto candidate = MissionDocument::decode(std::move(fit), std::move(pak), std::move(mission), log, document_.markerObjects());
    if (!candidate) return std::unexpected(candidate.error());
    adopt(std::move(*candidate));
    return {};
}
void CommandHistory::adopt(MissionDocument document)
{
    document_ = std::move(document);
    commands_.clear();
    cursor_ = used_ = evicted_ = 0;
}
std::expected<void, LoadError> CommandHistory::setText(NodeId id, std::string text, std::string label)
{
    const auto* field = document_.field(id);
    if (!field || field->prefix != "st" || field->value.size() != 1) return std::unexpected(LoadError{"command", "invalid_text_field", 0});
    SetTextCommand command{id, field->value, {Scalar{std::move(text)}}, std::move(label)};
    if (command.before == command.after) return {};
    return commit(std::move(command));
}
std::expected<void, LoadError> CommandHistory::apply(const DocumentCommand& command, bool forward)
{
    return std::visit([&](const auto& c) -> std::expected<void, LoadError> {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, SetTextCommand>) return document_.assign(c.field, forward ? c.after : c.before);
        else return document_.restore(forward ? c.after : c.before);
    }, command);
}
std::expected<void, LoadError> CommandHistory::commit(DocumentCommand command)
{
    if (cost(command) > budget_) return std::unexpected(LoadError{"command", "history_budget", 0});
    auto applied = apply(command, true);
    if (!applied) return applied;
    while (commands_.size() > cursor_) { used_ -= cost(commands_.back()); commands_.pop_back(); }
    used_ += cost(command);
    commands_.push_back(std::move(command));
    ++cursor_;
    while (used_ > budget_) { used_ -= cost(commands_.front()); commands_.pop_front(); --cursor_; ++evicted_; }
    return {};
}
std::expected<void, LoadError> CommandHistory::commitStructure(std::string fit, std::vector<std::byte> placements,
    const std::vector<EntityId>& ids, std::string label,
    const std::unordered_map<std::string, std::string>& renamed)
{
    auto candidate = document_.structuralCandidate(std::move(fit), std::move(placements), ids, renamed);
    if (!candidate) {
        if (log_) log_("[EDITOR_ENTITY_REJECT v1] action=structure reason=" + candidate.error().reason);
        return std::unexpected(candidate.error());
    }
    return commit(StructuralCommand{document_.structuralState(), std::move(*candidate), std::move(label)});
}
bool CommandHistory::undo()
{
    if (!canUndo()) return false;
    const auto& command = commands_[cursor_ - 1];
    if (!apply(command, false)) return false;
    --cursor_; return true;
}
bool CommandHistory::redo()
{
    if (!canRedo()) return false;
    const auto& command = commands_[cursor_];
    if (!apply(command, true)) return false;
    ++cursor_; return true;
}
}
