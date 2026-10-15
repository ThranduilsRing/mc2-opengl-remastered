#include "CommandHistory.h"
#include <algorithm>
#include <format>
#include <cmath>

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

LoadError CommandHistory::rejectObjective(std::string reason)
{
    if (log_) log_("[EDITOR_OBJECTIVE_REJECT v1] reason=" + reason + " rejected=1");
    return {"objective", std::move(reason), 0};
}

std::expected<void, LoadError> CommandHistory::commitObjectiveFit(std::string fit, std::string label,
    const std::unordered_map<std::string, std::string>& renamed)
{
    std::vector<EntityId> ids;
    for (const auto& entity : document_.entities()) ids.push_back(entity.id);
    auto candidate = document_.structuralCandidate(std::move(fit), document_.placements_, ids, renamed);
    if (!candidate) return std::unexpected(rejectObjective(candidate.error().reason));
    auto result = commit(StructuralCommand{document_.structuralState(), std::move(*candidate), std::move(label)});
    if (!result) return std::unexpected(rejectObjective(result.error().reason));
    return {};
}

std::expected<NodeId, LoadError> CommandHistory::addObjectiveEntry(NodeId objective, std::size_t list)
{
    return editObjectiveEntries(ObjectiveEdit::add, objective, list);
}
std::expected<NodeId, LoadError> CommandHistory::addObjectiveCondition(NodeId objective, std::size_t list, ConditionSpecies species,
    std::optional<EntityId> target)
{
    if ((list != 0 && list != 2) || std::ranges::find(conditionSchema(), species, &ConditionSpec::species) == conditionSchema().end())
        return std::unexpected(rejectObjective("unsupported_condition_creation"));
    const auto spec = std::ranges::find(conditionSchema(), species, &ConditionSpec::species);
    if (spec->reference && !target) return std::unexpected(rejectObjective("specific_condition_requires_target"));
    if (!spec->reference) target.reset();
    return editObjectiveEntries(ObjectiveEdit::add, objective, list, {}, 0, species, target);
}

std::expected<void, LoadError> CommandHistory::setObjectivePayload(NodeId id, std::string_view key, Scalar value)
{
    const auto reject = [&](std::string reason) -> std::expected<void, LoadError> {
        return std::unexpected(rejectObjective(std::move(reason)));
    };
    const ObjectiveEntry* selected{};
    for (const auto& objective : document_.objectives()) for (const auto list : {0U, 1U, 2U, 3U})
        for (const auto& entry : objective.entries[list]) if (entry.section == id) selected = &entry;
    if (!selected) return reject("unsupported_condition_payload");
    if (!selected->policy.empty()) return reject(std::string(selected->policy));
    const auto schema = objectivePayloadSchema(*selected);
    const auto fieldSpec = std::ranges::find(schema, key, &ObjectiveFieldSpec::key);
    if (fieldSpec == schema.end()) return reject("unsupported_payload_field");
    const auto* old = document_.find(id, key);
    if (!old || old->array || old->opaque || old->prefix != fieldSpec->prefix)
        return reject("ignored_payload_declaration_would_be_activated");
    if (old->warning.starts_with("outside signed")) return reject(std::string(old->warning));
    if (auto* real = std::get_if<double>(&value)) *real = static_cast<double>(static_cast<float>(*real));
    if (auto* text = std::get_if<std::string>(&value))
        for (auto at = text->find("\r\n"); at != text->npos; at = text->find("\r\n", at)) text->erase(at, 1);
    if (auto valid = validateObjectiveScalar(*fieldSpec, value); !valid) return reject(valid.error().reason);
    const auto* requestedReal = std::get_if<double>(&value);
    if (selected->payload[static_cast<std::size_t>(fieldSpec - schema.begin())] == value && (!requestedReal ||
        std::signbit(*requestedReal) == std::signbit(std::get<double>(old->value[0])))) return {};
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return reject(syntax.error().reason);
    const auto index = static_cast<std::size_t>(old - document_.syntax().fields.data());
    const auto& fresh = syntax->fields[index];
    auto encoded = value;
    if (auto* text = std::get_if<std::string>(&encoded); text && text->find('\n') != text->npos) {
        std::string wire = "\r\n";
        for (const char c : *text) { if (c == '\n') wire += '\r'; wire += c; }
        *text = std::move(wire);
    }
    const auto wire = fieldSpec->type == 1 ? std::format("{:.9g}", std::get<double>(value)) : formatValue(*old, Value{encoded});
    fit.replace(fresh.syntax.begin, fresh.syntax.end - fresh.syntax.begin, wire);
    auto check = decodeFit(fit);
    if (!check) return reject(check.error().reason);
    if (check->fields[index].warning == "reader_string_buffer_limit") return reject("reader_string_buffer_limit");
    auto decoded = check->fields[index].value[0];
    if (decoded != value) return reject("reader_conversion_changes_value");
    return commitObjectiveFit(std::move(fit), "Edit objective payload");
}
std::expected<void, LoadError> CommandHistory::deleteObjectiveEntry(NodeId entry)
{
    auto result = editObjectiveEntries(ObjectiveEdit::erase, {}, 0, entry);
    if (!result) return std::unexpected(result.error());
    return {};
}
std::expected<void, LoadError> CommandHistory::reorderObjectiveEntry(NodeId entry, std::uint32_t index)
{
    auto result = editObjectiveEntries(ObjectiveEdit::reorder, {}, 0, entry, index);
    if (!result) return std::unexpected(result.error());
    return {};
}

