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
            CHECK(row.y + row.h <= panelRect(p).y + panelRect(p).h - kRefusalH);
            CHECK(row.h >= 10);
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
        CHECK(last.y + last.h <= panelRect(p).y + panelRect(p).h - kRefusalH);
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

TEST_CASE("mechlab loc: overflow scrolling reaches every item of a full 12-cell location") {
    CHECK(maxScroll(5) == 0);
    CHECK(maxScroll(6) == 1);
    CHECK(maxScroll(12) == 7);
    CHECK(clampScroll(6, 9) == 1);
    CHECK(clampScroll(6, -3) == 0);
    CHECK(scrollByWheel(6, 0, 100) == 1);      // wheel back -> later items
    CHECK(scrollByWheel(6, 1, 100) == 1);      // clamped at the end
    CHECK(scrollByWheel(6, 1, -100) == 0);
    CHECK(scrollToReveal(6, 0, 5) == 1);       // a 6th item scrolled into view
    CHECK(scrollToReveal(12, 7, 0) == 0);
    const int ct = panelOfLocation("CenterTorso").value();
    for (int scroll : {0, 1}) {                // the 6th item is selectable once scrolled; the 1st is hidden then
        const Rect row4 = itemRowRect(ct, kMaxItemRows - 1);
        CHECK(itemIndexAt(ct, 6, scroll, row4.cx(), row4.cy()).value() == scroll + kMaxItemRows - 1);
    }
    const Rect row5 = itemRowRect(ct, 4);
    CHECK(!itemIndexAt(ct, 3, 0, row5.cx(), row5.cy()));   // empty row: no item
}

TEST_CASE("mechlab loc: a dropped item selects its placed instance, so Remove targets the moved copy") {
    const mc2pkg::PackageChassis c = marauder6x6();
    std::vector<mc2pkg::Placed> have;   // three copies of one heat sink already sit in the centre torso
    for (int i = 0; i < 3; ++i) { mc2pkg::Placed h; h.x = 2 + (i % 2); h.y = i / 2; h.ordinal = 7; h.weight = 1.0f; have.push_back(h); }
    Candidate sink{1, 1, mc2pkg::Mount::None, 7, 1.0f};
    const Explained e = explain(c, have, sink, 2);
    REQUIRE(e.reason == Reason::Legal);
    int matches = 0;
    for (const mc2pkg::Placed& h : have) matches += isSelectedInstance(h.x, h.y, e.x, e.y);
    CHECK(matches == 0);                                    // the placed cell is none of the other copies
    CHECK(isSelectedInstance(e.x, e.y, e.x, e.y));
    CHECK(!isSelectedInstance(e.x, e.y, have[0].x, have[0].y));
}

namespace {
mc2pkg::PackageChassis marauderShipped() {   // the layout the recipe ships (see marauder.json mc2.regions)
    mc2pkg::PackageChassis c = marauder6x6();
    c.regions = {{"LeftArm", 0, 0, 2, 3}, {"LeftTorso", 0, 3, 2, 3}, {"CenterTorso", 2, 0, 2, 6}, {"RightTorso", 4, 3, 2, 3}, {"RightArm", 4, 0, 2, 3}};
    c.hardpoints.clear();
    auto hp = [&](const char* id, const char* loc, mc2pkg::Mount m) {
        mc2pkg::Hardpoint h; h.id = id; h.location = loc; h.mount = m; h.key = mc2pkg::hardpointKey(id);
        c.hardpoints.push_back(h);
    };
    hp("LeftArm.Energy.0", "LeftArm", mc2pkg::Mount::Energy);
    hp("LeftArm.Energy.1", "LeftArm", mc2pkg::Mount::Energy);
    hp("RightArm.Energy.0", "RightArm", mc2pkg::Mount::Energy);
    hp("RightArm.Energy.1", "RightArm", mc2pkg::Mount::Energy);
    for (int i = 0; i < 3; ++i) hp(("RightTorso.Ballistic." + std::to_string(i)).c_str(), "RightTorso", mc2pkg::Mount::Ballistic);
    return c;
}
mc2pkg::Placed placedItem(int x, int y, int w, int h, mc2pkg::Mount m, int ord, float wt) {
    mc2pkg::Placed p; p.x = x; p.y = y; p.w = w; p.h = h; p.category = m; p.ordinal = ord; p.weight = wt;
    return p;
}
}

