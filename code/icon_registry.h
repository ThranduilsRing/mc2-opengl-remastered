#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <format>
#include "../mclib/fst_hash.h"
#include "unit_display_name.h"

namespace unit_icons {

struct HudSource { const char* sheet; int width, height; };
// The drawable selects source detail; the logical UI canvas only controls layout.
inline constexpr HudSource hudIconSource(int drawableWidth, int screenWidth, bool vehicle = false) {
    switch (drawableWidth > 0 ? drawableWidth : screenWidth) {
        case 640: return {vehicle ? "data/art/mcui_low5.tga" : "data/art/mcui_low4.tga", 25, 30};
        case 800: return {vehicle ? "data/art/mcui_med5.tga" : "data/art/mcui_med4.tga", 32, 38};
        default: return {vehicle ? "data/art/mcui_high8.tga" : "data/art/mcui_high7.tga", 40, 48};
    }
}
inline constexpr uint32_t logisticsIconColor(bool hudArt, uint32_t color) {
    return hudArt ? color | 0x00ffffffu : color;
}

inline std::string canonical(std::string key) {
    fst_normalize_loose_key_inplace(key.data());
    return key;
}
inline std::string legacyKey(std::string sheet, long index) {
    return canonical(sheet) + ":" + std::to_string(index);
}

struct Icon {
    int width = 0, height = 0, x = 0, y = 0;
    std::vector<uint32_t> pixels; // BGRA, matching gos texture locks
    std::vector<uint32_t> samplingBorder; // authored neighbors for legacy filtering
};

// Draw an icon into a w x h BGRA rect. Shrinking averages every source pixel under the destination
// pixel (premultiplied alpha); nearest sampling skipped rows/columns and mangled the single-source art
// when it is drawn below its authored size. Enlarging stays nearest.
inline void resampleIcon(const Icon& icon, uint32_t* dest, int stride, int w, int h) {
    for (int y = 0; y < h; ++y) {
        const int y0 = y * icon.height / h, y1 = std::max(y0 + 1, ((y + 1) * icon.height + h - 1) / h);
        for (int x = 0; x < w; ++x) {
            const int x0 = x * icon.width / w, x1 = std::max(x0 + 1, ((x + 1) * icon.width + w - 1) / w);
            uint64_t sum[4] = {}; // premultiplied b, g, r, a
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint32_t p = icon.pixels[static_cast<size_t>(sy) * icon.width + sx], a = p >> 24;
                    for (int c = 0; c < 3; ++c) sum[c] += ((p >> (8 * c)) & 0xff) * a;
                    sum[3] += a;
                }
            const uint64_t n = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
            const uint64_t a = sum[3];
            uint32_t out = static_cast<uint32_t>((a + n / 2) / n) << 24;
            if (a) for (int c = 0; c < 3; ++c) out |= static_cast<uint32_t>((sum[c] + a / 2) / a) << (8 * c);
            dest[static_cast<size_t>(y) * stride + x] = out;
        }
    }
}

// Hand-pixeled art for one draw size (hud/front/<stem>@34x40) beats the single high-res source
// at that size; the box filter above only serves sizes with no authored art (mods, odd resolutions).
// A better (lower) VFS rank still wins, so a mod's single-source replacement is not shadowed.
struct HudPick { std::string key; int rank; };
inline HudPick pickHudSingle(std::string single, int singleRank, std::string exact, int exactRank) {
    if (exactRank <= singleRank) return {std::move(exact), exactRank};
    return {std::move(single), singleRank};
}
inline std::string hudSizedKey(std::string_view side, std::string_view stem, int w, int h) {
    return std::format("hud/{}/{}@{}x{}", side, stem, w, h);
}

