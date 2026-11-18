#include "doctest.h"
#include "chassis_layout.h"
#include "armor_alloc.h"
#include "inifile.h"
#include "mc2fmt.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <iostream>
#include <cstring>
#include <functional>
#include <expected>
#include "Gates/gate_cold.h"

namespace {
using namespace mc2pkg;
const auto root = std::filesystem::path(MC2_LAYOUT_SOURCE_ROOT);
struct MemFit {
    std::string bytes;
    FitIniFile file;
    explicit MemFit(std::string body) : bytes("FITini\r\n" + std::move(body) + "\r\nFITend\r\n") {
        REQUIRE(file.open(bytes.data(), static_cast<int>(bytes.size())) == NO_ERR);
    }
    ~MemFit() { file.close(); }
};
ChassisLayout roomy() {
    ChassisLayout l; l.payload = 10000; l.fixedInventory.fill(0);
    for (int loc = 0; loc < 8; ++loc) {
        l.locations[loc].slots = 100;
        for (int n = 0; n < 12; ++n)
            l.locations[loc].hardpoints.push_back({31, hardpointKey(std::string(locationNames[loc]) + "." + std::to_string(n)), {}});
    }
    return l;
}
LayoutComponent weapon() {
    LayoutComponent c; c.id = 40; c.kind = EquipmentKind::Weapon; c.mountMask = Energy; c.weight = 2;
    return c;
}
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out; std::string part; bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') { part += ch; ++i; }
            else quoted = !quoted;
        } else if (ch == ',' && !quoted) { out.push_back(part); part.clear(); }
        else if (ch != '\r') part += ch;
    }
    out.push_back(part); return out;
}
std::vector<std::vector<std::string>> csv(const std::filesystem::path& path) {
    std::ifstream f(path); REQUIRE(f.good());
    std::vector<std::vector<std::string>> rows; std::string line;
    while (std::getline(f, line)) rows.push_back(fields(line));
    return rows;
}
double numeric(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
LayoutComponent csvComponent(const std::vector<std::string>& row) {
    REQUIRE(row.size() >= 29);
    LayoutComponent c; c.id = static_cast<int>(numeric(row[0])); c.size = static_cast<int>(numeric(row[3]));
    c.weight = numeric(row[6]); c.heat = numeric(row[5]); c.allowed = 0;
    c.width = static_cast<int>(numeric(row[27])); c.height = static_cast<int>(numeric(row[28]));
    for (int loc = 0; loc < 8; ++loc) if (row[11 + loc] == "Yes" || numeric(row[11 + loc]) > 0) c.allowed |= 1u << loc;
    const auto& type = row[1];
    if (type == "EnergyWeapon" || type == "Weapon") { c.kind = EquipmentKind::Weapon; c.mountMask = Energy; }
    else if (type == "BallisticWeapon") { c.kind = EquipmentKind::Weapon; c.mountMask = Ballistic; }
    else if (type == "MissileWeapon") { c.kind = EquipmentKind::Weapon; c.mountMask = Missile; }
    else if (type == "Ammo") c.kind = EquipmentKind::Ammo;
    else if (type == "Sensors") c.kind = EquipmentKind::Sensor;
    else if (type == "ECM" || type == "Probe" || type == "Jammer") c.kind = EquipmentKind::Electronics;
    else if (type == "JumpJet") c.kind = EquipmentKind::JumpJet;
    else if (type == "HeatSink") { c.kind = EquipmentKind::HeatSink; c.cooling = numeric(row[7]); }
    else if (type == "Bulk") c.kind = EquipmentKind::Armor;
    else c.kind = EquipmentKind::Fixed;
    c.implicitAmmo = c.kind == EquipmentKind::Weapon && numeric(row[22]) > 0;
    return c;
}
}

