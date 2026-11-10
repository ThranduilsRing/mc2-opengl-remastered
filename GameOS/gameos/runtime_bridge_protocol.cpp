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

std::string_view commandName(CommandKind kind) {
    switch (kind) {
    case CommandKind::Pause: return "pause";
    case CommandKind::Resume: return "resume";
    case CommandKind::Step: return "step";
    case CommandKind::SetSpeed: return "set_speed";
    }
    return "?";
}

CommandParse parseCommand(std::string_view text, std::string_view session) {
    const auto message = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (!message.is_object() || !message.contains("version") || message["version"] != 1 ||
        !message.contains("session") || message["session"] != std::string(session) ||
        !message.contains("type") || message["type"] != "command" ||
        !message.contains("cmd") || !message["cmd"].is_string()) return {std::nullopt, "bad_message"};
    const auto name = message["cmd"].get<std::string>();
    Command command;
    if (name == "pause") command.kind = CommandKind::Pause;
    else if (name == "resume") command.kind = CommandKind::Resume;
    else if (name == "step") {
        command.kind = CommandKind::Step;
        if (!message.contains("n") || !message["n"].is_number_integer()) return {std::nullopt, "bad_argument"};
        const auto steps = message["n"].get<long long>();
        if (steps < 1 || steps > maxStepsPerCommand) return {std::nullopt, "bad_argument"};
        command.steps = static_cast<int>(steps);
    } else if (name == "set_speed") {
        command.kind = CommandKind::SetSpeed;
        if (!message.contains("speed") || !message["speed"].is_number()) return {std::nullopt, "bad_argument"};
        const double speed = message["speed"].get<double>();
        if (!std::isfinite(speed) || speed < minSpeed || speed > maxSpeed) return {std::nullopt, "bad_argument"};
        command.speed = static_cast<float>(speed);
    } else return {std::nullopt, "unknown_command"};
    return {command, {}};
}

bool Control::apply(const Command& command) {
    switch (command.kind) {
    case CommandKind::Pause: paused_ = true; return true;
    case CommandKind::Resume: paused_ = false; steps_ = 0; return true;
    case CommandKind::Step:
        if (!paused_) return false;
        steps_ += static_cast<uint32_t>(command.steps);
        return true;
    case CommandKind::SetSpeed: speed_ = command.speed; return true;
    }
    return false;
}
bool Control::paused() const { return paused_.load(); }
int Control::takeSteps(int most) {
    uint32_t have = steps_.load();
    while (have > 0) {
        const uint32_t take = have < static_cast<uint32_t>(most) ? have : static_cast<uint32_t>(most);
        if (steps_.compare_exchange_weak(have, have - take)) return static_cast<int>(take);
    }
    return 0;
}
bool Control::takeSpeed(float& speed) {
    const float queued = speed_.exchange(0.0f);
    if (queued <= 0.0f) return false;
    speed = queued;
    lastSpeed_ = queued;
    return true;
}
void Control::noteTicks(int count) { if (count > 0) ticks_ += static_cast<uint64_t>(count); }
uint64_t Control::ticks() const { return ticks_.load(); }
float Control::lastSpeed() const { return lastSpeed_.load(); }
uint32_t Control::pendingSteps() const { return steps_.load(); }
Control& control() { static Control value; return value; }
}