TEST_CASE("mechlab loc: a save made on the old 6x4 layout is migrated or dropped, never restored blindly") {
    const mc2pkg::PackageChassis c = marauderShipped();
    // Old save, in saved order: coordinates of the pre-change 6x4 grid (LA col 0, LT col 1, CT cols 2-3, RT col 4, RA col 5).
    std::vector<mc2pkg::Placed> saved = {
        placedItem(0, 0, 1, 3, mc2pkg::Mount::Energy, 145, 6.0f),      // PPC left arm: still legal where it is
        placedItem(0, 3, 1, 1, mc2pkg::Mount::Energy, 156, 2.0f),      // ML: row 3 is now the left torso (no mounts) -> moves into the arm
        placedItem(5, 0, 1, 3, mc2pkg::Mount::Energy, 145, 6.0f),      // PPC right arm: col 5 is inside the new right arm
        placedItem(2, 0, 2, 2, mc2pkg::Mount::Ballistic, 100, 8.0f),   // AC/5 centre torso: the CT lost its ballistic mount -> right torso
        placedItem(1, 0, 1, 1, mc2pkg::Mount::None, 7, 1.0f),          // heat sinks that sat in the old left torso column
        placedItem(1, 1, 1, 1, mc2pkg::Mount::None, 7, 1.0f),
        placedItem(0, 0, 1, 1, mc2pkg::Mount::None, 160, 40.0f),       // does not fit the payload: dropped
    };
    std::vector<mc2pkg::Placed> loaded;
    int dropped = 0;
    for (mc2pkg::Placed it : saved) {
        const auto to = migratePlacement(c, loaded, it);
        if (!to) { ++dropped; continue; }
        it.x = to->x; it.y = to->y;
        loaded.push_back(it);
    }
    CHECK(dropped == 1);
    REQUIRE(loaded.size() == 6);
    CHECK(loaded[0].x == 0);   CHECK(loaded[0].y == 0);              // kept
    CHECK(loaded[1].x == 1);   CHECK(loaded[1].y == 0);              // the ML moved to the arm's second cell column
    CHECK(loaded[3].x == 4);   CHECK(loaded[3].y == 3);              // the AC/5 left the centre torso for the ballistic right torso
    bool ok = false;
    mc2pkg::assignMounts(c, loaded, &ok);
    CHECK(ok);                                                       // every weapon sits on a mount of its own location
    for (size_t i = 0; i < loaded.size(); ++i) {
        CHECK(mc2pkg::regionForFootprint(c, loaded[i].x, loaded[i].y, loaded[i].w, loaded[i].h) != nullptr);
        for (size_t j = i + 1; j < loaded.size(); ++j)
            CHECK(cellsFree({loaded[i]}, loaded[j].x, loaded[j].y, loaded[j].w, loaded[j].h));   // no overlap
    }
    CHECK(mc2pkg::massUsed(loaded) <= c.payload + 1e-3f);
    // deterministic: the same save migrates to the same coordinates
    std::vector<mc2pkg::Placed> again;
    for (mc2pkg::Placed it : saved) {
        const auto to = migratePlacement(c, again, it);
        if (!to) continue;
        it.x = to->x; it.y = to->y;
        again.push_back(it);
    }
    REQUIRE(again.size() == loaded.size());
    for (size_t i = 0; i < loaded.size(); ++i) { CHECK(again[i].x == loaded[i].x); CHECK(again[i].y == loaded[i].y); }
}
