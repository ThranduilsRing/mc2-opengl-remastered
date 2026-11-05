// RenderCore/IblHdriRegistry.h
//
// Maps theSkyNumber (1-21) to an HDRI equirectangular sky background asset.
// Generated from sky TGA colour analysis (2026-06-17) — see .claude/analyze_sky_tga.py.
//
// Each sky was measured across sky{N:02d}_front_right.tga + sky{N:02d}_back_left.tga
// (circular-mean hue, mean saturation/value, dark-pixel fraction).  The result was
// classified into one of six mood categories whose asset table is in kIblHdriSets[].
//
// Call site:
//   #include "RenderCore/IblHdriRegistry.h"
//   const RenderCore::IblHdriSet& s = RenderCore::lookupHdriForSkyNumber(skyNumber);
//   // s.exrPath is the relative path; append ".ktx2" for the GPU-resident sidecar.
//
// Firewall: header-only, constexpr, no GL, no game-side headers.
// Sister header IblShRegistry.h — same structure, keyed on mission name instead.
//
// Update discipline:
//   Re-run .claude/analyze_sky_tga.py after adding new sky TGA pairs, then
//   update kSkyNumberHdriMap[] below.  Do NOT change the order of kIblHdriSets[]
//   (lookups are by name, but index 0 must always be "default").

#pragma once
#include <cstddef>
#include <cstring>

