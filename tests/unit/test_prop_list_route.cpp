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

TEST_CASE("prop_list_route: only tier 0 keeps the soft fringe") {
    CHECK_FALSE(PropList::tierSkipsFringe(0));
    CHECK(PropList::tierSkipsFringe(1));
    CHECK(PropList::tierSkipsFringe(2));
    CHECK(PropList::tierSkipsFringe(3));
}
