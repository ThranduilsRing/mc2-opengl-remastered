// tests/unit/test_package_chassis.cpp
//
// BT2018-FIRST-CLASS P3/P7: the pure package-chassis rules (code/package_chassis.h) every
// legality caller shares -- mech lab drop/auto-place, deployment mount assignment, and the
// runtime object-type allocation. The fixture mirrors the generated bt2018:marauder
// chassisdef.fit (tools/bt2018_mechs/recipes/marauder.json): a 6x4 grid split into five
// location regions, 2+2 Energy arm mounts, one Missile per side torso, Ballistic + AP centre.
#include "doctest.h"
#include "package_chassis.h"
#include "equipment_registry.h"

using namespace mc2pkg;

namespace {

PackageChassis marauder() {
    PackageChassis c;
    c.chassisId = "bt2018:marauder";
    c.stem = "bt2018_marauder";
    c.tonnage = 75.0f;
    c.emptyMass = 41.0f;
    c.payload = 34.0f;
    c.regions = {{"LeftArm", 0, 0, 1, 4}, {"LeftTorso", 1, 0, 1, 4}, {"CenterTorso", 2, 0, 2, 4},
                 {"RightTorso", 4, 0, 1, 4}, {"RightArm", 5, 0, 1, 4}};
    auto hp = [&](const char* id, const char* loc, Mount m, const char* sock) {
        Hardpoint h; h.id = id; h.location = loc; h.mount = m; h.socket = sock; h.key = hardpointKey(id);
        c.hardpoints.push_back(h);
    };
    hp("LeftArm.Energy.0", "LeftArm", Mount::Energy, "weapon_leftarm");
    hp("LeftArm.Energy.1", "LeftArm", Mount::Energy, "weapon_leftarm2");
    hp("LeftTorso.Missile.0", "LeftTorso", Mount::Missile, "weapon_lefttorso");
    hp("CenterTorso.Ballistic.0", "CenterTorso", Mount::Ballistic, "weapon_centertorso");
    hp("CenterTorso.AntiPersonnel.0", "CenterTorso", Mount::AntiPersonnel, "weapon_centertorso2");
    hp("RightTorso.Missile.0", "RightTorso", Mount::Missile, "weapon_righttorso");
    hp("RightArm.Energy.0", "RightArm", Mount::Energy, "weapon_rightarm");
    hp("RightArm.Energy.1", "RightArm", Mount::Energy, "weapon_rightarm2");
    c.antiPersonnelOrdinals = {67, 68};
    return c;
}

Placed item(int x, int y, int w, int h, Mount cat, int ord, float wt) {
    Placed p; p.x = x; p.y = y; p.w = w; p.h = h; p.category = cat; p.ordinal = ord; p.weight = wt;
    return p;
}

// MAD-3R as the compiler places it: PPC+ML per arm, AC5 centre, 3 heat sinks per side torso.
std::vector<Placed> mad3r() {
    return {item(0, 0, 1, 3, Mount::Energy, 145, 6), item(0, 3, 1, 1, Mount::Energy, 156, 2),
            item(5, 0, 1, 3, Mount::Energy, 145, 6), item(5, 3, 1, 1, Mount::Energy, 156, 2),
            item(2, 0, 2, 2, Mount::Ballistic, 100, 8),
            item(1, 0, 1, 1, Mount::None, 7, 1), item(1, 1, 1, 1, Mount::None, 7, 1), item(1, 2, 1, 1, Mount::None, 7, 1),
            item(4, 0, 1, 1, Mount::None, 7, 1), item(4, 1, 1, 1, Mount::None, 7, 1), item(4, 2, 1, 1, Mount::None, 7, 1)};
}

} // namespace