TEST_CASE("chassis_layout: refusal precedence and hard limits") {
    auto l = roomy(); auto c = weapon(); LocationItem candidate{c, 4, -1, 0};
    CHECK(checkPlace(l, {}, candidate, 4).verdict == PlaceVerdict::Legal);
    candidate.component.allowed = 1;
    CHECK(checkPlace(l, {}, candidate, 4).verdict == PlaceVerdict::LocationNotAllowed);
    candidate.component = c; l.locations[4].slots = 0;
    CHECK(checkPlace(l, {}, candidate, 4).verdict == PlaceVerdict::NoFreeSlots);
    l.locations[4].slots = 100; l.locations[4].hardpoints.clear();
    CHECK(checkPlace(l, {}, candidate, 4).verdict == PlaceVerdict::NoFreeHardpoint);
    l = roomy(); l.payload = 1;
    const auto mass = checkPlace(l, {}, candidate, 4);
    CHECK(mass.verdict == PlaceVerdict::OverPayload); CHECK(mass.overage == doctest::Approx(1));
    l = roomy(); l.maxHeat = 1; candidate.component.heat = 2;
    CHECK(checkPlace(l, {}, candidate, 4).verdict == PlaceVerdict::TooHot);
    l = roomy(); candidate.component = c;
    std::vector<LocationItem> placed;
    for (int i = 0; i < 12; ++i) placed.push_back({c, 4, i, 0});
    CHECK(checkPlace(l, placed, candidate, 4).verdict == PlaceVerdict::NoFreeSlots);
    placed.clear();
    for (int i = 0; i < 50; ++i) placed.push_back({c, i % 4, i / 4, 0});
    CHECK(checkPlace(l, placed, candidate, 4).verdict == PlaceVerdict::SpecLimit);
    placed.clear();
    auto ammoWeapon = c; ammoWeapon.implicitAmmo = true;
    for (int i = 0; i < 27; ++i) placed.push_back({ammoWeapon, i % 4, i / 4, 0});
    candidate.component = ammoWeapon;
    CHECK(checkPlace(l, placed, candidate, 4).verdict == PlaceVerdict::InventoryLimit);
    placed.clear(); candidate.component = c; candidate.component.kind = EquipmentKind::JumpJet;
    placed.push_back({candidate.component, 6, 3, 0});
    CHECK(checkPlace(l, placed, candidate, 4).verdict == PlaceVerdict::OneJumpJet);
    candidate.component.kind = EquipmentKind::XLEngine; placed[0].component = candidate.component;
    CHECK(checkPlace(l, placed, candidate, 4).verdict == PlaceVerdict::OneXLEngine);
    candidate.component.kind = EquipmentKind::Electronics;
    placed.assign(3, {candidate.component, 0, -1, 0});
    CHECK(checkPlace(l, placed, candidate, 4).verdict == PlaceVerdict::TooManyElectronics);
}

TEST_CASE("chassis_layout: omni matching and pinned keys") {
    auto l = roomy(); l.locations[4].hardpoints = {{Energy | Ballistic, 10, {}}, {Energy, 11, {}}};
    auto energy = weapon(), ballistic = energy; ballistic.mountMask = Ballistic;
    std::vector<LocationItem> placed = {{energy, 4, 0, 0}};
    auto result = checkPlace(l, placed, {ballistic, 4, 1, 0}, 4);
    CHECK(result.verdict == PlaceVerdict::Legal); CHECK(result.hardpoint == 10);
    placed[0].hardpoint = 10;
    CHECK(checkPlace(l, placed, {ballistic, 4, 1, 0}, 4).verdict == PlaceVerdict::NoFreeHardpoint);
}

