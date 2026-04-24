// GameOS/gameos/asset_scale.cpp
#include "asset_scale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace AssetScale {

namespace {

struct NominalDims { uint32_t w; uint32_t h; };

struct State {
    std::unordered_map<std::string, NominalDims> manifest;
    std::unordered_set<std::string>              seenUnknown;     // path
    std::unordered_set<std::string>              seenOobFirst;    // "path|tag"
    bool   traceEnabled    = false;
    bool   inited          = false;

    // Counters (always accumulate; surfaced via dumpCountersTo).
    uint32_t cManifestEntries  = 0;
    uint32_t cManifestDupes    = 0;
    uint32_t cManifestBadLines = 0;
    uint32_t cFactorLookups    = 0;
    uint32_t cUnknownAsset     = 0;
    uint32_t cOobBlit          = 0;
    uint32_t cClampedRects     = 0;
};

State& state() { static State s; return s; }

void log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

std::string canonicalize(const char* raw) {
    if (!raw) return std::string();
    std::string s(raw);
    // backslash -> forward
    std::replace(s.begin(), s.end(), '\\', '/');
    // lowercase
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    // strip leading "./"
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    // strip leading "data/"
    if (s.rfind("data/", 0) == 0) s.erase(0, 5);
    // collapse "//"
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && s[i] == '/' && s[i-1] == '/') continue;
        out.push_back(s[i]);
    }
    return out;
}

// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

// Parse one CSV line "path,w,h". Returns true on success.
bool parseLine(const std::string& raw, std::string& path,
               uint32_t& w, uint32_t& h) {
    // Skip comments and blank.
    std::string s = trim(raw);
    if (s.empty() || s[0] == '#') return false;

    auto c1 = s.find(',');
    if (c1 == std::string::npos) return false;
    auto c2 = s.find(',', c1 + 1);
    if (c2 == std::string::npos) return false;

    std::string p = trim(s.substr(0, c1));
    std::string ws = trim(s.substr(c1 + 1, c2 - c1 - 1));
    std::string hs = trim(s.substr(c2 + 1));

    if (p.empty() || ws.empty() || hs.empty()) return false;

    char* end = nullptr;
    long wl = std::strtol(ws.c_str(), &end, 10);
    if (end == ws.c_str() || wl <= 0) return false;
    long hl = std::strtol(hs.c_str(), &end, 10);
    if (end == hs.c_str() || hl <= 0) return false;

    path = canonicalize(p.c_str());
    w = (uint32_t)wl;
    h = (uint32_t)hl;
    return true;
}

void loadManifest(const char* manifestPath) {
    State& st = state();
    FILE* f = std::fopen(manifestPath, "rb");
    if (!f) {
        log("[ASSET_SCALE v1] event=manifest_missing path=%s", manifestPath);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string buf;
    buf.resize(sz > 0 ? (size_t)sz : 0);
    if (sz > 0) std::fread(&buf[0], 1, (size_t)sz, f);
    std::fclose(f);

    int lineno = 0;
    size_t i = 0;
    while (i <= buf.size()) {
        size_t nl = buf.find('\n', i);
        if (nl == std::string::npos) nl = buf.size();
        std::string line = buf.substr(i, nl - i);
        // Strip CR for CRLF files.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lineno;
        std::string path; uint32_t w = 0, h = 0;
        std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            if (parseLine(line, path, w, h)) {
                auto ins = st.manifest.emplace(std::move(path), NominalDims{w, h});
                if (!ins.second) ++st.cManifestDupes;
                else             ++st.cManifestEntries;
            } else {
                ++st.cManifestBadLines;
                log("[ASSET_SCALE v1] event=manifest_bad_line lineno=%d raw=%s",
                    lineno, line.c_str());
            }
        }
        i = nl + 1;
    }
    log("[ASSET_SCALE v1] event=manifest_loaded entries=%u dupes=%u bad_lines=%u",
        st.cManifestEntries, st.cManifestDupes, st.cManifestBadLines);
}

} // anonymous namespace

AssetKey key(const char* rawPath) {
    return AssetKey(canonicalize(rawPath));
}

void init(const char* manifestPath) {
    State& st = state();
    if (st.inited) return;
    st.inited = true;
    const char* trace = std::getenv("MC2_ASSET_SCALE_TRACE");
    st.traceEnabled = (trace && trace[0] == '1');
    loadManifest(manifestPath);
}

void shutdown() {
    State& st = state();
    st = State();
}

void dumpCountersTo(FILE* out) {
    const State& st = state();
    std::fprintf(out,
        "[ASSET_SCALE v1] counters entries=%u dupes=%u bad_lines=%u "
        "lookups=%u unknown=%u oob=%u clamped=%u trace=%d\n",
        st.cManifestEntries, st.cManifestDupes, st.cManifestBadLines,
        st.cFactorLookups, st.cUnknownAsset, st.cOobBlit, st.cClampedRects,
        st.traceEnabled ? 1 : 0);
}

// Transform APIs filled in Task 3.
Vec2 factorFor(const AssetKey&, uint32_t, uint32_t, const char*)
    { return {1.f, 1.f}; }
int  nominalToActualAxis(const AssetKey&, int n, Axis, uint32_t, uint32_t, const char*)
    { return n; }
IVec2 nominalToActual(const AssetKey&, int nx, int ny, uint32_t, uint32_t, const char*)
    { return {nx, ny}; }
IRect nominalToActualRect(const AssetKey&, uint32_t, uint32_t,
                          float nx, float ny, float nw, float nh, const char*)
    { return { (int)std::floor(nx), (int)std::floor(ny),
               (int)std::ceil(nw),  (int)std::ceil(nh) }; }

} // namespace AssetScale
