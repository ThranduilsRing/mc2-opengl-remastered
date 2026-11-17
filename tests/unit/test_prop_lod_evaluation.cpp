#include "RenderCore/PropLodEvaluation.h"
#include "doctest.h"
#include <array>

// Trigger: PropLodEvaluation.h and gos_static_prop_batcher_mapload.cpp.
TEST_CASE("prop_lod_evaluation: valid and failed immutable chains evaluate once per map") {
    for (bool valid : {false, true}) {
        PropLodEvaluation memo;
        const std::array<uint32_t, 2> base{1, 2};
        int groups = 0, errors = 0;
        auto evaluate = [&] {
            if (memo.containsAll(base)) return;
            memo.remember(base, true);
            if (valid) ++groups; else ++errors;
        };
        evaluate(); evaluate();
        CHECK(groups == (valid ? 1 : 0));
        CHECK(errors == (valid ? 0 : 1));
        memo.clear(); evaluate();
        CHECK(groups == (valid ? 2 : 0));
        CHECK(errors == (valid ? 0 : 2));
    }
}
TEST_CASE("prop_lod_evaluation: unresolved base or target remains retryable") {
    PropLodEvaluation memo;
    const std::array<uint32_t, 2> pending{1, UINT32_MAX}, base{1, 2}, target{3, 4};
    memo.remember(pending, true);
    CHECK_FALSE(memo.containsAll(pending));
    memo.remember(base, PropLodEvaluation::resolved(pending));
    CHECK_FALSE(memo.containsAll(base));
    memo.remember(base, PropLodEvaluation::resolved(target));
    CHECK(memo.containsAll(base));
    CHECK_FALSE(memo.containsAll(target));
}
