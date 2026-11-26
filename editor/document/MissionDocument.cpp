#include "MissionDocument.h"
#include "FitReader.h"
#include "mission_codec.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <format>
#include <limits>

namespace editor::document {
namespace {
constexpr std::int64_t teamCount = 8;
constexpr std::int64_t legacyFlagIndexMax = std::numeric_limits<std::int32_t>::max();
std::string blockKey(std::string_view name)
{
    auto folded = identifier(name);
    if (folded.find("objective") != folded.npos) return std::string(name);
    return folded;
}
std::string pathText(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}
std::string logText(std::string_view text)
{
    std::string result;
    for (const auto c : text) {
        if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\0') result += "\\0";
        else result += c;
    }
    return result;
}
void diagnostic(const Diagnostic& log, std::string_view mission, const LoadError& error)
{
    if (log) log(std::format("[EDITOR_MODEL v1] mission={} field={} offset={} reason={} rejected=1", logText(mission), logText(error.field), error.offset, error.reason));
}
std::filesystem::path streamPath(const std::filesystem::path& path)
{
#ifdef _WIN32
    const auto native = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
    if (native.size() >= 248 && !native.starts_with(L"\\\\?\\"))
        return native.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native;
#endif
    return path;
}
std::expected<std::vector<std::byte>, LoadError> readBytes(const std::filesystem::path& path, std::size_t limit, bool allowEmpty = false)
{
    std::ifstream input(streamPath(path), std::ios::binary | std::ios::ate);
    if (!input) return std::unexpected(LoadError{pathText(path.filename()), "open_failed", 0});
    const auto size = input.tellg();
    if (size < 0 || (!allowEmpty && size == 0) || static_cast<std::uint64_t>(size) > limit) return std::unexpected(LoadError{pathText(path.filename()), "file_size_limit", 0});
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.data()), size);
    if (!input || input.gcount() != size || input.peek() != std::char_traits<char>::eof())
        return std::unexpected(LoadError{pathText(path.filename()), "read_count_mismatch", 0});
    return result;
}
std::uint32_t u32(std::span<const std::byte> bytes, std::size_t p)
{
    std::uint32_t n{};
    for (unsigned i = 0; i < 4; ++i) n |= std::to_integer<std::uint32_t>(bytes[p + i]) << (i * 8);
    return n;
}
float f32(std::span<const std::byte> bytes, std::size_t p) { return std::bit_cast<float>(u32(bytes, p)); }
std::optional<double> number(const Field* field)
{
    if (!field || field->value.size() != 1) return {};
    if (const auto* value = std::get_if<double>(&field->value[0])) return *value;
    if (const auto* value = std::get_if<std::int64_t>(&field->value[0])) return static_cast<double>(*value);
    return {};
}
bool writeBytes(const std::filesystem::path& path, std::span<const std::byte> bytes, bool exclusive = false)
{
    std::ofstream output(streamPath(path), std::ios::binary | (exclusive ? std::ios::noreplace : std::ios::trunc));
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    const bool wrote = static_cast<bool>(output);
    output.close();
    return wrote && static_cast<bool>(output);
}
std::array<std::filesystem::path, 2> sidecarPaths(std::filesystem::path fit)
{
    fit.replace_extension();
    auto ai = fit, specials = fit;
    ai += "_ai.fit"; specials += "_specials.fit";
    return {ai, specials};
}
std::expected<void, LoadError> recoveryState(const std::filesystem::path& fit)
{
    std::error_code ec;
    const auto directory = fit.parent_path().empty() ? std::filesystem::path(".") : fit.parent_path();
    const auto prefix = pathText(fit.stem()) + ".transaction.";
    for (std::filesystem::directory_iterator entry(directory, ec), end; !ec && entry != end; entry.increment(ec)) {
        if (!pathText(entry->path().filename()).starts_with(prefix)) continue;
        std::ifstream journal(streamPath(entry->path()));
        std::string state;
        std::getline(journal, state);
        if (state != "committed" && state != "rolled_back") {
            return std::unexpected(LoadError{"save", "recovery_required", 0});
        }
    }
    if (ec) return std::unexpected(LoadError{"save", "directory_query_failed", 0});
    return {};
}
}

std::expected<MissionDocument, LoadError> MissionDocument::read(const std::filesystem::path& fit, const std::filesystem::path& pak,
    const Diagnostic& log, std::span<const std::uint32_t> markerObjects)
{
    const auto recovery = recoveryState(fit);
    if (!recovery) { diagnostic(log, pathText(fit), recovery.error()); return std::unexpected(recovery.error()); }
    auto source = readBytes(fit, fitByteLimit);
    if (!source) { diagnostic(log, pathText(fit), source.error()); return std::unexpected(source.error()); }
    auto packets = readBytes(pak, pakByteLimit);
    if (!packets) { diagnostic(log, pathText(fit), packets.error()); return std::unexpected(packets.error()); }
    auto document = decode(std::string(reinterpret_cast<const char*>(source->data()), source->size()), std::move(*packets), pathText(fit), log, markerObjects);
    if (!document) return document;
    const auto sidecars = sidecarPaths(fit);
    for (std::size_t i = 0; i < sidecars.size(); ++i) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(sidecars[i], ec);
        if (ec) return std::unexpected(LoadError{pathText(sidecars[i]), "sidecar_query_failed", 0});
        if (!exists) continue;
        auto bytes = readBytes(sidecars[i], fitByteLimit, true);
        if (!bytes) return std::unexpected(bytes.error());
        document->brainSidecars_[i] = std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    }
    document->savedBrainSidecars_ = document->brainSidecars_;
    return document;
}

std::expected<MissionDocument, LoadError> MissionDocument::decode(std::string fit, std::vector<std::byte> pak, std::string mission,
    const Diagnostic& log, std::span<const std::uint32_t> markerObjects)
{
    return decodeSource(std::move(fit), std::move(pak), std::move(mission), log, markerObjects, true);
}

