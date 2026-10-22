// cook_cache.h -- the on-disk write-through cache every cooked render asset kind shares (terrain macro colour, shadow masks).
//
// WHAT: one file per (kind, key) under a cache directory, named <prefix><key as 16 hex digits><ext>. A write goes to a temp file and is renamed over
// the entry, so a crash leaves at most a .tmp (swept after an hour), never a partial entry; after each write only the newest maxFiles entries of that
// kind are kept. Reads bound the size before allocating. The bytes are opaque here: each kind validates its own header and payload hash.
// WHY: cooked assets are expensive to rebuild and cheap to load; one LRU per kind keeps a disk budget per kind without one kind evicting another.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mc2cache {

// Little-endian header fields of a cooked file (no native struct layout on disk).
inline void putLE(std::vector<uint8_t>& bytes, uint64_t word, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(word >> (8 * i)));
}
inline uint64_t getLE(std::span<const uint8_t> bytes, size_t offset, unsigned count) {
    uint64_t word = 0;
    for (unsigned i = 0; i < count; ++i) word |= uint64_t(bytes[offset + i]) << (8 * i);
    return word;
}

struct CacheKind {
    std::string_view prefix;   // e.g. "macro-", "shadow-"
    std::string_view ext;      // e.g. ".tsf", ".tsm"
    size_t maxFiles = 4;
    uint64_t maxBytes = 0;     // a larger file is never read
};

std::filesystem::path entryPath(std::string_view dir, const CacheKind& kind, uint64_t key);
// The entry's bytes, or nothing when it is missing, oversized or unreadable.
std::optional<std::vector<uint8_t>> readEntry(std::string_view dir, const CacheKind& kind, uint64_t key);
// Atomic write of the entry, then the kind's LRU sweep. False when the directory or the file cannot be written.
bool writeEntry(std::string_view dir, const CacheKind& kind, uint64_t key, std::span<const uint8_t> bytes);

}  // namespace mc2cache
