// GameOS/gameos/vk_validation_line.h -- the engine-log line for one Khronos validation-layer message.
//
// WHAT: `formatValidationLine(severity, id, text)` returns `[VK_VALIDATION v1] severity=<error|warning|info> id=<message id> text="<one line>"`.
// WHY: the layer's messages are multi-line free text with quotes, and the smoke parser (scripts/smoke_lib/vk_validation.py) grades one line per message: an
// error is red, a warning is counted. The id is made free of whitespace and the text of quotes, newlines and tabs, and the text is capped, so a message is
// always exactly one parseable line. Header-only and Vulkan-free so the unit tests read the same code as the engine.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace mc2vk {

inline constexpr size_t kValidationTextCap = 900;

inline std::string formatValidationLine(std::string_view severity, std::string_view id, std::string_view text) {
    std::string flat(text.substr(0, kValidationTextCap));
    for (char& ch : flat) {
        if (ch == '"') ch = '\'';
        else if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    std::string safeId(id.empty() ? std::string_view("-") : id);
    for (char& ch : safeId)
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ch = '_';
    const std::string sev(severity);
    auto print = [&](char* out, size_t capacity) {
        return std::snprintf(out, capacity, "[VK_VALIDATION v1] severity=%s id=%s text=\"%s\"", sev.c_str(), safeId.c_str(), flat.c_str());
    };
    std::string line(static_cast<size_t>(print(nullptr, 0)), ' ');
    print(line.data(), line.size() + 1);
    return line;
}

}  // namespace mc2vk
