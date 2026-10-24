// Self-lit two-sided card predicate: stock coil-style types are doubled, imported overrides never are.
#include "doctest.h"

#include "gos_static_prop_selflit.h"

#include <cstdint>
#include <span>
#include <vector>

namespace {
struct V { std::uint32_t aRGBLight; };
}

TEST_CASE("selflit: stock all-0xffffffff type is a two-sided card") {
    std::vector<V> v(4, V{0xffffffffu});
    CHECK(mc2_prop::isSelfLitTwoSidedCard<V>(false, std::span<const V>(v)));
}

TEST_CASE("selflit: an imported all-0xffffffff shape is NOT doubled") {
    std::vector<V> v(4, V{0xffffffffu});
    CHECK_FALSE(mc2_prop::isSelfLitTwoSidedCard<V>(true, std::span<const V>(v)));
}

TEST_CASE("selflit: any other vertex light, or an empty shape, is not a card") {
    std::vector<V> v(4, V{0xffffffffu});
    v[2].aRGBLight = 0xff808080u;
    CHECK_FALSE(mc2_prop::isSelfLitTwoSidedCard<V>(false, std::span<const V>(v)));
    CHECK_FALSE(mc2_prop::isSelfLitTwoSidedCard<V>(false, {}));
}
