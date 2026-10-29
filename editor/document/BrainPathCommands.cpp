#include "CommandHistory.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>

namespace editor::document {
namespace {
auto reject(std::string reason) { return std::unexpected(LoadError{"brain/path", std::move(reason), 0}); }
std::expected<std::uint32_t, LoadError> integer(const MissionDocument& doc, NodeId section,
    std::string_view key, std::uint32_t fallback = 0)
{
    const auto* field = doc.find(section, key);
    if (!field) return fallback;
    if (field->value.size() != 1 || !std::holds_alternative<std::int64_t>(field->value[0])) return reject("invalid_" + std::string(key));
    const auto value = std::get<std::int64_t>(field->value[0]);
    if (value < 0 || value > UINT32_MAX) return reject("invalid_" + std::string(key));
    return static_cast<std::uint32_t>(value);
}
std::expected<double, LoadError> real(const MissionDocument& doc, NodeId section, std::string_view key)
{
    const auto* field = doc.find(section, key);
    if (!field || field->value.size() != 1 || !std::holds_alternative<double>(field->value[0])) return reject("invalid_" + std::string(key));
    return std::get<double>(field->value[0]);
}
bool finite(double value) { return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(); }
bool text(std::string_view value) {
    return value.size() <= textByteLimit && value.find_first_of("\"\r\n") == value.npos && value.find('\0') == value.npos;
}
struct Edit { Span span; std::string text; };
Span line(const std::string& fit, const Field& field) {
    const auto name = fit.rfind(field.name, field.syntax.begin);
    const auto start = fit.rfind('\n', name);
    const auto end = fit.find('\n', field.syntax.end);
    return {start == fit.npos ? 0 : start + 1, end == fit.npos ? fit.size() : end + 1};
}
std::string edited(std::string fit, std::vector<Edit> edits) {
    std::ranges::stable_sort(edits, [](const Edit& a, const Edit& b) {
        return a.span.begin == b.span.begin ? a.span.end > b.span.end : a.span.begin > b.span.begin;
    });
    for (const auto& edit : edits) fit.replace(edit.span.begin, edit.span.end - edit.span.begin, edit.text);
    return fit;
}
std::vector<EntityId> identities(const MissionDocument& doc) {
    std::vector<EntityId> result;
    for (const auto& entity : doc.entities()) result.push_back(entity.id);
    return result;
}
bool routeKey(std::string_view key) {
    const auto name = identifier(key);
    if (name == "ordertype" || name == "orderstance" || name == "waypointcount" || name == "editorpathid" || name == "editorpathname") return true;
    if (!name.starts_with("waypoint")) return false;
    auto suffix = std::string_view(name).substr(8);
    std::size_t n{};
    while (n < suffix.size() && suffix[n] >= '0' && suffix[n] <= '9') ++n;
    return n && (suffix.substr(n) == "x" || suffix.substr(n) == "y" || suffix.substr(n) == "z" || suffix.substr(n) == "id");
}
}

std::expected<std::optional<PatrolPath>, LoadError> MissionDocument::patrolPath(EntityId id) const
{
    const auto* unit = entity(id);
    if (!unit || !unit->section) return reject("missing_part");
    const auto section = *unit->section;
    if (!find(section, "OrderType") && !find(section, "OrderStance") && !find(section, "WaypointCount")) return std::nullopt;
    auto type = integer(*this, section, "OrderType"), stance = integer(*this, section, "OrderStance", 2), count = integer(*this, section, "WaypointCount");
    auto pathId = integer(*this, section, "EditorPathId", *unit->exportedPart);
    if (!type || !stance || !count || !pathId || *type > 2 || *stance > 5 || *count > 128) return reject("invalid_route");
    // Explicit ORDER_NONE is preserved on import but cannot be mistaken for an authored move.
    if (*type == 0) return reject("inactive_route");
    PatrolPath result{*pathId, {}, *type == 2, static_cast<std::int32_t>(*stance), {}};
    if (const auto* name = find(section, "EditorPathName")) {
        if (name->value.size() != 1 || !std::holds_alternative<std::string>(name->value[0])) return reject("invalid_path_name");
        result.name = std::get<std::string>(name->value[0]);
    }
    std::set<std::uint32_t> ids;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto prefix = "Waypoint" + std::to_string(i);
        auto pointId = integer(*this, section, prefix + "Id", i + 1);
        if (!pointId || !*pointId || !ids.insert(*pointId).second) return reject("invalid_point_id");
        PatrolPoint point{*pointId};
        for (int axis = 0; axis < 3; ++axis) {
            auto value = real(*this, section, prefix + "XYZ"[axis]);
            if (!value || !finite(*value)) return reject("invalid_point_position");
            point.position[axis] = *value;
        }
        result.points.push_back(point);
    }
    return result;
}

