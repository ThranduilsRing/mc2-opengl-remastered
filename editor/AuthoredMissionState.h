#pragma once

#include <cmath>
#include <string>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include "inifile.h"
#include "objective_text.h"
#include <string_view>
#include <cctype>

// Catalog identity and authored mission values have different lifetimes. Saves
// consume these document values; only an authoring command consults the catalog.
struct PartDefinition
{
    unsigned long controlDataType = 0;
    std::string objectProfile;
    std::string csvFile;
    bool operator==(const PartDefinition&) const = default;
};

struct ObjectiveModelDefinition
{
    std::string name;
    long type = 0;
    float scale = 1.0f;
    bool operator==(const ObjectiveModelDefinition&) const = default;
};

// A resolved object can have a cell-rounded position different from its FIT
// reference. Preserve the authored reference until that object actually moves.
struct AuthoredTargetPosition
{
    float x = 0, y = 0, resolvedX = 0, resolvedY = 0;
    bool loaded = false;
    void read(float authoredX, float authoredY, float objectX, float objectY)
    {
        x = authoredX; y = authoredY;
        resolvedX = objectX; resolvedY = objectY;
        loaded = true;
    }
    float writeX(float currentX) const { return loaded && currentX == resolvedX ? x : currentX; }
    float writeY(float currentY) const { return loaded && currentY == resolvedY ? y : currentY; }
};

namespace EditorFit
{
// Validate the source before replacing the open document. The legacy FIT string
// reader has a fixed line buffer, so it cannot safely measure oversized values.
inline bool objectiveTextLengths(const char* fitPath, const char* mission)
{
    File file;
    if (file.open(fitPath) != NO_ERR) return true; // The document loader reports missing files.
    std::string source(file.getLength(), '\0');
    file.read(reinterpret_cast<MemoryPtr>(source.data()), static_cast<long>(source.size()));
    const std::string_view text(source);
    std::string section;
    for (size_t pos = 0; pos < text.size();)
    {
        const auto end = text.find('\n', pos);
        auto line = text.substr(pos, end == text.npos ? text.size() - pos : end - pos);
        const auto start = line.find_first_not_of(" \t\r");
        if (start != line.npos) line.remove_prefix(start);
        if (line.starts_with('['))
        {
            section = line.substr(0, line.find(']') + 1);
            for (auto& c : section) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (section.find("objective") != section.npos && line.size() > 3
            && (line[0] == 's' || line[0] == 'S') && (line[1] == 't' || line[1] == 'T')
            && (line[2] == ' ' || line[2] == '\t'))
        {
            const auto equal = line.find('=');
            const auto quote = equal == line.npos ? line.npos : line.find('"', equal);
            if (quote != line.npos)
            {
                const auto first = pos + (line.data() - (text.data() + pos)) + quote + 1;
                const auto last = text.find('"', first);
                if (last != text.npos)
                {
                    size_t length = last - first;
                    for (size_t i = first; i + 1 < last; ++i)
                        if (text[i] == '\r' && text[i + 1] == '\n') --length;
                    if (length > objective_text::kMaxTextLength)
                    {
                        auto field = line.substr(3, equal - 3);
                        field = field.substr(0, field.find_last_not_of(" \t") + 1);
                        fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s field=%s/%.*s length=%zu rejected=1\n",
                            mission, section.c_str(), static_cast<int>(field.size()), field.data(), length);
                        return false;
                    }
                    pos = last + 1;
                    continue;
                }
            }
        }
        pos = end == text.npos ? text.size() : end + 1;
    }
    return true;
}
inline bool reject(const char* field)
{
    fprintf(stderr, "[EDITOR_DOCUMENT v1] rejected malformed %s\n", field);
    return false;
}

// Legacy scalar readers accept trailing garbage. Document readers require the
// entire literal so malformed mod content cannot silently become authored zero.
inline std::optional<std::string> scalarLiteral(FitIniFile& file, long result)
{
    if (result != NO_ERR) return std::nullopt;
    const auto end = file.getLogicalPosition();
    const auto start = end > 4096 ? end - 4096 : 0;
    std::string tail(end - start, '\0');
    file.read(start, reinterpret_cast<MemoryPtr>(tail.data()), static_cast<long>(tail.size()));
    file.seek(end);
    const auto last = tail.find_last_not_of("\r\n");
    if (last == std::string::npos) return std::nullopt;
    const auto line = tail.find_last_of("\r\n", last);
    const auto equal = tail.find('=', line == std::string::npos ? 0 : line + 1);
    if (equal == std::string::npos || equal > last) return std::nullopt;
    return tail.substr(equal + 1, last - equal);
}
inline bool floatValue(FitIniFile& file, const char* key, float& value)
{
    auto literal = scalarLiteral(file, file.readIdFloat(key, value));
    if (!literal) return false;
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(literal->c_str(), &end);
    if (end == literal->c_str() || errno == ERANGE || !std::isfinite(parsed)) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return false;
    value = parsed;
    return true;
}
inline bool longValue(FitIniFile& file, const char* key, long& value, bool unsignedType = false)
{
    unsigned long ignored = 0;
    const long result = unsignedType ? file.readIdULong(key, ignored) : file.readIdLong(key, value);
    auto literal = scalarLiteral(file, result);
    if (!literal) return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(literal->c_str(), &end, 0);
    if (end == literal->c_str() || errno == ERANGE || (unsignedType && parsed < 0)) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return false;
    value = parsed;
    return true;
}
inline bool stringValue(FitIniFile& file, const char* key, std::string& value)
{
    char text[2048] = {};
    auto literal = scalarLiteral(file, file.readIdString(key, text, sizeof(text)));
    if (!literal) return false;
    const auto start = literal->find_first_not_of(" \t");
    const auto end = literal->find_last_not_of(" \t");
    if (start == std::string::npos || end <= start || (*literal)[start] != '"' || (*literal)[end] != '"') return false;
    value = text;
    return literal->substr(start + 1, end - start - 1) == value
        && value.find_first_of("\r\n\"") == std::string::npos;
}
}
