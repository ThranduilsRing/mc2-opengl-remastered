#include "RenderCore/PropCullPlan.h"
#include "doctest.h"

TEST_CASE("prop_cull_plan: HZB on, off, off, on, off across two parity slots") {
    using namespace PropList;
    const bool hzb[5] = {true, false, false, true, false};
    const int expect[5] = {0, 2, 3, 0, 2};   // 0 = dispatch, 2 = re-upload then direct, 3 = direct
    for (int slot = 0; slot < 2; ++slot) {   // each parity slot sees the whole sequence; slots are independent
        bool dirty = false;
        for (int i = 0; i < 5; ++i) {
            const CullDecision d = decideCull(hzb[i], dirty);
            dirty = d.slotDirty;
            CHECK((d.dispatch ? 0 : d.reupload ? 2 : 3) == expect[i]);
        }
    }
}

TEST_CASE("prop_cull_plan: a view that never has HZB is always direct and never re-uploads") {
    bool dirty = false;
    for (int i = 0; i < 4; ++i) {
        const PropList::CullDecision d = PropList::decideCull(false, dirty);
        dirty = d.slotDirty;
        CHECK_FALSE(d.dispatch);
        CHECK_FALSE(d.reupload);
    }
}