TEST_CASE("package chassis: the stock MAD-3R loadout mounts every weapon on a declared hardpoint") {
    const PackageChassis c = marauder();
    bool ok = false;
    const std::vector<int> m = assignMounts(c, mad3r(), &ok);
    REQUIRE(ok);
    CHECK(c.hardpoints[m[0]].id == "LeftArm.Energy.0");
    CHECK(c.hardpoints[m[1]].id == "LeftArm.Energy.1");
    CHECK(c.hardpoints[m[2]].id == "RightArm.Energy.0");
    CHECK(c.hardpoints[m[4]].id == "CenterTorso.Ballistic.0");
    CHECK(m[5] == -1);   // heat sinks take no mount
    CHECK(massUsed(mad3r()) == doctest::Approx(30.0f));
}

TEST_CASE("package chassis: wrong mount type, wrong location and overweight are refused") {
    const PackageChassis c = marauder();
    std::vector<Placed> base = mad3r();
    // A third energy weapon in the left arm: the arm has two Energy mounts, both used.
    base.erase(base.begin() + 1);   // free one LA cell (the ML at 0,3)
    CHECK(checkAdd(c, base, item(0, 3, 1, 1, Mount::Energy, 156, 2)) == Verdict::Legal);
    CHECK(checkAdd(c, mad3r(), item(1, 3, 1, 1, Mount::Energy, 156, 2)) == Verdict::NoFreeMount);   // LT has only Missile
    CHECK(checkAdd(c, mad3r(), item(1, 3, 1, 1, Mount::Missile, 120, 2)) == Verdict::Legal);        // LRM5 in LT: legal
    CHECK(checkAdd(c, mad3r(), item(0, 2, 2, 2, Mount::Ballistic, 100, 1)) == Verdict::OutsideRegion); // straddles LA/LT
    CHECK(checkAdd(c, mad3r(), item(1, 3, 1, 1, Mount::Missile, 120, 5)) == Verdict::OverPayload);  // 30 + 5 > 34
    CHECK(checkAdd(c, mad3r(), item(1, 3, 1, 1, Mount::None, 25, 2)) == Verdict::Legal);             // armour plate
}

TEST_CASE("package chassis: anti-personnel items prefer the AP mount, then their category") {
    const PackageChassis c = marauder();
    std::vector<Placed> v = {item(2, 2, 1, 1, Mount::Ballistic, 67, 0.5f),     // machine gun (AP capable)
                             item(2, 0, 2, 2, Mount::Ballistic, 100, 8)};      // AC5
    bool ok = false;
    const std::vector<int> m = assignMounts(c, v, &ok);
    REQUIRE(ok);
    CHECK(c.hardpoints[m[0]].mount == Mount::AntiPersonnel);
    CHECK(c.hardpoints[m[1]].mount == Mount::Ballistic);
    // A second AP-capable MG falls back to... nothing: the Ballistic mount is the AC5's.
    v.push_back(item(3, 2, 1, 1, Mount::Ballistic, 67, 0.5f));
    assignMounts(c, v, &ok);
    CHECK_FALSE(ok);
}

TEST_CASE("package chassis: auto-place probing finds a region with a free compatible mount") {
    const PackageChassis c = marauder();
    Placed lrm = item(-1, -1, 1, 2, Mount::Missile, 60, 4);
    CHECK(checkAdd(c, mad3r(), lrm) == Verdict::Legal);
    Placed ppc = item(-1, -1, 1, 3, Mount::Energy, 145, 1);
    CHECK(checkAdd(c, mad3r(), ppc) == Verdict::NoFreeMount);   // all four Energy mounts are used
}

TEST_CASE("package chassis: object-type numbers follow the legacy packets and respect the table limit") {
    std::vector<PackageChassis> v(3);
    CHECK(allocateTypeNums(v, 1188, kPackageTypeLimit) == 3);
    CHECK(v[0].objTypeNum == 1188);
    CHECK(v[2].objTypeNum == 1190);
    std::vector<PackageChassis> full(2);
    CHECK(allocateTypeNums(full, kPackageTypeLimit - 1, kPackageTypeLimit) == 1);
    CHECK(full[1].objTypeNum == -1);
}