TEST_CASE("chassis_layout: legacy electronics fixed equipment and implicit ammo boundaries") {
    auto l = roomy(); LocationVariant record; std::vector<LayoutComponent> specs;
    auto add = [&](EquipmentKind kind, bool ammo = false) {
        auto c = weapon(); c.kind = kind; c.implicitAmmo = ammo;
        if (kind != EquipmentKind::Weapon) c.mountMask = 0;
        specs.push_back(c); record.items.push_back({c.id, static_cast<int>(specs.size()), 2, 1});
    };
    add(EquipmentKind::Sensor);
    for (int n = 0; n < 4; ++n) add(EquipmentKind::Electronics);
    add(EquipmentKind::JumpJet); add(EquipmentKind::XLEngine);
    add(EquipmentKind::Weapon, true); add(EquipmentKind::Weapon);
    mapLegacyLocations(l, record, specs);
    CHECK(record.items[0].location == 0); CHECK(record.items[0].slot == 2);
    CHECK(record.items[3].slot == 5); CHECK(record.grandfathered);
    CHECK(record.items[5].location == 6); CHECK(record.items[5].slot == 3);
    CHECK(record.items[6].location == 1); CHECK(record.items[6].slot == 0);
    CHECK(record.items[7].location == 6); CHECK(record.items[7].slot == 4);
    CHECK(record.items[8].location == 7); CHECK(record.items[8].slot == 3); // ammo consumed LL slot5
    CHECK(record.items.size() == specs.size());
    record.items.erase(record.items.begin() + 4); specs.erase(specs.begin() + 4);
    mapLegacyLocations(l, record, specs); CHECK_FALSE(record.grandfathered);
    record.edited = true;
    CHECK(mapLocationsToLegacy(record, specs, 8, 8));
    CHECK(record.items[0].x == -3); CHECK(record.items[4].x == -2);
    auto fixed = weapon(); fixed.kind = EquipmentKind::Fixed; fixed.mountMask = 0;
    fixed.id = 1;
    LocationVariant exact; exact.items = {{1, -1, -1, 1, 1, 0, 0}}; exact.edited = true;
    CHECK(mapLocationsToLegacy(exact, {fixed}, 1, 1)); CHECK(exact.items[0].x == -1);
    mapLegacyLocations(l, exact, {fixed}); CHECK(exact.items[0].location == 1); CHECK(exact.items[0].slot == 0);
    CHECK(moveLocationItem(l, exact, {fixed}, 0, 4).verdict == PlaceVerdict::InvalidComponent);
}

TEST_CASE("chassis_layout: schema requires payload and all locations") {
    auto schema = [](bool payload) {
        std::string s = "[Layout]\r\nst ChassisId = \"core:atlas\"\r\nl Version = 1\r\nst Source = \"test\"\r\nl ArmorPool = 432\r\n";
        if (payload) s += "f Payload = 73.0\r\n";
        for (int loc = 0; loc < 8; ++loc) {
            s += std::string("[Location.") + locationNames[loc] + "]\r\nl Slots = 8\r\nl Structure = 17\r\nl ArmorMax = 34\r\nl ArmorRearMax = 0\r\nl HardpointCount = 1\r\nst Hardpoint0 = \"Energy|Ballistic|AMS\"\r\n";
        }
        return s;
    };
    MemFit good(schema(true)), bad(schema(false));
    auto layout = readChassisLayout(good.file); REQUIRE(layout);
    CHECK(layout->payload == 73); CHECK(layout->locations[4].hardpoints[0].mask == (Energy | Ballistic | AMS));
    CHECK(layout->locations[4].hardpoints[0].key == hardpointKey("LeftArm.0"));
    std::string error; CHECK_FALSE(readChassisLayout(bad.file, &error)); CHECK_FALSE(error.empty());
}

