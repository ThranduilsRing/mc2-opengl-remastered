#include "CommandHistory.h"
#include "build_fingerprint.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace editor::document;

struct Options {
    std::filesystem::path mission, output, log;
    bool smoke{};
};
Options options(int argc, char** argv)
{
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--smoke") result.smoke = true;
        else if (i + 1 < argc && key == "--mission") result.mission = argv[++i];
        else if (i + 1 < argc && key == "--output") result.output = argv[++i];
        else if (i + 1 < argc && key == "--log") result.log = argv[++i];
        else throw std::runtime_error("invalid_shell_argument");
    }
    if (result.mission.empty()) throw std::runtime_error("mission_required");
    if (result.smoke && (result.output.empty() || result.log.empty())) throw std::runtime_error("smoke_output_and_log_required");
    return result;
}

class Shell {
public:
    explicit Shell(Options options) : options_(std::move(options)), logFile_(options_.log, std::ios::trunc) {}
    ~Shell() {
        if (rendererReady_) ImGui_ImplOpenGL3_Shutdown();
        if (platformReady_) ImGui_ImplSDL2_Shutdown();
        if (imgui_) ImGui::DestroyContext(imgui_);
        if (context_) SDL_GL_DeleteContext(context_);
        if (window_) SDL_DestroyWindow(window_);
        if (sdlReady_) SDL_Quit();
    }
    int run() {
        log(MC2_BUILD_FP_LITERAL);
        auto fit = options_.mission; fit.replace_extension(".fit");
        auto pak = options_.mission; pak.replace_extension(".pak");
        auto loaded = MissionDocument::read(fit, pak, logger());
        if (!loaded) return 2;
        document_ = std::move(*loaded);
        history_ = std::make_unique<CommandHistory>(*document_);
        auto* title = document_->find("MissionSettings", "MissionName");
        if (title && title->value.size() == 1 && std::holds_alternative<std::string>(title->value[0])) titleId_ = title->id;
        resetDraft();
        if (options_.smoke) {
            if (!history_->setText(titleId_, longSmokeName(), "Mission name")) return error("long_name_setup_failed");
            resetDraft();
        }
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
        // Harness runs never occupy the desktop or request pointer capture.
        const bool hidden = options_.smoke || std::getenv("MC2_WINDOW_OFFSCREEN") != nullptr;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return error("sdl_init_failed");
        sdlReady_ = true;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        window_ = SDL_CreateWindow("Mission document", hidden ? -16000 : SDL_WINDOWPOS_CENTERED,
            hidden ? -16000 : SDL_WINDOWPOS_CENTERED, 1100, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
        if (!window_) return error("sdl_window_failed");
        context_ = SDL_GL_CreateContext(window_);
        if (!context_) return error("sdl_context_failed");
        imgui_ = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        platformReady_ = ImGui_ImplSDL2_InitForOpenGL(window_, context_);
        rendererReady_ = ImGui_ImplOpenGL3_Init("#version 330");
        if (!platformReady_ || !rendererReady_) return error("imgui_backend_failed");
        bool running = true;
        unsigned frames{};
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                // ImGui's SDL backend can capture the mouse; off-screen pumps do not enable input.
                if (!hidden) ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
            }
            ImGui_ImplOpenGL3_NewFrame();
            if (!hidden) ImGui_ImplSDL2_NewFrame();
            else { ImGui::GetIO().DisplaySize = ImVec2(1100, 720); ImGui::GetIO().DeltaTime = 1.0f / 60.0f; }
            ImGui::NewFrame();
            showDocument();
            ImGui::Render();
            glViewport(0, 0, 1100, 720);
            glClearColor(0.06f, 0.07f, 0.09f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(window_);
            if (options_.smoke && ++frames == 3) running = false;
            if (!options_.smoke) SDL_Delay(10);
        }
        if (options_.smoke) return smoke(frames);
        return 0;
    }
private:
    Diagnostic logger() { return [this](const std::string& line) { log(line); }; }
    void log(const std::string& line) { if (logFile_) { logFile_ << line << '\n'; logFile_.flush(); } else std::cerr << line << '\n'; }
    int error(const char* reason) { log(std::string("[EDITOR_SHELL v1] result=FAIL reason=") + reason); return 2; }
    void resetDraft() {
        if (!document_->field(titleId_)) return;
        draft_ = std::get<std::string>(document_->field(titleId_)->value[0]);
    }
    std::expected<void, LoadError> applyDraft() { return history_->setText(titleId_, draft_, "Mission name"); }
    static std::string longSmokeName() { return "Q3 long mission " + std::string(8192, 'x') + " retained tail"; }
    void showDocument() {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1100, 720));
        ImGui::Begin("Mission", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGui::Text("%s", options_.mission.string().c_str());
        ImGui::Text("Revision %llu | %s | undo bytes %zu | oldest commands evicted %zu",
            static_cast<unsigned long long>(document_->revision()), document_->modified() ? "Unsaved" : "Saved", history_->bytesUsed(), history_->evicted());
        ImGui::InputText("Stored mission name", &draft_);
        if (const auto* id = document_->find("MissionSettings", "MissionNameResourceStringID"))
            ImGui::Text("Localized name resource: %s", formatValue(*id, id->value).c_str());
        if (ImGui::Button("Apply name")) {
            auto result = applyDraft();
            status_ = result ? "Name applied" : result.error().reason;
        }
        ImGui::SameLine();
        if (ImGui::Button("Undo") && history_->undo()) resetDraft();
        ImGui::SameLine();
        if (ImGui::Button("Redo") && history_->redo()) resetDraft();
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            auto result = document_->save(options_.output.empty() ? options_.mission : options_.output, logger());
            status_ = result ? "Saved" : result.error().reason;
        }
        ImGui::TextUnformatted(status_.c_str());
        ImGui::Text("Terrain: %zu x %zu | Entities: %zu | Packets: %zu", document_->terrainSide(), document_->terrainSide(), document_->entities().size(), document_->archive().packets.size());
        ImGui::Separator();
        ImGui::BeginChild("Document fields");
        for (const auto& section : document_->syntax().sections) {
            ImGui::PushID(static_cast<int>(section.id.value));
            if (ImGui::TreeNode(section.name.c_str())) {
                for (auto id : section.fields) {
                    const auto* field = document_->field(id);
                    ImGui::Text("%s %s = %s", field->prefix.c_str(), field->name.c_str(), formatValue(*field, field->value).c_str());
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::End();
    }
    int smoke(unsigned frames) {
        if (!applyDraft() || draft_ != longSmokeName()) return error("long_name_apply_failed");
        if (!document_->save(options_.output / "long_name", logger())) return error("long_name_save_failed");
        auto longName = MissionDocument::read(options_.output / "long_name.fit", options_.output / "long_name.pak", logger());
        if (!longName || std::get<std::string>(longName->find("MissionSettings", "MissionName")->value[0]) != longSmokeName())
            return error("long_name_reopen_failed");
        if (!history_->undo()) return error("long_name_undo_failed");
        resetDraft();
        const auto original = document_->fitBytes();
        if (!document_->save(options_.output / "roundtrip", logger())) return error("roundtrip_save_failed");
        const std::string changed = "Q3 authored mission";
        if (!history_->setText(titleId_, changed, "Mission name")) return error("edit_failed");
        if (!document_->save(options_.output / "edited", logger())) return error("edit_save_failed");
        auto reopen = MissionDocument::read(options_.output / "edited.fit", options_.output / "edited.pak", logger());
        if (!reopen || std::get<std::string>(reopen->find("MissionSettings", "MissionName")->value[0]) != changed || reopen->fitBytes() == original)
            return error("edit_reopen_failed");
        if (!history_->undo() || document_->fitBytes() != original) return error("undo_failed");
        if (!document_->save(options_.output / "undone", logger())) return error("undo_save_failed");
        if (!history_->redo() || document_->fitBytes() != reopen->fitBytes() || !history_->undo()) return error("redo_failed");
        auto malformed = original;
        malformed.insert(malformed.size() / 2, 1, '\0');
        unsigned diagnostics{};
        auto rejected = MissionDocument::decode(std::move(malformed), document_->archive().originalBytes, "malformed.fit", [&](const std::string& line) { ++diagnostics; log(line); });
        if (rejected || diagnostics != 1 || document_->fitBytes() != original) return error("malformed_rejection_failed");
        log("[EDITOR_SHELL v1] result=PASS frames=" + std::to_string(frames) + " roundtrip=1 edited=1 reopened=1 undo=1 redo=1 malformed=1 unchanged=1 long_name=1");
        return 0;
    }
    Options options_;
    std::ofstream logFile_;
    std::optional<MissionDocument> document_;
    std::unique_ptr<CommandHistory> history_;
    NodeId titleId_;
    std::string draft_;
    std::string status_;
    SDL_Window* window_{};
    SDL_GLContext context_{};
    ImGuiContext* imgui_{};
    bool sdlReady_{}, platformReady_{}, rendererReady_{};
};
}
int main(int argc, char** argv)
{
    try { return Shell(options(argc, argv)).run(); }
    catch (const std::exception& error) { std::cerr << "[EDITOR_SHELL v1] result=FAIL reason=" << error.what() << '\n'; return 2; }
}