std::expected<BrainAssignment, LoadError> MissionDocument::brainAssignment(EntityId id) const
{
    const auto* unit = entity(id);
    if (!unit || !unit->warrior) return reject("missing_warrior_assignment");
    BrainAssignment result; result.entity = id;
    const auto warrior = *unit->warrior;
    if (const auto* module = find(warrior, "Brain")) {
        if (module->value.size() != 1 || !std::holds_alternative<std::string>(module->value[0])) return reject("invalid_module");
        result.module = std::get<std::string>(module->value[0]);
    }
    if (find(warrior, "NumStaticVars")) {
        auto count = integer(*this, warrior, "NumStaticVars");
        if (!count) return std::unexpected(count.error());
        result.staticVars = *count;
    }
    if (find(warrior, "NumCells")) {
        auto count = integer(*this, warrior, "NumCells");
        if (!count || *count > 1024) return reject("invalid_cell_count");
        result.cells.emplace();
        for (std::uint32_t i = 0; i < *count; ++i) {
            const auto name = section(warrior)->name + "Cell" + std::to_string(i);
            const auto* cell = find(name, "Cell");
            const Section* owner{};
            for (const auto& candidate : syntax_.sections) if (!candidate.parent && identifier(candidate.name) == identifier(name)) owner = &candidate;
            if (!cell || !owner) return reject("missing_cell");
            auto number = integer(*this, owner->id, "Cell"), type = integer(*this, owner->id, "MemType");
            const auto* value = find(owner->id, "Value");
            if (!number || !type || !value || value->value.size() != 1) return reject("invalid_cell");
            double numberValue{};
            if (const auto* n = std::get_if<double>(&value->value[0])) numberValue = *n;
            else if (const auto* n = std::get_if<std::int64_t>(&value->value[0])) numberValue = static_cast<double>(*n);
            else return reject("invalid_cell");
            result.cells->push_back({*number, *type, numberValue});
        }
    }
    if (unit->brain) {
        // Use current bytes: field edits can change span lengths since import.
        auto syntax = decodeFit(fitBytes());
        if (!syntax) return std::unexpected(syntax.error());
        const auto index = static_cast<std::size_t>(section(*unit->brain) - syntax_.sections.data());
        const auto span = syntax->sections[index].syntax;
        result.definition = syntax->originalBytes.substr(span.begin, span.end - span.begin);
    }
    return result;
}