// A named icon is one cell, but sheets reach the named path too (a two-row HUD strip, a row of views).
// A sheet has a transparent gutter straight through it between cells that recurs at one pitch tiling the
// image; one icon has no such gutter. `opaque(i, j)` reports pixel i along the scanned axis, j across.
// Returns the cell pitch along that axis, 0 for one cell.
template <class F> inline int cellPitch(int length, int cross, F opaque) {
    std::vector<bool> any(static_cast<size_t>(length), false);
    for (int i = 0; i < length; ++i)
        for (int j = 0; j < cross; ++j)
            if (opaque(i, j)) { any[static_cast<size_t>(i)] = true; break; }
    const auto first = std::find(any.begin(), any.end(), true);
    if (first == any.end()) return 0;
    const int lead = static_cast<int>(first - any.begin());
    int i = lead;
    while (i < length && any[static_cast<size_t>(i)]) ++i;
    if (i == length) return 0;
    while (i < length && !any[static_cast<size_t>(i)]) ++i;
    if (i == length) return 0;
    const int pitch = i - lead;
    return length % pitch == 0 ? pitch : 0;
}

// The first cell of a named icon (top-left), the whole image when it is one icon. Pixels are BGRA.
inline std::pair<int, int> firstCell(int w, int h, std::span<const uint32_t> pixels) {
    const int pw = cellPitch(w, h, [&](int x, int y) { return pixels[static_cast<size_t>(y) * w + x] >> 24; });
    const int ph = cellPitch(h, w, [&](int y, int x) { return pixels[static_cast<size_t>(y) * w + x] >> 24; });
    return {pw ? pw : w, ph ? ph : h};
}

inline std::vector<uint32_t> topLeft(int w, std::span<const uint32_t> pixels, int cw, int ch) {
    std::vector<uint32_t> out;
    out.reserve(static_cast<size_t>(cw) * ch);
    for (int y = 0; y < ch; ++y)
        out.insert(out.end(), pixels.begin() + static_cast<size_t>(y) * w, pixels.begin() + static_cast<size_t>(y) * w + cw);
    return out;
}

// An icon of any source size fits the cell it is drawn in: uniform scale, centered, never spilling.
inline std::array<int, 4> fitCell(int cw, int ch, int iw, int ih) {
    if (iw <= 0 || ih <= 0) return {0, 0, cw, ch};
    const float scale = std::min(static_cast<float>(cw) / iw, static_cast<float>(ch) / ih);
    const int w = std::max(1, static_cast<int>(iw * scale + .5f));
    const int h = std::max(1, static_cast<int>(ih * scale + .5f));
    return {(cw - w) / 2, (ch - h) / 2, w, h};
}

// Restores the cell an earlier fit left, then fits an icon of iw x ih into it. The undo is fitCell's own
// integer offset: a float half of an odd padding moved the widget half a pixel on every reassignment.
// aObject's contract: move() is relative, resize() keeps the top-left.
template <class Widget>
inline void fitWidget(Widget& widget, int cw, int ch, int iw, int ih) {
    const std::array<int, 4> undo = fitCell(cw, ch, static_cast<int>(widget.width()), static_cast<int>(widget.height()));
    widget.move(-static_cast<float>(undo[0]), -static_cast<float>(undo[1]));
    widget.resize(cw, ch);
    const std::array<int, 4> fit = fitCell(cw, ch, iw, ih);
    widget.resize(fit[2], fit[3]);
    widget.move(static_cast<float>(fit[0]), static_cast<float>(fit[1]));
}

// LookupModPriority ranks: a mod's load-order slot (the active mod 0, its dependencies 1..), else one of the
// install's own (loose 999998, kit 999999, stock 1000000). Only a mod's rank names a source of its own.
inline constexpr int kBaseRank = 999998;
inline bool modOwned(int rank) { return rank >= 0 && rank < kBaseRank; }
inline bool sameSource(int a, int b) { return modOwned(a) ? a == b : !modOwned(b); }

// A legacy sheet cell serves a chassis only when the cell is known to be that chassis: the same source
// declares the index and lays out the sheet, and the install's own chassis keep the stock layout under any
// drop-in sheet. A mod that brings its own chassis CSV has no cell in another source's sheet.
inline bool sheetServes(int sheetRank, int dataRank) { return !modOwned(dataRank) || sameSource(sheetRank, dataRank); }

