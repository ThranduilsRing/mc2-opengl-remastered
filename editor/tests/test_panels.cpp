#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../tests/unit/doctest.h"
#include "PanelProof.h"
#include <chrono>
#include <cstdlib>
#include <fstream>

TEST_CASE("panels/real ImGui input reaches every command and redo without a desktop")
{
    const auto* stock = std::getenv("MC2_EDITOR_STOCK_DIR");
    REQUIRE(stock != nullptr);
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
