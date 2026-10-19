// Gates/render_options_cfg.h -- read and rewrite the render-option lines of an options.cfg text.
//
// WHAT: the two text operations a settings UI needs on the file the game owns: read the value of one key, and set one key in place (or add it
// to the [MechCommander2] block). Pure string functions over the file contents; the caller does the file IO.
// WHY: the launcher is a separate executable and cannot use CPrefs, yet its checkboxes are the same settings. Sharing the table in
// render_options.h and this editor keeps the launcher and the engine agreeing on key names, types and line format, and keeps a hand-edited
// or game-written options.cfg intact (every line it does not own is passed through byte for byte, line endings included).
#pragma once

#include "render_options.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::options::cfg {

namespace opt_detail = mc2::options::detail;   // trim / equalsNoCase of the options table; cfg::detail below would otherwise hide them

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
    line = opt_detail::trim(line);
    const auto gap = line.find_first_of(" \t");
    if (gap == std::string_view::npos) return false;
    const auto prefix = line.substr(0, gap);
    if (prefix.size() == 1) type = prefix[0];
    else if (prefix == "st") type = 's';
    else return false;
    std::string_view rest = opt_detail::trim(line.substr(gap));
    std::size_t k = 0;
    while (k < rest.size() && rest[k] != ' ' && rest[k] != '\t' && rest[k] != '=') ++k;
    key = rest.substr(0, k);
    rest = opt_detail::trim(rest.substr(k));
    if (rest.empty() || rest[0] != '=') return false;
    value = opt_detail::trim(rest.substr(1));
    return !key.empty();
}

}  // namespace detail

// The value text of `key` in the [MechCommander2] block, if present.
inline std::optional<std::string> get(std::string_view text, std::string_view key) {
    bool inBlock = false;
    for (std::size_t pos = 0; pos < text.size();) {
        const detail::Line l = detail::lineAt(text, pos);
        pos = l.next;
        const std::string_view line = opt_detail::trim(text.substr(l.begin, l.end - l.begin));
        if (!line.empty() && line[0] == '[') {
            inBlock = opt_detail::equalsNoCase(line, "[MechCommander2]");
            continue;
        }
        if (!inBlock) continue;
        char type = 0;
        std::string_view k, v;
        if (detail::parseLine(line, type, k, v) && opt_detail::equalsNoCase(k, key)) return std::string(v);
    }
    return std::nullopt;
}

// The type prefix of an options.cfg line ("b ShadowCSM = TRUE").
inline char typeLetter(const Option& opt) {
    static const RenderOptions kProbe{};
    return withValue(kProbe, opt, [](const auto& field) -> char {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, bool>) return 'b';
        else if constexpr (std::is_same_v<T, int>) return 'l';
        else return 'f';
    });
}

// `text` with the line of `opt` set to `value` (already validated by the caller, e.g. "TRUE", "1", "0.45"). An existing line is replaced in
// place with its own line ending; a missing one is added at the end of the [MechCommander2] block. A file without that block is returned
// unchanged: the launcher must not invent an options.cfg whose missing resolution lines would change how the game boots.
inline std::string setRaw(std::string text, std::string_view block, std::string_view key, char type, std::string_view value) {
    const std::string newLine = (type == 's' ? std::string("st") : std::string(1, type)) + " " + std::string(key) + " = " + std::string(value);
    const std::string eol = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    bool inBlock = false;
    bool sawBlock = false;
    std::size_t blockEnd = std::string::npos;   // where an added line goes: just after the block's last line
    for (std::size_t pos = 0; pos < text.size();) {
        const detail::Line l = detail::lineAt(text, pos);
        pos = l.next;
        const std::string_view line = opt_detail::trim(std::string_view(text).substr(l.begin, l.end - l.begin));
        if (opt_detail::equalsNoCase(line, "FITend")) { inBlock = false; continue; }
        if (!line.empty() && line[0] == '[') {
            inBlock = opt_detail::equalsNoCase(line, "[" + std::string(block) + "]");
            if (inBlock) { sawBlock = true; blockEnd = l.next; }
            continue;
        }
        if (!inBlock) continue;
        char type = 0;
        std::string_view k, v;
        if (detail::parseLine(line, type, k, v) && opt_detail::equalsNoCase(k, key)) {
            text.replace(l.begin, l.end - l.begin, newLine);
            return text;
        }
        if (!line.empty()) blockEnd = l.next;   // keep the add point after the last non-blank line of the block
    }
    if (!sawBlock) {
        const auto end = text.find("FITend");
        text.insert(end == std::string::npos ? text.size() : end, "[" + std::string(block) + "]" + eol + newLine + eol);
        return text;
    }
    if (blockEnd == std::string::npos) blockEnd = text.size();
    std::string add = newLine + eol;
    if (blockEnd > 0 && blockEnd <= text.size() && text[blockEnd - 1] != '\n') add = eol + add;   // the block's last line had no line break
    text.insert(blockEnd, add);
    return text;
}

inline std::string set(std::string text, const Option& opt, std::string_view value) {
    bool sawBlock = false;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto line = detail::lineAt(text, pos); pos = line.next;
        if (opt_detail::equalsNoCase(opt_detail::trim(std::string_view(text).substr(line.begin, line.end - line.begin)), "[MechCommander2]")) {
            sawBlock = true; break;
        }
    }
    return sawBlock ? setRaw(std::move(text), "MechCommander2", opt.key, typeLetter(opt), value) : text;
}

// Fill `o` from every key of the table that `text` has, under the one strict value grammar of setFromText (booleans TRUE/FALSE/1/0 in any case,
// integers and floats must be a whole number, nothing after the value). A value that fails is left at what `o` already holds and, when
// `rejects` is given, appended to it so the caller that owns the log reports it; a key that appears twice is read at its first line.
// Both the engine's pre-window publish and CPrefs::load go through here, so the two can never read the same file differently.
// Returns how many keys were applied.
inline int loadFromText(RenderOptions& o, std::string_view text, std::vector<Reject>* rejects = nullptr) {
    int applied = 0;
    for (const Option& opt : kOptions) {
        const auto v = get(text, opt.key);
        if (!v) continue;
        const Verdict why = setFromText(o, opt, *v);
        if (why == Verdict::Ok) ++applied;
        else if (rejects) rejects->push_back({opt.key, *v, why});
    }
    return applied;
}

// The whole file at `path`, or an empty string when it cannot be opened.
inline std::string readTextFile(const char* path) {
    std::string text;
    if (std::FILE* f = std::fopen(path, "rb")) {
        char chunk[4096];
        std::size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
        std::fclose(f);
    }
    return text;
}

}  // namespace mc2::options::cfg
