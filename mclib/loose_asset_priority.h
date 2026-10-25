#pragma once
#include <filesystem>
#include <fstream>
#include <span>

// Extracted stock files are common in legacy installs. Only different loose
// bytes (or a new loose asset) claim priority over bundled replacement art.
inline bool looseAssetDiffers(const char* path, std::span<const unsigned char> stock)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != stock.size() || stock.empty()) return true;
    std::ifstream input(path, std::ios::binary);
    for (unsigned char byte : stock)
        if (input.get() != byte) return true;
    return false;
}
