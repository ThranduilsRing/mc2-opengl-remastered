#include "EntityPanels.h"
#include <misc/cpp/imgui_stdlib.h>
#include <format>

namespace editor::shell {
using namespace document;
namespace {
std::string text(const MissionDocument& doc, NodeId section, std::string_view key)
{
    const auto* field = doc.find(section, key);
    return field && field->value.size() == 1 && std::holds_alternative<std::string>(field->value[0]) ?
        std::get<std::string>(field->value[0]) : std::string{};
}
}
std::string EntityPanels::mode(const MissionDocument& doc, EntityId id)
{
    const auto* entity = doc.entity(id);
    const auto value = entity && entity->brain ? identifier(text(doc, *entity->brain, "compatibilityMode")) : "";
    return value == "hybrid" ? "Hybrid" : value == "enhanced" ? "Enhanced" : "Legacy";
}
std::string EntityPanels::opordSummary(const MissionDocument& doc, EntityId id)
{
    const auto* entity = doc.entity(id);
    if (!entity || !entity->brain) return "No declarative OPORDs";
    std::string result;
    for (const auto* name : {"PrimaryOPORD", "SecondaryOPORD", "TertiaryOPORD"}) {
        const auto* slot = doc.childSection(*entity->brain, name);
        if (!slot) continue;
        const auto type = text(doc, slot->id, "type");
        std::size_t points{};
        for (const auto& child : doc.syntax().sections)
            if (child.parent == slot->id && identifier(child.name) == "waypoint") ++points;
        result += std::format("{}: {} ({} waypoints)\n", name, type.empty() ? "unspecified" : type, points);
    }
    return result.empty() ? "No declarative OPORDs" : result;
}
void EntityPanels::record(const std::string& key, bool activated)
{
#ifdef MC2_EDITOR_SMOKE_HOST
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto& widget = widgets_[key];
    widget.center = ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2);
    if (activated) ++widget.activations;
#else
    (void)key;
    (void)activated;
#endif
}
bool EntityPanels::button(const char* key, const char* label)
{
    const auto activated = ImGui::Button(label); record(key, activated); return activated;
}
bool EntityPanels::choice(const std::string& key, const char* label, bool selected)
{
    const auto activated = ImGui::Selectable(label, selected); record(key, activated); return activated;
}
void EntityPanels::report(std::expected<void, LoadError> result)
{
    status_ = result ? "Applied" : result.error().reason;
}
void EntityPanels::refresh()
{
    if (revision_ == document_.revision()) return;
    revision_ = document_.revision();
    for (const bool building : {false, true}) {
        auto& selected = building ? building_ : unit_;
        if (!document_.entity(selected)) {
            selected = {};
            for (const auto& e : document_.entities()) if (e.packetRecord.has_value() == building) { selected = e.id; break; }
        }
    }
    if (const auto* e = document_.entity(unit_)) {
        unitDraft_.objectNumber = e->objectNumber;
        unitDraft_.objectProfile = e->objectProfile.value_or("");
        unitDraft_.csvFile = e->csvFile.value_or("");
        unitDraft_.transform = {e->position, e->rotation}; unitTeam_ = static_cast<int>(e->team);
        module_ = e->warrior ? text(document_, *e->warrior, "Brain") : "";
        unitDraft_.warriorProfile = e->warrior ? text(document_, *e->warrior, "Profile") : "";
    }
    if (const auto* e = document_.entity(building_)) {
        buildingDraft_.objectNumber = e->objectNumber;
        buildingDraft_.transform = {e->position, e->rotation}; buildingTeam_ = static_cast<int>(e->team);
    }
    if (!document_.section(player_)) {
        player_ = {};
        for (const auto& section : document_.syntax().sections) {
            const auto name = identifier(section.name);
            if (!section.parent && name.size() == 7 && name.starts_with("player") && name.back() >= '0' && name.back() <= '7') { player_ = section.id; break; }
        }
    }
    if (const auto* field = document_.find(player_, "DefaultTeam")) playerTeam_ = static_cast<int>(std::get<std::int64_t>(field->value[0]));
    else playerTeam_ = 0;
}
void EntityPanels::selector(bool building)
{
    auto& selected = building ? building_ : unit_;
    const auto key = building ? "building.select" : "unit.select";
    const auto preview = selected.value ? std::format("Document ID {}", selected.value) : "No selection";
    const bool open = ImGui::BeginCombo("Selection", preview.c_str()); record(key);
    if (open) {
        for (const auto& e : document_.entities()) {
            if (e.packetRecord.has_value() != building) continue;
            const auto label = std::format("ID {} / object {}", e.id.value, e.objectNumber);
            if (choice(std::format("{}.{}", key, e.id.value), label.c_str(), selected == e.id)) {
                selected = e.id; revision_ = ~std::uint64_t{};
            }
        }
        ImGui::EndCombo();
    }
}
void EntityPanels::team(const char* key, int& value, bool neutral)
{
    const auto preview = value == -1 ? "Neutral" : std::format("Team {}", value);
    const bool open = ImGui::BeginCombo("Team / alignment", preview.c_str()); record(key);
    if (open) {
        for (int i = neutral ? -1 : 0; i < 8; ++i) {
            const auto label = i == -1 ? "Neutral" : std::format("Team {}", i);
            if (choice(std::format("{}.{}", key, i), label.c_str(), i == value)) value = i;
        }
        ImGui::EndCombo();
    }
}
void EntityPanels::transform(const char* key, Transform& value)
{
    const auto changed = ImGui::InputDouble("X", &value.position[0]); record(std::string(key) + ".x", changed);
    ImGui::InputDouble("Y", &value.position[1]);
    ImGui::InputDouble("Z", &value.position[2]);
    ImGui::InputDouble("Rotation", &value.rotation);
}
void EntityPanels::draw()
{
    refresh();
    ImGui::SetNextWindowPos(ImVec2(0, 230)); ImGui::SetNextWindowSize(ImVec2(1440, 730));
    ImGui::Begin("Entities and players", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    if (button("undo", "Undo panel edit")) { history_.undo(); revision_ = ~std::uint64_t{}; }
    ImGui::SameLine();
    if (button("redo", "Redo panel edit")) { history_.redo(); revision_ = ~std::uint64_t{}; }
    ImGui::SameLine(); ImGui::BeginDisabled(!save_);
    if (button("save", "Save mission")) save_();
    ImGui::SameLine();
    if (button("reopen", "Reopen saved mission") && reopen_) reopen_();
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextUnformatted(status_.c_str());
    refresh();
    ImGui::Columns(3, "panels", true);
    ImGui::PushID("unit"); ImGui::TextUnformatted("Unit"); selector(false); refresh();
    ImGui::TextUnformatted("Placement defaults (copied from selection)");
    ImGui::InputScalar("Object number", ImGuiDataType_U32, &unitDraft_.objectNumber);
    ImGui::InputText("Object profile", &unitDraft_.objectProfile);
    ImGui::InputText("CSV file", &unitDraft_.csvFile);
    ImGui::InputText("Pilot profile", &unitDraft_.warriorProfile);
    transform("unit.transform", unitDraft_.transform); team("unit.team", unitTeam_, true);
    if (button("unit.transform.apply", "Apply transform")) report(history_.transform(unit_, unitDraft_.transform));
    ImGui::SameLine(); if (button("unit.team.apply", "Apply team")) report(history_.setTeam(unit_, unitTeam_));
    if (button("unit.place", "Place unit")) {
        unitDraft_.team = unitTeam_; unitDraft_.legacyBrain = module_;
        auto result = history_.insertUnit(unitDraft_);
        if (result) { unit_ = *result; status_ = "Unit placed"; } else status_ = result.error().reason;
    }
    ImGui::SameLine(); if (button("unit.delete", "Delete unit")) report(history_.erase(unit_));
    refresh();
    const auto* selected = document_.entity(unit_);
    ImGui::Separator(); ImGui::TextUnformatted("Brain assignment");
    const bool assigned = selected && selected->warrior.has_value();
    ImGui::BeginDisabled(!assigned);
    const auto current = mode(document_, unit_);
    const bool open = ImGui::BeginCombo("Compatibility mode", current.c_str()); record("brain.mode");
    if (open) {
        for (const auto* modeName : {"Legacy", "Hybrid", "Enhanced"})
            if (choice(std::string("brain.mode.") + modeName, modeName, current == modeName)) report(history_.setBrainMode(unit_, modeName));
        ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Runtime Hybrid arbitration is not implemented yet.");
    const auto changed = ImGui::InputText("Legacy ABL module", &module_); record("brain.module", changed);
    if (button("brain.module.apply", "Apply ABL reference")) report(history_.setLegacyBrain(unit_, module_));
    ImGui::EndDisabled();
    if (!assigned) ImGui::TextUnformatted("No explicit Pilot assignment; place a unit with a pilot profile.");
    ImGui::TextUnformatted("Enhanced OPORD summary (read only)");
    ImGui::TextWrapped("%s", opordSummary(document_, unit_).c_str());
    ImGui::PopID(); ImGui::NextColumn();
    ImGui::PushID("building"); ImGui::TextUnformatted("Building"); selector(true); refresh();
    ImGui::TextUnformatted("Placement defaults (copied from selection)");
    ImGui::InputScalar("Object number", ImGuiDataType_U32, &buildingDraft_.objectNumber);
    transform("building.transform", buildingDraft_.transform); team("building.team", buildingTeam_, true);
    if (button("building.transform.apply", "Apply transform")) report(history_.transform(building_, buildingDraft_.transform));
    ImGui::SameLine(); if (button("building.team.apply", "Apply team")) report(history_.setTeam(building_, buildingTeam_));
    if (button("building.place", "Place building")) {
        buildingDraft_.team = buildingTeam_;
        auto result = history_.insertBuilding(buildingDraft_);
        if (result) { building_ = *result; status_ = "Building placed"; } else status_ = result.error().reason;
    }
    ImGui::SameLine(); if (button("building.delete", "Delete building")) report(history_.erase(building_));
    ImGui::PopID(); ImGui::NextColumn();
    ImGui::PushID("player"); ImGui::TextUnformatted("Player");
    const auto* p = document_.section(player_);
    const bool players = ImGui::BeginCombo("Selection", p ? p->name.c_str() : "No authored players"); record("player.select");
    if (players) {
        for (const auto& section : document_.syntax().sections) {
            const auto name = identifier(section.name);
            if (!section.parent && name.size() == 7 && name.starts_with("player") && name.back() >= '0' && name.back() <= '7')
                if (choice("player.select." + name, section.name.c_str(), player_ == section.id)) { player_ = section.id; revision_ = ~std::uint64_t{}; }
        }
        ImGui::EndCombo();
    }
    refresh(); team("player.team", playerTeam_, false);
    ImGui::BeginDisabled(!player_.value);
    if (button("player.team.apply", "Apply player alignment")) report(history_.setPlayerTeam(player_, playerTeam_));
    ImGui::EndDisabled();
    ImGui::TextWrapped("Player identity is the authored PlayerN section. Alignment sets DefaultTeam.");
    ImGui::PopID(); ImGui::Columns(1); ImGui::End();
}
}
