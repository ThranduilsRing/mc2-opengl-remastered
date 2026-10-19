#include "CommandHistory.h"
#include "EntityPanels.h"
#include "FileWorkflow.h"
#include "ShellFrame.h"
#ifdef MC2_EDITOR_SMOKE_HOST
#include "PanelProof.h"
#include "FileProof.h"
#endif
#include "Gates/gate.h"
#include "build_fingerprint.h"

#include <SDL.h>
#include "../../tools/common/tool_vk_shell.h"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_sdl2.h>
#include <misc/cpp/imgui_stdlib.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace editor::document;

struct Options {
    std::filesystem::path mission, output, log, recent;
    ResolverConfig resolver{std::filesystem::current_path()};
    bool resolverRequested{};
#ifdef MC2_EDITOR_SMOKE_HOST
    bool smoke{};
    std::string panelSmoke;
    std::string fileSmoke;
#endif
};
Options options(int argc, char** argv)
{
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
#ifdef MC2_EDITOR_SMOKE_HOST
        if (key == "--smoke") result.smoke = true;
        else if (i + 1 < argc && key == "--panel-smoke") result.panelSmoke = argv[++i];
        else if (i + 1 < argc && key == "--file-smoke") result.fileSmoke = argv[++i];
        else
#endif
        if (i + 1 < argc && key == "--mission") result.mission = argv[++i];
        else if (i + 1 < argc && key == "--output") result.output = argv[++i];
        else if (i + 1 < argc && key == "--log") result.log = argv[++i];
        else if (i + 1 < argc && key == "--recent-file") result.recent = argv[++i];
        else if (i + 1 < argc && key == "--base") { result.resolver.base = argv[++i]; result.resolverRequested = true; }
        else if (i + 1 < argc && key == "--mods") { result.resolver.mods = argv[++i]; result.resolverRequested = true; }
        else if (i + 1 < argc && key == "--mod") { result.resolver.activeMod = argv[++i]; result.resolverRequested = true; }
        else throw std::runtime_error("invalid_shell_argument");
    }
    if (result.mission.empty()) throw std::runtime_error("mission_required");
#ifdef MC2_EDITOR_SMOKE_HOST
    if ((result.smoke || !result.panelSmoke.empty() || !result.fileSmoke.empty()) && (result.output.empty() || result.log.empty())) throw std::runtime_error("smoke_output_and_log_required");
#endif
    return result;
}

