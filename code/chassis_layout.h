#ifndef CHASSIS_LAYOUT_H
#define CHASSIS_LAYOUT_H
// Phase A metadata only. Neither MoverSpec nor package firing mounts reads this record.
// locations.fit schema v1 (also permitted inside a package chassisdef.fit):
// [Layout] st ChassisId, l Version=1, st Source, f Payload (required, tons),
// l ArmorPool. Eight [Location.<name>] blocks require l Slots, l Structure,
// l ArmorMax, l ArmorRearMax, l HardpointCount. Each st HardpointN is a '|'
// separated mask of Energy/Ballistic/Missile/AntiPersonnel/AMS; optional st
// SocketN names a firing socket. Keys are FNV-1a("<location>.<N>").
// Slots are a free component-size budget, NOT a rectangle or inventory count.
// Slot indices identify items (0..11), as in the sim's criticalSpaces table.
#include "package_chassis.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

class FitIniFile;
namespace mc2pkg {
inline constexpr std::array<const char*, 8> locationNames = {
    "Head", "CenterTorso", "LeftTorso", "RightTorso", "LeftArm", "RightArm", "LeftLeg", "RightLeg"};
inline std::optional<int> locationIndex(std::string_view name) {
    for (int i = 0; i < 8; ++i) if (name == locationNames[i]) return i;
    return std::nullopt;
}
enum class WeaponMask : unsigned { Energy = 1, Ballistic = 2, Missile = 4, AntiPersonnel = 8, AMS = 16 };
inline constexpr unsigned Energy = static_cast<unsigned>(WeaponMask::Energy);
inline constexpr unsigned Ballistic = static_cast<unsigned>(WeaponMask::Ballistic);
inline constexpr unsigned Missile = static_cast<unsigned>(WeaponMask::Missile);
inline constexpr unsigned AntiPersonnel = static_cast<unsigned>(WeaponMask::AntiPersonnel);
inline constexpr unsigned AMS = static_cast<unsigned>(WeaponMask::AMS);
struct LocationMount { unsigned mask = 0, key = 0; std::string socket; };
struct ChassisLocation {
    int slots = 0, structure = 0, armorMax = 0, armorRearMax = 0;
    std::vector<LocationMount> hardpoints;
};
struct ChassisLayout {
    std::string chassisId, source;
    double payload = 0;
    int armorPool = 0;
    double maxHeat = std::numeric_limits<double>::max(); // adapter supplies chassis cooling
    std::array<ChassisLocation, 8> locations{};
    // Non-replaceable sim inventory (incl. standard engine); fixed equipment
    // in a designer CSV is represented explicitly instead, with this set to zero.
    std::array<int, 8> fixedInventory = {2, 2, 0, 0, 2, 2, 2, 2};
};
std::optional<ChassisLayout> readChassisLayout(FitIniFile&, std::string* error = nullptr);
std::optional<ChassisLayout> loadChassisLayout(std::string_view path, std::string* error = nullptr);

enum class EquipmentKind { Other, Weapon, Ammo, Sensor, Electronics, JumpJet, XLEngine, Fixed, HeatSink, Armor };
struct LayoutComponent {
    int id = 0, size = 1, width = 1, height = 1;
    unsigned allowed = 255, mountMask = 0;
    double weight = 0, heat = 0, cooling = 0;
    EquipmentKind kind = EquipmentKind::Other;
    bool implicitAmmo = false;
};
struct LocationItem {
    LayoutComponent component;
    int location = -1, slot = -1;
    unsigned hardpoint = 0;
};
enum class PlaceVerdict {
    Legal, LocationNotAllowed, NoFreeSlots, NoFreeHardpoint, OverPayload, TooHot,
    OneJumpJet, OneXLEngine, TooManyElectronics, SpecLimit, InventoryLimit, InvalidComponent
};
struct PlaceResult {
    PlaceVerdict verdict = PlaceVerdict::Legal;
    double overage = 0;
    int slot = -1;
    unsigned hardpoint = 0;
};

// Maximum matching permits omni pods without greedy assignment rejecting a legal
// loadout. Explicit saved keys are pinned; unmatched legacy keys are allocated.
inline std::optional<std::vector<unsigned>> matchLocationMounts(
    const ChassisLayout& layout, const std::vector<LocationItem>& items) {
    std::vector<unsigned> keys(items.size(), 0);
    for (int loc = 0; loc < 8; ++loc) {
        const auto& mounts = layout.locations[loc].hardpoints;
        std::vector<int> owners(mounts.size(), -1);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            if (item.location != loc || !item.component.mountMask || !item.hardpoint) continue;
            auto m = std::find_if(mounts.begin(), mounts.end(), [&](const LocationMount& h) {
                return h.key == item.hardpoint && (h.mask & item.component.mountMask);
            });
            if (m == mounts.end()) return std::nullopt;
            const auto j = static_cast<std::size_t>(m - mounts.begin());
            if (owners[j] != -1) return std::nullopt;
            owners[j] = static_cast<int>(i);
        }
        auto augment = [&](auto&& self, int i, std::vector<bool>& seen) -> bool {
            for (std::size_t j = 0; j < mounts.size(); ++j) {
                if (seen[j] || !(mounts[j].mask & items[i].component.mountMask)) continue;
                seen[j] = true;
                if (owners[j] < 0 || (!items[owners[j]].hardpoint && self(self, owners[j], seen))) {
                    owners[j] = i; return true;
                }
            }
            return false;
        };
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].location != loc || !items[i].component.mountMask || items[i].hardpoint) continue;
            std::vector<bool> seen(mounts.size(), false);
            if (!augment(augment, static_cast<int>(i), seen)) return std::nullopt;
        }
        for (std::size_t j = 0; j < mounts.size(); ++j)
            if (owners[j] >= 0) keys[owners[j]] = mounts[j].key;
    }
    return keys;
}

