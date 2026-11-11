#pragma once
#include <array>
#include <atomic>
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

// Control slice 1: a closed enum of sim commands. No ABL eval, no arbitrary code; anything else is an error reply.
enum class CommandKind { Pause, Resume, Step, SetSpeed };
struct Command { CommandKind kind = CommandKind::Pause; int steps = 0; float speed = 1.0f; };
constexpr int maxStepsPerCommand = 100000;
constexpr double minSpeed = 0.25, maxSpeed = 4.0;   // the sim-speed multiplier's own clamp
struct CommandParse {
    std::optional<Command> command;
    std::string_view error;   // "bad_message", "unknown_command" or "bad_argument" when command is empty
};
// Parses {"version":1,"session":S,"type":"command","cmd":"pause|resume|step|set_speed","n":N,"speed":F}.
CommandParse parseCommand(std::string_view text, std::string_view session);
std::string_view commandName(CommandKind kind);

// Shared between the socket worker (writes) and the main thread (reads). Lock-free.
class Control {
public:
    // Worker thread. False when the command cannot apply right now ("step" while running).
    bool apply(const Command& command);
    // Main thread, once per Mission::update.
    bool paused() const;
    int takeSteps(int most);                  // consumes up to `most` queued steps
    bool takeSpeed(float& speed);             // true once per set_speed
    void noteTicks(int count);                // sim ticks actually run (serial or overlap worker)
    uint64_t ticks() const;
    float lastSpeed() const;
    uint32_t pendingSteps() const;
private:
    std::atomic<bool> paused_{false};
    std::atomic<uint32_t> steps_{0};
    std::atomic<float> speed_{0.0f};          // 0 = no change queued
    std::atomic<float> lastSpeed_{0.0f};
    std::atomic<uint64_t> ticks_{0};
};
Control& control();
} // namespace mc2::bridge
