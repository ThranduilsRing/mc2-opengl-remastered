// Trigger: code/game_rules.h, the [GameRules] campaign save block (LogisticsMissionInfo::save/load) and the options.cfg GameRules keys.
// rules-core: nothing reads the rules for gameplay yet, so these pin the defaults, the descriptor table, validation, persistence and the
// "an old save without the block plays under today's rules" boundary.
#include "doctest.h"

#define private public
#include "logisticsmissioninfo.h"
#undef private
#include "game_rules.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "inifile.h"
#include "Gates/preferences_cfg.h"
#include "Gates/preferences_values.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace mc2::rules;

namespace {
// Writes `info` the way LogisticsData::save does (the General block opened by the caller, then the campaign info) and returns the file text.
std::string saveToFile(LogisticsMissionInfo& info, const char* name) {
    const std::string path = std::string(name) + ".fit";
    FitIniFile out;
    REQUIRE(out.create(path.c_str()) == NO_ERR);
    out.writeBlock("General");
    info.save(out);
    out.close();
    return path;
}
bool hasRulesBlock(const std::string& path) {
    FitIniFile in;
    REQUIRE(in.open(path.c_str()) == NO_ERR);
    const bool has = in.seekBlock("GameRules") == NO_ERR;
    in.close();
    return has;
}
}  // namespace

TEST_CASE("game rules: defaults equal today's behaviour") {
    const GameRules g;
    CHECK(g.dropLimitMode == kDropTonnage);
    CHECK_FALSE(g.dropWeightPurchase);
    CHECK(g.heat == kHeatMC2);
    CHECK(g.ammoExplosions == kAmmoExplMC2);
    // Shipped called-shot behaviour is floors ON: the rule default follows the MC2_CALLED_SHOT_FIX gate default (Gates/gate_rows_0.inc).
    {
        mc2::gate::ScopedOverride<"MC2_CALLED_SHOT_FIX"> defaults(nullptr);
        CHECK(g.calledShots == (mc2::gate::get<"MC2_CALLED_SHOT_FIX">() ? kCalledFloors : kCalledClassic));
    }
    CHECK(g.calledShots == kCalledFloors);
    CHECK(g.pilotSkillWeight == 100);
    CHECK(g.salvage == kSalvageMC2);
    CHECK(g.cbillMultiplier == 100);
    // Unlimited ammo is a player preference, not a rule: the struct must not shadow it (the dialog shows the Preferences value).
    CHECK(find("UnlimitedAmmo") == std::nullopt);
    // Every default that has a real source today (the table rows below have none yet: the consumer lanes add theirs) matches it.
    // The defaults a later lane makes live (heat, ammo, called shots, salvage, C-Bills) are pinned by that lane's own source.
    // Every table row's default sits inside its own range, keys are unique and findable in any case.
    for (std::size_t i = 0; i < kRuleCount; ++i) {
        const long v = getLong(g, kRules[i]);
        CHECK(v >= kRules[i].lo);
        CHECK(v <= kRules[i].hi);
        CHECK(find(kRules[i].key) == i);
    }
    CHECK(find("heat") == find("HEAT"));
}

TEST_CASE("game rules: range rejection leaves the field untouched") {
    GameRules g;
    const Rule* cbill = &kRules[find("CBillMultiplier").value()];
    CHECK(setLong(g, *cbill, 201) == Verdict::OutOfRange);
    CHECK(setLong(g, *cbill, 49) == Verdict::OutOfRange);
    CHECK(g.cbillMultiplier == 100);
    CHECK(setLong(g, *cbill, 50) == Verdict::Ok);
    CHECK(g.cbillMultiplier == 50);
    CHECK(setFromText(g, *cbill, "abc") == Verdict::Garbage);
    CHECK(setFromText(g, *cbill, "150x") == Verdict::Garbage);
    CHECK(setFromText(g, *cbill, " 150 ") == Verdict::Ok);
    CHECK(g.cbillMultiplier == 150);
    const Rule* heat = &kRules[find("Heat").value()];
    CHECK(setLong(g, *heat, 3) == Verdict::OutOfRange);
    CHECK(setLong(g, *heat, -1) == Verdict::OutOfRange);
    CHECK(g.heat == kHeatMC2);
    const Rule* purchase = &kRules[find("DropWeightPurchase").value()];
    CHECK(setFromText(g, *purchase, "TRUE") == Verdict::Ok);
    CHECK(g.dropWeightPurchase);
    CHECK(setFromText(g, *purchase, "2") == Verdict::OutOfRange);
    CHECK(g.dropWeightPurchase);
    CHECK(setFromText(g, *purchase, "maybe") == Verdict::Garbage);
    CHECK(g.dropWeightPurchase);

    GameRules bad;
    bad.heat = 9;
    bad.cbillMultiplier = 5;
    bad.salvage = kSalvageScarce;
    CHECK(sanitize(bad) == 2);
    CHECK(bad.heat == kHeatMC2);
    CHECK(bad.cbillMultiplier == 100);
    CHECK(bad.salvage == kSalvageScarce);
}

