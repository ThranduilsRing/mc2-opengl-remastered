#include "FitReader.h"
#include "fit_number.h"
#include <algorithm>
#include <cctype>

namespace editor::document {
namespace {
std::string_view line(std::string_view bytes, std::size_t& at, std::size_t limit)
{
    const auto begin = at;
    while (at < bytes.size() && at - begin < limit && bytes[at] != '\r' && bytes[at] != '\n') ++at;
    const auto result = bytes.substr(begin, at - begin);
    ++at;
    if (at < bytes.size() && bytes[at] == '\n') ++at;
    return result;
}
bool declaration(std::string_view text, std::string_view search)
{
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    if (text.size() < search.size() || !std::equal(search.begin(), search.end(), text.begin(),
        [&](unsigned char a, unsigned char b) { return fold(a) == fold(b); })) return false;
    auto at = search.size();
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    return at < text.size() && text[at] == '=';
}
std::expected<std::string, LoadError> textValue(std::string_view bytes, std::string assembled, std::size_t at, std::size_t end)
{
    const auto separator = assembled.find('=');
    if (separator == assembled.npos) return std::unexpected(LoadError{"literal", "reader_string_buffer_limit", at});
    assembled.erase(0, separator + 1);
    const auto open = assembled.find('"');
    if (open == assembled.npos) return std::unexpected(LoadError{"literal", "reader_string_buffer_limit", at});
    while (assembled.find('"', open + 1) == assembled.npos && at < end) {
        const auto next = line(bytes, at, 2047);
        if (assembled.size() + next.size() + 1 >= 2048) break;
        assembled += next;
        assembled += '\n';
    }
    const auto close = assembled.find('"', open + 1);
    if (close == assembled.npos) return std::unexpected(LoadError{"literal", "reader_string_buffer_limit", at});
    return assembled.substr(open + 1, close - open - 1);
}
}

FitReader::FitReader(std::string_view bytes) : bytes_(bytes)
{
    std::size_t at{};
    line(bytes, at, 11);
    while (at < bytes.size()) {
        const auto text = line(bytes, at, 254);
        if (at >= bytes.size() || !text.starts_with('[')) continue;
        const auto close = text.find(']');
        if (close == text.npos || close >= 49) continue;
        if (!blocks_.empty()) blocks_.back().second.end = at;
        blocks_.push_back({std::string(text.substr(1, close - 1)), {at, bytes.size()}});
    }
}
std::optional<Span> FitReader::block(std::string_view name) const
{
    for (const auto& [id, span] : blocks_) if (id == name) return span;
    return {};
}
std::expected<Scalar, std::string_view> FitReader::read(std::string_view name, std::string_view prefix, std::string_view key) const
{
    const auto span = block(name);
    if (!span) return std::unexpected("absent");
    auto at = span->begin;
    const auto search = std::string(prefix) + " " + std::string(key);
    while (at < span->end) {
        const auto text = line(bytes_, at, prefix == "st" ? 2047 : 254);
        if (at >= span->end) break;
        if (!declaration(text, search)) continue;
        if (prefix == "st") {
            auto value = textValue(bytes_, std::string(text), at, span->end);
            if (!value || value->size() > objectiveTextLimit) return std::unexpected("reader_string_buffer_limit");
            return Scalar{std::move(*value)};
        }
        auto wire = text.substr(text.find('=') + 1);
        if (prefix == "b") {
            const auto first = wire.find_first_not_of(" \t\r\n\v\f");
            return Scalar{first == wire.npos || (wire[first] != '0' && wire[first] != 'f' && wire[first] != 'F')};
        }
        return readerNumber(prefix, std::string(wire));
    }
    return std::unexpected("absent");
}
std::expected<std::string, LoadError> readerText(std::string_view bytes, std::size_t equal)
{
    const auto start = bytes.rfind('\n', equal);
    auto at = start == bytes.npos ? 0 : start + 1;
    auto first = std::string(line(bytes, at, 2047));
    return textValue(bytes, std::move(first), at, bytes.size());
}
Scalar readerNumber(std::string_view prefix, std::string wire)
{
    if (prefix == "f") return static_cast<double>(mc2::fit::textToFloat(wire.data()));
    if (prefix == "l") return static_cast<std::int64_t>(mc2::fit::textToLong(wire.data()));
    return static_cast<std::int64_t>(static_cast<std::int32_t>(mc2::fit::textToULong(wire.data())));
}
std::string readerNumericWire(std::string_view bytes, std::size_t value)
{
    const auto lineStart = bytes.rfind('\n', value);
    const auto lineEnd = std::min(bytes.find_first_of("\r\n", value),
        (lineStart == bytes.npos ? 0 : lineStart + 1) + 254);
    return std::string(bytes.substr(value, lineEnd > value ? lineEnd - value : 0));
}

std::expected<void, LoadError> validateObjectiveBlock(std::string_view block)
{
    const auto bytes = "FITini\r\n" + std::string(block) + "\r\nFITend\r\n";
    const auto syntax = decodeFit(bytes);
    if (!syntax) return std::unexpected(syntax.error());
    for (const auto limit : {254U, 2047U}) {
        std::size_t at{};
        while (at < bytes.size()) {
            const auto begin = at;
            const auto text = line(bytes, at, limit);
            for (const auto& field : syntax->fields) {
                if (begin <= field.syntax.begin || begin >= field.syntax.end || bytes[field.syntax.begin] != '"') continue;
                bool collision = text.starts_with('[');
                for (const auto prefix : {"b ", "c ", "uc ", "s ", "us ", "l ", "ul ", "f ", "st "}) {
                    if (!identifier(text).starts_with(prefix)) continue;
                    const auto equal = text.find('=');
                    if (equal != text.npos && text.substr(std::string_view(prefix).size(), equal - std::string_view(prefix).size()).find_first_not_of(" \t") != text.npos) collision = true;
                }
                if (collision) return std::unexpected(LoadError{field.name, "reader_scanner_collision", begin});
            }
        }
    }
    return {};
}
}
