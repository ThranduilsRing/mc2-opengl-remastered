#include "PlayerFrontEnd.h"
#include "PlayerOptionRows.h"
#include "GraphicsOptionsWindow.h"
#include "imgui.h"
#include "gameos.hpp"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "Gates/render_options_cfg.h"
#include "../3rdparty/include/nlohmann/json.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace PlayerFrontEnd {
namespace fs = std::filesystem;
using Json = nlohmann::json;
bool (*saveRenderOptions)(const mc2::options::RenderOptions&) = nullptr;
namespace {
struct Pack {
    std::string id, name, description, credits, kind, group;
    Json env = Json::object();
    bool enabled = false;
};
Json settings = Json::object();
std::vector<Pack> packs;
std::string campaign, base, texture, campaignFit, pendingStart, error;
bool pickerOpen = false, scanned = false, restart = false;

std::string env(std::string_view key) {
    const char* value = mc2::gate::envText(std::string(key).c_str());
    return value ? value : "";
}
void setEnv(std::string_view key, std::string_view value) {
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
bool contains(std::string_view list, std::string_view id) {
    std::size_t start = 0;
    while (start < list.size()) {
        const auto end = list.find_first_of(",;", start);
        if (list.substr(start, end - start) == id) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
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
    const fs::path root = id.empty() ? fs::path("data") : fs::path("mods") / id / "data";
    for (const auto& file : files(root / "campaign"))
        if (file.extension() == ".fit") result.push_back(file.stem().string());
    // Stock campaign.fit is shipped in the FastFiles, invisible to directory iteration.
    if (id.empty() && std::find(result.begin(), result.end(), "campaign") == result.end()) result.insert(result.begin(), "campaign");
    return result;
}
void scan() {
    packs.clear();
    campaign = env(mc2::gate::name<"MC2_ACTIVE_MOD">());
    base = text(settings, "last_base");
    texture = text(settings, "tex_pack_mod");
    const auto deps = env(mc2::gate::name<"MC2_MOD_DEPS">());
    for (const auto& dir : files("mods")) {
        std::error_code ec;
        if (!fs::is_directory(dir / "data", ec)) continue;
        const auto id = dir.filename().string();
        if (id.empty() || id[0] == '.' || id.find_first_of(",;") != std::string::npos) continue;
        const auto json = readJson(dir / "mod.json");
        Pack pack;
        pack.id = id;
        pack.name = text(json, "name", id);
        pack.description = text(json, "description");
        pack.credits = text(json, "credits", text(json, "author"));
        pack.kind = text(json, "type");
        pack.group = text(json, "radioGroup");
        if (json.contains("env") && json["env"].is_object()) pack.env = json["env"];
        if (pack.kind.empty()) {
            bool playable = false;
            for (const auto& file : files(dir / "data" / "missions"))
                playable |= file.extension() == ".fit";
            pack.kind = id.find("-compat") != std::string::npos ? "dependency" : playable ? "campaign" : "assets";
        }
        pack.enabled = contains(deps, id);
        if (pack.enabled && pack.kind == "dependency") base = id;
        if (pack.enabled && pack.group == "texture-quality") texture = id;
        packs.push_back(std::move(pack));
    }
    const auto fits = campaigns(campaign);
    campaignFit = fits.empty() ? "" : fits.front();
    scanned = true;
}
void autoBase() {
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
            std::transform(source.begin(), source.end(), source.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            magic |= source.find("magicattack") != std::string::npos;
            for (std::size_t at = source.find("objectnumber"); at != std::string::npos; at = source.find("objectnumber", at + 12)) {
                const auto number = source.find_first_of("0123456789", at + 12);
                if (number != std::string::npos) high |= std::atoi(source.c_str() + number) > 1188;
            }
        }
    }
    base = magic ? "mco-compat" : high ? "mc2x-compat" : "";
}
std::string dependencies() {
    std::string deps;
    auto append = [&](std::string_view id) { if (id.empty()) return; if (!deps.empty()) deps += ','; deps += id; };
    for (const auto& pack : packs)
        if (pack.enabled && pack.kind != "campaign" && pack.kind != "dependency" && pack.group != "texture-quality") append(pack.id);
    append(texture);
    append(base);
    return deps;
}
void applyPackEnv(const Pack& pack) {
    for (const auto& [key, value] : pack.env.items())
        if (key.starts_with("MC2_") && value.is_string()) setEnv(key, value.get<std::string>());
}
}

void loadSettings() {
    // Keep shell/harness overrides. Existing installs retain the launcher's file.
    settings = readJson("launcher_env.json");
    if (!mc2::gate::get<"MC2_LAUNCHER_ENV_JSON">() || mc2::gate::isSet<"MC2_LAUNCHED">()) return;
    const bool explicitMod = mc2::gate::envText(mc2::gate::name<"MC2_ACTIVE_MOD">()) != nullptr;
    for (const auto& [key, value] : settings.items())
        if (key.starts_with("MC2_") && value.is_string() && !mc2::gate::envText(key.c_str())) setEnv(key, value.get<std::string>());
    if (!explicitMod) {
        setEnv(mc2::gate::name<"MC2_ACTIVE_MOD">(), text(settings, "last_campaign"));
        std::string deps = text(settings, "last_addons");
        for (const auto& id : {text(settings, "tex_pack_mod"), text(settings, "last_base")}) {
            if (id.empty()) continue;
            if (!deps.empty()) deps += ',';
            deps += id;
        }
        if (!mc2::gate::envText(mc2::gate::name<"MC2_MOD_DEPS">())) setEnv(mc2::gate::name<"MC2_MOD_DEPS">(), deps);
        const bool faster = text(settings, "last_faster_weapons") == "1";
        const bool generated = text(settings, "last_data_driven") == "1";
        for (const auto& [key, value] : {std::pair{mc2::gate::name<"MC2_DIRECT_FIRE_STRAIGHT">(), faster ? "1" : ""},
             {mc2::gate::name<"MC2_PROJECTILE_SPEED_MULT">(), faster ? "2" : ""}, {mc2::gate::name<"MC2_UNIT_PROFILE_DATA">(), generated ? "1" : ""},
             {mc2::gate::name<"MC2_UNIT_DEFINITION_SOURCE">(), generated ? "generated" : ""}, {mc2::gate::name<"MC2_EQUIPMENT_DEFINITION_SOURCE">(), generated ? "generated" : ""}})
            if (!mc2::gate::envText(key)) setEnv(key, value);
    }
    for (const auto& dir : files("mods")) {
        const auto id = dir.filename().string();
        if (id != env(mc2::gate::name<"MC2_ACTIVE_MOD">()) && !contains(env(mc2::gate::name<"MC2_MOD_DEPS">()), id)) continue;
        const auto json = readJson(dir / "mod.json");
        if (!json.contains("env") || !json["env"].is_object()) continue;
        for (const auto& [key, value] : json["env"].items())
            if (key.starts_with("MC2_") && value.is_string() && !mc2::gate::envText(key.c_str())) setEnv(key, value.get<std::string>());
    }
    pendingStart = text(settings, "start_campaign");
    if (!pendingStart.empty()) { settings.erase("start_campaign"); saveSettings(); }
    mc2::gate::invalidateAll();
}

std::string takeCampaignStart() { return std::exchange(pendingStart, {}); }

void drawMainMenu() {
    if (!ImGui::GetCurrentContext()) return;
    static bool bootScreen = false;
    if (!bootScreen) {
        const auto screen = mc2::gate::str<"MC2_BOOT_TO_SCREEN">();
        pickerOpen = screen == "campaigns";
        if (screen == "graphics") GraphicsOptionsWindow::setOpen(true);
        bootScreen = true;
    }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 280, viewport->WorkPos.y + 25), ImGuiCond_Always);
    ImGui::Begin("Campaign tools", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Campaigns & mods")) pickerOpen = !pickerOpen;
    ImGui::SameLine();
    if (ImGui::Button("Graphics")) GraphicsOptionsWindow::setOpen(true);
    ImGui::End();
    if (!pickerOpen) return;
    if (!scanned) scan();
    ImGui::SetNextWindowSize(ImVec2(650, 580), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Campaigns & mods", &pickerOpen)) { ImGui::End(); return; }
    if (ImGui::Button("Refresh")) scan();
    ImGui::SameLine();
    ImGui::TextUnformatted("Installed community campaigns");
    auto select = [&](std::string_view id) {
        campaign = id;
        autoBase();
        const auto fits = campaigns(id);
        campaignFit = fits.empty() ? "" : fits.front();
    };
    if (ImGui::BeginListBox("##packs", ImVec2(-1, 125))) {
        if (ImGui::Selectable("Stock campaign", campaign.empty())) select("");
        for (const auto& pack : packs)
            if (pack.kind == "campaign" && ImGui::Selectable(pack.name.c_str(), campaign == pack.id)) select(pack.id);
        ImGui::EndListBox();
    }
    for (const auto& pack : packs) if (pack.id == campaign) {
        ImGui::TextWrapped("%s", pack.description.c_str());
        if (!pack.credits.empty()) ImGui::TextWrapped("Credits: %s", pack.credits.c_str());
    }
    if (ImGui::BeginCombo("Campaign", campaignFit.c_str())) {
        for (const auto& fit : campaigns(campaign))
            if (ImGui::Selectable(fit.c_str(), campaignFit == fit)) campaignFit = fit;
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
    if (ImGui::TreeNode("Add-ons")) {
        for (auto& pack : packs) if (pack.kind != "campaign" && pack.kind != "dependency" && pack.group != "texture-quality")
            ImGui::Checkbox(pack.name.c_str(), &pack.enabled);
        ImGui::TreePop();
    }
    const auto deps = dependencies();
    const bool contentChanged = campaign != env(mc2::gate::name<"MC2_ACTIVE_MOD">()) || deps != env(mc2::gate::name<"MC2_MOD_DEPS">());
    const bool missingBase = !base.empty() && std::none_of(packs.begin(), packs.end(), [](const Pack& p) { return p.id == base; });
    if (missingBase) ImGui::TextWrapped("Required base %s is missing. Import the compatibility pack first.", base.c_str());
    if (campaignFit.empty()) ImGui::TextUnformatted("No loose campaign FIT found in this pack.");
    ImGui::BeginDisabled(campaignFit.empty() || missingBase);
    if (ImGui::Button(contentChanged ? "Start campaign (restart game)" : "Start campaign")) {
        settings["last_campaign"] = campaign;
        settings["last_base"] = base;
        settings["tex_pack_mod"] = texture;
        std::string addons;
        for (const auto& pack : packs) if (pack.enabled && pack.kind != "campaign" && pack.kind != "dependency" && pack.group != "texture-quality") {
            if (!addons.empty()) addons += ',';
            addons += pack.id;
        }
        settings["last_addons"] = addons;
        if (contentChanged) settings["start_campaign"] = campaignFit;
        if (saveSettings()) {
            if (contentChanged) {
                for (const auto& pack : packs) for (const auto& [key, value] : pack.env.items())
                    if (key.starts_with("MC2_")) setEnv(key, "");
                setEnv(mc2::gate::name<"MC2_ACTIVE_MOD">(), campaign);
                setEnv(mc2::gate::name<"MC2_MOD_DEPS">(), deps);
                // Base first, then texture/add-ons, campaign last: same content precedence as the VFS.
                for (const auto& id : {base, texture}) for (const auto& pack : packs) if (pack.id == id) applyPackEnv(pack);
                for (const auto& pack : packs) if (pack.enabled && pack.kind != "dependency" && pack.group != "texture-quality") applyPackEnv(pack);
                for (const auto& pack : packs) if (pack.id == campaign) applyPackEnv(pack);
                restart = true;
                gos_TerminateApplication();
            } else pendingStart = campaignFit;
            std::printf("[CAMPAIGN_PICKER] start mod=%s campaign=%s deps=%s restart=%d\n", campaign.c_str(), campaignFit.c_str(), deps.c_str(), int(contentChanged));
            pickerOpen = false;
        }
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Import new campaigns with mc2-launcher.exe Import for now, then Refresh here.");
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    ImGui::End();
}

void drawOptions() {
    if (!ImGui::CollapsingHeader("Player settings (saved; restart required)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    static auto draft = [] {
        mc2::options::RenderOptions options;
        mc2::options::cfg::loadFromText(options, mc2::options::cfg::readTextFile("options.cfg"));
        return options;
    }();
    for (const auto& preset : k_gfxPresets) {
        if (ImGui::Button(preset.label)) {
            draft.shadowQuality = preset.shadQual;
            draft.spatialAA = preset.aa;
            for (int i = 0; i < 6; ++i) settings[k_presetKeys[i]] = preset.values[i];
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    for (const auto& option : mc2::options::kOptions) {
        const char* label = std::string_view(option.key) == "RemasteredBuildings" ? "Remastered buildings (off: Classic)" : option.key;
        if (option.kind == mc2::options::Kind::Bool) ImGui::Checkbox(label, &(draft.*option.b));
        else if (option.kind == mc2::options::Kind::Long) {
            if (std::string_view(option.key) == "SpatialAA") {
                const char* labels[] = {"Off", "SMAA 1x", "FXAA", "CMAA2"};
                ImGui::Combo("Anti-aliasing", &(draft.*option.l), labels, 4);
            } else ImGui::SliderInt(label, &(draft.*option.l), int(option.lo), int(option.hi));
        } else ImGui::SliderFloat(label, &(draft.*option.f), float(option.lo), float(option.hi));
    }
    if (ImGui::TreeNode("Engine and display options")) {
        for (const auto& row : k_envVars) {
            if (row.cfg) continue;
            std::string value = text(settings, row.key, env(row.key));
            if (row.isBool) {
                bool on = value.empty() ? row.defaultVal && row.defaultVal[0] == '1' : value != "0";
                if (ImGui::Checkbox(row.label, &on)) settings[row.key] = on ? "1" : row.offVal ? row.offVal : "";
            } else {
                std::array<char, 256> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "%s", value.empty() && row.defaultVal ? row.defaultVal : value.c_str());
                if (ImGui::InputText(row.label, buffer.data(), buffer.size())) settings[row.key] = buffer.data();
            }
        }
        static constexpr const char* frameKeys[] = {mc2::gate::name<"MC2_FPS_CAP">(), mc2::gate::name<"MC2_VSYNC">()};
        for (const auto* key : frameKeys) {
            std::array<char, 32> value{};
            std::snprintf(value.data(), value.size(), "%s", text(settings, key, env(key)).c_str());
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
        else if (saveSettings()) error = "Saved. Restart the game to apply player settings.";
    }
    ImGui::TextWrapped("FSR and MSAA are not implemented. Available AA modes are spatial; no temporal history.");
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
}

int restartIfRequested(int argc, char** argv) {
    if (!restart) return 0;
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