TEST_CASE("chassis_layout: production FIT adapter old save and exact v1 v2 v1 coordinates") {
    FitIniFile old; REQUIRE(old.open((root / "tests/unit/fixtures/chassis_legacy.fit").string().c_str()) == NO_ERR);
    REQUIRE(old.seekBlock("Variant0") == NO_ERR);
    auto decoded = readLocationVariant(old); REQUIRE(decoded); auto record = *decoded; CHECK(record.version == 1); CHECK_FALSE(record.armorExplicit);
    const auto original = record.items;
    auto energy = weapon(); LayoutComponent sink; sink.id = 7; sink.kind = EquipmentKind::HeatSink;
    LayoutComponent sensor; sensor.id = 13; sensor.kind = EquipmentKind::Sensor;
    std::vector<LayoutComponent> specs = {energy, sink, sensor}; auto l = roomy();
    mapLegacyLocations(l, record, specs); REQUIRE_FALSE(record.grandfathered);
    CHECK(mapLocationsToLegacy(record, specs, 1, 1)); // unchanged must NEVER repack
    const auto path = std::filesystem::temp_directory_path() / "mc2_chassis_variant_roundtrip.fit";
    { FitIniFile out; REQUIRE(out.create(path.string().c_str()) == NO_ERR); out.writeBlock("Variant0");
      record.armor[10] = 217; record.items[0].hardpoint = 0xfedcba98u;
      REQUIRE(writeLocationVariant(out, record)); out.close(); }
    FitIniFile saved; REQUIRE(saved.open(path.string().c_str()) == NO_ERR); REQUIRE(saved.seekBlock("Variant0") == NO_ERR);
    auto savedRecord = readLocationVariant(saved); REQUIRE(savedRecord); auto read = *savedRecord;
    CHECK(read.version == 2); CHECK(read.armor[10] == 217); CHECK(read.items[0].hardpoint == 0xfedcba98u);
    REQUIRE(read.items.size() == original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        CHECK(read.items[i].id == original[i].id); CHECK(read.items[i].x == original[i].x);
        CHECK(read.items[i].y == original[i].y); CHECK(read.items[i].legacyLocation == original[i].legacyLocation);
    }
    l.payload = 0; validateLocations(l, read, specs); CHECK(read.grandfathered); CHECK(read.items.size() == 3);
    saved.close(); std::filesystem::remove(path);
    MemFit truncated("[Variant0]\r\nl ComponentCount = 1\r\nl Component0 = 40");
    REQUIRE(truncated.file.seekBlock("Variant0") == NO_ERR);
    CHECK_FALSE(readLocationVariant(truncated.file)); CHECK(read.items.size() == 3);
    record.edited = true; CHECK_FALSE(mapLocationsToLegacy(record, specs, 1, 1));
    CHECK(record.classicReadOnly); CHECK(record.items[0].x == original[0].x);
}

TEST_CASE("chassis_layout: installed stock variants production save round trip") {
    const auto install = mc2::gate::str<"MC2_GATE_INSTALL">();
    if (install.empty()) { MESSAGE("installed corpus unavailable; MC2_GATE_INSTALL unset"); return; }
    const auto directory = std::filesystem::path(install) / "data" / "objects";
    REQUIRE(std::filesystem::is_directory(directory));
    int chassis = 0, variants = 0, items = 0;
    const auto path = std::filesystem::temp_directory_path() / "mc2_installed_variant.fit";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".csv") continue;
        const auto rows = csv(entry.path()); bool found = false;
        for (std::size_t start = 0; start < rows.size(); ++start) {
            if (rows[start].empty() || rows[start][0] != "VariantInfo") continue;
            const auto csvName = entry.path().filename().string(); CAPTURE(csvName); CAPTURE(start); found = true; ++variants;
            LocationVariant original;
            for (auto n = start + 1; n < rows.size() && rows[n][0] != "ComponentPlacement"; ++n) {
                const auto& row = rows[n]; if (row.size() < 5 || row[0].find("Item") != 0) continue;
                const int id = static_cast<int>(numeric(row[4])); if (id == 255) continue;
                original.items.push_back({id, static_cast<int>(numeric(row[2])), static_cast<int>(numeric(row[3])), 1});
            }
            items += static_cast<int>(original.items.size());
            FitIniFile saved; REQUIRE(saved.create(path.string().c_str()) == NO_ERR); saved.writeBlock("Variant0");
            REQUIRE(writeLocationVariant(saved, original)); saved.close();
            REQUIRE(saved.open(path.string().c_str()) == NO_ERR); REQUIRE(saved.seekBlock("Variant0") == NO_ERR);
            auto restored = readLocationVariant(saved); REQUIRE(restored); REQUIRE(restored->items.size() == original.items.size());
            for (std::size_t n = 0; n < original.items.size(); ++n) {
                CHECK(restored->items[n].id == original.items[n].id);
                CHECK(restored->items[n].x == original.items[n].x); CHECK(restored->items[n].y == original.items[n].y);
                CHECK(restored->items[n].legacyLocation == original.items[n].legacyLocation);
            }
            saved.close(); std::filesystem::remove(path);
        }
        chassis += found;
    }
    CHECK(chassis == 68); CHECK(variants == 146);
    std::cout << "INSTALLED_CHASSIS_CORPUS chassis=" << chassis << " variants=" << variants << " items=" << items << '\n';
}

