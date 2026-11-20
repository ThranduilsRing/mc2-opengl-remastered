// code/game_rules.h -- the per-campaign / per-skirmish rule choices (the "Game Rules" menu).
//
// WHAT: one struct of values (GameRules), one descriptor table naming each rule with its kind, range and default, and the validation and
// FIT read/write shared by every place a value arrives: the campaign save ([GameRules] block), options.cfg (solo / new-campaign defaults,
// keys "GameRules<Key>") and the later dialog rows. Modelled on Gates/render_options.h (kOptions). Standard library only; it never logs.
// WHY: rules are chosen when a campaign or skirmish starts and may be edited between missions, so they live with the campaign, not in
// the renderer options. Every default equals today's behaviour: an absent key, an old save and a tool that never loads them all agree.
// Bay style is NOT a rule: it stays RenderOptions::mechLabLocations.
// Nothing reads these for gameplay yet; the combat/economy/drop-limit lanes consume rules().
#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::rules {

// Enum values are stored as long (the descriptor table drives one integer slot per rule). The numeric values are written to saves: never
// renumber, only append.
// DropLimitMode: what caps a drop.
inline constexpr long kDropTonnage = 0, kDropBattleValue = 1, kDropNone = 2;
// HeatMode, AmmoExplosionMode, CalledShotMode, SalvageMode.
inline constexpr long kHeatOff = 0, kHeatMC2 = 1, kHeatHarsh = 2;
inline constexpr long kAmmoExplOff = 0, kAmmoExplMC2 = 1, kAmmoExplLethal = 2;
inline constexpr long kCalledOff = 0, kCalledFloors = 1, kCalledClassic = 2;
inline constexpr long kSalvageGenerous = 0, kSalvageMC2 = 1, kSalvageScarce = 2;

struct GameRules {
    long dropLimitMode        = kDropTonnage;   // what caps a drop: tonnage (today), battle value, or nothing (tonnage still shown as info)
    bool dropWeightPurchase   = false;          // the player may buy extra drop weight
    long dropWeightStep       = 5;              // tons (or BV points per ton-equivalent later) per purchase
    long heat                 = kHeatMC2;
    long ammoExplosions       = kAmmoExplMC2;
    long calledShots          = kCalledFloors;  // Floors = the shipped behaviour (MC2_CALLED_SHOT_FIX default ON since 1120b); Classic = stock
    long pilotSkillWeight     = 100;            // percent of today's skill influence on to-hit
    long salvage              = kSalvageMC2;
    long cbillMultiplier      = 100;            // percent of the mission-objective C-Bill payout, 50..200 (salvage is a cost, not income; the additionalCBills stipend is unscaled)
    // Unlimited ammo is NOT a rule: it stays the player preference UnlimitedAmmo (Gates/preferences_values.h); the dialog shows that one.
    bool operator==(const GameRules&) const = default;
};

// The "BattleTech" preset button. PLACEHOLDER VALUES: the owner tunes these; only the shape (a function returning a filled GameRules) is fixed.
inline GameRules battleTechPreset() {
    GameRules r;
    r.dropLimitMode      = kDropBattleValue;
    r.dropWeightPurchase = true;
    r.dropWeightStep     = 5;
    r.heat               = kHeatHarsh;
    r.ammoExplosions     = kAmmoExplLethal;
    r.calledShots        = kCalledFloors;
    r.pilotSkillWeight   = 150;
    r.salvage            = kSalvageScarce;
    r.cbillMultiplier    = 75;
    return r;
}

enum class Kind { Bool, Enum, Int, Percent };

struct Rule {
    std::string_view   key;       // FIT key in the [GameRules] block (a string literal); options.cfg writes "GameRules" + key
    Kind               kind;
    long               lo, hi;    // accepted range for Enum, Int and Percent
    bool GameRules::*  b;
    long GameRules::*  l;
    const std::string_view* labels; // Enum: one display label per value, lo..hi; null otherwise
    std::size_t        nLabels;     // Enum: how many labels (checked against hi below)
};

