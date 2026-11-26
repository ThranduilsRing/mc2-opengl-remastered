#include "Gates/gate_cold.h"
#include "doctest.h"
#include "inifile.h"
#include "gamelog.h"
#include "Gates/preferences_cfg.h"
#include "code/prefs.h"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <string>
namespace {
using mc2::options::Preferences;
using mc2::options::RenderOptions;
TEST_CASE("Preferences/AA fresh and legacy defaults use SMAA; explicit FSR survives reload") {
    CHECK(RenderOptions{}.spatialAA == 1);
    CHECK_FALSE(RenderOptions{}.temporalAA());
    for (int mode = 0; mode <= 4; ++mode) {
        RenderOptions render;
        const auto* option = mc2::options::find("SpatialAA");
        REQUIRE(option);
        // 1 was the stored default even while present incorrectly forced FSR for every nonzero mode.
        const auto text = mc2::options::cfg::set("FITini\n[MechCommander2]\nFITend\n", *option, std::to_string(mode));
        mc2::options::cfg::loadFromText(render, text);
        CHECK(render.spatialAA == mode);
        CHECK(render.temporalAA() == (mode == 4));
        const auto snapshot = mc2::options::preferenceSnapshot(Preferences{}, render);
        const auto saved = mc2::options::rewritePreferences(text, {}, snapshot);
        RenderOptions reloaded;
        mc2::options::cfg::loadFromText(reloaded, saved);
        CHECK(reloaded.spatialAA == mode);
        CHECK(reloaded.temporalAA() == (mode == 4));
    }
    RenderOptions fresh;
    mc2::options::cfg::loadFromText(fresh, "FITini\n[MechCommander2]\nFITend\n");
    CHECK(fresh.spatialAA == 1);
    CHECK_FALSE(fresh.temporalAA());
}
void load(const std::string& text, Preferences& p, RenderOptions& render) {
    FitIniFile file;
    REQUIRE(file.open(text.data(), static_cast<int>(text.size())) == NO_ERR);
    mc2::options::loadPreferences(file, p);
    file.close();
    mc2::options::cfg::loadFromText(render, text);
}
TEST_CASE("Preferences/building art persists and command line overrides only the process") {
    using namespace mc2::options;
    CHECK_FALSE(RenderOptions{}.remasteredBuildings); // shipped default is Classic
    const auto* option = find("RemasteredBuildings");
    REQUIRE(option);
    auto text = cfg::set("FITini\n[MechCommander2]\nFITend\n", *option, "false");
    RenderOptions loaded;
    cfg::loadFromText(loaded, text);
    CHECK_FALSE(loaded.remasteredBuildings);
    text = cfg::set(text, *option, "true");
    cfg::loadFromText(loaded, text);
    CHECK(loaded.remasteredBuildings);
    const auto old = g_current;
    const auto oldArgument = g_buildingArtCommandLine;
    const auto oldPublished = g_published;
    const auto oldChanged = g_publishChanged;
    REQUIRE(buildingArtArgument("--classic-assets"));
    publish(loaded);
    CHECK_FALSE(current().remasteredBuildings);
    publish(loaded); // CPrefs publishes again after the early options read.
    CHECK_FALSE(current().remasteredBuildings);
    CHECK(loaded.remasteredBuildings); // transient flag never changes the saved draft
    REQUIRE(buildingArtArgument("--remastered-assets"));
    loaded.remasteredBuildings = false;
    publish(loaded);
    CHECK(current().remasteredBuildings);
    CHECK_FALSE(buildingArtArgument("--classic-assets-other"));
    g_buildingArtCommandLine = oldArgument;
    g_current = old;
    g_published = oldPublished;
    g_publishChanged = oldChanged;
}
}
TEST_CASE("Preferences/unchanged options.cfg retains every byte") {
    const char* source = std::getenv("MC2_PREFS_TEST_CFG");
    const std::string text = source ? mc2::options::cfg::readTextFile(source) :
        "FITini \r\n\r\n[MechCommander2]\r\n// player comment\r\n"
        "l DigitalMasterVolume = 192\r\nl MusicVolume = 64\r\n"
        "b Shadows = false\r\nST PlayerName0 = \"Pilot\"\r\n"
        "st IPAddress0 = \"127.0.0.1\"\r\nf ScrollSpeedMult = 2.000000\r\n"
        "b PostCAS = FALSE\r\nf PostCASSharpness = 0.500000\r\n"
        "st ModOwned = \"keep me\"\r\n[Keyboard]\r\nl Key0 = 123\r\nFITend \r\n";
    REQUIRE_FALSE(text.empty());
    Preferences settings; RenderOptions render;
    load(text, settings, render);
    const auto original = mc2::options::preferenceSnapshot(settings, render);
    CHECK(mc2::options::rewritePreferences(text, original, original) == text);
    if (source) std::printf("Preferences round-trip: %zu bytes identical (%s)\n", text.size(), source);
    const long next = settings.MusicVolume == 99 ? 98 : 99;
    settings.MusicVolume = next;
    const auto updated = mc2::options::rewritePreferences(text, original, mc2::options::preferenceSnapshot(settings, render));
    Preferences reloaded; RenderOptions nextRender;
    load(updated, reloaded, nextRender);
    CHECK(reloaded.MusicVolume == next);
    if (text.find("[Keyboard]") != std::string::npos)
        CHECK(updated.substr(updated.find("[Keyboard]")) == text.substr(text.find("[Keyboard]")));
    // Saving again without another edit is also a byte identity.
    const auto snapshot = mc2::options::preferenceSnapshot(reloaded, nextRender);
    CHECK(mc2::options::rewritePreferences(updated, snapshot, snapshot) == updated);
}
TEST_CASE("Preferences/water quality defaults and all levels persist") {
    using namespace mc2::options;
    CHECK(RenderOptions{}.reflectionQuality == 3);
    CHECK(RenderOptions{}.waterDetail == 1);
    const std::string source = "FITini\n[MechCommander2]\nst ModOwned = \"keep\"\nFITend\n";
    Preferences settings;
    RenderOptions original;
    const auto before = preferenceSnapshot(settings, original);
    for (int reflection = 0; reflection <= 3; ++reflection) {
        for (int detail = 0; detail <= 1; ++detail) {
            auto changed = original;
            changed.reflectionQuality = reflection;
            changed.waterDetail = detail;
            const auto text = rewritePreferences(source, before, preferenceSnapshot(settings, changed));
            Preferences reloadedSettings;
            RenderOptions reloaded;
            load(text, reloadedSettings, reloaded);
            CHECK(reloaded == changed);
            CHECK(text.find("st ModOwned = \"keep\"") != std::string::npos);
        }
    }
    RenderOptions invalid;
    std::vector<Reject> rejected;
    cfg::loadFromText(invalid, "[MechCommander2]\nl ReflectionQuality = 4\nl WaterDetail = -1\n", &rejected);
    CHECK(rejected.size() == 2);
    CHECK(invalid == original);
}
TEST_CASE("Preferences/string edits and newly created files retain the FIT grammar") {
    const std::string text = "FITini\n[MechCommander2]\nST PlayerName0 = \"Before\"\n[Keyboard]\nl Key0 = 7\nFITend\n";
    Preferences settings; RenderOptions render;
    load(text, settings, render);
    auto before = mc2::options::preferenceSnapshot(settings, render);
    std::strcpy(settings.playerName[0], "After");
    auto updated = mc2::options::rewritePreferences(text, before, mc2::options::preferenceSnapshot(settings, render));
    CHECK(updated.find("st PlayerName0 = \"After\"") != std::string::npos);
    CHECK(updated.find("Before") == std::string::npos);
    Preferences reloaded; RenderOptions next;
    load(updated, reloaded, next);
    CHECK(std::string(reloaded.playerName[0]) == "After");
    auto created = mc2::options::rewritePreferences("", before, before);
    load(created, reloaded, next);
    CHECK(reloaded.DigitalMasterVolume == settings.DigitalMasterVolume);
    CHECK(created.find("l DigitalMasterVolume") < created.find("FITend"));
}
TEST_CASE("GameLog/long lines are untruncated and unused channels do not leak") {
    auto* output = std::tmpfile(); REQUIRE(output != nullptr);
    mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, output);
    const auto previousLive = mc2::log::detail::g_live.exchange(true);
    const auto previousDecorate = mc2::log::detail::g_decorate.exchange(false);
    const auto previousLevel = mc2::log::detail::g_threshold[static_cast<int>(mc2::log::Cat::Sim)].exchange(mc2::log::Level::Info);
    struct RestoreLog {
        bool live, decorate; mc2::log::Level level; std::FILE* file;
        ~RestoreLog() {
            mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
            mc2::log::detail::g_live.store(live);
            mc2::log::detail::g_decorate.store(decorate);
            mc2::log::detail::g_threshold[static_cast<int>(mc2::log::Cat::Sim)].store(level);
            std::fclose(file);
        }
    } restore{previousLive, previousDecorate, previousLevel, output};
    GameLog::cleanup();
    auto* log = GameLog::getNewFile(); REQUIRE(log != nullptr);
    REQUIRE(log->open("combat.log") == 0);
    const std::string payload(512, 'x');
    log->write(payload.c_str());
    std::fflush(output); std::rewind(output);
    char line[1024]{}; REQUIRE(std::fgets(line, sizeof(line), output) != nullptr);
    CHECK(std::string(line) == "combat.log: " + payload + "\n");
    GameLog::cleanup();
    CHECK_FALSE(log->inUse);
    CHECK(GameLog::getNewFile() == log);
    for (int i = 0; i < MAX_GAMELOGS; ++i) {
        auto* channel = GameLog::getNewFile(); REQUIRE(channel != nullptr);
        channel->open("test");
    }
    CHECK(GameLog::getNewFile() == nullptr);
    GameLog::cleanup();
}