inline PlaceResult checkPlace(const ChassisLayout& layout, const std::vector<LocationItem>& placed,
                              const LocationItem& candidate, int location) {
    if (location < 0 || location >= 8 || !(candidate.component.allowed & (1u << location)))
        return {PlaceVerdict::LocationNotAllowed};
    if (candidate.component.size < 0 || !std::isfinite(candidate.component.weight) ||
        candidate.component.weight < 0 || !std::isfinite(candidate.component.heat) ||
        !std::isfinite(candidate.component.cooling)) return {PlaceVerdict::InvalidComponent};
    auto all = placed;
    auto cand = candidate;
    cand.location = location;
    int used = 0, count = layout.fixedInventory[location];
    std::array<bool, 12> occupied{};
    for (const auto& p : placed) if (p.location == location) {
        used += p.component.size; ++count;
        if (p.slot >= 0 && p.slot < 12) occupied[p.slot] = true;
    }
    if (cand.slot < 0) {
        for (int s = 0; s < 12; ++s) if (!occupied[s]) { cand.slot = s; break; }
    }
    if (cand.slot < 0 || cand.slot >= 12 || occupied[cand.slot] || count >= 12 ||
        used + cand.component.size > layout.locations[location].slots) return {PlaceVerdict::NoFreeSlots};
    all.push_back(cand);
    if (all.size() > 50) return {PlaceVerdict::SpecLimit};
    int inventory = 0, jump = 0, xl = 0, electronics = 0, weaponsAmmo = 0;
    for (int n : layout.fixedInventory) inventory += n;
    double mass = 0, heat = 0, cooling = layout.maxHeat;
    for (const auto& p : all) {
        const auto& c = p.component;
        mass += c.weight; heat += c.heat; cooling += c.cooling;
        jump += c.kind == EquipmentKind::JumpJet;
        xl += c.kind == EquipmentKind::XLEngine;
        electronics += c.kind == EquipmentKind::Electronics;
        const bool wa = c.kind == EquipmentKind::Weapon || c.kind == EquipmentKind::Ammo;
        weaponsAmmo += wa ? 1 + static_cast<int>(c.implicitAmmo) : 0;
        if (c.kind != EquipmentKind::HeatSink && c.kind != EquipmentKind::Armor &&
            c.kind != EquipmentKind::XLEngine) inventory += 1 + static_cast<int>(c.implicitAmmo);
    }
    if (jump > 1) return {PlaceVerdict::OneJumpJet};
    if (xl > 1) return {PlaceVerdict::OneXLEngine};
    if (electronics > 3) return {PlaceVerdict::TooManyElectronics};
    // resetComponents starts weapon/ammo insertion at 17 and permits 54 entries.
    if (inventory > 72 || weaponsAmmo > 54) return {PlaceVerdict::InventoryLimit};
    const auto keys = matchLocationMounts(layout, all);
    if (!keys) return {PlaceVerdict::NoFreeHardpoint};
    if (mass > layout.payload + 1e-6) return {PlaceVerdict::OverPayload, mass - layout.payload};
    if (heat > cooling + 1e-6) return {PlaceVerdict::TooHot};
    return {PlaceVerdict::Legal, 0, cand.slot, keys->back()};
}

