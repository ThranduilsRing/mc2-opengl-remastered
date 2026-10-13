#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "objmgr_watch_policy.h"

#include <cstdint>
#include <vector>

namespace {
namespace wp = mc2watch;

static constexpr long MAXOBJ = 7;

TEST_CASE("objmgr_contract_harness/assign_at_max_succeeds") {

    CHECK((wp::canAssignWatchId((unsigned long)MAXOBJ, MAXOBJ) == true));
    CHECK((wp::canAssignWatchId((unsigned long)MAXOBJ + 1, MAXOBJ) == false));

    std::vector<int> watchList(MAXOBJ + 1, 0);
    unsigned long next = 1;
    int assigned = 0;
    while (wp::canAssignWatchId(next, MAXOBJ)) {
        watchList[next] = (int)next;
        ++next; ++assigned;
    }
    CHECK((next == (unsigned long)MAXOBJ + 1));
    CHECK((assigned == MAXOBJ));
    return;
}

TEST_CASE("objmgr_contract_harness/assign_beyond_max_fails_safely") {

    CHECK((wp::canAssignWatchId((unsigned long)MAXOBJ + 1, MAXOBJ) == false));
    CHECK((wp::canAssignWatchId(1000000ul, MAXOBJ) == false));
    return;
}

TEST_CASE("objmgr_contract_harness/resolve_rejects_invalid_ids") {
    const unsigned long next = 5;
    CHECK((wp::isResolvableWatchId(0, next) == false));
    CHECK((wp::isResolvableWatchId(next, next) == false));
    CHECK((wp::isResolvableWatchId(next + 1, next) == false));
    CHECK((wp::isResolvableWatchId(1000000ul, next) == false));
    CHECK((wp::isResolvableWatchId((unsigned long)-1, next) == false));
    return;
}

TEST_CASE("objmgr_contract_harness/resolve_accepts_live_ids") {
    const unsigned long next = 5;
    for (unsigned long id = 1; id < next; ++id)
        CHECK((wp::isResolvableWatchId(id, next) == true));
    return;
}

TEST_CASE("objmgr_contract_harness/save_loop_limit_never_exceeds_capacity") {

    for (unsigned long next = 0; next <= (unsigned long)MAXOBJ + 4; ++next) {
        long hi = wp::clampSaveLoopLimit(next, MAXOBJ);
        CHECK((hi <= MAXOBJ));
        long expect = ((long)next < MAXOBJ) ? (long)next : MAXOBJ;
        CHECK((hi == expect));
    }
    return;
}

TEST_CASE("objmgr_contract_harness/copyfrom_clamps_corrupt_next") {
    const long cap = MAXOBJ + 1;
    CHECK((wp::clampSavedNextWatchId(-1, MAXOBJ) == 1ul));
    CHECK((wp::clampSavedNextWatchId(-99999, MAXOBJ) == 1ul));
    CHECK((wp::clampSavedNextWatchId(cap + 1, MAXOBJ) == (unsigned long)cap));
    CHECK((wp::clampSavedNextWatchId(0x7fffffff, MAXOBJ) == (unsigned long)cap));

    CHECK((wp::clampSavedNextWatchId(0, MAXOBJ) == 0ul));
    CHECK((wp::clampSavedNextWatchId(3, MAXOBJ) == 3ul));
    CHECK((wp::clampSavedNextWatchId(cap, MAXOBJ) == (unsigned long)cap));
    return;
}

TEST_CASE("objmgr_contract_harness/load_rejects_out_of_range_index") {
    CHECK((wp::isValidWatchSaveIndex(-1, MAXOBJ) == false));
    CHECK((wp::isValidWatchSaveIndex((int32_t)(MAXOBJ + 1), MAXOBJ) == false));
    CHECK((wp::isValidWatchSaveIndex(INT32_MAX, MAXOBJ) == false));
    CHECK((wp::isValidWatchSaveIndex(INT32_MIN, MAXOBJ) == false));
    return;
}

TEST_CASE("objmgr_contract_harness/load_allows_boundary_and_valid_index") {
    CHECK((wp::isValidWatchSaveIndex(0, MAXOBJ) == true));
    CHECK((wp::isValidWatchSaveIndex((int32_t)MAXOBJ, MAXOBJ) == true));
    CHECK((wp::isValidWatchSaveIndex(3, MAXOBJ) == true));
    return;
}

TEST_CASE("objmgr_contract_harness/load_restore_never_derefs_oob") {

    std::vector<int> objList(MAXOBJ + 1);
    for (long i = 0; i <= MAXOBJ; ++i) objList[i] = 0x1000 + (int)i;
    const int32_t watchSave[] = { -1, 0, (int32_t)MAXOBJ, (int32_t)(MAXOBJ + 1),
                                  INT32_MAX, INT32_MIN, 3 };
    std::vector<int> watchList(sizeof(watchSave) / sizeof(watchSave[0]), -777);
    for (size_t j = 0; j < watchList.size(); ++j) {
        const int32_t idx = watchSave[j];
        if (!wp::isValidWatchSaveIndex(idx, MAXOBJ)) {
            watchList[j] = 0;
            continue;
        }
        watchList[j] = objList[idx];
    }

    CHECK((watchList[0] == 0));
    CHECK((watchList[3] == 0));
    CHECK((watchList[4] == 0));
    CHECK((watchList[5] == 0));
    CHECK((watchList[1] == objList[0]));
    CHECK((watchList[2] == objList[MAXOBJ]));
    CHECK((watchList[6] == objList[3]));
    return;
}

TEST_CASE("objmgr_contract_harness/generation_assign_sets_one") {

    CHECK((wp::nextGenerationOnAssign(0) == (uint16_t)1));

    CHECK((wp::nextGenerationOnAssign(1) == (uint16_t)1));
    CHECK((wp::nextGenerationOnAssign(42) == (uint16_t)42));
    return;
}

TEST_CASE("objmgr_contract_harness/generation_bump_on_free") {
    CHECK((wp::bumpGenerationOnFree(1) == (uint16_t)2));
    CHECK((wp::bumpGenerationOnFree(0) == (uint16_t)1));

    CHECK((wp::bumpGenerationOnFree((uint16_t)65535) == (uint16_t)0));
    return;
}

TEST_CASE("objmgr_contract_harness/generation_stale_vs_fresh") {

    uint16_t gen = wp::nextGenerationOnAssign(0);
    const uint16_t oldHandleGen = gen;
    gen = wp::bumpGenerationOnFree(gen);
    const uint16_t newHandleGen = wp::nextGenerationOnAssign(gen);
    CHECK((newHandleGen == (uint16_t)2));

    CHECK((wp::generationMatches(gen, oldHandleGen) == false));

    CHECK((wp::generationMatches(gen, newHandleGen) == true));
    return;
}

TEST_CASE("objmgr_contract_harness/demo_intentional_fail" * doctest::should_fail()) {
    CHECK((wp::isValidWatchSaveIndex((int32_t)(MAXOBJ + 1), MAXOBJ) == true));
    return;
}

}
