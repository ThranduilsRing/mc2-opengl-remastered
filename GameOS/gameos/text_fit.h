#pragma once
// Pure fit math for gosRenderer::drawText: a wrapped bitmap-font block that is taller than its bounded region first
// tightens its line pitch (floor 85% of the font height), then drops the lines that still do not fit (the caller
// ellipsizes the last visible one). Header-only and engine-free so a doctest pins it (tests/unit/test_text_fit.cpp).
#include <algorithm>

namespace mc2::textfit {

struct Fit {
    int pitch;          // pixels between line origins
    int visibleLines;   // lines to draw (>= 1 when numLines >= 1)
    bool ellipsize;     // true: the last visible line is clipped with "..." because lines were dropped
};

inline constexpr int kMinPitchPercent = 85;

// Broken lines can still contain a single word wider than the region.
inline float fitWidth(int regionWidth, int widestLine)
{
    return std::clamp(static_cast<float>(regionWidth) / std::max(1, widestLine), 0.f, 1.f);
}

// regionHeight < fontHeight (no usable bounded region), a single line, and any block that already fits: unchanged.
inline Fit fitLines(int numLines, int fontHeight, int regionHeight)
{
    Fit f{fontHeight, numLines, false};
    if (numLines <= 1 || fontHeight <= 0 || regionHeight < fontHeight) return f;
    if (numLines * fontHeight <= regionHeight) return f;
    const int floorPitch = std::max(1, fontHeight * kMinPitchPercent / 100);
    const int fitPitch = (regionHeight - fontHeight) / (numLines - 1);
    if (fitPitch >= floorPitch) { f.pitch = fitPitch; return f; }
    f.pitch = floorPitch;
    // the last line must still fit: (visible - 1) * pitch + fontHeight <= regionHeight
    f.visibleLines = std::clamp((regionHeight - fontHeight) / floorPitch + 1, 1, numLines);   // numLines > 1 here
    f.ellipsize = f.visibleLines < numLines;
    return f;
}

}  // namespace mc2::textfit
