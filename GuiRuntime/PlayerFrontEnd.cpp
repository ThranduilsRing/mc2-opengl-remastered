#include "PlayerFrontEnd.h"
#include "GuiRuntime_internal.h"
#include "PlayerOptionRows.h"
#include "PlayerSettings.h"
#include <map>
#include "GraphicsOptionsWindow.h"
#include "GuiRuntime_internal.h"
#include "imgui.h"
#include "gameos.hpp"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "Gates/render_options_cfg.h"
#include "Gates/render_options_legacy_env.h"
#include "../3rdparty/include/nlohmann/json.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <chrono>
#include <iterator>
#include <optional>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace PlayerFrontEnd {
namespace fs = std::filesystem;
using Json = nlohmann::json;
bool (*saveRenderOptions)(const mc2::options::RenderOptions&) = nullptr;
namespace {
struct Pack {
    std::string id, name, description, credits, author, kind, group;
    Json env = Json::object();
    std::vector<std::string> dependencies;
    bool enabled = false;
    std::string problem;
    std::vector<std::string> missions;
    std::map<std::string, CampaignInfo> campaigns;
};
Json settings = Json::object();
std::vector<Pack> packs;
std::string campaign, base, texture, campaignFit, pendingStart, pendingMission, error;
bool pickerOpen = false, scanned = false, restart = false, settingsChanged = false;
std::map<std::string, std::optional<std::string>> inheritedEnv;

std::future<int> importJob;
bool importStarted = false;
std::string importError;
Json importStatus = Json::object();

void startImport() {
    importStarted = true;
    importError.clear();
    importStatus = Json::object();
    // The converter runs separately: filesystem work never blocks the menu thread.
    importJob = std::async(std::launch::async, [] {
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const auto log = CreateFileW(L"campaign-auto.log", GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
        const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
        startup.hStdInput = input;
        PROCESS_INFORMATION process{};
        std::wstring command = L"py -3 tools/import_campaign.py --auto --deploy .";
        // background-launch: trusted campaign converter, no installer execution or window.
        const bool launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
            DETACHED_PROCESS | IDLE_PRIORITY_CLASS, nullptr, nullptr, &startup, &process) != 0;
        const auto launchError = GetLastError();
        CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (!launched) return static_cast<int>(launchError);
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        return static_cast<int>(code);
#else
        const auto child = fork();
        if (child == 0) {
            execlp("python3", "python3", "tools/import_campaign.py", "--auto", "--deploy", ".", static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    });
}

std::optional<std::string> env(std::string_view key) {
    const char* value = mc2::gate::envText(std::string(key).c_str());
    return value ? std::optional<std::string>(value) : std::nullopt;
}
void setEnv(std::string_view key, std::string_view value) {
    inheritedEnv.try_emplace(std::string(key), env(key));
#ifdef _WIN32
    _putenv_s(std::string(key).c_str(), std::string(value).c_str());
#else
    if (value.empty()) unsetenv(std::string(key).c_str());
    else setenv(std::string(key).c_str(), std::string(value).c_str(), 1);
#endif
}
Json readJson(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream) return Json::object();
    auto result = Json::parse(stream, nullptr, false);
    return result.is_object() ? result : Json::object();
}
std::string text(const Json& json, std::string_view key, std::string_view fallback = {}) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
}
bool saveSettings() {
    std::ofstream stream("launcher_env.json.tmp", std::ios::binary | std::ios::trunc);
    stream << settings.dump(2) << '\n';
    stream.close();
    if (!stream) { error = "Could not save settings."; return false; }
#ifdef _WIN32
    const bool ok = MoveFileExA("launcher_env.json.tmp", "launcher_env.json", MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename("launcher_env.json.tmp", "launcher_env.json", ec);
    const bool ok = !ec;
#endif
    if (!ok) error = "Could not replace launcher_env.json.";
    return ok;
}
std::vector<fs::path> files(const fs::path& path) {
    std::vector<fs::path> result;
    std::error_code ec;
    for (fs::directory_iterator i(path, ec), end; !ec && i != end; i.increment(ec))
        result.push_back(i->path());
    std::sort(result.begin(), result.end());
    return result;
}
std::vector<std::string> campaigns(std::string_view id) {
    std::vector<std::string> result;
    for (const auto& pack : packs) if (pack.id == id && pack.kind == "assets") return {"campaign"};
    const fs::path root = id.empty() ? fs::path("data") : fs::path("mods") / id / "data";
    for (const auto& file : files(root / "campaign"))
        if (file.extension() == ".fit" || file.extension() == ".FIT") result.push_back(file.stem().string());
    // Stock campaign.fit is shipped in the FastFiles, invisible to directory iteration.
    if (id.empty() && std::find(result.begin(), result.end(), "campaign") == result.end()) result.insert(result.begin(), "campaign");
    return result;
}
void scan() {
    packs.clear();
    campaign = env(mc2::gate::name<"MC2_ACTIVE_MOD">()).value_or("");
    base = text(settings, "last_base");
    texture = text(settings, "tex_pack_mod");
    const auto deps = env(mc2::gate::name<"MC2_MOD_DEPS">()).value_or("");
    for (const auto& dir : files("mods")) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        const auto id = dir.filename().string();
        if (id.empty() || id[0] == '.') continue;
        std::ifstream manifest(dir / "mod.json");
        const bool manifestExists = manifest.is_open();
        const auto json = manifestExists ? Json::parse(manifest, nullptr, false) : Json::object();
        Pack pack;
        pack.id = id;
        bool archives = false;
        for (const auto& file : files(dir)) archives |= file.extension() == ".fst";
        pack.problem = packProblem(json, manifestExists, fs::is_directory(dir / "data", ec), archives, id);
        for (const auto& file : files(dir / "data" / "missions")) {
            auto ext = file.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".fit") continue;
            std::ifstream input(file);
            const std::string source((std::istreambuf_iterator<char>(input)), {});
            if (source.find("[MissionSettings]") != std::string::npos) pack.missions.push_back(file.stem().string());
        }
        for (const auto& file : files(dir / "data" / "campaign")) {
            if (file.extension() != ".fit" && file.extension() != ".FIT") continue;
            std::ifstream input(file);
            const std::string source((std::istreambuf_iterator<char>(input)), {});
            pack.campaigns.emplace(file.stem().string(), campaignInfo(source));
        }
        std::string fitTitle;
        if (!pack.campaigns.empty()) {
            const auto& [fit, info] = *pack.campaigns.begin();
            fitTitle = info.title;
        }
        pack.name = campaignTitle(text(json, "name"), fitTitle, id);
        pack.description = text(json, "description");
        pack.credits = text(json, "credits");
        pack.author = text(json, "author");
        pack.kind = text(json, "type");
        pack.group = text(json, "radioGroup");
        if (json.contains("env") && json["env"].is_object()) pack.env = json["env"];
        pack.kind = packKind(pack.kind, id, !pack.campaigns.empty(), !pack.missions.empty());
        if (pack.problem.empty() && pack.kind == "campaign" && pack.campaigns.empty() && pack.missions.empty())
            pack.problem = "No campaign FIT or playable mission FIT found; import the complete campaign.";
        if (pack.problem.empty() && json.contains("dependencies")) for (const auto& dep : json["dependencies"]) {
            pack.dependencies.push_back(dep.get<std::string>());
            if (!fs::is_directory(fs::path("mods") / dep.get<std::string>() / "data", ec)) {
                pack.problem = "Missing dependency: " + dep.get<std::string>() + "; import its compatibility or add-on pack first.";
                break;
            }
        }
        pack.enabled = contains(deps, id);
        if (pack.enabled && pack.kind == "dependency") base = id;
        if (pack.enabled && pack.group == "texture-quality") texture = id;
        packs.push_back(std::move(pack));
    }
    std::stable_sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) {
        return (a.kind == "campaign") > (b.kind == "campaign");
    });
    const auto fits = campaigns(campaign);
    campaignFit = fits.empty() ? "" : fits.front();
    scanned = true;
}
void autoBase() {
    if (campaign.empty()) { base.clear(); return; }
    for (const auto& pack : packs) if (pack.id == campaign && !pack.dependencies.empty()) {
        for (const auto& dep : pack.dependencies) if (dep == "mco-compat" || dep == "mc2x-compat") { base = dep; return; }
    }
    for (const auto& pack : packs) if (pack.id == campaign && pack.kind == "assets") { base.clear(); return; }
    bool high = false, magic = false;
    const auto missions = fs::path("mods") / campaign / "data" / "missions";
    for (const auto& dir : {missions, missions / "warriors"}) {
        for (const auto& file : files(dir)) {
            const auto ext = file.extension().string();
            if (ext != ".fit" && ext != ".abl" && ext != ".abi") continue;
            std::ifstream input(file, std::ios::binary);
            std::string source(262144, '\0');
            input.read(source.data(), source.size());
            source.resize(static_cast<std::size_t>(input.gcount()));
            std::transform(source.begin(), source.end(), source.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            magic |= source.find("magicattack") != std::string::npos;
            high |= hasHighObjectNumber(source);
        }
    }
    base = magic ? "mco-compat" : high ? "mc2x-compat" : "";
}
std::string dependencies() {
    return pickerDependencies(packs, campaign, base, texture);
}
void migrateSettings() {
    auto cfg = mc2::options::cfg::readTextFile("options.cfg");
    const auto original = cfg;
    auto migrated = settings;
    if (original.empty()) return;
    migrateLegacySettings(migrated, cfg);
    if (migrated == settings || original.empty()) return;
    if (cfg != original) {
        std::ofstream stream("options.cfg.tmp", std::ios::binary | std::ios::trunc);
        stream << cfg;
        stream.close();
        if (!stream) { error = "Could not migrate options.cfg."; return; }
#ifdef _WIN32
        if (!MoveFileExA("options.cfg.tmp", "options.cfg", MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
#else
        std::error_code ec;
        fs::rename("options.cfg.tmp", "options.cfg", ec);
        if (ec) return;
#endif
    }
    const auto previous = settings;
    settings = std::move(migrated);
    if (!saveSettings()) settings = previous;
    else if (env(mc2::gate::name<"MC2_LOG">())) std::printf("[PLAYER_SETTINGS] migrated legacy player settings\n");
}
}

void loadSettings() {
    // Keep shell/harness overrides. Existing installs retain the launcher's file.
    settings = readJson("launcher_env.json");
    if (!mc2::gate::get<"MC2_LAUNCHER_ENV_JSON">() || mc2::gate::isSet<"MC2_LAUNCHED">()) return;
    migrateSettings();
    const bool explicitMod = env(mc2::gate::name<"MC2_ACTIVE_MOD">()).has_value();
    for (const auto& [key, value] : settings.items())
        if (key.starts_with("MC2_") && mc2::gate::lookupName(key) && value.is_string() && !env(key)) setEnv(key, value.get<std::string>());
    if (!explicitMod) {
        setEnv(mc2::gate::name<"MC2_ACTIVE_MOD">(), text(settings, "last_campaign"));
        const auto deps = savedDependencies(settings);
        if (!env(mc2::gate::name<"MC2_MOD_DEPS">())) setEnv(mc2::gate::name<"MC2_MOD_DEPS">(), deps);
    }
    const bool faster = text(settings, "last_faster_weapons") == "1";
    const bool generated = text(settings, "last_data_driven") == "1";
    for (const auto& [key, value] : {std::pair{mc2::gate::name<"MC2_DIRECT_FIRE_STRAIGHT">(), faster ? "1" : ""},
         {mc2::gate::name<"MC2_PROJECTILE_SPEED_MULT">(), faster ? "2" : ""}, {mc2::gate::name<"MC2_UNIT_PROFILE_DATA">(), generated ? "1" : ""},
         {mc2::gate::name<"MC2_UNIT_DEFINITION_SOURCE">(), generated ? "generated" : ""}, {mc2::gate::name<"MC2_EQUIPMENT_DEFINITION_SOURCE">(), generated ? "generated" : ""}})
        if (!env(key)) setEnv(key, value);
    // Apply packs in VFS precedence order; only original shell overrides survive.
    std::vector<std::string> activePacks;
    const auto deps = env(mc2::gate::name<"MC2_MOD_DEPS">()).value_or("");
    for (std::size_t start = 0; start < deps.size();) {
        const auto end = deps.find_first_of(",;", start);
        const auto id = deps.substr(start, end - start);
        if (std::find(activePacks.begin(), activePacks.end(), id) == activePacks.end()) activePacks.push_back(id);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    activePacks.push_back(env(mc2::gate::name<"MC2_ACTIVE_MOD">()).value_or(""));
    for (const auto& id : activePacks) {
        if (id.empty() || (id != activePacks.back() && !contains(deps, id))) continue;
        const auto json = readJson(fs::path("mods") / id / "mod.json");
        if (!json.contains("env") || !json["env"].is_object()) continue;
        for (const auto& [key, value] : json["env"].items()) {
            const auto original = inheritedEnv.find(key);
            bool shellOverride;
            if (original != inheritedEnv.end()) {
                const auto& [name, inherited] = *original;
                shellOverride = inherited.has_value();
            } else shellOverride = env(key).has_value();
            if (key.starts_with("MC2_") && value.is_string() && !shellOverride) setEnv(key, value.get<std::string>());
        }
    }
    pendingStart = text(settings, "start_campaign");
    if (!pendingStart.empty()) { settings.erase("start_campaign"); saveSettings(); }
    pendingMission = text(settings, "start_mission");
    if (!pendingMission.empty()) { settings.erase("start_mission"); saveSettings(); }
    mc2::gate::invalidateAll();
}

void requestCampaignStart(std::string fit) { pendingStart = std::move(fit); }
std::string takeCampaignStart() { return std::exchange(pendingStart, {}); }
std::string takeMissionStart() { return std::exchange(pendingMission, {}); }

static void drawCommunityCredits() {
    const auto* vp = ImGui::GetMainViewport();
    const float scale = std::clamp(vp->WorkSize.y / 1080.0f, 0.75f, 2.0f);
    ImGui::PushFont(gui_runtime_detail::findUiFont("roboto", static_cast<int>(18 * scale)));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.715f, vp->WorkPos.y + 280 * scale), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.27f, vp->WorkSize.y - 320 * scale), ImGuiCond_Always);
    if (ImGui::Begin("Community", nullptr, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("Celebrating 25 years of MechCommander 2");
        ImGui::Separator();
        ImGui::TextUnformatted("For a quarter of a century, players and creators have kept the dropships flying. "
            "Every mission shared, tool built and helping hand has given this game another home. Thank you for keeping MC2 alive.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Two individuals in particular have kept this game alive for 25+ years:");
        const auto openURL = ImGui::GetPlatformIO().Platform_OpenInShellFn;
        ImGui::GetPlatformIO().Platform_OpenInShellFn = [](ImGuiContext*, auto url) { return SDL_OpenURL(url) == 0; };
        ImGui::TextLinkOpenURL("Wolfman", "https://mc2x.net/");
        ImGui::SameLine(); ImGui::TextUnformatted("and Magic,");
        ImGui::TextUnformatted("supported by many, many others.");
        ImGui::Spacing();
        ImGui::TextUnformatted("A special thank-you to alariq for the OpenGL/Linux port that this project builds on.");
        ImGui::TextLinkOpenURL("alariq's OpenGL/Linux port", "https://github.com/alariq/mc2");
        ImGui::GetPlatformIO().Platform_OpenInShellFn = openURL;
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Credits", ImGuiTreeNodeFlags_DefaultOpen)) {
            static const std::string credits = [] {
                std::ifstream stream("CREDITS.txt", std::ios::binary);
                return stream ? std::string(std::istreambuf_iterator<char>(stream), {}) : std::string("CREDITS.txt is missing from this installation.");
            }();
            if (ImGui::BeginChild("##credits", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders)) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(credits.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
        }
        ImGui::TextUnformatted("Free forever.");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopFont();
}

void drawMainMenu(bool showCredits) {
    if (!ImGui::GetCurrentContext()) return;
    if (showCredits) drawCommunityCredits();
    static bool bootScreen = false;
    if (!bootScreen) {
        const auto screen = mc2::gate::str<"MC2_BOOT_TO_SCREEN">();
        pickerOpen = screen == "campaigns";
        if (screen == "graphics") GraphicsOptionsWindow::setOpen(true);
        bootScreen = true;
    }
    const auto* viewport = ImGui::GetMainViewport();
    const float toolsScale = std::clamp(viewport->WorkSize.y / 1080.0f, 1.0f, 4.0f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 10 * toolsScale,
        viewport->WorkPos.y + 25 * toolsScale), ImGuiCond_Always, ImVec2(1, 0));
    if (!importStarted) startImport();
    static Uint32 nextImportRead = 0;
    if (importJob.valid() && SDL_GetTicks() >= nextImportRead) {
        nextImportRead = SDL_GetTicks() + 250;
        importStatus = readJson("mods/.campaign-auto.json");
        if (importJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto code = importJob.get();
            if (code) {
                std::ifstream log("campaign-auto.log");
                const std::string detail((std::istreambuf_iterator<char>(log)), {});
                importError = "Automatic campaign import failed (code " + std::to_string(code) + "; Python 3 required): " + detail.substr(detail.size() > 2000 ? detail.size() - 2000 : 0);
            }
            importStatus = readJson("mods/.campaign-auto.json");
            scan();
        }
    }
    ImGui::Begin("Campaign tools", nullptr, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Campaigns & mods")) pickerOpen = !pickerOpen;
    ImGui::SameLine();
    if (ImGui::Button("Graphics")) GraphicsOptionsWindow::setOpen(true);
    ImGui::End();
    if (!pickerOpen) return;
    if (!scanned) scan();
    const float scale = std::clamp(viewport->WorkSize.y / 1080.0f, 0.75f, 2.0f);
    ImGui::PushFont(gui_runtime_detail::findUiFont("agency", static_cast<int>(22 * scale)));
    const ImVec4 amber(0.94f, 0.66f, 0.24f, 1), dark(0.06f, 0.055f, 0.04f, 1), hover(0.34f, 0.24f, 0.10f, 1);
    for (const auto col : {ImGuiCol_Text, ImGuiCol_CheckMark, ImGuiCol_SliderGrab, ImGuiCol_Border}) ImGui::PushStyleColor(col, amber);
    for (const auto col : {ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg, ImGuiCol_FrameBg, ImGuiCol_TitleBg, ImGuiCol_TitleBgActive}) ImGui::PushStyleColor(col, dark);
    for (const auto col : {ImGuiCol_Button, ImGuiCol_Header, ImGuiCol_FrameBgHovered, ImGuiCol_ButtonHovered, ImGuiCol_HeaderHovered, ImGuiCol_FrameBgActive, ImGuiCol_ButtonActive, ImGuiCol_HeaderActive}) ImGui::PushStyleColor(col, hover);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * scale, 18 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(14 * scale, 10 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 6 * scale));
    auto popPickerStyle = [] { ImGui::PopStyleVar(3); ImGui::PopStyleColor(18); ImGui::PopFont(); };
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(900 * scale, 0), ImVec2(900 * scale, viewport->WorkSize.y * 0.85f));
    if (!ImGui::Begin("Campaigns & mods", &pickerOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) { ImGui::End(); popPickerStyle(); return; }
    if (ImGui::Button("Refresh") && !importJob.valid()) { scan(); startImport(); }
    ImGui::SameLine();
    ImGui::TextUnformatted("Installed campaigns, missions and add-ons");
    if (importJob.valid()) ImGui::TextWrapped("%s", text(importStatus, "message", "Starting automatic campaign import...").c_str());
    if (!importError.empty()) ImGui::TextWrapped("%s", importError.c_str());
    if (importStatus.contains("sources")) for (const auto& [source, result] : importStatus["sources"].items())
        if (result.contains("error")) ImGui::TextWrapped("%s: %s", source.c_str(), text(result, "error").c_str());
    auto select = [&](std::string_view id) {
        campaign = id;
        autoBase();
        const auto fits = campaigns(id);
        campaignFit = fits.empty() ? "" : fits.front();
    };
    ImGui::BeginTable("##campaignLayout", 2, ImGuiTableFlags_SizingStretchProp);
    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthFixed, 300 * scale);
    ImGui::TableSetupColumn("Details");
    ImGui::TableNextColumn();
    ImGui::SeparatorText("INSTALLED MODS");
    if (ImGui::BeginListBox("##packs", ImVec2(-1, 375 * scale))) {
        if (ImGui::Selectable("Stock campaign", campaign.empty())) select("");
        for (const auto& pack : packs) {
            if (importStatus.contains("sources") && importStatus["sources"].contains(pack.id) &&
                !importStatus["sources"][pack.id].contains("error")) continue;
            ImGui::PushID(pack.id.c_str());
            if (pack.kind == "campaign" && !pack.campaigns.empty()) {
                for (const auto& [fit, info] : pack.campaigns) {
                    const auto title = campaignTitle("", info.title, fit);
                    if (ImGui::Selectable(title.c_str(), campaign == pack.id && campaignFit == fit)) { select(pack.id); campaignFit = fit; }
                }
            } else if (ImGui::Selectable(pack.name.c_str(), campaign == pack.id)) select(pack.id);
            if (!pack.problem.empty()) ImGui::TextWrapped("%s", pack.problem.c_str());
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
    ImGui::TableNextColumn();
    if (campaign.empty()) { ImGui::SeparatorText("MechCommander 2"); ImGui::TextWrapped("The original campaign. Command your mercenary company through the Inner Sphere."); }
    for (const auto& pack : packs) if (pack.id == campaign) {
        ImGui::SeparatorText(pack.name.c_str());
        ImGui::TextWrapped("%s", pack.description.c_str());
        if (!pack.author.empty()) ImGui::TextWrapped("Author: %s", pack.author.c_str());
        if (!pack.credits.empty()) ImGui::TextWrapped("Credits: %s", pack.credits.c_str());
        const auto fit = pack.campaigns.find(campaignFit);
        if (fit != pack.campaigns.end()) {
            const auto& [name, info] = *fit;
            if (info.missions) ImGui::Text("Missions: %d", info.missions);
        }
        ImGui::TextWrapped("Folder: %s (%s)", pack.id.c_str(), pack.kind.c_str());
        if (pack.kind == "assets") ImGui::TextWrapped("Start the stock campaign with this add-on, or enable it below alongside another campaign.");
        if (!pack.problem.empty()) ImGui::TextWrapped("%s", pack.problem.c_str());
    }
    if (!base.empty()) ImGui::TextWrapped("Required base: %s", base.c_str());
    if (!texture.empty()) ImGui::TextWrapped("Texture pack: %s", texture.c_str());
    std::string selectedTitle = campaignFit;
    for (const auto& pack : packs) if (pack.id == campaign && pack.campaigns.contains(campaignFit))
        selectedTitle = campaignTitle("", pack.campaigns.at(campaignFit).title, campaignFit);
    ImGui::SeparatorText("CAMPAIGN SETUP");
    ImGui::PushItemWidth(235 * scale);
    if (ImGui::BeginCombo("Campaign", selectedTitle.c_str())) {
        for (const auto& fit : campaigns(campaign)) {
            std::string label = fit;
            for (const auto& pack : packs) if (pack.id == campaign && pack.campaigns.contains(fit))
                label = campaignTitle("", pack.campaigns.at(fit).title, fit);
            ImGui::PushID(fit.c_str());
            if (ImGui::Selectable(label.c_str(), campaignFit == fit)) campaignFit = fit;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Compatibility base", base.empty() ? "None" : base.c_str())) {
        if (ImGui::Selectable("None", base.empty())) base.clear();
        for (const auto& pack : packs) if (pack.kind == "dependency")
            if (ImGui::Selectable(pack.name.c_str(), base == pack.id)) base = pack.id;
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Texture pack", texture.empty() ? "None" : texture.c_str())) {
        if (ImGui::Selectable("None", texture.empty())) texture.clear();
        for (const auto& pack : packs) if (pack.group == "texture-quality")
            if (ImGui::Selectable(pack.name.c_str(), texture == pack.id)) texture = pack.id;
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    if (ImGui::TreeNodeEx("Add-ons", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (auto& pack : packs) if (pack.kind != "campaign" && pack.kind != "dependency" && pack.group != "texture-quality")
        {
            ImGui::BeginDisabled(!packToggleAllowed(pack.enabled, pack.problem));
            ImGui::Checkbox(pack.name.c_str(), &pack.enabled);
            ImGui::EndDisabled();
        }
        ImGui::TreePop();
    }
    ImGui::EndTable();
    ImGui::Separator();
    const auto deps = dependencies();
    const bool contentChanged = settingsChanged || campaign != env(mc2::gate::name<"MC2_ACTIVE_MOD">()).value_or("") || deps != env(mc2::gate::name<"MC2_MOD_DEPS">()).value_or("");
    const bool missingBase = !base.empty() && std::none_of(packs.begin(), packs.end(), [](const Pack& p) { return p.id == base; });
    if (missingBase) ImGui::TextWrapped("Required base %s is missing. Import the compatibility pack first.", base.c_str());
    std::string problem;
    const Pack* selected = nullptr;
    for (const auto& pack : packs) {
        if (pack.id == campaign) selected = &pack;
        if (packSelected(pack.id, campaign, deps) && !pack.problem.empty()) problem = pack.problem;
    }
    const bool single = selected && campaignFit.empty() && !selected->missions.empty();
    static std::string mission;
    if (single) {
        if (std::find(selected->missions.begin(), selected->missions.end(), mission) == selected->missions.end()) mission = selected->missions.front();
        if (ImGui::BeginCombo("Single mission", mission.c_str())) {
            for (const auto& name : selected->missions) if (ImGui::Selectable(name.c_str(), mission == name)) mission = name;
            ImGui::EndCombo();
        }
    }
    if (!problem.empty()) ImGui::TextWrapped("%s", problem.c_str());
    else if (campaignFit.empty() && !single) ImGui::TextUnformatted("No campaign or playable mission found; compatibility packs are used with a campaign.");
    ImGui::BeginDisabled(importJob.valid() || (campaignFit.empty() && !single) || missingBase || !problem.empty());
    const char* startLabel = single ? "Start single mission" : selected && selected->kind == "assets" ? "Start stock campaign with add-on" : "Start campaign";
    if (ImGui::Button(startLabel)) {
        settings["last_campaign"] = campaign;
        settings["last_base"] = base;
        settings["tex_pack_mod"] = texture;
        settings["last_addons"] = deps;
        if (contentChanged) settings[single ? "start_mission" : "start_campaign"] = single ? mission : campaignFit;
        if (saveSettings()) {
            if (contentChanged) {
                // Selecting content in the picker supersedes a startup mod override.
                inheritedEnv[mc2::gate::name<"MC2_ACTIVE_MOD">()] = std::nullopt;
                inheritedEnv[mc2::gate::name<"MC2_MOD_DEPS">()] = std::nullopt;
                restart = true;
                gos_TerminateApplication();
            } else if (single) pendingMission = mission;
            else pendingStart = campaignFit;
            std::printf("[CAMPAIGN_PICKER] start mod=%s campaign=%s deps=%s restart=%d\n", campaign.c_str(), campaignFit.c_str(), deps.c_str(), static_cast<int>(contentChanged));
            pickerOpen = false;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::TreeNode("Import community campaigns")) {
        ImGui::TextWrapped("Import a downloaded ZIP, extracted installer or legacy install folder. Import the full Omnitech or MC2X base before its campaign add-ons, then Refresh here.");
        ImGui::TextUnformatted("py -3 tools/import_campaign.py \"SOURCE\" --deploy .");
        if (selected) {
            const auto command = "py -3 tools/import_campaign.py \"" + fs::absolute(fs::path("mods") / selected->id).string() + "\" --deploy .";
            if (ImGui::Button("Copy import command for selected folder")) ImGui::SetClipboardText(command.c_str());
        }
        ImGui::TextWrapped("Run the command from this game folder. Requires Python 3. On Linux replace py -3 with python3. Source files are preserved. For installer EXEs, extract into a separate source folder first.");
        ImGui::TreePop();
    }
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    ImGui::End();
    popPickerStyle();
}

void drawOptions() {
    static mc2::options::RenderOptions draft;
    static std::string loadedCfg;
    static int lastFrame = -2;
    if (!ImGui::CollapsingHeader("Player settings (saved; restart required)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto cfg = mc2::options::cfg::readTextFile("options.cfg");
    if (lastFrame != ImGui::GetFrameCount() - 1 || ImGui::IsWindowAppearing() || cfg != loadedCfg) {
        draft = {};
        mc2::options::cfg::loadFromText(draft, cfg);
        loadedCfg = cfg;
    }
    lastFrame = ImGui::GetFrameCount();
    for (const auto& preset : k_gfxPresets) {
        if (ImGui::Button(preset.label)) {
            preset.apply(draft);
            for (int i = 0; i < 6; ++i) settings[k_presetKeys[i]] = preset.values[i];
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    static constexpr int shadowMB[] = {27, 107, 164};
    ImGui::Text("Approximate fixed VRAM: %d MB (excludes mission textures/geometry)",
        256 + 96 + 8 + shadowMB[std::clamp(draft.shadowQuality, 0, 2)]);
    for (const auto& option : mc2::options::kOptions) {
        const char* label = std::string_view(option.key) == "RemasteredBuildings" ? "Remastered buildings (off: Classic)" : option.key;
        if (option.kind == mc2::options::Kind::Bool) ImGui::Checkbox(label, &(draft.*option.b));
        else if (option.kind == mc2::options::Kind::Long) {
            if (std::string_view(option.key) == "SpatialAA") {
                const char* labels[] = {"Off", "SMAA 1x", "FXAA", "CMAA2", "FSR 3.1 native AA"};
                ImGui::Combo("Anti-aliasing", &(draft.*option.l), labels, IM_ARRAYSIZE(labels));
            } else if (std::string_view(option.key) == "WaterReflections") {
                const char* labels[] = {"Off", "Sky only", "Half-res"};
                ImGui::Combo("Water reflections", &(draft.*option.l), labels, 3);
            } else if (std::string_view(option.key) == "TextureQuality") {
                const char* labels[] = {"Auto (by VRAM)", "Low", "Medium", "High", "Ultra"};
                int idx = std::clamp(static_cast<int>(draft.*option.l) + 1, 0, 4);
                if (ImGui::Combo("Texture quality", &idx, labels, 5)) draft.*option.l = idx - 1;
            } else ImGui::SliderInt(label, &(draft.*option.l), static_cast<int>(option.lo), static_cast<int>(option.hi));
        } else ImGui::SliderFloat(label, &(draft.*option.f), static_cast<float>(option.lo), static_cast<float>(option.hi));
    }
    if (ImGui::TreeNode("Engine and display options")) {
        for (const auto& row : k_envVars) {
            if (row.cfg) continue;
            std::string value = text(settings, row.key, env(row.key).value_or(""));
            if (row.isBool) {
                bool on = value.empty() ? row.defaultVal && row.defaultVal[0] == '1' : value != "0";
                if (ImGui::Checkbox(row.label, &on)) settings[row.key] = on ? "1" : row.offVal ? row.offVal : "";
            } else {
                std::array<char, 256> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "%s", !settings.contains(row.key) && value.empty() && row.defaultVal ? row.defaultVal : value.c_str());
                if (ImGui::InputText(row.label, buffer.data(), buffer.size())) settings[row.key] = buffer.data();
            }
        }
        static constexpr const char* frameKeys[] = {mc2::gate::name<"MC2_FPS_CAP">(), mc2::gate::name<"MC2_VSYNC">()};
        for (const auto* key : frameKeys) {
            std::array<char, 32> value{};
            std::snprintf(value.data(), value.size(), "%s", text(settings, key, env(key).value_or("")).c_str());
            if (ImGui::InputText(key, value.data(), value.size())) settings[key] = value.data();
        }
        for (const auto& key : {"last_faster_weapons", "last_data_driven"}) {
            bool on = text(settings, key) == "1";
            if (ImGui::Checkbox(key == std::string_view("last_faster_weapons") ? "Faster weapons" : "Generated unit/equipment definitions", &on)) settings[key] = on ? "1" : "0";
        }
        ImGui::TreePop();
    }
    if (ImGui::Button("Save player settings")) {
        if (!saveRenderOptions || !saveRenderOptions(draft)) error = "Could not save options.cfg.";
        else if (saveSettings()) { settingsChanged = true; error = "Saved. Restart the game to apply player settings."; }
    }
    ImGui::TextWrapped("SMAA 1x is the default, without history or jitter. FSR 3.1 native AA is an optional temporal mode.");
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
}

int restartIfRequested(int argc, char** argv) {
    if (!restart) return 0;
    // A child must reload saved choices, not mistake our injected values for shell overrides.
    const auto original = std::move(inheritedEnv);
    for (const auto& [key, value] : original) setEnv(key, value.value_or(""));
#ifdef _WIN32
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string command = GetCommandLineA();
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) return 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    (void)argc;
    execv("/proc/self/exe", argv);
    return 1;
#endif
    return 0;
}
}