TEST_CASE("package chassis: hardpoint keys are the HardpointId FNV-1a hash") {
    const PackageChassis c = marauder();
    CHECK(c.hardpointById("RightArm.Energy.1") != nullptr);
    CHECK(c.hardpointById("RightArm.Energy.2") == nullptr);
    CHECK(hardpointKey("") == 0u);
    CHECK(mountFromName("AntiPersonnel") == Mount::AntiPersonnel);
    CHECK(mountFromName("Melee") == Mount::None);
}

TEST_CASE("equipment ADD: a pack missile rack keeps the stock missile-type token") {
    EquipmentAddRowFields f;
    f.ordinal = 60;
    f.formOrdinal = 9;   // MissileWeapon
    f.displayName = "LRM 10 Rack";
    f.ammoMasterId = 220;
    f.iconFile = "MCL_MC_LRMRack.tga";
    CHECK(buildEquipmentAddCsvLine(f).find(",1,0,") != std::string::npos);
    f.ammoToken = "LRM";
    CHECK(buildEquipmentAddCsvLine(f).find(",LRM,0,") != std::string::npos);
}

// FX-LOOP-AIM: every weapon_* socket a package chassis names must be a node that exists in its GLB (a missing bone
// silently drops the mount to the legacy round-robin node). Needs the install data (MC2_TEST_INSTALL_DATA).
#include "stock_data_scan.h"

TEST_CASE("package chassis: bone-name parsing and GLB node lookup") {
    const std::string json = R"({"nodes": {"joint_torso": "j_Spine2", "weapon_rightarm": "j_RForearm", "weapon_leftarm": "j_LForearm"}})";
    const auto bones = stockscan::packageWeaponBones(json);
    REQUIRE(bones.size() == 2);
    CHECK(bones[0].first == "weapon_rightarm");
    CHECK(bones[0].second == "j_RForearm");
    const std::string body = R"({"nodes":[{"name":"j_RForearm"},{"name": "j_LForearm"}]})";
    std::string glb = "glTF";
    glb.append(8, '\0');
    glb.push_back(static_cast<char>(body.size()));
    glb.append(3, '\0');
    glb += "JSON" + body;
    CHECK(stockscan::glbHasNode(glb, "j_RForearm"));
    CHECK(stockscan::glbHasNode(glb, "j_LForearm"));
    CHECK_FALSE(stockscan::glbHasNode(glb, "j_Missing"));
}

TEST_CASE("package chassis: every weapon_* name maps to a bone present in the GLB (needs the install data)") {
    using namespace stockscan;
    const fs::path tgl = installDataRoot() / "tgl";
    if (installDataRoot().empty()) { MESSAGE("MC2_TEST_INSTALL_DATA not found: package scan skipped"); return; }
    int packages = 0;
    for (const auto& e : fs::directory_iterator(tgl)) {
        const std::string fn = e.path().filename().string();
        if (fn.size() < 13 || fn.compare(fn.size() - 13, 13, ".package.json") != 0) continue;
        const std::string json = readAll(e.path());
        const size_t g = json.find("\"sourceGlb\"");
        REQUIRE(g != std::string::npos);
        const size_t b = json.find('"', json.find(':', g) + 1);
        const std::string glbName = json.substr(b + 1, json.find('"', b + 1) - b - 1);
        const std::string glb = readAll(tgl / glbName);
        INFO(fn << " -> " << glbName);
        REQUIRE_FALSE(glb.empty());
        const auto bones = packageWeaponBones(json);
        CHECK_FALSE(bones.empty());
        for (const auto& [socket, bone] : bones) {
            INFO(fn << " " << socket << " -> " << bone);
            CHECK(glbHasNode(glb, bone));
        }
        ++packages;
    }
    MESSAGE("scanned " << packages << " packages");
    CHECK(packages >= 20);
}