struct VariantItem {
    int id = 0, x = 0, y = 0, legacyLocation = 0;
    int location = -1, slot = -1;
    unsigned hardpoint = 0;
};
struct LocationVariant {
    int version = 1;
    std::vector<VariantItem> items; // always original order, including grandfathered items
    std::array<int, 11> armor{};
    bool grandfathered = false, edited = false, classicReadOnly = false;
};
inline bool canSaveLocationVariant(const LocationVariant& record) { return !record.grandfathered; }
// Shared production FIT adapters. A malformed record fails atomically; legality
// is checked AFTER restoring the complete list, never by addComponent/grid fit.
std::optional<LocationVariant> readLocationVariant(FitIniFile&);
bool writeLocationVariant(FitIniFile&, const LocationVariant&);

inline bool validateLocations(const ChassisLayout& layout, LocationVariant& record,
                              const std::vector<LayoutComponent>& components) {
    if (components.size() != record.items.size()) { record.grandfathered = true; return false; }
    std::vector<LocationItem> all;
    bool legal = true;
    for (std::size_t i = 0; i < record.items.size(); ++i) {
        const auto& item = record.items[i];
        all.push_back({components[i], item.location, item.slot, item.hardpoint});
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        auto others = all;
        others.erase(others.begin() + static_cast<std::ptrdiff_t>(i));
        if (checkPlace(layout, others, all[i], all[i].location).verdict != PlaceVerdict::Legal) legal = false;
    }
    record.grandfathered = !legal;
    return legal;
}

inline void mapLegacyLocations(const ChassisLayout& layout, LocationVariant& record,
                               const std::vector<LayoutComponent>& components,
                               const PackageChassis* package = nullptr) {
    if (record.items.size() != components.size()) { record.grandfathered = true; return; }
    // Physical critical entries copied from logisticsTable, starting at index 17.
    std::vector<std::pair<int, int>> roundRobin = {{6,4},{6,5},{7,3},{7,4},{7,5}};
    for (int s = 0; s < 8; ++s) {
        roundRobin.emplace_back(4, s + 4); roundRobin.emplace_back(2, s);
        roundRobin.emplace_back(1, s + 2); roundRobin.emplace_back(3, s); roundRobin.emplace_back(5, s + 4);
    }
    roundRobin.insert(roundRobin.end(), {{2,8},{1,10},{3,8},{2,9},{1,11},{3,9},{2,10},{3,10},{2,11},{3,11}});
    std::size_t cursor = 0;
    int electronics = 0;
    std::array<int, 8> next{};
    std::array<int, 4> pairedFixed{};
    for (std::size_t i = 0; i < record.items.size(); ++i) {
        auto& item = record.items[i]; const auto& c = components[i];
        if (c.kind == EquipmentKind::Fixed) {
            if (c.id == 1) { item.location = 1; item.slot = 0; }
            else if (c.id == 22) { item.location = 1; item.slot = 1; }
            else if (c.id == 6) { item.location = 0; item.slot = 0; }
            else if (c.id == 35) { item.location = 0; item.slot = 1; }
            else if (c.id == 4 || c.id == 33) {
                const int pair = c.id == 4 ? 0 : 1;
                item.location = 4 + pairedFixed[pair]++; item.slot = c.id == 4 ? 1 : 0;
            } else if (c.id == 5 || c.id == 34) {
                const int pair = c.id == 5 ? 2 : 3;
                item.location = 6 + pairedFixed[pair]++; item.slot = c.id == 5 ? 1 : 0;
            }
        }
        else if (c.kind == EquipmentKind::Sensor) { item.location = 0; item.slot = 2; }
        else if (c.kind == EquipmentKind::Electronics) { item.location = 0; item.slot = 3 + electronics++; }
        else if (c.kind == EquipmentKind::JumpJet) { item.location = 6; item.slot = 3; }
        else if (c.kind == EquipmentKind::XLEngine) { item.location = 1; item.slot = 0; }
        else if (c.kind == EquipmentKind::Weapon || c.kind == EquipmentKind::Ammo) {
            if (cursor < roundRobin.size()) { const auto [location, slot] = roundRobin[cursor]; item.location = location; item.slot = slot; }
            cursor += 1 + static_cast<int>(c.implicitAmmo);
        } else { item.location = -1; item.slot = -1; }
        if (package && c.kind != EquipmentKind::Fixed) {
            const auto* region = regionForFootprint(*package, item.x, item.y, c.width, c.height);
            if (region) { item.location = locationIndex(region->location).value_or(-1); item.slot = next[item.location >= 0 ? item.location : 0]++; }
        }
    }
    // Heat sinks/armor have no resetComponents inventory entry. Give their lab
    // metadata a free allowed slot after the sim-visible equipment is mapped.
    for (std::size_t i = 0; i < record.items.size(); ++i) {
        auto& item = record.items[i]; const auto& c = components[i];
        if (item.location >= 0) continue;
        if (c.kind == EquipmentKind::Weapon || c.kind == EquipmentKind::Ammo) continue;
        for (int loc = 0; loc < 8 && item.location < 0; ++loc) if (c.allowed & (1u << loc)) {
            for (int s = 0; s < 12; ++s) {
                bool taken = false;
                for (const auto& other : record.items) if (other.location == loc && other.slot == s) taken = true;
                if (!taken) { item.location = loc; item.slot = s; break; }
            }
        }
    }
    std::vector<LocationItem> placed;
    for (std::size_t i = 0; i < record.items.size(); ++i)
        placed.push_back({components[i], record.items[i].location, record.items[i].slot, record.items[i].hardpoint});
    if (const auto keys = matchLocationMounts(layout, placed))
        for (std::size_t i = 0; i < record.items.size(); ++i) record.items[i].hardpoint = (*keys)[i];
    record.version = 2;
    validateLocations(layout, record, components);
}