TEST_CASE("chassis_layout: every repository stock designer variant uses exact CSV placement") {
    const auto directory = root / "tools/unit_converter/tests/fixtures/stock/objects";
    auto compRows = csv(directory / "compbas.csv");
    std::map<int, LayoutComponent> comp;
    for (std::size_t i = 1; i < compRows.size(); ++i) {
        if (compRows[i][0].empty()) continue;
        compRows[i].resize(std::max<std::size_t>(29, compRows[i].size()));
        auto c = csvComponent(compRows[i]); comp[c.id] = c;
    }
    int chassisCount = 0, variantCount = 0, itemCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".csv" || entry.path().filename() == "compbas.csv") continue;
        ++chassisCount; const auto rows = csv(entry.path());
        std::vector<LocationVariant> variants; std::vector<std::vector<LayoutComponent>> components;
        for (std::size_t start = 0; start < rows.size(); ++start) {
            if (rows[start][0] != "VariantInfo") continue;
            ++variantCount; LocationVariant v; v.version = 2;
            std::vector<LayoutComponent> specs; std::map<int, std::size_t> indices;
            std::size_t placement = start;
            for (; placement < rows.size() && rows[placement][0] != "ComponentPlacement"; ++placement) {
                const auto& row = rows[placement];
                if (row.size() < 5 || row[0].find("Item") != 0) continue;
                const int id = static_cast<int>(numeric(row[4])); if (id == 255) continue;
                REQUIRE(comp.count(id) == 1);
                indices[static_cast<int>(numeric(row[0].substr(4)))] = v.items.size();
                v.items.push_back({id, static_cast<int>(numeric(row[2])), static_cast<int>(numeric(row[3])), 1});
                specs.push_back(comp.at(id));
            }
            REQUIRE(placement + 12 < rows.size());
            static constexpr int columns[] = {0, 4, 5, 2, 3, 1, 6, 7};
            for (int slot = 0; slot < 12; ++slot) for (int col = 0; col < 8; ++col) {
                const auto& row = rows[placement + slot + 1];
                if (row.size() <= static_cast<std::size_t>(col + 1) || row[col + 1].empty()) continue;
                auto it = indices.find(static_cast<int>(numeric(row[col + 1])));
                if (it == indices.end()) continue; // empty 255 inventory entries
                auto& item = v.items[it->second];
                if (item.location < 0) { item.location = columns[col]; item.slot = slot; }
            }
            itemCount += static_cast<int>(v.items.size()); variants.push_back(v); components.push_back(specs);
        }
        REQUIRE_FALSE(variants.empty());
        auto layout = roomy();
        layout.payload = numeric(rows.at(2).at(4)) * 0.8 - 7; // tonnage - (20% tonnage + 7)
        for (auto& loc : layout.locations) { loc.slots = 0; loc.hardpoints.clear(); }
        for (std::size_t n = 0; n < variants.size(); ++n) {
            std::array<int, 8> sizes{}; std::array<std::map<unsigned, int>, 8> mounts;
            double mass = 0;
            for (std::size_t i = 0; i < variants[n].items.size(); ++i) {
                const auto& item = variants[n].items[i]; const auto& c = components[n][i];
                REQUIRE(item.location >= 0); sizes[item.location] += c.size; mass += c.weight;
                if (c.mountMask) ++mounts[item.location][c.mountMask];
            }
            layout.payload = std::max(layout.payload, mass);
            for (int loc = 0; loc < 8; ++loc) {
                layout.locations[loc].slots = std::max(layout.locations[loc].slots, sizes[loc]);
                for (const auto& m : mounts[loc]) {
                    int count = 0;
                    for (const auto& h : layout.locations[loc].hardpoints) if (h.mask == m.first) ++count;
                    for (; count < m.second; ++count)
                        layout.locations[loc].hardpoints.push_back({m.first, hardpointKey(std::string(locationNames[loc]) + "." + std::to_string(layout.locations[loc].hardpoints.size())), {}});
                }
            }
        }
        for (std::size_t n = 0; n < variants.size(); ++n) {
            CAPTURE(n); CHECK(validateLocations(layout, variants[n], components[n]));
            const auto& original = variants[n];
            const auto path = std::filesystem::temp_directory_path() / ("mc2_stock_layout_" + std::to_string(n) + ".fit");
            {
                FitIniFile saved;
                REQUIRE(saved.create(path.string().c_str()) == NO_ERR);
                saved.writeBlock("Variant0");
                REQUIRE(writeLocationVariant(saved, original));
                saved.close();
            }
            FitIniFile saved;
            REQUIRE(saved.open(path.string().c_str()) == NO_ERR);
            REQUIRE(saved.seekBlock("Variant0") == NO_ERR);
            auto savedRecord = readLocationVariant(saved);
            REQUIRE(savedRecord);
            auto restored = *savedRecord;
            REQUIRE(restored.items.size() == original.items.size());
            CHECK(validateLocations(layout, restored, components[n]));
            for (std::size_t i = 0; i < original.items.size(); ++i) {
                CHECK(restored.items[i].id == original.items[i].id);
                CHECK(restored.items[i].x == original.items[i].x);
                CHECK(restored.items[i].y == original.items[i].y);
                CHECK(restored.items[i].location == original.items[i].location);
                CHECK(restored.items[i].slot == original.items[i].slot);
            }
            saved.close();
            std::filesystem::remove(path);
        }
    }
    CHECK(chassisCount == 1); CHECK(variantCount == 3);
    std::cout << "CHASSIS_CORPUS chassis=" << chassisCount << " variants=" << variantCount << " items=" << itemCount << '\n';
}

