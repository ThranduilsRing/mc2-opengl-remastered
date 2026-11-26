#include "doctest.h"
#include "game_rules.h"
#include "brain_execution_policy.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"
#include "net/runtime.h"
#include "gos_terrain_lighting.h"
#include <vector>

namespace mc2::brainpolicy {
Policy g_policy = Policy::Legacy;
std::uint32_t g_generation = 1;
const char* name(Policy p) { return p == Policy::Legacy ? "legacy" : "techbrain"; }
static bool missionFitHasBrainBlock(std::string_view) { return false; }
static bool overlayBrainFile(std::string_view) { return false; }
bool profileInstalled(std::string_view) { return false; }
#include "review_decide.inc"
}
namespace {
struct LogisticsData {
    static LogisticsData* instance;
    mc2::rules::GameRules saved;
    void* missionInfo = nullptr;
#include "review_campaign.inc"
    void publishRules() { mc2::rules::publish(saved); }
};
LogisticsData* LogisticsData::instance = nullptr;
struct FullPathFileName : std::string {
    void init(const char* dir, const char* name, const char* ext) { assign(dir).append(name).append(ext); }
    operator const char*() const { return c_str(); }
};
void loadRules() {
    const char* missionPath = "data/missions/";
    const char* missionFileName = "mc2_01";
#include "review_save.inc"
}
constexpr int MISSION_LOAD_SP_QUICKSTART = 0;
bool MPlayer = false;
struct { mc2::rules::GameRules gameRules; } prefs;
void launchRules(int loadType) {
#include "review_launch.inc"
}
bool lightingEnabled = false;
std::vector<gos_terrain_lighting::GpuTerrainLight> s_beaconLights;
}
namespace gos_terrain_lighting { bool IsEnabled() { return lightingEnabled; } }
#include "review_beacon.inc"

TEST_CASE("ReviewRules/save load refreshes policy in both directions before BRN1") {
    const auto oldRules = mc2::rules::current();
    const auto oldPolicy = mc2::brainpolicy::current();
    mc2::gate::ScopedOverride<"MC2_BRAIN_POLICY"> automatic("auto");
    mc2::gate::ScopedOverride<"MC2_PLAY_MISSION"> noCommander("0");
    LogisticsData data;
    LogisticsData::instance = &data;
    for (const long ai : {1L, 0L}) {
        data.saved.ai = ai;
        loadRules();
        CHECK_FALSE(mc2::brainpolicy::techBrain()); // sidecar-free Smarter uses Magic ABL
        CHECK(mc2::rules::current().ai == ai);
    }
    LogisticsData::instance = nullptr;
    mc2::rules::publish(oldRules);
    mc2::brainpolicy::g_policy = oldPolicy;
}

TEST_CASE("ReviewRules/Magic profile is restricted to Smarter ABL assets") {
    using mc2::brainpolicy::magicAsset;
    for (const auto key : {"data/missions/corebrain.abx", "data/missions/warriors/patrol.abl"}) {
        CHECK(magicAsset(key, 0).empty());
        CHECK(magicAsset(key, 1, [](auto) { return false; }).empty());
        CHECK(magicAsset(key, 1, [](auto) { return true; }) == magicAsset(key, 1));
        CHECK(magicAsset(key, 1) == std::string("mods/magic-brain-1.45/") + key);
    }
    for (const auto key : {"data/missions/mc2_01.abl", "data/missions/orders.abx",
                           "data/missions/warriors/pilot.fit", "data/missions/warriors/../x.abl"})
        CHECK(magicAsset(key, 1).empty());
}
TEST_CASE("ReviewRules/campaign rules win including quickstart without logistics") {
    const auto oldRules = mc2::rules::current();
    LogisticsData data;
    for (const long ai : {0L, 1L}) {
        data.saved.ai = ai;
        prefs.gameRules.ai = 1 - ai;
        for (const bool campaign : {false, true}) {
            data.missionInfo = campaign ? &data : nullptr;
            for (const int loadType : {0, 1}) {
                LogisticsData::instance = &data;
                data.publishRules();
                launchRules(loadType);
                CHECK(mc2::rules::current().ai == ((!campaign && loadType == 0) ? 1 - ai : ai));
            }
        }
    }
    LogisticsData::instance = nullptr;
    mc2::rules::publish(oldRules);
}
TEST_CASE("ReviewRules/disabled terrain lighting does not accumulate beacons") {
    for (const bool enabled : {false, true}) {
        lightingEnabled = enabled;
        s_beaconLights.clear();
        for (int i = 0; i < 1000; ++i) gos_terrain_lighting::QueueBeaconLight({});
        CHECK(s_beaconLights.size() == (enabled ? 1000 : 0));
    }
    s_beaconLights.clear();
}
