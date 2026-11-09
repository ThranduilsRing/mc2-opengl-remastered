#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mc2::content {
// Source hashing is synchronous at cache lookup. Batch filesystem reads while
// retaining an 8-byte-aligned chunk boundary for the word-at-a-time digest.
template<class FileT, class Hash>
std::optional<uint64_t> readSourceDigest(FileT& file, Hash hash) {
    auto left = file.fileSize();
    uint64_t digest = 14695981039346656037ull;
    std::vector<uint8_t> chunk(std::min<std::size_t>(left, 1024 * 1024));
    while (left) {
        const auto count = std::min<std::size_t>(left, chunk.size());
        if (file.read(chunk.data(), static_cast<long>(count)) != static_cast<long>(count)) return std::nullopt;
        digest = hash(std::span<const uint8_t>(chunk.data(), count), digest);
        left -= static_cast<decltype(left)>(count);
    }
    return digest;
}
}
