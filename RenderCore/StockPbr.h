#pragma once
#include <string>
#include <string_view>
#include <cstdint>

namespace RenderCore {
inline bool stockPbrArchive(uintmax_t bytes, uint32_t crc) {
    // Shipped stock tgl.fst; standalone mod archives retain their own atlases.
    return bytes == 25925639u && crc == 0x2620c4b9u;
}
// Texture names, rather than appearance names, identify the fixed stock UV atlas.
inline std::string stockPbrStem(std::string_view texture) {
    const auto slash = texture.find_last_of("/\\");
    texture.remove_prefix(slash == std::string_view::npos ? 0 : slash + 1);
    texture = texture.substr(0, texture.find_last_of('.'));
    std::string stem(texture);
    for (char& c : stem) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (stem.size() >= 3 && stem.compare(stem.size() - 3, 3, "rgb") == 0 && stem != "vtolrgb")
        stem.resize(stem.size() - 3);
    return stem;
}
inline bool preferStockPbr(bool exists, bool legacyMod, int legacyPriority, int pbrPriority) {
    return exists && !(legacyMod && legacyPriority <= pbrPriority);
}
} // namespace RenderCore
