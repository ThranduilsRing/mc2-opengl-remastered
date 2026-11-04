// Save v2 FIX1 section (B3): the sim state the legacy packets and the other sections never carried.
//   music    - what an ABL script sees from getCurrentMusicId (ScriptMusic), as time remaining on the scenario clock
//   backoffs - per-warrior failed-jump backoff (goal cell and age in path-manager frames)
//   paths    - pending path requests in queue order (warrior index, source, selectionIndex, moveParams)
// Pure value codec (no engine types); capture/apply live in saveload.cpp / saveload_load.cpp. Optional section: an older
// build drops it and behaves as before.
#pragma once

#include "save_archive.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace mc2::save {

inline constexpr std::uint32_t fixSectionVersion = 1;
inline constexpr std::uint32_t fixMaxEntries = 4096;
inline constexpr std::int32_t fixMaxPathSource = 50;  // MovePathManager::sourceTally size

struct FixMusic {
    std::int32_t id = -1;
    double remaining = 0.0;  // seconds left on the scenario clock; 0 for a looping or stopped tune
    bool loops = false;
    bool operator==(const FixMusic&) const = default;
};

struct FixBackoff {
    std::uint32_t warrior = 0;
    std::int32_t goalR = -1, goalC = -1;
    std::int32_t age = 0;  // path-manager frames since the failed solve
    bool operator==(const FixBackoff&) const = default;
};

struct FixPathRequest {
    std::uint32_t warrior = 0;
    std::int32_t source = 0, selectionIndex = 0;
    std::uint32_t moveParams = 0;
    bool operator==(const FixPathRequest&) const = default;
};

struct FixSection {
    FixMusic music;
    std::vector<FixBackoff> backoffs;
    std::vector<FixPathRequest> paths;

    void serialize(Archive& a) {
        a(music.id, music.remaining, music.loops);
        a.sequence(backoffs, fixMaxEntries, [](Archive& s, FixBackoff& b) { s(b.warrior, b.goalR, b.goalC, b.age); });
        a.sequence(paths, fixMaxEntries, [](Archive& s, FixPathRequest& p) { s(p.warrior, p.source, p.selectionIndex, p.moveParams); });
    }

    // Range checks a reader applies before touching live state.
    // musicCount is the music table size; the script music id indexes nothing, but a stale or hostile id is refused.
    bool valid(std::uint32_t warriorCount, std::int32_t musicCount = std::numeric_limits<std::int32_t>::max()) const {
        if (music.id < -1 || music.id >= musicCount || !std::isfinite(music.remaining) || music.remaining < 0.0) return false;
        for (const auto& b : backoffs)
            if (b.warrior >= warriorCount || b.age < 0) return false;
        for (const auto& p : paths)
            if (p.warrior >= warriorCount || p.source < 0 || p.source >= fixMaxPathSource) return false;
        return true;
    }
    bool operator==(const FixSection&) const = default;
};

inline Bytes encodeFixSection(FixSection value) {
    Archive a;
    value.serialize(a);
    return a.good() ? a.bytes() : Bytes{};
}

inline std::optional<FixSection> decodeFixSection(std::span<const std::uint8_t> bytes) {
    Archive a(bytes);
    FixSection staged;
    staged.serialize(a);
    if (!a.finish()) return std::nullopt;
    return staged;
}

// Version check, decode and range check in one step; nullopt means the loader logs one reject and keeps the defaults.
inline std::optional<FixSection> decodeValidFixSection(std::uint32_t version, std::span<const std::uint8_t> bytes,
                                                       std::uint32_t warriorCount, std::int32_t musicCount) {
    if (version != fixSectionVersion) return std::nullopt;
    auto decoded = decodeFixSection(bytes);
    if (!decoded || !decoded->valid(warriorCount, musicCount)) return std::nullopt;
    return decoded;
}

}  // namespace mc2::save
