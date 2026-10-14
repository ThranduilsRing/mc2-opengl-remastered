#include "PlacementPolicy.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <limits>
#include <charconv>
#include <zlib.h>

namespace editor::document {
namespace {
CatalogSource source(const ResolvedFile& file) { return {file.key, file.layer, file.source}; }
bool stem(std::string_view text) {
    return !text.empty() && text.size() <= 255 && std::ranges::all_of(text, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
auto reject(std::string reason, const Diagnostic& log) {
    if (log) log("[EDITOR_PLACEMENT_REJECT v1] reason=" + reason);
    return std::unexpected(LoadError{"placement", std::move(reason), 0});
}
std::expected<std::string, LoadError> tableText(const ResolvedFile& file) {
    std::string text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
    if (text.size() < 2 || static_cast<unsigned char>(text[0]) != 0x78 ||
        ((static_cast<unsigned char>(text[0]) << 8) + static_cast<unsigned char>(text[1])) % 31 != 0) {
        if (text.size() > 64 * 1024) return std::unexpected(LoadError{"pilot", "table_size_limit", 0});
        return text;
    }
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return std::unexpected(LoadError{"pilot", "decode_init", 0});
    std::string decoded(64 * 1024, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(text.data()); stream.avail_in = static_cast<uInt>(text.size());
    stream.next_out = reinterpret_cast<Bytef*>(decoded.data()); stream.avail_out = static_cast<uInt>(decoded.size());
    const auto status = inflate(&stream, Z_FINISH);
    const auto size = stream.total_out;
    const bool valid = status == Z_STREAM_END && stream.avail_in == 0;
    inflateEnd(&stream);
    if (!valid) return std::unexpected(LoadError{"pilot", "invalid_table_encoding", 0});
    decoded.resize(size); return decoded;
}
bool pilotDefinition(const ResolvedFile& file) {
    const std::string text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
    if (text.find('\0') != text.npos) return false;
    std::istringstream lines(text);
    std::string line, section;
    bool header{}, end{}, desc{};
    while (std::getline(lines, line)) {
        const auto comment = line.find("//");
        if (comment != line.npos) line.resize(comment);
        const auto first = line.find_first_not_of(" \t\r");
        if (first == line.npos) continue;
        line = identifier(std::string_view(line).substr(first, line.find_last_not_of(" \t\r") - first + 1));
        if (line == "fitini") { if (header || end) return false; header = true; continue; }
        if (!header || end) return false;
        if (line == "fitend") end = true;
        else if (line.starts_with('[') && line.ends_with(']')) section = line;
        else if (section == "[general]") {
            std::istringstream declaration(line);
            std::string type, name, equal;
            declaration >> type >> name >> equal;
            if (name != "descindex") continue;
            if (desc || (type != "l" && type != "ul") || equal != "=") return false;
            std::string value; unsigned count{};
            // Stock descriptions contain multiple numeric resource IDs; selection uses the profile identity.
            while (declaration >> value) {
                std::int32_t number{};
                const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), number);
                if (error != std::errc{} || last != value.data() + value.size() || number < 0) return false;
                ++count;
            }
            desc = count != 0;
        }
    }
    return header && end && desc;
}
}
std::expected<PlacementPolicy, LoadError> PlacementPolicy::load(const FileResolver& resolver, Diagnostic log)
{
    PlacementPolicy policy;
    for (std::size_t index = 0; index < 2; ++index) {
        auto table = resolver.read(index ? "data/objects/badpilots.csv" : "data/objects/pilots.csv");
        if (!table) return reject("pilot_table_missing", log);
        auto text = tableText(*table);
        if (!text) return reject(text.error().reason, log);
        std::istringstream lines(*text);
        std::string name;
        bool ended{};
        while (std::getline(lines, name)) {
            if (!name.empty() && name.back() == '\r') name.pop_back();
            if (name.empty()) { ended = true; continue; }
            if (ended) return reject("pilot_table_trailing_data", log);
            if (!stem(name)) return reject("invalid_pilot_name", log);
            if (policy.pilots_[index].size() >= 512) return reject("pilot_count_limit", log);
            const auto key = "data/missions/warriors/" + name + ".fit";
            auto definition = resolver.read(key);
            // The legacy table excludes missing definitions; preserve its first usable pilot.
            if (!definition) {
                const auto keys = resolver.keys(key);
                if (std::ranges::find(keys, FileResolver::normalize(key)) != keys.end())
                    return reject("unreadable_pilot_definition", log);
                continue;
            }
            if (!pilotDefinition(*definition)) return reject("invalid_pilot_definition", log);
            policy.pilots_[index].push_back({name, source(*table), source(*definition)});
        }
        if (policy.pilots_[index].empty()) return reject("no_usable_pilot", log);
    }
    return policy;
}
std::expected<PlacementPlan, LoadError> PlacementPolicy::make(const MissionDocument& document,
    const CatalogEntry& entry, const PlacementChoice& choice, Diagnostic log) const
{
    const auto finite = [](double v) { return std::isfinite(v) && std::abs(v) <= 3.402823466e38; };
    if (choice.player < 0 || choice.player > 7 || choice.team < 0 || choice.team > 7 ||
        !finite(choice.transform.rotation) || !std::ranges::all_of(choice.transform.position, finite) ||
        !stem(entry.stem) || choice.variant > 15 || choice.forestInstance < -1 ||
        choice.forestInstance == std::numeric_limits<std::int32_t>::max() || choice.forestScale < 0) return reject("invalid_choice", log);
    const auto* team = document.find("Player" + std::to_string(choice.player), "DefaultTeam");
    if (!team || team->value.size() != 1 || !std::holds_alternative<std::int64_t>(team->value[0]) ||
        std::get<std::int64_t>(team->value[0]) != choice.team) return reject("player_team_mismatch", log);
    PlacementPlan plan;
    plan.sources = {entry.enumeration, entry.definition};
    if (entry.variantFile) plan.sources.push_back(*entry.variantFile);
    if (entry.family == ObjectFamily::mech || entry.family == ObjectFamily::vehicle) {
        if (choice.forestInstance != -1 || choice.forestScale != 0) return reject("unit_forest", log);
        const bool player = choice.player == 0, vehicle = entry.appearanceType == CatalogAppearance::vehicle;
        const auto& pilots = pilots_[player ? 0 : 1];
        if (pilots.empty()) return reject("no_usable_pilot", log);
        UnitPlacement unit;
        unit.objectNumber = entry.id; unit.transform = choice.transform;
        unit.team = choice.team; unit.commander = choice.player; unit.playerPart = player;
        unit.controlDataType = vehicle ? 2 : 1;
        unit.objectProfile = vehicle ? (player ? "pv20600" : "pv20500") : (player ? "PM207300" : "PM101100");
        unit.csvFile = entry.stem; unit.variant = choice.variant;
        unit.writeElevation = false;
        unit.warriorProfile = pilots.front().profile; unit.legacyBrain = player ? "PBrain" : "DredAttack01";
        if (!player) unit.cells = {{2, 1, 150}, {9, 0, 0}, {28, 0, 0}};
        plan.sources.push_back(pilots.front().table); plan.sources.push_back(pilots.front().definition);
        plan.value = std::move(unit);
    } else {
        if (choice.variant) return reject("building_variant", log);
        BuildingPlacement building;
        building.objectNumber = entry.id; building.transform = choice.transform;
        building.team = entry.alignable ? choice.player : -1;
        building.forestInstance = choice.forestInstance; building.scale = choice.forestScale;
        if (entry.family == ObjectFamily::marker) building.markerRadius = 256;
        plan.value = building;
    }
    return plan;
}
std::expected<EntityId, LoadError> PlacementPlan::insert(CommandHistory& history) const
{
    return std::visit([&](const auto& placement) -> std::expected<EntityId, LoadError> {
        if constexpr (std::is_same_v<std::decay_t<decltype(placement)>, UnitPlacement>) return history.insertUnit(placement);
        else return history.insertBuilding(placement);
    }, value);
}
}