std::expected<void, LoadError> CommandHistory::setPatrolPath(std::span<const EntityId> units, std::optional<PatrolPath> path)
{
    if (units.empty()) return reject("missing_units");
    if (path) {
        if (!path->id || !text(path->name) || path->stance < 0 || path->stance > 5 || path->points.empty() || path->points.size() > 128) return reject("invalid_path");
        std::set<std::uint32_t> ids;
        for (const auto& point : path->points)
            if (!point.id || !ids.insert(point.id).second || !std::ranges::all_of(point.position, finite)) return reject("invalid_point");
    }
    bool changed{};
    std::set<std::uint64_t> seen;
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return std::unexpected(syntax.error());
    std::vector<Edit> edits;
    const auto nl = fit.find("\r\n") == fit.npos ? "\n" : "\r\n";
    for (const auto id : units) {
        const auto* unit = document_.entity(id);
        if (!unit || !unit->section || !seen.insert(id.value).second) return reject("invalid_part_assignment");
        const auto old = document_.patrolPath(id);
        if (old && *old == path) continue;
        changed = true;
        const auto index = static_cast<std::size_t>(document_.section(*unit->section) - document_.syntax().sections.data());
        const auto& section = syntax->sections[index];
        for (const auto fieldId : section.fields) {
            const auto& field = syntax->fields[static_cast<std::size_t>(fieldId.value - 1)];
            if (routeKey(field.name)) edits.push_back({line(fit, field), {}});
        }
        if (!path) continue;
        std::string block = nl;
        const auto add = [&](std::string key, std::string value) { block += key + " = " + value + nl; };
        add("l OrderType", path->loop ? "2" : "1"); add("l OrderStance", std::to_string(path->stance));
        add("ul WaypointCount", std::to_string(path->points.size()));
        add("ul EditorPathId", std::to_string(path->id)); add("st EditorPathName", "\"" + path->name + "\"");
        for (std::size_t i = 0; i < path->points.size(); ++i) {
            const auto prefix = "Waypoint" + std::to_string(i);
            add("ul " + prefix + "Id", std::to_string(path->points[i].id));
            for (int axis = 0; axis < 3; ++axis) add("f " + prefix + "XYZ"[axis], std::format("{:.17g}", path->points[i].position[axis]));
        }
        edits.push_back({{section.syntax.end, section.syntax.end}, std::move(block)});
    }
    if (!changed) return {};
    return commitStructure(edited(std::move(fit), std::move(edits)), document_.placements_, identities(document_), "Patrol path");
}

std::expected<void, LoadError> CommandHistory::setBrainSidecar(std::size_t index, std::optional<std::string> source)
{
    if (index >= document_.brainSidecars_.size()) return reject("invalid_sidecar_index");
    if (source && (source->size() > fitByteLimit || source->find('\0') != source->npos)) return reject("invalid_sidecar_source");
    if (document_.brainSidecars_[index] == source) return {};
    auto after = document_.structuralState();
    after.brainSidecars[index] = std::move(source);
    return commit(StructuralCommand{document_.structuralState(), std::move(after), "Brain sidecar"});
}