// Compile the production restore and deployment adapters verbatim. The doubles
// supply only the chassis/component database; no simulated deployment copy.
namespace deployment_seam {
enum { COMPONENT_FORM_WEAPON_ENERGY, COMPONENT_FORM_WEAPON_BALLISTIC, COMPONENT_FORM_WEAPON_MISSILE, COMPONENT_FORM_WEAPON, COMPONENT_FORM_BULK, OTHER };
enum LOGISTICS_COMPONENT_LOCATION { HEAD, CHEST, LEGS };
constexpr int NEED_BIGGER_ARRAY = -10;
struct LogisticsComponent {
    int id, type; int points = 32;
    int getDamage() const { return points; } int getCost() const { return 3000; }
    int getID() const { return id; } int getType() const { return type; }
    int getComponentWidth() const { return 1; } int getComponentHeight() const { return 1; }
    float getWeight() const { return 1; }
};
struct LogisticsVariant;
struct LogisticsData {
    static LogisticsData* instance;
    std::vector<LogisticsComponent> components;
    LogisticsVariant* modifiedVariant = nullptr;
    void* currentlyModifiedMech = nullptr;
    std::expected<void, int> acceptMechModifications(const char* name);
    std::optional<std::reference_wrapper<LogisticsComponent>> getComponent(int id) {
        for (auto& c : components) if (c.id == id) return c;
        return std::nullopt;
    }
};
LogisticsData* LogisticsData::instance = nullptr;
struct Chassis {
    mc2pkg::PackageChassis package;
    std::string fileName = "fixture";
    bool armorEditable = true;
    int baseCost = 100, maxWeight = 100, baseArmor = 80, maxArmor = 432;
    mc2::armor::Allocation baseArmorLoc{9,11,9,9,8,8,9,9,4,2,2};
    const auto* getPackage() const { return &package; }
};
struct LogisticsVariant {
    struct ComponentInfo {
        long xCoord = 0, yCoord = 0;
        LOGISTICS_COMPONENT_LOCATION location = CHEST;
        LogisticsComponent* component = nullptr;
        int bodyLocation = -1, bodySlot = -1;
        unsigned layoutHardpoint = 0;
    };
    long componentCount = 0;
    ComponentInfo components[54];
    Chassis* chassis = nullptr;
    mc2pkg::LocationVariant layoutState;
    std::string variantName = "fixture";
    int getComponents(long&, long*);
    int getPackageMounts(long&, unsigned*) const;
    bool restoreLocationRecord(mc2pkg::LocationVariant);
    int getArmor() const; int getCost() const; int getWeight() const;
    mc2::armor::Allocation armorCapacity() const;
    mc2::armor::Allocation armorAllocation(bool legacy = false) const;
    bool hasExplicitArmor() const { return chassis->armorEditable && layoutState.armorExplicit; }
    bool stepArmor(int location,int delta);
    auto locationRecord() const { return layoutState; } // legality is covered by the shared validator cases
};
#include "layout_deployment.inc"
constexpr int INVALID_VARIANT = 1027;
#include "layout_accept.inc"
}

