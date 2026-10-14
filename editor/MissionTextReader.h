#pragma once

#include "inifile.h"
#include "objective_text.h"
#include "mc2str.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace EditorFit
{
inline constexpr unsigned long kMaxMissionBytes = 16 * 1024 * 1024;
enum class TextStatus { Absent, Read, Rejected };
inline std::optional<std::string> readMissionSource(File& input, std::string_view mission)
{
    const auto length = input.getLength();
    if (length > kMaxMissionBytes)
    {
        fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%.*s length=%lu limit=%lu reason=document_too_large rejected=1\n",
            static_cast<int>(mission.size()), mission.data(), length, kMaxMissionBytes);
        return std::nullopt;
    }
    std::string source(length, '\0');
    const auto count = input.read(0, reinterpret_cast<MemoryPtr>(source.data()), static_cast<long>(source.size()));
    if (count != static_cast<long>(length))
    {
        fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%.*s expected=%lu actual=%ld reason=short_read rejected=1\n",
            static_cast<int>(mission.size()), mission.data(), length, count);
        return std::nullopt;
    }
    return source;
}
// Text belongs to the document, rather than the FIT reader's fixed line buffer.
// Required-field validation precedes any replacement of the live editor state.
class MissionTextReader : public FitIniFile
{
    struct Field
    {
        std::string block, key, literal;
        bool text = false;
        bool complete = true;
        bool logged = false;
    };
    std::vector<Field> fields;
    std::string mission;

    static std::string_view trim(std::string_view value)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == value.npos) return {};
        return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
    }
    Field* find(std::string_view block, std::string_view key)
    {
        for (auto& field : fields)
            if (field.block == block && mc2::str::iequals(field.key, key)) return &field;
        return nullptr;
    }
    bool require(std::string_view block, std::string_view key)
    {
        if (find(block, key)) return true;
        fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s field=%.*s/%.*s missing=1 rejected=1\n",
            mission.c_str(), static_cast<int>(block.size()), block.data(), static_cast<int>(key.size()), key.data());
        return false;
    }
    bool rejectCount(std::string_view block, std::string_view key)
    {
        fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s field=%.*s/%.*s reason=invalid_count rejected=1\n",
            mission.c_str(), static_cast<int>(block.size()), block.data(), static_cast<int>(key.size()), key.data());
        return false;
    }
    static bool declaration(std::string_view line)
    {
        const auto space = line.find_first_of(" \t");
        const auto equal = line.find('=');
        if (space == line.npos || equal == line.npos || space >= equal || trim(line.substr(space, equal - space)).empty()) return false;
        const auto type = line.substr(0, std::min(space, line.find('[')));
        for (const auto kind : {"st", "b", "f", "d", "l", "ul", "s", "us", "c", "uc"})
            if (mc2::str::iequals(type, kind)) return true;
        return false;
    }
    TextStatus reject(Field& field, const char* reason, size_t length, unsigned long limit)
    {
        if (!field.logged)
        {
            fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s field=%s/%s length=%zu limit=%lu reason=%s rejected=1\n",
                mission.c_str(), field.block.c_str(), field.key.c_str(), length, limit, reason);
            field.logged = true;
        }
        return TextStatus::Rejected;
    }
    TextStatus text(Field& field, std::string& value, unsigned long limit)
    {
        if (!field.text || !field.complete)
            return reject(field, "malformed_literal", field.literal.size(), limit);
        auto parsed = field.literal.substr(1, field.literal.size() - 2);
        std::erase(parsed, '\r');
        if (parsed.size() > limit) return reject(field, "too_long", parsed.size(), limit);
        value = objective_text::toMemoryBreaks(parsed);
        return TextStatus::Read;
    }
    std::optional<size_t> species(std::string_view block, const char* key,
        std::span<const char* const> names, const char* legacyKey)
    {
        Field* field = find(block, key);
        size_t index = names.size();
        if (field)
        {
            std::string value;
            if (text(*field, value, objective_text::kMaxTextLength) != TextStatus::Read) return std::nullopt;
            for (size_t i = 0; i < names.size(); ++i) if (value == names[i]) { index = i; break; }
        }
        else if (legacyKey && (field = find(block, legacyKey)))
        {
            auto literal = trim(field->literal);
            if (literal.starts_with('+')) literal.remove_prefix(1);
            const bool hexadecimal = literal.starts_with("0x") || literal.starts_with("0X");
            if (hexadecimal) literal.remove_prefix(2);
            const auto [end, error] = std::from_chars(literal.data(), literal.data() + literal.size(), index, hexadecimal ? 16 : 10);
            if (field->text || error != std::errc{} || end != literal.data() + literal.size()) index = names.size();
        }
        if (!field) return std::nullopt;
        if (index >= names.size() || std::string_view(names[index]).ends_with("EnemyUnitGroup"))
        {
            reject(*field, "invalid_species", field->literal.size(), static_cast<unsigned long>(names.size() - 1));
            return std::nullopt;
        }
        return index;
    }
    bool requireEntry(std::string_view block, const char* speciesKey, std::span<const char* const> names, const char* legacyKey)
    {
        if (!find(block, speciesKey) && (!legacyKey || !find(block, legacyKey))) return require(block, speciesKey);
        const auto index = species(block, speciesKey, names, legacyKey);
        if (!index) return true; // A rejected discriminator omits its entry, without inventing a species.
        const std::string_view name = names[*index];
        if (name == "PlayBIK") return require(block, "AVIFileName");
        if (name == "PlayWAV") return require(block, "WAVFileName");
        if (name == "DisplayTextMessage") return require(block, "Message");
        if (name == "DisplayResourceTextMessage") return require(block, "ResourceStringID");
        if (name == "MakeNewTechnologyAvailable") return require(block, "PurchaseFileName");
        if (name == "SetBooleanFlag" || name == "BooleanFlagIsSet")
            return find(block, "FlagIndex") || require(block, "FlagID");
        if (name == "ElapsedMissionTime") return require(block, "Time");
        if (name.find("NumberOf") != name.npos) return require(block, "Num");
        if (name.starts_with("Move"))
            return require(block, "TargetCenterX") && require(block, "TargetCenterY") && require(block, "TargetRadius");
        if (name.find("Specific") != name.npos)
            return require(block, "PositionX") && require(block, "PositionY");
        return true;
    }
