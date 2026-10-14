#include "CommandHistory.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>

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
    if (!valid(building.transform) || building.team < -1 || building.team >= 8) return reject("invalid_placement");
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
    put(placements, offset + 32, building.forest);
    put(placements, offset + 36, building.scale);
    const EntityId id{document_.nextEntityId_++};
    auto ids = identities(document_);
    ids.insert(ids.begin() + count, id);
    auto applied = commitStructure(document_.fitBytes(), std::move(placements), ids, "Place building");
    if (!applied) return std::unexpected(applied.error());
    return id;
}

std::expected<EntityId, LoadError> CommandHistory::insertUnit(const UnitPlacement& unit)
{
    if (!valid(unit.transform) || unit.team < -1 || unit.team >= 8 || unit.commander < -1 || unit.commander >= 8 ||
        !validText(unit.objectProfile) || !validText(unit.csvFile) || !validText(unit.warriorProfile) || !validText(unit.legacyBrain) ||
        unit.objectProfile.empty() || unit.warriorProfile.empty()) return reject("invalid_unit");
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
    const auto nl = newline(fit);
    std::vector<Edit> edits{{count->syntax, std::to_string(part)}};
    if (warriors) edits.push_back({find(*syntax, *warriors, "NumWarriors")->syntax, std::to_string(warrior)});
    std::string body = nl;
    if (!warriors) body += "[Warriors]" + nl + "ul NumWarriors = 1" + nl;
    body += "[Warrior" + std::to_string(warrior) + "]" + nl + "st Profile = \"" + unit.warriorProfile + "\"" + nl +
        "st Brain = \"" + unit.legacyBrain + "\"" + nl + "ul NumCells = 0" + nl + "ul NumStaticVars = 0" + nl;
    body += "[Part" + std::to_string(part) + "]" + nl + "ul ObjectNumber = " + std::to_string(unit.objectNumber) + nl +
        "ul ControlType = " + std::to_string(unit.controlType) + nl + "ul ControlDataType = " + std::to_string(unit.controlDataType) + nl +
        "st ObjectProfile = \"" + unit.objectProfile + "\"" + nl + "st CSVFile = \"" + unit.csvFile + "\"" + nl +
        "ul Pilot = " + std::to_string(warrior) + nl + "c TeamID = " + std::to_string(unit.team) + nl +
        "c CommanderID = " + std::to_string(unit.commander) + nl + "b PlayerPart = " + (unit.playerPart ? "TRUE" : "FALSE") + nl +
        "l Active = 1" + nl + "l Exists = 1" + nl + "c MyIcon = 0" + nl +
        "ul SquadNum = 0" + nl + "ul NumAlternatives = 0" + nl + "ul AlternativeStartIndex = 0" + nl +
        "l[15] IndicesOfAlternatives = -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1," + nl;
    for (std::size_t i = 0; i < 3; ++i) body += "f Position" + std::string(1, "XYZ"[i]) + " = " + real(unit.transform.position[i]) + nl;
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
    if (!valid(transform)) return reject("invalid_transform");
    if (entity->position == transform.position && entity->rotation == transform.rotation) return {};
    auto placements = document_.placements_;
    auto fit = document_.fitBytes();
    if (entity->packetRecord) putTransform(placements, 4 + *entity->packetRecord * 40, transform);
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
    auto ids = identities(document_);
    std::erase(ids, id);
    auto placements = document_.placements_;
    auto fit = document_.fitBytes();
    std::unordered_map<std::string, std::string> renamed;
    if (entity->packetRecord) {
        const auto offset = 4 + *entity->packetRecord * 40;
        placements.erase(placements.begin() + offset, placements.begin() + offset + 40);
        put(placements, 0, static_cast<std::uint32_t>((placements.size() - 4) / 40));
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
}
