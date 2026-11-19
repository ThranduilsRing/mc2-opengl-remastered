// A campaign file with no usable group (GroupCount < 1, no CampaignName) is rejected by LogisticsMissionInfo::init, and a missing or
// out-of-range stage answers empty from the accessors instead of indexing a null groups array.
#include "doctest.h"

#define private public
#include "logisticsmissioninfo.h"
#undef private
#include "inifile.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

std::string writeFit(const char* name, const char* body) {
    const std::string path = std::string(name) + ".fit";
    std::ofstream out(path, std::ios::binary);
    out << "FITini\r\n" << body;
    return path;
}

bool initFrom(LogisticsMissionInfo& info, const char* name, const char* body) {
    const std::string path = writeFit(name, body);
    FitIniFile file;
    REQUIRE(file.open(path.c_str()) == NO_ERR);
    const bool ok = info.init(file).has_value();
    file.close();
    std::remove(path.c_str());
    return ok;
}

}

TEST_CASE("campaign init: GroupCount 0 and a missing CampaignName are rejected") {
    LogisticsMissionInfo zero;
    CHECK_FALSE(initFrom(zero, "lmi_guard_zero", "[Campaign]\r\nst CampaignName = \"x\"\r\nl GroupCount = 0\r\n"));
    CHECK(zero.groupCount == 0);

    LogisticsMissionInfo unnamed;
    CHECK_FALSE(initFrom(unnamed, "lmi_guard_unnamed", "[Campaign]\r\nl GroupCount = 1\r\n"));
}

TEST_CASE("campaign accessors with no groups answer empty") {
    LogisticsMissionInfo info;
    const char* names[4];
    int count = 4;
    CHECK(info.getAvailableMissions(names, count) == 0);
    CHECK(count == 0);
    count = 4;
    CHECK(info.getCurrentMissions(names, count) == 0);
    CHECK(count == 0);
    CHECK(info.getCurrentPurchaseFile().empty());
    CHECK_FALSE(info.setNextMission("m0101").has_value());

    info.currentStage = 5;   // out of range for groupCount 0
    count = 4;
    CHECK(info.getAvailableMissions(names, count) == 0);
    CHECK(info.getCurrentPurchaseFile().empty());
    CHECK_FALSE(info.setNextMission("m0101").has_value());
}