// The keyboard adapter is the only UI dependency of the real preferences IO TU.
int loadPreferenceHotKeys(FitIniFile&) { return 0; }
int savePreferenceHotKeys(std::string&) { return 0; }
unsigned long long preferenceHotKeysFingerprint() { return 0; }
TEST_CASE("Preferences/real CPrefs load-save is byte identical and publishes startup volumes") {
    namespace fs = std::filesystem;
    const char* source = std::getenv("MC2_PREFS_TEST_CFG");
    const std::string text = source ? mc2::options::cfg::readTextFile(source) :
        "FITini\r\n[MechCommander2]\r\nl MusicVolume = 73\r\nb Shadows = FALSE\r\n"
        "st ModSetting = \"kept\"\r\n[Keyboard]\r\nl Key0 = 123\r\nFITend\r\n";
    REQUIRE_FALSE(text.empty());
    const auto originalDirectory = fs::current_path();
    const auto directory = fs::temp_directory_path() / ("mc2-prefs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(directory));
    const auto originalRuntime = mc2::options::runtimePreferences();
    const CPrefs originalPrefs = prefs;
    struct Restore {
        fs::path original, scratch;
        mc2::options::Preferences runtime;
        CPrefs previous;
        ~Restore() { prefs = previous; mc2::options::runtimePreferences() = runtime; fs::current_path(original); fs::remove_all(scratch); }
    } restore{originalDirectory, directory, originalRuntime, originalPrefs};
    fs::current_path(directory);
    { std::ofstream file("options.cfg", std::ios::binary); file << text; REQUIRE(file.good()); }
    REQUIRE(prefs.load() == 0);
    CHECK(mc2::options::runtimePreferences().MusicVolume == prefs.MusicVolume);
    REQUIRE(prefs.save().has_value());
    CHECK(mc2::options::cfg::readTextFile("options.cfg") == text);
    CHECK_FALSE(fs::exists("options.cfg.old"));
    prefs.MusicVolume = prefs.MusicVolume == 101 ? 102 : 101;
    REQUIRE(prefs.save().has_value());
    CHECK(mc2::options::cfg::readTextFile("options.cfg.old") == text);
    CPrefs reloaded; REQUIRE(reloaded.load() == 0);
    CHECK(reloaded.MusicVolume == prefs.MusicVolume);
    const auto updated = mc2::options::cfg::readTextFile("options.cfg");
    REQUIRE(reloaded.save().has_value());
    CHECK(mc2::options::cfg::readTextFile("options.cfg") == updated);
    std::printf("CPrefs round-trip: %zu bytes identical; edited value reloaded; backup identical\n", text.size());
}

TEST_CASE("Preferences/fresh options.cfg writes no GameRules key until one differs from the default") {
    namespace fs = std::filesystem;
    const auto originalDirectory = fs::current_path();
    const auto directory = fs::temp_directory_path() / ("mc2-prefs-rules-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(directory));
    const auto originalRuntime = mc2::options::runtimePreferences();
    const CPrefs originalPrefs = prefs;
    struct Restore {
        fs::path original, scratch;
        mc2::options::Preferences runtime;
        CPrefs previous;
        ~Restore() { prefs = previous; mc2::options::runtimePreferences() = runtime; fs::current_path(original); fs::remove_all(scratch); }
    } restore{originalDirectory, directory, originalRuntime, originalPrefs};
    fs::current_path(directory);
    CPrefs fresh; REQUIRE(fresh.load() == 0);   // no options.cfg: the create path
    REQUIRE(fresh.save().has_value());
    CHECK(mc2::options::cfg::readTextFile("options.cfg").find("GameRules") == std::string::npos);
    fresh.gameRules.heat = mc2::rules::kHeatHarsh;
    REQUIRE(fresh.save().has_value());
    const auto text = mc2::options::cfg::readTextFile("options.cfg");
    CHECK(text.find("GameRulesHeat") != std::string::npos);
    CHECK(text.find("GameRulesSalvage") == std::string::npos);
    fresh.gameRules.heat = mc2::rules::kHeatMC2;   // revert to the default: the stored key follows
    REQUIRE(fresh.save().has_value());
    CPrefs reloaded; REQUIRE(reloaded.load() == 0);
    CHECK(reloaded.gameRules.heat == mc2::rules::kHeatMC2);
    fresh.gameRules.mods = "@purchase-all";
    REQUIRE(fresh.save().has_value());
    mc2::gate::ScopedOverride<"MC2_LAUNCHED"> launcher("1");
    mc2::gate::ScopedOverride<"MC2_CHEAT_INFINITE_MONEY"> money("1");
    CPrefs preselected; REQUIRE(preselected.load() == 0);
    CHECK(mc2::mods::selected(preselected.gameRules.mods, "@infinite-money"));
    CHECK_FALSE(mc2::mods::selected(preselected.gameRules.mods, "@purchase-all"));
}

TEST_CASE("Preferences/explicit empty indexed names override the legacy fallback") {
    const std::string text = "FITini\n[MechCommander2]\nST PlayerName0 = \"\"\nst PlayerName = \"Legacy\"\nFITend\n";
    Preferences settings; RenderOptions render;
    load(text, settings, render);
    CHECK(std::string(settings.playerName[0]).empty());
}

TEST_CASE("Preferences/UI saves preserve unknown keys and active overrides") {
    bool active = false;
    CHECK_FALSE(mc2::options::cfg::editCommitted(true, true, active));
    CHECK_FALSE(mc2::options::cfg::editCommitted(true, true, active));
    CHECK(mc2::options::cfg::editCommitted(false, false, active));
    CHECK_FALSE(mc2::options::cfg::editCommitted(false, false, active)); // failed save is not retried each frame
    CHECK(mc2::options::cfg::editCommitted(true, false, active)); // checkbox/keyboard commit
    const auto oldOverrides = mc2::options::g_activeOverrides;
    const auto oldCurrent = mc2::options::g_current;
    const auto oldPublished = mc2::options::g_published;
    const auto oldChanged = mc2::options::g_publishChanged;
    mc2::options::g_activeOverrides = "ShadowQuality=0";
    RenderOptions draft;
    draft.shadowQuality = 2;
    const std::string text = "FITini\n[MechCommander2]\nst FutureModKey = untouched\nFITend\n";
    const auto saved = mc2::options::rewritePreferences(text, {}, mc2::options::preferenceSnapshot(Preferences{}, draft));
    CHECK(saved.find("st FutureModKey = untouched") != std::string::npos);
    RenderOptions reloaded;
    mc2::options::cfg::loadFromText(reloaded, saved);
    CHECK(reloaded.shadowQuality == 2);
    mc2::options::publish(reloaded);
    CHECK(mc2::options::current().shadowQuality == 0);
    mc2::options::g_activeOverrides = oldOverrides;
    mc2::options::g_current = oldCurrent;
    mc2::options::g_published = oldPublished;
    mc2::options::g_publishChanged = oldChanged;
}