namespace detail {
constexpr Rule boolRule(std::string_view k, bool GameRules::*m) { return {k, Kind::Bool, 0, 1, m, nullptr, nullptr, 0}; }
template <std::size_t N>
constexpr Rule enumRule(std::string_view k, long GameRules::*m, long hi, const std::string_view (&labels)[N]) { return {k, Kind::Enum, 0, hi, nullptr, m, labels, N}; }
constexpr Rule intRule(std::string_view k, long GameRules::*m, long lo, long hi) { return {k, Kind::Int, lo, hi, nullptr, m, nullptr, 0}; }
constexpr Rule pctRule(std::string_view k, long GameRules::*m, long lo, long hi) { return {k, Kind::Percent, lo, hi, nullptr, m, nullptr, 0}; }
inline constexpr std::string_view kDropLabels[]    = {"Tonnage", "Battle Value", "None"};
inline constexpr std::string_view kHeatLabels[]    = {"Off", "MC2", "Harsh"};
inline constexpr std::string_view kAmmoLabels[]    = {"Off", "MC2", "Lethal"};
inline constexpr std::string_view kCalledLabels[]  = {"Off", "Floors", "Classic"};
inline constexpr std::string_view kSalvageLabels[] = {"Generous", "MC2", "Scarce"};
}  // namespace detail

inline constexpr Rule kRules[] = {
    detail::enumRule("DropLimitMode",      &GameRules::dropLimitMode,    kDropNone,      detail::kDropLabels),
    detail::boolRule("DropWeightPurchase", &GameRules::dropWeightPurchase),
    detail::intRule ("DropWeightStep",     &GameRules::dropWeightStep,   1, 50),
    detail::enumRule("Heat",               &GameRules::heat,             kHeatHarsh,     detail::kHeatLabels),
    detail::enumRule("AmmoExplosions",     &GameRules::ammoExplosions,   kAmmoExplLethal, detail::kAmmoLabels),
    detail::enumRule("CalledShots",        &GameRules::calledShots,      kCalledClassic, detail::kCalledLabels),
    detail::pctRule ("PilotSkillWeight",   &GameRules::pilotSkillWeight, 0, 200),
    detail::enumRule("Salvage",            &GameRules::salvage,          kSalvageScarce, detail::kSalvageLabels),
    detail::pctRule ("CBillMultiplier",    &GameRules::cbillMultiplier,  50, 200),
};

inline constexpr std::size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

namespace detail {
constexpr bool labelsMatchRanges() {
    for (const Rule& r : kRules)
        if (r.kind == Kind::Enum && (r.lo != 0 || r.nLabels != static_cast<std::size_t>(r.hi - r.lo + 1))) return false;
    return true;
}
}  // namespace detail
static_assert(detail::labelsMatchRanges(), "every Enum rule needs exactly one label per value lo..hi");

enum class Verdict { Ok, Garbage, OutOfRange };

namespace detail {
constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
}  // namespace detail

// Keys are case-insensitive; an unknown key is the caller's to ignore.
inline std::optional<std::size_t> find(std::string_view key) {
    for (std::size_t i = 0; i < kRuleCount; ++i)
        if (detail::equalsNoCase(key, kRules[i].key)) return i;
    return std::nullopt;
}

// A rejected value leaves the field untouched. Bool rules take 0/1, the others a value inside [lo, hi].
inline Verdict setLong(GameRules& g, const Rule& r, long v) {
    if (v < r.lo || v > r.hi) return Verdict::OutOfRange;
    if (r.kind == Kind::Bool) g.*(r.b) = v != 0;
    else g.*(r.l) = v;
    return Verdict::Ok;
}

inline long getLong(const GameRules& g, const Rule& r) { return r.kind == Kind::Bool ? (g.*(r.b) ? 1L : 0L) : g.*(r.l); }

// "Key=Value" text form: booleans TRUE/FALSE/1/0 in any case, numbers must parse in full.
inline Verdict setFromText(GameRules& g, const Rule& r, std::string_view text) {
    text = detail::trim(text);
    if (r.kind == Kind::Bool) {
        if (detail::equalsNoCase(text, "true"))  return setLong(g, r, 1);
        if (detail::equalsNoCase(text, "false")) return setLong(g, r, 0);
    }
    long v = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), v);
    if (text.empty() || res.ec != std::errc{} || res.ptr != text.data() + text.size()) return Verdict::Garbage;
    return setLong(g, r, v);
}

