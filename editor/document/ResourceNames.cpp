#include "ObjectCatalog.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <memory>

namespace editor::document {
namespace {
struct Names {
    struct String { std::string text, source; };
    std::unordered_map<std::string, String> strings;
    std::unordered_map<std::int64_t, std::string> ids, aliases;
};
std::expected<std::string, LoadError> withoutComments(std::string text)
{
    bool quote{}, block{}, line{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (line) { if (c == '\n') line = false; else text[i] = ' '; }
        else if (block) {
            if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') { text[i++] = ' '; text[i] = ' '; block = false; }
            else if (c != '\n') text[i] = ' ';
        } else if (quote && c == '\\' && i + 1 < text.size()) ++i;
        else if (c == '"') quote = !quote;
        else if (!quote && c == '/' && i + 1 < text.size()) {
            if (text[i + 1] == '*') { text[i++] = ' '; text[i] = ' '; block = true; }
            else if (text[i + 1] == '/') { text[i++] = ' '; text[i] = ' '; line = true; }
        }
    }
    if (block) return std::unexpected(LoadError{"resources", "unterminated_comment", 0});
    return text;
}
using Fields = std::unordered_map<std::string, std::string>;
std::expected<Fields, LoadError> fields(std::string_view body)
{
    Fields result;
    std::size_t p{};
    const auto bad = [&] { return std::unexpected(LoadError{"resources", "malformed_field", p}); };
    const auto white = [&] { while (p < body.size() && (std::isspace(static_cast<unsigned char>(body[p])) || body[p] == ';')) ++p; };
    while (p < body.size()) {
        white();
        if (p == body.size()) break;
        const auto begin = p;
        while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) ++p;
        if (begin == p) return bad();
        const auto name = identifier(body.substr(begin, p - begin));
        white();
        if (p == body.size() || body[p++] != '=') return bad();
        white();
        if (p == body.size()) return bad();
        std::string value;
        if (body[p] == '"') {
            ++p;
            bool closed{};
            while (p < body.size()) {
                auto c = body[p++];
                if (c == '"') { closed = true; break; }
                if (c == '\\') {
                    if (p == body.size()) return bad();
                    c = body[p++];
                    if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
                }
                value += c;
            }
            if (!closed) return bad();
        } else {
            const auto start = p;
            while (p < body.size() && !std::isspace(static_cast<unsigned char>(body[p])) && body[p] != ';') ++p;
            value = body.substr(start, p - start);
        }
        if (p < body.size() && body[p] != ';' && body[p] != '\r' && body[p] != '\n' && body[p] != ' ' && body[p] != '\t') return bad();
        if (!result.emplace(name, value).second) return bad();
    }
    return result;
}
std::optional<std::int64_t> id(std::string_view value)
{
    std::int64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result <= 0) return {};
    return result;
}
std::size_t brace(std::string_view text, std::size_t begin, bool closing)
{
    bool quote{};
    std::size_t depth = closing ? 1 : 0;
    for (auto p = begin; p < text.size(); ++p) {
        const auto c = text[p];
        if (quote && c == '\\' && p + 1 < text.size()) ++p;
        else if (c == '"') quote = !quote;
        else if (!quote && c == '{') { if (!closing) return p; ++depth; }
        else if (!quote && c == '}' && closing && --depth == 0) return p;
    }
    return text.npos;
}
}

ObjectCatalog::ResourceName resourceNames(const FileResolver& resolver, Diagnostic log)
{
    auto names = std::make_shared<Names>();
    std::vector<ResolvedFile> files;
    for (const auto prefix : {"data/defs/text/en_us/", "data/defs/debug/", "data/defs/menus/game/", "data/defs/menus/viewer/"})
        for (const auto& key : resolver.keys(prefix))
            if (key.ends_with(".fit") && key.find('/', std::string_view(prefix).size()) == key.npos) {
                auto file = resolver.read(key);
                if (file) files.push_back(std::move(*file));
                else if (log) log("[EDITOR_RESOURCE_REJECT v1] file=" + key + " reason=" + file.error().reason);
            }
    // Base strings precede mod additions; explicit aliases retain the game's last-wins policy.
    std::stable_partition(files.begin(), files.end(), [](const auto& file) { return !file.layer.starts_with("mod:"); });
    for (const auto& file : files) {
        auto content = withoutComments({reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()});
        if (!content) { if (log) log("[EDITOR_RESOURCE_REJECT v1] file=" + file.key + " reason=" + content.error().reason); continue; }
        std::size_t pos{}, ordinal{};
        while (pos < content->size()) {
            const auto open = brace(*content, pos, false);
            if (open == content->npos) break;
            auto end = open;
            while (end > pos && std::isspace(static_cast<unsigned char>((*content)[end - 1]))) --end;
            auto start = end;
            while (start > pos && (std::isalnum(static_cast<unsigned char>((*content)[start - 1])) || (*content)[start - 1] == '_')) --start;
            const auto kind = identifier(std::string_view(*content).substr(start, end - start));
            const auto close = brace(*content, open + 1, true);
            ++ordinal;
            if (close == content->npos) { if (log) log("[EDITOR_RESOURCE_REJECT v1] file=" + file.key + " reason=missing_block_end"); break; }
            const auto body = std::string_view(*content).substr(open + 1, close - open - 1);
            pos = close + 1;
            if (kind != "string" && kind != "legacytextalias") continue;
            auto parsed = fields(body);
            if (!parsed) { if (log) log("[EDITOR_RESOURCE_REJECT v1] file=" + file.key + " entry=" + std::to_string(ordinal) + " reason=" + parsed.error().reason); continue; }
            auto& f = *parsed;
            const auto numeric = id(f["legacyid"]);
            if (kind == "legacytextalias" && numeric) names->aliases[*numeric] = f["textkey"];
            else if (kind == "string") {
                const auto stringKey = f["key"];
                if (stringKey.empty()) continue;
                auto value = f["text"];
                if (value.empty()) value = f["value"];
                const auto source = f["source"];
                names->strings.try_emplace(stringKey, Names::String{value, source});
                if (numeric) {
                    const auto old = names->ids.find(*numeric);
                    if (old == names->ids.end() || (identifier(names->strings[old->second].source) == "mc2res") != (identifier(source) == "mc2res"))
                        names->ids[*numeric] = stringKey;
                }
            }
        }
    }
    return [names](std::int64_t number) -> std::optional<std::string> {
        const auto lookup = [&](const auto& map) -> std::optional<std::string> {
            const auto key = map.find(number);
            if (key == map.end()) return {};
            const auto text = names->strings.find(key->second);
            if (text == names->strings.end() || text->second.text.empty() || text->second.text.front() == '<') return {};
            return text->second.text;
        };
        if (auto alias = lookup(names->aliases)) return alias;
        return lookup(names->ids);
    };
}
} // namespace editor::document
