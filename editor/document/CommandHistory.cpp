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
}
std::expected<void, LoadError> CommandHistory::setText(NodeId id, std::string text, std::string label)
{
    const auto* field = document_.field(id);
    if (!field || field->prefix != "st" || field->value.size() != 1) return std::unexpected(LoadError{"command", "invalid_text_field", 0});
    SetTextCommand command{id, field->value, {Scalar{std::move(text)}}, std::move(label)};
    if (command.before == command.after) return {};
    if (cost(command) > budget_) return std::unexpected(LoadError{field->name, "history_budget", 0});
    auto applied = document_.assign(id, command.after);
    if (!applied) return applied;
    while (commands_.size() > cursor_) { used_ -= cost(commands_.back()); commands_.pop_back(); }
    used_ += cost(command);
    commands_.push_back(std::move(command));
    ++cursor_;
    while (used_ > budget_) { used_ -= cost(commands_.front()); commands_.pop_front(); --cursor_; ++evicted_; }
    return {};
}
bool CommandHistory::undo()
{
    if (!canUndo()) return false;
    const auto& command = commands_[cursor_ - 1];
    if (!document_.assign(command.field, command.before)) return false;
    --cursor_; return true;
}
bool CommandHistory::redo()
{
    if (!canRedo()) return false;
    const auto& command = commands_[cursor_];
    if (!document_.assign(command.field, command.after)) return false;
    ++cursor_; return true;
}
}
