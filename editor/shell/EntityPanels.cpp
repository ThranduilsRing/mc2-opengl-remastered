#include "EntityPanels.h"
#include <misc/cpp/imgui_stdlib.h>
#include <format>
#include <algorithm>

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
void EntityPanels::record(std::string_view key, bool activated)
{
#ifdef MC2_EDITOR_SMOKE_HOST
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto& widget = widgets_[std::string(key)];
    widget.center = ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2);
    if (activated) ++widget.activations;
#else
    (void)key;
    (void)activated;
#endif
}
bool EntityPanels::button(std::string_view key, const char* label)
{
    const auto activated = ImGui::Button(label); record(key, activated); return activated;
}
bool EntityPanels::choice(std::string_view key, const char* label, bool selected)
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
        brainPathError_.clear();
        auto path = document_.patrolPath(unit_);
        pathDraft_ = path && *path ? **path : document::PatrolPath{e->exportedPart.value_or(1), "", false, 2, {}};
        if (!path) brainPathError_ = path.error().reason;
        auto brain = document_.brainAssignment(unit_);
        brainDraft_ = brain ? std::optional(*brain) : std::nullopt;
        if (!brain && e->warrior) brainPathError_ += " " + brain.error().reason;
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
void EntityPanels::team(std::string_view key, int& value, bool neutral)
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
void EntityPanels::transform(std::string_view key, Transform& value)
{
    const auto changed = ImGui::InputDouble("X", &value.position[0]); record(std::string(key) + ".x", changed || ImGui::IsItemActivated());
    const auto y = ImGui::InputDouble("Y", &value.position[1]); record(std::string(key) + ".y", y || ImGui::IsItemActivated());
    const auto z = ImGui::InputDouble("Z", &value.position[2]); record(std::string(key) + ".z", z || ImGui::IsItemActivated());
    const auto r = ImGui::InputDouble("Rotation", &value.rotation); record(std::string(key) + ".rotation", r || ImGui::IsItemActivated());
}
std::expected<EntityId, LoadError> EntityPanels::place(bool building)
{
    if (!policy_) return building ? history_.insertBuilding(buildingDraft_) : history_.insertUnit(unitDraft_);
    const auto reject = [&](std::string reason) {
        if (placementLog_) placementLog_("[EDITOR_PLACEMENT_REJECT v1] reason=" + reason);
        return std::unexpected(LoadError{"placement", std::move(reason), 0});
    };
    const auto found = std::ranges::find_if(catalog_->entries(), [&](const auto& entry) {
        const bool unit = entry.family == ObjectFamily::mech || entry.family == ObjectFamily::vehicle;
        return unit != building && entry.id == (building ? buildingDraft_.objectNumber : unitDraft_.objectNumber) &&
            (building || identifier(entry.stem) == identifier(unitDraft_.csvFile));
    });
    if (found == catalog_->entries().end()) return reject("catalog_identity_missing");
    return place(*found, building ? buildingDraft_.transform : unitDraft_.transform, building ? 0 : unitDraft_.variant);
}
std::expected<EntityId, LoadError> EntityPanels::place(const CatalogEntry& entry, const Transform& transform, unsigned variant)
{
    const auto* mapped = document_.find("Player" + std::to_string(placementPlayer_), "DefaultTeam");
    if (!mapped || mapped->value.size() != 1 || !std::holds_alternative<std::int64_t>(mapped->value[0]))
    {
        if (placementLog_) placementLog_("[EDITOR_PLACEMENT_REJECT v1] reason=player_mapping_missing");
        return std::unexpected(LoadError{"placement", "player_mapping_missing", 0});
    }
    PlacementChoice choice{placementPlayer_, static_cast<std::int32_t>(std::get<std::int64_t>(mapped->value[0])),
        transform, variant};
    auto plan = policy_->make(document_, entry, choice, placementLog_);
    if (!plan) return std::unexpected(plan.error());
    auto inserted = plan->insert(history_);
    if (inserted) placementSources_ = plan->sources;
    return inserted;
}
std::vector<const CatalogEntry*> EntityPanels::chooserEntries() const
{
    std::vector<const CatalogEntry*> entries;
    if (!catalog_) return entries;
    auto search = identifier(chooserSearch_);
    const auto first = search.find_first_not_of(" \t");
    search = first == search.npos ? "" : search.substr(first, search.find_last_not_of(" \t") - first + 1);
    for (const auto& entry : catalog_->entries()) {
        if (chooserFamily_ >= 0 && static_cast<int>(entry.family) != chooserFamily_) continue;
        if (!search.empty() && identifier(entry.displayName).find(search) == std::string::npos &&
            identifier(entry.stem).find(search) == std::string::npos) continue;
        entries.push_back(&entry);
    }
    return entries;
}
const CatalogEntry* EntityPanels::chooserSelection() const { return chooserSelected_; }
void EntityPanels::chooser()
{
    ImGui::PushID("chooser"); ImGui::TextUnformatted("Object chooser");
    if (!catalog_ || !policy_) {
        ImGui::TextWrapped("Open with a game data root to load the object catalog."); ImGui::PopID(); return;
    }
    ImGui::Columns(2, "chooser-columns", true);
    const bool open = ImGui::BeginCombo("Family", chooserFamily_ < 0 ? "All families" : familyName(static_cast<ObjectFamily>(chooserFamily_)).data(), ImGuiComboFlags_HeightLargest);
    record("chooser.family", open);
    if (open) {
        for (int i = -1; i < 9; ++i) {
            const auto label = i < 0 ? "All families" : familyName(static_cast<ObjectFamily>(i)).data();
            if (choice("chooser.family." + std::to_string(i), label, chooserFamily_ == i)) {
                chooserFamily_ = i; chooserSelected_ = nullptr;
            }
        }
        ImGui::EndCombo();
    }
    const auto changed = ImGui::InputText("Search name", &chooserSearch_); record("chooser.search", changed);
    if (changed) chooserSelected_ = nullptr;
    const auto entries = chooserEntries();
    ImGui::Text("%zu matching objects", entries.size());
    ImGui::BeginChild("catalog", ImVec2(0, 155), true);
    for (const auto* entry : entries) {
        const auto key = std::format("chooser.entry.{}.{}", entry->id, entry->stem);
        const auto label = std::format("{} / {}##{}", entry->displayName, entry->id, entry->stem);
        if (choice(key, label.c_str(), chooserSelected_ == entry)) { chooserSelected_ = entry; chooserVariant_ = 0; }
    }
    ImGui::EndChild();
    ImGui::NextColumn();
    if (chooserSelected_) {
        ImGui::Text("ID %u / %s", chooserSelected_->id, familyName(chooserSelected_->family).data());
        const auto source = [](const char* label, const CatalogSource& source) {
            ImGui::TextWrapped("%s: %s\n%s\n%s", label, source.layer.c_str(), source.key.c_str(), source.source.generic_string().c_str());
        };
        source("Enumeration", chooserSelected_->enumeration);
        source("Definition", chooserSelected_->definition);
        if (chooserSelected_->variantFile) source("Variants", *chooserSelected_->variantFile);
    }
    const auto playerChanged = ImGui::InputInt("Current player (0..7)", &placementPlayer_); record("chooser.player", playerChanged);
    transform("chooser.transform", chooserTransform_);
    ImGui::InputScalar("Variant", ImGuiDataType_U32, &chooserVariant_);
    ImGui::BeginDisabled(!chooserSelected_);
    if (button("chooser.place", "Place selected object")) {
        auto result = place(*chooserSelected_, chooserTransform_, chooserVariant_);
        if (result) {
            if (document_.entity(*result)->packetRecord) building_ = *result; else unit_ = *result;
            status_ = "Object placed";
        } else status_ = result.error().reason;
    }
    ImGui::EndDisabled(); ImGui::Columns(1); ImGui::PopID();
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
    if (button("objective.toggle", showObjectives_ ? "Entities" : "Objectives")) showObjectives_ = !showObjectives_;
    if (showObjectives_) { objectives(); ImGui::End(); return; }
    if (policy_) {
        if (button("chooser.toggle", showChooser_ ? "Entity properties" : "Object chooser")) showChooser_ = !showChooser_;
        if (showChooser_) { chooser(); ImGui::End(); return; }
    }
    refresh();
    ImGui::Columns(3, "panels", true);
    ImGui::PushID("unit"); ImGui::TextUnformatted("Unit"); selector(false); refresh();
    ImGui::TextUnformatted(policy_ ? "Catalog placement uses the selected player defaults" : "Placement defaults (copied from selection)");
    ImGui::InputScalar("Object number", ImGuiDataType_U32, &unitDraft_.objectNumber);
    ImGui::BeginDisabled(policy_ != nullptr);
    ImGui::InputText("Object profile", &unitDraft_.objectProfile);
    ImGui::InputText("Pilot profile", &unitDraft_.warriorProfile);
    ImGui::EndDisabled();
    ImGui::InputText("CSV file", &unitDraft_.csvFile);
    if (policy_) ImGui::InputScalar("Variant", ImGuiDataType_U32, &unitDraft_.variant);
    transform("unit.transform", unitDraft_.transform); team("unit.team", unitTeam_, true);
    if (button("unit.transform.apply", "Apply transform")) report(history_.transform(unit_, unitDraft_.transform));
    ImGui::SameLine(); if (button("unit.team.apply", "Apply team")) report(history_.setTeam(unit_, unitTeam_));
    if (button("unit.place", "Place unit")) {
        unitDraft_.team = unitTeam_; unitDraft_.legacyBrain = module_;
        auto result = place(false);
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
    brainPaths();
    ImGui::PopID(); ImGui::NextColumn();
    ImGui::PushID("building"); ImGui::TextUnformatted("Building"); selector(true); refresh();
    ImGui::TextUnformatted("Placement defaults (copied from selection)");
    ImGui::InputScalar("Object number", ImGuiDataType_U32, &buildingDraft_.objectNumber);
    transform("building.transform", buildingDraft_.transform); team("building.team", buildingTeam_, true);
    if (button("building.transform.apply", "Apply transform")) report(history_.transform(building_, buildingDraft_.transform));
    ImGui::SameLine(); if (button("building.team.apply", "Apply team")) report(history_.setTeam(building_, buildingTeam_));
    if (button("building.place", "Place building")) {
        buildingDraft_.team = buildingTeam_;
        auto result = place(true);
        if (result) { building_ = *result; status_ = "Building placed"; } else status_ = result.error().reason;
    }
    ImGui::SameLine();
    if (button("building.delete", "Delete building")) report(history_.erase(building_));
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
    ImGui::PopID();
    ImGui::Columns(1); ImGui::End();
}
}
