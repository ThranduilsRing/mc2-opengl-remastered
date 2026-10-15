#include "EntityPanels.h"
#include <misc/cpp/imgui_stdlib.h>
#include <format>
#include <algorithm>

namespace editor::shell {
using namespace document;
void EntityPanels::objectives()
{
    ImGui::PushID("objectives");
#ifdef MC2_EDITOR_SMOKE_HOST
    objectiveRows_.clear();
#endif
    const auto label = [](const Objective& item) { return std::format("Team {} / objective {}", item.team, item.exportedIndex + 1); };
    const auto version = std::get<std::int64_t>(document_.find("Objectives Version", "Version")->value[0]);
    if (objectiveTeam_ >= (version == 2 ? 1U : 8U)) objectiveTeam_ = 0;
    const bool teams = ImGui::BeginCombo("Objective team", std::to_string(objectiveTeam_).c_str());
    record("objective.team", teams);
    if (teams) {
        for (std::uint32_t i = 0; i < (version == 2 ? 1U : 8U); ++i)
            if (choice("objective.team." + std::to_string(i), std::to_string(i).c_str(), objectiveTeam_ == i)) {
                objectiveTeam_ = i; objective_ = {}; objectiveRevision_ = ~std::uint64_t{};
            }
        ImGui::EndCombo();
    }
    if (button("objective.add", "Add objective")) {
        const auto result = history_.addObjective(objectiveTeam_);
        if (result) { objective_ = *result; status_ = "Applied"; }
        else status_ = result.error().reason;
    }
    const auto* selected = document_.objective(objective_);
    if (!selected) {
        for (const auto& item : document_.objectives()) if (item.team == objectiveTeam_) {
            objective_ = item.section; selected = &item; objectiveRevision_ = ~std::uint64_t{}; break;
        }
    }
    const bool open = ImGui::BeginCombo("Existing objective", selected ? label(*selected).c_str() : "No objectives", ImGuiComboFlags_HeightLargest);
    record("objective.select", open);
    if (open) {
        for (const auto& item : document_.objectives()) if (item.team == objectiveTeam_)
            if (choice("objective.select." + std::to_string(item.section.value), label(item).c_str(), item.section == objective_)) {
                objective_ = item.section; objectiveRevision_ = ~std::uint64_t{};
            }
        ImGui::EndCombo();
    }
    selected = document_.objective(objective_);
    if (!selected) { ImGui::TextUnformatted("This team has no objectives. Add creates an empty objective."); ImGui::PopID(); return; }
    if (button("objective.copy", "Copy objective")) {
        const auto result = history_.copyObjective(objective_);
        if (result) { objective_ = *result; status_ = "Applied"; }
        else status_ = result.error().reason;
    }
    ImGui::SameLine();
    if (button("objective.delete", "Delete objective")) report(history_.deleteObjective(objective_));
    selected = document_.objective(objective_);
    if (!selected) { ImGui::PopID(); return; }
    const auto count = std::ranges::count(document_.objectives(), objectiveTeam_, &Objective::team);
    ImGui::BeginDisabled(selected->exportedIndex == 0);
    if (button("objective.up", "Move up")) report(history_.reorderObjective(objective_, selected->exportedIndex - 1));
    ImGui::EndDisabled(); ImGui::SameLine();
    selected = document_.objective(objective_);
    ImGui::BeginDisabled(selected->exportedIndex + 1 >= count);
    if (button("objective.down", "Move down")) report(history_.reorderObjective(objective_, selected->exportedIndex + 1));
    ImGui::EndDisabled();
    selected = document_.objective(objective_);
    ImGui::TextWrapped("Order changes prerequisite meaning. Existing prerequisite flags are preserved.");
    for (const auto id : document_.section(objective_)->fields) {
        const auto* field = document_.field(id);
        if (!field->warning.empty()) ImGui::TextWrapped("%s: %s (original line preserved)", field->name.c_str(), field->warning.data());
    }
    if (button("objective.entries", showObjectiveEntries_ ? "Show objective fields and references" : "Edit condition/action entries"))
        showObjectiveEntries_ = !showObjectiveEntries_;
    if (!showObjectiveEntries_) {
        const auto entityName = [&](const Entity& entity) {
            std::string name = entity.csvFile.value_or(entity.objectProfile.value_or("Object " + std::to_string(entity.objectNumber)));
            if (catalog_) for (const auto& item : catalog_->entries())
                if (item.id == entity.objectNumber) { name = item.displayName; break; }
            return name + (entity.exportedPart ? " / Part " + std::to_string(*entity.exportedPart) :
                " / Building " + std::to_string(*entity.packetRecord + 1));
        };
        for (const auto list : {0, 2}) for (const auto& entry : selected->entries[list]) {
            const auto& ref = entry.reference;
            const char* states[] = {"Resolved", "Missing", "Ambiguous", "RuntimeOnlyWid", "Unsupported"};
            const auto* target = ref.target ? document_.entity(*ref.target) : nullptr;
            const auto identity = document_.section(entry.section)->name;
            const auto text = identity + ": " + states[static_cast<int>(ref.status)] + " / " +
                (target ? entityName(*target) + " / " : "") + std::string(ref.reason);
            ImGui::TextWrapped("%s", text.c_str());
    #ifdef MC2_EDITOR_SMOKE_HOST
            objectiveRows_.push_back(identity + "\t" + states[static_cast<int>(ref.status)] + "\t" +
                (target ? entityName(*target) : "") + "\t" + std::string(ref.reason));
    #endif
            if (ref.status != ReferenceStatus::Missing && ref.status != ReferenceStatus::Ambiguous) continue;
            const auto key = "objective.resolve." + std::to_string(entry.section.value);
            const bool resolving = ImGui::BeginCombo(("Resolve this entry##" + key).c_str(), "Choose target");
            record(key, resolving);
            if (resolving) {
                for (const auto& entity : document_.entities()) {
                    if ((ref.kind == ReferenceKind::unit) != entity.exportedPart.has_value()) continue;
                    if (choice(key + "." + std::to_string(entity.id.value), entityName(entity).c_str(), false)) {
                        report(history_.resolveObjectiveEntry(entry.section, entity.id));
                        ImGui::EndCombo(); ImGui::PopID(); return;
                    }
                }
                ImGui::EndCombo();
            }
        }
        const auto specs = objectiveSchema();
        const bool fields = ImGui::BeginCombo("Authoring field", specs[objectiveField_].key.data(), ImGuiComboFlags_HeightLargest);
        record("objective.field", fields);
        if (fields) {
            for (std::size_t i = 0; i < specs.size(); ++i)
                if (choice("objective.field." + std::string(specs[i].key), specs[i].key.data(), objectiveField_ == i)) {
                    objectiveField_ = i; objectiveRevision_ = ~std::uint64_t{};
                }
            ImGui::EndCombo();
        }
        if (button("objective.next", "Next field")) {
            objectiveField_ = (objectiveField_ + 1) % specs.size(); objectiveRevision_ = ~std::uint64_t{};
        }
        const auto& spec = specs[objectiveField_];
        const auto* scalar = document_.objectiveScalar(objective_, spec.key);
        if (!scalar->warning.empty()) {
            ImGui::TextUnformatted("Original line:");
            const auto* field = document_.field(*scalar->source);
            const auto raw = document_.syntax().originalBytes.substr(field->syntax.begin, field->syntax.end - field->syntax.begin);
            ImGui::TextWrapped("%s %s = %s", field->prefix.c_str(), field->name.c_str(), raw.c_str());
            if (scalar->warning != "ignored by the game") { ImGui::TextUnformatted("Read-only"); ImGui::PopID(); return; }
            ImGui::TextUnformatted("Value the game uses (editable draft):");
        }
        if (objectiveRevision_ != document_.revision()) {
            objectiveDraft_ = document_.objectiveScalar(objective_, spec.key)->value;
            objectiveRevision_ = document_.revision();
        }
        bool changed{};
        if (auto* value = std::get_if<std::string>(&objectiveDraft_)) {
            if (spec.key == "Title" || spec.key == "Description")
                changed = ImGui::InputTextMultiline("Value", value, ImVec2(-1, 96), ImGuiInputTextFlags_CtrlEnterForNewLine);
            else changed = ImGui::InputText("Value", value);
        }
        else if (auto* value = std::get_if<bool>(&objectiveDraft_)) changed = ImGui::Checkbox("Value", value);
        else if (auto* value = std::get_if<std::int64_t>(&objectiveDraft_)) changed = ImGui::InputScalar("Value", ImGuiDataType_S64, value);
        else if (auto* value = std::get_if<double>(&objectiveDraft_)) changed = ImGui::InputDouble("Value", value, 0, 0, "%.17g");
        record("objective.value", changed || ImGui::IsItemActivated());
        if (button("objective.apply", scalar->warning.empty() ? "Apply objective field" : "author this field as the game reads it"))
            report(history_.setObjectiveScalar(objective_, spec.key, objectiveDraft_));
        selected = document_.objective(objective_);
        ImGui::TextWrapped("Text supports up to 2,000 characters. Ctrl+Enter adds a line break in title and description. Resource IDs refer to existing string tables. Flag IDs refer to existing scripts.");
        ImGui::Separator();
    } else {
        const char* lists[] = {"Success conditions", "Success actions", "Failure conditions", "Failure actions"};
        const char* semantics[] = {
            "All must succeed; a failed condition fails the objective. Empty success conditions can succeed immediately.",
            "Execute in list order when the objective succeeds.",
            "All succeeding overrides success and fails the objective.",
            "Execute in list order when the objective fails."
        };
        for (std::size_t i = 0; i < 4; ++i)
            ImGui::TextWrapped("%s: %s", lists[i], semantics[i]);
        for (std::size_t i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            const bool activated = ImGui::RadioButton(lists[i], objectiveList_ == i);
            record("objective.list." + std::to_string(i), activated);
            if (activated) objectiveList_ = i;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (i != objectiveList_) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s: %zu", lists[i], selected->entries[i].size());
            const auto key = "objective.entry." + std::to_string(i);
            if (i % 2 == 0) {
                const auto schema = conditionSchema();
                const bool speciesOpen = ImGui::BeginCombo("Condition species", conditionCreation_ == 0 ? "BooleanFlagIsSet" : schema[conditionCreation_ - 1].name.data(), ImGuiComboFlags_HeightLargest);
                record(key + ".species", speciesOpen);
                if (speciesOpen) {
                    if (choice(key + ".species.BooleanFlagIsSet", "BooleanFlagIsSet", conditionCreation_ == 0)) conditionCreation_ = 0;
                    for (std::size_t s = 0; s < schema.size(); ++s)
                        if (choice(key + ".species." + std::string(schema[s].name), schema[s].name.data(), conditionCreation_ == static_cast<int>(s + 1))) conditionCreation_ = static_cast<int>(s + 1);
                    ImGui::EndCombo();
                }
            }
            if (i % 2 == 0 && conditionCreation_ != 0 && conditionSchema()[conditionCreation_ - 1].reference) {
                const auto* target = conditionTarget_ ? document_.entity(*conditionTarget_) : nullptr;
                const auto label = target ? std::to_string(target->id.value) : "Choose target";
                const bool targetOpen = ImGui::BeginCombo("New condition target", label.c_str(), ImGuiComboFlags_HeightLargest);
                record(key + ".target", targetOpen);
                if (targetOpen) {
                    for (const auto& entity : document_.entities()) {
                        if (entity.exportedPart.has_value() == conditionSchema()[conditionCreation_ - 1].name.ends_with("Structure")) continue;
                        const auto name = std::to_string(entity.id.value);
                        if (choice(key + ".target." + name, name.c_str(), conditionTarget_ == entity.id)) conditionTarget_ = entity.id;
                    }
                    ImGui::EndCombo();
                }
            }
            auto& draft = actionCreation_[i / 2];
            if (i % 2 != 0 && button((key + ".creation").c_str(), "New action payload"))
                actionCreationOpen_[i / 2] = !actionCreationOpen_[i / 2];
            if (i % 2 != 0 && actionCreationOpen_[i / 2]) {
                const auto schema = actionSchema(true);
                auto spec = std::ranges::find(schema, draft.species, &ActionSpec::species);
                const bool speciesOpen = ImGui::BeginCombo("Action species", spec->name.data(), ImGuiComboFlags_HeightLargest);
                record(key + ".species", speciesOpen);
                if (speciesOpen) {
                    for (const auto& candidate : schema)
                        if (choice(key + ".species." + std::string(candidate.name), candidate.name.data(), draft.species == candidate.species)) {
                            draft = ActionDraft{candidate.species};
                        }
                    ImGui::EndCombo();
                    spec = std::ranges::find(schema, draft.species, &ActionSpec::species);
                }
                if (draft.payload.empty()) for (const auto& field : spec->fields) {
                    if (field.type == 3) draft.payload.emplace_back(std::string{});
                    else if (field.type == 2) draft.payload.emplace_back(false);
                    else draft.payload.emplace_back(std::int64_t{});
                }
                if (draft.species == ActionSpecies::flag && ImGui::Checkbox("New flag uses FlagIndex", &draft.flagIndex))
                    draft.payload[0] = draft.flagIndex ? Scalar{std::int64_t{}} : Scalar{std::string{}};
                for (std::size_t f = 0; f < spec->fields.size(); ++f) {
                    auto name = std::string(spec->fields[f].key);
                    if (draft.flagIndex && f == 0) name = "FlagIndex";
                    bool changed{};
                    if (auto* value = std::get_if<std::string>(&draft.payload[f])) changed = ImGui::InputTextMultiline(("New " + name).c_str(), value, ImVec2(-1, 55), ImGuiInputTextFlags_CtrlEnterForNewLine);
                    else if (auto* value = std::get_if<bool>(&draft.payload[f])) changed = ImGui::Checkbox(("New " + name).c_str(), value);
                    else changed = ImGui::InputScalar(("New " + name).c_str(), ImGuiDataType_S64, &std::get<std::int64_t>(draft.payload[f]));
                    record(key + ".new." + name, changed || ImGui::IsItemActivated());
                }
            }
            const auto valid = i % 2 == 0 || encodeActionDraft(draft).has_value();
            if (i % 2 != 0 && !actionCreationOpen_[i / 2]) ImGui::SameLine();
            ImGui::BeginDisabled(!valid);
            if (button((key + ".add").c_str(), i % 2 == 0 ? (conditionCreation_ == 0 ? "Add BooleanFlagIsSet" : "Add selected condition") : "Add selected action")) {
                const auto result = i % 2 != 0 ? history_.addObjectiveAction(objective_, i, draft) : conditionCreation_ != 0 ?
                    history_.addObjectiveCondition(objective_, i, conditionSchema()[conditionCreation_ - 1].species, conditionTarget_) :
                    history_.addObjectiveEntry(objective_, i);
                if (result) { objectiveEntry_ = *result; status_ = "Applied"; actionCreationOpen_[i / 2] = false; }
                else status_ = result.error().reason;
                ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID(); return;
            }
            ImGui::EndDisabled();
            for (std::size_t j = 0; j < selected->entries[i].size(); ++j) {
                const auto& entry = selected->entries[i][j];
                const auto row = document_.section(entry.section)->name;
                if (choice(key + ".select." + std::to_string(j), row.c_str(), objectiveEntry_ == entry.section)) {
                    objectiveEntry_ = entry.section; entryRevision_ = ~std::uint64_t{};
                }
                if (objectiveEntry_ != entry.section) continue;
                if (!entry.policy.empty()) ImGui::TextWrapped("%s", entry.policy.data());
                if (button((key + ".delete").c_str(), "Delete entry")) {
                    report(history_.deleteObjectiveEntry(entry.section));
                    ImGui::PopID(); ImGui::PopID(); return;
                }
                ImGui::BeginDisabled(!entry.policy.empty() || j == 0);
                if (button((key + ".up").c_str(), "Entry up")) {
                    report(history_.reorderObjectiveEntry(entry.section, static_cast<std::uint32_t>(j - 1)));
                    ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID(); return;
                }
                ImGui::EndDisabled(); ImGui::SameLine();
                ImGui::BeginDisabled(!entry.policy.empty() || j + 1 == selected->entries[i].size());
                if (button((key + ".down").c_str(), "Entry down")) {
                    report(history_.reorderObjectiveEntry(entry.section, static_cast<std::uint32_t>(j + 1)));
                    ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID(); return;
                }
                ImGui::EndDisabled();
                if (entry.flag) {
                    if (entryRevision_ != document_.revision()) {
                        flagDraft_ = entry.flag->id.value_or(""); flagUseIndex_ = entry.flag->indexFallback;
                        flagIndexDraft_ = flagUseIndex_ ? std::stoll(flagDraft_) : 0;
                        flagValueDraft_ = entry.flag->value; entryRevision_ = document_.revision();
                    }
                    ImGui::TextWrapped("Reader ID: %s; Value: %s", entry.flag->id ? entry.flag->id->c_str() : "<unreadable; skipped>", entry.flag->value ? "true" : "false");
                    ImGui::TextWrapped("%s", entry.flag->warning.c_str());
                    ImGui::BeginDisabled(!entry.policy.empty());
                    record(key + ".index-mode", ImGui::Checkbox("Use FlagIndex fallback", &flagUseIndex_));
                    if (flagUseIndex_) record(key + ".index", ImGui::InputScalar("FlagIndex", ImGuiDataType_S64, &flagIndexDraft_));
                    else record(key + ".id", ImGui::InputTextMultiline("FlagID", &flagDraft_, ImVec2(-1, 55), ImGuiInputTextFlags_CtrlEnterForNewLine));
                    record(key + ".value", ImGui::Checkbox("Flag Value", &flagValueDraft_));
                    if (button((key + ".apply").c_str(), "Apply flag payload")) {
                        report(history_.setObjectiveFlag(entry.section, flagUseIndex_ ? std::nullopt : std::optional{flagDraft_}, flagIndexDraft_, flagValueDraft_));
                        ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID(); return;
                    }
                    ImGui::EndDisabled();
                } else if (entry.policy.empty()) {
                    const auto fields = objectivePayloadSchema(entry);
                    if (!fields.empty()) {
                        if (entryRevision_ != document_.revision()) {
                            for (std::size_t f = 0; f < fields.size(); ++f)
                                payloadDraft_[f] = entry.payload[f];
                            entryRevision_ = document_.revision();
                        }
                        for (std::size_t f = 0; f < fields.size(); ++f) {
                            const auto& field = fields[f];
                            const auto* source = document_.find(entry.section, field.key);
                            if (!source->warning.empty()) ImGui::TextWrapped("%s: %s; authored bytes preserved", field.key.data(), source->warning.data());
                            if (field.type == 0) record(key + "." + std::string(field.key), ImGui::InputScalar(field.key.data(), ImGuiDataType_S64, &std::get<std::int64_t>(payloadDraft_[f])));
                            else if (field.type == 1) record(key + "." + std::string(field.key), ImGui::InputScalar(field.key.data(), ImGuiDataType_Double, &std::get<double>(payloadDraft_[f]), nullptr, nullptr, "%.9g"));
                            else record(key + "." + std::string(field.key), ImGui::InputTextMultiline(field.key.data(), &std::get<std::string>(payloadDraft_[f]), ImVec2(-1, 96), ImGuiInputTextFlags_CtrlEnterForNewLine));
                            if (button((key + ".apply." + std::string(field.key)).c_str(), ("Apply " + std::string(field.key)).c_str())) {
                                report(history_.setObjectivePayload(entry.section, field.key, payloadDraft_[f]));
                                ImGui::PopID(); ImGui::PopID(); return;
                            }
                        }
                    } else if (entry.reference.kind != ReferenceKind::none) ImGui::TextUnformatted("Use the objective reference rows to resolve the target.");
                    else ImGui::TextUnformatted("Payload authoring for this species is deferred.");
                }
                if (ImGui::TreeNode("Authored bytes (preserved declarations included)")) {
                    const auto span = document_.section(entry.section)->syntax;
                    const auto& source = document_.syntax().originalBytes;
                    ImGui::TextUnformatted(source.data() + span.begin, source.data() + span.end);
                    ImGui::TreePop();
                }
            }
            if (ImGui::TreeNode("Ignored entry blocks and tails")) {
                constexpr std::array suffixes = {"Condition", "Action", "FailureCondition", "FailureAction"};
                const auto base = identifier(document_.section(objective_)->name + suffixes[i]);
                for (const auto& section : document_.syntax().sections) {
                    if (section.parent) continue;
                    auto name = identifier(section.name);
                    name.erase(0, name.find_first_not_of(" \t"));
                    name.erase(name.find_last_not_of(" \t") + 1);
                    if (version == 2 && name.starts_with("team0")) name.erase(0, 5);
                    if (!name.starts_with(base) || name.size() == base.size() ||
                        !std::ranges::all_of(std::string_view(name).substr(base.size()), [](char c) { return c >= '0' && c <= '9'; }) ||
                        std::ranges::find(selected->entries[i], section.id, &ObjectiveEntry::section) != selected->entries[i].end()) continue;
                    ImGui::TextWrapped("%s: ignored by the game's block name/count; preserved byte-exact", section.name.c_str());
                    const auto& source = document_.syntax().originalBytes;
                    ImGui::TextUnformatted(source.data() + section.syntax.begin, source.data() + section.syntax.end);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TextWrapped("Team BooleanFlags and saved runtime state are preserved. Quarantined or skipped entries can be deleted explicitly; individual editing and reordering are refused.");
    }
    ImGui::PopID();
}
}