namespace RenderCore {

// ---------------------------------------------------------------------------
// Asset table — one entry per distinct HDRI file
// ---------------------------------------------------------------------------

struct IblHdriSet {
    const char* name;    // mood label, e.g. "day_clear"
    const char* exrPath; // relative path to .exr; loader derives .ktx2 sidecar
};

// Index 0 MUST be "default" — it is the universal fallback returned when a
// skyNumber has no explicit mapping or when its named set cannot be resolved.
constexpr IblHdriSet kIblHdriSets[] = {
    { "default",      "data/hdr/DaySkyHDRI063B_4K.exr"             }, // [0] 4K fallback
    { "day_clear",    "data/hdr/pizzo_pernice_puresky_16k.exr"      }, // [1] 16K mountain clear
    { "day_warm",     "data/hdr/citrus_orchard_puresky_16k.exr"     }, // [2] warm day
    { "sunset",       "data/hdr/belfast_sunset_puresky_16k.exr"     }, // [3] sunset/dusk
    { "overcast",     "data/hdr/mud_road_puresky_16k.exr"           }, // [4] grey overcast
    { "night",        "data/hdr/qwantani_night_puresky_16k.exr"     }, // [5] original fallback night sky
    { "dramatic",     "data/hdr/pizzo_pernice_puresky_16k.exr"      }, // [6] reuse clear; dim via exposure
    { "night_moon",   "data/hdr/NightSkyHDRI007_16k.exr"            }, // [7] 16K night sky with moon (peak_lum=1.8)
    { "night_stars",  "data/hdr/NightSkyHDRI008_16k.exr"            }, // [8] 16K star field, no moon (p99.9 bright stars)
    // TODO: reassign sky numbers 5, 8, 20, 21 between night_stars/night_moon as preferred
};
constexpr size_t kIblHdriSetCount = sizeof(kIblHdriSets) / sizeof(kIblHdriSets[0]);

// ---------------------------------------------------------------------------
// Sky-number -> mood mapping
// ---------------------------------------------------------------------------

struct IblHdriSkyEntry {
    int         skyNumber;   // theSkyNumber from .fit [TheSky] SkyNumber = N (1-21)
    const char* hdriSetName; // key into kIblHdriSets[].name
};

// Measurements: py .claude/analyze_sky_tga.py  (front_right + back_left, circular-mean H)
// sky# | mean_H | mean_S | mean_V | dark_frac | band
//   1  |  330.4 | 0.0644 | 0.7331 | 0.0000 | red/orange  (very low S, near-white)
//   2  |    6.7 | 0.2388 | 0.7671 | 0.0000 | red/orange  (warm orange-red, medium S)
//   3  |  284.3 | 0.4305 | 0.3030 | 0.0157 | purple      (high S, dim)
//   4  |   58.7 | 0.1155 | 0.8735 | 0.0000 | yellow      (low S, very bright)
//   5  |  359.4 | 0.2598 | 0.0426 | 0.9220 | red/orange  (92% dark pixels = night)
//   6  |  238.7 | 0.1795 | 0.8164 | 0.0000 | blue        (medium S, bright blue)
//   7  |  291.0 | 0.1032 | 0.7533 | 0.0000 | purple      (very low S)
//   8  |  331.5 | 0.1841 | 0.0556 | 0.8594 | red/orange  (86% dark pixels = night)
//   9  |   33.3 | 0.5158 | 0.6462 | 0.0000 | orange      (high S = fiery sunset)
//  10  |  251.1 | 0.0795 | 0.8709 | 0.0000 | blue        (very low S, bright)
//  11  |  230.2 | 0.0171 | 0.7131 | 0.0000 | blue        (near-zero S = grey)
//  12  |   52.9 | 0.2162 | 0.8505 | 0.0000 | yellow      (medium S, warm bright)
//  13  |   42.8 | 0.2973 | 0.8149 | 0.0000 | orange/yel  (warm orange, bright)
//  14  |   66.4 | 0.0510 | 0.7585 | 0.0000 | green       (near-zero S, white-grey)
//  15  |  207.6 | 0.0949 | 0.9813 | 0.0000 | cyan        (very bright, low S)
//  16  |  229.9 | 0.2099 | 0.8997 | 0.0000 | blue        (clear blue, bright)
//  17  |  234.1 | 0.0729 | 0.3756 | 0.0029 | blue        (dim blue, stormy)
//  18  |   21.8 | 0.4143 | 0.3003 | 0.0414 | red/orange  (orange-red, medium S, dim)
//  19  |  207.2 | 0.1188 | 0.9842 | 0.0000 | cyan        (very bright, low S)
//  20  |  334.1 | 0.1313 | 0.0200 | 0.9691 | red/orange  (97% dark pixels = night)
//  21  |  299.1 | 0.2875 | 0.1161 | 0.6853 | purple      (69% dark = night)

constexpr IblHdriSkyEntry kSkyNumberHdriMap[] = {
    {  1, "day_clear" }, // H=330° S=0.06 V=0.73 — near-white, bright; beach/coastal day (user override)
    {  2, "sunset"    }, // H=7°   S=0.24 V=0.77 — warm orange-red, medium sat; classic sunset palette
    {  3, "dramatic"  }, // H=284° S=0.43 V=0.30 — purple, high saturation, dim; used in dark dramatic missions
    {  4, "overcast"  }, // H=59°  S=0.12 V=0.87 — yellow-tinted bright grey; featureless overcast day
    {  5, "night_moon" }, // H=359° S=0.26 V=0.04 dark_frac=0.92 — 92% near-black, red tinge; moonlit night
    {  6, "day_clear"  }, // H=239° S=0.18 V=0.82 — blue, medium sat, bright; clear daytime
    {  7, "overcast"   }, // H=291° S=0.10 V=0.75 — purple cast but very low sat; overcast grey
    {  8, "night_stars"}, // H=332° S=0.18 V=0.06 dark_frac=0.86 — 86% near-black; dark night with stars
    {  9, "sunset"    }, // H=33°  S=0.52 V=0.65 — fiery orange, high saturation; dramatic sunset
    { 10, "day_clear" }, // H=251° S=0.08 V=0.87 — blue-white, low sat, very bright; hazy clear sky
    { 11, "overcast"  }, // H=230° S=0.02 V=0.71 — near-zero saturation blue-grey; heavy overcast
    { 12, "day_warm"  }, // H=53°  S=0.22 V=0.85 — warm yellow, medium sat, bright; warm sunny day
    { 13, "day_warm"  }, // H=43°  S=0.30 V=0.81 — orange-warm, medium-high sat, bright; warm afternoon
    { 14, "overcast"  }, // H=66°  S=0.05 V=0.76 — near-zero saturation white; flat overcast
    { 15, "day_clear" }, // H=208° S=0.09 V=0.98 — near-white cyan, very bright; high-noon clear
    { 16, "day_clear" }, // H=230° S=0.21 V=0.90 — saturated blue, bright; classic clear-day sky
    { 17, "dramatic"  }, // H=234° S=0.07 V=0.38 — dim desaturated blue; stormy/dusk atmosphere
    { 18, "sunset"    }, // H=22°  S=0.41 V=0.30 — dark orange-red, medium-high sat; deep sunset
    { 19, "day_clear" }, // H=207° S=0.12 V=0.98 — near-white bright cyan; pristine clear sky
    { 20, "night_stars"}, // H=334° S=0.13 V=0.02 dark_frac=0.97 — 97% near-black; deep starfield night
    { 21, "night_moon" }, // H=299° S=0.29 V=0.12 dark_frac=0.69 — 69% dark, purple aurora tinge; moonlit
};
constexpr size_t kSkyNumberHdriMapCount =
    sizeof(kSkyNumberHdriMap) / sizeof(kSkyNumberHdriMap[0]);

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

namespace detail {

constexpr char hdriAsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool hdriNameEq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (hdriAsciiLower(*a) != hdriAsciiLower(*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

} // namespace detail

// Find an HDRI set by mood name (case-insensitive). Returns nullptr if not found.
inline const IblHdriSet* findHdriSetByName(const char* name) {
    if (!name || name[0] == '\0') return nullptr;
    for (const auto & kIblHdriSet : kIblHdriSets)
        if (detail::hdriNameEq(kIblHdriSet.name, name)) return &kIblHdriSet;
    return nullptr;
}

// Primary lookup: theSkyNumber (1-21) -> HDRI set.
// Returns kIblHdriSets[0] ("default") for unmapped or out-of-range sky numbers,
// and also when a mapping entry names a set that does not exist in kIblHdriSets[].
inline const IblHdriSet& lookupHdriForSkyNumber(int skyNumber) {
    for (size_t i = 0; i < kSkyNumberHdriMapCount; ++i) {
        if (kSkyNumberHdriMap[i].skyNumber == skyNumber) {
            if (const IblHdriSet* s = findHdriSetByName(kSkyNumberHdriMap[i].hdriSetName))
                return *s;
            break; // entry found but set name unresolvable -> fall through to default
        }
    }
    return kIblHdriSets[0]; // "default"
}

} // namespace RenderCore