// Any field outside its range goes back to its default (a hand-edited options.cfg); returns how many were reset.
inline int sanitize(GameRules& g) {
    const GameRules defaults;
    int reset = 0;
    for (const Rule& r : kRules) {
        const long v = getLong(g, r);
        if (v < r.lo || v > r.hi) {
            setLong(g, r, getLong(defaults, r));
            ++reset;
        }
    }
    return reset;
}

// options.cfg: the keys are "GameRules" + Key, typed lines like every other preference ("l GameRulesHeat = 2", "b GameRulesDropWeightPurchase = TRUE").
// cfgEntries lists them for the lossless options.cfg writer (key, text, type letter 'b' or 'l'); loadCfgText reads them back from the raw file text
// with setFromText, so garbage ("l GameRulesHeat = abc") or an out-of-range value keeps the default instead of becoming 0. First occurrence wins.
struct CfgEntry { std::string key, text; char type; };

inline std::vector<CfgEntry> cfgEntries(const GameRules& g) {
    std::vector<CfgEntry> out;
    for (const Rule& r : kRules) {
        std::string key = "GameRules";
        key += r.key;
        if (r.kind == Kind::Bool) out.push_back({key, g.*(r.b) ? "TRUE" : "FALSE", 'b'});
        else out.push_back({key, std::to_string(g.*(r.l)), 'l'});
    }
    return out;
}

inline void loadCfgText(GameRules& g, std::string_view text) {
    constexpr std::string_view prefix = "GameRules";
    bool seen[kRuleCount] = {};
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = detail::trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.starts_with("//") || line.starts_with(';') || line.starts_with('#')) continue;   // a commented-out key does not count
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view name = detail::trim(line.substr(0, eq));
        const std::size_t sp = name.find_last_of(" \t");   // drop the type letter ("l ", "b ")
        if (sp != std::string_view::npos) name = name.substr(sp + 1);
        if (name.size() <= prefix.size() || !detail::equalsNoCase(name.substr(0, prefix.size()), prefix)) continue;
        const auto idx = find(name.substr(prefix.size()));
        if (!idx || seen[*idx]) continue;
        seen[*idx] = true;
        setFromText(g, kRules[*idx], line.substr(eq + 1));
    }
}

// The campaign save block. Written only when a rule differs from today's behaviour, so a default campaign saves byte-identically to older builds,
// and read key by key: an absent key (an old save) or an out-of-range value keeps the default. Old builds ignore the unknown block. A typed line
// whose value is not a number reads as 0 through FitIniFile (atol): only the program writes this block, and the hand-edited file is options.cfg
// (loadCfgText above, which does reject garbage).
inline constexpr const char* kFitBlock = "GameRules";

template <class File>
void writeFit(File& file, const GameRules& g) {
    if (g == GameRules{}) return;
    file.writeBlock(kFitBlock);
    for (const Rule& r : kRules) {
        if (r.kind == Kind::Bool) file.writeIdBoolean(r.key, g.*(r.b));
        else file.writeIdLong(r.key, g.*(r.l));
    }
}

// Returns the rules found in `file` (defaults when the block or a key is missing). The file position is left inside the block when it exists.
template <class File>
GameRules readFit(File& file) {
    GameRules g;
    if (file.seekBlock(kFitBlock) != 0) return g;
    for (const Rule& r : kRules) {
        if (r.kind == Kind::Bool) {
            bool v = g.*(r.b);
            if (file.readIdBoolean(r.key, v) == 0) g.*(r.b) = v;
        } else {
            long v = 0;
            if (file.readIdLong(r.key, v) == 0) setLong(g, r, v);
        }
    }
    return g;
}

// The immutable snapshot the simulation reads, published at mission start (Mission::init) and again by loadLogistics after an in-mission save is
// restored (the first point the saved rules are read; the old mission's snapshot stays current until then, so nothing may read current() while a
// load is running), mirroring mc2::options::current(). Publish only while no sim tick runs. A tool that never publishes sees the defaults.
inline GameRules g_current{};
inline const GameRules& current() { return g_current; }
inline void publish(const GameRules& g) { g_current = g; }

// What a mission start publishes: the campaign / skirmish rules for a solo game, the defaults for any networked one (legacy MPlayer or the
// lockstep mc2::net session): the wire carries no rules, so every peer must simulate the same ones.
inline GameRules rulesToPublish(bool networked, const GameRules& g) { return networked ? GameRules{} : g; }

}  // namespace mc2::rules
