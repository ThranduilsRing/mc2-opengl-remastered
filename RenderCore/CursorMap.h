// CursorMap.h - pure mapping of the software cursor quad from the game's logical 2D space
// (the HUD batch space: 800x600 on menus, window size in a mission) to ImGui display space.
// Mirrors gosRenderer::flushHUDBatch: logical -> UI canvas box (menus only; full drawable
// otherwise) -> drawable, then drawable -> ImGui DisplaySize.
#pragma once

namespace mc2 {

struct CursorMap {
    float ox, oy;   // display-space position of logical (0,0)
    float sx, sy;   // display px per logical px (applies to positions and sprite sizes)
    float x(float lx) const { return ox + lx * sx; }
    float y(float ly) const { return oy + ly * sy; }
};

// canvas box (bx,by,bw,bh) is in drawable px; pass the full drawable when no canvas is active.
inline CursorMap makeCursorMap(float logicalW, float logicalH,
                               float bx, float by, float bw, float bh,
                               float drawW, float drawH, float dispW, float dispH)
{
    CursorMap m{ 0.0f, 0.0f, 1.0f, 1.0f };
    if (logicalW <= 0.0f || logicalH <= 0.0f || drawW <= 0.0f || drawH <= 0.0f)
        return m;
    const float kx = dispW / drawW, ky = dispH / drawH;
    m.sx = (bw / logicalW) * kx;
    m.sy = (bh / logicalH) * ky;
    m.ox = bx * kx;
    m.oy = by * ky;
    return m;
}

} // namespace mc2
