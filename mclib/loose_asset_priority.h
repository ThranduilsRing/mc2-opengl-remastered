#pragma once
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>

// The texture cooker writes KTX2 beside its TGA/TXM input, without a receipt.
// Source textures are classified separately so edited loose bitmaps still win.
template <class SourceExists>
inline bool looseTextureCookHasSource(std::filesystem::path path, SourceExists sourceExists)
{
    if (path.extension() != ".ktx2") return false;
    for (const char* ext : {".tga", ".txm"}) {
        path.replace_extension(ext);
        if (sourceExists(path)) return true;
    }
    return false;
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
