// code/icon_atlas_cell.h
//
// Pure cell/UV math for indexing a mech/unit icon out of a grid atlas texture.
// Extracted (ICON-ATLAS-CELL-EXTRACT-1) from byte-identical copies that lived in
// code/logisticsmechicon.cpp and code/mechlistbox.cpp, so the column-count and UV
// derivation has ONE definition that both call (and that a contract harness can
// test without launching the game — see tools/icon_atlas_harness/).
//
// Firewall: header-only, no GL, no game-side headers, no globals. Inputs are the
// icon index, the cell (widget) width/height in px, and the atlas width in px;
// output is the column count and the four pixel-space UV corners.
//
// FLOOR (not round) column count: a W-wide atlas of `cellW`-px cells holds
// floor(W/cellW) columns. A prior `+0.5f` rounded UP when the remainder >= half a
// cell (MCO's 1024/40 = 25.6 -> 26), inventing a phantom column that misplaced
// high-index icons. The +0.01f epsilon guards float underflow on exact divisions.
// Retail (256/25=10.24) and MC2X (512/25=20.48) floor identically to the legacy
// value. When cellW <= 0 (atlas size unavailable) or the computed count < 1, fall
// back to 10 columns (the legacy square-atlas default).

#ifndef MC2_ICON_ATLAS_CELL_H
#define MC2_ICON_ATLAS_CELL_H

namespace icon_atlas {

constexpr long kFallbackCols = 10;

struct Cell {
    long  cols;   // columns in the atlas
    long  col;    // index % cols
    long  row;    // index / cols
    float u;      // left  px (col * cellW)
    float v;      // top   px (row * cellH)
    float u2;     // right px ((col+1) * cellW)
    float v2;     // bottom px ((row+1) * cellH)
};

// Column count from atlas width and cell width — floor with epsilon, fallback 10.
inline long columnsForAtlas(float atlasW, float cellW) {
    long cols = (cellW > 0.f) ? (long)(atlasW / cellW + 0.01f) : kFallbackCols;
    if (cols < 1) cols = kFallbackCols;
    return cols;
}

// Full cell/UV derivation for `index` into an atlas of `cellW` x `cellH` cells
// within an `atlasW`-px-wide texture. Pixel-space UVs (caller divides by atlas
// size for normalized UVs, matching the legacy call sites).
inline Cell cellForIndex(long index, float cellW, float cellH, float atlasW) {
    Cell c;
    c.cols = columnsForAtlas(atlasW, cellW);
    c.col  = index % c.cols;
    c.row  = index / c.cols;
    c.u  = (float)c.col * cellW;
    c.v  = (float)c.row * cellH;
    c.u2 = (float)(c.col + 1) * cellW;
    c.v2 = (float)(c.row + 1) * cellH;
    return c;
}

// Row count from atlas height and cell height — same floor+epsilon as
// columnsForAtlas, but NO fallback: <=0 inputs return 0 = "row bound
// unknowable" (the checked variant then never flags).
inline long rowsForAtlas(float atlasH, float cellH) {
    return (cellH > 0.f && atlasH > 0.f) ? (long)(atlasH / cellH + 0.01f) : 0;
}

// META-FIX design B PART 3a — checked variant. IDENTICAL cell math: on OOB
// it returns the LEGACY-COMPUTED cell UNCHANGED (legacy wrap is load-bearing
// for MC2X high-index content — substituting cell 0 would swap legit mod
// icons on any false positive) and sets *oob=true so the caller can emit
// one "[ASSET_LOAD v1] event=icon_oob" line. OOB = negative index, or a row
// at/beyond floor(atlasH/cellH) when both bounds are known. cellW/cellH come
// from WIDGET size at the call sites, so the row bound is harness-validated
// against retail 256x256, MC2X 512, and MCO 1024/40 geometries
// (tools/icon_atlas_harness/) before any smoke bucket counts icon_oob red.
inline Cell cellForIndexChecked(long index, float cellW, float cellH,
                                float atlasW, float atlasH, bool* oob) {
    Cell c = cellForIndex(index, cellW, cellH, atlasW);
    if (oob) {
        *oob = false;
        if (index < 0) {
            *oob = true;
        } else {
            const long rows = rowsForAtlas(atlasH, cellH);
            if (rows > 0 && c.row >= rows)
                *oob = true;
        }
    }
    return c;
}

} // namespace icon_atlas

#endif // MC2_ICON_ATLAS_CELL_H