std::expected<NodeId, LoadError> CommandHistory::editObjectiveEntries(ObjectiveEdit edit, NodeId owner,
    std::size_t list, NodeId id, std::uint32_t index, std::optional<ConditionSpecies> species, std::optional<EntityId> target)
{
    const auto reject = [&](std::string reason) -> std::expected<NodeId, LoadError> {
        return std::unexpected(rejectObjective(std::move(reason)));
    };
    if (edit != ObjectiveEdit::add) {
        for (const auto& objective : document_.objectives()) for (std::size_t i = 0; i < 4; ++i)
            for (const auto& entry : objective.entries[i]) if (entry.section == id) { owner = objective.section; list = i; }
    }
    const auto* objective = document_.objective(owner);
    if (!objective || list >= 4) return reject("missing_objective_or_list");
    const auto& entries = objective->entries[list];
    const auto found = std::ranges::find(entries, id, &ObjectiveEntry::section);
    if (edit != ObjectiveEdit::add && found == entries.end()) return reject("missing_entry");
    std::vector<NodeId> order;
    for (const auto& entry : entries) order.push_back(entry.section);
    if (edit == ObjectiveEdit::add) {
        if (entries.size() >= nodeLimit) return reject("entry_count_limit");
        index = static_cast<std::uint32_t>(order.size()); order.push_back({});
    } else if (edit == ObjectiveEdit::erase) std::erase(order, id);
    else {
        if (!found->policy.empty()) return reject(std::string(found->policy));
        if (index >= order.size()) return reject("invalid_entry_index");
        const auto old = static_cast<std::size_t>(found - entries.begin());
        if (index == old) return id;
        for (auto i = std::min<std::size_t>(old, index); i <= std::max<std::size_t>(old, index); ++i)
            if (!entries[i].policy.empty()) return reject("reorder_crosses_preserved_entry");
        std::erase(order, id); order.insert(order.begin() + index, id);
    }
    constexpr std::array suffixes = {"Condition", "Action", "FailureCondition", "FailureAction"};
    constexpr std::array counts = {"NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions"};
    const auto version = std::get<std::int64_t>(document_.find("Objectives Version", "Version")->value[0]);
    const auto base = (version == 2 && list != 0 ? "Team0" : "") + document_.section(owner)->name + suffixes[list];
    auto fit = document_.fitBytes();
    auto fresh = decodeFit(fit);
    if (!fresh) return reject(fresh.error().reason);
    const auto freshSection = [&](NodeId node) -> const Section& {
        return fresh->sections[static_cast<std::size_t>(document_.section(node) - document_.syntax().sections.data())];
    };
    std::vector<Edit> edits;
    const auto nl = fit.find("\r\n") == fit.npos ? "\n" : "\r\n";
    const auto* count = document_.find(owner, counts[list]);
    if (count && (count->prefix != "ul" || count->opaque)) return reject("ignored_list_count_preserved");
    if (count) {
        const auto at = static_cast<std::size_t>(count - document_.syntax().fields.data());
        edits.push_back({fresh->fields[at].syntax, std::to_string(order.size())});
    } else {
        const auto end = freshSection(owner).syntax.end;
        edits.push_back({{end, end}, std::string(nl) + "ul " + counts[list] + " = " + std::to_string(order.size()) + nl});
    }
    std::unordered_map<std::string, std::string> renamed;
    for (const auto& entry : entries) {
        const auto& section = freshSection(entry.section);
        const auto position = std::ranges::find(order, entry.section);
        if (position == order.end()) {
            edits.push_back({section.syntax, {}}); renamed.emplace(section.name, "");
        } else {
            const auto name = base + std::to_string(position - order.begin());
            if (name != section.name) {
                edits.push_back({section.nameSyntax, name}); renamed.emplace(section.name, name);
            }
        }
    }
    if (edit == ObjectiveEdit::add) {
        const auto name = base + std::to_string(index);
        for (const auto& section : fresh->sections) if (!section.parent && identifier(section.name) == identifier(name))
            return reject("entry_add_would_awaken_ignored_tail");
        auto text = (fresh->endMarker && fit[fresh->endMarker - 1] != '\n' ? std::string(nl) : std::string{}) + "[" + name + "]" + nl + "st " + suffixes[list] +
            "SpeciesString = \"" + (list % 2 == 0 ? "BooleanFlagIsSet" : "SetBooleanFlag") + "\"" + nl +
            "st FlagID = \"\"" + nl + "b Value = TRUE" + nl;
        if (species) {
            const auto spec = std::ranges::find(conditionSchema(), *species, &ConditionSpec::species);
            text = "[" + name + "]" + nl + "st " + suffixes[list] + "SpeciesString = \"" + std::string(spec->name) + "\"" + nl;
            if (fresh->endMarker && fit[fresh->endMarker - 1] != '\n') text.insert(0, nl);
            const auto* entity = target ? document_.entity(*target) : nullptr;
            if (spec->reference && !entity) return reject("specific_condition_requires_target");
            for (const auto& field : spec->fields) {
                const auto wire = spec->reference ? std::format("{:.9g}", static_cast<float>(entity->position[field.key == "PositionX" ? 0 : 1])) : "0";
                text += std::string(field.prefix) + " " + std::string(field.key) + " = " + wire + nl;
            }
        }
        edits.insert(edits.begin(), {{fresh->endMarker, fresh->endMarker}, text});
    }
    std::ranges::stable_sort(edits, [](const Edit& a, const Edit& b) { return a.span.begin > b.span.begin; });
    for (const auto& change : edits) fit.replace(change.span.begin, change.span.end - change.span.begin, change.text);
    if (species && target) {
        std::vector<EntityId> ids;
        for (const auto& entity : document_.entities()) ids.push_back(entity.id);
        auto candidate = document_.structuralCandidate(std::move(fit), document_.placements_, ids, renamed);
        if (!candidate) return reject(candidate.error().reason);
        const auto candidateOwner = std::ranges::find(candidate->objectives, owner, &Objective::section);
        const auto& entry = candidateOwner->entries[list][index];
        if (entry.reference.status != ReferenceStatus::Resolved || entry.reference.target != target)
            return reject("target_not_unique_or_ineligible");
        const auto added = entry.section;
        auto result = commit(StructuralCommand{document_.structuralState(), std::move(*candidate), "Add targeted condition"});
        if (!result) return reject(result.error().reason);
        return added;
    }
    auto result = commitObjectiveFit(std::move(fit), "Edit objective entries", renamed);
    if (!result) return std::unexpected(result.error());
    return edit == ObjectiveEdit::add ? document_.objective(owner)->entries[list][index].section : id;
}

