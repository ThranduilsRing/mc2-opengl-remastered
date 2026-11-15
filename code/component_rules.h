#pragma once
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace mc2::components {
// Cooked namespace and authored cells keep legacy overlays and resized footprints from cropping the new art.
struct CookedIcon { std::string_view source; const char* file; int width, height; };
inline const CookedIcon* cookedIcon(std::string_view file) {
    static constexpr CookedIcon icons[] = {
        {"mcl_mc_armorplating.tga", "mc2_component_armorplating.tga", 1, 1},
        {"mcl_mc_clanerlargelaser.tga", "mc2_component_clanerlargelaser.tga", 1, 2},
        {"mcl_mc_clanerlaser.tga", "mc2_component_clanerlaser.tga", 1, 1},
        {"mcl_mc_clanerppc.tga", "mc2_component_clanerppc.tga", 1, 3},
        {"mcl_mc_clangaussrifle.tga", "mc2_component_clangaussrifle.tga", 2, 4},
        {"mcl_mc_clanheavylargelaser.tga", "mc2_component_clanheavylargelaser.tga", 1, 2},
        {"mcl_mc_clanheavylaser.tga", "mc2_component_clanheavylaser.tga", 1, 1},
        {"mcl_mc_clanlrmrack.tga", "mc2_component_clanlrmrack.tga", 1, 1},
        {"mcl_mc_clanpulselargelaser.tga", "mc2_component_clanpulselargelaser.tga", 1, 2},
        {"mcl_mc_clanpulselaser.tga", "mc2_component_clanpulselaser.tga", 1, 1},
        {"mcl_mc_clanstreaksrmpack.tga", "mc2_component_clanstreaksrmpack.tga", 1, 1},
        {"mcl_mc_clanultraheavyautocannon.tga", "mc2_component_clanultraheavyautocannon.tga", 2, 4},
        {"mcl_mc_clanultralightautocannon.tga", "mc2_component_clanultralightautocannon.tga", 2, 2},
        {"mcl_mc_clanultramediumautocannon.tga", "mc2_component_clanultramediumautocannon.tga", 2, 3},
        {"mcl_mc_erlargelaser.tga", "mc2_component_erlargelaser.tga", 1, 2},
        {"mcl_mc_erlaser.tga", "mc2_component_erlaser.tga", 1, 1},
        {"mcl_mc_erppc.tga", "mc2_component_erppc.tga", 1, 3},
        {"mcl_mc_flamerarray.tga", "mc2_component_flamerarray.tga", 1, 1},
        {"mcl_mc_gaussrifle.tga", "mc2_component_gaussrifle.tga", 2, 4},
        {"mcl_mc_heatsink.tga", "mc2_component_heatsink.tga", 1, 1},
        {"mcl_mc_heavyautocannon.tga", "mc2_component_heavyautocannon.tga", 2, 4},
        {"mcl_mc_jumpjets.tga", "mc2_component_jumpjets.tga", 2, 2},
        {"mcl_mc_largelaser.tga", "mc2_component_largelaser.tga", 1, 2},
        {"mcl_mc_laser.tga", "mc2_component_laser.tga", 1, 1},
        {"mcl_mc_lightautocannon.tga", "mc2_component_lightautocannon.tga", 2, 2},
        {"mcl_mc_lightgaussrifle.tga", "mc2_component_lightgaussrifle.tga", 2, 3},
        {"mcl_mc_longtomcannon.tga", "mc2_component_longtomcannon.tga", 2, 5},
        {"mcl_mc_lrmrack.tga", "mc2_component_lrmrack.tga", 1, 1},
        {"mcl_mc_mediumautocannon.tga", "mc2_component_mediumautocannon.tga", 2, 3},
        {"mcl_mc_mgarray.tga", "mc2_component_mgarray.tga", 1, 1},
        {"mcl_mc_ppc.tga", "mc2_component_ppc.tga", 1, 3},
        {"mcl_mc_pulselargelaser.tga", "mc2_component_pulselargelaser.tga", 1, 2},
        {"mcl_mc_pulselaser.tga", "mc2_component_pulselaser.tga", 1, 1},
        {"mcl_mc_srmpack.tga", "mc2_component_srmpack.tga", 1, 1},
        {"mcl_mc_streaksrmpack.tga", "mc2_component_streaksrmpack.tga", 1, 1},
        {"mcl_mc_swarmlrmrack.tga", "mc2_component_swarmlrmrack.tga", 1, 1},
        {"mcl_mc_thunderboltmissile.tga", "mc2_component_thunderboltmissile.tga", 2, 4},
        {"mcl_mc_ultraheavyautocannon.tga", "mc2_component_ultraheavyautocannon.tga", 2, 4},
        {"mcl_mc_ultralightautocannon.tga", "mc2_component_ultralightautocannon.tga", 2, 2},
        {"mcl_mc_ultramediumautocannon.tga", "mc2_component_ultramediumautocannon.tga", 2, 3},
    };
    for (const auto& icon : icons) if (file == icon.source || file == icon.file) return &icon;
    return nullptr;
}
// Lowercase legacy launcher aliases resolve to the one-cell model cook.
inline const char* launcherIcon(std::string_view file) {
    constexpr std::pair<std::string_view, const char*> families[] = {
        {"clanlrm", "mcl_mc_clanlrmrack.tga"}, {"swarmlrm", "mcl_mc_swarmlrmrack.tga"},
        {"lrm", "mcl_mc_lrmrack.tga"}, {"clanstreaksrm", "mcl_mc_clanstreaksrmpack.tga"},
        {"streaksrm", "mcl_mc_streaksrmpack.tga"}, {"srm", "mcl_mc_srmpack.tga"}};
    for (const auto& [family, icon] : families) if (file.find(family) != file.npos) return icon;
    return nullptr;
}
// Variants without stock models share the same textured family preview as their icons.
inline const char* previewModel(std::string_view file) {
    constexpr std::pair<std::string_view, const char*> families[] = {
        {"clanlrm", "mcl_mc_clanlrmrack"}, {"swarmlrm", "mcl_mc_swarmlrmrack"},
        {"lrm", "mcl_mc_lrmrack"}, {"clanstreaksrm", "mcl_mc_clanstreaksrmpack"},
        {"streaksrm", "mcl_mc_streaksrmpack"}, {"srm", "mcl_mc_srmpack"},
        {"clanlightlbx", "mcl_mc_clanultralightautocannon"}, {"xlengine", "mcl_mc_heatsink"}};
    for (const auto& [family, model] : families) if (file.find(family) != file.npos) return model;
    return nullptr;
}
// Inactive storage can contain a previous loadout. Unmapped live components
// are preserved without indexing the logistics table with a missing location.
template<std::size_t N, class Item, class Location, class Logistics>
inline void resetSnapshot(int32_t (&out)[N], const Item (&inventory)[N], long active,
                          const Location (&locations)[N], const Logistics (&table)[N]) {
    for (std::size_t i = 0; i < N; ++i) out[i] = 0xff;
    for (long item = 0; item < active && item < static_cast<long>(N); ++item) {
        out[item] = inventory[item].masterID;
        if (out[item] == 0xff) continue;
        for (std::size_t location = 0; location < N; ++location) {
            if (locations[location] != item) continue;
            if (table[location].clearOut) out[item] = 0xff;
            break;
        }
    }
}

// Rack sizes are authored in the CSV name, never the localized display string.
// Unnumbered missiles (Thunderbolt) are single projectiles.
inline int rackSize(std::string_view name) {
    auto marker = name.find("LRM");
    if (marker == name.npos) marker = name.find("SRM");
    if (marker == name.npos) marker = name.find("Missile/");
    if (marker == name.npos) return 1;
    auto digit = name.find_first_of("0123456789", marker);
    if (digit == name.npos) return 1;
    int count = 0;
    while (digit < name.size() && name[digit] >= '0' && name[digit] <= '9') {
        count = count * 10 + name[digit++] - '0';
        if (count > 32) return 1;
    }
    return count > 0 ? count : 1;
}

inline float nextSpeedClass(float speed) {
    constexpr float ladder[] = {5, 10, 15, 19, 23, 27, 31, 35};
    for (int i = 0; i < 7; ++i)
        if (speed <= ladder[i]) return ladder[i + 1];
    return speed;
}

inline bool defensive(float damage, unsigned char range) {
    return damage == 0.0f && range == 255;
}

inline bool canIntercept(bool disabled, float ready, float now, long ammo) {
    return !disabled && ready <= now && ammo > 0;
}
}