std::expected<MissionDocument, LoadError> MissionDocument::decodeSource(std::string fit, std::vector<std::byte> pak,
    std::string mission, const Diagnostic& log, std::span<const std::uint32_t> markerObjects, bool resolve)
{
    auto syntax = decodeFit(std::move(fit));
    if (!syntax) { diagnostic(log, mission, syntax.error()); return std::unexpected(syntax.error()); }
    auto archive = decodePak(std::move(pak));
    if (!archive) { diagnostic(log, mission, archive.error()); return std::unexpected(archive.error()); }
    MissionDocument document;
    document.syntax_ = std::move(*syntax);
    document.pak_ = std::move(*archive);
    document.mission_ = std::move(mission);
    document.markerObjects_.assign(markerObjects.begin(), markerObjects.end());
    document.rebuildIndex();
    auto valid = document.validateSemantics();
    if (!valid) { diagnostic(log, document.mission_, valid.error()); return std::unexpected(valid.error()); }
    auto references = document.decodeReferences(resolve, log);
    if (!references) { diagnostic(log, document.mission_, references.error()); return std::unexpected(references.error()); }
    if (log) for (const auto& section : document.syntax_.sections) for (const auto id : section.fields) {
        const auto& field = *document.field(id);
        if (!field.warning.empty()) log(std::format("[EDITOR_OBJECTIVE_WARNING v1] field={}/{} warning={} preserved=1",
            logText(section.name), logText(field.name), field.warning));
    }
    document.savedFit_ = document.syntax_.originalBytes;
    document.savedPlacements_ = document.placements_ = document.pak_.packets[1].decoded;
    document.nextEntityId_ = document.entities_.size() + 1;
    return document;
}

const Field* MissionDocument::field(NodeId id) const
{
    const auto found = fieldIndex_.find(id.value);
    return found == fieldIndex_.end() ? nullptr : &syntax_.fields[found->second];
}
const Section* MissionDocument::section(NodeId id) const
{
    for (const auto& value : syntax_.sections) if (value.id == id) return &value;
    return nullptr;
}
const Section* MissionDocument::childSection(NodeId parent, std::string_view name) const
{
    for (const auto& value : syntax_.sections)
        if (value.parent == parent && identifier(value.name) == identifier(name)) return &value;
    return nullptr;
}
const Field* MissionDocument::find(NodeId block, std::string_view name) const
{
    if (const auto* s = section(block)) for (auto id : s->fields)
        if (const auto* value = field(id); value && identifier(value->name) == identifier(name)) return value;
    return nullptr;
}
const Entity* MissionDocument::entity(EntityId id) const
{
    for (const auto& value : entities_) if (value.id == id) return &value;
    return nullptr;
}
bool MissionDocument::hasMarkerReference(EntityId id) const
{
    const auto reference = markerReference(id);
    return !reference || reference->has_value();
}
std::optional<std::array<std::uint32_t, 2>> MissionDocument::parentCell(EntityId id) const
{
    const auto* selected = entity(id);
    if (!selected || !selected->packetRecord) return {};
    return mc2::mission::parentCell(mc2::mission::objectRecord(placements_, *selected->packetRecord).parentId);
}

bool MissionDocument::isMarkerPacket(const Entity& entity) const
{
    return entity.packetRecord && std::ranges::find(markerObjects_, entity.objectNumber) != markerObjects_.end();
}
std::expected<std::optional<NodeId>, LoadError> MissionDocument::markerReference(EntityId id) const
{
    const auto* selected = entity(id);
    if (!selected || !isMarkerPacket(*selected)) return std::optional<NodeId>{};
    std::optional<NodeId> result;
    for (const auto& section : syntax_.sections) {
        const auto name = identifier(section.name);
        const auto suffix = std::string_view(name).substr(std::min(name.size(), std::size_t{9}));
        if (section.parent || !name.starts_with("navmarker") || suffix.empty() ||
            !std::ranges::all_of(suffix, [](char c) { return c >= '0' && c <= '9'; })) continue;
        const auto x = number(find(section.id, "xPos")), y = number(find(section.id, "yPos"));
        if (!x || !y || std::abs(selected->position[0] - static_cast<float>(*x)) >= 1.0 ||
            std::abs(selected->position[1] - static_cast<float>(*y)) >= 1.0) continue;
        // FIT has no packet ID; ambiguous XY references cannot safely choose a radius owner.
        const auto matches = std::ranges::count_if(entities_, [&](const Entity& e) {
            return isMarkerPacket(e) && std::abs(e.position[0] - static_cast<float>(*x)) < 1.0 &&
                std::abs(e.position[1] - static_cast<float>(*y)) < 1.0;
        });
        if (result || matches != 1) return std::unexpected(LoadError{"NavMarkers", "ambiguous_marker_reference", 0});
        result = section.id;
    }
    return result;
}
void MissionDocument::rebuildIndex()
{
    fieldIndex_.clear();
    for (std::size_t i = 0; i < syntax_.fields.size(); ++i) fieldIndex_.emplace(syntax_.fields[i].id.value, i);
    index_.clear();
    for (const auto& section : syntax_.sections) {
        if (section.parent) continue;
        auto& fields = index_[blockKey(section.name)];
        for (auto id : section.fields) fields.emplace(identifier(field(id)->name), id);
    }
}
const Field* MissionDocument::find(std::string_view section, std::string_view name) const
{
    const auto block = index_.find(blockKey(section));
    if (block == index_.end()) return nullptr;
    const auto found = block->second.find(identifier(name));
    return found == block->second.end() ? nullptr : field(found->second);
}

