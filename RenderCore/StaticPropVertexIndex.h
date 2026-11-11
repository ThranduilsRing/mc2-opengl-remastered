#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace mc2_prop {
// One packed static-prop vertex (GpuStaticPropBatcher kVertexStride bytes) as a map key. A fixed array, not a
// std::string: the string key heap-allocated for every emitted corner, which dominated registration time.
inline constexpr std::size_t kPackedVertexBytes = 40;
using PackedVertexKey = std::array<uint8_t, kPackedVertexBytes>;

struct PackedVertexKeyHash {
    std::size_t operator()(const PackedVertexKey& key) const noexcept {
        static_assert(kPackedVertexBytes % 8 == 0);
        uint64_t h = 14695981039346656037ull;   // FNV-1a style over 8-byte words (the key is a whole number of words)
        for (std::size_t i = 0; i < kPackedVertexBytes; i += 8) {
            uint64_t w;
            std::memcpy(&w, key.data() + i, 8);
            h = (h ^ w) * 1099511628211ull;
            h ^= h >> 29;
        }
        return static_cast<std::size_t>(h);
    }
};

using VertexIndex = std::unordered_map<PackedVertexKey, uint32_t, PackedVertexKeyHash>;

// At most one unique packed vertex per emitted corner. Reserve before the
// registration walk so a dense imported shape never rehashes on the main thread.
inline VertexIndex makeVertexIndex(std::size_t triangles, bool twoSided) {
    VertexIndex result;
    result.reserve(triangles * (twoSided ? 6u : 3u));
    return result;
}
}
