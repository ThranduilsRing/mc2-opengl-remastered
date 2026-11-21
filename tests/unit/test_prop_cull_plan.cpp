#include "RenderCore/PropCullPlan.h"
#include "doctest.h"

TEST_CASE("prop_cull_plan: HZB on, off, off, on, off across two parity slots") {
    using namespace PropList;
    bool dirty[2] = {false, false};
    const bool hzb[5] = {true, false, false, true, false};
    // Frame i uses slot i % 2: slot 0 sees on, off, off; slot 1 sees off, on. 0 = dispatch, 2 = re-upload then direct, 3 = direct.
    const int expect[5] = {0, 3, 2, 0, 2};
    for (int i = 0; i < 5; ++i) {
        const CullDecision d = decideCull(hzb[i], dirty[i & 1]);
        dirty[i & 1] = d.slotDirty;
        CHECK((d.dispatch ? 0 : d.reupload ? 2 : 3) == expect[i]);
    }
    CHECK_FALSE(dirty[0]);   // re-uploaded: the next direct frame on slot 0 neither dispatches nor re-uploads
    CHECK_FALSE(decideCull(false, dirty[0]).reupload);
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
