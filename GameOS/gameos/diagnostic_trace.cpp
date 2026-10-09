
#include "diagnostic_trace.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // GetCurrentThreadId, MoveFileExW

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <sstream>
#include <unordered_set>

namespace {

// ---- Known registered tags ----
// Sessions querying get_diagnostic_events() with an unknown tag receive an error.
// Add new tags here as diagnostic sites are migrated from stderr.
const std::unordered_set<std::string>& knownTags() {
    static const std::unordered_set<std::string> tags = {
        "GPU_CULL",
        "LIGHTBAKE_PROOF",
        "ANIM_GATE",
        "SPFLUSH_COST_SPLIT",
        "TerrainLOD_prod",
        "TERRAIN_ACTIVE_AB",
        "TERRAIN_SOLID_AB",
        "CONFIG",
        "ENV",
        "BUILD",
        "DEVICE",
        "SHADER_COMPILE",
        "LOGISTICS",
        "FRAME_JOBS",
        "CURSOR_TARGET",
        "OVERLAY_MAGENTA",
        "ANIM_CADENCE",
        "TARGETING",
        "COMBAT",   // COMBAT-TRACE-1: weapon-fire events (attacker/target wid+team). Opt-in.
        "RNG",      // DETERMINISTIC-RNG-1: per gos_rand() result (compile-gated by MC2_RNG_TRACE). Opt-in.
    };
    return tags;
}

// Default tag whitelist (used when MC2_DIAG_TAGS is unset)
const std::unordered_set<std::string>& defaultWhitelist() {
    static const std::unordered_set<std::string> tags = {
        "GPU_CULL",
        "LIGHTBAKE_PROOF",
        "ANIM_GATE",
        "SPFLUSH_COST_SPLIT",
        "TerrainLOD_prod",
        "CONFIG",
        "BUILD",
        "DEVICE",
        "SHADER_COMPILE",
    };
    return tags;
}

// ---- State ----
struct DiagState {
    std::mutex       mtx;
    FILE*            file        = nullptr;
    bool             initialized = false;
    bool             disabled    = false;
    bool             allTags     = false;           // MC2_DIAG_TAGS=*
    std::unordered_set<std::string> enabledTags;
    std::string      sessionId;
    int              pid         = 0;
    // Start time for ts_ms (milliseconds since init)
    std::chrono::steady_clock::time_point startTime;
};

DiagState& state() {
    static DiagState s;
    return s;
}

// Parse MC2_DIAG_TAGS into the enabled set.
// Returns true if "all tags" mode (*), false otherwise.
// Sets disabled=true if "none".
bool parseTagsEnv(std::unordered_set<std::string>& outTags, bool& outDisabled) {
    const char* val = std::getenv("MC2_DIAG_TAGS");
    if (!val || !val[0]) {
        // Default: use high-value whitelist
        outTags     = defaultWhitelist();
        outDisabled = false;
        return false;  // not allTags
    }
    if (std::strcmp(val, "*") == 0) {
        outDisabled = false;
        return true;  // allTags
    }
    if (std::strcmp(val, "none") == 0) {
        outDisabled = true;
        return false;
    }

    // Parse comma-separated list
    outTags.clear();
    outDisabled = false;
    const std::string s(val);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        std::string tag = s.substr(pos, comma - pos);
        // Trim whitespace
        while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.erase(0, 1);
        while (!tag.empty() && (tag.back()  == ' ' || tag.back()  == '\t')) tag.pop_back();
        if (!tag.empty()) {
            // Warn if unknown tag (to stderr, since file may not be open yet)
            if (knownTags().find(tag) == knownTags().end()) {
                fprintf(stderr, "[MC2_DIAG] WARNING: unknown tag in MC2_DIAG_TAGS: %s\n", tag.c_str());
            }
            outTags.insert(tag);
        }
        pos = comma + 1;
    }
    return false;  // not allTags
}

// Get output path from env or default
std::filesystem::path outputPath() {
    const char* p = std::getenv("MC2_DIAGNOSTIC_TRACE_FILE");
    if (p && p[0]) return std::filesystem::path(p);
    return std::filesystem::path("debug_state/diagnostic_trace.jsonl");
}

constexpr uintmax_t kRotateThreshold = 10u * 1024u * 1024u;  // 10 MB

// Rotate file if oversized: rename to .prev.jsonl
void maybeRotate(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < kRotateThreshold) return;

    // Build prev path: replace .jsonl extension with .prev.jsonl
    std::filesystem::path prev = path;
    prev.replace_extension(".prev.jsonl");

    // Overwrite any existing .prev.jsonl
    std::filesystem::rename(path, prev, ec);
    // If rename fails, proceed and overwrite the main file (don't lose events)
}

} // namespace

namespace mc2_diag {

void init(const char* sessionId, int pid) {
    DiagState& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);

    if (s.initialized) return;
    s.initialized = true;
    s.startTime = std::chrono::steady_clock::now();
    s.sessionId = sessionId ? sessionId : "";
    s.pid = pid;

