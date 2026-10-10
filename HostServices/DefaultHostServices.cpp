// HostServices/DefaultHostServices.cpp
// ENGINE-HOST-SERVICES-0 — std-library-only default host services.
// No GameOS, no GL, no Mission: this TU links into the GL-free unit-test
// target unchanged.
#include "DefaultHostServices.h"

#include "Gates/gate_cold.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace mc2host {

// --- DiskFileSystem --------------------------------------------------------

bool DiskFileSystem::exists(const char* path) const {
    if (path == nullptr) {
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

bool DiskFileSystem::read(const char* path, std::vector<uint8_t>& out) const {
    out.clear();
    if (path == nullptr) {
        return false;
    }
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) {
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size == 0) {
        return true;  // empty file is a valid read
    }
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), size);
    if (!f) {
        out.clear();
        return false;
    }
    return true;
}

// --- StderrLogger ----------------------------------------------------------

namespace {
const char* levelTag(ILogger::Level level) {
    switch (level) {
        case ILogger::Level::Trace: return "TRACE";
        case ILogger::Level::Info:  return "INFO";
        case ILogger::Level::Warn:  return "WARN";
        case ILogger::Level::Error: return "ERROR";
    }
    return "?";
}
} // namespace

void StderrLogger::log(Level level, const char* category, const char* msg) {
    std::fprintf(stderr, "[%s] %s: %s\n",
                 levelTag(level),
                 category ? category : "",
                 msg ? msg : "");
}

// --- ChronoClock -----------------------------------------------------------

ChronoClock::ChronoClock()
    : startNanos_(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count()) {}

double ChronoClock::elapsedSeconds() const {
    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return static_cast<double>(now - startNanos_) * 1e-9;
}

// --- EnvConfig -------------------------------------------------------------

const char* EnvConfig::str(const char* key, const char* dflt) const {
    if (key == nullptr) {
        return dflt;
    }
    const char* v = mc2::gate::envText(key);   // caller-supplied runtime key (the EnvConfig interface)
    return v ? v : dflt;
}

bool EnvConfig::flag(const char* key, bool dflt) const {
    // The one boolean parser of the tree: unset/empty -> dflt, 0/false/off/no -> false,
    // anything else present -> true.
    return mc2::gate::parseBool((key != nullptr) ? mc2::gate::envText(key) : nullptr, dflt);
}

// --- Bundle ----------------------------------------------------------------

HostServices defaultHostServices() {
    static DiskFileSystem s_files;
    static StderrLogger   s_logger;
    static ChronoClock    s_clock;
    static EnvConfig       s_config;

    HostServices hs;
    hs.files  = &s_files;
    hs.log    = &s_logger;
    hs.clock  = &s_clock;
    hs.config = &s_config;
    return hs;
}

} // namespace mc2host
