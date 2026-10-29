#include "runtime_bridge_protocol.h"
#include "../../3rdparty/include/nlohmann/json.hpp"

namespace mc2::bridge {
std::optional<Subscription> subscribe(std::string_view text, std::string_view session) {
    const auto message = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
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
}