TEST_CASE("chassis_layout: production restore retains grandfathered items and Phase A deployment bytes") {
    using namespace deployment_seam;
    LogisticsData database; database.components = {{41, COMPONENT_FORM_WEAPON_ENERGY}, {42, OTHER}, {43, COMPONENT_FORM_WEAPON_BALLISTIC}};
    LogisticsData::instance = &database;
    Chassis chassis;
    chassis.package.regions = {{"LeftArm", 0, 0, 3, 3}};
    chassis.package.hardpoints = {{"LeftArm.Energy.0", "LeftArm", mc2pkg::Mount::Energy, {}, 91},
                                  {"LeftArm.Ballistic.0", "LeftArm", mc2pkg::Mount::Ballistic, {}, 92}};
    LogisticsVariant v; v.chassis = &chassis;
    mc2pkg::LocationVariant before;
    before.items = {{41, 0, 0, 1}, {42, 1, 0, 1}, {43, 2, 0, 1}};
    REQUIRE(v.restoreLocationRecord(before));
    long idsBefore[54]{}, idsAfter[54]{}; unsigned mountsBefore[54]{}, mountsAfter[54]{}; long count = 54;
    v.getComponents(count, idsBefore); count = 54; REQUIRE(v.getPackageMounts(count, mountsBefore) == 0);
    auto changed = before; changed.version = 2; changed.grandfathered = true;
    changed.armor.fill(255);
    for (auto& item : changed.items) { item.location = 7; item.slot = 11; item.hardpoint = 0xdeadbeef; }
    REQUIRE(v.restoreLocationRecord(changed)); CHECK(v.componentCount == 3); CHECK(v.layoutState.grandfathered);
    database.modifiedVariant = &v;
    database.currentlyModifiedMech = &v;
    const auto rejected = database.acceptMechModifications("invalid");
    REQUIRE_FALSE(rejected); CHECK(rejected.error() == INVALID_VARIANT);
    v.layoutState.grandfathered = false;
    CHECK(database.acceptMechModifications("legal"));
    count = 54; v.getComponents(count, idsAfter); count = 54; REQUIRE(v.getPackageMounts(count, mountsAfter) == 0);
    CHECK(std::memcmp(idsBefore, idsAfter, sizeof(idsBefore)) == 0);
    CHECK(std::memcmp(mountsBefore, mountsAfter, sizeof(mountsBefore)) == 0);
    CHECK(mountsAfter[0] == 91); CHECK(mountsAfter[1] == 0); CHECK(mountsAfter[2] == 92);
    changed.items.push_back({999, 0, 0, 1}); CHECK_FALSE(v.restoreLocationRecord(changed)); CHECK(v.componentCount == 3);
    LogisticsData::instance = nullptr;
}

