// Trigger: code/game_rules.h, the [GameRules] campaign save block (LogisticsMissionInfo::save/load) and the options.cfg GameRules keys.
// rules-core: nothing reads the rules for gameplay yet, so these pin the defaults, the descriptor table, validation, persistence and the
// "an old save without the block plays under today's rules" boundary.
#include "doctest.h"

#define private public
#include "logisticsmissioninfo.h"
#undef private
#include "game_rules.h"
#include "inifile.h"
#include "Gates/preferences_cfg.h"

#include <cstdio>
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
    CHECK(g.calledShots == kCalledClassic);
    CHECK(g.pilotSkillWeight == 100);
    CHECK(g.salvage == kSalvageMC2);
    CHECK(g.cbillMultiplier == 100);
    CHECK(g.unlimitedAmmo);   // the Preferences default
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
    const Rule* ammo = &kRules[find("UnlimitedAmmo").value()];
    CHECK(setFromText(g, *ammo, "FALSE") == Verdict::Ok);
    CHECK_FALSE(g.unlimitedAmmo);
    CHECK(setFromText(g, *ammo, "2") == Verdict::OutOfRange);
    CHECK_FALSE(g.unlimitedAmmo);

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

TEST_CASE("game rules: options.cfg keys round trip and a hand-edited bad value falls back") {
    using namespace mc2::options;
    Preferences p;
    p.gameRules = battleTechPreset();
    const auto snapshot = preferenceSnapshot(p, RenderOptions{});
    bool sawKey = false;
    for (const auto& v : snapshot) sawKey |= v.key == "GameRulesHeat" && v.text == "2" && v.type == 'l';
    CHECK(sawKey);
    const std::string text = rewritePreferences("", {}, snapshot);

    Preferences reloaded;
    FitIniFile file;
    REQUIRE(file.open(text.data(), static_cast<int>(text.size())) == NO_ERR);
    loadPreferences(file, reloaded);
    file.close();
    CHECK(reloaded.gameRules == battleTechPreset());

    // No keys at all (an old options.cfg) leaves today's defaults.
    const std::string old = "FITini\n[MechCommander2]\nl MusicVolume = 10\nFITend\n";
    Preferences fresh;
    FitIniFile oldFile;
    REQUIRE(oldFile.open(old.data(), static_cast<int>(old.size())) == NO_ERR);
    loadPreferences(oldFile, fresh);
    oldFile.close();
    CHECK(fresh.gameRules == GameRules{});

    const std::string bad = "FITini\n[MechCommander2]\nl GameRulesHeat = 77\nl GameRulesCBillMultiplier = 120\nFITend\n";
    Preferences edited;
    FitIniFile badFile;
    REQUIRE(badFile.open(bad.data(), static_cast<int>(bad.size())) == NO_ERR);
    loadPreferences(badFile, edited);
    badFile.close();
    CHECK(edited.gameRules.heat == kHeatMC2);
    CHECK(edited.gameRules.cbillMultiplier == 120);
}

TEST_CASE("game rules: the published snapshot defaults and republishes") {
    CHECK(current() == GameRules{});
    publish(battleTechPreset());
    CHECK(current() == battleTechPreset());
    publish(GameRules{});
    CHECK(current() == GameRules{});
}
