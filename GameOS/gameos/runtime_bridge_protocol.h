#pragma once
#include "../../3rdparty/include/nlohmann/json.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <optional>

namespace mc2::bridge {
using Json = nlohmann::json;
constexpr uint32_t maxFrameBytes = 1024 * 1024;
inline std::array<unsigned char, 4> frameHeader(uint32_t size) {
    return {static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
            static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)};
}
inline uint32_t frameSize(const unsigned char* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
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
inline std::optional<Subscription> subscribe(const Json& message, std::string_view session) {
    if (!message.is_object() || !message.contains("version") || message["version"] != 1 ||
        !message.contains("session") || message["session"] != std::string(session) ||
        !message.contains("type") || message["type"] != "subscribe" ||
        !message.contains("hz") || !message["hz"].is_number() ||
        !message.contains("streams") || !message["streams"].is_array()) return std::nullopt;
    Subscription next;
    next.hz = message["hz"].get<double>();
    if (!std::isfinite(next.hz) || next.hz < 0.25 || next.hz > 30) return std::nullopt;
    for (const auto& stream : message["streams"]) {
        if (stream == "runtime.observation") next.state = true;
        else if (stream == "mover.transform") next.movers = true;
        else return std::nullopt;
    }
    return next;
}
} // namespace mc2::bridge
