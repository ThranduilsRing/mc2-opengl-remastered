#include "EntityPanels.h"
#include <misc/cpp/imgui_stdlib.h>
#include <format>
#include <algorithm>

namespace editor::shell {
using namespace document;
void EntityPanels::objectives()
{
    ImGui::PushID("objectives");
    const auto label = [](const Objective& item) { return std::format("Team {} / objective {}", item.team, item.exportedIndex + 1); };
    const auto* selected = document_.objective(objective_);
    if (!selected && !document_.objectives().empty()) {
        objective_ = document_.objectives().front().section;
        selected = document_.objective(objective_);
    }
    const bool open = ImGui::BeginCombo("Existing objective", selected ? label(*selected).c_str() : "No objectives");
    record("objective.select", open);
    if (open) {
        for (const auto& item : document_.objectives())
            if (choice("objective.select." + std::to_string(item.section.value), label(item).c_str(), item.section == objective_)) {
                objective_ = item.section; objectiveRevision_ = ~std::uint64_t{};
            }
        ImGui::EndCombo();
    }
    selected = document_.objective(objective_);
    if (!selected) { ImGui::TextUnformatted("This mission has no existing objectives."); ImGui::PopID(); return; }
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
    if (button("objective.apply", "Apply objective field")) report(history_.setObjectiveScalar(objective_, spec.key, objectiveDraft_));
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
