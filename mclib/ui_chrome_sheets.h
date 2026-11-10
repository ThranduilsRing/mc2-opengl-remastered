// UI-CHROME-SPLIT-1: per-widget UI chrome sources and the load-time re-pack of the legacy sheets.
//
// data/art/ui/manifest.json (written by tools/ui_art_split.py) lists, per legacy sheet (MCUI_Universal1, MCL_MB_4, ...),
// each widget PNG and the rect it occupies in that sheet. composeSheet() pastes the widgets back at those rects, so a
// FIT that crops (u, v, w, h) out of the sheet samples exactly the pixels the authored sheet had. Pure logic: no engine
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
    std::vector<Widget> widgets;
};

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

// Pastes every widget at its rect. All-or-nothing: one missing or wrong-sized widget fails the whole sheet.
bool composeSheet(const Sheet& sheet, const WidgetLoader& load, Pixels& out, std::string* err = nullptr);

// An uncompressed (stored-block) 8-bit PNG: the engine's PNG decoder reads it like any other.
std::vector<uint8_t> encodePngStored(const Pixels& px);

// "data/art/gui/MCUI_Universal1.tga" (any case, any slash, any extension) -> "mcui_universal1"; empty when the path is
// not a data/art/gui sheet.
std::string sheetKeyFromPath(std::string_view path);

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
