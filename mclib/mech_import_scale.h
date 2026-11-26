#pragma once
#include <string_view>

namespace mc2mechscale {
// MC2 stock Mad Cat: 52.8158 WU; Sarna miniature: 47.42 mm at 1/265.
// This 12.5663 m anchor is inferred from miniature scale, not a canonical height.
// https://www.sarna.net/wiki/100mm_Timber_Wolf_(Mad_Cat)
// Stock ASE least-squares height fit (12 chassis, 30-100 t): 0.50505628*t + 17.88260556 WU.
// Missing heights use that stock fit; estimates must not be treated as canon.
inline constexpr float worldUnitsPerMetre = 4.20297144f;
struct Height { std::string_view chassis; float metres; bool estimated; std::string_view reference; };
inline constexpr Height heights[] = {
    {"annihilator", 16.27140097f, true, "https://www.sarna.net/wiki/Annihilator"},
    {"archer", 12.66640660f, true, "https://www.sarna.net/wiki/Archer"},
    {"assassin", 9.06141222f, true, "https://www.sarna.net/wiki/Assassin"},
    {"atlas", 16.27140097f, true, "https://www.sarna.net/wiki/Atlas"},
    {"awesome", 13.86807139f, true, "https://www.sarna.net/wiki/Awesome"},
    {"banshee", 15.67056858f, true, "https://www.sarna.net/wiki/Banshee"},
    {"battlemaster", 14.46890379f, true, "https://www.sarna.net/wiki/Battlemaster"},
    {"blackjack", 9.66224462f, true, "https://www.sarna.net/wiki/Blackjack"},
    {"blackknight", 13.26723899f, true, "https://www.sarna.net/wiki/Black_Knight"},
    {"bullshark", 15.67056858f, true, "https://www.sarna.net/wiki/Bullshark"},
    {"cataphract", 12.66640660f, true, "https://www.sarna.net/wiki/Cataphract"},
    {"catapult", 12.06557420f, true, "https://www.sarna.net/wiki/Catapult"},
    {"centurion", 10.26307701f, true, "https://www.sarna.net/wiki/Centurion"},
    {"champion", 11.46474180f, true, "https://www.sarna.net/wiki/Champion"},
    {"cicada", 9.06141222f, true, "https://www.sarna.net/wiki/Cicada"},
    {"commando", 7.25891503f, true, "https://www.sarna.net/wiki/Commando"},
    {"crab", 10.26307701f, true, "https://www.sarna.net/wiki/Crab"},
    {"crusader", 12.06557420f, true, "https://www.sarna.net/wiki/Crusader"},
    {"cyclops", 15.06973618f, true, "https://www.sarna.net/wiki/Cyclops"},
    {"direwolf", 16.27140097f, true, "https://www.sarna.net/wiki/Dire_Wolf"},
    {"dragon", 11.46474180f, true, "https://www.sarna.net/wiki/Dragon"},
    {"enforcer", 10.26307701f, true, "https://www.sarna.net/wiki/Enforcer"},
    {"executioner", 15.67056858f, true, "https://www.sarna.net/wiki/Executioner"},
    {"firestarter", 8.46057982f, true, "https://www.sarna.net/wiki/Firestarter"},
    {"flea", 6.65808264f, true, "https://www.sarna.net/wiki/Flea"},
    {"gargoyle", 13.86807139f, true, "https://www.sarna.net/wiki/Gargoyle"},
    {"grasshopper", 12.66640660f, true, "https://www.sarna.net/wiki/Grasshopper"},
    {"griffin", 10.86390941f, true, "https://www.sarna.net/wiki/Griffin"},
    {"hatchetman", 9.66224462f, true, "https://www.sarna.net/wiki/Hatchetman"},
    {"highlander", 15.06973618f, true, "https://www.sarna.net/wiki/Highlander"},
    {"hunchback", 10.26307701f, true, "https://www.sarna.net/wiki/Hunchback"},
    {"jagermech", 12.06557420f, true, "https://www.sarna.net/wiki/Jagermech"},
    {"javelin", 7.85974743f, true, "https://www.sarna.net/wiki/Javelin"},
    {"jenner", 8.46057982f, true, "https://www.sarna.net/wiki/Jenner"},
    {"kingcrab", 16.27140097f, true, "https://www.sarna.net/wiki/King_Crab"},
    {"kintaro", 10.86390941f, true, "https://www.sarna.net/wiki/Kintaro"},
    {"kitfox", 7.85974743f, true, "https://www.sarna.net/wiki/Kit_Fox"},
    {"locust", 6.65808264f, true, "https://www.sarna.net/wiki/Locust"},
    {"longbow", 14.46890379f, true, "https://www.sarna.net/wiki/Longbow"},
    {"madcat", 13.26723899f, true, "https://www.sarna.net/wiki/Timber_Wolf"},
    {"maddog", 11.46474180f, true, "https://www.sarna.net/wiki/Mad_Dog"},
    {"marauder", 12.00000000f, false, "https://www.sarna.net/wiki/Marauder"},
    {"orion", 13.26723899f, true, "https://www.sarna.net/wiki/Orion"},
    {"panther", 8.46057982f, true, "https://www.sarna.net/wiki/Panther"},
    {"phoenixhawk", 11.00000000f, false, "https://www.sarna.net/wiki/Phoenix_Hawk"},
    {"puma", 8.46057982f, true, "https://www.sarna.net/wiki/Adder"},
    {"quickdraw", 11.46474180f, true, "https://www.sarna.net/wiki/Quickdraw"},
    {"raven", 8.46057982f, true, "https://www.sarna.net/wiki/Raven"},
    {"rifleman", 11.46474180f, true, "https://www.sarna.net/wiki/Rifleman"},
    {"shadowcat", 9.66224462f, true, "https://www.sarna.net/wiki/Shadow_Cat"},
    {"shadowhawk", 10.86390941f, true, "https://www.sarna.net/wiki/Shadow_Hawk"},
    {"spider", 7.85974743f, true, "https://www.sarna.net/wiki/Spider"},
    {"stalker", 14.46890379f, true, "https://www.sarna.net/wiki/Stalker"},
    {"stormcrow", 10.86390941f, true, "https://www.sarna.net/wiki/Stormcrow"},
    {"summoner", 12.66640660f, true, "https://www.sarna.net/wiki/Summoner"},
    {"thunderbolt", 12.06557420f, true, "https://www.sarna.net/wiki/Thunderbolt"},
    {"timberwolf", 13.26723899f, true, "https://www.sarna.net/wiki/Timber_Wolf"},
    {"trebuchet", 10.26307701f, true, "https://www.sarna.net/wiki/Trebuchet"},
    {"urbanmech", 7.85974743f, true, "https://www.sarna.net/wiki/Urbanmech"},
    {"victor", 14.00000000f, false, "https://www.sarna.net/wiki/Victor"},
    {"vindicator", 9.66224462f, true, "https://www.sarna.net/wiki/Vindicator"},
    {"vulcan", 9.06141222f, true, "https://www.sarna.net/wiki/Vulcan"},
    {"warhammer", 12.66640660f, true, "https://www.sarna.net/wiki/Warhammer"},
    {"warhawk", 14.46890379f, true, "https://www.sarna.net/wiki/Warhawk"},
    {"wolfhound", 8.46057982f, true, "https://www.sarna.net/wiki/Wolfhound"},
    {"wolverine", 10.86390941f, true, "https://www.sarna.net/wiki/Wolverine"},
    {"zeus", 13.86807139f, true, "https://www.sarna.net/wiki/Zeus"},
};

inline float targetHeight(std::string_view id) {
    if (id.substr(0, 4) == "mw5:") id.remove_prefix(4);
    else if (id.substr(0, 7) == "bt2018:") id.remove_prefix(7);
    else return 0.0f; // Stock and MCO never enter this correction.
    for (const auto& row : heights) if (row.chassis == id) return row.metres * worldUnitsPerMetre;
    return 0.0f;
}
} // namespace mc2mechscale
