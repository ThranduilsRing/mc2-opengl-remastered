// mclib/fast_hash.h -- the 8-byte-word content hash shared by the cook cache and the VFS content digest.
// Chunk splits at multiples of 8 bytes hash identically to one call, so streamed and in-memory digests agree.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mc2::content {
inline uint64_t fastHash(std::span<const uint8_t> bytes, uint64_t h = 14695981039346656037ull) {
    const uint8_t* p = bytes.data();
    size_t n = bytes.size();
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; n; ++p, --n) { h ^= *p; h *= 1099511628211ull; }
    return h;
}
}
