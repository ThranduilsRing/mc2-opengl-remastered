// code/jobs_fatal.cpp
// MC2-JOBS-CORE-1 (§2.0) — implementation of the always-compiled fatal check.
//
// Self-contained (C++17 stdlib only) so it links into both the engine and the
// standalone job-system unit harness without dragging in the gameos
// ErrorHandler / diagnostic_trace init machinery. It writes a single JSONL
// record directly to the SAME file the smoke gate consumes
// (MC2_DIAGNOSTIC_TRACE_FILE, default debug_state/diagnostic_trace.jsonl), so a
// tripped guard is visible in artifacts even on a headless run. Appending a
// well-formed {"tag":"JOBS_FATAL",...} line is line-compatible with the JSONL
// reader (mc2_diag emits one JSON object per line).
#include "jobs_fatal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

namespace {

const char* tracePath() {
    static const std::string p = mc2::gate::str<"MC2_DIAGNOSTIC_TRACE_FILE">();   // "" when unset or empty
    if (!p.empty() && p != "off") return p.c_str();
    return "debug_state/diagnostic_trace.jsonl";
}

// Append one JSONL record. Best-effort: on any failure we still proceed (the
// caller of fatal() aborts regardless; logOnce() simply drops the line).
void appendTrace(const char* tag, const char* condOrTag,
                 const char* file, int line) {
    const char* path = tracePath();
    if (std::strcmp(path, "off") == 0) return;

    std::ostringstream tid;
    tid << std::this_thread::get_id();

    // Minimal JSON escaping for the fields we control (cond/tag/file are code
    // literals, but escape defensively so a backslash in a path can't break it).
    auto esc = [](const char* s) {
        std::string out;
        for (const char* c = s; c && *c; ++c) {
            if (*c == '"' || *c == '\\') out += '\\';
            out += *c;
        }
        return out;
    };

    std::ostringstream line_;
    line_ << "{\"tag\":\"" << tag << "\",\"v\":1"
          << ",\"cond\":\"" << esc(condOrTag) << "\""
          << ",\"thread\":\"" << tid.str() << "\""
          << ",\"file\":\"" << esc(file) << "\""
          << ",\"line\":" << line << "}\n";
    const std::string s = line_.str();

    // "ab": append, binary (no CRLF translation — matches mc2_diag).
    if (std::FILE* f = std::fopen(path, "ab")) {
        std::fwrite(s.data(), 1, s.size(), f);
        std::fflush(f);
        std::fclose(f);
    }
    // Also mirror to stderr so it is visible without the trace file too.
    std::fprintf(stderr, "[JOBS] %s %s (%s:%d)\n", tag, condOrTag, file, line);
}

} // namespace

namespace mc2 { namespace jobs { namespace detail {

void fatal(const char* cond, const char* file, int line) {
    appendTrace("JOBS_FATAL", cond, file, line);
    std::abort();
}

void logOnce(const char* tag, const char* file, int line) {
    // One record per distinct call site (file:line), first hit only. A tiny
    // guarded set keeps this alloc-light and thread-safe; the reentrancy guard
    // that calls this is off the memory-corruption path so a mutex here is fine.
    static std::mutex mu;
    static std::string seen;  // newline-delimited "file:line" keys
    std::ostringstream key;
    key << file << ':' << line;
    const std::string k = key.str();
    {
        std::lock_guard<std::mutex> lk(mu);
        if (seen.find("\n" + k + "\n") != std::string::npos) return;
        seen += "\n" + k + "\n";
    }
    appendTrace("JOBS_LOGONCE", tag, file, line);
}

}}} // namespace mc2::jobs::detail