class Shell {
public:
    explicit Shell(Options options) : options_(std::move(options)), logFile_(options_.log, std::ios::trunc) {}
    ~Shell() {
        toolvk::shutdown(native_);
        if (platformReady_) ImGui_ImplSDL2_Shutdown();
        if (imgui_) ImGui::DestroyContext(imgui_);
        if (window_) SDL_DestroyWindow(window_);
        if (sdlReady_) SDL_Quit();
    }
    int run() {
        log(MC2_BUILD_FP_LITERAL);
        auto fit = options_.mission; fit.replace_extension(".fit");
        auto pak = options_.mission; pak.replace_extension(".pak");
        if (options_.resolverRequested || std::filesystem::exists(options_.resolver.base / "system.cfg")) {
            auto resolver = FileResolver::mount(options_.resolver);
            if (!resolver) return error(resolver.error().reason.c_str());
            resolver_ = std::move(*resolver);
            catalog_ = ObjectCatalog::load(*resolver_, logger(), resourceNames(*resolver_, logger()));
        }
        const auto markerObjects = catalog_ ? catalog_->navMarkerObjects() : std::vector<std::uint32_t>{};
        auto loaded = MissionDocument::read(fit, pak, logger(), markerObjects);
        if (!loaded) return 2;
        document_ = std::move(*loaded);
        history_ = std::make_unique<CommandHistory>(*document_, 32 * 1024 * 1024, logger());
        panels_ = std::make_unique<editor::shell::EntityPanels>(*document_, *history_);
        bool fixtures{};
#ifdef MC2_EDITOR_SMOKE_HOST
        fixtures = (!options_.panelSmoke.empty() && !options_.panelSmoke.starts_with("chooser")) || !options_.fileSmoke.empty();
#endif
        if (!fixtures && resolver_) {
            auto policy = PlacementPolicy::load(*resolver_, logger());
            if (!policy) return error(policy.error().reason.c_str());
            policy_ = std::move(*policy);
            panels_->placementPolicy(*catalog_, *policy_, logger());
        }
        files_ = std::make_unique<editor::shell::FileWorkflow>(*document_, *history_, *panels_,
            options_.output.empty() ? options_.mission : options_.output, logger(), [this] {
            if (const auto* title = document_->find("MissionSettings", "MissionName")) titleId_ = title->id;
            else titleId_ = {};
            resetDraft();
        }, options_.recent.empty() ? std::filesystem::current_path() / "editor-recent.txt" : options_.recent);
#ifdef MC2_EDITOR_SMOKE_HOST
        if (!options_.panelSmoke.empty()) {
            panelProof_ = std::make_unique<editor::shell::PanelProof>(*document_, *history_, *panels_, options_.output, options_.panelSmoke, logger(), true, [this](const std::filesystem::path& path) { return capturePanel(path); });
        }
#endif
        auto* title = document_->find("MissionSettings", "MissionName");
        if (title && title->value.size() == 1 && std::holds_alternative<std::string>(title->value[0])) titleId_ = title->id;
        resetDraft();
#ifdef MC2_EDITOR_SMOKE_HOST
        if (options_.smoke) {
            if (!history_->setText(titleId_, longSmokeName(), "Mission name")) return error("long_name_setup_failed");
            resetDraft();
        }
#endif
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
        // Harness runs never occupy the desktop or request pointer capture.
        const bool hidden = mc2::gate::get<"MC2_WINDOW_OFFSCREEN">() || toolvk::captureRequested()
#ifdef MC2_EDITOR_SMOKE_HOST
            || options_.smoke || !options_.panelSmoke.empty() || !options_.fileSmoke.empty()
#endif
            ;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return error("sdl_init_failed");
        sdlReady_ = true;
        window_ = SDL_CreateWindow("Mission document", hidden ? -16000 : SDL_WINDOWPOS_CENTERED,
            hidden ? -16000 : SDL_WINDOWPOS_CENTERED, 1440, 960, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
        if (!window_) return error("sdl_window_failed");
        imgui_ = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        platformReady_ = ImGui_ImplSDL2_InitForVulkan(window_);
        rendererReady_ = platformReady_ && toolvk::init(native_, window_, "Mission Editor Shell");
        native_.clear[0] = 0.06f; native_.clear[1] = 0.07f; native_.clear[2] = 0.09f; native_.clear[3] = 1.0f;
        if (!platformReady_ || !rendererReady_) return error("imgui_backend_failed");
#ifdef MC2_EDITOR_SMOKE_HOST
        if (!options_.fileSmoke.empty()) {
            fileProof_ = std::make_unique<editor::shell::FileProof>(*document_, *history_, *panels_, *files_,
                options_.output, options_.fileSmoke, logger(), [this] {
                    SDL_Event close{}; close.type = SDL_WINDOWEVENT; close.window.event = SDL_WINDOWEVENT_CLOSE;
                    close.window.windowID = SDL_GetWindowID(window_); SDL_PushEvent(&close);
                });
        }
#endif
        bool running = true;
#ifdef MC2_EDITOR_SMOKE_HOST
        unsigned frames{};
#endif
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                // ImGui's SDL backend can capture the mouse; off-screen pumps do not enable input.
                if (!hidden) ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT &&
                    event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window_)))
                    files_->request(editor::shell::FileWorkflow::Action::Close);
            }
            if (!toolvk::begin(native_)) { SDL_Delay(10); continue; }
            ImGui_ImplVulkan_NewFrame();
            if (!hidden) ImGui_ImplSDL2_NewFrame();
            else { ImGui::GetIO().DisplaySize = ImVec2(1440, 960); ImGui::GetIO().DeltaTime = 1.0f / 60.0f; }
            ImGui::NewFrame();
            running = editor::shell::drawWhileOpen(*files_, [this] { showDocument(); }, [this] { panels_->draw(); });
            ImGui::Render();
#ifdef MC2_EDITOR_SMOKE_HOST
            if (panelProof_) { const bool more = panelProof_->tick(); running = running && more; }
            if (fileProof_) { const bool more = fileProof_->tick(); running = running && more; }
#endif
            const bool captured = toolvk::end(native_);
            if (native_.submitFailed) return error("native_submit_failed");
#ifdef MC2_EDITOR_SMOKE_HOST
            if (panelCaptureQueued_ && captured) {
                if (!native_.captureSucceeded) return error("native_panel_capture_failed");
                panelCaptureDone_ = true;
            } else
#endif
            if (captured) running = false;
#ifdef MC2_EDITOR_SMOKE_HOST
            if (options_.smoke && ++frames == 3) running = false;
            if (!options_.smoke)
#endif
                SDL_Delay(10);
        }
        if (toolvk::validationErrors() != 0) return error("native_validation_failed");
#ifdef MC2_EDITOR_SMOKE_HOST
        if (options_.smoke) return smoke(frames);
        if (panelProof_) return panelProof_->passed() ? 0 : error("panel_proof_interrupted");
        if (fileProof_) { fileProof_->finish(files_->closing()); return fileProof_->passed() ? 0 : error("file_proof_interrupted"); }
#endif
        return 0;
    }
private:
#ifdef MC2_EDITOR_SMOKE_HOST
    bool capturePanel(const std::filesystem::path& path) {
        if (panelCaptureDone_) return true;
        if (!panelCaptureQueued_) {
            native_.capturePath = path.string();
            native_.captureFrame = native_.frameNo + 1;
            panelCaptureQueued_ = true;
        }
        return false;
    }
