#include "doctest.h"
#include "building_static_eligibility.h"
#include "battle_bench_sink.h"
#include <string_view>

using mc2::building_static::activityNeedsUpdate;

TEST_CASE("building_static_eligibility/census reasons keep count units")
{
    using namespace mc2::battlebench;
    // The formatter interprets the suffix beginning at PresAcquireNs as nanoseconds.
    CHECK(BldgUpdateType < PresAcquireNs);
    CHECK(TreeUpdateOther < PresAcquireNs);
    CHECK(std::string_view(kFieldNames[std::to_underlying(BldgUpdateActivity)]) == "bldg.update.activity");
    CHECK(std::string_view(kFieldNames[std::to_underlying(TreeUpdateUnregistered)]) == "tree.update.unregistered");
}

TEST_CASE("building_static_eligibility/stopped effects do not keep geometry dynamic")
{
    // stopActivity retains both allocated handles, including on a powered-down building.
    CHECK_FALSE(activityNeedsUpdate(false, true, true));
    CHECK_FALSE(activityNeedsUpdate(false, true, false));
    CHECK_FALSE(activityNeedsUpdate(false, false, true));
    // Starting the retained effects again must restore the full Execute pass.
    CHECK(activityNeedsUpdate(true, true, true));
    CHECK(activityNeedsUpdate(true, true, false));
    CHECK(activityNeedsUpdate(true, false, true));
    // Failed/disabled effect creation has no Execute work, regardless of the flag.
    CHECK_FALSE(activityNeedsUpdate(true, false, false));
    CHECK_FALSE(activityNeedsUpdate(false, false, false));
}
