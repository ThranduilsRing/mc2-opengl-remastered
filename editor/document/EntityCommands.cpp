#include "CommandHistory.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_set>

namespace editor::document {
namespace {
// Runtime reserves warrior slot zero and loads at most MAX_WARRIORS - 1 assignments.
constexpr std::int64_t creationWarriorLimit = 119;
struct Edit { Span span; std::string text; };
const Section* root(const Syntax& syntax, std::string_view name)
{
    for (const auto& section : syntax.sections)
        if (!section.parent && identifier(section.name) == identifier(name)) return &section;
    return nullptr;
}
const Field* find(const Syntax& syntax, const Section& section, std::string_view name)
{
    for (auto id : section.fields) {
        const auto& field = syntax.fields[static_cast<std::size_t>(id.value - 1)];
        if (identifier(field.name) == identifier(name)) return &field;
    }
    return nullptr;
}
bool commanderGroup(const Section& section)
{
    if (section.parent) return false;
    const auto name = identifier(section.name);
    if (!name.starts_with("commander")) return false;
    const auto separator = name.find("group:", 9);
    if (separator == std::string::npos) return false;
    const auto digits = [](std::string_view text) {
        return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
    };
    return digits(std::string_view(name).substr(9, separator - 9)) &&
        digits(std::string_view(name).substr(separator + 6));
}
std::string edited(std::string bytes, std::vector<Edit> edits)
{
    std::ranges::stable_sort(edits, [](const Edit& a, const Edit& b) { return a.span.begin > b.span.begin; });
    for (const auto& edit : edits) bytes.replace(edit.span.begin, edit.span.end - edit.span.begin, edit.text);
    return bytes;
}
void put(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>(value >> (8 * i));
}
void putTransform(std::vector<std::byte>& bytes, std::size_t offset, const Transform& transform)
{
    for (std::size_t i = 0; i < 3; ++i) put(bytes, offset + 4 + i * 4, std::bit_cast<std::uint32_t>(static_cast<float>(transform.position[i])));
    put(bytes, offset + 16, std::bit_cast<std::uint32_t>(static_cast<float>(transform.rotation)));
}
bool valid(const Transform& transform)
{
    const auto scalar = [](double n) { return std::isfinite(n) && std::abs(n) <= std::numeric_limits<float>::max(); };
    return scalar(transform.rotation) && std::ranges::all_of(transform.position, scalar);
}
bool validText(const std::string& text)
{
    return text.size() <= textByteLimit && text.find_first_of("\"\r\n") == std::string::npos && text.find('\0') == std::string::npos;
}
std::string real(double value) { return std::format("{:.9g}", static_cast<float>(value)); }
std::string newline(const std::string& bytes) { return bytes.find("\r\n") == std::string::npos ? "\n" : "\r\n"; }
std::unexpected<LoadError> reject(std::string reason) { return std::unexpected(LoadError{"entity", std::move(reason), 0}); }
std::vector<EntityId> identities(const MissionDocument& document)
{
    std::vector<EntityId> ids;
    for (const auto& entity : document.entities()) ids.push_back(entity.id);
    return ids;
}
}

std::expected<EntityId, LoadError> CommandHistory::insertBuilding(const BuildingPlacement& building)
{
    const auto reject = [this](std::string reason) {
        if (log_) log_("[EDITOR_PLACEMENT_REJECT v1] reason=" + reason);
        return std::unexpected(LoadError{"placement", std::move(reason), 0});
    };
    if (!valid(building.transform) || building.team < -1 || building.team >= 8 || building.damage < 0 ||
        building.forestInstance < -1 || building.forestInstance == std::numeric_limits<std::int32_t>::max() ||
        building.scale < 0 || (building.markerRadius && (!std::isfinite(*building.markerRadius) ||
            *building.markerRadius < 0 || *building.markerRadius > std::numeric_limits<float>::max()))) return reject("invalid_placement");
    auto fit = document_.fitBytes();
    if (building.markerRadius) {
        auto syntax = decodeFit(fit);
        if (!syntax) return std::unexpected(syntax.error());
        const auto* markers = root(*syntax, "NavMarkers");
        const auto* count = markers ? find(*syntax, *markers, "NumNavMarkers") : nullptr;
        if (markers && (!count || count->value.size() != 1 || !std::holds_alternative<std::int64_t>(count->value[0])))
            return reject("invalid_marker_count");
        const auto number = count ? std::get<std::int64_t>(count->value[0]) : 0;
        if (number < 0 || number >= nodeLimit || root(*syntax, "NavMarker" + std::to_string(number))) return reject("invalid_marker_count");
        const auto nl = newline(fit);
        std::string body = nl;
        if (!markers) body += "[NavMarkers]" + nl + "ul NumNavMarkers = 1" + nl;
        body += "[NavMarker" + std::to_string(number) + "]" + nl + "f xPos = " + real(building.transform.position[0]) + nl +
            "f yPos = " + real(building.transform.position[1]) + nl + "f radius = " + real(*building.markerRadius) + nl;
        std::vector<Edit> edits{{{syntax->endMarker, syntax->endMarker}, std::move(body)}};
        if (count) edits.push_back({count->syntax, std::to_string(number + 1)});
        fit = edited(std::move(fit), std::move(edits));
    }
    auto placements = document_.placements_;
    const auto count = std::ranges::count_if(document_.entities(), [](const Entity& e) { return e.packetRecord.has_value(); });
    if (count >= nodeLimit) return reject("entity_count_limit");
    if (placements.empty()) placements.resize(4);
    const auto offset = placements.size();
    placements.resize(offset + 40);
    put(placements, 0, static_cast<std::uint32_t>(count + 1));
    put(placements, offset, building.objectNumber);
    putTransform(placements, offset, building.transform);
    put(placements, offset + 20, std::bit_cast<std::uint32_t>(building.damage));
    put(placements, offset + 24, std::bit_cast<std::uint32_t>(building.team));
    put(placements, offset + 28, building.parentCell);
    put(placements, offset + 32, static_cast<std::uint32_t>(building.forestInstance + 1));
    put(placements, offset + 36, building.forestInstance == -1 ? 0 : static_cast<std::uint32_t>(building.scale));
    const EntityId id{document_.nextEntityId_};
    auto ids = identities(document_);
    ids.insert(ids.begin() + count, id);
    auto applied = commitStructure(std::move(fit), std::move(placements), ids, "Place building");
    if (!applied) return std::unexpected(applied.error());
    ++document_.nextEntityId_;
    return id;
}

std::expected<EntityId, LoadError> CommandHistory::insertUnit(const UnitPlacement& unit)
{
    const auto reject = [this](std::string reason) {
        if (log_) log_("[EDITOR_PLACEMENT_REJECT v1] reason=" + reason);
        return std::unexpected(LoadError{"placement", std::move(reason), 0});
    };
    const auto name = [](const std::string& value) {
        return validText(value) && value.size() <= 255 && value.find('\\') == value.npos;
    };
    if (!valid(unit.transform) || unit.team < -1 || unit.team >= 8 || unit.commander < -1 || unit.commander >= 8 ||
        !name(unit.objectProfile) || !name(unit.csvFile) || !name(unit.warriorProfile) || !name(unit.legacyBrain) ||
        unit.objectProfile.empty() || unit.csvFile.empty() || unit.warriorProfile.empty() || unit.legacyBrain.empty() ||
        unit.damage < 0 || unit.variant > 15 || unit.staticVars < 0 || unit.cells.size() > 255) return reject("invalid_unit");
    std::unordered_set<std::int32_t> cells;
    for (const auto& cell : unit.cells) {
        if (cell.number < 0 || (cell.type != 0 && cell.type != 1) || !std::isfinite(cell.value) ||
            std::abs(cell.value) > std::numeric_limits<float>::max() || !cells.insert(cell.number).second)
            return reject("invalid_brain_cell");
    }
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return std::unexpected(syntax.error());
    const auto* parts = root(*syntax, "Parts");
    const auto* warriors = root(*syntax, "Warriors");
    const auto* count = find(*syntax, *parts, "NumParts");
    const auto part = std::get<std::int64_t>(count->value[0]) + 1;
    std::int64_t warrior = 1;
    if (warriors) {
        const auto* n = find(*syntax, *warriors, "NumWarriors");
        if (!n || n->value.size() != 1 || !std::holds_alternative<std::int64_t>(n->value[0])) return reject("invalid_warrior_count");
        warrior = std::get<std::int64_t>(n->value[0]) + 1;
    }
    if (part > 65535 || warrior <= 0 || warrior > 65535) return reject("entity_count_limit");
    if (warrior > creationWarriorLimit) return reject("runtime_warrior_limit");
    // Orphan warrior blocks stay authored; creation must never shadow one of them.
    if (root(*syntax, "Warrior" + std::to_string(warrior))) return reject("warrior_index_collision");
    std::uint64_t nextSquad = 1;
    std::unordered_set<std::int64_t> alternatives;
    const auto integer = [&](const Section& section, std::string_view key, std::int64_t fallback) {
        const auto* field = find(*syntax, section, key);
        return field && field->value.size() == 1 && std::holds_alternative<std::int64_t>(field->value[0]) ?
            std::get<std::int64_t>(field->value[0]) : fallback;
    };
    for (const auto& entity : document_.entities()) if (entity.section) {
        const auto& section = *document_.section(*entity.section);
        const auto squad = integer(section, "SquadNum", 0);
        if (squad < 0 || squad >= std::numeric_limits<std::uint32_t>::max()) return reject("invalid_squad");
        nextSquad = std::max(nextSquad, static_cast<std::uint64_t>(squad) + 1);
        const auto n = integer(section, "NumAlternatives", 0);
        const auto* indices = find(*syntax, section, "IndicesOfAlternatives");
        if (n < 0 || (n && (!indices || static_cast<std::size_t>(n) > indices->value.size()))) return reject("invalid_alternatives");
        for (std::int64_t i = 0; i < n; ++i) {
            const auto* index = std::get_if<std::int64_t>(&indices->value[static_cast<std::size_t>(i)]);
            if (!index || *index <= 0 || *index >= part) return reject("invalid_alternatives");
            alternatives.insert(*index);
        }
    }
    const auto squad = unit.squad.value_or(static_cast<std::uint32_t>(nextSquad));
    // Empty alternative cursors retain the legacy export value; existing authored references stay untouched.
    std::int64_t alternativeStart = part - static_cast<std::int64_t>(alternatives.size()) + 2;
    for (const auto& entity : document_.entities()) if (entity.section && !alternatives.contains(*entity.exportedPart)) {
        const auto& section = *document_.section(*entity.section);
        const auto player = integer(section, "CommanderID", 0), previousSquad = integer(section, "SquadNum", 0);
        if (player < unit.commander || (player == unit.commander && previousSquad <= squad))
            alternativeStart += 1 + integer(section, "NumAlternatives", 0);
    }
    const auto nl = newline(fit);
    std::vector<Edit> edits{{count->syntax, std::to_string(part)}};
    if (warriors) edits.push_back({find(*syntax, *warriors, "NumWarriors")->syntax, std::to_string(warrior)});
    std::string body = nl;
    if (!warriors) body += "[Warriors]" + nl + "ul NumWarriors = 1" + nl;
    body += "[Warrior" + std::to_string(warrior) + "]" + nl + "st Profile = \"" + unit.warriorProfile + "\"" + nl +
        "st Brain = \"" + unit.legacyBrain + "\"" + nl + "l NumCells = " + std::to_string(unit.cells.size()) + nl +
        "l NumStaticVars = " + std::to_string(unit.staticVars) + nl;
    for (std::size_t i = 0; i < unit.cells.size(); ++i) {
        const auto& cell = unit.cells[i];
        body += "[Warrior" + std::to_string(warrior) + "Cell" + std::to_string(i) + "]" + nl +
            "l Cell = " + std::to_string(cell.number) + nl + "l MemType = " + std::to_string(cell.type) + nl +
            "f Value = " + real(cell.value) + nl;
    }
    body += "[Part" + std::to_string(part) + "]" + nl + "ul ObjectNumber = " + std::to_string(unit.objectNumber) + nl +
        "ul ControlType = " + std::to_string(unit.controlType) + nl + "ul ControlDataType = " + std::to_string(unit.controlDataType) + nl +
        "st ObjectProfile = \"" + unit.objectProfile + "\"" + nl + "st CSVFile = \"" + unit.csvFile + "\"" + nl +
        "ul Pilot = " + std::to_string(warrior) + nl + "c TeamID = " + std::to_string(unit.team) + nl +
        "c CommanderID = " + std::to_string(unit.commander) + nl + "b PlayerPart = " + (unit.playerPart ? "TRUE" : "FALSE") + nl +
        "l Active = 1" + nl + "l Exists = 1" + nl + "c MyIcon = 0" + nl +
        "f Damage = " + real(unit.damage) + nl + "ul BaseColor = " + std::to_string(unit.baseColor) + nl +
        "ul HighlightColor1 = " + std::to_string(unit.highlightColor1) + nl + "ul HighlightColor2 = " + std::to_string(unit.highlightColor2) + nl +
        "ul SelfRepairBehavior = " + (unit.selfRepair ? "1" : "0") + nl + "ul VariantNumber = " + std::to_string(unit.variant) + nl +
        "ul SquadNum = " + std::to_string(squad) + nl + "ul NumAlternatives = 0" + nl + "ul AlternativeStartIndex = " + std::to_string(alternativeStart) + nl +
        "l[15] IndicesOfAlternatives = -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1," + nl;
    for (std::size_t i = 0; i < (unit.writeElevation ? 3u : 2u); ++i) body += "f Position" + std::string(1, "XYZ"[i]) + " = " + real(unit.transform.position[i]) + nl;
    body += "f Rotation = " + real(unit.transform.rotation) + nl;
    edits.push_back({{syntax->endMarker, syntax->endMarker}, std::move(body)});
    auto ids = identities(document_);
    const EntityId id{document_.nextEntityId_++};
    ids.push_back(id);
    auto applied = commitStructure(edited(std::move(fit), std::move(edits)), document_.placements_, ids, "Place unit");
    if (!applied) return std::unexpected(applied.error());
    return id;
}

std::expected<void, LoadError> CommandHistory::transform(EntityId id, const Transform& transform)
{
    const auto* entity = document_.entity(id);
    if (!entity) return reject("missing_entity");
    if (!valid(transform)) {
        if (log_) log_("[EDITOR_ENTITY_REJECT v1] action=transform reason=invalid_transform");
        return reject("invalid_transform");
    }
    if (entity->position == transform.position && entity->rotation == transform.rotation) return {};
    const auto marker = document_.markerReference(id);
    if (!marker) {
        if (log_) log_("[EDITOR_ENTITY_REJECT v1] action=transform reason=" + marker.error().reason);
        return std::unexpected(marker.error());
    }
    if (*marker && std::ranges::any_of(document_.entities(), [&](const Entity& other) {
        return other.id != id && document_.isMarkerPacket(other) &&
            std::abs(other.position[0] - static_cast<float>(transform.position[0])) < 1.0 &&
            std::abs(other.position[1] - static_cast<float>(transform.position[1])) < 1.0;
    })) {
        if (log_) log_("[EDITOR_ENTITY_REJECT v1] action=transform reason=marker_position_collision");
        return reject("marker_position_collision");
    }
    auto placements = document_.placements_;
    auto fit = document_.fitBytes();
    if (entity->packetRecord) {
        putTransform(placements, 4 + *entity->packetRecord * 40, transform);
        if (*marker) {
            auto syntax = decodeFit(fit);
            if (!syntax) return std::unexpected(syntax.error());
            const auto* section = root(*syntax, document_.section(**marker)->name);
            fit = edited(std::move(fit), {{find(*syntax, *section, "xPos")->syntax, real(transform.position[0])},
                {find(*syntax, *section, "yPos")->syntax, real(transform.position[1])}});
        }
    }
    else {
        auto syntax = decodeFit(fit);
        if (!syntax) return std::unexpected(syntax.error());
        const auto* section = root(*syntax, document_.section(*entity->section)->name);
        std::vector<Edit> edits;
        std::string absent;
        const auto nl = newline(fit);
        const std::array<std::string_view, 4> keys{"PositionX", "PositionY", "PositionZ", "Rotation"};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto value = i == 3 ? transform.rotation : transform.position[i];
            const auto previous = i == 3 ? entity->rotation : entity->position[i];
            if (value == previous) continue;
            if (const auto* field = find(*syntax, *section, keys[i])) edits.push_back({field->syntax, real(value)});
            else absent += "f " + std::string(keys[i]) + " = " + real(value) + nl;
        }
        if (!absent.empty()) edits.push_back({{section->syntax.end, section->syntax.end}, nl + absent});
        fit = edited(std::move(fit), std::move(edits));
    }
    return commitStructure(std::move(fit), std::move(placements), identities(document_), "Transform entity");
}