#endif
    Diagnostic logger() { return [this](const std::string& line) { log(line); }; }
    void log(const std::string& line) { if (logFile_) { logFile_ << line << '\n'; logFile_.flush(); } else std::cerr << line << '\n'; }
    int error(const char* reason) { log(std::string("[EDITOR_SHELL v1] result=FAIL reason=") + reason); return 2; }
    void resetDraft() {
        draft_.clear();
        if (!document_->field(titleId_)) return;
        draft_ = std::get<std::string>(document_->field(titleId_)->value[0]);
    }
    std::expected<void, LoadError> applyDraft() { return history_->setText(titleId_, draft_, "Mission name"); }
#ifdef MC2_EDITOR_SMOKE_HOST
    static std::string longSmokeName() { return "Q3 long mission " + std::string(8192, 'x') + " retained tail"; }
#endif
    void showDocument() {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1440, 230));
        ImGui::Begin("Mission", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
#ifdef MC2_EDITOR_SMOKE_HOST
        if (panelProof_) panelProof_->drawFiles(); else
#endif
        files_->draw();
        if (files_->closing()) { ImGui::End(); return; }
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
        ImGui::TextUnformatted(status_.c_str());
        ImGui::Text("Terrain: %zu x %zu | Entities: %zu | Packets: %zu", document_->terrainSide(), document_->terrainSide(), document_->entities().size(), document_->archive().packets.size());
        ImGui::End();
    }
#ifdef MC2_EDITOR_SMOKE_HOST
    int smoke(unsigned frames) {
        if (!applyDraft() || draft_ != longSmokeName()) return error("long_name_apply_failed");
        if (!document_->save(options_.output / "long_name", logger())) return error("long_name_save_failed");
        auto longName = MissionDocument::read(options_.output / "long_name.fit", options_.output / "long_name.pak", logger(), document_->markerObjects());
        if (!longName || std::get<std::string>(longName->find("MissionSettings", "MissionName")->value[0]) != longSmokeName())
            return error("long_name_reopen_failed");
        if (!history_->undo()) return error("long_name_undo_failed");
        resetDraft();
        const auto original = document_->fitBytes();
        if (!document_->save(options_.output / "roundtrip", logger())) return error("roundtrip_save_failed");
        const std::string changed = "Q3 authored mission";
        if (!history_->setText(titleId_, changed, "Mission name")) return error("edit_failed");
        if (!document_->save(options_.output / "edited", logger())) return error("edit_save_failed");
        auto reopen = MissionDocument::read(options_.output / "edited.fit", options_.output / "edited.pak", logger(), document_->markerObjects());
        if (!reopen || std::get<std::string>(reopen->find("MissionSettings", "MissionName")->value[0]) != changed || reopen->fitBytes() == original)
            return error("edit_reopen_failed");
        if (!history_->undo() || document_->fitBytes() != original) return error("undo_failed");
        if (!document_->save(options_.output / "undone", logger())) return error("undo_save_failed");
        if (!history_->redo() || document_->fitBytes() != reopen->fitBytes() || !history_->undo()) return error("redo_failed");
        auto malformed = original;
        malformed.insert(malformed.size() / 2, 1, '\0');
        unsigned diagnostics{};
        auto rejected = history_->replace(std::move(malformed), document_->archive().originalBytes, "malformed.fit", [&](const std::string& line) { ++diagnostics; log(line); });
        if (rejected || diagnostics != 1 || document_->fitBytes() != original) return error("malformed_rejection_failed");
        log("[EDITOR_SHELL v1] result=PASS frames=" + std::to_string(frames) + " roundtrip=1 edited=1 reopened=1 undo=1 redo=1 malformed=1 unchanged=1 long_name=1");
        return 0;
    }
#endif
    Options options_;
    std::ofstream logFile_;
    std::optional<MissionDocument> document_;
    std::optional<FileResolver> resolver_;
    std::optional<ObjectCatalog> catalog_;
    std::optional<PlacementPolicy> policy_;
    std::unique_ptr<CommandHistory> history_;
    std::unique_ptr<editor::shell::EntityPanels> panels_;
    std::unique_ptr<editor::shell::FileWorkflow> files_;
#ifdef MC2_EDITOR_SMOKE_HOST
    std::unique_ptr<editor::shell::PanelProof> panelProof_;
    std::unique_ptr<editor::shell::FileProof> fileProof_;
#endif
    NodeId titleId_;
    std::string draft_;
    std::string status_;
    SDL_Window* window_{};
    toolvk::Shell native_{};
#ifdef MC2_EDITOR_SMOKE_HOST
    bool panelCaptureQueued_{}, panelCaptureDone_{};
#endif
    ImGuiContext* imgui_{};
    bool sdlReady_{}, platformReady_{}, rendererReady_{};
};
}
int main(int argc, char** argv)
{
    try { return Shell(options(argc, argv)).run(); }
    catch (const std::exception& error) { std::cerr << "[EDITOR_SHELL v1] result=FAIL reason=" << error.what() << '\n'; return 2; }
}
