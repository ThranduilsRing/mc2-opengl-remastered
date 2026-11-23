// tests/unit/test_mechlab_loc_layout.cpp
//
// MECHLAB-LOC-1: the location view's one rect table (code/mechlab_loc_layout.h). Drawing maps legacy rects
// through toScreen(); the mouse arrives in legacy canvas coordinates, so the hit rect IS the draw rect. These
// cases pin that at 800x600, 1080p and 4K, plus the refusal reasons a panel reports.
#include "doctest.h"
#include "mechlab_loc_layout.h"

using namespace mc2::mechlab_loc;

TEST_CASE("mechlab loc: record sheet uses the same contained geometry at every resolution") {
    for (const Canvas cv : {Canvas{1,1,0,0},Canvas{2.4f,1.8f,0,0},Canvas{4.8f,3.6f,0,0}}) {
        for (int p=0;p<8;++p) {
            const auto box=panelRect(p);
            CHECK(box.x >= 217+kDx); CHECK(box.x+box.w <= 533+kDx);
            CHECK(box.y >= 150); CHECK(box.y+box.h <= 476);
            if (p != kDoll) { CHECK(box.x >= kPreview.x+kPreview.w); CHECK(box.x+box.w <= 620); }   // clear of the mech preview (right edge 281) and the list divider
            for (int q=p+1;q<8;++q) {
                const auto other=panelRect(q);
                CHECK((box.x+box.w<=other.x || other.x+other.w<=box.x || box.y+box.h<=other.y || other.y+other.h<=box.y));
            }
            for (bool rear : {false,true}) {
                if (rear && !torso(p)) continue;
                const auto value=armorValueRect(p,rear);   // armor number centred in the panel and between its steppers
                CHECK(value.cx() == doctest::Approx(box.cx()));
                CHECK(value.cx() == doctest::Approx((stepperRect(p,rear,false).cx()+stepperRect(p,rear,true).cx())*.5f));
                for (bool plus : {false,true}) {
                    const auto hit=stepperRect(p,rear,plus), draw=toScreen(hit,cv);
                    CHECK(hit.contains(draw.cx()/cv.sx,draw.cy()/cv.sy));
                    CHECK(box.contains(hit.x,hit.y)); CHECK(box.contains(hit.x+hit.w-.01f,hit.y+hit.h-.01f));
                }
            }
            for (int row=0;row<visibleRows(p);++row) {
                const auto hit=itemRowRect(p,row,visibleRows(p)),draw=toScreen(hit,cv);
                CHECK(itemRowAt(p,draw.cx()/cv.sx,draw.cy()/cv.sy,visibleRows(p)) == row);
                CHECK(hit.y+hit.h<=box.y+box.h);
            }
        }
    }
}
TEST_CASE("mechlab loc: list item cost/heat/warning rects stay inside the item and off the icon") {
    auto overlap = [](const Rect& a, const Rect& b) { return a.x < b.x+b.w && b.x < a.x+a.w && a.y < b.y+b.h && b.y < a.y+a.h; };
    auto inside = [](const Rect& a, const Rect& b) { return a.x >= b.x && a.y >= b.y && a.x+a.w <= b.x+b.w && a.y+a.h <= b.y+b.h; };
    for (int cw = 1; cw <= 2; ++cw) for (int ch = 1; ch <= 6; ++ch) {
        const ItemRects r = componentItemRects(cw, ch);
        for (const Rect* t : {&r.cost, &r.costIcon, &r.heat, &r.heatIcon, &r.iconOutline, &r.warn}) CHECK(inside(*t, r.item));
        for (const Rect* t : {&r.cost, &r.costIcon, &r.heat, &r.heatIcon}) CHECK(!overlap(*t, r.iconOutline));   // digits never cross the icon box
        for (const Rect* t : {&r.weight, &r.weightIcon}) {   // weight sits with the heat: in the item, off the icon, off every other cell
            CHECK(inside(*t, r.item)); CHECK(!overlap(*t, r.iconOutline)); CHECK(!overlap(*t, r.name));
            for (const Rect* o : {&r.cost, &r.costIcon, &r.heat, &r.heatIcon}) CHECK(!overlap(*t, *o));
        }
        CHECK(!overlap(r.weight, r.weightIcon));
        CHECK(!overlap(r.cost, r.heat)); CHECK(!overlap(r.cost, r.heatIcon)); CHECK(!overlap(r.heat, r.costIcon));
        for (const Rect* t : {&r.cost, &r.costIcon, &r.heat, &r.heatIcon}) CHECK(!overlap(r.name, *t));   // a long name never runs into the header/cost row
        CHECK(inside(r.name, r.item));
        CHECK(inside(r.warn, r.iconOutline));    // HEAT EXCEEDED stays inside the icon outline (no wrap past the item edge)
        CHECK(r.warn.x >= 0);
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

TEST_CASE("mechlab loc: every record sheet item is reachable by scrolling") {
    for (int p=0;p<8;++p) for (int item=0;item<20;++item) {
        const int scroll=scrollToReveal(20,0,item,p);
        const int row=item-scroll;
        REQUIRE(row>=0); REQUIRE(row<visibleRows(p));
        const auto r=itemRowRect(p,row,20);
        CHECK(itemIndexAt(p,20,scroll,r.cx(),r.cy()) == item);
        CHECK(scrollByWheel(20,scroll,100,p) <= maxScroll(20,p));
    }
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

// Old saves are in saved order with coordinates of the pre-change 6x4 grid (LA col 0, LT col 1, CT cols 2-3, RT col 4, RA col 5).
namespace {
LoadoutItem grid(int x, int y, int w, int h, mc2pkg::Mount m, int ord, float wt) { return {placedItem(x, y, w, h, m, ord, wt), false}; }
LoadoutItem slotItem(int ord, float wt) { return {placedItem(-1, -1, 1, 1, mc2pkg::Mount::None, ord, wt), true}; }
constexpr auto E = mc2pkg::Mount::Energy;
constexpr auto N = mc2pkg::Mount::None;

std::vector<LoadoutItem> astraSixSinks() {
    return {grid(1, 0, 1, 1, N, 7, 1.0f), grid(1, 1, 1, 1, N, 7, 1.0f), grid(1, 2, 1, 1, N, 7, 1.0f),
            grid(4, 0, 1, 1, N, 7, 1.0f), grid(4, 1, 1, 1, N, 7, 1.0f), grid(4, 2, 1, 1, N, 7, 1.0f),
            grid(0, 0, 1, 3, E, 145, 6.0f), grid(5, 0, 1, 3, E, 145, 6.0f),
            grid(0, 3, 1, 1, E, 156, 1.0f), grid(5, 3, 1, 1, E, 156, 1.0f)};
}
std::vector<LoadoutItem> missileMarauder() {
    return {grid(0, 0, 1, 3, E, 145, 6.0f), grid(0, 3, 1, 1, E, 156, 1.0f), grid(5, 0, 1, 3, E, 145, 6.0f), grid(5, 3, 1, 1, E, 156, 1.0f),
            grid(1, 0, 1, 2, mc2pkg::Mount::Missile, 60, 5.0f), grid(4, 0, 1, 1, N, 7, 1.0f), grid(4, 1, 1, 1, N, 7, 1.0f)};
}
// every invariant of a migration result: kept items in a region, no overlap, mounts ok, payload.
void checkInvariants(const mc2pkg::PackageChassis& c, const std::vector<LoadoutItem>& in, const LoadoutResult& r) {
    REQUIRE(r.cells.size() == in.size());
    std::vector<LoadoutItem> out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (!r.kept[i]) continue;
        LoadoutItem it = in[i];
        it.p.x = r.cells[i].x; it.p.y = r.cells[i].y;
        out.push_back(it);
    }
    CHECK(loadoutLegal(c, out));
}
bool at(const LoadoutResult& r, size_t i, int x, int y) { return r.kept[i] && r.cells[i].x == x && r.cells[i].y == y; }
const mc2pkg::Region* regionOf(const mc2pkg::PackageChassis& c, const LoadoutResult& r, const std::vector<LoadoutItem>& in, size_t i) {
    return mc2pkg::regionForFootprint(c, r.cells[i].x, r.cells[i].y, in[i].p.w, in[i].p.h);
}
}

TEST_CASE("mechlab loc U1: six heat sinks hold the cells under the arm mounts, nothing is dropped") {
    const mc2pkg::PackageChassis c = marauderShipped();
    const auto in = astraSixSinks();
    CHECK(!loadoutLegal(c, in));
    const LoadoutResult r = migrateLoadout(c, in);
    CHECK(r.dropped == 0);
    checkInvariants(c, in, r);
    CHECK(at(r, 6, 0, 0));
    CHECK(at(r, 7, 5, 0));
    REQUIRE(regionOf(c, r, in, 8));
    REQUIRE(regionOf(c, r, in, 9));
    CHECK(regionOf(c, r, in, 8)->location == "LeftArm");
    CHECK(regionOf(c, r, in, 9)->location == "RightArm");
    int sinksStayed = 0;
    for (size_t i = 0; i < 6; ++i) sinksStayed += at(r, i, in[i].p.x, in[i].p.y);
    CHECK(sinksStayed >= 4);
}

TEST_CASE("mechlab loc U2: a missile Marauder loses only the missile rack, and the refund is cost x mechs") {
    const mc2pkg::PackageChassis c = marauderShipped();
    const auto in = missileMarauder();
    const LoadoutResult r = migrateLoadout(c, in);
    CHECK(r.dropped == 1);
    CHECK(r.droppedWeapons == 1);
    CHECK(!r.kept[4]);
    for (size_t i = 0; i < in.size(); ++i) if (i != 4) CHECK(r.kept[i]);
    mc2pkg::Placed probe = in[4].p;
    std::vector<mc2pkg::Placed> others;
    for (size_t i = 0; i < in.size(); ++i) if (r.kept[i]) { mc2pkg::Placed p = in[i].p; p.x = r.cells[i].x; p.y = r.cells[i].y; others.push_back(p); }
    probe.x = -1; probe.y = -1;
    CHECK(std::string(mc2pkg::verdictName(mc2pkg::checkAdd(c, others, probe))) == "no-free-mount");
    checkInvariants(c, in, r);
    CHECK(layoutRefund(250, 0) == 0);
    CHECK(layoutRefund(250, 1) == 250);
    CHECK(layoutRefund(250, 2) == 500);
}

TEST_CASE("mechlab loc U3b: an absent LayoutRev (read error, value 0) is grandfathered rev 1 and restores") {
    CHECK(savedLayoutAction(savedLayoutRev(1, 0), 1) == SavedLayoutAction::Restore);
    CHECK(savedLayoutAction(savedLayoutRev(0, 2), 3) == SavedLayoutAction::Migrate);
    CHECK(savedLayoutAction(savedLayoutRev(0, 3), 3) == SavedLayoutAction::Restore);
}

TEST_CASE("mechlab loc U3: migration is idempotent, a legal loadout is returned unchanged") {
    CHECK(savedLayoutAction(2, 3) == SavedLayoutAction::Migrate);
    CHECK(savedLayoutAction(3, 3) == SavedLayoutAction::Restore);
    const mc2pkg::PackageChassis c = marauderShipped();
    for (const auto& in : {astraSixSinks(), missileMarauder()}) {
        const LoadoutResult first = migrateLoadout(c, in);
        std::vector<LoadoutItem> again;
        for (size_t i = 0; i < in.size(); ++i) {
            if (!first.kept[i]) continue;
            LoadoutItem it = in[i];
            it.p.x = first.cells[i].x; it.p.y = first.cells[i].y;
            again.push_back(it);
        }
        const LoadoutResult second = migrateLoadout(c, again);
        CHECK(second.moved == 0);
        CHECK(second.dropped == 0);
        REQUIRE(second.cells.size() == again.size());
        for (size_t i = 0, k = 0; i < in.size(); ++i) {
            if (!first.kept[i]) continue;
            CHECK(second.cells[k].x == first.cells[i].x);
            CHECK(second.cells[k].y == first.cells[i].y);
            ++k;
        }
    }
    // the shipped MAD-3R loadout on the current layout
    const std::vector<LoadoutItem> shipped = {grid(0, 0, 1, 3, E, 145, 6.0f), grid(1, 0, 1, 1, E, 156, 1.0f), grid(5, 0, 1, 3, E, 145, 6.0f),
                                              grid(4, 0, 1, 1, E, 156, 1.0f), grid(2, 0, 1, 1, N, 7, 1.0f), grid(2, 1, 1, 1, N, 7, 1.0f)};
    REQUIRE(loadoutLegal(c, shipped));
    const LoadoutResult r = migrateLoadout(c, shipped);
    CHECK(r.moved == 0);
    CHECK(r.dropped == 0);
    for (size_t i = 0; i < shipped.size(); ++i) CHECK(at(r, i, shipped[i].p.x, shipped[i].p.y));
}

TEST_CASE("mechlab loc: downgrade skips the saved layout for the pack default") {
    CHECK(savedLayoutAction(3, 2) == SavedLayoutAction::PackDefault);
    CHECK(savedLayoutAction(3, 1) == SavedLayoutAction::PackDefault);
    CHECK(savedLayoutAction(2, 1) == SavedLayoutAction::PackDefault);
}

TEST_CASE("mechlab loc U4: the same input migrates to the same result twice") {
    const mc2pkg::PackageChassis c = marauderShipped();
    for (const auto& in : {astraSixSinks(), missileMarauder()}) {
        const LoadoutResult a = migrateLoadout(c, in), b = migrateLoadout(c, in);
        REQUIRE(a.cells.size() == b.cells.size());
        for (size_t i = 0; i < a.cells.size(); ++i) {
            CHECK(a.cells[i].x == b.cells[i].x);
            CHECK(a.cells[i].y == b.cells[i].y);
            CHECK(a.kept[i] == b.kept[i]);
        }
    }
}

TEST_CASE("mechlab loc U5: payload pressure drops sinks, never a weapon") {
    const mc2pkg::PackageChassis c = marauderShipped();   // payload 34
    std::vector<LoadoutItem> in;
    for (int i = 0; i < 20; ++i) in.push_back(grid(2 + (i % 2), i / 2, 1, 1, N, 7, 1.0f));   // twenty 1 t sinks first in saved order
    in.push_back(grid(0, 0, 1, 3, E, 145, 7.0f));
    in.push_back(grid(5, 0, 1, 3, E, 145, 7.0f));
    in.push_back(grid(0, 3, 1, 1, E, 156, 6.0f));
    const LoadoutResult r = migrateLoadout(c, in);   // 20 t of weapons leave room for 14 sinks
    CHECK(r.droppedWeapons == 0);
    CHECK(r.dropped == 6);
    for (size_t i = 20; i < in.size(); ++i) CHECK(r.kept[i]);
    checkInvariants(c, in, r);
}

TEST_CASE("mechlab loc U6: slot items count toward mass, never move, and are dropped only when mass forces it") {
    const mc2pkg::PackageChassis c = marauderShipped();
    std::vector<LoadoutItem> in = {slotItem(90, 2.0f), grid(0, 3, 1, 1, E, 156, 1.0f), grid(0, 0, 1, 3, E, 145, 6.0f)};   // ML on the old row: forces a migration
    LoadoutResult r = migrateLoadout(c, in);
    CHECK(r.dropped == 0);
    CHECK(r.kept[0]);
    CHECK(r.cells[0].x == -1);
    CHECK(r.cells[0].y == -1);
    in.push_back(grid(1, 0, 1, 1, N, 160, 32.0f));   // 2 + 1 + 6 + 32 > 34: the equipment goes, the slot item stays
    r = migrateLoadout(c, in);
    CHECK(r.kept[0]);
    CHECK(!r.kept[3]);
    in[0].p.weight = 30.0f;                           // a slot item heavy enough that mass forces a drop of something
    r = migrateLoadout(c, in);
    CHECK(r.dropped >= 1);
    CHECK(mc2pkg::massUsed([&] { std::vector<mc2pkg::Placed> v; for (size_t i = 0; i < in.size(); ++i) if (r.kept[i]) v.push_back(in[i].p); return v; }()) <= c.payload + 1e-3f);
}

TEST_CASE("mechlab loc U7: when keeping cells fragments a region the packed attempt wins") {
    const mc2pkg::PackageChassis c = marauderShipped();
    const int rt = 3;   // RightTorso: ballistic mounts, 2x3 cells at (4,3)
    REQUIRE(c.regions[rt].location == "RightTorso");
    const std::vector<LoadoutItem> in = {
        grid(4, 4, 1, 1, mc2pkg::Mount::Ballistic, 120, 0.5f),   // kept cells split the torso rows
        grid(5, 4, 1, 1, mc2pkg::Mount::Ballistic, 120, 0.5f),
        grid(0, 0, 1, 3, mc2pkg::Mount::Ballistic, 100, 5.0f),   // an old-layout cell with no ballistic mount: has to move
    };
    const LoadoutResult sticky = detail::solveLoadout(c, in, true);
    const LoadoutResult packed = detail::solveLoadout(c, in, false);
    CHECK(sticky.droppedWeapons == 1);
    CHECK(packed.droppedWeapons == 0);
    const LoadoutResult r = migrateLoadout(c, in);
    CHECK(r.dropped == 0);
    CHECK(at(r, 2, 4, 3));
    checkInvariants(c, in, r);
}

TEST_CASE("mechlab loc U10: view state resets scrolls and re-keys the doll icon") {
    LocViewState v;
    v.scroll[2] = 4; v.scroll[kDoll] = 1;
    v.dollKey = "mad";
    CHECK(!v.needsIcon("mad"));
    CHECK(v.needsIcon("hunchback"));
    v.reset();
    for (int s : v.scroll) CHECK(s == 0);
    CHECK(v.needsIcon("mad"));
    v.dollKey = "mad";
    CHECK(!v.needsIcon("mad"));
    CHECK(v.needsIcon("hunchback"));
}

TEST_CASE("layout/armor warnings: untouched stock none, edited layout or armor shows them") {
    CHECK_FALSE(mc2::mechlab_loc::showLayoutArmorWarnings(false, false));
    CHECK(mc2::mechlab_loc::showLayoutArmorWarnings(true, false));
    CHECK(mc2::mechlab_loc::showLayoutArmorWarnings(false, true));
}

TEST_CASE("overweight warning is package-only; stock Bushwacker 56/55 is not flagged") {
    CHECK_FALSE(mc2::mechlab_loc::overweight(false, 38.f, 0.f));
    CHECK(mc2::mechlab_loc::overweight(true, 34.5f, 34.f));
    CHECK_FALSE(mc2::mechlab_loc::overweight(true, 34.f, 34.f));
}
