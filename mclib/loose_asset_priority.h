#pragma once
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <cstdint>

// Only an unchanged, receipted cook may yield to bundled art. A paired source
// (even archived stock) says nothing about whether the user edited the KTX2.
inline bool verifiedLooseTextureCook(const std::filesystem::path& path)
{
    if (path.extension() != ".ktx2") return false;
    std::ifstream stamp(path.string() + ".cookstamp"); // path-audit: loose-only cook provenance
    uint64_t expected = 0;
    if (!(stamp >> std::hex >> expected)) return false;
    std::ifstream input(path, std::ios::binary); // path-audit: loose-only cook verification
    if (!input) return false;
    uint64_t hash = 14695981039346656037ull;
    char buffer[65536];
    while (input.read(buffer, sizeof(buffer)) || input.gcount())
        for (std::streamsize i = 0; i < input.gcount(); ++i)
            hash = (hash ^ static_cast<unsigned char>(buffer[i])) * 1099511628211ull;
    return input.eof() && hash == expected;
}

// Extracted stock files are common in legacy installs. Only different loose
// bytes (or a new loose asset) claim priority over bundled replacement art.
inline bool looseAssetDiffers(std::string_view name, std::span<const unsigned char> stock)
{
    const std::filesystem::path path(name);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != stock.size() || stock.empty()) return true;
    std::ifstream input(path, std::ios::binary); // path-audit: loose-only (compare install overlay against packed stock)
    for (unsigned char byte : stock)
        if (input.get() != byte) return true;
    return false;
}
