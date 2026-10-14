#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "FileProof.h"
#include "ShellFrame.h"
#include <chrono>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace editor::document;
using namespace editor::shell;

TEST_CASE("files/real widgets own lifecycle decisions and preserve failed transitions" * doctest::test_suite("stock-fixture"))
{
    const auto& stock = editor::tests::inputs().stockDirectory;
    for (const auto* kind : {"file_save", "file_save_as", "file_recent", "file_failures",
        "file_open_save", "file_open_discard", "file_open_cancel",
        "file_reopen_save", "file_reopen_discard", "file_reopen_cancel",
        "file_close_save", "file_close_discard", "file_close_cancel"}) {
        CAPTURE(kind);
        const auto directory = std::filesystem::absolute(std::filesystem::path("scratch/e4-widget-tests") /
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) / kind);
        std::filesystem::create_directories(directory);
        std::ofstream log(directory / "widgets.log");
        unsigned diagnostics{};
        const auto logger = [&](const std::string& line) {
            if (line.find("rejected=1") != std::string::npos) ++diagnostics;
            log << line << '\n'; log.flush();
        };
        auto doc = MissionDocument::read(std::filesystem::path(stock) / "mc2_01.fit", std::filesystem::path(stock) / "mc2_01.pak", logger);
        REQUIRE(doc);
        CommandHistory history(*doc); EntityPanels panels(*doc, history);
        FileWorkflow files(*doc, history, panels, directory / "initial", logger, {}, directory / "recent.txt");
        FileProof proof(*doc, history, panels, files, directory, kind, logger, [&] { files.request(FileWorkflow::Action::Close); });
        auto* context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1440, 960); io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        try {
            bool running = true;
            while (running) {
                ImGui::NewFrame();
                running = drawWhileOpen(files, [&] {
                    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1440, 230});
                    ImGui::Begin("Files", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize); files.draw(); ImGui::End();
                }, [&] { panels.draw(); });
                ImGui::Render(); const bool more = proof.tick(); running = running && more;
            }
            proof.finish(files.closing());
            CHECK(proof.passed());
            CHECK(diagnostics == (std::string_view(kind) == "file_failures" ? 5 : 0));
            const auto expectedRecent = files.recent();
            {
                FileWorkflow restarted(*doc, history, panels, files.active(), logger, {}, directory / "recent.txt");
                CHECK(restarted.recent() == expectedRecent);
            }
        } catch (...) { ImGui::DestroyContext(context); throw; }
        ImGui::DestroyContext(context);
    }
}

TEST_CASE("files/malformed recent lists reject once and preserve the live history" * doctest::test_suite("stock-fixture"))
{
    const auto& stock = editor::tests::inputs().stockDirectory;
    auto doc = MissionDocument::read(std::filesystem::path(stock) / "mc2_01.fit", std::filesystem::path(stock) / "mc2_01.pak", {});
    REQUIRE(doc);
    CommandHistory history(*doc); EntityPanels panels(*doc, history);
    REQUIRE(history.setText(doc->find("MissionSettings", "MissionName")->id, "retained edit", "Name"));
    const auto before = doc->fitBytes();
    const auto revision = doc->revision(), used = history.bytesUsed();
    const auto directory = std::filesystem::path("scratch/e4-recent-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    const auto valid = FileWorkflow::pathText(std::filesystem::absolute(std::filesystem::path(stock) / "mc2_01.fit"));
    for (const auto& invalid : {std::string("bad\0path\n", 9), std::string(12 * 32768 + 1, 'p'), std::string(13, '\n'),
        std::string("relative.fit\n"), valid + ".pak\n", valid + "\nrelative.fit\n"}) {
        const auto store = directory / ("recent-" + std::to_string(invalid.size()) + ".txt");
        { std::ofstream output(store, std::ios::binary); output.write(invalid.data(), invalid.size()); }
        std::vector<std::string> diagnostics;
        FileWorkflow files(*doc, history, panels, std::filesystem::path(stock) / "mc2_01.fit",
            [&](const std::string& line) { diagnostics.push_back(line); }, {}, store);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].find("rejected=1") != std::string::npos);
        CHECK(diagnostics[0].find('\n') == std::string::npos);
        CHECK(files.recent().size() == 1);
        CHECK(doc->fitBytes() == before);
        CHECK(doc->revision() == revision);
        CHECK(history.bytesUsed() == used);
        CHECK(history.canUndo());
    }
    REQUIRE(history.undo());
    const auto undone = doc->fitBytes();
    const auto undoRevision = doc->revision();
    std::vector<std::string> diagnostics;
    FileWorkflow files(*doc, history, panels, std::filesystem::path(stock) / "mc2_01.fit",
        [&](const std::string& line) { diagnostics.push_back(line); });
    const auto active = files.active();
    files.request(FileWorkflow::Action::Open, directory / "missing.fit");
    CHECK(diagnostics.size() == 1);
    std::filesystem::create_directory(directory / "blocked.pak");
    CHECK_FALSE(files.save(directory / "blocked"));
    CHECK(diagnostics.size() == 2);
    CHECK(doc->fitBytes() == undone);
    CHECK(doc->revision() == undoRevision);
    CHECK(history.bytesUsed() == used);
    CHECK_FALSE(history.canUndo());
    CHECK(history.canRedo());
    CHECK(files.active() == active);
}