std::expected<void, LoadError> MissionDocument::validateSemantics()
{
    const auto reject = [](std::string field, std::string reason) { return std::unexpected(LoadError{std::move(field), std::move(reason), 0}); };
    const auto typed = [&](std::string_view block, std::string_view key, std::size_t type) {
        const auto* value = find(block, key);
        return value && !value->opaque && value->value.size() == 1 && value->value[0].index() == type;
    };
    if (!typed("MissionSettings", "Blurb2", 3) || !typed("MissionSettings", "scenarioTuneNum", 0)) return reject("MissionSettings", "missing_or_malformed_required_metadata");
    const auto* resource = find("MissionSettings", "MissionNameUseResourceString");
    if (resource && !typed("MissionSettings", "MissionNameUseResourceString", 2)) return reject("MissionSettings/MissionNameUseResourceString", "malformed_value");
    const bool resourceName = resource && std::get_if<bool>(&resource->value[0]) && std::get<bool>(resource->value[0]);
    const auto nameKey = resourceName ? "MissionNameResourceStringID" : "MissionName";
    if (!typed("MissionSettings", nameKey, resourceName ? 0 : 3)) return reject("MissionSettings/" + std::string(nameKey), "missing_or_malformed_required");
    if (!typed("Script", "ScenarioScript", 3)) return reject("Script/ScenarioScript", "missing_or_malformed_required");
    const auto sectionId = [&](std::string_view block) {
        const auto key = blockKey(block);
        for (const auto& section : syntax_.sections) if (!section.parent && blockKey(section.name) == key) return section.id;
        return NodeId{};
    };
    for (const auto& section : syntax_.sections) {
        const auto name = identifier(section.name);
        if (!section.parent && name.size() == 7 && name.starts_with("player") && name.back() >= '0' && name.back() <= '7') {
            if (const auto* team = find(section.id, "DefaultTeam"); team &&
                (team->value.size() != 1 || !std::holds_alternative<std::int64_t>(team->value[0]) ||
                    std::get<std::int64_t>(team->value[0]) < 0 || std::get<std::int64_t>(team->value[0]) >= 8))
                return reject(section.name + "/DefaultTeam", "invalid_team");
        }
        if (section.parent || !name.starts_with("warrior") || name.size() <= 7 ||
            !std::ranges::all_of(std::string_view(name).substr(7), [](char c) { return c >= '0' && c <= '9'; })) continue;
        if (const auto* legacy = find(section.id, "Brain"); legacy &&
            (legacy->value.size() != 1 || !std::holds_alternative<std::string>(legacy->value[0]) || legacy->prefix != "st"))
            return reject(section.name + "/Brain", "invalid_module_reference");
        if (const auto* brain = childSection(section.id, "Brain")) {
            if (const auto* mode = find(brain->id, "compatibilityMode")) {
                if (mode->value.size() != 1 || !std::holds_alternative<std::string>(mode->value[0]) ||
                    syntax_.originalBytes[mode->syntax.begin] != '"')
                    return reject(section.name + "/Brain/compatibilityMode", "invalid_brain_mode");
                const auto value = identifier(std::get<std::string>(mode->value[0]));
                if (value != "legacy" && value != "hybrid" && value != "enhanced")
                    return reject(section.name + "/Brain/compatibilityMode", "invalid_brain_mode");
            }
        }
    }
    const FitReader reader(syntax_.originalBytes);
    const auto versionRead = reader.read("Objectives Version", "ul", "Version");
    const auto version = versionRead ? std::optional{std::get<std::int64_t>(*versionRead)} : std::nullopt;
    if (!version || (*version != 2 && *version != 3)) return reject("Objectives Version/Version", "unsupported_or_missing_version");
    const std::array<std::string_view, 24> conditions = {"DestroyAllEnemyUnits", "DestroyNumberOfEnemyUnits", "DestroyEnemyUnitGroup", "DestroySpecificEnemyUnit", "DestroySpecificStructure",
        "CaptureOrDestroyAllEnemyUnits", "CaptureOrDestroyNumberOfEnemyUnits", "CaptureOrDestroyEnemyUnitGroup", "CaptureOrDestroySpecificEnemyUnit", "CaptureOrDestroySpecificStructure",
        "DeadOrFledAllEnemyUnits", "DeadOrFledNumberOfEnemyUnits", "DeadOrFledEnemyUnitGroup", "DeadOrFledSpecificEnemyUnit", "CaptureSpecificUnit", "CaptureSpecificStructure",
        "GuardSpecificUnit", "GuardSpecificStructure", "MoveAnyUnitToArea", "MoveAllUnitsToArea", "MoveAllSurvivingUnitsToArea", "MoveAllSurvivingMechsToArea", "BooleanFlagIsSet", "ElapsedMissionTime"};
    const std::array<std::string_view, 6> actions = {"PlayBIK", "PlayWAV", "DisplayTextMessage", "DisplayResourceTextMessage", "SetBooleanFlag", "MakeNewTechnologyAvailable"};
    const std::array<std::string_view, 4> counts = {"NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions"};
    const std::array<std::string_view, 4> suffixes = {"Condition", "Action", "FailureCondition", "FailureAction"};
    for (std::uint32_t team = 0; team < (*version == 2 ? 1U : 8U); ++team) {
        const auto block = *version == 2 ? std::string("Objectives") : std::format("Team{}Objectives", team);
        const auto countRead = reader.read(block, "ul", "NumObjectives");
        const auto count = countRead ? std::optional{std::get<std::int64_t>(*countRead)} : std::nullopt;
        if (!count || *count < 0 || *count > nodeLimit || static_cast<std::uint64_t>(*count) > syntax_.sections.size()) return reject(block + "/NumObjectives", "invalid_or_missing_count");
        for (std::uint32_t i = 0; i < *count; ++i) {
            const auto name = *version == 2 ? std::format("Objective{}", i) : std::format("Team{}Objective{}", team, i);
            Objective objective{sectionId(name), team, i, {}};
            if (!objective.section.value || !reader.block(name)) return reject(name, "missing_objective_block");
            if (const auto* alias = find(name, "PreviousObjectiveMustBeComplete"); alias &&
                (alias->prefix != "b" || !typed(name, "PreviousObjectiveMustBeComplete", 2)))
                preserveObjectiveField(alias->id, "ignored by the game");
            if (const auto* alias = find(name, "ActivateFlagIndex")) {
                const auto wire = std::string_view(syntax_.originalBytes).substr(alias->syntax.begin, alias->syntax.end - alias->syntax.begin);
                const auto value = number(alias).value_or(0);
                const bool bits = (wire.starts_with("0x") && value >= 0 && value <= 4294967295.0) || wire.starts_with("0X");
                if (alias->prefix != "ul" || !typed(name, "ActivateFlagIndex", 0) || alias->warning.starts_with("outside signed") || (!bits && (value < -2147483648.0 || value > legacyFlagIndexMax)))
                    preserveObjectiveField(alias->id, alias->prefix != "ul" ? "ignored by the game" :
                        "outside signed 32-bit range; the game reader overflows or wraps this value");
            }
            if (auto scalars = decodeObjectiveScalars(objective); !scalars) return scalars;
            if (!reader.read(name, "ul", "NumConditions")) return reject(name + "/NumConditions", "missing_required");
            for (std::size_t list = 0; list < 4; ++list) {
                const auto* countField = find(name, counts[list]);
                const bool ignored = list != 0 && countField && countField->prefix != "ul";
                if (ignored) preserveObjectiveField(countField->id, "ignored by the game");
                const auto entriesRead = reader.read(name, "ul", counts[list]);
                const auto entries = entriesRead ? std::get<std::int64_t>(*entriesRead) : 0;
                if (entries < 0 || entries > nodeLimit || static_cast<std::uint64_t>(entries) > syntax_.sections.size()) return reject(name + "/" + std::string(counts[list]), "invalid_count");
                const bool condition = list % 2 == 0;
                const std::span<const std::string_view> names = condition ? std::span<const std::string_view>(conditions) : std::span<const std::string_view>(actions);
                for (std::uint32_t j = 0; j < entries; ++j) {
                    const auto entryBase = *version == 2 && list != 0 ? std::format("Team0Objective{}", i) : name;
                    const auto entry = std::format("{}{}{}", entryBase, suffixes[list], j);
                    const auto speciesKey = std::string(suffixes[list]) + "SpeciesString";
                    const auto* speciesField = find(entry, speciesKey);
                    if (!sectionId(entry).value) {
                        if (std::ranges::any_of(syntax_.sections, [&](const Section& section) {
                            const auto first = section.name.find_first_not_of(" \t"), last = section.name.find_last_not_of(" \t");
                            return !section.parent && first != section.name.npos &&
                                identifier(std::string_view(section.name).substr(first, last - first + 1)) == identifier(entry);
                        })) continue;
                        return reject(entry, "missing_objective_entry");
                    }
                    std::size_t species = names.size();
                    std::string_view policy;
                    std::optional<std::string> speciesText;
                    if (auto text = reader.read(entry, "st", speciesKey)) speciesText = std::get<std::string>(std::move(*text));
                    if (speciesText) {
                        // Species names match the game exactly; near matches remain unsupported data.
                        for (std::size_t k = 0; k < names.size(); ++k) if (*speciesText == names[k]) species = k;
                        if (species == names.size()) policy = condition ?
                            "unknown condition: game falls back to DestroyAllEnemyUnits; entry read-only" :
                            "unknown action: game falls back to PlayBIK; entry read-only";
                    } else if (list == 0) {
                        const auto key = std::string(suffixes[list]) + "Species";
                        const auto legacy = reader.read(entry, "ul", key);
                        if (legacy) {
                            const auto number = std::get<std::int64_t>(*legacy);
                            if (number >= 0 && number < static_cast<std::int64_t>(names.size())) species = static_cast<std::size_t>(number);
                        }
                        if (species == names.size()) policy = speciesField ?
                            "unreadable species string and no supported numeric fallback; game skips entry" :
                            "missing species string and no supported numeric fallback; game skips entry";
                    } else {
                        policy = speciesField ? "unreadable species string; game skips entry; numeric fallback unavailable" :
                            "missing species string; game skips entry; numeric fallback unavailable";
                    }
                    if (species == names.size() || names[species].ends_with("EnemyUnitGroup")) {
                        ObjectiveEntry preserved{sectionId(entry), std::monostate{}};
                        preserved.policy = policy.empty() ? "unimplemented group predicate; game skips entry" : policy;
                        objective.entries[list].push_back(std::move(preserved));
                        continue;
                    }
                    const auto kind = names[species];
                    std::vector<std::string_view> required;
                    if (kind == "PlayBIK") required = {"AVIFileName"};
                    else if (kind == "PlayWAV") required = {"WAVFileName"};
                    else if (kind == "DisplayTextMessage") required = {"Message"};
                    else if (kind == "DisplayResourceTextMessage") required = {"ResourceStringID"};
                    else if (kind == "MakeNewTechnologyAvailable") required = {"PurchaseFileName"};
                    else if (kind == "ElapsedMissionTime") required = {"Time"};
                    else if (kind.find("NumberOf") != std::string_view::npos) required = {"Num"};
                    else if (kind.starts_with("Move")) required = {"TargetCenterX", "TargetCenterY", "TargetRadius"};
                    else if (kind.find("Specific") != std::string_view::npos) {
                        if (!reader.read(entry, "l", kind.ends_with("Structure") ? "BuildingWID" : "MoverWID")) required = {"PositionX", "PositionY"};
                    }
                    bool ignoredPayload{};
                    for (const auto key : required) {
                        const auto type = key == "AVIFileName" || key == "WAVFileName" || key == "Message" || key == "PurchaseFileName" || key == "FlagID" ? 3U : key == "ResourceStringID" || key == "Num" ? 0U : 1U;
                        const auto* payload = find(entry, key);
                        const auto prefix = type == 0 ? "ul" : type == 1 ? "f" : "st";
                        if (!reader.read(entry, prefix, key)) {
                            if (payload) preserveObjectiveField(payload->id, "ignored by the game; required payload unreadable");
                            ignoredPayload = true;
                            continue;
                        }
                    }
                    ObjectiveEntry value{sectionId(entry), ConditionSpecies{}};
                    if (ignoredPayload) value.policy = "required payload unreadable; game skips entry; entry read-only";
                    if (condition) value.species = static_cast<ConditionSpecies>(species);
                    else value.species = static_cast<ActionSpecies>(species);
                    if (!ignoredPayload) for (const auto& field : objectivePayloadSchema(value)) {
                        auto payload = reader.read(entry, field.prefix, field.key);
                        if (!payload) return reject(entry + "/" + std::string(field.key), "required_payload_unreadable");
                        value.payload.push_back(std::move(*payload));
                    }
                    if (kind == "BooleanFlagIsSet" || kind == "SetBooleanFlag") {
                        value.flag.emplace();
                        auto& flag = *value.flag;
                        const auto* id = find(entry, "FlagID");
                        if (auto text = reader.read(entry, "st", "FlagID")) {
                            flag.id = std::get<std::string>(std::move(*text));
                            if (id) flag.idSource = id->id;
                        }
                        const auto* index = find(entry, "FlagIndex");
                        if (!flag.id) {
                            if (auto n = reader.read(entry, "ul", "FlagIndex")) {
                                flag.id = std::to_string(std::get<std::int64_t>(*n));
                                flag.indexFallback = true;
                                if (index) { flag.idSource = index->id; flag.warning = index->warning; }
                            }
                        }
                        if (!flag.id) {
                            flag.warning = "no readable FlagID or FlagIndex; game skips entry";
                            value.policy = "unreadable flag ID/index; game skips entry; entry read-only";
                        }
                        else if (id && flag.indexFallback) flag.warning += " FlagID unreadable or oversized; FlagIndex supplies ID";
                        else if (index && !flag.indexFallback) flag.warning += " FlagIndex ignored because readable FlagID wins (including empty ID)";
                        const auto* state = find(entry, "Value");
                        if (auto actual = reader.read(entry, "b", "Value")) {
                            flag.value = std::get<bool>(*actual);
                            if (state) {
                                flag.valueSource = state->id;
                                if (state->opaque) flag.warning += " noncanonical Value; reader conversion shown; bytes preserved";
                            }
                        } else flag.warning += " Value absent or unreadable; defaults true";
                    }
                    objective.entries[list].push_back(value);
                }
            }
            objectives_.push_back(std::move(objective));
        }
    }
    const auto& terrain = pak_.packets[0].decoded;
    if (terrain.empty() || terrain.size() % 32) return reject("Packet0", "invalid_terrain_length");
    const auto tiles = terrain.size() / 32;
    const auto side = static_cast<std::size_t>(std::sqrt(static_cast<double>(tiles)));
    if (side < 2 || side > 1024 || side * side != tiles) return reject("Packet0", "terrain_dimension_limit");
    terrainSide_ = side;
    terrain_.reserve(tiles);
    for (std::size_t p = 0; p < terrain.size(); p += 32) {
        TerrainTile tile;
        for (std::size_t i = 0; i < 3; ++i) tile.normal[i] = f32(terrain, p + i * 4);
        tile.elevation = f32(terrain, p + 12);
        if (!std::isfinite(tile.elevation) || !std::isfinite(tile.normal[0]) || !std::isfinite(tile.normal[1]) || !std::isfinite(tile.normal[2]))
            return reject("Packet0", "nonfinite_terrain");
        tile.texture = u32(terrain, p + 16); tile.light = u32(terrain, p + 20); tile.material = u32(terrain, p + 24);
        for (std::size_t i = 0; i < 4; ++i) tile.flags[i] = std::to_integer<std::uint8_t>(terrain[p + 28 + i]);
        terrain_.push_back(tile);
    }
    const auto& placements = pak_.packets[1].decoded;
    if (!placements.empty()) {
        if (placements.size() < 4) return reject("Packet1", "truncated_placements");
        const auto count = mc2::mission::objectCount(placements);
        if (!count || *count > nodeLimit) return reject("Packet1", "placement_count_mismatch");
        for (std::size_t i = 0; i < *count; ++i) {
            const auto record = mc2::mission::objectRecord(placements, i);
            Entity entity{EntityId{entities_.size() + 1}, {}, {}, i, static_cast<std::uint32_t>(record.type), {}, record.rotation, record.team};
            for (std::size_t j = 0; j < 3; ++j) entity.position[j] = record.position[j];
            if (!std::isfinite(entity.rotation) || !std::isfinite(entity.position[0]) || !std::isfinite(entity.position[1]) || !std::isfinite(entity.position[2])) return reject("Packet1", "nonfinite_placement");
            if (entity.team < -1 || entity.team >= teamCount) return reject("Packet1/TeamID", "out_of_range");
            entities_.push_back(entity);
        }
    }
    const auto markerCount = number(find("NavMarkers", "NumNavMarkers"));
    if (index_.contains("navmarkers") && (!typed("NavMarkers", "NumNavMarkers", 0) || !markerCount ||
        *markerCount < 0 || *markerCount > entities_.size())) return reject("NavMarkers", "invalid_marker_count");
    std::vector<EntityId> matchedMarkers;
    std::size_t markerSections{};
    for (const auto& section : syntax_.sections) {
        const auto name = identifier(section.name);
        const auto suffix = std::string_view(name).substr(std::min(name.size(), std::size_t{9}));
        if (!section.parent && name.starts_with("navmarker") && !suffix.empty() &&
            std::ranges::all_of(suffix, [](char c) { return c >= '0' && c <= '9'; })) ++markerSections;
    }
    const auto packetMarkers = std::ranges::count_if(entities_, [&](const Entity& e) { return isMarkerPacket(e); });
    if (markerSections != markerCount.value_or(0) || markerSections != packetMarkers)
        return reject("NavMarkers", "marker_count_mismatch");
    for (std::size_t i = 0; i < markerSections; ++i) {
        const auto block = "NavMarker" + std::to_string(i);
        const auto x = number(find(block, "xPos")), y = number(find(block, "yPos")), radius = number(find(block, "radius"));
        if (!typed(block, "xPos", 1) || !typed(block, "yPos", 1) || !typed(block, "radius", 1) ||
            !x || !y || !radius || *radius < 0 || *radius > std::numeric_limits<float>::max() ||
            std::abs(*x) > std::numeric_limits<float>::max() || std::abs(*y) > std::numeric_limits<float>::max())
            return reject(block, "invalid_marker_payload");
        const auto matches = [&](const Entity& e) {
            return isMarkerPacket(e) &&
                std::abs(e.position[0] - static_cast<float>(*x)) < 1.0 && std::abs(e.position[1] - static_cast<float>(*y)) < 1.0;
        };
        const auto match = std::ranges::find_if(entities_, matches);
        if (match == entities_.end()) return reject(block, "marker_packet_desync");
        if (std::ranges::count_if(entities_, matches) != 1 || std::ranges::find(matchedMarkers, match->id) != matchedMarkers.end())
            return reject(block, "ambiguous_marker_reference");
        matchedMarkers.push_back(match->id);
    }
    const auto partCount = number(find("Parts", "NumParts"));
    if (!typed("Parts", "NumParts", 0) || !partCount || *partCount < 0 || *partCount > 65535 || std::floor(*partCount) != *partCount) return reject("Parts/NumParts", "invalid_or_missing_count");
    for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(*partCount); ++i) {
        const auto section = "Part" + std::to_string(i);
        const auto x = number(find(section, "PositionX")), y = number(find(section, "PositionY"));
        const auto object = number(find(section, "ObjectNumber"));
        if (!typed(section, "PositionX", 1) || !typed(section, "PositionY", 1) || !typed(section, "ObjectNumber", 0) || !x || !y || !object || *object < 0 || *object > std::numeric_limits<std::uint32_t>::max()) return reject(section, "missing_required_part_payload");
        for (const auto key : {"PositionZ", "Rotation"})
            if (find(section, key) && !typed(section, key, 1)) return reject(section + "/" + key, "malformed_value");
        std::int64_t team{};
        if (const auto* value = find(section, "TeamID")) {
            if (!typed(section, "TeamID", 0)) return reject(section + "/TeamID", "malformed_value");
            team = std::get<std::int64_t>(value->value[0]);
            if (team < -1 || team >= teamCount) return reject(section + "/TeamID", "out_of_range");
        }
        const std::optional<NodeId> block = sectionId(section);
        Entity entity{EntityId{entities_.size() + 1}, i, block, {}, static_cast<std::uint32_t>(*object),
            {*x, *y, number(find(section, "PositionZ")).value_or(0)}, number(find(section, "Rotation")).value_or(0),
            team};
        for (const auto& [key, target] : {std::pair{"ControlType", &entity.controlType}, std::pair{"ControlDataType", &entity.controlDataType}}) {
            if (find(section, key) && !typed(section, key, 0)) return reject(section + "/" + key, "malformed_value");
            if (const auto value = number(find(section, key))) {
                if (!typed(section, key, 0) || *value < 0 || *value > std::numeric_limits<std::uint32_t>::max()) return reject(section + "/" + key, "malformed_value");
                *target = static_cast<std::uint32_t>(*value);
            }
        }
        for (const auto& [key, target] : {std::pair{"ObjectProfile", &entity.objectProfile}, std::pair{"CSVFile", &entity.csvFile}}) {
            if (const auto* value = find(section, key)) {
                if (!typed(section, key, 3)) return reject(section + "/" + key, "malformed_value");
                *target = std::get<std::string>(value->value[0]);
            }
        }
        if (const auto* pilot = find(section, "Pilot")) {
            const auto value = number(pilot);
            if (!typed(section, "Pilot", 0) || !value || *value < 0 || *value > 65535) return reject(section + "/Pilot", "invalid_reference");
            if (*value != 0) {
                const auto warrior = sectionId("Warrior" + std::to_string(static_cast<std::uint32_t>(*value)));
                if (!warrior.value) return reject(section + "/Pilot", "missing_warrior");
                entity.warrior = warrior;
                if (const auto* brain = childSection(warrior, "Brain")) entity.brain = brain->id;
            }
        }
        entities_.push_back(std::move(entity));
    }
    return {};
}

