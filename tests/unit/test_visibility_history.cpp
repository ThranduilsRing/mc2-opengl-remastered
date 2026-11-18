#include "doctest.h"
#include "../../RenderCore/VisibilityHistory.h"
using RenderCore::VisibilityHistory;
using RenderCore::VisibilityTier;

TEST_SUITE("VisibilityHistory") {
TEST_CASE("handleless prop records cannot change actor visibility") {
    VisibilityHistory actors(2), withProps(5);
    for (uint32_t frame=0; frame<4; ++frame) {
        actors.advance(); withProps.advance();
        const uint32_t expectedSlot=frame%VisibilityHistory::kSlots;
        auto sample=[&](uint32_t slot, VisibilityHistory::Sample& s) {
            if (slot!=expectedSlot) return false;
            s.count=2;
            s.ids[0]=7; s.bits[0]=frame%2;
            s.ids[1]=19; s.bits[1]=(frame+1)%2;
            return true;
        };
        REQUIRE(actors.consume(sample)==VisibilityTier::Tier1_NMinusOne);
        REQUIRE(withProps.consume([&](uint32_t slot, VisibilityHistory::Sample& s) {
            if (!sample(slot,s)) return false;
            // Both admitted and rejected static leaves have no actor handle.
            s.count=5;
            s.ids[2]=0; s.bits[2]=0;
            s.ids[3]=0; s.bits[3]=1;
            s.ids[4]=0; s.bits[4]=frame%2;
            return true;
        })==VisibilityTier::Tier1_NMinusOne);
        for (bool conservativeOr : {false,true}) {
            const auto a=actors.buildSnapshot(32,conservativeOr,true);
            const auto b=withProps.buildSnapshot(32,conservativeOr,true);
            CHECK(a.overflow==b.overflow);
            CHECK(a.visible==b.visible);
            CHECK(a.invisible==b.invisible);
            CHECK(a.orAdmits==b.orAdmits);
            for (uint32_t id=0; id<32; ++id)
                CHECK(actors.isVisible(id)==withProps.isVisible(id));
        }
    }
}
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

#include "../../GameOS/gameos/static_prop_visible_set.h"
#include <vector>

TEST_CASE("visible static store clears nonempty empty different populations and preserves frozen records") {
    struct Store {
        uint64_t generation = 7;
        std::vector<uint32_t> instances;
        uint64_t persistentStaticGen() const { return generation; }
        void clearPersistentStatic() { instances.clear(); generation = ~uint64_t(0); }
    } store;
    uint64_t signature = 0;
    const std::vector<uint32_t> first{1, 2}, empty, next{3};
    auto flush = [&](const std::vector<uint32_t>& visible, bool frozen = false) {
        const bool dirty = static_prop::prepareVisibleStore(store, visible, frozen, 7, signature);
        if (dirty) { store.instances = visible; store.generation = 7; }
        return dirty;
    };
    REQUIRE(flush(first));
    REQUIRE(store.instances.size() == 2);
    CHECK(store.instances[0] == 1);
    CHECK_FALSE(flush(first));
    REQUIRE(flush(empty));
    CHECK(store.instances.empty());
    CHECK_FALSE(flush(empty));
    REQUIRE(flush(next));
    REQUIRE(store.instances.size() == 1);
    CHECK(store.instances[0] == 3);
    const auto beforeFrozen = signature;
    CHECK_FALSE(flush(empty, true));
    REQUIRE(store.instances.size() == 1);
    CHECK(store.instances[0] == 3);
    CHECK(signature == beforeFrozen);
}

TEST_CASE("VisibilityHistory: transition stats equal full rescan with duplicate IDs and OR") {
    VisibilityHistory h(8);
    for (uint32_t frame=0; frame<6; ++frame) {
        h.advance();
        REQUIRE(h.consume([&](uint32_t slot, VisibilityHistory::Sample& s) {
            if (slot!=frame%3) return false;
            s.ids={0,1,1,2,2,7,19,999};
            s.bits={0,0,static_cast<uint8_t>(frame%2),1,0,0,1,0};
            s.count=8; return true;
        })==VisibilityTier::Tier1_NMinusOne);
        for (uint32_t cap : {0u,1u,8u,32u,1001u}) for (bool useOr : {false,true}) {
            const auto stats=h.buildSnapshot(cap,useOr,true);
            uint32_t visible=0, invisible=0;
            for (uint32_t id=1; id<cap; ++id) {
                if (h.isVisible(id)) ++visible; else ++invisible;
            }
            CHECK(stats.visible==visible); CHECK(stats.invisible==invisible);
            const auto quiet=h.buildSnapshot(cap,useOr,false);
            CHECK(quiet.visible==0); CHECK(quiet.invisible==0);
            CHECK(quiet.orAdmits==stats.orAdmits); CHECK(quiet.overflow==stats.overflow);
        }
    }
}
