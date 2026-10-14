#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include "inifile.h"
#include "objective_text.h"
#include <string_view>
#include <cctype>
#include "MissionTextReader.h"

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
struct ModelReadResult
{
    bool accepted = true;
    unsigned defaultedFields = 0, rejectedFields = 0;
};

// Missing model metadata uses the game's defaults, including on reused objects.
// Rejected text preserves its field; invalid numeric metadata rejects the objective.
inline ModelReadResult readObjectiveModel(MissionTextReader& file, ObjectiveModelDefinition& definition)
{
    auto parsed = definition;
    ModelReadResult result;
    const auto nameStatus = file.text("ObjectiveModel", parsed.name);
    const bool missingName = nameStatus == TextStatus::Absent;
    const bool missingType = !file.has("ModelType"), missingScale = !file.has("ModelScale");
    if (missingName) parsed.name.clear();
    if (missingType) parsed.type = 0;
    if (missingScale) parsed.scale = 1.0f;
    result.defaultedFields = (missingName ? 1u : 0u) | (missingType ? 2u : 0u) | (missingScale ? 4u : 0u);
    if (result.defaultedFields)
        fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s block=%s defaulted=1 ObjectiveModel=%d ModelType=%d ModelScale=%d\n",
            file.missionName().c_str(), file.blockName(), missingName, missingType, missingScale);
    if (nameStatus == TextStatus::Rejected) result.rejectedFields |= 1u;
    const auto rejectNumeric = [&](const char* key, unsigned mask) {
        fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s field=%s/%s reason=malformed_value rejected=1\n",
            file.missionName().c_str(), file.blockName(), key);
        result.accepted = false;
        result.rejectedFields |= mask;
    };
    if (!missingType && (!longValue(file, "ModelType", parsed.type) || parsed.type < 0 || parsed.type > 255))
    { rejectNumeric("ModelType", 2u); return result; }
    if (!missingScale && (!floatValue(file, "ModelScale", parsed.scale) || parsed.scale <= 0))
    { rejectNumeric("ModelScale", 4u); return result; }
    definition = std::move(parsed);
    return result;
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