public:
    // Candidate objectives retain authored coordinates until the new world's objects exist.
    bool deferObjectReferences = false;
    using FitIniFile::open;
    const std::string& missionName() const { return mission; }
    const char* blockName() const { return currentBlockId ? currentBlockId : ""; }
    bool has(const char* key) { return find(currentBlockId ? currentBlockId : "", key) != nullptr; }
    long open(const char* path, FileMode mode = READ, long children = 50, bool keepCase = false) override
    {
        fields.clear();
        mission = path;
        if (mode != READ) return FitIniFile::open(path, mode, children, keepCase);
        File input;
        auto result = input.open(path, mode, children, keepCase);
        if (result != NO_ERR) return result;
        auto source = readMissionSource(input, mission);
        if (!source) return READ_PAST_EOF_ERR;
        input.close();
        result = FitIniFile::open(path, mode, children, keepCase);
        if (result != NO_ERR) return result;
        const std::string_view text(*source);
        std::string block;
        for (size_t pos = 0; pos < text.size();)
        {
            const auto end = text.find('\n', pos);
            const auto next = end == text.npos ? text.size() : end + 1;
            auto line = trim(text.substr(pos, next - pos));
            if (line.starts_with('['))
            {
                const auto close = line.find(']');
                if (close != line.npos) block = line.substr(1, close - 1);
            }
            else if (declaration(line))
            {
                const auto space = line.find_first_of(" \t");
                const auto equal = line.find('=');
                if (space != line.npos && equal != line.npos && space < equal)
                {
                    Field field{block, std::string(trim(line.substr(space, equal - space))),
                        std::string(trim(line.substr(equal + 1))), mc2::str::iequals(line.substr(0, space), "st")};
                    if (field.text)
                    {
                        field.complete = false;
                        if (field.literal.starts_with('"'))
                        {
                            const auto literal = trim(line.substr(equal + 1));
                            const auto first = static_cast<size_t>(literal.data() - text.data());
                            auto current = first + 1;
                            auto lineEnd = next;
                            while (true)
                            {
                                const auto close = text.find('"', current);
                                if (close != text.npos && close < lineEnd)
                                {
                                    const auto suffix = trim(text.substr(close + 1, lineEnd - close - 1));
                                    field.literal = text.substr(first, close - first + 1);
                                    field.complete = (suffix.empty() || suffix.starts_with("//"))
                                        && text.substr(first, lineEnd - first).find('\0') == text.npos;
                                    pos = lineEnd;
                                    break;
                                }
                                current = lineEnd;
                                if (current == text.size()) { pos = current; break; }
                                const auto followingEnd = text.find('\n', current);
                                lineEnd = followingEnd == text.npos ? text.size() : followingEnd + 1;
                                const auto following = trim(text.substr(current, lineEnd - current));
                                if (following.starts_with('[') || declaration(following) || following == "FitEnd")
                                { pos = current; break; }
                            }
                            if (!field.complete) field.literal = text.substr(first, pos - first);
                            fields.push_back(std::move(field));
                            continue;
                        }
                    }
                    fields.push_back(std::move(field));
                }
            }
            pos = next;
        }
        return NO_ERR;
    }

    TextStatus text(const char* key, std::string& value, unsigned long limit = objective_text::kMaxTextLength)
    {
        auto* field = find(currentBlockId ? currentBlockId : "", key);
        return field ? text(*field, value, limit) : TextStatus::Absent;
    }
    std::optional<size_t> species(const char* key, std::span<const char* const> names, const char* legacyKey = nullptr)
    { return species(currentBlockId ? currentBlockId : "", key, names, legacyKey); }
    bool requiredFields(int teams, std::span<const char* const> conditions, std::span<const char* const> actions)
    {
        if (!require("MissionSettings", "scenarioTuneNum") || !require("Script", "ScenarioScript")
            || !require("MissionSettings", "Blurb2")) return false;
        bool resourceName = false;
        seekBlock("MissionSettings");
        readIdBoolean("MissionNameUseResourceString", resourceName);
        if (!require("MissionSettings", resourceName ? "MissionNameResourceStringID" : "MissionName")) return false;
        if (!require("Objectives Version", "Version")) return false;
        seekBlock("Objectives Version");
        unsigned long version = 0;
        readIdULong("Version", version);
        for (int team = 0; team < (version == 2 ? 1 : teams); ++team)
        {
            const auto block = version == 2 ? std::string("Objectives") : std::format("Team{}Objectives", team);
            if (!require(block, "NumObjectives")) return false;
            seekBlock(block.c_str());
            unsigned long count = 0;
            readIdULong("NumObjectives", count);
            if (count > fields.size()) return rejectCount(block, "NumObjectives");
            for (unsigned long i = 0; i < count; ++i)
            {
                const auto objective = version == 2 ? std::format("Objective{}", i) : std::format("Team{}Objective{}", team, i);
                if (!require(objective, "NumConditions")) return false;
                struct List { const char* count; const char* suffix; const char* species; };
                for (const auto& list : {List{"NumConditions", "Condition", "ConditionSpeciesString"},
                    List{"NumActions", "Action", "ActionSpeciesString"},
                    List{"NumFailureConditions", "FailureCondition", "FailureConditionSpeciesString"},
                    List{"NumFailureActions", "FailureAction", "FailureActionSpeciesString"}})
                {
                    seekBlock(objective.c_str());
                    unsigned long entries = 0;
                    readIdULong(list.count, entries);
                    if (entries > fields.size()) return rejectCount(objective, list.count);
                    for (unsigned long entry = 0; entry < entries; ++entry)
                        if (!requireEntry(std::format("{}{}{}", objective, list.suffix, entry), list.species,
                            std::string_view(list.suffix).ends_with("Condition") ? conditions : actions,
                            std::string_view(list.suffix) == "Condition" ? "ConditionSpecies" : nullptr)) return false;
                }
            }
        }
        return true;
    }
};

inline TextStatus readText(FitIniFile& file, const char* key, std::string& value,
    unsigned long limit = objective_text::kMaxTextLength)
{
    if (auto* reader = dynamic_cast<MissionTextReader*>(&file)) return reader->text(key, value, limit);
    return TextStatus::Absent;
}
inline std::optional<std::string> text(FitIniFile& file, const char* key)
{
    std::string value;
    if (readText(file, key, value) != TextStatus::Read) return std::nullopt;
    return value;
}
inline std::optional<std::string> flagID(FitIniFile& file, const char* key, const char* indexKey)
{
    std::string value;
    const auto status = readText(file, key, value);
    if (status == TextStatus::Read) return value;
    if (status == TextStatus::Rejected) return std::nullopt;
    unsigned long index = 0;
    if (file.readIdULong(indexKey, index) == NO_ERR) return std::to_string(static_cast<int>(index));
    return std::nullopt;
}
inline std::optional<size_t> species(FitIniFile& file, const char* key,
    std::span<const char* const> names, const char* legacyKey = nullptr)
{
    if (auto* reader = dynamic_cast<MissionTextReader*>(&file)) return reader->species(key, names, legacyKey);
    return std::nullopt;
}
}
