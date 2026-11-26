#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace mc2::loaddetail {

struct Info {
    std::string_view stage;                      // display label of the load phase; empty = none
    std::string_view preset = "Custom";
    bool hasProgress = false;                    // the coarse 0..100 load percent is known
    float progress = 0.f;
    std::uint64_t ramUsed = 0, ramTotal = 0;     // system RAM, bytes; a zero total = unavailable
    std::int64_t vramUsed = -1, vramBudget = -1; // bytes; negative = unavailable
    int width = 0, height = 0;                   // render resolution
    int quality = 2, shadows = 2, textures = 3, antiAliasing = 1; // option levels
};

inline constexpr const char* kQuality[] = {"Potato", "Low", "High"};
inline constexpr const char* kShadows[] = {"Potato", "Medium", "High"};
inline constexpr const char* kTextures[] = {"Low", "Medium", "High", "Ultra"};
inline constexpr const char* kAntiAliasing[] = {"Off", "SMAA 1x", "FXAA", "CMAA2", "FSR 3.1"};

template <std::size_t N>
inline std::string pick(int level, const char* const (&names)[N]) {
    return names[level >= 0 && level < static_cast<int>(N) ? level : 0];
}

inline std::string gib(std::int64_t bytes) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / 1073741824.0);
    return text;
}

// Five lines: stage, RAM, VRAM, resolution/preset and graphics options.
inline std::string format(const Info& i) {
    std::string out = i.stage.empty() ? "Mission" : std::string(i.stage);
    if (i.hasProgress) {
        const float clamped = i.progress < 0.f ? 0.f : i.progress > 100.f ? 100.f : i.progress;
        char percent[16];
        std::snprintf(percent, sizeof(percent), " %d%%", static_cast<int>(clamped));
        out += percent;
    }
    out += "\nRAM   ";
    if (i.ramTotal) out += gib(static_cast<std::int64_t>(i.ramUsed)) + " / " + gib(static_cast<std::int64_t>(i.ramTotal)) + " GB";
    else out += "n/a";
    out += "\nVRAM  ";
    if (i.vramUsed >= 0 && i.vramBudget > 0) out += gib(i.vramUsed) + " / " + gib(i.vramBudget) + " GB";
    else out += "n/a";
    out += "\n";
    if (i.width > 0 && i.height > 0) {
        char resolution[32];
        std::snprintf(resolution, sizeof(resolution), "%dx%d  ", i.width, i.height);
        out += resolution;
    }
    out += "Preset " + std::string(i.preset) + "\nQuality " + pick(i.quality, kQuality) + "  Shadows " + pick(i.shadows, kShadows) +
           "  Textures " + pick(i.textures, kTextures) + "  AA " + pick(i.antiAliasing, kAntiAliasing);
    return out;
}
} // namespace mc2::loaddetail
