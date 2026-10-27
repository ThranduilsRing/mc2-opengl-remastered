// weapon_csv.h — engine-free compbas.csv / effects.csv model for the weapon GUI.
// Same data model + validation rules as tools/mc2weapon/mc2weapon.py; edits write
// a loose mods/<id>/data/objects/compbas.csv overlay (the game resolves it first).
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace mc2w {

// Logical column indices into a compbas row, resolved by header text.
struct ColIdx {
    int masterID = -1, type = -1, name = -1, slots = -1, recycle = -1, heat = -1,
        tons = -1, damage = -1, br = -1, rp = -1, range = -1, missileType = -1,
        fields = -1, fxid = -1, ammoMasterId = -1, iconX = -1, iconY = -1;
};

struct FxEntry {
    int id = 0;
    std::string name, muzzle, hit, miss, objNum, weaponName;
};

struct Compbas {
    std::string srcPath;
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;  // data rows (after header)
    ColIdx idx;

    bool load(const std::string& path, std::string& err);
    bool write(const std::string& path, std::string& err) const;
    int findByMasterId(std::string_view mid) const;  // -1 if none

    // cell access by logical field name for the selected row (safe, "" if OOB).
    std::string cell(int row, int colIdx) const;
    void setCell(int row, int colIdx, std::string_view v);
};

bool loadEffects(const std::string& path, std::vector<FxEntry>& out, std::string& err);
bool isWeaponType(std::string_view type);

// "" if valid, else a human message. `kind`: ufloat|uint|int|range|wtype|fxid|str.
std::string validateCell(std::string_view kind, std::string_view value,
                         const std::vector<FxEntry>& fx);

// Whole-row validation -> list of problems (empty = valid).
std::vector<std::string> validateRow(const Compbas& cb, int row,
                                     const std::vector<FxEntry>& fx);

// Write overlay: <modRoot>/<modId>/data/objects/compbas.csv + mod.json (if absent).
bool writeOverlay(const std::string& modRoot, const std::string& modId,
                  const Compbas& cb, std::string& outPath, std::string& err);

extern const char* kWeaponTypes[3];   // EnergyWeapon, BallisticWeapon, MissileWeapon
extern const char* kRanges[3];        // short, medium, long

}  // namespace mc2w
