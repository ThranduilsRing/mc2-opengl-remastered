// tests/unit/test_logistics_save_version.cpp
//
// MECHLAB-LOC-1: the compatibility boundary for revised chassis layouts (code/logistics_save_version.h).
#include "doctest.h"
#include "logistics_save_version.h"

#include <string>
#include <map>

using namespace mc2::logistics_save;

namespace {
struct SavedCell { int id; long x, y; };
struct CellFile {
    std::map<std::string, long> values;
    long readIdLong(const char* key, long& value) {
        const auto found = values.find(key);
        if (found == values.end()) return 1;
        value = found->second;
        return 0;
    }
};
}

TEST_CASE("logistics save version: read saved cells without the newer chassis-layout codec") {
    CellFile file{{{"ComponentCount", 2}, {"Component0", 145}, {"Component0x", 4}, {"Component0y", 5},
                   {"Component1", 13}, {"Component1x", -3}, {"Component1y", -3}}};
    const auto items = readComponentCells<SavedCell>(file);
    REQUIRE(items);
    REQUIRE(items->size() == 2);
    CHECK((*items)[0].id == 145);
    CHECK((*items)[0].x == 4);
    CHECK((*items)[0].y == 5);
    CHECK((*items)[1].id == 13);
    CHECK((*items)[1].x == -3);
    CHECK((*items)[1].y == -3);
    file.values.erase("Component1y");
    CHECK_FALSE(readComponentCells<SavedCell>(file));
    file.values["ComponentCount"] = 55;
    CHECK_FALSE(readComponentCells<SavedCell>(file));
    file.values["ComponentCount"] = -1;
    CHECK_FALSE(readComponentCells<SavedCell>(file));
    file.values["ComponentCount"] = 0;
    REQUIRE(readComponentCells<SavedCell>(file));
    CHECK(readComponentCells<SavedCell>(file)->empty());
    file.values.erase("ComponentCount");
    CHECK_FALSE(readComponentCells<SavedCell>(file));
}

TEST_CASE("logistics save version U8: 10005 only for a revised layout, both readable, older readers refuse 10005") {
    CHECK(saveVersionFor(false) == 10004);
    CHECK(saveVersionFor(true) == 10005);
    CHECK(saveVersionReadable(10004));
    CHECK(saveVersionReadable(10005));
    CHECK(!saveVersionReadable(10003));
    CHECK(!saveVersionReadable(10006));
    const auto oldReader = [](long v) { return v == 10004; };   // every build before this one compares for equality
    CHECK(oldReader(saveVersionFor(false)));
    CHECK(!oldReader(saveVersionFor(true)));
}

TEST_CASE("logistics save version U9: a rev-2 variant block has no Chassis line, rev 1 and stock keep it") {
    CHECK(std::string(variantChassisKey(0)) == "Chassis");
    CHECK(std::string(variantChassisKey(1)) == "Chassis");
    CHECK(std::string(variantChassisKey(2)) == "LayoutChassis");
    CHECK(!layoutRevised(1));
    CHECK(layoutRevised(2));
    const auto block = [](int rev) {   // the lines LogisticsVariant::save writes for the chassis identity
        std::string t = "st " + std::string(variantChassisKey(rev)) + " = \"marauder\"\n";
        if (layoutRevised(rev)) t += "l LayoutRev = " + std::to_string(rev) + "\n";
        t += "st VariantName = \"x\"\n";
        return t;
    };
    const auto hasChassisLine = [](const std::string& t) {   // the FIT line-prefix match an older reader performs
        return t.rfind("st Chassis", 0) == 0 || t.find("\nst Chassis") != std::string::npos;
    };
    CHECK(hasChassisLine(block(1)));
    CHECK(!hasChassisLine(block(2)));
}
