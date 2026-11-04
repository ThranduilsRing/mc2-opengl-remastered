// gameplay_height.sig hash (TERRAIN-BEAUTY-SIG-1): FNV-1a64 over a u32 side then side*side float32 elevations,
// formatted "<side> <hex16>". Must match tools/terrain_beautify/height_sig.py (tests/unit/test_height_sig.cpp
// carries the Python test vector).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace mc2terrain {

inline std::string gameplayHeightSig(int side, const float* elev, size_t count)
{
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](const unsigned char* p, size_t n) {
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    };
    const uint32_t sideU = static_cast<uint32_t>(side);
    unsigned char sb[sizeof(sideU)];
    std::memcpy(sb, &sideU, sizeof(sideU));   // little endian, as the Python side writes it
    mix(sb, sizeof(sb));
    mix(reinterpret_cast<const unsigned char*>(elev), count * sizeof(float));
    return std::format("{} {:016x}", side, h);
}

}  // namespace mc2terrain
