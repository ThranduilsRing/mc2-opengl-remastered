#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace mc2_prop {
// At most one unique packed vertex per emitted corner. Reserve before the
// registration walk so a dense imported shape never rehashes on the main thread.
inline std::unordered_map<std::string, uint32_t> makeVertexIndex(std::size_t triangles, bool twoSided) {
    std::unordered_map<std::string, uint32_t> result;
    result.reserve(triangles * (twoSided ? 6u : 3u));
    return result;
}
}
