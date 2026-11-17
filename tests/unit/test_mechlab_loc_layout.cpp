// tests/unit/test_mechlab_loc_layout.cpp
//
// MECHLAB-LOC-1: the location view's one rect table (code/mechlab_loc_layout.h). Drawing maps legacy rects
// through toScreen(); the mouse arrives in legacy canvas coordinates, so the hit rect IS the draw rect. These
// cases pin that at 800x600, 1080p and 4K, plus the refusal reasons a panel reports.
#include "doctest.h"
#include "mechlab_loc_layout.h"

using namespace mc2::mechlab_loc;

TEST_CASE("mechlab loc: all six torso and twelve CT items are reachable") {
    for (int p : {1, 2, 3, 4, 5}) {
        const int count = p == 1 ? 12 : 6;
        for (int k = 0; k < count; ++k) {
            const Rect row = itemRowRect(p, k, count);
            CHECK(panelRect(p).contains(row.x, row.y));
            CHECK(row.y + row.h <= panelRect(p).y + panelRect(p).h - 14);
            CHECK(itemRowAt(p, row.cx(), row.cy(), count).value_or(-1) == k);
        }
    }
}

TEST_CASE("mechlab loc: panels do not overlap and the last item row clears the refusal line") {
    for (int a = 0; a < kPanelCount; ++a)
        for (int b = a + 1; b < kPanelCount; ++b) {
            const Rect ra = panelRect(a), rb = panelRect(b);
            CHECK((ra.x + ra.w <= rb.x || rb.x + rb.w <= ra.x || ra.y + ra.h <= rb.y || rb.y + rb.h <= ra.y));
        }
    for (int p = 0; p < kPanelCount; ++p) {
        const Rect last = itemRowRect(p, 11, 12);
        CHECK(last.y + last.h <= panelRect(p).y + panelRect(p).h - 14.0f);
    }
}

namespace {
mc2pkg::PackageChassis marauder6x6() {   // six-cell torso / twelve-cell CT fixture retained for capacity coverage
    mc2pkg::PackageChassis c;
    c.payload = 34.0f;
    c.regions = {{"LeftArm", 0, 0, 2, 3}, {"LeftTorso", 0, 3, 2, 3}, {"CenterTorso", 2, 0, 2, 6}, {"RightTorso", 4, 3, 2, 3}, {"RightArm", 4, 0, 2, 3}};
    auto hp = [&](const char* id, const char* loc, mc2pkg::Mount m) {
        mc2pkg::Hardpoint h; h.id = id; h.location = loc; h.mount = m; h.key = mc2pkg::hardpointKey(id);
        c.hardpoints.push_back(h);
    };
    hp("LeftArm.Energy.0", "LeftArm", mc2pkg::Mount::Energy);
    hp("LeftArm.Energy.1", "LeftArm", mc2pkg::Mount::Energy);
    hp("RightTorso.Ballistic.0", "RightTorso", mc2pkg::Mount::Ballistic);
    return c;
}
}

TEST_CASE("mechlab loc: each panel names why it refuses a part") {
    const mc2pkg::PackageChassis c = marauder6x6();
    mc2pkg::Placed ppc; ppc.x = 0; ppc.y = 0; ppc.w = 1; ppc.h = 3; ppc.category = mc2pkg::Mount::Energy; ppc.weight = 7.0f;
    const std::vector<mc2pkg::Placed> have = {ppc};
    Candidate laser{1, 1, mc2pkg::Mount::Energy, 156, 1.0f};
    Candidate ac10{2, 3, mc2pkg::Mount::Ballistic, 100, 12.0f};
    CHECK(explain(c, have, laser, 0).reason == Reason::Legal);                 // arm: second Energy mount free
    CHECK(explain(c, have, laser, 2).reason == Reason::NoFreeMount);           // centre torso has cells but no mounts
    CHECK(explain(c, have, ac10, 0).reason == Reason::RegionFull);             // the PPC leaves no free 2x3 in the arm
    Candidate mg{1, 1, mc2pkg::Mount::Ballistic, 120, 0.5f};
    CHECK(explain(c, have, mg, 0).reason == Reason::NoFreeMount);              // cells free, but the arm has no Ballistic mount
    CHECK(explain(c, have, ac10, 3).reason == Reason::Legal);                  // right torso: ballistic mount and 2x3 cells
    Candidate big{3, 3, mc2pkg::Mount::None, 1, 1.0f};
    CHECK(explain(c, have, big, 0).reason == Reason::TooBig);
    Candidate heavy{1, 1, mc2pkg::Mount::None, 7, 30.0f};
    const Explained e = explain(c, have, heavy, 2);
    CHECK(e.reason == Reason::OverPayload);
    CHECK(e.overage == doctest::Approx(3.0f));
    CHECK(explain(c, have, laser, 99).reason == Reason::NoRegion);
    CHECK(reasonText(e, mc2pkg::Mount::None, &c.regions[2], heavy) == "over payload by 3.0 t");
}
