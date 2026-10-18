// Pure decision kernel for which [WarriorN] Profile string the editor's
// mission save writes. Locks the fix for the Bubba x3 regression where a
// broken pilots.csv environment blanked every Profile on re-save.

#include "doctest.h"

#include <string>

#include "../../editor/PilotProfilePreserve.h"

TEST_CASE("raw on-disk profile wins over catalog match")
{
    bool loss = true;
    const char* r = PilotProfileForSave("pmwflash", "pmwbubba", &loss);
    CHECK(std::string(r) == "pmwflash");
    CHECK(loss == false);
}

TEST_CASE("broken catalog (fileName NULL) still round-trips disk string")
{
    // The actual regression: initPilots loaded 0 pilots -> fallback
    // s_GoodPilots[0].fileName == NULL. Old code wrote "".
    bool loss = true;
    const char* r = PilotProfileForSave("pmwflash", nullptr, &loss);
    CHECK(std::string(r) == "pmwflash");
    CHECK(loss == false);
}

TEST_CASE("freshly placed unit uses catalog match")
{
    bool loss = true;
    const char* r = PilotProfileForSave("", "pmwbubba", &loss);
    CHECK(std::string(r) == "pmwbubba");
    CHECK(loss == false);
}

TEST_CASE("nothing available -> empty string with data-loss flag")
{
    bool loss = false;
    const char* r = PilotProfileForSave("", nullptr, &loss);
    CHECK(std::string(r).empty());
    CHECK(loss == true);

    loss = false;
    r = PilotProfileForSave(nullptr, "", &loss);
    CHECK(std::string(r).empty());
    CHECK(loss == true);
}

TEST_CASE("null dataLoss out-pointer is tolerated")
{
    CHECK(std::string(PilotProfileForSave(nullptr, nullptr, nullptr)).empty());
    CHECK(std::string(PilotProfileForSave("x", nullptr, nullptr)) == "x");
}
