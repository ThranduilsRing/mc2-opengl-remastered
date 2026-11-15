// UI-CHROME-SPLIT-1: per-widget UI chrome sources and the load-time re-pack of the legacy sheets.
//
// data/art/ui/manifest.json (written by tools/ui_art_split.py) lists, per legacy sheet (MCUI_Universal1, MCL_MB_4, ...),
// each widget PNG and the rect it occupies in that sheet. composeSheet() pastes the widgets back at those rects, so a
// FIT that crops (u, v, w, h) out of the sheet samples exactly the pixels the authored sheet had. Each
// widget file also keeps `pad` texels of the authored neighbours around its rect, so a bilinear tap at the rect edge reads
// real texels, not the key. Pure logic: no engine
// types, no file access (the caller supplies the widget pixels).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::ui_chrome {

// Inner right-hand bay in the menu's authored 800x600 frame.
inline std::array<float, 4> menuCreditsBay(float frameWidth, float frameHeight) {
    return {604 * frameWidth / 800, 292 * frameHeight / 600,
            168 * frameWidth / 800, 248 * frameHeight / 600};
}

struct Widget {
    std::string name;       // file stem under data/art/ui/<sheet>/
    int x = 0, y = 0, w = 0, h = 0;   // rect in the sheet, top-left origin
    bool hasNine = false;
    std::array<int, 4> nine{};        // left, top, right, bottom insets in texels
};

struct Sheet {
    std::string key;        // lower-case sheet stem, e.g. "mcui_universal1"
    int w = 0, h = 0;
    bool rgba = false;      // false: RGB sheet (uncovered texels become the FF00FF key)
    int pad = 0;            // each widget file carries this many authored neighbour texels around its rect (bilinear taps at the rect edge)
    std::vector<Widget> widgets;
};

// The widget file's extent: the rect grown by sheet.pad texels per side, clamped to the sheet.
struct PaddedRect {
    int x0, y0, x1, y1;   // [x0, x1) x [y0, y1) in sheet texels
};
inline PaddedRect paddedRect(const Sheet& s, const Widget& w) {
    const auto lo = [&](int v) { return v > s.pad ? v - s.pad : 0; };
    const auto hi = [&](int v, int limit) { return v + s.pad < limit ? v + s.pad : limit; };
    return {lo(w.x), lo(w.y), hi(w.x + w.w, s.w), hi(w.y + w.h, s.h)};
}

struct Manifest {
    std::map<std::string, Sheet> sheets;
    std::optional<std::reference_wrapper<const Sheet>> find(std::string_view key) const;
};

// False on malformed JSON, a wrong version, a widget outside its sheet, or a duplicate widget name.
bool parseManifest(std::string_view json, Manifest& out, std::string* err = nullptr);
std::string manifestToJson(const Manifest& m);

struct Pixels {
    int w = 0, h = 0, channels = 0;   // channels: 3 (RGB8) or 4 (RGBA8)
    std::vector<uint8_t> data;
};

// Returns false (and leaves `px` empty) when the widget file is absent or undecodable.
using WidgetLoader = std::function<bool(const Sheet& sheet, const Widget& widget, Pixels& px)>;
// Stock menu paint spans amber through green in the source art. Share the
// outer-chrome yellow (48 degrees HSV), retaining saturation, value and alpha.
inline uint32_t menuYellow(uint32_t argb) {
    const int r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    const int hi = r > g ? r : g, lo = r < g ? r : g;
    const int chroma = hi - b;
    if (b >= lo || chroma * 100 < hi * 35 ||
        (g - b) * 100 < (r - b) * 10 || (r - b) * 100 < (g - b) * 67) return argb;
    return (argb & 0xff000000u) | (static_cast<uint32_t>(hi) << 16) |
           (static_cast<uint32_t>(b + (chroma * 4 + 2) / 5) << 8) | static_cast<uint32_t>(b);
}

// Animated tint supplies the button colour; warm neutral carrier texels would
// multiply a second hue into it. Keep their authored shading as luminance.
inline uint32_t menuTintCarrier(uint32_t argb) {
    const int r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    if (r < g || g < b || (r - b) * 2 > r) return argb;
    return (argb & 0xff000000u) | (static_cast<uint32_t>(r) * 0x010101u);
}


// Remove the baked matte only from stock main-menu/dialog chrome, preserving button wells.
bool clearMenuMatte(std::string_view path, Pixels& px);

// Pastes every widget at its rect. All-or-nothing: one missing or wrong-sized widget fails the whole sheet.
bool composeSheet(const Sheet& sheet, const WidgetLoader& load, Pixels& out, std::string* err = nullptr);

// An uncompressed (stored-block) 8-bit PNG: the engine's PNG decoder reads it like any other.
std::vector<uint8_t> encodePngStored(const Pixels& px);

// "data/art/gui/MCUI_Universal1.tga" (any case, any slash, any extension) -> "mcui_universal1"; empty when the path is
// not a data/art/gui sheet.
std::string sheetKeyFromPath(std::string_view path);

// Only chrome sub-crops need neighbour rejection. Content sprites retain their authored UVs.
inline bool insetChromeCrop(std::string_view path, bool pixelPerfect, bool legacySpace, bool nineSlice,
                            float u1, float v1, float u2, float v2) {
    if (pixelPerfect || legacySpace || (u1 == 0.f && v1 == 0.f && u2 == 1.f && v2 == 1.f)) return false;
    const auto key = sheetKeyFromPath(path);
    if (key.starts_with("mcl_mm_")) return false; // continuous menu frame strips share authored edge samples
    return nineSlice || key.starts_with("mcl_") || key.starts_with("mcui_universal") || key.starts_with("mcui_mr_");
}

// Nine-slice geometry: the source image (srcW x srcH texels) is cut by `insets` (l, t, r, b); corners keep their texel
// size, edges stretch along one axis, the centre along both. Quad order: row-major from the top-left. Returns false when
// the insets leave no centre or the destination is smaller than the (scaled) corners; cornerScale* is the page scale (callers then draw one stretched quad).
struct Quad {
    float dx0, dy0, dx1, dy1;   // destination rect
    float sx0, sy0, sx1, sy1;   // source rect in texels
};
bool nineSliceQuads(int srcW, int srcH, const std::array<int, 4>& insets, float dx, float dy, float dw, float dh,
                    std::array<Quad, 9>& out, float cornerScaleX = 1.f, float cornerScaleY = 1.f);

}  // namespace mc2::ui_chrome
