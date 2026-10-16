#include "EntityPanels.h"
#include "ObjectivePresentation.h"
#include <algorithm>
#include <format>

namespace editor::shell {
using namespace document;
void EntityPanels::resourceDraft(const std::string& key, std::int64_t& id)
{
    ImGui::PushID(key.c_str());
    record(key + ".id", ImGui::InputScalar("Resource ID", ImGuiDataType_S64, &id));
    const auto* strings = catalog_ ? catalog_->strings() : nullptr;
    resourcePreview(strings, id);
    if (strings && ImGui::TreeNode("Browse existing strings (read-only)")) {
        record(key + ".search", ImGui::InputText("ID or text", &resourceSearch_));
        ImGui::BeginChild("Resource results", ImVec2(0, 160));
        for (const auto& item : strings->search(resourceSearch_)) {
            const auto label = std::format("{} / {} / {}", item.id, item.key, item.source.layer);
            if (choice(key + ".choose." + std::to_string(item.id), label.c_str(), id == item.id)) id = item.id;
        }
        ImGui::EndChild();
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EntityPanels::objectiveTextControls(const std::string& key, ObjectiveTextDraft& draft)
{
    if (ImGui::RadioButton("Literal", !draft.resource)) draft.resource = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Resource", draft.resource)) draft.resource = true;
    record(key + ".literal", textControl("Literal draft", draft.literal));
    resourceDraft(key, draft.id);
    ImGui::TextUnformatted(draft.resource ? "Preview: resource text above" : "Preview: literal text");
    if (!draft.resource) ImGui::TextUnformatted(draft.literal.data(), draft.literal.data() + draft.literal.size());
}

void EntityPanels::objectiveModelControls(const std::string& key, std::optional<ObjectiveModelDraft>& draft)
{
    if (catalog_) {
        record(key + ".search", ImGui::InputText("Find model", &modelSearch_));
        if (ImGui::BeginCombo("Catalog model", draft ? draft->name.c_str() : "Choose a model to preview")) {
            for (const auto& item : catalog_->entries()) {
                const auto label = std::format("{} / {} / {}", item.id, item.stem, item.displayName);
                if (!identifier(label).contains(identifier(modelSearch_))) continue;
                if (choice(key + ".choose." + std::to_string(item.id), label.c_str(), false)) draft = objectiveModel(item);
            }
            ImGui::EndCombo();
        }
    } else ImGui::TextUnformatted("Model catalog unavailable; authored fields retained.");
    if (draft) {
        ImGui::TextWrapped("Model preview: %s / type %lld / scale %.9g / ID %lld / version %lld",
            draft->name.c_str(), static_cast<long long>(draft->type), draft->scale,
            static_cast<long long>(draft->id), static_cast<long long>(draft->version));
        if (button((key + ".cancel").c_str(), "Cancel model preview")) draft.reset();
    }
}

void EntityPanels::objectiveTemplates()
{
    const bool open = ImGui::TreeNode("Add from template");
    record("objective.template.open", open);
    if (!open) return;
    ImGui::PushID("template");
    constexpr const char* names[] = {"Hidden trigger", "Destroy N enemy units", "Move to area"};
    const bool choosing = ImGui::BeginCombo("Recipe", names[static_cast<int>(templateDraft_.recipe)]);
    record("objective.template.recipe", choosing);
    if (choosing) {
        for (int i = 0; i < 3; ++i)
            if (choice("objective.template.choose." + std::to_string(i), names[i], static_cast<int>(templateDraft_.recipe) == i))
                templateDraft_ = objectiveTemplateDraft(static_cast<ObjectiveTemplate>(i));
        ImGui::EndCombo();
    }
    if (templateDraft_.recipe == ObjectiveTemplate::hiddenTrigger) {
        record("objective.template.condition.flag", ImGui::InputText("Condition flag ID", &templateDraft_.conditionFlag));
        record("objective.template.condition.value", ImGui::Checkbox("Condition flag value", &templateDraft_.conditionValue));
        record("objective.template.action.flag", ImGui::InputText("Action flag ID", &templateDraft_.actionFlag));
        record("objective.template.action.value", ImGui::Checkbox("Action flag value", &templateDraft_.actionValue));
    } else if (templateDraft_.recipe == ObjectiveTemplate::destroyEnemies) {
        record("objective.template.count", ImGui::InputScalar("Enemy count", ImGuiDataType_S64, &templateDraft_.count));
    } else {
        record("objective.template.x", ImGui::InputDouble("Center X", &templateDraft_.centerX));
        record("objective.template.y", ImGui::InputDouble("Center Y", &templateDraft_.centerY));
        record("objective.template.radius", ImGui::InputDouble("Radius", &templateDraft_.radius));
    }
    for (const auto& [key, draft] : {std::pair{"Title", &templateDraft_.title}, {"Description", &templateDraft_.description}}) {
        ImGui::PushID(key);
        ImGui::TextUnformatted(key);
        objectiveTextControls(std::string("objective.template.") + key, *draft);
        ImGui::PopID();
    }
    objectiveModelControls("objective.template.model", templateDraft_.model);
    if (button("objective.template.add", "Add template objective")) {
        const auto result = history_.addObjectiveTemplate(objectiveTeam_, templateDraft_);
        if (result) { objective_ = *result; status_ = "Applied"; }
        else status_ = result.error().reason;
    }
    ImGui::PopID();
    ImGui::TreePop();
}

void EntityPanels::objectivePresentation()
{
    if (!ImGui::TreeNode("Text and model presentation")) return;
    const auto value = [&](std::string_view key) -> const Scalar& { return document_.objectiveScalar(objective_, key)->value; };
    if (presentationObjective_ != objective_ || presentationRevision_ != document_.revision()) {
        for (std::size_t i = 0; i < textDrafts_.size(); ++i) {
            const std::string key = i == 0 ? "Title" : "Description";
            textDrafts_[i] = {std::get<bool>(value(key + "UseResourceString")),
                std::get<std::string>(value(key)), std::get<std::int64_t>(value(key + "ResourceStringID"))};
        }
        modelDraft_.reset();
        presentationObjective_ = objective_; presentationRevision_ = document_.revision();
    }
    for (std::size_t i = 0; i < textDrafts_.size(); ++i) {
        const std::string key = i == 0 ? "Title" : "Description";
        ImGui::PushID(key.c_str());
        auto& draft = textDrafts_[i];
        ImGui::TextUnformatted(key.c_str());
        objectiveTextControls("objective.presentation." + key, draft);
        if (button(("objective.presentation." + key + ".apply").c_str(), "Apply text and source")) {
            report(history_.setObjectiveText(objective_, key, draft));
            ImGui::PopID(); ImGui::TreePop(); return;
        }
        ImGui::PopID();
    }
    ImGui::TextWrapped("Authored model: %s / type %lld / scale %.9g / ID %lld / version %lld",
        std::get<std::string>(value("ObjectiveModel")).c_str(), static_cast<long long>(std::get<std::int64_t>(value("ModelType"))),
        std::get<double>(value("ModelScale")), static_cast<long long>(std::get<std::int64_t>(value("ModelID"))),
        static_cast<long long>(std::get<std::int64_t>(value("ModelID Version"))));
    if (catalog_) {
        const auto rawId = std::get<std::int64_t>(value("ModelID"));
        const auto found = std::ranges::find_if(catalog_->entries(), [&](const auto& item) { return item.id == rawId; });
        if (found == catalog_->entries().end()) ImGui::TextUnformatted("Custom model or missing catalog ID; authored fields retained.");
        else if (const auto model = objectiveModel(*found); model->name != std::get<std::string>(value("ObjectiveModel")) ||
            model->type != std::get<std::int64_t>(value("ModelType")) || model->scale != std::get<double>(value("ModelScale")))
            ImGui::TextUnformatted("Catalog ID differs from authored model fields; authored fields retained.");
    }
    objectiveModelControls("objective.model", modelDraft_);
    if (modelDraft_) {
        ImGui::TextWrapped("Apply writes ObjectiveModel=%s, ModelType=%lld, ModelScale=%.9g, ModelID=%lld, ModelID Version=%lld. Colors are preserved.",
            modelDraft_->name.c_str(), static_cast<long long>(modelDraft_->type), modelDraft_->scale,
            static_cast<long long>(modelDraft_->id), static_cast<long long>(modelDraft_->version));
        if (button("objective.model.apply", "Apply model preview")) {
            report(history_.setObjectiveModel(objective_, *modelDraft_));
            ImGui::TreePop(); return;
        }
    }
    ImGui::TreePop();
}
}
