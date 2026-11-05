#include "gos_crashpad.h"
#include <sentry.h>
#include <filesystem>
#include <string>
#include <cstdio>
#include <cstring>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace {
bool g_active = false;
bool g_tried = false;
std::string g_db;  // <exe_dir>/crashes/crashpad

#ifndef _WIN32
char g_crash_line[PATH_MAX + 96];
size_t g_crash_line_len = 0;

// Runs in the faulting process's signal handler: only write(2) of a prebuilt line. The event is returned
// unchanged so crashpad goes on to write the dump.
sentry_value_t on_crash(const sentry_ucontext_t*, sentry_value_t event, sentry_hint_t*, void*)
{
    const ssize_t r = write(2, g_crash_line, g_crash_line_len);
    (void)r;
    return event;
}
#endif

std::filesystem::path exe_dir()
{
#ifdef _WIN32
    if (!_wpgmptr) return {};
    return std::filesystem::path(_wpgmptr).parent_path();
#else
    char p[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) return {};
    p[n] = 0;
    return std::filesystem::path(p).parent_path();
#endif
}
}  // namespace

bool crashpad_start()
{
    if (g_tried) return g_active;
    g_tried = true;
    try {
        const std::filesystem::path dir = exe_dir();
        if (dir.empty()) return false;
        const std::filesystem::path handler = dir / (
#ifdef _WIN32
            "crashpad_handler.exe"
#else
            "crashpad_handler"
#endif
        );
        std::error_code ec;
        if (!std::filesystem::exists(handler, ec)) return false;  // no handler shipped: the legacy writer stays in charge
        const std::filesystem::path db = dir / "crashes" / "crashpad";
        std::filesystem::create_directories(db, ec);
        g_db = db.string();

        sentry_options_t* o = sentry_options_new();
        if (!o) return false;
        // No DSN and the null transport: nothing is ever sent. Crashpad is also given an empty upload URL.
        sentry_options_set_auto_session_tracking(o, 0);
        sentry_options_set_system_crash_reporter_enabled(o, 1);  // keep WER/OS reporting as before
#ifdef _WIN32
        sentry_options_set_handler_pathw(o, handler.c_str());
        sentry_options_set_database_pathw(o, db.c_str());
#else
        sentry_options_set_handler_path(o, handler.c_str());
        sentry_options_set_database_path(o, db.c_str());
        g_crash_line_len = static_cast<size_t>(std::snprintf(g_crash_line, sizeof g_crash_line,
            "\n[CRASH] crashpad minidump pending in %s/pending\n", g_db.c_str()));
        if (g_crash_line_len >= sizeof g_crash_line) g_crash_line_len = sizeof g_crash_line - 1;
        sentry_options_set_on_crash(o, on_crash, nullptr);
#endif
        g_active = sentry_init(o) == 0;
    } catch (...) {
        g_active = false;
    }
    if (g_active)
        std::fprintf(stderr, "[CRASH] crashpad handler started, database=%s\n", g_db.c_str());
    else
        std::fprintf(stderr, "[CRASH] crashpad handler NOT started, legacy dump writer stays active\n");
    return g_active;
}

bool crashpad_active() { return g_active; }

const char* crashpad_db_dir() { return g_db.c_str(); }

bool crashpad_newest_dump(char* out, size_t cap)
{
    if (!g_active || !out || cap == 0) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::file_time_type best{};
    std::string bestPath;
    for (const char* sub : { "pending", "completed" }) {
        for (fs::directory_iterator it(fs::path(g_db) / sub, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".dmp") continue;
            const auto t = it->last_write_time(ec);
            if (ec) { ec.clear(); continue; }
            if (bestPath.empty() || t > best) { best = t; bestPath = it->path().string(); }
        }
        ec.clear();
    }
    if (bestPath.empty() || bestPath.size() >= cap) return false;
    if (fs::file_time_type::clock::now() - best > std::chrono::seconds(60)) return false;  // a dump from an earlier run
    std::memcpy(out, bestPath.c_str(), bestPath.size() + 1);
    return true;
}