bool MissionDocument::referenceMatches(const Objective& objective, const ObjectiveEntry& entry, const Entity& entity) const
{
    if ((entry.reference.kind == ReferenceKind::unit) != entity.exportedPart.has_value()) return false;
    const auto species = std::get<ConditionSpecies>(entry.species);
    if ((species == ConditionSpecies::captureOrDestroyUnit || species == ConditionSpecies::captureUnit) &&
        entity.team == objective.team) return false;
    if (species == ConditionSpecies::guardUnit && entity.team != objective.team) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* field = find(entry.section, i == 0 ? "PositionX" : "PositionY");
        const auto a = static_cast<float>(entity.position[i]), b = static_cast<float>(std::get<double>(field->value[0]));
        const float difference = std::abs(a - b), bigger = std::max(std::abs(a), std::abs(b));
        const float proportion = bigger == 0 ? 0 : difference / bigger;
        if (!(proportion < std::pow(10.0, -std::numeric_limits<float>::digits10) * 10.0)) return false;
    }
    return true;
}

std::expected<void, LoadError> MissionDocument::decodeReferences(bool resolve, const Diagnostic& log)
{
    std::size_t storage{};
    for (auto& objective : objectives_) for (auto& list : objective.entries) for (auto& entry : list) {
        auto& reference = entry.reference;
        const auto* species = std::get_if<ConditionSpecies>(&entry.species);
        if (!species || !entry.policy.empty()) continue;
        switch (*species) {
        case ConditionSpecies::destroyUnit: case ConditionSpecies::deadOrFledUnit:
            reference.kind = ReferenceKind::unit; break;
        case ConditionSpecies::captureOrDestroyUnit: case ConditionSpecies::captureUnit:
            reference.kind = ReferenceKind::unit; break;
        case ConditionSpecies::guardUnit:
            reference.kind = ReferenceKind::unit; break;
        case ConditionSpecies::destroyStructure: case ConditionSpecies::captureOrDestroyStructure:
        case ConditionSpecies::captureStructure: case ConditionSpecies::guardStructure:
            reference.kind = ReferenceKind::building; break;
        default: continue;
        }
        for (const auto key : {"MoverWID", "BuildingWID", "PositionX", "PositionY", "Commander", "Group", "Mate", "CellX", "CellY"}) {
            if (const auto* source = find(entry.section, key)) reference.sources.push_back({source->id, key,
                syntax_.originalBytes.substr(source->syntax.begin, source->syntax.end - source->syntax.begin)});
        }
        storage += sizeof(ObjectiveReference) + referenceStorage(reference);
        if (storage > fitByteLimit) return std::unexpected(LoadError{"objectives", "reference_storage_limit", 0});
        const auto* wid = find(entry.section, reference.kind == ReferenceKind::unit ? "MoverWID" : "BuildingWID");
        if (wid && !wid->array && !wid->opaque && wid->prefix == "l" && wid->value.size() == 1 &&
            std::holds_alternative<std::int64_t>(wid->value[0])) {
            reference.status = ReferenceStatus::RuntimeOnlyWid;
            reference.reason = "runtime watch ID has no document identity";
            continue;
        }
        const auto coordinate = [&](std::string_view key) -> std::optional<float> {
            const auto* field = find(entry.section, key);
            if (!field || field->opaque || field->prefix != "f" || field->value.size() != 1) return {};
            const auto* value = std::get_if<double>(&field->value[0]);
            if (!value || !std::isfinite(*value) || std::abs(*value) > std::numeric_limits<float>::max()) return {};
            return static_cast<float>(*value);
        };
        const auto x = coordinate("PositionX"), y = coordinate("PositionY");
        if (!x || !y) {
            entry.policy = "required payload unreadable; game skips entry; entry read-only";
            reference = {};
            continue;
        }
        if (!resolve) continue;
        std::size_t matches{};
        for (const auto& entity : entities_) {
            if (!referenceMatches(objective, entry, entity)) continue;
            ++matches;
            reference.target = entity.id;
        }
        reference.status = matches == 1 ? ReferenceStatus::Resolved : matches ? ReferenceStatus::Ambiguous : ReferenceStatus::Missing;
        reference.reason = matches == 1 ? "unique eligible XY target" : matches ? "multiple eligible XY targets" : "no eligible XY target";
        if (matches != 1) reference.target.reset();
    }
    if (log) for (const auto& objective : objectives_) for (const auto& list : objective.entries) for (const auto& entry : list)
        if (entry.reference.status == ReferenceStatus::Missing || entry.reference.status == ReferenceStatus::Ambiguous)
            log(std::format("[EDITOR_OBJECTIVE_REFERENCE v1] entry={} reason={} preserved=1",
                logText(section(entry.section)->name), entry.reference.reason));
    return {};
}