inline PlaceResult moveLocationItem(const ChassisLayout& layout, LocationVariant& record,
                                    const std::vector<LayoutComponent>& specs, std::size_t index, int location) {
    if (specs.size() != record.items.size() || index >= specs.size()) return {PlaceVerdict::InvalidComponent};
    if (specs[index].kind == EquipmentKind::Fixed) return {PlaceVerdict::InvalidComponent};
    std::vector<LocationItem> placed;
    for (std::size_t i = 0; i < specs.size(); ++i) if (i != index)
        placed.push_back({specs[i], record.items[i].location, record.items[i].slot, record.items[i].hardpoint});
    auto result = checkPlace(layout, placed, {specs[index], location, -1, 0}, location);
    if (result.verdict == PlaceVerdict::Legal) {
        auto& item = record.items[index]; item.location = location; item.slot = result.slot; item.hardpoint = result.hardpoint;
        record.version = 2; record.edited = true;
        validateLocations(layout, record, specs);
    }
    return result;
}

// Repack only after a location edit. Failure keeps every original coordinate and
// item and makes CLASSIC read-only. Stable largest-height/width first, as init().
inline bool mapLocationsToLegacy(LocationVariant& record, const std::vector<LayoutComponent>& components,
                                 int width, int height, const PackageChassis* package = nullptr) {
    if (!record.edited) return true;
    if (components.size() != record.items.size() || width <= 0 || height <= 0) return false;
    auto trial = record.items;
    std::vector<std::size_t> order(trial.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (components[a].height != components[b].height) return components[a].height > components[b].height;
        return components[a].width > components[b].width;
    });
    std::vector<bool> occupied(static_cast<std::size_t>(width) * height, false);
    for (auto i : order) {
        const auto& c = components[i]; auto& item = trial[i];
        if (c.kind == EquipmentKind::Fixed) continue;
        if (c.kind == EquipmentKind::JumpJet) { item.x = item.y = -2; continue; }
        if (c.kind == EquipmentKind::Sensor || c.kind == EquipmentKind::Electronics) { item.x = item.y = -3; continue; }
        bool found = false;
        for (int y = 0; y + c.height <= height && !found; ++y) for (int x = 0; x + c.width <= width && !found; ++x) {
            if (c.width <= 0 || c.height <= 0) break;
            if (package) {
                const auto* r = regionForFootprint(*package, x, y, c.width, c.height);
                if (!r || locationIndex(r->location).value_or(-1) != item.location) continue;
            }
            bool free = true;
            for (int dy = 0; dy < c.height; ++dy) for (int dx = 0; dx < c.width; ++dx)
                if (occupied[(y + dy) * width + x + dx]) free = false;
            if (!free) continue;
            item.x = x; item.y = y; found = true;
            for (int dy = 0; dy < c.height; ++dy) for (int dx = 0; dx < c.width; ++dx)
                occupied[(y + dy) * width + x + dx] = true;
        }
        if (!found) { record.classicReadOnly = true; return false; }
    }
    record.items = std::move(trial); record.classicReadOnly = false;
    return true;
}
} // namespace mc2pkg
#endif
