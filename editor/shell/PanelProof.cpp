#include "PanelProof.h"
#include <SDL_opengl.h>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <format>

namespace editor::shell {
using namespace document;
namespace {
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
std::vector<std::byte> pak(const MissionDocument& doc) { return {doc.pakBytes().begin(), doc.pakBytes().end()}; }
void capture(const std::filesystem::path& path)
{
    constexpr unsigned width = 1440, height = 960, size = width * height * 3;
    std::vector<unsigned char> bmp(54 + size);
    const auto put = [&](unsigned offset, unsigned value) {
        for (unsigned i = 0; i < 4; ++i) bmp[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    bmp[0] = 'B'; bmp[1] = 'M'; put(2, static_cast<unsigned>(bmp.size())); put(10, 54); put(14, 40);
    put(18, width); put(22, height); bmp[26] = 1; bmp[28] = 24; put(34, size);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, bmp.data() + 54);
    for (std::size_t i = 54; i < bmp.size(); i += 3) std::swap(bmp[i], bmp[i + 2]);
    std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
    require(static_cast<bool>(file), "panel_capture_failed");
}
}
void PanelProof::click(std::string key, std::function<void()> verify) { steps_.push_back({std::move(key), {}, std::move(verify)}); }
void PanelProof::input(std::string key, std::string value) { steps_.push_back({std::move(key), std::move(value), {}}); }
void PanelProof::saveCheck(const std::string& stem)
{
    require(document_.save(output_ / stem, log_).has_value(), "panel_save_failed");
    auto reopen = MissionDocument::read(output_ / (stem + ".fit"), output_ / (stem + ".pak"), log_, document_.markerObjects());
    require(reopen.has_value(), "panel_reopen_failed");
    require(reopen->fitBytes() == document_.fitBytes() && pak(*reopen) == pak(document_), "panel_reopen_bytes_differ");
}
void PanelProof::operation(const std::string& name, const std::string& widget, std::function<void()> semantic)
{
    click("", [this] { beforeFit_ = document_.fitBytes(); beforePak_ = pak(document_); });
    click(widget, [this, name, semantic] {
        require(document_.fitBytes() != beforeFit_ || pak(document_) != beforePak_, "panel_edit_did_not_change_output");
        require(panels_.status() == "Applied" || panels_.status() == "Unit placed" || panels_.status() == "Building placed" || panels_.status() == "Object placed", "panel_command_rejected");
        semantic(); afterFit_ = document_.fitBytes(); afterPak_ = pak(document_); saveCheck(name + "-edited");
    });
    click("undo", [this, name] {
        require(document_.fitBytes() == beforeFit_ && pak(document_) == beforePak_, "panel_undo_bytes_differ"); saveCheck(name + "-undone");
    });
    click("redo", [this, name] {
        require(document_.fitBytes() == afterFit_ && pak(document_) == afterPak_, "panel_redo_bytes_differ"); saveCheck(name + "-redone");
    });
    click("undo", [this, name] {
        require(document_.fitBytes() == beforeFit_ && pak(document_) == beforePak_, "panel_final_undo_bytes_differ");
        log_("[EDITOR_PANEL_OPERATION v1] name=" + name + " changed=1 reopened=1 undo=1 redo=1 widgets=1");
    });
}
PanelProof::PanelProof(MissionDocument& doc, CommandHistory& history, EntityPanels& panels,
    std::filesystem::path output, std::string kind, Diagnostic log, bool captureRendered)
    : document_(doc), history_(history), panels_(panels), output_(std::move(output)), kind_(std::move(kind)), log_(std::move(log)), captureRendered_(captureRendered)
{
    if (kind_ == "objectives_panel") { objectivesProof(); return; }
    if (kind_ == "objectives_crud_panel") { objectiveCrudProof(); return; }
    if (kind_.starts_with("chooser")) { chooserProof(); return; }
    if (kind_ == "marker_panel") {
        EntityId marker;
        for (const auto& e : doc.entities()) if (doc.hasMarkerReference(e.id) && e.position[0] == -4288 && e.position[1] == 4288) { marker = e.id; break; }
        require(marker.value != 0, "panel_marker_missing");
        // The fixture puts the chosen marker first so selection reaches a visible row without a thousand-row scroll.
        auto placements = doc.archive().packets[1].decoded;
        const auto offset = 4 + *doc.entity(marker)->packetRecord * 40;
        std::rotate(placements.begin() + 4, placements.begin() + offset, placements.begin() + offset + 40);
        auto encoded = encodePlacements(doc.archive(), placements); require(encoded.has_value(), "marker_fixture_packet_failed");
        require(history.replace(doc.fitBytes(), std::move(*encoded), "marker-fixture", log_).has_value(), "marker_fixture_replace_failed");
        marker = doc.entities().front().id;
        saveCheck("fixture");
        files_ = std::make_unique<FileWorkflow>(doc, history, panels, output_ / "widget-save", log_);
        click("building.select"); click("building.select." + std::to_string(marker.value));
        input("building.transform.x", "-4000");
        operation("marker-transform", "building.transform.apply", [this, marker] {
            require(document_.entity(marker)->position[0] == -4000 && document_.hasMarkerReference(marker), "marker_transform_desync");
        });
        operation("marker-delete", "building.delete", [this, marker] {
            require(!document_.entity(marker), "marker_delete_missing");
        });
        click("save"); click("reopen"); return;
    }
    // Copies receive isolated, fully specified entities; stock archives remain untouched.
    const std::string seed = "FITini\n[MissionSettings]\nst MissionName = \"E3-b panel fixture\"\n"
        "st Blurb2 = \"\"\nuc scenarioTuneNum = 0\n[Script]\nst ScenarioScript = \"unit\"\n"
        "[Objectives Version]\nul Version = 2\n[Objectives]\nul NumObjectives = 0\n"
        "[Parts]\nul NumParts = 2\n[Part1]\nul ObjectNumber = 123\nst ObjectProfile = \"PM1\"\nst CSVFile = \"unit1\"\n"
        "ul Pilot = 2\nf PositionX = 12.5\nf PositionY = 7\nf Rotation = 45\nc TeamID = 0\n"
        "[Part2]\nul ObjectNumber = 124\nst ObjectProfile = \"PM2\"\nst CSVFile = \"unit2\"\n"
        "ul Pilot = 1\nf PositionX = 12.5\nf PositionY = 7\nf Rotation = 45\nc TeamID = 0\n"
        "[Warriors]\nul NumWarriors = 2\n[Warrior1]\nst Profile = \"pilot\"\n"
        "st Brain = \"dredattack01\" // legacy module\nul NumCells = 1\n"
        "Brain {\n // preserve this comment\n compatibilityMode = \"Legacy\";\n UnknownSwitch = strange;\n"
        " PrimaryOPORD { type = Patrol; Waypoint { index = 0; x = 4; y = 8; } }\n}\n"
        "[Warrior1Cell0]\nul Cell = 0\nul MemType = 1\nf Value = 17.500\n"
        "[Warrior2]\nst Profile = \"other_pilot\"\nst Brain = \"other_module\"\n"
        "[Player0]\nul DefaultTeam = 0\nFITend\n";
    auto empty = encodePlacements(doc.archive(), {}); require(empty.has_value(), "panel_fixture_packet_failed");
    require(history_.replace(seed, std::move(*empty), "e3b-fixture", log_).has_value(), "panel_fixture_seed_failed");
    BuildingPlacement building; building.objectNumber = 10; building.transform = {{12.5, 7, 0}, 45};
    require(history_.insertBuilding(building).has_value(), "panel_fixture_building_failed");
    building.objectNumber = 11;
    require(history_.insertBuilding(building).has_value(), "panel_fixture_second_building_failed");
    auto fixture = doc.fitBytes();
    require(history_.replace(fixture, pak(doc), "e3b-fixture", log_).has_value(), "panel_fixture_replace_failed");
    const auto unitId = doc.entities().back().id;
    EntityId buildingId;
    for (const auto& e : doc.entities()) if (e.packetRecord) buildingId = e.id;
    saveCheck("fixture");
    files_ = std::make_unique<FileWorkflow>(doc, history, panels, output_ / "widget-save", log_);
    click("unit.select"); click("unit.select." + std::to_string(unitId.value));
    click("building.select"); click("building.select." + std::to_string(buildingId.value));
    click("player.select"); click("player.select.player0");
    if (kind_ == "unit_panel" || kind_ == "building_panel") {
        const bool isBuilding = kind_ == "building_panel";
        const std::string prefix = isBuilding ? "building" : "unit";
        const auto selected = isBuilding ? buildingId : unitId;
        input(prefix + ".transform.x", "123.25");
        operation(prefix + "-transform", prefix + ".transform.apply", [this, selected] {
            require(document_.entity(selected)->position[0] == 123.25, "panel_transform_value_missing");
            for (const auto& e : document_.entities()) if (e.id != selected)
                require(e.position[0] == 12.5, "panel_transform_changed_coincident_entity");
        });
        click(prefix + ".team"); click(prefix + ".team.3");
        operation(prefix + "-team", prefix + ".team.apply", [this, selected] {
            require(document_.entity(selected)->team == 3, "panel_team_value_missing");
            for (const auto& e : document_.entities()) if (e.id != selected)
                require(e.team == 0, "panel_team_changed_coincident_entity");
        });
        const auto count = doc.entities().size();
        operation(prefix + "-place", prefix + ".place", [this, count] {
            require(document_.entities().size() == count + 1, "panel_placement_missing");
        });
        click(prefix + ".select"); click(prefix + ".select." + std::to_string(selected.value));
        operation(prefix + "-delete", prefix + ".delete", [this, selected] {
            require(document_.entity(selected) == nullptr, "panel_deletion_missing");
        });
    } else if (kind_ == "player_panel") {
        click("player.team"); click("player.team.3");
        operation("player-alignment", "player.team.apply", [this] {
            require(document_.find("Player0", "DefaultTeam")->value == Value{std::int64_t{3}}, "panel_player_value_missing");
        });
    } else if (kind_ == "brain_modes") {
        for (const auto* modeName : {"Hybrid", "Enhanced"}) {
            click("brain.mode");
            operation(std::string("brain-") + modeName, std::string("brain.mode.") + modeName, [this, unitId, modeName] {
                require(EntityPanels::mode(document_, unitId) == modeName, "panel_brain_mode_missing");
            });
        }
        input("brain.module", "e3b_fixture_module");
        operation("brain-module", "brain.module.apply", [this, unitId] {
            const auto* e = document_.entity(unitId);
            require(document_.find(*e->warrior, "Brain")->value == Value{std::string("e3b_fixture_module")}, "panel_module_missing");
        });
        click("brain.mode"); click("brain.mode.Hybrid", [this] { saveCheck("existing-hybrid"); });
        click("", [this] {
            auto hybrid = MissionDocument::read(output_ / "existing-hybrid.fit", output_ / "existing-hybrid.pak", log_, document_.markerObjects());
            require(hybrid.has_value(), "hybrid_fixture_reopen_failed");
            const auto fit = hybrid->fitBytes(); const auto bytes = pak(*hybrid);
            require(hybrid->save(output_ / "hybrid-roundtrip", log_).has_value(), "hybrid_roundtrip_save_failed");
            auto second = MissionDocument::read(output_ / "hybrid-roundtrip.fit", output_ / "hybrid-roundtrip.pak", log_, document_.markerObjects());
            require(second.has_value(), "hybrid_roundtrip_reopen_failed");
            require(second->fitBytes() == fit && pak(*second) == bytes &&
                EntityPanels::mode(*second, second->entities().back().id) == "Hybrid", "hybrid_roundtrip_changed");
        });
        click("brain.mode");
        operation("brain-Legacy", "brain.mode.Legacy", [this, unitId] {
            require(EntityPanels::mode(document_, unitId) == "Legacy", "panel_legacy_mode_missing");
        });
    } else require(kind_ == "panel_render", "unknown_panel_smoke");
    click("", [this] { beforeFit_ = document_.fitBytes(); beforePak_ = pak(document_); });
    click("save", [this] {
        auto saved = MissionDocument::read(output_ / "widget-save.fit", output_ / "widget-save.pak", log_, document_.markerObjects());
        require(saved && saved->fitBytes() == beforeFit_ && pak(*saved) == beforePak_, "panel_save_widget_failed");
    });
    click("reopen", [this] {
        require(document_.fitBytes() == beforeFit_ && pak(document_) == beforePak_ && !history_.canUndo() && !history_.canRedo(), "panel_reopen_widget_failed");
    });
    click("unit.select"); click("unit.select." + std::to_string(unitId.value));
    click("building.select"); click("building.select." + std::to_string(buildingId.value));
}
void PanelProof::chooserProof()
{
    require(!panels_.chooserEntries().empty(), "chooser_catalog_missing");
    saveCheck("fixture"); saveCheck("e5_reference_before");
    files_ = std::make_unique<FileWorkflow>(document_, history_, panels_, output_ / "widget-save", log_);
    struct Object { unsigned id; std::string stem; ObjectFamily family; };
    std::vector<Object> objects{{53, "Anubis", ObjectFamily::mech}, {117, "APC", ObjectFamily::vehicle},
        {263, "Barrels", ObjectFamily::building}, {525, "AutoCannonCamo1", ObjectFamily::turret},
        {252, "Birch1", ObjectFamily::tree}, {157, "D_HeavyGate", ObjectFamily::gate},
        {607, "bridge", ObjectFamily::bridge}, {68, "Spotlight", ObjectFamily::light}, {290, "Nav1", ObjectFamily::marker}};
    if (kind_ == "chooser_mod") {
        const auto entries = panels_.chooserEntries();
        const auto found = std::ranges::find_if(entries, [](const auto* e) { return e->id == 2; });
        require(found != entries.end() && (*found)->definition.layer == "mod:MC2X-CVE-G", "chooser_mod_override_missing");
        objects = {{2, (*found)->stem, (*found)->family}};
    } else if (kind_ == "chooser_render" || kind_ == "chooser_player") objects.resize(1);
    if (kind_ == "chooser_player") input("chooser.player", "2");
    input("chooser.transform.x", "-4288"); input("chooser.transform.y", "4288"); input("chooser.transform.z", "200");
    for (const auto& object : objects) {
        const auto family = std::string(familyName(object.family));
        click("", [this] { beforeView_ = chooserView(); });
        click("chooser.family");
        click("chooser.family." + std::to_string(static_cast<int>(object.family)), [this, object, family] {
            const auto filtered = panels_.chooserEntries();
            require(!filtered.empty() && std::ranges::all_of(filtered, [&](const auto* e) { return e->family == object.family; }), "chooser_browse_output_missing");
            viewCheck(family, "browse");
        });
        input("chooser.search", "no-such-e5b2-object");
        click("", [this] { beforeView_ = chooserView(); });
        input("chooser.search", object.stem);
        click("", [this, object, family] {
            const auto filtered = panels_.chooserEntries();
            require(!filtered.empty() && std::ranges::all_of(filtered, [&](const auto* e) {
                return identifier(e->stem).contains(identifier(object.stem)) || identifier(e->displayName).contains(identifier(object.stem));
            }), "chooser_search_output_missing");
            viewCheck(family, "search");
        });
        click(std::format("chooser.entry.{}.{}", object.id, object.stem), [this, object, family] {
            require(panels_.chooserSelection() && panels_.chooserSelection()->id == object.id &&
                panels_.chooserSelection()->stem == object.stem, "chooser_preview_identity_missing");
            const auto& entry = *panels_.chooserSelection();
            std::ofstream(output_ / (family + "-preview.tsv"), std::ios::binary) << std::format("{}\t{}\t{}\t{}\t{}\n",
                entry.id, entry.stem, familyName(entry.family), entry.definition.layer, entry.definition.source.generic_string());
        });
        input("chooser.transform.rotation", object.family == ObjectFamily::marker ? "4.5" : "0");
        operation("chooser-" + family, "chooser.place", [this, object, family] {
            const auto found = std::ranges::find_if(document_.entities(), [&](const Entity& e) {
                return e.objectNumber == object.id && e.position[0] == -4288 && e.position[1] == 4288;
            });
            require(found != document_.entities().end(), "chooser_insert_identity_missing");
            if (kind_ == "chooser_player") {
                require(found->section && document_.find(*found->section, "CommanderID")->value == Value{std::int64_t{2}} &&
                    document_.find(*found->section, "PlayerPart")->value == Value{false}, "chooser_current_player_missing");
            }
            require(!panels_.placementSources().empty(), "chooser_insert_sources_missing");
            std::ofstream sources(output_ / (family + "-sources.tsv"), std::ios::binary);
            for (const auto& source : panels_.placementSources())
                sources << source.key << '\t' << source.layer << '\t' << source.source.generic_string() << '\n';
            saveCheck("e5_reference_" + family);
            log_(std::format("[EDITOR_LEGACY_PLACEMENT v1] family={} fit_id={} stem={} x=-4288 y=4288 z=200 input_team=0 team={} painted=1 undo=1 redo=1 saved=1 removed=1 reopened=1",
                family, object.id, object.stem, found->team));
            if (kind_ == "chooser_mod") require(panels_.chooserSelection()->definition.layer == "mod:MC2X-CVE-G", "chooser_mod_preview_missing");
        });
        // Each family browse starts with an empty query so prior searches cannot hide entries.
        input("chooser.search", " ");
    }
    click("redo", [this] { beforeFit_ = document_.fitBytes(); beforePak_ = pak(document_); });
    click("save", [this] {
        auto saved = MissionDocument::read(output_ / "widget-save.fit", output_ / "widget-save.pak", log_, document_.markerObjects());
        require(saved && saved->fitBytes() == beforeFit_ && pak(*saved) == beforePak_, "chooser_save_widget_failed");
    });
    // A changed in-memory mission makes reopen prove replacement instead of a no-op.
    click("undo");
    click("reopen");
    click("dirty.discard", [this] {
        require(document_.fitBytes() == beforeFit_ && pak(document_) == beforePak_ && !history_.canUndo() && !history_.canRedo(), "chooser_reopen_widget_failed");
    });
    input("chooser.search", objects.back().stem);
    click(std::format("chooser.entry.{}.{}", objects.back().id, objects.back().stem));
}
std::string PanelProof::chooserView() const
{
    std::string view;
    for (const auto* entry : panels_.chooserEntries())
        view += std::format("{}\t{}\t{}\t{}\t{}\n", entry->id, entry->stem, familyName(entry->family),
            entry->definition.layer, entry->definition.source.generic_string());
    return view;
}
void PanelProof::viewCheck(const std::string& family, const std::string& stage)
{
    const auto view = chooserView();
    require(view != beforeView_, "chooser_filter_output_unchanged");
    std::ofstream(output_ / (family + "-" + stage + "-before.tsv"), std::ios::binary) << beforeView_;
    std::ofstream(output_ / (family + "-" + stage + "-after.tsv"), std::ios::binary) << view;
    log_("[EDITOR_CHOOSER_VIEW v1] family=" + family + " stage=" + stage + " changed=1 widgets=1");
}
bool PanelProof::tick()
{
    require(++frames_ < (kind_ == "objectives_panel" ? 4000U : 1500U), "panel_widget_timeout");
    if (step_ == steps_.size()) {
        require(ImGui::GetDrawData()->TotalVtxCount > 0, "panel_render_empty");
        if (captureRendered_) capture(output_ / "panels.bmp");
        log_("[EDITOR_PANEL v1] name=" + kind_ + " result=PASS widgets=1 saved_widget=1 reopened_widget=1 rendered=" + (captureRendered_ ? "1" : "0")); passed_ = true; return false;
    }
    auto& step = steps_[step_];
    if (step.widget.empty()) { if (step.verify) step.verify(); ++step_; return true; }
    const auto& widgets = step.widget.starts_with("dirty.") ? files_->widgets() : panels_.widgets();
    const auto found = widgets.find(step.widget);
    require(found != widgets.end(), "panel_widget_not_rendered");
    auto& io = ImGui::GetIO();
    if (phase_ == 0) {
        priorActivations_ = found->second.activations;
        io.AddMousePosEvent(found->second.center.x, found->second.center.y);
        io.AddMouseButtonEvent(0, true);
    } else if (phase_ == 2) io.AddMouseButtonEvent(0, false);
    if (!step.input.empty()) {
        if (phase_ == 4) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); }
        if (phase_ == 6) { io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddInputCharactersUTF8(step.input.c_str()); }
        if (phase_ == 8) io.AddKeyEvent(ImGuiKey_Enter, true);
        if (phase_ == 10) io.AddKeyEvent(ImGuiKey_Enter, false);
    }
    const auto end = step.input.empty() ? 4U : 12U;
    if (++phase_ > end) {
        if (!(found->second.activations > priorActivations_ || step.widget.ends_with(".select") || step.widget.ends_with(".team") || step.widget == "brain.mode"))
            throw std::runtime_error("panel_widget_not_activated:" + step.widget);
        if (step.verify) step.verify(); ++step_; phase_ = 0;
    }
    return true;
}
}