std::string MissionDocument::fitBytes() const
{
    std::string result;
    std::size_t last{};
    for (const auto& field : syntax_.fields) {
        if (field.original == field.value) continue;
        result.append(syntax_.originalBytes, last, field.syntax.begin - last);
        result += formatValue(field, field.value);
        last = field.syntax.end;
    }
    result.append(syntax_.originalBytes, last, std::string::npos);
    return result;
}
bool MissionDocument::modified() const
{
    return placements_ != savedPlacements_ || fitBytes() != savedFit_ || brainSidecars_ != savedBrainSidecars_;
}

std::expected<void, LoadError> MissionDocument::assign(NodeId id, Value value)
{
    const auto* target = field(id);
    if (!target || target->opaque || value.size() != target->original.size()) return std::unexpected(LoadError{"command", "invalid_field_or_type", 0});
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i].index() != target->original[i].index()) return std::unexpected(LoadError{target->name, "invalid_value_type", 0});
        if (auto* s = std::get_if<std::string>(&value[i]); s && value != target->original && (s->size() > textByteLimit || s->find_first_of("\"\r\n") != std::string::npos || s->find('\0') != std::string::npos))
            return std::unexpected(LoadError{target->name, "invalid_text", 0});
        if (auto* n = std::get_if<double>(&value[i]); n && !std::isfinite(*n)) return std::unexpected(LoadError{target->name, "nonfinite_value", 0});
    }
    // Generic field assignment is limited to metadata; entity edits use validated structural commands.
    if (target->prefix != "st" || (target != find("MissionSettings", "MissionName") && target != find("MissionSettings", "Author")))
        return std::unexpected(LoadError{target->name, "unsupported_edit", 0});
    for (auto& field : syntax_.fields) if (field.id == id) { field.value = std::move(value); break; }
    ++revision_;
    return {};
}

