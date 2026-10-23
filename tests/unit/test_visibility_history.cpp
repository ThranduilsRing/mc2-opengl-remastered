#include "doctest.h"
#include "../../RenderCore/VisibilityHistory.h"
using RenderCore::VisibilityHistory;
using RenderCore::VisibilityTier;

TEST_SUITE("VisibilityHistory") {
TEST_CASE("dense terrain missions retain high actor IDs across range changes") {
    VisibilityHistory h(1); h.advance();
    auto high=[](uint32_t slot, VisibilityHistory::Sample& s) {
        if (slot!=0) return false;
        s.count=1; s.ids[0]=45000; s.bits[0]=0; return true;
    };
    REQUIRE(h.consume(high)==VisibilityTier::Tier1_NMinusOne);
    CHECK(h.buildSnapshot(60001,false).overflow==0);
    CHECK_FALSE(h.isVisible(45000));
    CHECK(h.isVisible(60000)); // no submitted record stays conservative
    CHECK(h.buildSnapshot(100,false).overflow==45000);
    CHECK(h.isVisible(45000)); // stale/out-of-range handles fail open
}
TEST_CASE("accepted IDs remain paired across ring reuse and conservative OR") {
    VisibilityHistory h(3);
    auto first=[](uint32_t slot, VisibilityHistory::Sample& s) {
        if (slot!=0) return false;
        s.count=2; s.ids[0]=7; s.bits[0]=1; s.ids[1]=19; s.bits[1]=0; return true;
    };
    h.advance(); REQUIRE(h.consume(first)==VisibilityTier::Tier1_NMinusOne);
    h.buildSnapshot(8192,true); CHECK(h.isVisible(7)); CHECK_FALSE(h.isVisible(19));
    auto second=[](uint32_t slot, VisibilityHistory::Sample& s) {
        if (slot!=1) return false;
        // The record order changed: interpreting the old bits with new IDs loses actor7.
        s.count=2; s.ids[0]=19; s.bits[0]=1; s.ids[1]=7; s.bits[1]=0; return true;
    };
    h.advance(); REQUIRE(h.consume(second)==VisibilityTier::Tier1_NMinusOne);
    CHECK(h.buildSnapshot(8192,true).orAdmits==1);
    CHECK(h.isVisible(7)); CHECK(h.isVisible(19));
    h.buildSnapshot(8192,false); CHECK_FALSE(h.isVisible(7)); CHECK(h.isVisible(19));
}
TEST_CASE("N-2 is accepted without waiting for N-1") {
    VisibilityHistory h(1); h.advance();
    unsigned probes=0;
    auto ready=[&](uint32_t slot, VisibilityHistory::Sample& s) {
        ++probes; if (slot!=2) return false;
        s.count=1; s.ids[0]=11; s.bits[0]=0; return true;
    };
    CHECK(h.consume(ready)==VisibilityTier::Tier2_NMinusTwo);
    CHECK(probes==3); h.buildSnapshot(8192,false); CHECK_FALSE(h.isVisible(11));
}
TEST_CASE("ten unavailable frames abandon old invisibility and fail open") {
    VisibilityHistory h(1); h.advance();
    auto once=[](uint32_t slot, VisibilityHistory::Sample& s) {
        if (slot!=0) return false;
        s.count=1; s.ids[0]=5; s.bits[0]=0; return true;
    };
    REQUIRE(h.consume(once)==VisibilityTier::Tier1_NMinusOne);
    auto unavailable=[](uint32_t, VisibilityHistory::Sample&){return false;};
    for (unsigned i=0;i<9;++i) { h.advance(); h.consume(unavailable); }
    h.buildSnapshot(8192,true); CHECK_FALSE(h.isVisible(5));
    h.advance(); CHECK(h.consume(unavailable)==VisibilityTier::Tier3_Conservative);
    h.buildSnapshot(8192,true); CHECK(h.isVisible(5)); CHECK(h.visibleCount()==UINT32_MAX);
}
TEST_CASE("bad actor handles and impossible record counts never index outside samples") {
    VisibilityHistory h(1); h.advance();
    auto badId=[](uint32_t slot, VisibilityHistory::Sample& s) {
        if (slot!=0) return false;
        s.count=1; s.ids[0]=99999; s.bits[0]=0; return true;
    };
    h.consume(badId); CHECK(h.buildSnapshot(8192,true).overflow==99999);
    CHECK(h.isVisible(99999));
    auto badCount=[](uint32_t, VisibilityHistory::Sample& s) {s.count=2;return true;};
    CHECK(h.consume(badCount)==VisibilityTier::Tier3_Conservative);
}
}
