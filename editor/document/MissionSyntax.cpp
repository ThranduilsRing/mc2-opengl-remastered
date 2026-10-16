#include "MissionDocument.h"
#include "fit_number.h"

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

std::expected<std::string, LoadError> readerText(const std::string& bytes, std::size_t equal)
{
    const auto start = bytes.rfind('\n', equal);
    std::size_t at = start == bytes.npos ? 0 : start + 1;
    const auto line = [&]() {
        const auto begin = at;
        while (at < bytes.size() && at - begin < 2047 && bytes[at] != '\r' && bytes[at] != '\n') ++at;
        auto result = bytes.substr(begin, at - begin);
        if (at < bytes.size()) ++at;
        // RAM readLine skips the next LF even when the terminator was itself LF.
        if (at < bytes.size() && bytes[at] == '\n') ++at;
        return result;
    };
    auto assembled = line();
    const auto separator = assembled.find('=');
    if (separator == assembled.npos) return std::unexpected(LoadError{"literal", "reader_string_buffer_limit", equal});
    assembled.erase(0, separator + 1);
    const auto open = assembled.find('"');
    if (open == assembled.npos) return std::unexpected(LoadError{"literal", "reader_string_buffer_limit", equal});
    while (assembled.find('"', open + 1) == assembled.npos && at < bytes.size()) {
        auto next = line();
        if (assembled.size() + next.size() + 1 >= 2048)
            return std::unexpected(LoadError{"literal", "reader_string_buffer_limit", equal});
        assembled += next + '\n';
    }
    const auto close = assembled.find('"', open + 1);
    if (close == assembled.npos) return std::unexpected(LoadError{"literal", "reader_string_buffer_limit", equal});
    return assembled.substr(open + 1, close - open - 1);
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
                if (++p - begin > textByteLimit) return std::unexpected(LoadError{"literal", "text_limit", start});
            }
            if (p == bytes.size()) return std::unexpected(LoadError{"literal", "unterminated_literal", start});
            auto value = bytes.substr(begin, p - begin);
            std::erase(value, '\r');
            tokens.push_back({kind, {start, ++p}, std::move(value)});
            const auto lineStart = bytes.rfind('\n', start);
            const auto declaration = std::string_view(bytes).substr(lineStart == bytes.npos ? 0 : lineStart + 1,
                start - (lineStart == bytes.npos ? 0 : lineStart + 1));
            if (identifier(declaration.substr(0, 3)) == "st ")
                while (p < bytes.size() && bytes[p] != '\r' && bytes[p] != '\n') ++p;
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

std::expected<Scalar, LoadError> scalar(const Token& token, std::string_view prefix, bool objectiveInteger = false)
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
        // Decimal overflow remains preservable syntax; the objective decoder marks it read-only.
        if (objectiveInteger && !hex && ec == std::errc::result_out_of_range && end == text.data() + text.size())
            return Scalar{text.starts_with('-') ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max()};
        // Unsigned FIT values also use signed decimal spellings for stored bit patterns.
        const auto maximum = prefix == "uc" ? 255LL : prefix == "c" ? 127LL : prefix == "us" ? 65535LL : prefix == "s" ? 32767LL : prefix == "ul" ? 4294967295LL : 2147483647LL;
        const auto minimum = prefix == "c" ? -128LL : prefix == "uc" ? 0LL : prefix == "s" ? -32768LL : prefix == "us" ? 0LL : -2147483648LL;
        if (ec != std::errc{} || end != text.data() + text.size() ||
            (!objectiveInteger && (n < minimum || n > maximum))) return bad();
        return Scalar{n};
    }
    return Scalar{token.value};
}

