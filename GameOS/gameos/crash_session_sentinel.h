#pragma once
// Session sentinel: a file the game writes when its frame loop starts and deletes when the loop ends or a crash bundle is written.
// A sentinel left behind by a process that is gone means the session died with no handler running (external kill, a stack overflow
// that faulted inside the filter, a fast-fail): the next launch turns it into a crash record, so no exit is silent.
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mc2::crash {

struct SessionSentinel {
    uint32_t pid = 0;
    uint64_t created = 0;   // process creation time (FILETIME ticks): a reused pid has a different one
    std::string build;
};

inline std::string formatSentinel(const SessionSentinel& s) {
    return "pid=" + std::to_string(s.pid) + "\ncreated=" + std::to_string(s.created) + "\nbuild=" + s.build + "\n";
}

inline std::optional<SessionSentinel> parseSentinel(std::string_view text) {
    SessionSentinel s;
    bool havePid = false, haveCreated = false;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "pid") havePid = std::from_chars(value.data(), value.data() + value.size(), s.pid).ec == std::errc{};
        else if (key == "created") haveCreated = std::from_chars(value.data(), value.data() + value.size(), s.created).ec == std::errc{};
        else if (key == "build") s.build = std::string(value);
    }
    if (!havePid || !haveCreated || s.pid == 0) return std::nullopt;
    return s;
}

// The sentinel's session is over when its pid is gone or now names a different process.
inline bool sentinelOrphaned(const SessionSentinel& s, bool pidAlive, uint64_t liveCreated) {
    return !pidAlive || liveCreated != s.created;
}

// Retire an orphan only after its record is durable, or the harness marked this exact session before stopping it.
template <typename WriteRecord>
bool retireOrphanedSentinel(const SessionSentinel& s, const std::optional<SessionSentinel>& stopped, WriteRecord writeRecord) {
    if (stopped && s.created != 0 && stopped->pid == s.pid && stopped->created == s.created) return true;
    return writeRecord();
}

}  // namespace mc2::crash