TEST_CASE("chassis_layout: optional explicit armor FIT roundtrip and legacy absence") {
    for (bool explicitArmor : {false,true}) {
        LocationVariant record; record.armorExplicit = explicitArmor;
        record.armor = {0,20,10,5,4,3,2,1,0,0,0};
        const auto path = std::filesystem::temp_directory_path()/"mc2_armor_roundtrip.fit";
        { FitIniFile out; REQUIRE(out.create(path.string().c_str()) == NO_ERR);
          out.writeBlock("Variant0"); REQUIRE(writeLocationVariant(out,record)); out.close(); }
        { std::ifstream in(path); std::string contents((std::istreambuf_iterator<char>(in)),{});
          CHECK((contents.find("ArmorAlloc") != std::string::npos) == explicitArmor); }
        FitIniFile in; REQUIRE(in.open(path.string().c_str()) == NO_ERR); REQUIRE(in.seekBlock("Variant0") == NO_ERR);
        auto read = readLocationVariant(in); REQUIRE(read); CHECK(read->version == 2);
        CHECK(read->armorExplicit == explicitArmor); CHECK(read->armor == record.armor);
        in.close(); std::filesystem::remove(path);
    }
    using namespace deployment_seam;
    LogisticsData database; LogisticsData::instance = &database;
    Chassis chassis; LogisticsVariant variant; variant.chassis = &chassis;
    LocationVariant record; record.version = 2; record.armorExplicit = true;
    record.armor.fill(255); REQUIRE(variant.restoreLocationRecord(record));
    CHECK(mc2::armor::total(variant.layoutState.armor) <= variant.getArmor());
    const auto once = variant.layoutState.armor;
    REQUIRE(variant.restoreLocationRecord(variant.layoutState)); CHECK(variant.layoutState.armor == once);
    record.armor.fill(0); REQUIRE(variant.restoreLocationRecord(record));
    CHECK(variant.layoutState.armorExplicit); CHECK(mc2::armor::total(variant.layoutState.armor) == 0);
    record.armorExplicit = false; record.armor.fill(217);
    REQUIRE(variant.restoreLocationRecord(record)); CHECK_FALSE(variant.layoutState.armorExplicit);
    CHECK(variant.layoutState.armor == record.armor); // absent key never normalises legacy records
    LogisticsData::instance = nullptr;
}
TEST_CASE("chassis_layout: production armor edits preserve cost tonnage and plating pool") {
    using namespace deployment_seam;
    Chassis chassis;
    LogisticsComponent plating{25,COMPONENT_FORM_BULK,64};
    LogisticsVariant v; v.chassis = &chassis; v.componentCount=1; v.components[0].component=&plating;
    const int cost=v.getCost(), weight=v.getWeight();
    CHECK(v.getArmor() == 144);
    CHECK(v.stepArmor(1,-1)); CHECK(v.stepArmor(8,1));
    CHECK(v.getCost() == cost); CHECK(v.getWeight() == weight);
    CHECK(v.getArmor() == 144); CHECK(v.componentCount == 1);
    CHECK(v.layoutState.armorExplicit);
    const auto saved=v.layoutState.armor;
    CHECK(v.armorAllocation(true) == mc2::armor::legacyDistribution(chassis.baseArmorLoc,64));
    CHECK(v.layoutState.armor == saved);
    v.layoutState.armor.fill(0); v.componentCount=0;
    CHECK(v.armorAllocation() == mc2::armor::Allocation{});
    v.layoutState.armorExplicit=false;
    CHECK(v.armorAllocation() == chassis.baseArmorLoc);
}