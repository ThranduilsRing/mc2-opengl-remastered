#include "MissionDocument.h"

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
void diagnostic(const Diagnostic& log, std::string_view mission, const LoadError& error)
{
    if (log) log(std::format("[EDITOR_MODEL v1] mission={} field={} offset={} reason={} rejected=1", mission, error.field, error.offset, error.reason));
}
std::expected<std::vector<std::byte>, LoadError> readBytes(const std::filesystem::path& path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::unexpected(LoadError{path.filename().string(), "open_failed", 0});
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > limit) return std::unexpected(LoadError{path.filename().string(), "file_size_limit", 0});
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.data()), size);
    if (!input || input.gcount() != size || input.peek() != std::char_traits<char>::eof())
        return std::unexpected(LoadError{path.filename().string(), "read_count_mismatch", 0});
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
bool writeBytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    const bool wrote = static_cast<bool>(output);
    output.close();
    return wrote && static_cast<bool>(output);
}
}

std::expected<MissionDocument, LoadError> MissionDocument::read(const std::filesystem::path& fit, const std::filesystem::path& pak, const Diagnostic& log)
{
    std::error_code ec;
    const auto directory = fit.parent_path().empty() ? std::filesystem::path(".") : fit.parent_path();
    const auto prefix = fit.stem().string() + ".transaction.";
    for (std::filesystem::directory_iterator entry(directory, ec), end; !ec && entry != end; entry.increment(ec)) {
        if (!entry->path().filename().string().starts_with(prefix)) continue;
        std::ifstream journal(entry->path());
        std::string state;
        std::getline(journal, state);
        if (state != "committed" && state != "rolled_back") {
            LoadError error{"save", "recovery_required", 0};
            diagnostic(log, fit.string(), error);
            return std::unexpected(error);
        }
    }
    if (ec) { LoadError error{"save", "directory_query_failed", 0}; diagnostic(log, fit.string(), error); return std::unexpected(error); }
    auto source = readBytes(fit, fitByteLimit);
    if (!source) { diagnostic(log, fit.string(), source.error()); return std::unexpected(source.error()); }
    auto packets = readBytes(pak, pakByteLimit);
    if (!packets) { diagnostic(log, fit.string(), packets.error()); return std::unexpected(packets.error()); }
    return decode(std::string(reinterpret_cast<const char*>(source->data()), source->size()), std::move(*packets), fit.string(), log);
}

std::expected<MissionDocument, LoadError> MissionDocument::decode(std::string fit, std::vector<std::byte> pak, std::string mission, const Diagnostic& log)
{
    auto syntax = decodeFit(std::move(fit));
    if (!syntax) { diagnostic(log, mission, syntax.error()); return std::unexpected(syntax.error()); }
    auto archive = decodePak(std::move(pak));
    if (!archive) { diagnostic(log, mission, archive.error()); return std::unexpected(archive.error()); }
    MissionDocument document;
    document.syntax_ = std::move(*syntax);
    document.pak_ = std::move(*archive);
    document.mission_ = std::move(mission);
    for (const auto& section : document.syntax_.sections) {
        if (section.parent) continue;
        auto& fields = document.index_[identifier(section.name)];
        for (auto id : section.fields) fields.emplace(identifier(document.field(id)->name), id);
    }
    auto valid = document.validateSemantics();
    if (!valid) { diagnostic(log, document.mission_, valid.error()); return std::unexpected(valid.error()); }
    for (const auto& field : document.syntax_.fields) document.savepoint_.push_back(field.value);
    return document;
}

const Field* MissionDocument::field(NodeId id) const
{
    if (id.value == 0 || id.value > syntax_.fields.size()) return nullptr;
    return &syntax_.fields[static_cast<std::size_t>(id.value - 1)];
}
const Field* MissionDocument::find(std::string_view section, std::string_view name) const
{
    const auto block = index_.find(identifier(section));
    if (block == index_.end()) return nullptr;
    const auto found = block->second.find(identifier(name));
    return found == block->second.end() ? nullptr : field(found->second);
}