std::expected<void, LoadError> MissionDocument::save(const std::filesystem::path& stem, const Diagnostic& log)
{
    const auto reject = [&](std::string reason) -> std::expected<void, LoadError> {
        LoadError error{"save", std::move(reason), 0}; diagnostic(log, pathText(stem), error); return std::unexpected(error);
    };
    auto fit = stem; fit.replace_extension(".fit");
    auto pak = stem; pak.replace_extension(".pak");
    const auto mission = pathText(fit);
    fit = streamPath(fit); pak = streamPath(pak);
    const auto transaction = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto fitStage = fit; fitStage += ".stage." + transaction;
    auto pakStage = pak; pakStage += ".stage." + transaction;
    auto fitBackup = fit; fitBackup += ".previous." + transaction;
    auto pakBackup = pak; pakBackup += ".previous." + transaction;
    auto journal = fit; journal.replace_extension(); journal += ".transaction.active";
    auto completedJournal = fit; completedJournal.replace_extension(); completedJournal += ".transaction." + transaction;
    const auto sidecars = sidecarPaths(fit);
    auto sidecarStages = sidecars, sidecarBackups = sidecars;
    std::array<bool, 2> hadSidecars{}, oldSidecars{}, newSidecars{};
    for (std::size_t i = 0; i < sidecars.size(); ++i) {
        sidecarStages[i] += ".stage." + transaction;
        sidecarBackups[i] += ".previous." + transaction;
    }
    std::error_code ec;
    if (std::filesystem::exists(fitStage, ec) || std::filesystem::exists(pakStage, ec) || std::filesystem::exists(fitBackup, ec) || std::filesystem::exists(pakBackup, ec) || std::filesystem::exists(completedJournal, ec)) return reject("recovery_files_present");
    if (ec) return reject("destination_query_failed");
    const bool hadFit = std::filesystem::exists(fit, ec);
    if (ec || (hadFit && !std::filesystem::is_regular_file(fit, ec))) return reject("destination_not_regular");
    const bool hadPak = std::filesystem::exists(pak, ec);
    if (ec || (hadPak && !std::filesystem::is_regular_file(pak, ec))) return reject("destination_not_regular");
    for (std::size_t i = 0; i < sidecars.size(); ++i) {
        hadSidecars[i] = std::filesystem::exists(sidecars[i], ec);
        if (ec || (hadSidecars[i] && !std::filesystem::is_regular_file(sidecars[i], ec))) return reject("destination_not_regular");
        if (std::filesystem::exists(sidecarStages[i], ec) || std::filesystem::exists(sidecarBackups[i], ec)) return reject("recovery_files_present");
        if (ec) return reject("destination_query_failed");
    }
    if (!fit.parent_path().empty()) std::filesystem::create_directories(fit.parent_path(), ec);
    if (ec) return reject("staging_directory_failed");
    const auto recovery = recoveryState(fit);
    if (!recovery) return reject(recovery.error().reason);
    std::string pending = "pending\nFIT=" + pathText(fit) + "\nPAK=" + pathText(pak) + "\nFIT_previous=" + pathText(fitBackup) + "\nPAK_previous=" + pathText(pakBackup) + "\n";
    for (std::size_t i = 0; i < sidecars.size(); ++i)
        pending += "sidecar=" + pathText(sidecars[i]) + "\nsidecar_previous=" + pathText(sidecarBackups[i]) +
            "\nsidecar_stage=" + pathText(sidecarStages[i]) + "\nsidecar_present=" + (brainSidecars_[i] ? "1\n" : "0\n");
    // A single exclusive journal reserves this destination against another publisher.
    if (!writeBytes(journal, std::as_bytes(std::span(pending)), true)) return reject("journal_write_failed");
    const auto output = fitBytes();
    if (!writeBytes(fitStage, std::as_bytes(std::span(output)), true) || !writeBytes(pakStage, pakBytes(), true)) return reject("staging_write_failed");
    auto verified = read(fitStage, pakStage, {}, markerObjects_);
    if (!verified || verified->fitBytes() != output || !std::ranges::equal(verified->pakBytes(), pakBytes())) return reject("staging_verification_failed");
    for (std::size_t i = 0; i < sidecars.size(); ++i) if (brainSidecars_[i]) {
        const auto& source = *brainSidecars_[i];
        if (!writeBytes(sidecarStages[i], std::as_bytes(std::span(source)), true)) return reject("staging_write_failed");
        const auto bytes = readBytes(sidecarStages[i], fitByteLimit, true);
        if (!bytes || bytes->size() != source.size() ||
            !std::ranges::equal(*bytes, std::as_bytes(std::span(source)))) return reject("staging_verification_failed");
    }
    bool oldFit{}, oldPak{}, newFit{}, newPak{};
    auto rollback = [&] {
        bool ok = true;
        const auto restore = [&](const auto& from, const auto& to) {
            std::error_code error; std::filesystem::rename(from, to, error); ok &= !error;
        };
        if (newFit) restore(fit, fitStage);
        if (newPak) restore(pak, pakStage);
        for (std::size_t i = 0; i < sidecars.size(); ++i) if (newSidecars[i]) restore(sidecars[i], sidecarStages[i]);
        if (oldFit) restore(fitBackup, fit);
        if (oldPak) restore(pakBackup, pak);
        for (std::size_t i = 0; i < sidecars.size(); ++i) if (oldSidecars[i]) restore(sidecarBackups[i], sidecars[i]);
        if (ok) {
            const std::string rolledBack = "rolled_back\n" + pending.substr(8);
            ok = writeBytes(journal, std::as_bytes(std::span(rolledBack)));
            if (ok) { std::error_code error; std::filesystem::rename(journal, completedJournal, error); ok = !error; }
        }
        return ok;
    };
    if (hadFit) { std::filesystem::rename(fit, fitBackup, ec); if (ec) return reject(rollback() ? "backup_failed" : "rollback_failed"); oldFit = true; }
    if (hadPak) { std::filesystem::rename(pak, pakBackup, ec); if (ec) return reject(rollback() ? "backup_failed" : "rollback_failed"); oldPak = true; }
    for (std::size_t i = 0; i < sidecars.size(); ++i) if (hadSidecars[i]) {
        std::filesystem::rename(sidecars[i], sidecarBackups[i], ec);
        if (ec) return reject(rollback() ? "backup_failed" : "rollback_failed");
        oldSidecars[i] = true;
    }
    std::filesystem::rename(fitStage, fit, ec);
    if (ec) return reject(rollback() ? "publication_failed" : "rollback_failed");
    newFit = true;
    std::filesystem::rename(pakStage, pak, ec);
    if (ec) return reject(rollback() ? "publication_failed" : "rollback_failed");
    newPak = true;
    for (std::size_t i = 0; i < sidecars.size(); ++i) if (brainSidecars_[i]) {
        std::filesystem::rename(sidecarStages[i], sidecars[i], ec);
        if (ec) return reject(rollback() ? "publication_failed" : "rollback_failed");
        newSidecars[i] = true;
    }
    const std::string committed = "committed\n" + pending.substr(8);
    if (!writeBytes(journal, std::as_bytes(std::span(committed)))) return reject(rollback() ? "journal_commit_failed" : "rollback_failed");
    std::filesystem::rename(journal, completedJournal, ec);
    if (ec) return reject(rollback() ? "journal_archive_failed" : "rollback_failed");
    savedFit_ = output;
    savedPlacements_ = placements_;
    savedBrainSidecars_ = brainSidecars_;
    mission_ = mission;
    return {};
}
}