TEST_CASE("game rules: unknown keys are ignored and the BattleTech preset round trips") {
    CHECK_FALSE(find("NoSuchRule").has_value());
    CHECK_FALSE(find("").has_value());

    const GameRules preset = battleTechPreset();
    CHECK_FALSE(preset == GameRules{});
    GameRules viaText;
    for (const Rule& r : kRules)
        CHECK(setFromText(viaText, r, std::to_string(getLong(preset, r))) == Verdict::Ok);
    CHECK(viaText == preset);
    // Every preset value is inside its range (the owner will retune the numbers, not the shape).
    GameRules copy = preset;
    CHECK(sanitize(copy) == 0);
}

TEST_CASE("game rules: campaign save round trip through LogisticsMissionInfo") {
    LogisticsMissionInfo info;
    info.setRules(battleTechPreset());
    const std::string path = saveToFile(info, "game_rules_roundtrip");
    CHECK(hasRulesBlock(path));

    // load() reads the rules even for a save that names no campaign file (a skirmish save); the starting rules are wiped first.
    LogisticsMissionInfo loaded;
    GameRules other;
    other.heat = kHeatOff;
    loaded.setRules(other);
    FitIniFile in;
    REQUIRE(in.open(path.c_str()) == NO_ERR);
    CHECK(loaded.load(in).has_value());
    in.close();
    CHECK(loaded.getRules() == battleTechPreset());
    std::remove(path.c_str());
}

TEST_CASE("game rules: an old save without [GameRules] loads with the defaults and default saves stay byte-identical") {
    LogisticsMissionInfo info;
    const std::string path = saveToFile(info, "game_rules_old_save");
    CHECK_FALSE(hasRulesBlock(path));   // a default campaign writes no block: identical to a pre-rules save

    LogisticsMissionInfo loaded;
    loaded.setRules(battleTechPreset());   // stale rules from the previous campaign must not leak into an old save
    FitIniFile in;
    REQUIRE(in.open(path.c_str()) == NO_ERR);
    CHECK(loaded.load(in).has_value());
    in.close();
    CHECK(loaded.getRules() == GameRules{});
    std::remove(path.c_str());
}

TEST_CASE("game rules: a partly damaged [GameRules] block keeps the good keys and defaults the bad ones") {
    const std::string path = "game_rules_damaged.fit";
    {
        FitIniFile out;
        REQUIRE(out.create(path.c_str()) == NO_ERR);
        out.writeBlock("GameRules");
        out.writeIdLong("Heat", 2);
        out.writeIdLong("CBillMultiplier", 900);   // out of range
        out.writeIdLong("FutureRule", 7);          // unknown key from a newer build
        out.close();
    }
    FitIniFile in;
    REQUIRE(in.open(path.c_str()) == NO_ERR);
    const GameRules g = readFit(in);
    in.close();
    std::remove(path.c_str());
    CHECK(g.heat == kHeatHarsh);
    CHECK(g.cbillMultiplier == 100);
    CHECK(g.dropLimitMode == kDropTonnage);
}

TEST_CASE("game rules: options.cfg keys round trip, garbage and out-of-range values keep the default") {
    using namespace mc2::options;
    auto snapshot = preferenceSnapshot(Preferences{}, RenderOptions{});
    const auto base = snapshot.size();
    for (const auto& e : cfgEntries(battleTechPreset())) snapshot.push_back({e.key, e.text, e.type});
    bool sawKey = false;
    for (const auto& v : snapshot) sawKey |= v.key == "GameRulesHeat" && v.text == "2" && v.type == 'l';
    CHECK(sawKey);
    CHECK(snapshot.size() == base + kRuleCount);
    const std::string text = rewritePreferences("", {}, snapshot);
    GameRules reloaded;
    loadCfgText(reloaded, text);
    CHECK(reloaded == battleTechPreset());
    // The Preferences loader still reads its own keys from the same text and ignores the rule keys.
    Preferences p;
    FitIniFile file;
    REQUIRE(file.open(text.data(), static_cast<int>(text.size())) == NO_ERR);
    loadPreferences(file, p);
    file.close();
    CHECK(p.useUnlimitedAmmo == Preferences{}.useUnlimitedAmmo);

    // No keys at all (an old options.cfg) leaves today's defaults.
    GameRules fresh;
    loadCfgText(fresh, "FITini\n[MechCommander2]\nl MusicVolume = 10\nFITend\n");
    CHECK(fresh == GameRules{});

    // Out of range, garbage (FitIniFile's atol would make these 0 = a valid non-default Heat Off / Called Off / Weight 0) and a bad bool: default stays.
    GameRules edited;
    loadCfgText(edited,
        "FITini\r\n[MechCommander2]\r\nl GameRulesHeat = 77\r\nl GameRulesCBillMultiplier = 120\r\n"
        "l GameRulesAmmoExplosions = abc\r\nl GameRulesCalledShots = 1x\r\nl GameRulesPilotSkillWeight = \r\n"
        "b GameRulesDropWeightPurchase = banana\r\nl gamerulessalvage = 0\r\nl GameRulesSalvage = 2\r\nFITend\r\n");
    CHECK(edited.heat == kHeatMC2);
    CHECK(edited.cbillMultiplier == 120);
    CHECK(edited.ammoExplosions == kAmmoExplMC2);
    CHECK(edited.calledShots == kCalledFloors);
    CHECK(edited.pilotSkillWeight == 100);
    CHECK_FALSE(edited.dropWeightPurchase);
    CHECK(edited.salvage == kSalvageGenerous);   // keys are case-insensitive and the first occurrence wins

    // A commented-out key is ignored and does not shadow the real one after it.
    GameRules commented;
    loadCfgText(commented, "// l GameRulesHeat = 2\r\n; l GameRulesCBillMultiplier = 60\r\n  # l GameRulesSalvage = 2\r\nl GameRulesHeat = 0\r\n");
    CHECK(commented.heat == kHeatOff);
    CHECK(commented.cbillMultiplier == 100);
    CHECK(commented.salvage == kSalvageMC2);
}