std::expected<void, LoadError> MissionDocument::validateSemantics()
{
    const auto reject = [](std::string field, std::string reason) { return std::unexpected(LoadError{std::move(field), std::move(reason), 0}); };
    const auto typed = [&](std::string_view block, std::string_view key, std::size_t type) {
        const auto* value = find(block, key);
        return value && value->value.size() == 1 && value->value[0].index() == type;
    };
    if (!typed("MissionSettings", "Blurb2", 3) || !typed("MissionSettings", "scenarioTuneNum", 0)) return reject("MissionSettings", "missing_or_malformed_required_metadata");
    const auto* resource = find("MissionSettings", "MissionNameUseResourceString");
    if (resource && !typed("MissionSettings", "MissionNameUseResourceString", 2)) return reject("MissionSettings/MissionNameUseResourceString", "malformed_value");
    const bool resourceName = resource && std::get_if<bool>(&resource->value[0]) && std::get<bool>(resource->value[0]);
    const auto nameKey = resourceName ? "MissionNameResourceStringID" : "MissionName";
    if (!typed("MissionSettings", nameKey, resourceName ? 0 : 3)) return reject("MissionSettings/" + std::string(nameKey), "missing_or_malformed_required");
    if (!typed("Script", "ScenarioScript", 3)) return reject("Script/ScenarioScript", "missing_or_malformed_required");
    const auto sectionId = [&](std::string_view block) {
        for (const auto& section : syntax_.sections) if (!section.parent && identifier(section.name) == identifier(block)) return section.id;
        return NodeId{};
    };
    const auto version = number(find("Objectives Version", "Version"));
    if (!typed("Objectives Version", "Version", 0) || !version || (*version != 2 && *version != 3)) return reject("Objectives Version/Version", "unsupported_or_missing_version");
    const std::array<std::string_view, 24> conditions = {"DestroyAllEnemyUnits", "DestroyNumberOfEnemyUnits", "DestroyEnemyUnitGroup", "DestroySpecificEnemyUnit", "DestroySpecificStructure",
        "CaptureOrDestroyAllEnemyUnits", "CaptureOrDestroyNumberOfEnemyUnits", "CaptureOrDestroyEnemyUnitGroup", "CaptureOrDestroySpecificEnemyUnit", "CaptureOrDestroySpecificStructure",
        "DeadOrFledAllEnemyUnits", "DeadOrFledNumberOfEnemyUnits", "DeadOrFledEnemyUnitGroup", "DeadOrFledSpecificEnemyUnit", "CaptureSpecificUnit", "CaptureSpecificStructure",
        "GuardSpecificUnit", "GuardSpecificStructure", "MoveAnyUnitToArea", "MoveAllUnitsToArea", "MoveAllSurvivingUnitsToArea", "MoveAllSurvivingMechsToArea", "BooleanFlagIsSet", "ElapsedMissionTime"};
    const std::array<std::string_view, 6> actions = {"PlayBIK", "PlayWAV", "DisplayTextMessage", "DisplayResourceTextMessage", "SetBooleanFlag", "MakeNewTechnologyAvailable"};
    const std::array<std::string_view, 4> counts = {"NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions"};
    const std::array<std::string_view, 4> suffixes = {"Condition", "Action", "FailureCondition", "FailureAction"};
    for (std::uint32_t team = 0; team < (*version == 2 ? 1U : 8U); ++team) {
        const auto block = *version == 2 ? std::string("Objectives") : std::format("Team{}Objectives", team);
        const auto count = number(find(block, "NumObjectives"));
        if (!typed(block, "NumObjectives", 0) || !count || *count < 0 || *count > nodeLimit || *count > syntax_.sections.size()) return reject(block + "/NumObjectives", "invalid_or_missing_count");
        for (std::uint32_t i = 0; i < *count; ++i) {
            const auto name = *version == 2 ? std::format("Objective{}", i) : std::format("Team{}Objective{}", team, i);
            Objective objective{sectionId(name), team, i, {}};
            if (!typed(name, "NumConditions", 0)) return reject(name + "/NumConditions", "missing_required");
            if (find(name, "ObjectiveModel") && !typed(name, "ObjectiveModel", 3)) return reject(name + "/ObjectiveModel", "malformed_value");
            if (const auto* modelType = find(name, "ModelType"); modelType && (!typed(name, "ModelType", 0) || number(modelType).value_or(-1) < 0 || number(modelType).value_or(256) > 255)) return reject(name + "/ModelType", "malformed_value");
            if (const auto* scale = find(name, "ModelScale"); scale && (!typed(name, "ModelScale", 1) || number(scale).value_or(0) <= 0)) return reject(name + "/ModelScale", "malformed_value");
            for (std::size_t list = 0; list < 4; ++list) {
                const auto entries = number(find(name, counts[list])).value_or(0);
                if (find(name, counts[list]) && !typed(name, counts[list], 0)) return reject(name + "/" + std::string(counts[list]), "malformed_count");
                if (entries < 0 || entries > nodeLimit || entries > syntax_.sections.size() || std::floor(entries) != entries) return reject(name + "/" + std::string(counts[list]), "invalid_count");
                const bool condition = list % 2 == 0;
                const std::span<const std::string_view> names = condition ? std::span<const std::string_view>(conditions) : std::span<const std::string_view>(actions);
                for (std::uint32_t j = 0; j < entries; ++j) {
                    const auto entry = std::format("{}{}{}", name, suffixes[list], j);
                    const auto speciesKey = std::string(suffixes[list]) + "SpeciesString";
                    const auto* speciesField = find(entry, speciesKey);
                    std::size_t species = names.size();
                    if (speciesField && typed(entry, speciesKey, 3)) {
                        const auto& text = std::get<std::string>(speciesField->value[0]);
                        for (std::size_t k = 0; k < names.size(); ++k) if (identifier(text) == identifier(names[k])) species = k;
                    } else if (!speciesField) {
                        const auto key = std::string(suffixes[list]) + "Species";
                        const auto legacy = number(find(entry, key));
                        if (typed(entry, key, 0) && legacy && *legacy >= 0 && *legacy < names.size() && std::floor(*legacy) == *legacy) species = static_cast<std::size_t>(*legacy);
                    }
                    if (species == names.size() || names[species].ends_with("EnemyUnitGroup")) return reject(entry + "/" + speciesKey, "invalid_or_missing_species");
                    const auto kind = names[species];
                    std::vector<std::string_view> required;
                    if (kind == "PlayBIK") required = {"AVIFileName"};
                    else if (kind == "PlayWAV") required = {"WAVFileName"};
                    else if (kind == "DisplayTextMessage") required = {"Message"};
                    else if (kind == "DisplayResourceTextMessage") required = {"ResourceStringID"};
                    else if (kind == "MakeNewTechnologyAvailable") required = {"PurchaseFileName"};
                    else if (kind == "ElapsedMissionTime") required = {"Time"};
                    else if (kind == "BooleanFlagIsSet" || kind == "SetBooleanFlag") {
                        if (const auto* index = find(entry, "FlagIndex")) {
                            if (!typed(entry, "FlagIndex", 0)) return reject(entry + "/FlagIndex", "malformed_value");
                            const auto value = std::get<std::int64_t>(index->value[0]);
                            if (value < 0 || value > legacyFlagIndexMax) return reject(entry + "/FlagIndex", "out_of_range");
                        } else required = {"FlagID"};
                    }
                    else if (kind.find("NumberOf") != std::string_view::npos) required = {"Num"};
                    else if (kind.starts_with("Move")) required = {"TargetCenterX", "TargetCenterY", "TargetRadius"};
                    else if (kind.find("Specific") != std::string_view::npos) required = {"PositionX", "PositionY"};
                    for (const auto key : required) {
                        const auto type = key == "AVIFileName" || key == "WAVFileName" || key == "Message" || key == "PurchaseFileName" || key == "FlagID" ? 3U : key == "ResourceStringID" || key == "Num" ? 0U : 1U;
                        if (!typed(entry, key, type)) return reject(entry + "/" + std::string(key), "missing_or_malformed_required_payload");
                    }
                    ObjectiveEntry value{sectionId(entry), ConditionSpecies{}};
                    if (condition) value.species = static_cast<ConditionSpecies>(species);
                    else value.species = static_cast<ActionSpecies>(species);
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
        const auto count = u32(placements, 0);
        if (count > nodeLimit || placements.size() != 4 + static_cast<std::size_t>(count) * 40) return reject("Packet1", "placement_count_mismatch");
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = 4 + i * 40;
            Entity entity{EntityId{entities_.size() + 1}, {}, {}, i, u32(placements, p), {}, f32(placements, p + 16), std::bit_cast<std::int32_t>(u32(placements, p + 24))};
            for (std::size_t j = 0; j < 3; ++j) entity.position[j] = f32(placements, p + 4 + j * 4);
            if (!std::isfinite(entity.rotation) || !std::isfinite(entity.position[0]) || !std::isfinite(entity.position[1]) || !std::isfinite(entity.position[2])) return reject("Packet1", "nonfinite_placement");
            entities_.push_back(entity);
        }
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
        std::optional<NodeId> block;
        for (const auto& s : syntax_.sections) if (s.name == section && !s.parent) block = s.id;
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
        const auto brain = sectionId("Warrior" + std::to_string(i));
        if (brain.value) entity.brain = brain;
        entities_.push_back(std::move(entity));
    }
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
    for (std::size_t i = 0; i < syntax_.fields.size(); ++i) if (syntax_.fields[i].value != savepoint_[i]) return true;
    return false;
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
    // The first shell slice exposes authored text only; numeric schema edits arrive with their tools.
    if (target->prefix != "st" || (target != find("MissionSettings", "MissionName") && target != find("MissionSettings", "Author")))
        return std::unexpected(LoadError{target->name, "unsupported_edit", 0});
    syntax_.fields[static_cast<std::size_t>(id.value - 1)].value = std::move(value);
    ++revision_;
    return {};
}

std::expected<void, LoadError> MissionDocument::save(const std::filesystem::path& stem, const Diagnostic& log)
{
    const auto reject = [&](std::string reason) -> std::expected<void, LoadError> {
        LoadError error{"save", std::move(reason), 0}; diagnostic(log, mission_, error); return std::unexpected(error);
    };
    auto fit = stem; fit.replace_extension(".fit");
    auto pak = stem; pak.replace_extension(".pak");
    const auto transaction = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto fitStage = fit; fitStage += ".stage." + transaction;
    auto pakStage = pak; pakStage += ".stage." + transaction;
    auto fitBackup = fit; fitBackup += ".previous." + transaction;
    auto pakBackup = pak; pakBackup += ".previous." + transaction;
    auto journal = fit; journal.replace_extension(); journal += ".transaction." + transaction;
    std::error_code ec;
    if (std::filesystem::exists(fitStage, ec) || std::filesystem::exists(pakStage, ec) || std::filesystem::exists(fitBackup, ec) || std::filesystem::exists(pakBackup, ec)) return reject("recovery_files_present");
    if (ec) return reject("destination_query_failed");
    const bool hadFit = std::filesystem::exists(fit, ec);
    if (ec || (hadFit && !std::filesystem::is_regular_file(fit, ec))) return reject("destination_not_regular");
    const bool hadPak = std::filesystem::exists(pak, ec);
    if (ec || (hadPak && !std::filesystem::is_regular_file(pak, ec))) return reject("destination_not_regular");
    if (!fit.parent_path().empty()) std::filesystem::create_directories(fit.parent_path(), ec);
    if (ec) return reject("staging_directory_failed");
    const auto output = fitBytes();
    if (!writeBytes(fitStage, std::as_bytes(std::span(output))) || !writeBytes(pakStage, pakBytes())) return reject("staging_write_failed");
    auto verified = read(fitStage, pakStage, {});
    if (!verified || verified->fitBytes() != output || verified->pak_.originalBytes != pak_.originalBytes) return reject("staging_verification_failed");
    const std::string pending = "pending\nFIT=" + fit.string() + "\nPAK=" + pak.string() + "\nFIT_previous=" + fitBackup.string() + "\nPAK_previous=" + pakBackup.string() + "\n";
    if (!writeBytes(journal, std::as_bytes(std::span(pending)))) return reject("journal_write_failed");
    bool oldFit{}, oldPak{}, newFit{}, newPak{};
    auto rollback = [&] {
        bool ok = true;
        const auto restore = [&](const auto& from, const auto& to) {
            std::error_code error; std::filesystem::rename(from, to, error); ok &= !error;
        };
        if (newFit) restore(fit, fitStage);
        if (newPak) restore(pak, pakStage);
        if (oldFit) restore(fitBackup, fit);
        if (oldPak) restore(pakBackup, pak);
        if (ok) {
            const std::string rolledBack = "rolled_back\n" + pending.substr(8);
            ok = writeBytes(journal, std::as_bytes(std::span(rolledBack)));
        }
        return ok;
    };
    if (hadFit) { std::filesystem::rename(fit, fitBackup, ec); if (ec) return reject(rollback() ? "backup_failed" : "rollback_failed"); oldFit = true; }
    if (hadPak) { std::filesystem::rename(pak, pakBackup, ec); if (ec) return reject(rollback() ? "backup_failed" : "rollback_failed"); oldPak = true; }
    std::filesystem::rename(fitStage, fit, ec);
    if (ec) return reject(rollback() ? "publication_failed" : "rollback_failed");
    newFit = true;
    std::filesystem::rename(pakStage, pak, ec);
    if (ec) return reject(rollback() ? "publication_failed" : "rollback_failed");
    newPak = true;
    const std::string committed = "committed\n" + pending.substr(8);
    if (!writeBytes(journal, std::as_bytes(std::span(committed)))) return reject(rollback() ? "journal_commit_failed" : "rollback_failed");
    for (std::size_t i = 0; i < syntax_.fields.size(); ++i) savepoint_[i] = syntax_.fields[i].value;
    return {};
}
}