std::expected<void, LoadError> CommandHistory::erase(EntityId id)
{
    const auto* entity = document_.entity(id);
    if (!entity) return reject("missing_entity");
    const auto marker = document_.markerReference(id);
    if (!marker) {
        if (log_) log_("[EDITOR_ENTITY_REJECT v1] action=delete reason=" + marker.error().reason);
        return std::unexpected(marker.error());
    }
    auto ids = identities(document_);
    std::erase(ids, id);
    auto placements = document_.placements_;
    auto fit = document_.fitBytes();
    std::unordered_map<std::string, std::string> renamed;
    if (entity->packetRecord) {
        const auto offset = 4 + *entity->packetRecord * 40;
        placements.erase(placements.begin() + offset, placements.begin() + offset + 40);
        put(placements, 0, static_cast<std::uint32_t>((placements.size() - 4) / 40));
        if (*marker) {
            auto syntax = decodeFit(fit);
            if (!syntax) return std::unexpected(syntax.error());
            const auto* removed = root(*syntax, document_.section(**marker)->name);
            const auto* count = find(*syntax, *root(*syntax, "NavMarkers"), "NumNavMarkers");
            const auto total = std::get<std::int64_t>(count->value[0]);
            std::vector<Edit> edits{{removed->syntax, ""}, {count->syntax, std::to_string(total - 1)}};
            renamed.emplace(identifier(removed->name), "");
            bool following{};
            for (std::int64_t i = 0; i < total; ++i) {
                const auto* section = root(*syntax, "NavMarker" + std::to_string(i));
                if (section == removed) { following = true; continue; }
                if (!following) continue;
                // Surviving blocks retain their stable IDs and unknown fields after export renumbering.
                const auto name = "NavMarker" + std::to_string(i - 1);
                edits.push_back({section->nameSyntax, name});
                renamed.emplace(identifier(section->name), identifier(name));
            }
            fit = edited(std::move(fit), std::move(edits));
        }
    } else {
        auto syntax = decodeFit(fit);
        if (!syntax) return std::unexpected(syntax.error());
        const auto* parts = root(*syntax, "Parts");
        const auto* count = find(*syntax, *parts, "NumParts");
        const auto total = std::get<std::int64_t>(count->value[0]);
        const auto* removed = root(*syntax, document_.section(*entity->section)->name);
        std::vector<Edit> edits{{removed->syntax, ""}, {count->syntax, std::to_string(total - 1)}};
        if (const auto* alternatives = find(*syntax, *removed, "NumAlternatives"); alternatives &&
            (alternatives->value.size() != 1 || !std::holds_alternative<std::int64_t>(alternatives->value[0]) ||
                std::get<std::int64_t>(alternatives->value[0]) != 0)) return reject("part_references_require_graph_edit");
        for (std::int64_t i = 1; i <= total; ++i) {
            if (i == *entity->exportedPart) continue;
            const auto* section = root(*syntax, "Part" + std::to_string(i));
            for (auto key : {"IndicesOfAlternatives", "AlternativeStartIndex"}) {
                const auto* field = find(*syntax, *section, key);
                if (!field) continue;
                auto values = field->value;
                for (auto& value : values) {
                    auto* index = std::get_if<std::int64_t>(&value);
                    if (!index) return reject("malformed_part_reference");
                    if (*index == *entity->exportedPart) return reject("part_references_require_graph_edit");
                    if (*index > *entity->exportedPart) --*index;
                }
                if (values != field->value) edits.push_back({field->syntax, formatValue(*field, values)});
            }
        }
        // Empty slots preserve group positions; only surviving Part references compact.
        for (const auto& section : syntax->sections) {
            if (!commanderGroup(section)) continue;
            const auto* mates = find(*syntax, section, "Mates");
            if (!mates || mates->prefix != "l" || mates->value.size() != 12) return reject("malformed_group_mates");
            auto values = mates->value;
            for (auto& value : values) {
                auto* index = std::get_if<std::int64_t>(&value);
                if (!index || *index > total) return reject("malformed_group_mates");
                if (*index == *entity->exportedPart) *index = 0;
                else if (*index > *entity->exportedPart) --*index;
            }
            if (values != mates->value) edits.push_back({mates->syntax, formatValue(*mates, values)});
        }
        renamed.emplace(identifier(removed->name), "");
        // Runtime requires dense Part indices; document IDs and explicit Pilot links remain stable.
        for (auto i = *entity->exportedPart + 1; i <= total; ++i) {
            const auto* section = root(*syntax, "Part" + std::to_string(i));
            const auto name = "Part" + std::to_string(i - 1);
            edits.push_back({section->nameSyntax, name});
            renamed.emplace(identifier(section->name), identifier(name));
        }
        fit = edited(std::move(fit), std::move(edits));
    }
    return commitStructure(std::move(fit), std::move(placements), ids, "Delete entity", renamed);
}

