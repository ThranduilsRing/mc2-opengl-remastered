#pragma once
#include "MissionDocument.h"

#include <deque>

namespace editor::document {
struct SetTextCommand {
    NodeId field;
    Value before, after;
    std::string label;
};

class CommandHistory {
public:
    explicit CommandHistory(MissionDocument& document, std::size_t budget = 32 * 1024 * 1024)
        : document_(document), budget_(budget) {}
    CommandHistory(const CommandHistory&) = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;
    std::expected<void, LoadError> setText(NodeId field, std::string text, std::string label);
    bool undo();
    bool redo();
    bool canUndo() const { return cursor_ != 0; }
    bool canRedo() const { return cursor_ != commands_.size(); }
    std::size_t evicted() const { return evicted_; }
    std::size_t bytesUsed() const { return used_; }
private:
    MissionDocument& document_;
    std::deque<SetTextCommand> commands_;
    std::size_t cursor_{}, used_{}, evicted_{}, budget_;
};
}
