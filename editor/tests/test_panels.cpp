#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "PanelProof.h"
#include <chrono>
#include <fstream>
#include <iostream>

TEST_CASE("panels/chooser stock mod and marker edits reach real widgets" * doctest::test_suite("placement-live"))
{
    using namespace editor::document;
    const auto& paths = editor::tests::inputs();
    for (const auto* kind : {"chooser", "chooser_mod", "chooser_player", "chooser_render", "marker_panel"}) {
        CAPTURE(kind);
        const auto directory = std::filesystem::path("scratch/e5b2-widget-tests") /
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) / kind;
        std::filesystem::create_directories(directory);
        std::ofstream log(directory / "shell.log");
        const auto logger = [&](const std::string& line) { log << line << '\n'; log.flush(); };
        const auto seed = paths.placementGoldens / (std::string("e5_reference_") + (std::string_view(kind) == "marker_panel" ? "marker" : "before"));
        auto resolver = FileResolver::mount({paths.placementBase, paths.placementMods, std::string_view(kind) == "chooser_mod" ? "MC2X-CVE-G" : ""}); REQUIRE(resolver);
        auto policy = PlacementPolicy::load(*resolver, logger); REQUIRE(policy);
        const auto catalog = ObjectCatalog::load(*resolver, logger, resourceNames(*resolver, logger));
        auto doc = MissionDocument::read(seed.string() + ".fit", seed.string() + ".pak", logger, catalog.navMarkerObjects()); REQUIRE(doc);
        CommandHistory history(*doc);
        editor::shell::EntityPanels panels(*doc, history);
        if (std::string_view(kind).starts_with("chooser")) panels.placementPolicy(catalog, *policy, logger);
        editor::shell::PanelProof proof(*doc, history, panels, directory, kind, logger, false);
        auto* context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1440, 960); io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        try {
            bool running = true;
            while (running) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1440, 230));
                ImGui::Begin("Files"); proof.drawFiles(); ImGui::End();
                panels.draw(); ImGui::Render(); running = proof.tick();
            }
            CHECK(proof.passed());
            std::cout << "[EDITOR_WIDGET_ARTIFACT v1] name=" << kind << " directory=" << directory.generic_string() << '\n';
        } catch (...) { ImGui::DestroyContext(context); throw; }
        ImGui::DestroyContext(context);
    }
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
            std::filesystem::path(stock) / "mc2_01.pak", logger, editor::tests::inputs().markerObjects); REQUIRE(doc);
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
