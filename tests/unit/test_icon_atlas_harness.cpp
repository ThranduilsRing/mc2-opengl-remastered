#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "icon_atlas_cell.h"

namespace {
namespace ia = icon_atlas;

TEST_CASE("icon_atlas_harness/cols_retail_256") {
    CHECK((ia::columnsForAtlas(256.f, 25.f) == 10));
    return;
}

TEST_CASE("icon_atlas_harness/cols_mc2x_512") {
    CHECK((ia::columnsForAtlas(512.f, 25.f) == 20));
    return;
}

TEST_CASE("icon_atlas_harness/cols_mco_1024_floors_not_rounds") {
    CHECK((ia::columnsForAtlas(1024.f, 40.f) == 25));
    return;
}

TEST_CASE("icon_atlas_harness/cols_exact_division_epsilon") {
    CHECK((ia::columnsForAtlas(256.f, 32.f) == 8));
    CHECK((ia::columnsForAtlas(512.f, 64.f) == 8));
    return;
}

TEST_CASE("icon_atlas_harness/cols_fallback_on_zero_cell") {
    CHECK((ia::columnsForAtlas(1024.f, 0.f) == ia::kFallbackCols));
    CHECK((ia::columnsForAtlas(1024.f, -5.f) == ia::kFallbackCols));
    return;
}

TEST_CASE("icon_atlas_harness/cols_fallback_when_cell_wider_than_atlas") {

    CHECK((ia::columnsForAtlas(30.f, 40.f) == ia::kFallbackCols));
    return;
}

TEST_CASE("icon_atlas_harness/cell_index0") {
    ia::Cell c = ia::cellForIndex(0, 25.f, 38.f, 256.f);
    CHECK((c.col == 0 && c.row == 0));
    CHECK((c.u == 0.f && c.v == 0.f));
    CHECK((c.u2 == 25.f && c.v2 == 38.f));
    return;
}

TEST_CASE("icon_atlas_harness/cell_high_index_wrap_512") {
    ia::Cell c = ia::cellForIndex(142, 25.f, 38.f, 512.f);
    CHECK((c.cols == 20));
    CHECK((c.col == 142 % 20));
    CHECK((c.row == 142 / 20));
    CHECK((c.u == (float)(142 % 20) * 25.f));
    CHECK((c.v == (float)(142 / 20) * 38.f));
    return;
}

TEST_CASE("icon_atlas_harness/cell_row_boundary") {
    ia::Cell last = ia::cellForIndex(9, 25.f, 38.f, 256.f);
    CHECK((last.col == 9 && last.row == 0));
    ia::Cell wrap = ia::cellForIndex(10, 25.f, 38.f, 256.f);
    CHECK((wrap.col == 0 && wrap.row == 1));
    CHECK((wrap.v == 38.f));
    return;
}

TEST_CASE("icon_atlas_harness/cell_uv_monotonic") {
    for (long idx : {0, 1, 25, 99, 142}) {
        ia::Cell c = ia::cellForIndex(idx, 25.f, 38.f, 512.f);
        CHECK((c.u2 > c.u));
        CHECK((c.v2 > c.v));
    }
    return;
}

TEST_CASE("icon_atlas_harness/cell_odd_dimensions") {
    ia::Cell c = ia::cellForIndex(50, 33.f, 41.f, 777.f);
    CHECK((c.cols == 23));
    CHECK((c.col == 50 % 23 && c.row == 50 / 23));
    return;
}

TEST_CASE("icon_atlas_harness/checked_retail_256_bounds") {
    bool oob = true;
    ia::Cell c = ia::cellForIndexChecked(79, 25.f, 30.f, 256.f, 256.f, &oob);
    CHECK((!oob && c.row == 7));
    ia::Cell c2 = ia::cellForIndexChecked(80, 25.f, 30.f, 256.f, 256.f, &oob);
    CHECK((oob && c2.row == 8));
    ia::Cell legacy = ia::cellForIndex(80, 25.f, 30.f, 256.f);
    CHECK((c2.col == legacy.col && c2.row == legacy.row &&
                c2.u == legacy.u && c2.v == legacy.v));
    return;
}

TEST_CASE("icon_atlas_harness/checked_mc2x_512_bounds") {
    bool oob = true;
    ia::Cell c = ia::cellForIndexChecked(142, 25.f, 30.f, 512.f, 512.f, &oob);
    CHECK((!oob && c.row == 7));
    (void)ia::cellForIndexChecked(118, 25.f, 30.f, 512.f, 512.f, &oob);
    CHECK((!oob));
    (void)ia::cellForIndexChecked(339, 25.f, 30.f, 512.f, 512.f, &oob);
    CHECK((!oob));
    (void)ia::cellForIndexChecked(340, 25.f, 30.f, 512.f, 512.f, &oob);
    CHECK((oob));
    return;
}

TEST_CASE("icon_atlas_harness/checked_mco_1024_bounds") {
    bool oob = true;
    (void)ia::cellForIndexChecked(624, 40.f, 40.f, 1024.f, 1024.f, &oob);
    CHECK((!oob));
    (void)ia::cellForIndexChecked(625, 40.f, 40.f, 1024.f, 1024.f, &oob);
    CHECK((oob));
    return;
}

TEST_CASE("icon_atlas_harness/checked_unknown_geometry_never_flags") {
    bool oob = true;
    (void)ia::cellForIndexChecked(9999, 25.f, 30.f, 256.f, 0.f, &oob);
    CHECK((!oob));
    (void)ia::cellForIndexChecked(9999, 25.f, 0.f, 256.f, 256.f, &oob);
    CHECK((!oob));
    return;
}

TEST_CASE("icon_atlas_harness/checked_negative_and_exact_rows") {
    bool oob = false;
    (void)ia::cellForIndexChecked(-1, 25.f, 30.f, 256.f, 256.f, &oob);
    CHECK((oob));

    (void)ia::cellForIndexChecked(63, 32.f, 32.f, 256.f, 256.f, &oob);
    CHECK((!oob));
    (void)ia::cellForIndexChecked(64, 32.f, 32.f, 256.f, 256.f, &oob);
    CHECK((oob));
    return;
}

TEST_CASE("icon_atlas_harness/demo_intentional_fail" * doctest::should_fail()) {
    CHECK((ia::columnsForAtlas(1024.f, 40.f) == 26));
    return;
}

}
