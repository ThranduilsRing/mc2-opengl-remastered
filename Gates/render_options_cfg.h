// Gates/render_options_cfg.h -- read and rewrite the render-option lines of an options.cfg text.
//
// WHAT: the two text operations a settings UI needs on the file the game owns: read the value of one key, and set one key in place (or add it
// to the [MechCommander2] block). Pure string functions over the file contents; the caller does the file IO.
// WHY: the launcher is a separate executable and cannot use CPrefs, yet its checkboxes are the same settings. Sharing the table in
// render_options.h and this editor keeps the launcher and the engine agreeing on key names, types and line format, and keeps a hand-edited
// or game-written options.cfg intact (every line it does not own is passed through byte for byte, line endings included).
#pragma once

#include "render_options.h"

#include <optional>
#include <string>
#include <string_view>

namespace mc2::options::cfg {

namespace detail {

struct Line {
    std::size_t begin = 0;   // first byte of the line
    std::size_t end = 0;     // one past the last byte before the line break
    std::size_t next = 0;    // first byte of the next line
};

// The next line of `text` from `pos`.
inline Line lineAt(std::string_view text, std::size_t pos) {
    Line l;
    l.begin = pos;
    std::size_t i = pos;
    while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
    l.end = i;
    if (i < text.size() && text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i += 2;
    else if (i < text.size()) i += 1;
    l.next = i;
    return l;
}

// "<type> <key> = <value>" -> true with the pieces; the key match is case-insensitive like the engine's reader.
inline bool parseLine(std::string_view line, char& type, std::string_view& key, std::string_view& value) {
    line = mc2::options::detail::trim(line);
    if (line.size() < 5 || (line[1] != ' ' && line[1] != '\t')) return false;
    type = line[0];
    std::string_view rest = mc2::options::detail::trim(line.substr(2));
    std::size_t k = 0;
    while (k < rest.size() && rest[k] != ' ' && rest[k] != '\t' && rest[k] != '=') ++k;
    key = rest.substr(0, k);
    rest = mc2::options::detail::trim(rest.substr(k));
    if (rest.empty() || rest[0] != '=') return false;
    value = mc2::options::detail::trim(rest.substr(1));
    return !key.empty();
}

}  // namespace detail

// The value text of `key` in the [MechCommander2] block, if present.
inline std::optional<std::string> get(std::string_view text, std::string_view key) {
    bool inBlock = false;
    for (std::size_t pos = 0; pos < text.size();) {
        const detail::Line l = detail::lineAt(text, pos);
        pos = l.next;
        const std::string_view line = mc2::options::detail::trim(text.substr(l.begin, l.end - l.begin));
        if (!line.empty() && line[0] == '[') {
            inBlock = mc2::options::detail::equalsNoCase(line, "[MechCommander2]");
            continue;
        }
        if (!inBlock) continue;
        char type = 0;
        std::string_view k, v;
        if (detail::parseLine(line, type, k, v) && mc2::options::detail::equalsNoCase(k, key)) return std::string(v);
    }
    return std::nullopt;
}

inline char typeLetter(const Option& opt) {
    switch (opt.kind) {
    case Kind::Bool: return 'b';
    case Kind::Long: return 'l';
    case Kind::Float: return 'f';
    }
    return 'b';
}

// `text` with the line of `opt` set to `value` (already validated by the caller, e.g. "TRUE", "1", "0.45"). An existing line is replaced in
// place with its own line ending; a missing one is added at the end of the [MechCommander2] block. A file without that block is returned
// unchanged: the launcher must not invent an options.cfg whose missing resolution lines would change how the game boots.
inline std::string set(std::string text, const Option& opt, std::string_view value) {
    const std::string newLine = std::string(1, typeLetter(opt)) + " " + opt.key + " = " + std::string(value);
    const std::string eol = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    bool inBlock = false;
    bool sawBlock = false;
    std::size_t blockEnd = std::string::npos;   // where an added line goes: just after the block's last line
    for (std::size_t pos = 0; pos < text.size();) {
        const detail::Line l = detail::lineAt(text, pos);
        pos = l.next;
        const std::string_view line = mc2::options::detail::trim(std::string_view(text).substr(l.begin, l.end - l.begin));
        if (!line.empty() && line[0] == '[') {
            inBlock = mc2::options::detail::equalsNoCase(line, "[MechCommander2]");
            if (inBlock) { sawBlock = true; blockEnd = l.next; }
            continue;
        }
        if (!inBlock) continue;
        char type = 0;
        std::string_view k, v;
        if (detail::parseLine(line, type, k, v) && mc2::options::detail::equalsNoCase(k, opt.key)) {
            text.replace(l.begin, l.end - l.begin, newLine);
            return text;
        }
        if (!line.empty()) blockEnd = l.next;   // keep the add point after the last non-blank line of the block
    }
    if (!sawBlock) return text;
    if (blockEnd == std::string::npos) blockEnd = text.size();
    std::string add = newLine + eol;
    if (blockEnd > 0 && blockEnd <= text.size() && text[blockEnd - 1] != '\n') add = eol + add;   // the block's last line had no line break
    text.insert(blockEnd, add);
    return text;
}

}  // namespace mc2::options::cfg