std::expected<void, LoadError> CommandHistory::setScalar(NodeId id, std::string_view key,
    std::string_view prefix, Scalar value, std::string label, bool exactReal)
{
    const auto* original = document_.section(id);
    if (!original) return reject("missing_section");
    if (key != "compatibilityMode")
        if (const auto* old = document_.find(id, key); old && old->prefix == prefix && old->value == Value{value}) return {};
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return std::unexpected(syntax.error());
    const auto index = static_cast<std::size_t>(original - document_.syntax().sections.data());
    const auto& section = syntax->sections[index];
    Field spelling; spelling.prefix = prefix;
    const auto text = exactReal && std::holds_alternative<double>(value) ?
        std::format("{:.17g}", std::get<double>(value)) : formatValue(spelling, Value{std::move(value)});
    if (const auto* field = find(*syntax, section, key)) {
        std::vector<Edit> edits{{field->syntax, text}};
        if (!section.parent && field->prefix != prefix) {
            auto end = fit.rfind(field->name, field->syntax.begin);
            auto begin = fit.find_last_of("\r\n;{}", end);
            begin = begin == fit.npos ? 0 : begin + 1;
            while (begin < end && (fit[begin] == ' ' || fit[begin] == '\t')) ++begin;
            while (end && (fit[end - 1] == ' ' || fit[end - 1] == '\t')) --end;
            edits.push_back({{begin, end}, std::string(prefix)});
        }
        if (section.parent && key == "compatibilityMode") {
            // The game skips Brain's opening line and treats sub-block lines separately.
            // Only this declaration gains line boundaries; other authored bytes survive.
            const auto begin = fit.rfind(field->name, field->syntax.begin);
            auto end = field->syntax.end;
            while (end < fit.size() && (fit[end] == ' ' || fit[end] == '\t')) ++end;
            if (end < fit.size() && fit[end] == ';') ++end;
            const auto line = fit.rfind('\n', begin);
            const auto linePrefix = std::string_view(fit).substr(line == std::string::npos ? 0 : line + 1,
                begin - (line == std::string::npos ? 0 : line + 1));
            const auto nextLine = fit.find('\n', end);
            const auto suffix = std::string_view(fit).substr(end, nextLine == std::string::npos ? fit.size() - end : nextLine - end);
            if (linePrefix.find_first_not_of(" \t\r") != std::string_view::npos)
                edits.push_back({{begin, begin}, newline(fit)});
            if (suffix.find_first_not_of(" \t\r") != std::string_view::npos)
                edits.push_back({{end, end}, newline(fit)});
        }
        fit = edited(std::move(fit), std::move(edits));
        if (fit == document_.fitBytes()) return {};
    }
    else {
        const auto point = section.syntax.end - (section.parent ? 1 : 0);
        const auto nl = newline(fit);
        const auto declaration = (section.parent ? " " : std::string(prefix) + " ") +
            std::string(key) + " = " + text + (section.parent ? ";" : "") + nl;
        fit.insert(point, nl + declaration);
    }
    return commitStructure(std::move(fit), document_.placements_, identities(document_), std::move(label));
}

