#include "MissionDocument.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_set>

namespace editor::document {
namespace {
enum class TokenKind { word, text, openSquare, closeSquare, openBrace, closeBrace, equal, comma, semi, newline, end };
struct Token { TokenKind kind; Span span; std::string value; };

bool declarationBoundary(std::string_view line)
{
    const auto space = line.find_first_of(" \t");
    const auto equal = line.find('=');
    if (space == std::string_view::npos || equal == std::string_view::npos || equal <= space) return false;
    const auto type = identifier(line.substr(0, std::min(space, line.find('['))));
    return type == "st" || type == "f" || type == "b" || type == "l" || type == "ul" || type == "c" || type == "uc" || type == "s" || type == "us";
}

std::expected<std::vector<Token>, LoadError> tokenize(const std::string& bytes)
{
    if (const auto nul = bytes.find('\0'); nul != std::string::npos) return std::unexpected(LoadError{"syntax", "embedded_nul", nul});
    std::vector<Token> tokens;
    for (std::size_t p = 0; p < bytes.size();) {
        if (tokens.size() >= nodeLimit * 12) return std::unexpected(LoadError{"syntax", "token_limit", p});
        const auto start = p;
        const char c = bytes[p++];
        if (c == '\0') return std::unexpected(LoadError{"syntax", "embedded_nul", start});
        if (c == ' ' || c == '\t' || c == '\r') continue;
        if (c == '/' && p < bytes.size() && bytes[p] == '/') {
            while (p < bytes.size() && bytes[p] != '\n') ++p;
            continue;
        }
        TokenKind kind = TokenKind::word;
        switch (c) {
        case '\n': kind = TokenKind::newline; break;
        case '[': kind = TokenKind::openSquare; break;
        case ']': kind = TokenKind::closeSquare; break;
        case '{': kind = TokenKind::openBrace; break;
        case '}': kind = TokenKind::closeBrace; break;
        case '=': kind = TokenKind::equal; break;
        case ',': kind = TokenKind::comma; break;
        case ';': kind = TokenKind::semi; break;
        case '"': {
            kind = TokenKind::text;
            const auto begin = p;
            while (p < bytes.size() && bytes[p] != '"') {
                if (bytes[p] == '\0') return std::unexpected(LoadError{"literal", "embedded_nul", p});
                if (bytes[p] == '\n') {
                    auto next = p + 1;
                    while (next < bytes.size() && (bytes[next] == ' ' || bytes[next] == '\t' || bytes[next] == '\r')) ++next;
                    const auto end = bytes.find('\n', next);
                    const auto line = std::string_view(bytes).substr(next, end == std::string::npos ? bytes.size() - next : end - next);
                    if (line.starts_with('[') || line.starts_with('}') || identifier(line).starts_with("fitend") || declarationBoundary(line))
                        return std::unexpected(LoadError{"literal", "unterminated_literal", start});
                }
                if (++p - begin > textByteLimit) return std::unexpected(LoadError{"literal", "text_limit", start});
            }
            if (p == bytes.size()) return std::unexpected(LoadError{"literal", "unterminated_literal", start});
            auto value = bytes.substr(begin, p - begin);
            std::erase(value, '\r');
            tokens.push_back({kind, {start, ++p}, std::move(value)});
            continue;
        }
        default:
            while (p < bytes.size() && std::string_view(" \t\r\n[]{}=,;\"").find(bytes[p]) == std::string_view::npos) {
                if (bytes[p] == '/' && p + 1 < bytes.size() && bytes[p + 1] == '/') break;
                if (bytes[p] == '\0') return std::unexpected(LoadError{"syntax", "embedded_nul", p});
                ++p;
            }
            break;
        }
        if (tokens.size() >= nodeLimit * 12) return std::unexpected(LoadError{"syntax", "token_limit", start});
        tokens.push_back({kind, {start, p}, bytes.substr(start, p - start)});
    }
    tokens.push_back({TokenKind::end, {bytes.size(), bytes.size()}, {}});
    return tokens;
}

std::expected<Scalar, LoadError> scalar(const Token& token, std::string_view prefix)
{
    const auto bad = [&] { return std::unexpected(LoadError{"value", "malformed_value", token.span.begin}); };
    if (prefix == "st") {
        if (token.kind != TokenKind::text) return bad();
        return Scalar{token.value};
    }
    if (prefix == "b") {
        if (identifier(token.value) == "true" || token.value == "1") return Scalar{true};
        if (identifier(token.value) == "false" || token.value == "0") return Scalar{false};
        return bad();
    }
    if (prefix == "f") {
        double n{};
        auto [end, ec] = std::from_chars(token.value.data(), token.value.data() + token.value.size(), n);
        if (ec != std::errc{} || end != token.value.data() + token.value.size() || !std::isfinite(n) || std::abs(n) > std::numeric_limits<float>::max()) return bad();
        return Scalar{n};
    }
    if (prefix == "l" || prefix == "ul" || prefix == "c" || prefix == "uc" || prefix == "s" || prefix == "us") {
        std::int64_t n{};
        auto text = std::string_view(token.value);
        bool hex = text.starts_with("0x") || text.starts_with("0X");
        if (hex) text.remove_prefix(2);
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n, hex ? 16 : 10);
        // Unsigned FIT values also use signed decimal spellings for stored bit patterns.
        const auto maximum = prefix == "uc" ? 255LL : prefix == "c" ? 127LL : prefix == "us" ? 65535LL : prefix == "s" ? 32767LL : prefix == "ul" ? 4294967295LL : 2147483647LL;
        const auto minimum = prefix == "c" ? -128LL : prefix == "uc" ? 0LL : prefix == "s" ? -32768LL : prefix == "us" ? 0LL : -2147483648LL;
        if (ec != std::errc{} || end != text.data() + text.size() || n < minimum || n > maximum) return bad();
        return Scalar{n};
    }
    return Scalar{token.value};
}

SectionKind sectionKind(std::string_view name)
{
    if (name == "MissionSettings" || name == "Script" || name == "Music") return SectionKind::settings;
    if (name == "Terrain") return SectionKind::terrain;
    if (name.starts_with("Part")) return SectionKind::entity;
    if (name.find("Objective") != std::string_view::npos) return SectionKind::objective;
    if (name.starts_with("Team")) return SectionKind::team;
    if (name.starts_with("Player")) return SectionKind::player;
    if (name.starts_with("Warrior") || name == "Brain") return SectionKind::brain;
    if (name == "Cameras") return SectionKind::camera;
    if (name == "Water" || name == "Weather" || name == "TheSky") return SectionKind::environment;
    return SectionKind::extension;
}

class Parser {
public:
    Parser(std::string bytes, std::vector<Token> tokens) : tokens_(std::move(tokens)) { result_.originalBytes = std::move(bytes); }
    std::expected<Syntax, LoadError> run() {
        while (peek().kind == TokenKind::newline) ++cursor_;
        if (identifier(peek().value) != "fitini") return failure("missing_FITini");
        ++cursor_;
        std::optional<std::size_t> section;
        while (peek().kind != TokenKind::end && identifier(peek().value) != "fitend") {
            if (separator()) continue;
            if (identifier(peek().value) == "fitini") { ++cursor_; continue; }
            if (peek().kind == TokenKind::openSquare) {
                const auto start = peek().span.begin;
                if (section) result_.sections[*section].syntax.end = start;
                ++cursor_;
                const auto nameStart = peek().span.begin;
                if (peek().kind != TokenKind::word) return failure("invalid_block");
                std::string name = tokens_[cursor_++].value;
                while (peek().kind == TokenKind::word) { name += ' ' + peek().value; ++cursor_; }
                if (peek().kind != TokenKind::closeSquare || !topNames_.insert(identifier(name)).second) return failure("invalid_or_duplicate_block");
                const auto nameEnd = tokens_[cursor_ - 1].span.end;
                ++cursor_;
                section = addSection(name, std::nullopt);
                result_.sections[*section].syntax.begin = start;
                result_.sections[*section].nameSyntax = {nameStart, nameEnd};
            } else {
                if (!section) return failure("field_without_block");
                auto read = declaration(*section, 0);
                if (!read) return std::unexpected(read.error());
            }
        }
        if (identifier(peek().value) != "fitend") return failure("missing_FITend");
        result_.endMarker = peek().span.begin;
        if (section) result_.sections[*section].syntax.end = result_.endMarker;
        ++cursor_;
        while (separator()) {}
        if (peek().kind != TokenKind::end) return failure("trailing_document_garbage");
        return std::move(result_);
    }
private:
    const Token& peek() const { return tokens_[cursor_]; }
    bool separator() {
        if (peek().kind != TokenKind::newline && peek().kind != TokenKind::semi) return false;
        ++cursor_; return true;
    }
    std::unexpected<LoadError> failure(std::string reason) const { return std::unexpected(LoadError{"syntax", std::move(reason), peek().span.begin}); }
    std::size_t addSection(std::string name, std::optional<NodeId> parent) {
        const auto index = result_.sections.size();
        result_.sections.push_back({NodeId{nodeLimit + index + 1}, parent, name, sectionKind(name), {}});
        fieldNames_.emplace_back();
        return index;
    }
    std::expected<void, LoadError> declaration(std::size_t section, std::size_t depth) {
        if (depth > nestingLimit || result_.fields.size() + result_.sections.size() >= nodeLimit) return failure("node_or_depth_limit");
        if (peek().kind != TokenKind::word) return failure("expected_declaration");
        const auto start = peek().span.begin;
        std::string prefix, name = tokens_[cursor_++].value;
        std::size_t count = 1;
        bool array = false;
        if (peek().kind == TokenKind::openSquare) {
            array = true; prefix = name; ++cursor_;
            const auto& length = peek().value;
            const auto [end, ec] = std::from_chars(length.data(), length.data() + length.size(), count);
            if (ec != std::errc{} || end != length.data() + length.size() || count > nodeLimit) return failure("array_count_limit");
            ++cursor_;
            if (peek().kind != TokenKind::closeSquare) return failure("invalid_array_count");
            ++cursor_;
            if (peek().kind != TokenKind::word) return failure("missing_field_name");
            name = tokens_[cursor_++].value;
        } else if (peek().kind == TokenKind::word) {
            prefix = name; name = tokens_[cursor_++].value;
            while (peek().kind == TokenKind::word) { name += ' ' + peek().value; ++cursor_; }
        }
        if (peek().kind == TokenKind::openBrace) {
            const auto parent = result_.sections[section].id;
            if (identifier(name) == "brain" && !result_.sections[section].parent &&
                identifier(result_.sections[section].name).starts_with("warrior")) {
                for (const auto& sibling : result_.sections)
                    if (sibling.parent == parent && identifier(sibling.name) == "brain") return failure("duplicate_nested_block");
            }
            auto child = addSection(name, parent);
            result_.sections[child].syntax.begin = start;
            ++cursor_;
            while (peek().kind != TokenKind::closeBrace) {
                if (peek().kind == TokenKind::end || peek().value == "FITend" || peek().kind == TokenKind::openSquare) return failure("unterminated_block");
                if (separator()) continue;
                auto read = declaration(child, depth + 1);
                if (!read) return read;
            }
            result_.sections[child].syntax.end = peek().span.end;
            ++cursor_; return {};
        }
        if (peek().kind != TokenKind::equal) return failure("missing_equals");
        ++cursor_;
        prefix = identifier(prefix);
        const bool opaque = prefix != "st" && prefix != "b" && prefix != "f" && prefix != "l" && prefix != "ul" && prefix != "c" && prefix != "uc" && prefix != "s" && prefix != "us";
        Field field{NodeId{result_.fields.size() + 1}, name, prefix, {peek().span.begin, peek().span.begin}, {}, {}, opaque};
        const auto path = result_.sections[section].name + "/" + name;
        if (!fieldNames_[section].insert(identifier(name)).second) return failure("duplicate_field");
        for (std::size_t i = 0; i < count; ++i) {
            if (i && peek().kind == TokenKind::newline) ++cursor_;
            if (peek().kind != TokenKind::word && peek().kind != TokenKind::text) return failure("missing_value");
            auto value = scalar(peek(), prefix);
            if (!value) { auto err = value.error(); err.field = path; return std::unexpected(std::move(err)); }
            field.value.push_back(std::move(*value));
            field.syntax.end = peek().span.end;
            ++cursor_;
            if (array && peek().kind == TokenKind::comma) { field.syntax.end = peek().span.end; ++cursor_; }
            else if (i + 1 != count) return failure("array_length_mismatch");
        }
        if (peek().kind != TokenKind::newline && peek().kind != TokenKind::semi && peek().kind != TokenKind::closeBrace && peek().kind != TokenKind::end)
            return failure("trailing_value_garbage");
        field.original = field.value;
        result_.sections[section].fields.push_back(field.id);
        result_.fields.push_back(std::move(field));
        return {};
    }
    Syntax result_;
    std::vector<Token> tokens_;
    std::unordered_set<std::string> topNames_;
    std::vector<std::unordered_set<std::string>> fieldNames_;
    std::size_t cursor_{};
};
}

std::expected<Syntax, LoadError> decodeFit(std::string bytes)
{
    if (bytes.empty() || bytes.size() > fitByteLimit) return std::unexpected(LoadError{"FIT", "file_size_limit", 0});
    auto tokens = tokenize(bytes);
    if (!tokens) return std::unexpected(tokens.error());
    return Parser(std::move(bytes), std::move(*tokens)).run();
}

std::string formatValue(const Field& field, const Value& value)
{
    std::string result;
    for (const auto& item : value) {
        if (!result.empty()) result += ',';
        result += std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) return '"' + v + '"';
            else if constexpr (std::is_same_v<T, bool>) return v ? "TRUE" : "FALSE";
            else if constexpr (std::is_same_v<T, double>) return std::format("{:.6f}", v);
            else return std::to_string(v);
        }, item);
    }
    if (field.original.size() > 1) result += ',';
    return result;
}
}