// What the resolver knows about the unit it finds art for: the chassis CSV path (its stem is the first
// own-art name) and the name the author wrote in that file (the second; MCO ships "mechicons/Storm crow.tga").
struct Subject { std::string file; std::string name; };

// One chassis' art question, with everything apply() knows about where its files come from.
struct Query {
    std::string stem, name;        // the chassis CSV stem; the author's MechName
    std::string sheet;             // the template's sheet, holding the chassis' declared cell
    long index = 0;
    int dataRank = 0, sheetRank = 0;
    std::vector<std::string> hud;  // HUD front art keys for the stem, best first
};
struct Pick { const Icon* icon = nullptr; const Icon* hud = nullptr; bool own = false; };

// Source keys survive packing. Only this load-time cache owns atlas coordinates.
class Registry {
public:
    std::map<std::string, Icon> icons;
    int edge = 0;
    std::vector<uint32_t> atlas;

    bool add(std::string key, int w, int h, std::span<const uint32_t> pixels) {
        if (w <= 0 || h <= 0 || w > 16382 || h > 16382 || pixels.size() != static_cast<size_t>(w) * h)
            return false;
        icons[canonical(std::move(key))] = {w, h, 0, 0, {pixels.begin(), pixels.end()}};
        return true;
    }

    // The compatibility table is generated from the SOURCE grid, never the packed atlas.
    // Floor division deliberately preserves MCO's 1024/40 = 25 columns.
    void sheet(std::string key, std::span<const uint32_t> pixels,
               int w, int h, int cellW, int cellH, bool strip = false) {
        if (w <= 0 || h <= 0 || cellW <= 0 || cellH <= 0 || pixels.size() != static_cast<size_t>(w) * h) return;
        const int cols = w / cellW, rows = strip ? std::min(2, h / cellH) : h / cellH;
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                std::vector<uint32_t> tile(static_cast<size_t>(cellW) * cellH);
                for (int y = 0; y < cellH; ++y)
                    std::copy_n(pixels.data() + static_cast<size_t>(row * cellH + y) * w + col * cellW,
                                cellW, tile.data() + static_cast<size_t>(y) * cellW);
                const auto iconKey = legacyKey(strip && row ? std::format("{}/back", key) : key,
                                               strip ? col : row * cols + col);
                add(iconKey, cellW, cellH, tile);
                if (!strip) {
                    auto& border = icons.at(iconKey).samplingBorder;
                    border.resize(static_cast<size_t>(cellW + 2) * (cellH + 2));
                    for (int y = -1; y <= cellH; ++y)
                        for (int x = -1; x <= cellW; ++x)
                            border[static_cast<size_t>(y + 1) * (cellW + 2) + x + 1] =
                                pixels[static_cast<size_t>(std::clamp(row * cellH + y, 0, h - 1)) * w +
                                       std::clamp(col * cellW + x, 0, w - 1)];
                }
            }
        }
    }

    const Icon* find(std::string key) const {
        auto it = icons.find(canonical(key));
        if (it == icons.end()) return nullptr;
        const auto& [name, icon] = *it;
        return &icon;
    }

    // Order: the mod's own art (CSV stem, then the author's MechName) -> HUD front art -> a sheet cell the same
    // source declares -> none (the caller draws its placeholder). One cell; the caller fits it.
    Pick select(const std::map<std::string, int>& priority, const Query& q) const {
        const auto rank = [&](const std::string& key) {
            const auto it = priority.find(key);
            return it == priority.end() ? -1 : it->second;
        };
        const std::string stem = "mech/" + q.stem, byName = "mech/" + q.name;
        const auto here = [&](const std::string& key) { return rank(key) >= 0 && sameSource(rank(key), q.dataRank) && find(key); };
        // The author's MechName names art too, when the same source that owns the chassis owns that file. It wins
        // over a same-stem icon another source owns: that one is the stock chassis' art, not this mod's.
        const std::string named = here(byName) && !here(stem) ? byName : stem;
        const int namedRank = rank(named);
        const bool individual = namedRank >= 0 && namedRank <= q.sheetRank && find(named);
        Pick pick;
        int bestRank = individual ? namedRank : q.sheetRank;
        // Use the HUD front art in the existing logistics rectangle. Unmatched art keeps its old source.
        for (const auto& key : q.hud) {
            const int r = rank(key);
            if (r < 0 || r > bestRank || (pick.hud && r == bestRank)) continue;
            if (const Icon* icon = find(key)) { pick.hud = icon; bestRank = r; }
        }
        if (pick.hud) { pick.icon = pick.hud; return pick; }
        if (individual) { pick.icon = find(named); pick.own = true; }
        if (!pick.icon && sheetServes(q.sheetRank, q.dataRank)) pick.icon = find(legacyKey(q.sheet, q.index));
        return pick;
    }

    // Lower VFS rank wins: a mod atlas must beat a base game's split icon.
    const Icon* lookup(std::string named, std::string legacy, int namedRank, int sheetRank) const {
        if (namedRank <= sheetRank)
            if (const Icon* icon = find(std::move(named))) return icon;
        return find(std::move(legacy));
    }

    // Single-source HUD icon first; a mod's tiered file wins only with a better (lower) VFS rank.
    const Icon* lookupHud(std::string single, std::string tiered, int singleRank, int tieredRank,
                          std::string legacy, int sheetRank) const {
        return lookup(tieredRank < singleRank ? std::move(tiered) : std::move(single), std::move(legacy),
                      std::min(singleRank, tieredRank), sheetRank);
    }

    bool pack() {
        std::vector<Icon*> ordered;
        size_t area = 0;
        for (auto& [key, icon] : icons) {
            ordered.push_back(&icon);
            area += static_cast<size_t>(icon.width + 2) * (icon.height + 2);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const Icon* a, const Icon* b) {
            return a->height > b->height;
        });
        for (edge = 16; edge <= 16384; edge *= 2) {
            if (static_cast<size_t>(edge) * edge < area) continue;
            int x = 0, y = 0, rowH = 0;
            bool fits = true;
            for (Icon* icon : ordered) {
                if (x + icon->width + 2 > edge) { x = 0; y += rowH; rowH = 0; }
                if (icon->width + 2 > edge || y + icon->height + 2 > edge) { fits = false; break; }
                icon->x = x + 1; icon->y = y + 1;
                x += icon->width + 2;
                rowH = std::max(rowH, icon->height + 2);
            }
            if (!fits) continue;
            atlas.assign(static_cast<size_t>(edge) * edge, 0);
            for (const Icon* icon : ordered) {
                // Preserve legacy sheet filtering; loose images extrude their own edge.
                for (int dy = -1; dy <= icon->height; ++dy)
                    for (int dx = -1; dx <= icon->width; ++dx)
                        atlas[static_cast<size_t>(icon->y + dy) * edge + icon->x + dx] =
                            !icon->samplingBorder.empty() ?
                            icon->samplingBorder[static_cast<size_t>(dy + 1) * (icon->width + 2) + dx + 1] :
                            icon->pixels[static_cast<size_t>(std::clamp(dy, 0, icon->height - 1)) * icon->width +
                                         std::clamp(dx, 0, icon->width - 1)];
            }
            return true;
        }
        atlas.clear(); edge = 0;
        return false;
    }
};

} // namespace unit_icons

class aObject;
namespace unit_icons {
bool apply(aObject& widget, const aObject& source, long index, const Subject& subject);
void blit(std::string sheet, long index, bool back, uint32_t* dest, int stride, int width, int height,
          std::string chassis = {});
// Single-file pilot portrait (data/art/pilots/NN.png). Null when absent or when a mod's replacement
// of `legacySheet` (the old portrait grid) outranks it; the caller then uses the grid.
const Icon* pilotIcon(long index, std::string_view legacySheet);
bool pilotUV(long index, std::string_view legacySheet, unsigned long& sourceId, float& u0, float& v0, float& u1, float& v1);
bool applyPilot(aObject& widget, long index);
const Icon& damageRect(long slot);
unsigned long damageTexture(long slot);
void shutdown();
}
