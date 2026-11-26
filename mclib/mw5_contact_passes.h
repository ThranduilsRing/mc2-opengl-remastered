#pragma once
#include <string_view>

namespace mc2ground {
// Generated from mw5-foot-plant-2/contact-final.log: stance <2 cm AND contact_frames >0.
// Bits: walk, run, reverse, limp left, limp right. Unknown assets retain package playback.
struct ContactPass { std::string_view chassis; unsigned gaits; };
inline constexpr ContactPass contactPasses[] = {
    {"mw5_kitfox", 15},
    {"mw5_archer", 29},
    {"mw5_assassin", 29},
    {"mw5_atlas", 24},
    {"mw5_awesome", 29},
    {"mw5_blackjack", 31},
    {"mw5_catapult", 29},
    {"mw5_centurion", 29},
    {"mw5_champion", 31},
    {"mw5_cicada", 31},
    {"mw5_commando", 31},
    {"mw5_crab", 31},
    {"mw5_crusader", 29},
    {"mw5_cyclops", 7},
    {"mw5_direwolf", 31},
    {"mw5_dragon", 2},
    {"mw5_executioner", 2},
    {"mw5_firestarter", 31},
    {"mw5_flea", 31},
    {"mw5_gargoyle", 31},
    {"mw5_highlander", 31},
    {"mw5_hunchback", 0},
    {"mw5_jagermech", 31},
    {"mw5_javelin", 27},
    {"mw5_jenner", 26},
    {"mw5_locust", 31},
    {"mw5_longbow", 30},
    {"mw5_madcat", 31},
    {"mw5_maddog", 31},
    {"mw5_marauder", 31},
    {"mw5_orion", 31},
    {"mw5_panther", 31},
    {"mw5_puma", 15},
    {"mw5_quickdraw", 31},
    {"mw5_raven", 31},
    {"mw5_rifleman", 31},
    {"mw5_shadowcat", 31},
    {"mw5_shadowhawk", 24},
    {"mw5_stormcrow", 31},
    {"mw5_summoner", 31},
    {"mw5_urbanmech", 26},
    {"mw5_victor", 31},
    {"mw5_vindicator", 24},
    {"mw5_vulcan", 7},
    {"mw5_warhammer", 31},
    {"mw5_warhawk", 31},
    {"mw5_wolfhound", 30},
    {"mw5_zeus", 29},
};
inline bool contactCurveAllowed(std::string_view path, std::string_view clip) {
    const auto slash = path.find_last_of("/\\");
    auto stem = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
    if (const auto dot = stem.rfind('.'); dot != std::string_view::npos) stem = stem.substr(0, dot);
    constexpr std::string_view clips[] = {"atlas_moveCoreWalkFwd", "atlas_moveCoreRunFwd",
        "atlas_moveCoreWalkBwd", "atlas_moveCoreWalkLimpFwdLeft", "atlas_moveCoreWalkLimpFwdRight"};
    for (unsigned i = 0; i < 5; ++i)
        if (clip == clips[i])
            for (const auto& row : contactPasses)
                if (stem == row.chassis) return (row.gaits & (1u << i)) != 0;
    return false;
}
} // namespace mc2ground
