#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <format>
#include "../mclib/fst_hash.h"

namespace unit_icons {

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
void apply(aObject& widget, const aObject& source, long index, std::string chassis);
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