TEST_CASE("game rules: static layout, labels and the publish decision") {
    for (const Rule& r : kRules)
        if (r.kind == Kind::Enum) {
            REQUIRE(r.labels != nullptr);
            CHECK(r.nLabels == static_cast<std::size_t>(r.hi + 1));
        }
    // A networked game (legacy MPlayer or the mc2::net lockstep session) publishes the defaults whatever the local rules say.
    CHECK(rulesToPublish(true, battleTechPreset()) == GameRules{});
    CHECK(rulesToPublish(false, battleTechPreset()) == battleTechPreset());
}

TEST_CASE("game rules: clear() resets and a campaign save names its campaign file, the rules read on that path") {
    LogisticsMissionInfo info;
    info.setRules(battleTechPreset());

    const std::string campaign = "game_rules_campaign.fit";
    {
        FitIniFile out;
        REQUIRE(out.create(campaign.c_str()) == NO_ERR);
        out.writeBlock("Campaign");
        out.writeIdString("CampaignName", "rules test");
        out.writeIdLong("GroupCount", 1);
        out.writeBlock("Group0");
        out.writeIdLong("MissionCount", 0);
        out.close();
    }
    {   // init() starts with clear(), which must put the rules back to the defaults (clear() itself is not linkable through the private-public hack)
        FitIniFile campaignFile;
        REQUIRE(campaignFile.open(campaign.c_str()) == NO_ERR);
        info.setRules(battleTechPreset());
        REQUIRE(info.init(campaignFile).has_value());
        campaignFile.close();
        CHECK(info.getRules() == GameRules{});
    }
    const std::string save = "game_rules_campaign_save.fit";
    {
        FitIniFile out;
        REQUIRE(out.create(save.c_str()) == NO_ERR);
        out.writeBlock("General");
        out.writeIdString("CampaignFile", campaign.c_str());
        out.writeIdLong("CBills", 0);
        out.writeIdLong("Group", 0);
        out.writeIdLong("CompletedMissions", 0);
        out.writeBlock("AdditionalPurchaseFiles");
        writeFit(out, battleTechPreset());
        out.close();
    }
    LogisticsMissionInfo loaded;
    FitIniFile in;
    REQUIRE(in.open(save.c_str()) == NO_ERR);
    const auto r = loaded.load(in);
    in.close();
    std::remove(save.c_str());
    std::remove(campaign.c_str());
    REQUIRE(r.has_value());
    CHECK(loaded.getRules() == battleTechPreset());   // the campaign-file path (logisticsmissioninfo.cpp, end of load), not the skirmish early return
}

TEST_CASE("game rules: the published snapshot defaults and republishes") {
    CHECK(current() == GameRules{});
    publish(battleTechPreset());
    CHECK(current() == battleTechPreset());
    publish(GameRules{});
    CHECK(current() == GameRules{});
}

// Dialog contract (code/gamerulesdialog.cpp): the page supplies one value button per descriptor row, and DEFAULTS / BATTLETECH PRESET
// are plain assignments that the cfg text must round trip. Every rule's default option is identifiable as "MC2 Default" in the dialog.
TEST_CASE("game rules: dialog page rows equal the descriptor table, presets round trip, the default option says MC2 Default") {
    std::ifstream in(std::string(MC2_REPO_ROOT) + "/data/defs/ui/packages/default/game/mcl_gamerules.fit", std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string page = ss.str();
    const std::string needle = "key = \"game.mcl_gamerules.button.row";
    std::size_t buttons = 0;
    for (std::size_t at = page.find(needle); at != std::string::npos; at = page.find(needle, at + 1)) ++buttons;
    CHECK(buttons == kRuleCount);

    for (const GameRules& preset : {GameRules{}, battleTechPreset()}) {
        std::string text;
        for (const CfgEntry& e : cfgEntries(preset)) text += e.key + "=" + e.text + "\n";
        GameRules back;
        loadCfgText(back, text);
        CHECK(back == preset);
    }

    const GameRules defaults;
    for (const Rule& r : kRules)
        if (r.kind == Kind::Enum)
            CHECK(std::string(r.labels[getLong(defaults, r) - r.lo]).rfind("MC2 Default", 0) == 0);
}
