// tools/icon_atlas_harness/icon_atlas_harness.cpp
// SUBSYSTEM-HARNESS-ARC / ICON-ATLAS-HARNESS-1
//
// Tests the REAL icon-atlas cell/UV math (code/icon_atlas_cell.h) that
// logisticsmechicon.cpp and mechlistbox.cpp both call after
// ICON-ATLAS-CELL-EXTRACT-1 — no duplication, no GL, no game, no atlas texture.
// Regression-locks the column-count fixes (MCO 1024/40 phantom column, MC2X 512
// high-index addressing) whose edge cases are invisible to a 30s tier1 smoke.
//
// Build (standalone):
//   cmake -S tools/icon_atlas_harness -B build64-iconatlas -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-iconatlas --config RelWithDebInfo --target icon_atlas_harness

#include "contract_harness.h"
#include "icon_atlas_cell.h"

using namespace contract_harness;
namespace ia = icon_atlas;

// ---- column-count tests (the bug class) ------------------------------------

static bool test_cols_retail_256(TestCtx& t) {
    CH_CHECK(t, ia::columnsForAtlas(256.f, 25.f) == 10);   // 10.24 -> 10
    return t.failures == 0;
}

static bool test_cols_mc2x_512(TestCtx& t) {
    CH_CHECK(t, ia::columnsForAtlas(512.f, 25.f) == 20);   // 20.48 -> 20
    return t.failures == 0;
}

// THE regression lock: MCO 1024-wide / 40px cells must FLOOR to 25, not round to 26.
static bool test_cols_mco_1024_floors_not_rounds(TestCtx& t) {
    CH_CHECK(t, ia::columnsForAtlas(1024.f, 40.f) == 25);  // 25.6 -> 25 (NOT 26)
    return t.failures == 0;
}

static bool test_cols_exact_division_epsilon(TestCtx& t) {
    CH_CHECK(t, ia::columnsForAtlas(256.f, 32.f) == 8);    // exactly 8, epsilon must not push to 7
    CH_CHECK(t, ia::columnsForAtlas(512.f, 64.f) == 8);
    return t.failures == 0;
}

static bool test_cols_fallback_on_zero_cell(TestCtx& t) {
    CH_CHECK(t, ia::columnsForAtlas(1024.f, 0.f) == ia::kFallbackCols);   // cellW<=0 -> 10
    CH_CHECK(t, ia::columnsForAtlas(1024.f, -5.f) == ia::kFallbackCols);
    return t.failures == 0;
}

static bool test_cols_fallback_when_cell_wider_than_atlas(TestCtx& t) {
    // atlas 30 wide, cell 40 wide -> 0 cols -> fallback 10 (never 0, avoids %0).
    CH_CHECK(t, ia::columnsForAtlas(30.f, 40.f) == ia::kFallbackCols);
    return t.failures == 0;
}

// ---- cell/UV tests ---------------------------------------------------------

static bool test_cell_index0(TestCtx& t) {
    ia::Cell c = ia::cellForIndex(0, 25.f, 38.f, 256.f);
    CH_CHECK(t, c.col == 0 && c.row == 0);
    CH_CHECK(t, c.u == 0.f && c.v == 0.f);
    CH_CHECK(t, c.u2 == 25.f && c.v2 == 38.f);
    return t.failures == 0;
}

// High-index wrap on a 512 atlas (20 cols): APC=142 -> col 2, row 7.
static bool test_cell_high_index_wrap_512(TestCtx& t) {
    ia::Cell c = ia::cellForIndex(142, 25.f, 38.f, 512.f);
    CH_CHECK(t, c.cols == 20);
    CH_CHECK(t, c.col == 142 % 20);   // 2
    CH_CHECK(t, c.row == 142 / 20);   // 7
    CH_CHECK(t, c.u == (float)(142 % 20) * 25.f);
    CH_CHECK(t, c.v == (float)(142 / 20) * 38.f);
    return t.failures == 0;
}

// Final cell on the row boundary: last column then wrap to next row.
static bool test_cell_row_boundary(TestCtx& t) {
    ia::Cell last = ia::cellForIndex(9, 25.f, 38.f, 256.f);   // 10 cols -> idx 9 = col 9 row 0
    CH_CHECK(t, last.col == 9 && last.row == 0);
    ia::Cell wrap = ia::cellForIndex(10, 25.f, 38.f, 256.f);  // idx 10 = col 0 row 1
    CH_CHECK(t, wrap.col == 0 && wrap.row == 1);
    CH_CHECK(t, wrap.v == 38.f);
    return t.failures == 0;
}

// UVs strictly increase (u2>u, v2>v) for any valid cell.
static bool test_cell_uv_monotonic(TestCtx& t) {
    for (long idx : {0, 1, 25, 99, 142}) {
        ia::Cell c = ia::cellForIndex(idx, 25.f, 38.f, 512.f);
        CH_CHECK(t, c.u2 > c.u);
        CH_CHECK(t, c.v2 > c.v);
    }
    return t.failures == 0;
}

// Odd / non-square atlas dimensions must not crash or divide by zero.
static bool test_cell_odd_dimensions(TestCtx& t) {
    ia::Cell c = ia::cellForIndex(50, 33.f, 41.f, 777.f);  // 777/33 = 23.5 -> 23
    CH_CHECK(t, c.cols == 23);
    CH_CHECK(t, c.col == 50 % 23 && c.row == 50 / 23);
    return t.failures == 0;
}

// ---- cellForIndexChecked row-bound tests (META-FIX design B PART 3a) -------
// Validates the OOB row bound against the THREE shipped atlas geometries
// (retail 256x256, MC2X 512x512, MCO 1024-wide/40px cells) — the smoke
// icon_oob bucket may only be armed red after these pass (cellW/cellH come
// from WIDGET size at the call sites, so the bound must be proven here).