    // Check if disabled
    const char* fileEnv = std::getenv("MC2_DIAGNOSTIC_TRACE_FILE");
    if (fileEnv && std::strcmp(fileEnv, "off") == 0) {
        s.disabled = true;
        return;
    }

    // Parse tag list
    s.allTags = parseTagsEnv(s.enabledTags, s.disabled);
    if (s.disabled) return;

    // Determine output path and create directory
    const std::filesystem::path path = outputPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        fprintf(stderr, "[MC2_DIAG] WARNING: cannot create dir %s: %s\n",
                path.parent_path().string().c_str(), ec.message().c_str());
        s.disabled = true;
        return;
    }

    // Rotate if oversized
    if (std::filesystem::exists(path)) {
        maybeRotate(path);
    }

    // Open in BINARY append mode (line-buffered via manual fflush).
    // "ab": binary so fwrite writes raw bytes without \n→\r\n translation.
    //   No BOM is written (binary mode bypasses CRT text encoding).
    // Wide path (_wfopen) handles non-ASCII deploy directories.
    // NOT L"a,ccs=UTF-8": that opens a wide-char stream (writes UTF-8 BOM,
    //   expects fputws/fwprintf) and triggers MSVC _invalid_parameter abort
    //   on the subsequent narrow fwrite() call.
    s.file = _wfopen(path.wstring().c_str(), L"ab");
    if (!s.file) {
        // Try ASCII path as fallback
        s.file = fopen(path.string().c_str(), "ab");
    }
    if (!s.file) {
        fprintf(stderr, "[MC2_DIAG] WARNING: cannot open trace file %s\n",
                path.string().c_str());
        s.disabled = true;
        return;
    }

    // Emit a startup CONFIG event so sessions can confirm the trace path works
    // even before any diagnostic sites are migrated from stderr.
    {
        const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream line;
        line << "{\"tag\":\"CONFIG\",\"v\":1"
             << ",\"session_id\":\"" << s.sessionId << "\""
             << ",\"pid\":" << s.pid
             << ",\"tid\":0"
             << ",\"frame\":0"
             << ",\"ts_ms\":0"
             << ",\"written_epoch\":" << static_cast<double>(nowUs) / 1e6
             << ",\"data\":{\"event\":\"diagnostic_trace_initialized\""
             << ",\"tags_mode\":" << (s.allTags ? "\"all\"" : "\"whitelist\"")
             << "}}\n";
        const std::string lineStr = line.str();
        fwrite(lineStr.c_str(), 1, lineStr.size(), s.file);
        fflush(s.file);
    }
}

bool enabled() {
    const DiagState& s = state();
    return s.initialized && !s.disabled && s.file != nullptr;
}

bool tagEnabled(const char* tag) {
    if (!tag) return false;
    const DiagState& s = state();
    if (!enabled()) return false;
    if (s.allTags) return true;
    return s.enabledTags.count(tag) > 0;
}

bool writeEvent(const char* tag, int version, uint64_t frame, const char* dataJson) {
    if (!tag || !dataJson) return false;

    DiagState& s = state();

    // Fast path without lock
    if (!s.initialized || s.disabled || !s.file) return false;
    if (!s.allTags && s.enabledTags.count(tag) == 0) return false;

    // Compute timestamps
    const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - s.startTime).count();

    const DWORD tid = GetCurrentThreadId();

    // Format JSON line (no heap allocation in hot path -- use stack buffer)
    // Format: {"tag":"...","v":N,"session_id":"...","pid":N,"tid":N,"frame":N,"ts_ms":N,"written_epoch":N,"data":{...}}
    // Use ostringstream (heap, but only when tag is enabled)
    std::ostringstream line;
    line << "{\"tag\":\"" << tag << "\""
         << ",\"v\":" << version
         << ",\"session_id\":\"" << s.sessionId << "\""
         << ",\"pid\":" << s.pid
         << ",\"tid\":" << static_cast<unsigned long>(tid)
         << ",\"frame\":" << static_cast<unsigned long long>(frame)
         << ",\"ts_ms\":" << static_cast<long long>(elapsedMs)
         << ",\"written_epoch\":" << static_cast<double>(nowUs) / 1e6
         << ",\"data\":" << dataJson
         << "}\n";

    const std::string lineStr = line.str();

    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.file) return false;

    const size_t written = fwrite(lineStr.c_str(), 1, lineStr.size(), s.file);
    fflush(s.file);  // Line-buffered: flush after each event for crash-survival

    return written == lineStr.size();
}

void flush() {
    DiagState& s = state();
    if (s.file) fflush(s.file);
}

void shutdown() {
    DiagState& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.file) {
        fflush(s.file);
        fclose(s.file);
        s.file = nullptr;
    }
    s.disabled = true;  // prevent further writes after shutdown
}

} // namespace mc2_diag
