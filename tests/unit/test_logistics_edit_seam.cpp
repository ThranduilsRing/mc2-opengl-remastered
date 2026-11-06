#include "doctest.h"
#include "logisticsdata.h"
#include "logisticsmech.h"
#include "logisticsvariant.h"
#include "logisticspilot.h"
#include "inifile.h"
#include "Gates/gate.h"

#include <filesystem>
#include <fstream>

namespace {
struct LogisticsFixture {
    LogisticsData* previous = LogisticsData::instance;
    LogisticsFixture() { LogisticsData::instance = nullptr; }
    ~LogisticsFixture() { LogisticsData::instance = previous; }
};
}

// The smart-load gate is cached before main. Run with MC2_SMART_LOAD=0 so
// setMechToModify exercises the edit lifecycle without loading campaign assets.
TEST_CASE("LogisticsEdit/reentry and switching restore original variants" *
          doctest::skip(mc2::gate::get<"MC2_SMART_LOAD">()))
{
    LogisticsFixture fixture;
    LogisticsData data;
    auto* chassis = new LogisticsChassis;
    chassis->ID = 1;
    LogisticsVariant originalA(chassis, true), originalB(chassis, true);
    LogisticsMech a(&originalA, 0), b(&originalB, 1);

    REQUIRE(data.setMechToModify(&a));
    auto* editable = a.getVariant();
    editable->setName("edited A");
    REQUIRE(data.setMechToModify(&a));
    CHECK(a.getVariant() == editable);
    CHECK(a.getVariant()->getName() == "edited A");
    REQUIRE(data.cancelMechModfications());
    CHECK(a.getVariant() == &originalA);
    CHECK(chassis->refCount == 2);

    REQUIRE(data.setMechToModify(&a));
    REQUIRE(data.setMechToModify(&b));
    CHECK(a.getVariant() == &originalA);
    CHECK(b.getVariant() != &originalB);
    CHECK(chassis->refCount == 3);
    REQUIRE(data.cancelMechModfications());
    CHECK(a.getVariant() == &originalA);
    CHECK(b.getVariant() == &originalB);
    CHECK(chassis->refCount == 2);
}

TEST_CASE("LogisticsEdit/pilot rank ordering precedes gunnery")
{
    LogisticsFixture fixture;
    LogisticsData data;
    LogisticsPilot low, high;
    const auto path = std::filesystem::temp_directory_path() / "mc2-pilot-rank.fit";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove(path); }
    } cleanup{path};
    // The lower-ranked pilot has better gunnery: falling through to the skill
    // comparisons must fail the low/high assertion.
    {
        std::ofstream out(path);
        out << "FITini\n[Low]\nf Gunnery = 60\nf Piloting = 10\n"
               "[High]\nf Gunnery = 50\nf Piloting = 90\nFITend\n";
        REQUIRE(out.good());
    }
    FitIniFile file;
    REQUIRE(file.open(path.string().c_str()) == NO_ERR);
    REQUIRE(file.seekBlock("Low") == NO_ERR);
    low.load(file);
    REQUIRE(file.seekBlock("High") == NO_ERR);
    high.load(file);
    file.close();
    REQUIRE(low.getRank() < high.getRank());
    CHECK(data.comparePilots(&low, &high, 0) == -1);
    CHECK(data.comparePilots(&high, &low, 0) == 1);
}
