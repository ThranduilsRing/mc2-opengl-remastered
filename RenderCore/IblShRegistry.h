// RenderCore/IblShRegistry.h
//
// V-IBL-STATIC-2: per-mission SH coefficient set selection.
//
// Maps a mission identifier (case-insensitive name) to a named SH-L2
// coefficient set. The default set ("default") points at the generator-
// owned kIblShCoeffs in IblShCoeffs.h, so all missions that lack an
// explicit registry entry fall back to the V-IBL-STATIC-1 baseline
// (byte-identical to that slice's gate-ON behavior).
//
// Schema:
//   struct IblShSet { name; coeffs[9][3]; }
//   kIblShSets[]   -- compile-time table; index 0 is "default".
//   kMissionShMap[]-- mission name -> set-name mapping (currently empty).
//   lookupShSet(const char* missionName)
//     -- case-insensitive lookup; returns kIblShSets[0] ("default") for
//        null/empty/unknown mission names. Never returns nullptr.
//
// Firewall: header-only constexpr. No GL, no game-side includes.
// Sister header IblShCoeffs.h remains generator-owned (tools/ibl/project_sh.py)
// and is included here so coefficient regeneration does not touch this file.

#pragma once

#include <cstddef>
#include "IblShCoeffs.h"

namespace RenderCore {

struct IblShSet {
    const char* name;          // human-readable, e.g. "default", "DaySkyHDRI063B"
    const float (*coeffs)[3];  // pointer to 9-vec3 array; non-owning
};

// Index 0 is "default" and MUST exist. Future entries: append; never
// reorder (lookup is by name, not index, but a stable default at [0] is
// the universal fallback).
constexpr IblShSet kIblShSets[] = {
    { "default", kIblShCoeffs },
};
constexpr std::size_t kIblShSetCount = sizeof(kIblShSets) / sizeof(kIblShSets[0]);

// Mission -> SH-set name. Empty for V-IBL-STATIC-2; every mission falls
// back to "default" via lookupShSet(). Future per-mission tuning slices
// should append real entries here. Lookup iterates by kMissionShMapCount
// and ignores null placeholders, so there is no terminator-order trap.
struct IblShMissionEntry {
    const char* missionName;   // case-insensitive match key
    const char* shSetName;     // must exist in kIblShSets[]
};
constexpr IblShMissionEntry kMissionShMap[] = {
    // Null placeholder keeps this constexpr table non-empty until the first
    // real mission mapping lands. Add future mappings before or after it.
    { nullptr, nullptr },
};
constexpr std::size_t kMissionShMapCount =
    sizeof(kMissionShMap) / sizeof(kMissionShMap[0]);

namespace detail {

constexpr char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool nameEqualsCI(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    while (*a && *b) {
        if (asciiLower(*a) != asciiLower(*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

} // namespace detail

// Find an SH set by name. Returns nullptr if not found.
inline const IblShSet* findShSetByName(const char* setName) {
    if (setName == nullptr || setName[0] == '\0') return nullptr;
    for (const auto & iblShSet : kIblShSets) {
        if (detail::nameEqualsCI(iblShSet.name, setName)) {
            return &iblShSet;
        }
    }
    return nullptr;
}

// Mission -> SH set. Never returns nullptr; null/empty/unknown names
// resolve to kIblShSets[0] ("default"). If a registry entry names a set
// that does not exist (typo), also falls back to default.
inline const IblShSet& lookupShSet(const char* missionName) {
    if (missionName != nullptr && missionName[0] != '\0') {
        for (std::size_t i = 0; i < kMissionShMapCount; ++i) {
            const IblShMissionEntry& e = kMissionShMap[i];
            if (detail::nameEqualsCI(e.missionName, missionName)) {
                if (const IblShSet* s = findShSetByName(e.shSetName)) {
                    return *s;
                }
                break;  // entry points at unknown set -> default
            }
        }
    }
    return kIblShSets[0];  // "default"
}

} // namespace RenderCore
