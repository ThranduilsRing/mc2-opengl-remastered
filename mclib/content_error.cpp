#include "content_error.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

namespace mc2::content {
namespace {
constexpr std::array names{"texture", "shape", "objtype", "ammo", "movetype", "sound", "missiondata"};
std::mutex mutex;
std::array<std::size_t, names.size()> counts{};
std::set<std::tuple<Kind, std::string, std::string>> seen;

std::string singleLine(std::string_view text) {
    std::string result(text);
    for (char& c : result) if (c == '\n' || c == '\r') c = ' ';
    return result;
}
}

void error(Kind kind, std::string_view path, std::string_view message) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= names.size()) return;
    std::lock_guard lock(mutex);
    auto key = std::make_tuple(kind, singleLine(path), singleLine(message));
    const auto [entry, inserted] = seen.insert(key);
    if (!inserted) return;
    ++counts[index];
    std::fprintf(stderr, "[CONTENT] kind=%s path=%s msg=%s\n", names[index],
                 std::get<1>(key).c_str(), std::get<2>(key).c_str());
}

std::size_t count(Kind kind) {
    std::lock_guard lock(mutex);
    const auto index = static_cast<std::size_t>(kind);
    return index < counts.size() ? counts[index] : 0;
}

void resetMission() {
    std::lock_guard lock(mutex);
    counts.fill(0);
    seen.clear();
}

void reportMission(std::string_view mission) {
    std::lock_guard lock(mutex);
    std::size_t total = 0;
    for (auto value : counts) total += value;
    std::fprintf(stderr, "[CONTENT_SUMMARY] mission=%s total=%zu", singleLine(mission).c_str(), total);
    for (std::size_t i = 0; i < names.size(); ++i) std::fprintf(stderr, " %s=%zu", names[i], counts[i]);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}
} // namespace mc2::content
