#include "CommandHistory.h"
#include <algorithm>
#include <format>

namespace editor::document {
namespace {
struct Edit { Span span; std::string text; };
std::string defaults(const std::string& name, const std::string& nl)
{
    std::string text = "[" + name + "]" + nl;
    for (const auto& spec : objectiveSchema()) {
        Scalar value;
        if (spec.type == 0) value = spec.key == "ModelID Version" ? std::int64_t{2} : spec.integerDefault;
        else if (spec.type == 1) value = static_cast<double>(spec.integerDefault);
        else if (spec.type == 2) value = false;
        else value = std::string{};
        Field spelling; spelling.prefix = spec.prefix;
        text += std::string(spec.prefix) + " " + std::string(spec.key) + " = " + formatValue(spelling, Value{value}) + nl;
    }
    for (const auto* key : {"NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions"})
        text += "ul " + std::string(key) + " = 0" + nl;
    return text;
}
}

std::expected<NodeId, LoadError> CommandHistory::addObjective(std::uint32_t team)
{
    return editObjectives(ObjectiveEdit::add, team);
}
std::expected<NodeId, LoadError> CommandHistory::copyObjective(NodeId id)
{
    return editObjectives(ObjectiveEdit::copy, 0, id);
}
std::expected<void, LoadError> CommandHistory::deleteObjective(NodeId id)
{
    auto result = editObjectives(ObjectiveEdit::erase, 0, id);
    if (!result) return std::unexpected(result.error());
    return {};
}
std::expected<void, LoadError> CommandHistory::reorderObjective(NodeId id, std::uint32_t index)
{
    auto result = editObjectives(ObjectiveEdit::reorder, 0, id, index);
    if (!result) return std::unexpected(result.error());
    return {};
}
std::expected<NodeId, LoadError> CommandHistory::editObjectives(ObjectiveEdit edit, std::uint32_t team,
    NodeId id, std::uint32_t index)
{
    const auto reject = [&](std::string reason) -> std::expected<NodeId, LoadError> {
        if (log_) log_("[EDITOR_OBJECTIVE_REJECT v1] action=crud reason=" + reason + " rejected=1");
        return std::unexpected(LoadError{"objective", std::move(reason), 0});
    };
    const auto* selected = document_.objective(id);
    if (edit != ObjectiveEdit::add) {
        if (!selected) return reject("missing_objective");
        team = selected->team;
    }
    const auto* versionField = document_.find("Objectives Version", "Version");
    if (!versionField || versionField->prefix != "ul" || versionField->value.size() != 1 ||
        !std::holds_alternative<std::int64_t>(versionField->value[0])) return reject("invalid_objective_version");
    const auto version = std::get<std::int64_t>(versionField->value[0]);
    if (version != 2 && version != 3) return reject("invalid_objective_version");
    if (team >= (version == 2 ? 1U : 8U)) return reject("invalid_objective_team");
    std::vector<NodeId> order;
    for (const auto& item : document_.objectives()) if (item.team == team) order.push_back(item.section);
    const auto previousCount = order.size();
    if (edit == ObjectiveEdit::reorder) {
        if (index >= order.size()) return reject("invalid_objective_index");
        if (index == selected->exportedIndex) return id;
        std::erase(order, id); order.insert(order.begin() + index, id);
    } else if (edit == ObjectiveEdit::erase) std::erase(order, id);
    else {
        if (order.size() >= nodeLimit) return reject("objective_count_limit");
        index = edit == ObjectiveEdit::copy ? selected->exportedIndex + 1 : static_cast<std::uint32_t>(order.size());
        order.insert(order.begin() + index, NodeId{});
    }
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return reject(syntax.error().reason);
    const auto name = [&](std::size_t i) {
        return version == 2 ? std::format("Objective{}", i) : std::format("Team{}Objective{}", team, i);
    };
    const auto block = version == 2 ? std::string("Objectives") : std::format("Team{}Objectives", team);
    const auto* count = document_.find(block, "NumObjectives");
    if (!count || count->prefix != "ul" || count->value.size() != 1 ||
        !std::holds_alternative<std::int64_t>(count->value[0]) ||
        std::get<std::int64_t>(count->value[0]) != static_cast<std::int64_t>(previousCount))
        return reject("invalid_objective_count");
    // Reparsed spans include prior edits; IDs retain document identity.
    const auto countSection = std::ranges::find_if(syntax->sections, [&](const Section& section) {
        return !section.parent && identifier(section.name) == identifier(block);
    });
    if (countSection == syntax->sections.end()) return reject("invalid_objective_count");
    const auto freshCount = std::ranges::find_if(syntax->fields, [&](const Field& field) {
        return identifier(field.name) == "numobjectives" &&
            std::ranges::find(countSection->fields, field.id) != countSection->fields.end();
    });
    if (freshCount == syntax->fields.end()) return reject("invalid_objective_count");
    std::vector<Edit> edits;
    if (order.size() != previousCount) edits.push_back({freshCount->syntax, std::to_string(order.size())});
    std::unordered_map<std::string, std::string> renamed;
    std::string appended;
    const auto nl = fit.find("\r\n") == fit.npos ? "\n" : "\r\n";
    if (edit == ObjectiveEdit::add) appended = defaults(name(index), nl);
    const auto sourceName = selected ? identifier(document_.section(id)->name) : std::string{};
    std::unordered_map<std::string, const Objective*> owners;
    for (const auto& item : document_.objectives()) if (item.team == team)
        owners.emplace(identifier(document_.section(item.section)->name), &item);
    std::unordered_map<std::uint64_t, std::size_t> positions;
    for (std::size_t i = 0; i < order.size(); ++i) positions.emplace(order[i].value, i);
    const auto objectivePrefix = version == 2 ? std::string("objective") : std::format("team{}objective", team);
    for (std::size_t i = 0; i < document_.syntax().sections.size(); ++i) {
        const auto& old = document_.syntax().sections[i];
        if (old.parent) continue;
        const auto key = identifier(old.name);
        if (!key.starts_with(objectivePrefix)) continue;
        const auto end = key.find_first_not_of("0123456789", objectivePrefix.size());
        const auto found = owners.find(key.substr(0, end));
        if (found == owners.end()) continue;
        const auto* owner = found->second;
        const auto& fresh = syntax->sections[i];
        const auto base = identifier(document_.section(owner->section)->name);
        const auto suffix = old.name.substr(base.size());
        if (edit == ObjectiveEdit::copy && base == sourceName) {
            auto copy = fit.substr(fresh.syntax.begin, fresh.syntax.end - fresh.syntax.begin);
            copy.replace(fresh.nameSyntax.begin - fresh.syntax.begin,
                fresh.nameSyntax.end - fresh.nameSyntax.begin, name(index) + suffix);
            appended += copy;
            if (appended.back() != '\n') appended += nl;
        }
        const auto position = positions.find(owner->section.value);
        if (position == positions.end()) {
            edits.push_back({fresh.syntax, {}}); renamed.emplace(key, "");
        } else {
            const auto replacement = name(position->second) + suffix;
            if (replacement != old.name) {
                edits.push_back({fresh.nameSyntax, replacement});
                renamed.emplace(key, identifier(replacement));
            }
        }
    }
    if (!appended.empty()) {
        if (syntax->endMarker && fit[syntax->endMarker - 1] != '\n') appended.insert(0, nl);
        edits.push_back({{syntax->endMarker, syntax->endMarker}, std::move(appended)});
    }
    std::ranges::sort(edits, [](const Edit& a, const Edit& b) { return a.span.begin > b.span.begin; });
    for (const auto& change : edits) fit.replace(change.span.begin, change.span.end - change.span.begin, change.text);
    std::vector<EntityId> ids;
    for (const auto& entity : document_.entities()) ids.push_back(entity.id);
    auto candidate = document_.structuralCandidate(std::move(fit), document_.placements_, ids, renamed);
    if (!candidate) return reject(candidate.error().reason);
    NodeId result = id;
    if (edit == ObjectiveEdit::add || edit == ObjectiveEdit::copy)
        for (const auto& item : candidate->objectives) if (item.team == team && item.exportedIndex == index) result = item.section;
    const auto label = edit == ObjectiveEdit::add ? "Add objective" : edit == ObjectiveEdit::copy ? "Copy objective" :
        edit == ObjectiveEdit::erase ? "Delete objective" : "Reorder objective";
    auto committed = commit(StructuralCommand{document_.structuralState(), std::move(*candidate), label});
    if (!committed) return reject(committed.error().reason);
    return result;
}
}
