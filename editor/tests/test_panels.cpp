#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "PanelProof.h"
#include <chrono>
#include <fstream>

TEST_CASE("panels/marker transform and delete buttons cannot activate" * doctest::test_suite("stock-fixture"))
{
    using namespace editor::document;
    const auto stock = std::filesystem::path(editor::tests::inputs().stockDirectory);
    auto doc = MissionDocument::read(stock / "mc2_01.fit", stock / "mc2_01.pak", {}); REQUIRE(doc);
    CommandHistory history(*doc);
    auto empty = encodePlacements(doc->archive(), {}); REQUIRE(empty);
    REQUIRE(history.replace(doc->fitBytes(), std::move(*empty), "marker-panel", {}));
    BuildingPlacement marker; marker.objectNumber = 290; marker.transform = {{100, 200, 200}, 0}; marker.markerRadius = 256;
    const auto id = history.insertBuilding(marker); REQUIRE(id);
    editor::shell::EntityPanels panels(*doc, history);
    const auto fit = doc->fitBytes();
    const auto pak = std::vector<std::byte>(doc->pakBytes().begin(), doc->pakBytes().end());
    const auto revision = doc->revision();
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1440, 960); io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto frame = [&] { ImGui::NewFrame(); panels.draw(); ImGui::Render(); };
    frame(); frame();
    CHECK(panels.building() == *id);
    for (const auto* key : {"building.transform.apply", "building.delete"}) {
        const auto position = panels.widgets().at(key).center;
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
        CHECK(panels.widgets().at(key).activations == 0);
        CHECK(panels.status().empty()); CHECK(doc->revision() == revision);
        CHECK(doc->fitBytes() == fit); CHECK(std::ranges::equal(doc->pakBytes(), pak));
    }
    ImGui::DestroyContext(context);
}

TEST_CASE("panels/real ImGui input reaches every command and redo without a desktop" * doctest::test_suite("stock-fixture"))
{
    const auto& stock = editor::tests::inputs().stockDirectory;
    for (const auto* kind : {"unit_panel", "building_panel", "player_panel", "brain_modes", "panel_render"}) {
        CAPTURE(kind);
        const auto directory = std::filesystem::path("scratch/e3b-widget-tests") /
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) / kind;
        std::filesystem::create_directories(directory);
        std::ofstream log(directory / "widgets.log");
        const auto logger = [&](const std::string& line) { log << line << '\n'; log.flush(); };
        auto doc = editor::document::MissionDocument::read(std::filesystem::path(stock) / "mc2_01.fit",
            std::filesystem::path(stock) / "mc2_01.pak", logger); REQUIRE(doc);
        editor::document::CommandHistory history(*doc);
        editor::shell::EntityPanels panels(*doc, history);
        editor::shell::PanelProof proof(*doc, history, panels, directory, kind, logger, false);
        auto* context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1440, 960); io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        try {
            bool running = true;
            while (running) { ImGui::NewFrame(); panels.draw(); ImGui::Render(); running = proof.tick(); }
            CHECK(proof.passed());
            CHECK(editor::shell::EntityPanels::opordSummary(*doc, doc->entities().back().id).find("Patrol") != std::string::npos);
        } catch (...) { ImGui::DestroyContext(context); throw; }
        ImGui::DestroyContext(context);
    }
}