TEST_CASE("files/recent publication failure preserves memory and disk while save commits" * doctest::test_suite("stock-fixture"))
{
    const auto& stock = editor::tests::inputs().stockDirectory;
    auto doc = MissionDocument::read(std::filesystem::path(stock) / "mc2_01.fit", std::filesystem::path(stock) / "mc2_01.pak", {});
    REQUIRE(doc);
    const auto directory = std::filesystem::absolute(std::filesystem::path("scratch/e4-recent-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    CommandHistory history(*doc); EntityPanels panels(*doc, history);
    std::vector<std::string> diagnostics;
    const auto logger = [&](const std::string& line) { diagnostics.push_back(line); };
    const auto store = directory / "recent.txt";
    FileWorkflow files(*doc, history, panels, directory / "initial.fit", logger, {}, store);
    const auto recent = files.recent();
    const auto diskText = [&] { std::ifstream input(store, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {}); };
    const auto before = diskText();
    const auto title = doc->find("MissionSettings", "MissionName")->id;
    REQUIRE(history.setText(title, "committed despite recent failure", "Name"));
    const auto bytes = doc->fitBytes(); const auto revision = doc->revision();
#ifdef _WIN32
    const auto locked = CreateFileW(store.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(locked != INVALID_HANDLE_VALUE);
    const bool saved = files.save(directory / "saved.fit");
    CloseHandle(locked);
    CHECK(saved);
    CHECK(diskText() == before);
    CHECK(files.recent() == recent);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0] == "[EDITOR_FILES v1] recent_updated=0 reason=recent_write_failed");
    CHECK(files.status() == "Saved; recent list update failed");
    CHECK(files.active() == directory / "saved.fit");
    CHECK(doc->revision() == revision);
    CHECK(doc->fitBytes() == bytes);
    CHECK_FALSE(doc->modified());
    CHECK(history.canUndo());
    auto reopened = MissionDocument::read(directory / "saved.fit", directory / "saved.pak", {});
    REQUIRE(reopened); CHECK(reopened->fitBytes() == bytes);
    REQUIRE(history.undo()); CHECK(doc->modified());
    REQUIRE(history.redo()); CHECK_FALSE(doc->modified());
    diagnostics.clear();
    CHECK(files.save()); CHECK(diagnostics.empty());
    CHECK(files.recent().front() == files.active()); CHECK(diskText() != before);
#endif
    const auto blocker = directory / "blocked";
    { std::ofstream output(blocker); output << "not a directory"; }
    diagnostics.clear();
    FileWorkflow blocked(*doc, history, panels, directory / "initial.fit", logger, {}, blocker / "recent.txt");
    CHECK(blocked.recent().empty());
    diagnostics.clear();
    CHECK(blocked.save(directory / "stage-failure.fit"));
    CHECK(blocked.recent().empty());
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0] == "[EDITOR_FILES v1] recent_updated=0 reason=recent_write_failed");
    CHECK(blocked.status() == "Saved; recent list update failed");
    CHECK_FALSE(doc->modified());
}

