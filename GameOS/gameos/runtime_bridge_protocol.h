#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <optional>

namespace mc2::bridge {
constexpr uint32_t maxFrameBytes = 1024 * 1024;
inline std::array<unsigned char, 4> frameHeader(uint32_t size) {
    return {static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
            static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)};
}
inline uint32_t frameSize(const unsigned char* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}
inline bool validSession(std::string_view text) {
    if (text.size() != 36) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
        else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                   (text[i] >= 'A' && text[i] <= 'F'))) return false;
    }
    return true;
}
struct Subscription { double hz = 0; bool state = false, movers = false; };
std::optional<Subscription> subscribe(std::string_view text, std::string_view session);
} // namespace mc2::bridge