std::expected<void, LoadError> CommandHistory::setBrainAssignment(const BrainAssignment& assignment)
{
    auto before = document_.brainAssignment(assignment.entity);
    if (!before) return std::unexpected(before.error());
    if (*before == assignment) return {};
    if (assignment.staticVars != before->staticVars) return reject("static_variables_require_named_definitions");
    if ((assignment.module && !text(*assignment.module)) ||
        (assignment.cells && assignment.cells->size() > 1024)) return reject("invalid_brain_assignment");
    std::set<std::uint32_t> numbers;
    if (before->cells != assignment.cells && assignment.cells) for (const auto& cell : *assignment.cells)
        if (cell.number >= 1024 || cell.type > 1 || !finite(cell.value) || !numbers.insert(cell.number).second ||
            (cell.type == 0 && (std::trunc(cell.value) != cell.value || cell.value < INT32_MIN || cell.value > INT32_MAX))) return reject("invalid_cell");
    if (assignment.definition) {
        auto definition = decodeFit("FITini\n[Warrior1]\n" + *assignment.definition + "\nFITend\n");
        if (!definition || definition->sections.size() < 2 || !definition->sections[1].parent ||
            identifier(definition->sections[1].name) != "brain" || !definition->sections[0].fields.empty()) return reject("invalid_brain_definition");
        for (std::size_t i = 2; i < definition->sections.size(); ++i)
            if (!definition->sections[i].parent || *definition->sections[i].parent == definition->sections[0].id) return reject("invalid_brain_definition");
    }
    const auto* unit = document_.entity(assignment.entity);
    // Shared Pilot links are intentional: editing a Warrior affects every linked Part.
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return std::unexpected(syntax.error());
    const auto index = static_cast<std::size_t>(document_.section(*unit->warrior) - document_.syntax().sections.data());
    const auto& warrior = syntax->sections[index];
    const auto nl = fit.find("\r\n") == fit.npos ? "\n" : "\r\n";
    std::vector<Edit> edits;
    std::string block = nl;
    const auto replace = [&](std::string_view name, bool changed, const std::optional<std::string>& value, std::string_view prefix) {
        if (!changed) return;
        for (const auto id : warrior.fields) {
            const auto& field = syntax->fields[static_cast<std::size_t>(id.value - 1)];
            if (identifier(field.name) == identifier(name)) edits.push_back({line(fit, field), {}});
        }
        if (value) block += std::string(prefix) + " " + std::string(name) + " = " + *value + nl;
    };
    replace("Brain", before->module != assignment.module, assignment.module ? std::optional("\"" + *assignment.module + "\"") : std::nullopt, "st");
    replace("NumStaticVars", before->staticVars != assignment.staticVars, assignment.staticVars ? std::optional(std::to_string(*assignment.staticVars)) : std::nullopt, "ul");
    replace("NumCells", before->cells != assignment.cells, assignment.cells ? std::optional(std::to_string(assignment.cells->size())) : std::nullopt, "l");
    if (before->definition != assignment.definition) {
        if (unit->brain) edits.push_back({syntax->sections[static_cast<std::size_t>(document_.section(*unit->brain) - document_.syntax().sections.data())].syntax, {}});
        if (assignment.definition) block += *assignment.definition + nl;
    }
    if (before->cells != assignment.cells) {
        if (before->cells) for (std::size_t i = 0; i < before->cells->size(); ++i) {
            const auto name = identifier(warrior.name + "Cell" + std::to_string(i));
            for (const auto& section : syntax->sections) if (!section.parent && identifier(section.name) == name) {
                if (!assignment.cells || i >= assignment.cells->size()) { edits.push_back({section.syntax, {}}); continue; }
                const auto& cell = (*assignment.cells)[i];
                const auto& old = (*before->cells)[i];
                for (const auto id : section.fields) {
                    const auto& field = syntax->fields[static_cast<std::size_t>(id.value - 1)];
                    const auto key = identifier(field.name);
                    if (key == "cell" && cell.number != old.number) edits.push_back({field.syntax, std::to_string(cell.number)});
                    if (key == "memtype" && cell.type != old.type) edits.push_back({field.syntax, std::to_string(cell.type)});
                    if (key == "value" && (cell.value != old.value || cell.type != old.type)) {
                        edits.push_back({field.syntax, std::format("{:.17g}", cell.value)});
                        const auto end = fit.rfind(field.name, field.syntax.begin);
                        auto begin = fit.find_last_of("\r\n", end);
                        begin = begin == fit.npos ? 0 : begin + 1;
                        while (begin < end && (fit[begin] == ' ' || fit[begin] == '\t')) ++begin;
                        edits.push_back({{begin, end}, cell.type == 0 ? "l " : "f "});
                    }
                }
            }
        }
        std::string cells;
        if (assignment.cells) for (std::size_t i = before->cells ? before->cells->size() : 0; i < assignment.cells->size(); ++i) {
            const auto name = identifier(warrior.name + "Cell" + std::to_string(i));
            if (std::ranges::any_of(syntax->sections, [&](const Section& section) { return !section.parent && identifier(section.name) == name; }))
                return reject("would_awaken_ignored_cell");
            const auto& cell = (*assignment.cells)[i];
            cells += "[" + warrior.name + "Cell" + std::to_string(i) + "]" + nl +
                "l Cell = " + std::to_string(cell.number) + nl + "l MemType = " + std::to_string(cell.type) + nl +
                (cell.type == 0 ? "l Value = " : "f Value = ") + std::format("{:.17g}", cell.value) + nl;
        }
        block += cells;
    }
    // Insert into the Warrior before its following cell sections.
    edits.push_back({{warrior.syntax.end, warrior.syntax.end}, std::move(block)});
    return commitStructure(edited(std::move(fit), std::move(edits)), document_.placements_, identities(document_), "Brain assignment");
}
}
