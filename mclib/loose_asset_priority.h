#pragma once
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <cstdint>
#include <algorithm>
#include <cctype>

inline std::string assetLower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

inline bool looseTexture(const std::filesystem::path& path)
{
    const auto ext = assetLower(path.extension().string());
    return ext == ".ktx2" || ext == ".tga";
}

// Kit beats a loose texture only when it matches stock bytes receipted by deploy
// from data/kit/stock-textures.json. Legacy general-cooker stamps are invalid.
inline bool verifiedLooseTextureCook(const std::filesystem::path& path)
{
    // Deploy also receipts byte-verified stock TGA sources (some are loose-only).
    if (!looseTexture(path)) return false;
    std::error_code error;
    const bool hasStamp = std::filesystem::exists(path.string() + ".cookstamp", error);
    if (error) return false;
    if (!hasStamp) return false;
    std::ifstream stamp(path.string() + ".cookstamp"); // path-audit: loose-only cook provenance
    uint64_t expected = 0;
    std::string version;
    if (!(stamp >> version >> std::hex >> expected) || version != "mc2-stock-v1") return false;
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

template<class StockReader>
inline bool userLooseBuildingAsset(std::string_view name, std::string_view owner,
                                   StockReader&& readStock)
{
    if (!owner.empty() && assetLower(owner) != "mc2-industrial-kit") return true;
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::path(name), error)) return false;
    if (looseTexture(std::filesystem::path(name)))
        return !verifiedLooseTextureCook(std::filesystem::path(name));
    return looseAssetDiffers(name, readStock());
}
