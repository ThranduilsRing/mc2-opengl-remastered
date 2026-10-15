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
    const bool open = ImGui::BeginCombo("Existing objective", selected ? label(*selected).c_str() : "No objectives");
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
    const char* lists[] = {"Success conditions", "Success actions", "Failure conditions", "Failure actions"};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto unsupported = std::ranges::count_if(selected->entries[i], [](const auto& entry) { return std::holds_alternative<std::monostate>(entry.species); });
        ImGui::Text("%s: %zu (%lld unsupported, preserved only)", lists[i], selected->entries[i].size(), static_cast<long long>(unsupported));
    }
    ImGui::TextWrapped("Condition/action payloads, team BooleanFlags and saved runtime state are preserved. Unsupported entries have no execution preview.");
    ImGui::PopID();
}
}
