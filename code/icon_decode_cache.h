#pragma once
#include "icon_registry.h"
#include "../mclib/fast_hash.h"
#include "../GameOS/gameos/cook_cache.h"
#include <cstring>

namespace unit_icons {
// Version covers the lossless HUD decode and BGRA conversion recipe. Sources are
// resolved through the VFS, so mod replacements and packed art share this cache.
inline uint64_t iconDecodeKey(uint64_t key, std::string_view path, uint64_t digest, int rank) {
    std::vector<uint8_t> fields;
    mc2cache::putLE(fields, path.size(), 8);
    fields.insert(fields.end(), path.begin(), path.end());
    mc2cache::putLE(fields, digest, 8);
    mc2cache::putLE(fields, static_cast<uint32_t>(rank), 4);
    return mc2::content::fastHash(fields, key);
}
inline constexpr uint64_t iconDecodeVersion = 0x49434f4e00000001ull;
inline constexpr mc2cache::CacheKind iconDecodeKind{"icons-", ".uid", 4, 256 * 1024 * 1024};

inline std::vector<uint8_t> encodeIcons(const Registry& registry, const std::map<std::string, int>& ranks, uint64_t key) {
    std::vector<uint8_t> bytes;
    size_t size = 28;
    for (const auto& [name, rank] : ranks) size += 16 + name.size() + registry.icons.at(name).pixels.size() * 4;
    if (size > iconDecodeKind.maxBytes) return {};
    bytes.reserve(size);
    mc2cache::putLE(bytes, iconDecodeVersion, 8);
    mc2cache::putLE(bytes, key, 8);
    mc2cache::putLE(bytes, ranks.size(), 4);
    for (const auto& [name, rank] : ranks) {
        const auto& icon = registry.icons.at(name);
        mc2cache::putLE(bytes, name.size(), 4);
        bytes.insert(bytes.end(), name.begin(), name.end());
        mc2cache::putLE(bytes, static_cast<uint32_t>(rank), 4);
        mc2cache::putLE(bytes, icon.width, 4);
        mc2cache::putLE(bytes, icon.height, 4);
        const auto pixels = std::as_bytes(std::span(icon.pixels));
        const auto* first = reinterpret_cast<const uint8_t*>(pixels.data());
        bytes.insert(bytes.end(), first, first + pixels.size());
    }
    mc2cache::putLE(bytes, mc2::content::fastHash(bytes), 8);
    return bytes;
}

// Validate the complete entry before publishing any icons. A torn or corrupt
// cache leaves the live registry untouched and takes the original decode path.
inline bool decodeIcons(std::span<const uint8_t> bytes, uint64_t key, Registry& registry, std::map<std::string, int>& ranks) {
    if (bytes.size() < 28 || bytes.size() > iconDecodeKind.maxBytes ||
        mc2cache::getLE(bytes, 0, 8) != iconDecodeVersion || mc2cache::getLE(bytes, 8, 8) != key ||
        mc2cache::getLE(bytes, bytes.size() - 8, 8) != mc2::content::fastHash(bytes.first(bytes.size() - 8))) return false;
    const auto count = mc2cache::getLE(bytes, 16, 4);
    if (count > 65536) return false;
    bytes = bytes.subspan(20, bytes.size() - 28);
    Registry candidate;
    std::map<std::string, int> candidateRanks;
    for (uint64_t i = 0; i < count; ++i) {
        if (bytes.size() < 4) return false;
        const auto length = mc2cache::getLE(bytes, 0, 4);
        bytes = bytes.subspan(4);
        if (!length || length > 4096 || bytes.size() < length + 12) return false;
        std::string name(reinterpret_cast<const char*>(bytes.data()), static_cast<size_t>(length));
        bytes = bytes.subspan(static_cast<size_t>(length));
        const auto rank = mc2cache::getLE(bytes, 0, 4);
        const auto w = mc2cache::getLE(bytes, 4, 4), h = mc2cache::getLE(bytes, 8, 4);
        bytes = bytes.subspan(12);
        if (!w || !h || w > 16382 || h > 16382 || w * h > bytes.size() / 4 ||
            rank > 0x7fffffff || canonical(name) != name || candidate.icons.contains(name) ||
            (!name.starts_with("hud/") && !name.starts_with("pilot/"))) return false;
        std::vector<uint32_t> pixels(static_cast<size_t>(w * h));
        std::memcpy(pixels.data(), bytes.data(), pixels.size() * 4);
        bytes = bytes.subspan(pixels.size() * 4);
        candidate.add(name, static_cast<int>(w), static_cast<int>(h), pixels);
        candidateRanks.emplace(std::move(name), static_cast<int>(rank));
    }
    if (!bytes.empty()) return false;
    for (auto& [name, icon] : candidate.icons) registry.icons.insert_or_assign(name, std::move(icon));
    ranks = std::move(candidateRanks);
    return true;
}
} // namespace unit_icons