TEST_CASE("files/accepted close suppresses a queued editable click with an unguarded negative control" * doctest::test_suite("stock-fixture"))
{
    const auto& stock = editor::tests::inputs().stockDirectory;
    for (bool guard : {true, false}) {
        auto doc = MissionDocument::read(std::filesystem::path(stock) / "mc2_01.fit", std::filesystem::path(stock) / "mc2_01.pak", {});
        REQUIRE(doc); CommandHistory history(*doc); EntityPanels panels(*doc, history);
        FileWorkflow files(*doc, history, panels, std::filesystem::path(stock) / "mc2_01.fit", {});
        const auto before = doc->fitBytes(); const auto revision = doc->revision();
        auto* context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = {1440, 960}; io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImVec2 center{};
        const auto editable = [&] {
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({400, 200});
            ImGui::Begin("Queued edit");
            if (ImGui::Button("Apply name")) CHECK(history.setText(doc->find("MissionSettings", "MissionName")->id, "unexpected late edit", "Name"));
            const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(); center = {(a.x + b.x) / 2, (a.y + b.y) / 2};
            ImGui::End();
        };
        for (unsigned frame = 0; frame < 7; ++frame) {
            if (frame == 2) io.AddMousePosEvent(center.x, center.y);
            if (frame == 4) io.AddMouseButtonEvent(0, true);
            if (frame == 6) { io.AddMouseButtonEvent(0, false); files.request(FileWorkflow::Action::Close); }
            ImGui::NewFrame();
            if (guard) drawWhileOpen(files, [] {}, editable); else editable();
            ImGui::Render();
        }
        CHECK(files.closing());
        CHECK((doc->fitBytes() == before) == guard);
        CHECK((doc->revision() == revision) == guard);
        CHECK(history.canUndo() == !guard);
        bool panelsDrawn{};
        CHECK_FALSE(drawWhileOpen(files, [] {}, [&] { panelsDrawn = true; }));
        CHECK_FALSE(panelsDrawn);
        ImGui::DestroyContext(context);
    }
}

TEST_CASE("files/close proof cannot terminate a loop missing production close handling" * doctest::test_suite("stock-fixture"))
{
    const auto& stock = editor::tests::inputs().stockDirectory;
    auto doc = MissionDocument::read(std::filesystem::path(stock) / "mc2_01.fit", std::filesystem::path(stock) / "mc2_01.pak", {});
    REQUIRE(doc); CommandHistory history(*doc); EntityPanels panels(*doc, history);
    const auto directory = std::filesystem::absolute(std::filesystem::path("scratch/e4-widget-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) / "close-negative");
    std::filesystem::create_directories(directory);
    FileWorkflow files(*doc, history, panels, directory / "initial", {});
    FileProof proof(*doc, history, panels, files, directory, "file_close_discard", [](const std::string&) {},
        [&] { files.request(FileWorkflow::Action::Close); });
    auto* context = ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = {1440, 960}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    try {
        unsigned extraFrames{};
        bool running = true;
        for (unsigned frame = 0; frame < 1000 && extraFrames < 3; ++frame) {
            ImGui::NewFrame();
            drawWhileOpen(files, [&] {
                ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1440, 230});
                ImGui::Begin("Files"); files.draw(); ImGui::End();
            }, [&] { panels.draw(); });
            ImGui::Render(); running = proof.tick();
            if (!running) break;
            if (files.closing()) ++extraFrames;
        }
        CHECK(running); CHECK(extraFrames == 3); CHECK(proof.completed()); CHECK_FALSE(proof.passed());
        CHECK_THROWS_WITH(proof.finish(false), "file_loop_termination_missing");
    } catch (...) { ImGui::DestroyContext(context); throw; }
    ImGui::DestroyContext(context);
}