std::expected<void, LoadError> CommandHistory::setTeam(EntityId id, std::int64_t team)
{
    const auto* entity = document_.entity(id);
    if (!entity) return reject("missing_entity");
    if (team < -1 || team >= 8) return reject("invalid_team");
    if (entity->team == team) return {};
    if (entity->section) return setScalar(*entity->section, "TeamID", "c", team, "Entity team");
    auto placements = document_.placements_;
    put(placements, 4 + *entity->packetRecord * 40 + 24, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(team)));
    return commitStructure(document_.fitBytes(), std::move(placements), identities(document_), "Building team");
}

std::expected<void, LoadError> CommandHistory::setPlayerTeam(NodeId id, std::int64_t team)
{
    const auto* section = document_.section(id);
    if (!section || section->parent) return reject("missing_player");
    const auto name = identifier(section->name);
    if (name.size() != 7 || !name.starts_with("player") || name.back() < '0' || name.back() > '7') return reject("missing_player");
    if (team < 0 || team >= 8) return reject("invalid_team");
    return setScalar(id, "DefaultTeam", "ul", team, "Player alignment");
}

std::expected<void, LoadError> CommandHistory::setLegacyBrain(EntityId id, std::string module)
{
    const auto* entity = document_.entity(id);
    if (!entity || !entity->warrior) return reject("missing_warrior_assignment");
    if (!validText(module)) return reject("invalid_module_reference");
    return setScalar(*entity->warrior, "Brain", "st", std::move(module), "Legacy ABL module");
}

std::expected<void, LoadError> CommandHistory::setBrainMode(EntityId id, std::string_view mode)
{
    if (mode != "Legacy" && mode != "Hybrid" && mode != "Enhanced") return reject("invalid_brain_mode");
    const auto* entity = document_.entity(id);
    if (!entity || !entity->warrior) return reject("missing_warrior_assignment");
    if (entity->brain) return setScalar(*entity->brain, "compatibilityMode", "", std::string(mode), "Brain compatibility");
    // Absence means Legacy; choosing it preserves imported scalar brains and cells.
    if (mode == "Legacy") return {};
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return std::unexpected(syntax.error());
    const auto* warrior = root(*syntax, document_.section(*entity->warrior)->name);
    const auto nl = newline(fit);
    fit.insert(warrior->syntax.end, nl + "Brain {" + nl + " compatibilityMode = \"" +
        std::string(mode) + "\";" + nl + "}" + nl);
    return commitStructure(std::move(fit), document_.placements_, identities(document_), "Brain compatibility");
}
}
