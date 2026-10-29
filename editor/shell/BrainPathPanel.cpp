#include "EntityPanels.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>

namespace editor::shell {
void EntityPanels::brainPaths()
{
    using namespace document;
    if (!brainPathError_.empty()) ImGui::TextWrapped("%s", brainPathError_.c_str());
    if (ImGui::TreeNode("Mission brain sidecars")) {
        ImGui::TextWrapped("Mission-wide sources. AI uses [Brain] unitRef/mode and [Tactics]; specials use TechSpecial bodies.");
        constexpr std::array labels{"AI source (_ai.fit)", "Specials source (_specials.fit)"};
        for (std::size_t i = 0; i < labels.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::InputTextMultiline(labels[i], &brainSidecarDrafts_[i], ImVec2(-1, 160));
            if (ImGui::Button("Apply source")) report(history_.setBrainSidecar(i, brainSidecarDrafts_[i]));
            ImGui::SameLine();
            if (ImGui::Button("Remove sidecar")) report(history_.setBrainSidecar(i, std::nullopt));
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Patrol path")) {
        ImGui::InputText("Path name", &pathDraft_.name);
        ImGui::Checkbox("Loop", &pathDraft_.loop);
        ImGui::SliderInt("Stance", &pathDraft_.stance, 0, 5);
        for (std::size_t i = 0; i < pathDraft_.points.size(); ++i) {
            ImGui::PushID(static_cast<int>(pathDraft_.points[i].id));
            ImGui::Text("Point %u", pathDraft_.points[i].id);
            ImGui::InputScalarN("XYZ", ImGuiDataType_Double, pathDraft_.points[i].position.data(), 3);
            if (ImGui::Button("Up") && i) std::swap(pathDraft_.points[i], pathDraft_.points[i - 1]);
            ImGui::SameLine();
            if (ImGui::Button("Down") && i + 1 < pathDraft_.points.size()) std::swap(pathDraft_.points[i], pathDraft_.points[i + 1]);
            ImGui::SameLine();
            const bool remove = ImGui::Button("Delete point");
            ImGui::PopID();
            if (remove) { pathDraft_.points.erase(pathDraft_.points.begin() + i); break; }
        }
        if (ImGui::Button("Add point") && pathDraft_.points.size() < 128) {
            std::uint32_t id = 1;
            for (const auto& point : pathDraft_.points) id = std::max(id, point.id + 1);
            const auto* unit = document_.entity(unit_);
            if (unit) pathDraft_.points.push_back({id, unit->position});
        }
        ImGui::SameLine(); if (ImGui::Button("Reverse")) std::ranges::reverse(pathDraft_.points);
        const std::array units{unit_};
        if (ImGui::Button("Apply path")) report(history_.setPatrolPath(units, pathDraft_));
        ImGui::SameLine(); if (ImGui::Button("Remove authored route")) report(history_.setPatrolPath(units, std::nullopt));
        ImGui::TreePop();
    }
    if (brainDraft_ && ImGui::TreeNode("Brain definition and cells")) {
        auto& brain = *brainDraft_;
        if (brain.cells) {
            for (std::size_t i = 0; i < brain.cells->size(); ++i) {
                auto& cell = (*brain.cells)[i]; ImGui::PushID(static_cast<int>(i));
                ImGui::InputScalar("Cell", ImGuiDataType_U32, &cell.number);
                ImGui::InputScalar("MemType", ImGuiDataType_U32, &cell.type);
                ImGui::InputDouble("Value", &cell.value);
                const bool remove = ImGui::Button("Delete cell"); ImGui::PopID();
                if (remove) { brain.cells->erase(brain.cells->begin() + i); break; }
            }
        }
        if (ImGui::Button("Add cell")) {
            if (!brain.cells) brain.cells.emplace();
            std::uint32_t number{};
            for (const auto& cell : *brain.cells) number = std::max(number, cell.number + 1);
            brain.cells->push_back({number, 1, 0});
        }
        if (brain.staticVars) ImGui::Text("Named static variables: %u (preserved)", *brain.staticVars);
        if (brain.definition) {
            ImGui::InputTextMultiline("Brain block", &*brain.definition, ImVec2(-1, 180));
            if (ImGui::Button("Remove declarative definition")) brain.definition.reset();
        } else if (ImGui::Button("Create Enhanced definition"))
            brain.definition = "Brain {\n compatibilityMode = \"Enhanced\";\n PrimaryOPORD {\n type = Guard;\n }\n}";
        if (ImGui::Button("Apply brain definition and cells")) {
            brain.module = module_;
            report(history_.setBrainAssignment(brain));
        }
        ImGui::TreePop();
    }
}
}