// Retail 256x256, 25x30 cells: 10 cols x 8 rows = idx 0..79 in-bounds.
static bool test_checked_retail_256_bounds(TestCtx& t) {
    bool oob = true;
    ia::Cell c = ia::cellForIndexChecked(79, 25.f, 30.f, 256.f, 256.f, &oob);
    CH_CHECK(t, !oob && c.row == 7);                 // last legit retail slot
    ia::Cell c2 = ia::cellForIndexChecked(80, 25.f, 30.f, 256.f, 256.f, &oob);
    CH_CHECK(t, oob && c2.row == 8);                 // first OOB row...
    ia::Cell legacy = ia::cellForIndex(80, 25.f, 30.f, 256.f);
    CH_CHECK(t, c2.col == legacy.col && c2.row == legacy.row &&
                c2.u == legacy.u && c2.v == legacy.v);   // ...cell UNCHANGED
    return t.failures == 0;
}

// MC2X 512x512, 25x30 widget cells: 20 cols x 17 rows (512/30=17.07->17).
// High-index mod icons (Ambulance=118, APC=142) must stay in-bounds.
static bool test_checked_mc2x_512_bounds(TestCtx& t) {
    bool oob = true;
    ia::Cell c = ia::cellForIndexChecked(142, 25.f, 30.f, 512.f, 512.f, &oob);
    CH_CHECK(t, !oob && c.row == 7);                 // APC=142 legit
    (void)ia::cellForIndexChecked(118, 25.f, 30.f, 512.f, 512.f, &oob);
    CH_CHECK(t, !oob);                               // Ambulance=118 legit
    (void)ia::cellForIndexChecked(339, 25.f, 30.f, 512.f, 512.f, &oob);
    CH_CHECK(t, !oob);                               // row 16 = last in-bounds
    (void)ia::cellForIndexChecked(340, 25.f, 30.f, 512.f, 512.f, &oob);
    CH_CHECK(t, oob);                                // row 17 = OOB
    return t.failures == 0;
}

// MCO 1024-wide / 40px cells, 1024 tall: 25 cols x 25 rows (both floor).
static bool test_checked_mco_1024_bounds(TestCtx& t) {
    bool oob = true;
    (void)ia::cellForIndexChecked(624, 40.f, 40.f, 1024.f, 1024.f, &oob);
    CH_CHECK(t, !oob);                               // col 24 row 24 = last
    (void)ia::cellForIndexChecked(625, 40.f, 40.f, 1024.f, 1024.f, &oob);
    CH_CHECK(t, oob);                                // row 25 = OOB
    return t.failures == 0;
}

// Unknown geometry (atlasH/cellH <= 0) must NEVER flag — bound unknowable.
static bool test_checked_unknown_geometry_never_flags(TestCtx& t) {
    bool oob = true;
    (void)ia::cellForIndexChecked(9999, 25.f, 30.f, 256.f, 0.f, &oob);
    CH_CHECK(t, !oob);
    (void)ia::cellForIndexChecked(9999, 25.f, 0.f, 256.f, 256.f, &oob);
    CH_CHECK(t, !oob);
    return t.failures == 0;
}

// Negative index flags; exact-division epsilon holds on the row bound.
static bool test_checked_negative_and_exact_rows(TestCtx& t) {
    bool oob = false;
    (void)ia::cellForIndexChecked(-1, 25.f, 30.f, 256.f, 256.f, &oob);
    CH_CHECK(t, oob);
    // 256/32 = exactly 8 rows: idx 63 (row 7, 8 cols) in, idx 64 (row 8) out.
    (void)ia::cellForIndexChecked(63, 32.f, 32.f, 256.f, 256.f, &oob);
    CH_CHECK(t, !oob);
    (void)ia::cellForIndexChecked(64, 32.f, 32.f, 256.f, 256.f, &oob);
    CH_CHECK(t, oob);
    return t.failures == 0;
}

// Demo failure (in_default=false): proves the failure path via --test only.
static bool test_demo_intentional_fail(TestCtx& t) {
    CH_CHECK(t, ia::columnsForAtlas(1024.f, 40.f) == 26);  // intentionally wrong (real is 25)
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("icon_atlas_harness");
    h.add("cols_retail_256",                    test_cols_retail_256);
    h.add("cols_mc2x_512",                      test_cols_mc2x_512);
    h.add("cols_mco_1024_floors_not_rounds",    test_cols_mco_1024_floors_not_rounds);
    h.add("cols_exact_division_epsilon",        test_cols_exact_division_epsilon);
    h.add("cols_fallback_on_zero_cell",         test_cols_fallback_on_zero_cell);
    h.add("cols_fallback_when_cell_wider_than_atlas", test_cols_fallback_when_cell_wider_than_atlas);
    h.add("cell_index0",                        test_cell_index0);
    h.add("cell_high_index_wrap_512",           test_cell_high_index_wrap_512);
    h.add("cell_row_boundary",                  test_cell_row_boundary);
    h.add("cell_uv_monotonic",                  test_cell_uv_monotonic);
    h.add("cell_odd_dimensions",                test_cell_odd_dimensions);
    h.add("checked_retail_256_bounds",          test_checked_retail_256_bounds);
    h.add("checked_mc2x_512_bounds",            test_checked_mc2x_512_bounds);
    h.add("checked_mco_1024_bounds",            test_checked_mco_1024_bounds);
    h.add("checked_unknown_geometry_never_flags", test_checked_unknown_geometry_never_flags);
    h.add("checked_negative_and_exact_rows",    test_checked_negative_and_exact_rows);
    h.add("demo_intentional_fail",              test_demo_intentional_fail, /*inDefault=*/false);
    return h.run(argc, argv);
}