SectionKind sectionKind(std::string_view name)
{
    if (name == "MissionSettings" || name == "Script" || name == "Music") return SectionKind::settings;
    if (name == "Terrain") return SectionKind::terrain;
    if (name.starts_with("Part")) return SectionKind::entity;
    if (identifier(name).find("objective") != std::string::npos) return SectionKind::objective;
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
                auto nameStart = peek().span.begin;
                if (peek().kind != TokenKind::word) return failure("invalid_block");
                std::string name = tokens_[cursor_++].value;
                while (peek().kind == TokenKind::word) { name += ' ' + peek().value; ++cursor_; }
                if (peek().kind != TokenKind::closeSquare) return failure("invalid_or_duplicate_block");
                auto nameEnd = tokens_[cursor_ - 1].span.end;
                if (sectionKind(name) == SectionKind::objective) {
                    nameStart = start + 1; nameEnd = peek().span.begin;
                    name = result_.originalBytes.substr(nameStart, nameEnd - nameStart);
                }
                if (!topNames_.insert(sectionKind(name) == SectionKind::objective ? name : identifier(name)).second) return failure("invalid_or_duplicate_block");
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
        const auto equal = peek().span.begin;
        ++cursor_;
        prefix = identifier(prefix);
        const bool teamFlags = identifier(result_.sections[section].name).ends_with("booleanflags");
        const bool opaque = prefix != "st" && prefix != "b" && prefix != "f" && prefix != "l" && prefix != "ul" && prefix != "c" && prefix != "uc" && prefix != "s" && prefix != "us";
        Field field{NodeId{result_.fields.size() + 1}, name, prefix, {peek().span.begin, peek().span.begin}, {}, {}, opaque};
        field.array = array;
        bool objectiveInteger{};
        if (result_.sections[section].kind == SectionKind::objective) {
            for (const auto& spec : objectiveSchema()) {
                if (identifier(spec.key) == identifier(name) && prefix != spec.prefix && spec.key != "DisplayMarker") field.opaque = true;
                if (identifier(spec.key) == identifier(name) && prefix == spec.prefix && spec.type == 0) objectiveInteger = true;
            }
            objectiveInteger |= (identifier(name) == "activateflagindex" || identifier(name) == "flagindex") && prefix == "ul";
            objectiveInteger |= identifier(name) == "num" && prefix == "ul";
            if ((identifier(name) == "previousobjectivemustbecomplete" && prefix != "b") ||
                (prefix != "ul" && (identifier(name) == "activateflagindex" || identifier(name) == "numactions" ||
                    identifier(name) == "numfailureconditions" || identifier(name) == "numfailureactions"))) field.opaque = true;
        }
        const auto path = result_.sections[section].name + "/" + name;
        if (!fieldNames_[section].insert(identifier(name)).second) return failure("duplicate_field");
        for (std::size_t i = 0; i < count; ++i) {
            if (i && peek().kind == TokenKind::newline) ++cursor_;
            if (peek().kind != TokenKind::word && peek().kind != TokenKind::text) return failure("missing_value");
            auto value = field.opaque ? std::expected<Scalar, LoadError>{Scalar{peek().value}} : scalar(peek(), prefix, objectiveInteger);
            if (!array && prefix == "st" && (result_.sections[section].kind == SectionKind::objective || teamFlags) && peek().kind == TokenKind::text) {
                auto text = readerText(result_.originalBytes, equal);
                if (text) value = Scalar{std::move(*text)};
                else { field.opaque = true; field.warning = "reader_string_buffer_limit"; }
            }
            if (!array && result_.sections[section].kind == SectionKind::objective &&
                (prefix == "ul" || prefix == "l" || prefix == "f")) {
                const auto lineStart = result_.originalBytes.rfind('\n', peek().span.begin);
                const auto lineEnd = std::min(result_.originalBytes.find_first_of("\r\n", peek().span.begin),
                    (lineStart == std::string::npos ? 0 : lineStart + 1) + 254);
                auto wire = result_.originalBytes.substr(peek().span.begin, lineEnd - peek().span.begin);
                if (value && prefix != "f" && wire.find("0x") == wire.npos && wire.find("0X") == wire.npos)
                    if (const auto* authored = std::get_if<std::int64_t>(&*value); authored &&
                        (*authored < -2147483648LL || *authored > 2147483647LL))
                        field.warning = "outside signed 32-bit range; the game reader overflows or wraps this value";
                if (prefix == "f") value = Scalar{static_cast<double>(mc2::fit::textToFloat(wire.data()))};
                else if (prefix == "l") value = Scalar{static_cast<std::int64_t>(mc2::fit::textToLong(wire.data()))};
                else value = Scalar{static_cast<std::int64_t>(static_cast<std::int32_t>(mc2::fit::textToULong(wire.data())))};
                if (prefix == "f" && !std::isfinite(std::get<double>(*value)))
                    return std::unexpected(LoadError{path, "malformed_value", peek().span.begin});
            }
            if (!value && result_.sections[section].kind == SectionKind::objective &&
                (identifier(name) == "positionx" || identifier(name) == "positiony" ||
                    identifier(name) == "moverwid" || identifier(name) == "buildingwid" ||
                    identifier(name) == "flagid" || identifier(name) == "flagindex" || identifier(name) == "value" ||
                    identifier(name).ends_with("speciesstring") ||
                    identifier(result_.sections[section].name).find("condition") != std::string::npos ||
                    identifier(result_.sections[section].name).find("action") != std::string::npos)) {
                // Unreadable reader inputs remain visible, byte-exact syntax.
                field.opaque = true;
                value = Scalar{peek().value};
            }
            if (!value && teamFlags) { field.opaque = true; value = Scalar{peek().value}; }
            if (!value) { auto err = value.error(); err.field = path; return std::unexpected(std::move(err)); }
            field.value.push_back(std::move(*value));
            field.syntax.end = peek().span.end;
            ++cursor_;
            if (array && peek().kind == TokenKind::comma) { field.syntax.end = peek().span.end; ++cursor_; }
            else if (i + 1 != count) return failure("array_length_mismatch");
        }
        if (peek().kind != TokenKind::newline && peek().kind != TokenKind::semi && peek().kind != TokenKind::closeBrace && peek().kind != TokenKind::end)
            return failure("trailing_value_garbage");
        if (!array && result_.sections[section].kind == SectionKind::objective && (prefix == "ul" || prefix == "l")) {
            const auto lineEnd = result_.originalBytes.find_first_of("\r\n", field.syntax.end);
            const auto tail = std::string_view(result_.originalBytes).substr(field.syntax.end, lineEnd - field.syntax.end);
            // A hex-bearing comment is numeric input to FIT; replacing the value must remove it too.
            if (tail.find("0x") != tail.npos) {
                field.syntax.end += tail.size();
                field.warning = "numeric comment consumed by game; rewriting value removes comment";
            }
        }
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