std::expected<void, LoadError> CommandHistory::setObjectiveFlag(NodeId id, std::optional<std::string> flagID,
    std::int64_t index, bool value)
{
    const auto reject = [&](std::string reason) -> std::expected<void, LoadError> {
        return std::unexpected(rejectObjective(std::move(reason)));
    };
    const ObjectiveEntry* selected{};
    for (const auto& objective : document_.objectives()) for (const auto& list : objective.entries)
        for (const auto& entry : list) if (entry.section == id) selected = &entry;
    if (!selected) return reject("missing_entry");
    if (!selected->policy.empty()) return reject(std::string(selected->policy));
    if (!selected->flag) return reject("payload_authoring_deferred_for_species");
    if (flagID) {
        for (auto at = flagID->find("\r\n"); at != flagID->npos; at = flagID->find("\r\n", at)) flagID->erase(at, 1);
        const ObjectiveFieldSpec spec{"FlagID", "st", 3};
        if (auto valid = validateObjectiveScalar(spec, Scalar{*flagID}); !valid) return reject(valid.error().reason);
    } else if (index < -2147483648LL || index > 2147483647LL) return reject("FlagIndex_outside_signed_32_bit");
    if (selected->flag->id == (flagID ? flagID : std::optional{std::to_string(index)}) &&
        selected->flag->indexFallback == !flagID.has_value() && selected->flag->value == value) return {};
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return reject(syntax.error().reason);
    const auto& section = syntax->sections[static_cast<std::size_t>(document_.section(id) - document_.syntax().sections.data())];
    std::vector<Edit> edits;
    const auto nl = fit.find("\r\n") == fit.npos ? "\n" : "\r\n";
    std::string added;
    const auto set = [&](std::string_view key, std::string_view prefix, std::string wire) {
        const auto* old = document_.find(id, key);
        if (old) {
            if (old->prefix != prefix || old->array) return false;
            const auto at = static_cast<std::size_t>(old - document_.syntax().fields.data());
            edits.push_back({syntax->fields[at].syntax, std::move(wire)});
        } else added += std::string(prefix) + " " + std::string(key) + " = " + wire + nl;
        return true;
    };
    if (flagID) {
        std::string text = *flagID;
        if (text.find('\n') != text.npos) {
            std::string wire = "\r\n";
            for (const auto c : text) { if (c == '\n') wire += '\r'; wire += c; }
            text = std::move(wire);
        }
        if (!set("FlagID", "st", "\"" + text + "\"")) return reject("preserved_FlagID_prefix_or_array");
    } else {
        if (const auto* old = document_.find(id, "FlagID")) {
            if (selected->flag->idSource == old->id && !selected->flag->indexFallback)
                return reject("readable_FlagID_would_override_FlagIndex");
        }
        if (!set("FlagIndex", "ul", std::to_string(index))) return reject("preserved_FlagIndex_prefix_or_array");
    }
    if (value != selected->flag->value && !set("Value", "b", value ? "TRUE" : "FALSE"))
        return reject("preserved_Value_prefix_or_array");
    if (!added.empty()) edits.push_back({{section.syntax.end, section.syntax.end}, std::string(nl) + added});
    std::ranges::sort(edits, [](const Edit& a, const Edit& b) { return a.span.begin > b.span.begin; });
    for (const auto& change : edits) fit.replace(change.span.begin, change.span.end - change.span.begin, change.text);
    if (fit == document_.fitBytes()) return {};
    return commitObjectiveFit(std::move(fit), "Set objective boolean flag");
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
std::expected<void, LoadError> CommandHistory::resolveObjectiveEntry(NodeId id, EntityId target)
{
    const auto reject = [&](std::string reason) -> std::expected<void, LoadError> {
        return std::unexpected(rejectObjective(std::move(reason)));
    };
    const ObjectiveEntry* selected{};
    for (const auto& objective : document_.objectives()) for (const auto& list : objective.entries)
        for (const auto& entry : list) if (entry.section == id) selected = &entry;
    if (!selected || (selected->reference.status != ReferenceStatus::Missing &&
        selected->reference.status != ReferenceStatus::Ambiguous)) return reject("entry_not_resolvable");
    const auto* entity = document_.entity(target);
    if (!entity) return reject("missing_entity");
    auto fit = document_.fitBytes();
    auto syntax = decodeFit(fit);
    if (!syntax) return reject(syntax.error().reason);
    const auto index = static_cast<std::size_t>(document_.section(id) - document_.syntax().sections.data());
    std::vector<Edit> edits;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto key = axis == 0 ? "PositionX" : "PositionY";
        const auto* old = document_.find(id, key);
        if (!old) return reject("missing_coordinate");
        if (static_cast<float>(std::get<double>(old->value[0])) == static_cast<float>(entity->position[axis])) continue;
        const auto fresh = std::ranges::find_if(syntax->fields, [&](const Field& field) {
            return identifier(field.name) == identifier(key) &&
                std::ranges::find(syntax->sections[index].fields, field.id) != syntax->sections[index].fields.end();
        });
        if (fresh == syntax->fields.end()) return reject("missing_coordinate");
        edits.push_back({fresh->syntax, std::format("{:.9g}", static_cast<float>(entity->position[axis]))});
    }
    std::ranges::sort(edits, [](const Edit& a, const Edit& b) { return a.span.begin > b.span.begin; });
    for (const auto& edit : edits) fit.replace(edit.span.begin, edit.span.end - edit.span.begin, edit.text);
    std::vector<EntityId> ids;
    for (const auto& item : document_.entities()) ids.push_back(item.id);
    auto candidate = document_.structuralCandidate(std::move(fit), document_.placements_, ids, {}, id, target);
    if (!candidate) return reject(candidate.error().reason);
    auto result = commit(StructuralCommand{document_.structuralState(), std::move(*candidate), "Resolve objective entry"});
    if (!result) return reject(result.error().reason);
    return {};
}
std::expected<NodeId, LoadError> CommandHistory::editObjectives(ObjectiveEdit edit, std::uint32_t team,
    NodeId id, std::uint32_t index)
{
    const auto reject = [&](std::string reason) -> std::expected<NodeId, LoadError> {
        return std::unexpected(rejectObjective(std::move(reason)));
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
        const bool teamEntry = version == 2 && key.starts_with("team0objective");
        const auto ownerKey = teamEntry ? key.substr(5) : key;
        if (!ownerKey.starts_with(objectivePrefix)) continue;
        const auto end = ownerKey.find_first_not_of("0123456789", objectivePrefix.size());
        const auto found = owners.find(ownerKey.substr(0, end));
        if (found == owners.end()) continue;
        const auto* owner = found->second;
        const auto& fresh = syntax->sections[i];
        const auto base = identifier(document_.section(owner->section)->name);
        const auto suffix = old.name.substr(base.size() + (teamEntry ? 5 : 0));
        const auto entryPrefix = teamEntry ? "Team0" : "";
        bool live = old.id == owner->section;
        for (const auto& list : owner->entries) for (const auto& entry : list) live |= entry.section == old.id;
        if (!live) {
            if (edit == ObjectiveEdit::copy && base == sourceName) {
                auto copy = fit.substr(fresh.syntax.begin, fresh.syntax.end - fresh.syntax.begin);
                auto spelling = old.name;
                const auto digits = base.size() + (teamEntry ? 5 : 0) - std::to_string(owner->exportedIndex).size();
                spelling.replace(digits, std::to_string(owner->exportedIndex).size(), std::to_string(index));
                copy.replace(fresh.nameSyntax.begin - fresh.syntax.begin, fresh.nameSyntax.end - fresh.nameSyntax.begin, spelling);
                appended += copy;
                if (appended.back() != '\n') appended += nl;
            }
            const auto position = positions.find(owner->section.value);
            if (position == positions.end() || position->second != owner->exportedIndex)
                return reject("objective_rename_would_change_ignored_block");
            continue;
        }
        if (edit == ObjectiveEdit::copy && base == sourceName) {
            auto copy = fit.substr(fresh.syntax.begin, fresh.syntax.end - fresh.syntax.begin);
            copy.replace(fresh.nameSyntax.begin - fresh.syntax.begin,
                fresh.nameSyntax.end - fresh.nameSyntax.begin, entryPrefix + name(index) + suffix);
            appended += copy;
            if (appended.back() != '\n') appended += nl;
        }
        const auto position = positions.find(owner->section.value);
        if (position == positions.end()) {
            edits.push_back({fresh.syntax, {}}); renamed.emplace(old.name, "");
        } else {
            const auto replacement = entryPrefix + name(position->second) + suffix;
            if (replacement != old.name) {
                edits.push_back({fresh.nameSyntax, replacement});
                renamed.emplace(old.name, replacement);
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
        for (auto& item : candidate->objectives) if (item.team == team && item.exportedIndex == index) {
            result = item.section;
        }
    const auto label = edit == ObjectiveEdit::add ? "Add objective" : edit == ObjectiveEdit::copy ? "Copy objective" :
        edit == ObjectiveEdit::erase ? "Delete objective" : "Reorder objective";
    auto committed = commit(StructuralCommand{document_.structuralState(), std::move(*candidate), label});
    if (!committed) return reject(committed.error().reason);
    return result;
}
}
