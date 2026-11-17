#include "RenderCore/PropListRoute.h"
#include "doctest.h"

TEST_CASE("prop_list_route: opaque packets split by double-sidedness, alpha packets go to core") {
    using namespace PropList;
    // A tree type: opaque double-sided trunk first, then the alpha leaf packet, then an opaque packet after it.
    auto trunk = routeCore(true, 0, true);
    CHECK(trunk.list == kOpaqueDoubleSided);
    CHECK(trunk.opaquePrefix);
    auto leaf = routeCore(trunk.opaquePrefix, 1, true);
    CHECK(leaf.list == kCore);
    CHECK(leaf.alphaTestable);
    auto after = routeCore(leaf.opaquePrefix, 0, false);   // a later coplanar packet keeps its order behind the leaf
    CHECK(after.list == kCore);
    CHECK_FALSE(after.alphaTestable);
    // A building: single-sided opaque packets take the culling opaque list.
    CHECK(routeCore(true, 0, false).list == kOpaque);
    CHECK(routeCore(true, 2, false).list == kCore);   // alphaTest == 2 (late class) is alpha-tested too
}

TEST_CASE("prop_list_route: a single-sided opaque packet after a double-sided one keeps draw order") {
    using namespace PropList;
    auto ss = routeCore(true, 0, false);
    CHECK(ss.list == kOpaque);
    auto ds = routeCore(ss.opaquePrefix, 0, true, ss.doubleSidedSeen);
    CHECK(ds.list == kOpaqueDoubleSided);   // kOpaque -> kOpaqueDoubleSided is already in draw order
    CHECK(ds.doubleSidedSeen);
    auto ds2 = routeCore(ds.opaquePrefix, 0, true, ds.doubleSidedSeen);
    CHECK(ds2.list == kOpaqueDoubleSided);
    auto late = routeCore(ds2.opaquePrefix, 0, false, ds2.doubleSidedSeen);
    CHECK(late.list == kCore);   // kOpaque draws before kOpaqueDoubleSided, so it would lose the GEQUAL tie
    CHECK_FALSE(late.opaquePrefix);
}

TEST_CASE("prop_list_route: only tier 0 keeps the soft fringe") {
    CHECK_FALSE(PropList::tierSkipsFringe(0));
    CHECK(PropList::tierSkipsFringe(1));
    CHECK(PropList::tierSkipsFringe(2));
    CHECK(PropList::tierSkipsFringe(3));
}
